/* driver.cc —— 接口与理由见 driver.hh。
 *
 * 每个常量的出处都写在旁边，因为它们全都来自 `host.py`：段几何、协议区初值、完成判据。
 * 「我们自己的环境」里只有三样东西是 host.py 没有的 —— 代码区加载地址（`image.hh` 的
 * `kCodeBase`）、栈（真线程栈在 Unicorn 里不存在）、magic 桩区（`stubs.hh`）。
 */
#include "driver.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "image.hh"
#include "spec.hh"
#include "stubs.hh"

namespace tc {
namespace {

// host.py 的三个数：段对齐/默认尺寸、数组 seq 头、命令块里的目标速度。
constexpr uint64_t kPage = 0x10000;                    // host.py PAGE
// `uc_mem_map_ptr` 的宿主内存对齐。文档只要求**地址与大小**是 4 KB 的倍数（段几何本来就是 64 KB
// 对齐，已满足），没规定宿主指针 —— 按页对齐是最不会出事的写法。
constexpr uint64_t kHostAlign = 4096;
constexpr uint64_t kRegion = 0x100000;                 // host.py REGION：「只有地址、不知多大」给 1 MB
constexpr uint64_t kArrLen = 4096;                     // host.py ARR_LEN
constexpr uint64_t kArrCapLog2 = 13;                   // host.py ARR_CAP_LOG2
constexpr uint64_t kSpeed = 10000000000000ull;         // host.py `_seed_memory`：ctl_cycle_speed_ms = 10^13

// 栈：host.py 用真线程栈（`_start_thread` 的 `and rsp, -16; jmp entry`），Unicorn 里寄存器初值
// 全 0 —— 不铺栈，第一条 `push` 就往地址 0 写。位置与游戏段（≈1.68 TB）、code（0x1000_0000）、
// arena（4 GB）、保留区（64 TiB）都不重叠。
constexpr uint64_t kStackBase = 0x40000000;
constexpr uint64_t kStackSize = 0x800000;  // 8 MB：host.py 的线程栈默认 1 MB，这里给足不省

// 「程序已经在等命令」的判据：`run_sim` 内层 `while get_command(ctl_command_id) == last_command_id`
// 每圈读一次这个 u64（`rv32im.si:10305`）。启动路径（`reset_sim`/`mode_refresh`/`on_ui_update`）
// 读它的次数远少于这个数，而内层循环是无限自旋 —— 读满即「进去了」。
// 早一点/晚一点停都不影响语义：命令是在停住之后才写的，程序读到它时一定已经走出等待循环。
constexpr uint64_t kWaitReads = 32;

constexpr uint64_t kCheckEvery = 1000;  // 每多少条指令查一次完成判据（设计 §4 K2）
// 连续几次采样都满足才算完成（host.py `_tick` 的 `stable >= 2`）。采样判据现在只是兜底：
// `sim_running` 写 0 是精确的完成点，采样会把它往后多跑几百条（切点深度靠不住）。
constexpr int kDoneSamples = 2;

// 防死循环的界：不是判据，只保证「判据永远不成立」时也停得下来并报出 pc。
// 启动期的大头是 `reset_sim` 那次 128 MB 内存镜像拷贝；它现在由 `try_rep_movsb` 换成原生
// `memmove`（Unicorn 按**字节**执行 `rep movsb`，一字节一次 code hook），所以这里的界只剩保险作用。
constexpr uint64_t kMaxBootInstrs = 1000000000ull;
constexpr uint64_t kMaxRunInstrs = 2000000000ull;

// EFLAGS 的 DF（bit 10）：`rep movsb` 的方向。0 = 递增（特判的前提），1 = 递减（必退）。
constexpr uint64_t kDirectionFlag = 0x400;

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* host.py 的 `XORSHIFT_SEED` / `pattern_bytes`：两边传同一个档位，就得得到同一串字节。 */
constexpr uint64_t kXorshiftSeed = 0x2545F4914F6CDD1Dull;

uint64_t xorshift_step(uint64_t x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

// 每块从同一图案重新起头（与 host.py 一致：图案是**块内偏移**的函数，加/减一个块不影响别的块）。
std::vector<uint8_t> pattern(Seed seed, uint64_t size) {
    std::vector<uint8_t> out(size, 0);
    switch (seed) {
        case Seed::kNone:
            break;
        case Seed::kSeq:
            for (uint64_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(i & 0xFF);
            break;
        case Seed::kXorshift: {
            uint64_t x = kXorshiftSeed;
            uint64_t i = 0;
            for (; i + 8 <= size; i += 8) {
                x = xorshift_step(x);
                for (int k = 0; k < 8; ++k) out[i + static_cast<uint64_t>(k)] = static_cast<uint8_t>(x >> (8 * k));
            }
            if (i < size) {  // 不是 8 的倍数：尾部用下一步的低字节补（host.py 同）
                x = xorshift_step(x);
                for (uint64_t k = 0; i + k < size; ++k)
                    out[i + k] = static_cast<uint8_t>(x >> (8 * k));
            }
            break;
        }
    }
    return out;
}

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

void check(uc_err err, const std::string& what) {
    if (err != UC_ERR_OK) throw std::runtime_error(what + ": " + uc_strerror(err));
}

uint64_t rd_u64(uc_engine* uc, uint64_t addr) {
    uint64_t v = 0;
    check(uc_mem_read(uc, addr, &v, sizeof(v)), "读 " + hex(addr));
    return v;
}

void wr_u64(uc_engine* uc, uint64_t addr, uint64_t v) {
    check(uc_mem_write(uc, addr, &v, sizeof(v)), "写 " + hex(addr));
}

uint64_t reg(uc_engine* uc, int id) {
    uint64_t v = 0;
    check(uc_reg_read(uc, id, &v), "读寄存器");
    return v;
}

void set_reg(uc_engine* uc, int id, uint64_t v) { check(uc_reg_write(uc, id, &v), "写寄存器"); }

// `uc_mem_map_ptr` 的宿主内存：按 `kHostAlign` 对齐、大小也向上对齐，而且必须是**零**——
// `uc_mem_map` 给的是零页，命令块/设置块的初值靠的就是这一点（见 `seed_protocol`），新分配的
// 堆内存是脏的。
std::shared_ptr<uint8_t> alloc_host_bytes(uint64_t size) {
    const size_t n = static_cast<size_t>(align_up(size, kHostAlign));
    auto* bytes = static_cast<uint8_t*>(::operator new(n, std::align_val_t(kHostAlign)));
    std::memset(bytes, 0, n);
    return std::shared_ptr<uint8_t>(bytes, [](uint8_t* p) {
        ::operator delete(p, std::align_val_t(kHostAlign));
    });
}

// [addr, addr+size) 必须整个落在**同一个**宿主区里：段之间不连续，跨段就没有一个裸指针能用。
HostRegion* find_host_region(std::vector<HostRegion>& regions, uint64_t addr, uint64_t size) {
    for (HostRegion& r : regions)
        if (r.lo <= addr && addr + size <= r.hi) return &r;
    return nullptr;
}

// `rep movsb` 的机器码是 F3（REP 前缀）+ A4（MOVSB）。判据只看**指令起点**的两个字节：
// code hook 只在指令边界上回调，所以「首字节是 F3、次字节是 A4」就等于这条指令是 `rep movsb`
// ——不需要反汇编器，也不会把 F3 当别的指令的前缀误判。
bool is_rep_movsb_encoding(const uint8_t* bytes, uint64_t size) {
    return size >= 2 && bytes[0] == 0xF3 && bytes[1] == 0xA4;
}

// `[(lo, hi)] → [(lo, hi)]`：逐行对应 host.py 的 `merge_spans`。
std::vector<std::pair<uint64_t, uint64_t>> merge_ranges(
    const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    spans.reserve(ranges.size());
    for (const auto& [lo, hi] : ranges) {
        const uint64_t lo2 = lo & ~(kPage - 1);
        // `hi <= lo` = 「只知道有这么个地址、不知道多大」→ 给 kRegion
        const uint64_t end = std::max(hi, lo + (hi <= lo ? kRegion : 0));
        const uint64_t hi2 = align_up(end, kPage);
        spans.emplace_back(lo2, std::max(hi2, lo2 + kPage));
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (const auto& [lo, hi] : spans) {
        if (!out.empty() && lo <= out.back().second)
            out.back().second = std::max(out.back().second, hi);
        else
            out.emplace_back(lo, hi);
    }
    return out;
}

// 协议区与游戏段地址：全部从 `Spec` 算，不写字面常量（设计 §4 K5）。
struct Protocol {
    uint64_t command = 0;       // commands + CommandIndex.ctl_command*8
    uint64_t command_id = 0;    // commands + ctl_command_id*8
    uint64_t target_cycle = 0;  // settings + StateIndex.sim_target_cycle*8
    uint64_t cycle = 0;         // settings + sim_cycle*8
    uint64_t last_command_id = 0;
    uint64_t running = 0;      // settings + sim_running*8
    uint64_t error_buffer = 0; // `error_buffer`：前 2 字节是小端 U16 的字节数（rv32im.si:174）
    uint64_t run_value = 0;    // SimCommand.run
    uint64_t settings = 0;         // settings 数组首（上面那些 = 它 + 下标*8）
    uint64_t running_index = 0;    // StateIndex 里 sim_running 的下标
};

size_t index_of(const std::vector<std::string>& members, const char* name, const char* type) {
    for (size_t i = 0; i < members.size(); ++i) {
        if (members[i] == name) return i;
    }
    throw std::runtime_error(std::string(type) + " 里没有 " + name);
}

uint64_t need_addr(const Spec& spec, const char* name) {
    const auto a = spec.addr(name);
    if (!a) throw std::runtime_error("spec 里没有地址 " + std::string(name));
    return *a;
}

Protocol resolve_protocol(const Spec& spec) {
    const auto enums = spec.names();
    const auto need = [&](const char* type) -> const std::vector<std::string>& {
        const auto it = enums.find(type);
        if (it == enums.end()) throw std::runtime_error(std::string("spec 里没有枚举 ") + type);
        return it->second;
    };
    const uint64_t cmd_block = need_addr(spec, "commands");
    const uint64_t state = need_addr(spec, "settings");
    const auto& ctl = need("CommandIndex");
    const auto& st = need("StateIndex");
    const auto& cmdv = need("SimCommand");

    Protocol p;
    p.command = cmd_block + index_of(ctl, "ctl_command", "CommandIndex") * 8;
    p.command_id = cmd_block + index_of(ctl, "ctl_command_id", "CommandIndex") * 8;
    p.target_cycle = state + index_of(st, "sim_target_cycle", "StateIndex") * 8;
    p.cycle = state + index_of(st, "sim_cycle", "StateIndex") * 8;
    p.last_command_id = state + index_of(st, "sim_last_command_id", "StateIndex") * 8;
    p.settings = state;
    p.running_index = index_of(st, "sim_running", "StateIndex");
    p.running = state + p.running_index * 8;
    p.error_buffer = need_addr(spec, "error_buffer");
    p.run_value = index_of(cmdv, "run", "SimCommand");
    return p;
}

// host.py `_seed_memory` 的三件事。前两件（命令块/设置块清零）靠新映射的零页天然成立；
// 这两件必须显式写，否则程序的行为与宿主里不同：
//   - `ctl_cycle_speed_ms = 10^13`：`mode_run` 用它算 `speed`，为 0 时按真时间放行，tick 就慢了；
//   - 每个数组 `p-8` 的 seq 头 `(len<<16)|cap_log2`：`.len()` 读的就是它，为 0 时循环体不执行。
void seed_protocol(uc_engine* uc, const Spec& spec, const Protocol& p) {
    wr_u64(uc, p.command, 0);
    wr_u64(uc, p.command_id, 0);
    wr_u64(uc, p.command + 2 * 8, kSpeed);  // ctl_cycle_speed_ms 紧随 ctl_command/ctl_command_id
    for (const auto& arr : spec.arrays()) wr_u64(uc, arr.addr - 8, (kArrLen << 16) | kArrCapLog2);
}

// host.py `_seed_reset_data`：把 `#RESET_DATA_*` 那几块（游戏里的真实内存/ROM 镜像）填成图案。
// 不灌 = 全零 = 「`reset_sim` 每次整块拷一遍」等于没拷（历史行为，`docs/facts` 有记录）。
// 两条硬检查照 host.py 的：块必须在已映射区间内，且不许盖住**别的**已声明对象 —— 灌错了就是
// 把图案写进协议区，那比不灌难查得多。
struct SeedStats {
    size_t blocks = 0;
    uint64_t bytes = 0;
};

SeedStats seed_reset_data(uc_engine* uc, const Spec& spec, Seed seed,
                          const std::vector<std::pair<uint64_t, uint64_t>>& spans) {
    SeedStats st;
    for (const auto& b : spec.reset_data_blocks()) {
        const uint64_t hi = b.lo + b.size;
        bool inside = false;
        for (const auto& [lo, span_hi] : spans) {
            if (lo <= b.lo && hi <= span_hi) {
                inside = true;
                break;
            }
        }
        if (!inside)
            throw std::runtime_error("RESET_DATA 块 " + b.name + " 的 [" + hex(b.lo) + ", " +
                                     hex(hi) + ") 不在已映射区间里 —— 灌图案会写坏内存");
        for (const auto& [name, addr] : spec.scalars()) {
            if (name != b.name && b.lo <= addr && addr < hi)
                throw std::runtime_error("RESET_DATA 块 " + b.name + " 盖住了已声明对象 " + name);
        }
        for (const auto& arr : spec.arrays()) {
            if (b.lo <= arr.addr && arr.addr < hi)
                throw std::runtime_error("RESET_DATA 块 " + b.name + " 盖住了数组 " + arr.name);
        }
        const std::vector<uint8_t> bytes = pattern(seed, b.size);
        check(uc_mem_write(uc, b.lo, bytes.data(), bytes.size()), "灌图案到 " + b.name);
        ++st.blocks;
        st.bytes += b.size;
    }
    return st;
}

struct RunCtx {
    Protocol p;
    std::unordered_set<uint64_t> unnamed_pcs;  // import 为空的 dll_refs 站点（指令地址）
    uint64_t unnamed_hits = 0;
    uint64_t unnamed_first_pc = 0;
    uint64_t instrs = 0;  // 本段（boot / run）执行的指令数
    uint64_t max_instrs = 0;
    uint64_t cmd_id = 0;  // 0 = 还没发命令，不判完成
    int stable = 0;
    bool done = false;
    bool capped = false;
    std::string error;
    uint64_t pc = 0;              // 当前指令的绝对地址（mem hook 不给 pc，只能这么记）
    bool running_zero = false;    // 监视窗口内 `sim_running` 被写 0（精确完成）
    std::vector<RunningWrite> running_writes;

    // 启动期的 `rep movsb` 特判（`try_rep_movsb`）。开关只在这里：抓迹窗口里必须关。
    struct RepFast {
        bool enabled = false;
        bool warned_df = false;
        bool warned_unmapped = false;
        std::vector<RepCopy> copies;
        uint64_t bytes = 0;
        std::vector<std::string> warnings;
        // 已判定「按字节执行」的站点，下标是 code 偏移。映射与 code 区字节在整轮里都不变，所以这个
        // 判定只做一次 —— 不记的话，一条被退回的 `rep movsb` 会**每字节**重跑一遍判定（实测慢
        // 2.3 倍）；用偏移下标的字节而不是哈希表，因为这查询就在 1.34 亿次回调的热路上。
        std::vector<uint8_t> give_up;
    } rep;
    const Image* image = nullptr;                    // 判「这条指令是不是 F3 A4」要它的字节
    std::vector<HostRegion>* host_regions = nullptr;  // 裸指针只能往这些块里写
};

// 启动期把 `rep movsb` 换成宿主原生 `memmove`：Unicorn 按**字节**执行这条指令（一字节一次 code
// hook），`reset_sim` 那次 128 MB 内存镜像拷贝因此是 1.34 亿条指令、5 s。特判只做两件事 ——
// 拷完、把 rcx 清 0（`rep movsb` 此时什么都不做：指令照常执行、rip 照常前进 2 字节）。
//
// 只在启动期开：抓迹窗口里这么做会**跳过 mem write hook**（`uc_mem_write` 与宿主写都不触发
// hook），那一段 store 就从序列里消失了，等于把判据挖掉一块。
//
// 三处必退（宁可按字节跑，不改语义）：
//   - count = 0：此时 `rep movsb` 什么都不做，特判也不能动内存/寄存器；
//   - DF = 1：方向递减，特判只做递增；
//   - 源/目的不在 `uc_mem_map_ptr` 的宿主内存里：没有裸指针可写。
void try_rep_movsb(uc_engine* uc, RunCtx* c, uint64_t pc) {
    RunCtx::RepFast& rep = c->rep;
    const Image& image = *c->image;
    const uint64_t off = pc - image.code_base;
    if (pc < image.code_base || off >= image.code.size()) return;
    if (rep.give_up[off] != 0) return;
    if (!is_rep_movsb_encoding(image.code.data() + off, image.code.size() - off)) return;
    // `image.code` 是**改写前**的字节（`patch_dll_refs` 改的是引擎里的代码区），所以再按引擎里的
    // 真字节复核一次：把一条真指令的 rcx 清零就是在破坏它。
    uint8_t encoding[2] = {};
    if (uc_mem_read(uc, pc, encoding, sizeof(encoding)) != UC_ERR_OK) return;
    if (!is_rep_movsb_encoding(encoding, sizeof(encoding))) {
        rep.give_up[off] = 1;
        return;
    }

    const uint64_t count = reg(uc, UC_X86_REG_RCX);
    if (count == 0) return;
    if ((reg(uc, UC_X86_REG_EFLAGS) & kDirectionFlag) != 0) {
        if (!rep.warned_df) {
            rep.warnings.push_back("DF=1 的 `rep movsb` 不特判，按字节执行（首次 pc=" + hex(pc) +
                                   " count=" + std::to_string(count) + "）");
            rep.warned_df = true;
        }
        return;
    }

    const uint64_t src = reg(uc, UC_X86_REG_RSI);
    const uint64_t dst = reg(uc, UC_X86_REG_RDI);
    HostRegion* from = find_host_region(*c->host_regions, src, count);
    HostRegion* to = find_host_region(*c->host_regions, dst, count);
    if (from == nullptr || to == nullptr) {
        if (!rep.warned_unmapped) {
            rep.warnings.push_back("`rep movsb` 的源/目的不在宿主映射里，按字节执行（首次 pc=" +
                                   hex(pc) + " src=" + hex(src) + " dst=" + hex(dst) + "）");
            rep.warned_unmapped = true;
        }
        rep.give_up[off] = 1;
        return;
    }

    // `memmove` 而不是 `memcpy`：递增方向的 `rep movsb` 在源/目的重叠时是「按序逐字节」的语义，
    // 重叠下用 `memcpy` 是未定义行为，而这里没有任何东西能保证两块不重叠。
    std::memmove(to->bytes.get() + (dst - to->lo), from->bytes.get() + (src - from->lo),
                 static_cast<size_t>(count));
    set_reg(uc, UC_X86_REG_RCX, 0);
    rep.copies.push_back(RepCopy{pc, count, src, dst});
    rep.bytes += count;
}

void check_hook(uc_engine* uc, uint64_t address, uint32_t /*size*/, void* user_data) {
    RunCtx* c = static_cast<RunCtx*>(user_data);
    c->pc = address;
    ++c->instrs;
    if (c->rep.enabled) try_rep_movsb(uc, c, address);

    if (!c->unnamed_pcs.empty() && c->unnamed_pcs.count(address) != 0) {
        ++c->unnamed_hits;
        if (c->unnamed_first_pc == 0) c->unnamed_first_pc = address;
    }
    if (c->instrs > c->max_instrs) {
        c->capped = true;
        uc_emu_stop(uc);
        return;
    }
    if (c->cmd_id == 0 || c->instrs % kCheckEvery != 0) return;

    uint64_t last = 0, running = 0;
    const uc_err e1 = uc_mem_read(uc, c->p.last_command_id, &last, sizeof(last));
    const uc_err e2 = uc_mem_read(uc, c->p.running, &running, sizeof(running));
    if (e1 != UC_ERR_OK || e2 != UC_ERR_OK) {
        c->error = std::string("读完成判据失败: ") + uc_strerror(e1 != UC_ERR_OK ? e1 : e2);
        uc_emu_stop(uc);
        return;
    }
    if (last == c->cmd_id && running == 0) {
        if (++c->stable >= kDoneSamples) {
            c->done = true;
            uc_emu_stop(uc);
        }
    } else {
        c->stable = 0;
    }
}

struct WaitCtx {
    uint64_t reads = 0;
    bool reached = false;
};

void wait_read_hook(uc_engine* uc, uc_mem_type /*type*/, uint64_t /*addr*/, int /*size*/,
                    int64_t /*value*/, void* user_data) {
    WaitCtx* w = static_cast<WaitCtx*>(user_data);
    if (++w->reads < kWaitReads) return;
    w->reached = true;
    uc_emu_stop(uc);
}

// `sim_running` 的写监视：程序跑完一次 run 的动作就是把它写回 0，所以这一下就是切点。
// 它比 Trace 的写 hook 后挂：万一 Unicorn 按挂载顺序回调，Trace 会先把这条写记进序列再停。
void running_write_hook(uc_engine* uc, uc_mem_type /*type*/, uint64_t /*addr*/, int size,
                        int64_t value, void* user_data) {
    RunCtx* c = static_cast<RunCtx*>(user_data);
    const uint64_t v = static_cast<uint64_t>(value);
    c->running_writes.push_back(RunningWrite{c->pc, static_cast<uint32_t>(size), v});
    // size > 8 时 Unicorn 不给真值（见 Trace::write_hook），不能当「写了 0」。
    if (size > 8 || v != 0) return;
    c->running_zero = true;
    uc_emu_stop(uc);
}

// `error_buffer` 的前 2 字节是小端 U16 的字节数（`rv32im.si:174` 的 `store(error_buffer, U16 len)`）。
// 按字节读，不依赖宿主端序。
uint16_t error_len(uc_engine* uc, uint64_t addr) {
    uint8_t b[2] = {};
    if (uc_mem_read(uc, addr, b, sizeof(b)) != UC_ERR_OK) return 0;
    return static_cast<uint16_t>(b[0] | (static_cast<uint16_t>(b[1]) << 8));
}

std::string where(uc_engine* uc) {
    return "pc=" + hex(reg(uc, UC_X86_REG_RIP)) + " rsp=" + hex(reg(uc, UC_X86_REG_RSP));
}

// boot 快照的字节数上限：`game_hi - game_lo` 超了就不写（一次性几 GB 的写入只能是把 DLL / host
// 地址算进了游戏区间，不是「大电路的快照」）。
constexpr uint64_t kMaxBootDumpBytes = 2ull << 30;

// `--dump-boot`：`[game_lo, game_hi)` 的原始字节，无 header（地址由调用方从 `.si` 算）。
// 文件大小恒等于 `game_hi - game_lo`：区间内没有映射的地址写 0 —— 快照吃的是一段连续地址，
// 而 `map_game` 只映射 `spec.regions()` 合并后的段；同一份 `.si` 两边段几何相同，空洞也一样。
struct BootDump {
    uint64_t lo = 0;
    uint64_t hi = 0;
    uint64_t bytes = 0;
};

BootDump dump_boot_memory(uc_engine* uc, const Spec& spec, const Layout& layout,
                          const std::string& path) {
    BootDump d;
    d.lo = spec.game_lo();
    d.hi = spec.game_hi();
    if (d.hi < d.lo)
        throw std::runtime_error("difftrace: --dump-boot 的游戏地址空间是负的: " + hex(d.lo) + " .. " +
                                 hex(d.hi));
    d.bytes = d.hi - d.lo;
    if (d.bytes > kMaxBootDumpBytes)
        throw std::runtime_error("difftrace: --dump-boot 的游戏地址空间 " + hex(d.lo) + " .. " +
                                 hex(d.hi) + " 有 " + std::to_string(d.bytes) +
                                 " 字节，超过 2 GiB 上限");

    std::vector<uint8_t> bytes(d.bytes, 0);
    for (const auto& [lo, hi] : layout.spans) {
        const uint64_t a = std::max(lo, d.lo);
        const uint64_t b = std::min(hi, d.hi);
        if (a >= b) continue;
        check(uc_mem_read(uc, a, bytes.data() + (a - d.lo), b - a),
              "读 boot 快照 " + hex(a) + ".." + hex(b));
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) throw std::runtime_error("difftrace: 打不开 boot 快照文件写: " + path);
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    const int closed = std::fclose(f);
    if (n != bytes.size() || closed != 0)
        throw std::runtime_error("difftrace: 写 boot 快照文件失败: " + path + "（写了 " +
                                 std::to_string(n) + " / " + std::to_string(bytes.size()) +
                                 " 字节）");
    return d;
}

// 要写桩地址的槽清单 —— `Stubs::install` 的唯一输入，magic 编号 = 清单下标。
// 两条导入机制互斥地贡献槽：参考产物全是 `dll_refs` 的 IAT 槽（`image.dll.iat` 整张表：DLL 自己的
// 代码也会调到别的导入），我们的产物全是 sidecar 指出的位置（`stub_refs`）。两者都只是
// 「某个导入的地址存在这里」，所以合成一份 —— host.py 的 `_patch_externs` 也是让 extern 跳板
// 拿到与 IAT 槽**同一个**桩地址（同一个 handler、同一份记录），两边的调用序列才能直接比。
//
// 顺序必须稳定：DLL 整表在前（参考产物的编号与以前逐字一致），sidecar 按文件顺序在后。
std::vector<Thunk> import_slots(const Image& image) {
    std::vector<Thunk> slots = image.dll.iat;
    slots.reserve(slots.size() + image.stub_refs.size());
    for (const StubRef& ref : image.stub_refs) {
        // 代码桩要改的是 `movabs rax,imm64` 的 imm64（指令起点 +2），数据槽是它自己。
        const uint64_t at = image.code_base + ref.code_offset + (ref.kind == StubKind::Code ? 2 : 0);
        slots.push_back({at, ref.import});
    }
    return slots;
}

// 认不出的导入 `Stubs::install` 不动它的槽（stubs.hh 的理由），而 sidecar 的槽里留着烤进来的
// 绝对地址 —— 那一跳会落在映射外，现场只能看到一个 `UC_ERR_FETCH_UNMAPPED`。所以在这里先吵，
// 并把符号名报出来（host.py 的 `EXIT_STUB_CONTRACT` 同：宁可死，不把不可控的真调用放进去）。
void require_stub_imports(const Image& image,
                          const std::vector<std::pair<uint64_t, std::string>>& magic_table) {
    for (const StubRef& ref : image.stub_refs) {
        const bool known = std::any_of(magic_table.begin(), magic_table.end(),
                                       [&](const std::pair<uint64_t, std::string>& e) {
                                           return e.second == ref.import;
                                       });
        if (!known)
            throw std::runtime_error("sidecar 的 " + hex(ref.code_offset) + "（" +
                                     (ref.kind == StubKind::Code ? "stub " : "slot ") + ref.import +
                                     "）没有桩：magic 表里没有这个名字");
    }
}

}  // namespace

void Trace::attach(uc_engine* uc) {
    uc_ = uc;
    // begin=1, end=0 = 「所有地址」（与 `work/tmp/unicorn_sdk/bench.cc` 同一个写法）。
    check(uc_hook_add(uc, &code_hook_, UC_HOOK_CODE, reinterpret_cast<void*>(&Trace::code_hook), this,
                      1, 0),
          "Trace: 挂 code hook");
    check(uc_hook_add(uc, &write_hook_, UC_HOOK_MEM_WRITE,
                      reinterpret_cast<void*>(&Trace::write_hook), this, 1, 0),
          "Trace: 挂 write hook");
}

void Trace::code_hook(uc_engine* /*uc*/, uint64_t address, uint32_t /*size*/, void* user_data) {
    Trace* t = static_cast<Trace*>(user_data);
    t->pc_ = address;
    ++t->instrs_;
}

void Trace::write_hook(uc_engine* /*uc*/, uc_mem_type /*type*/, uint64_t addr, int size,
                       int64_t value, void* user_data) {
    Trace* t = static_cast<Trace*>(user_data);
    // size > 8（SSE 的 16 字节写）时 Unicorn 不给真值 —— 记成 0 会在两边同样出现，不影响判等。
    t->stores_.push_back(Store{t->pc_, addr, static_cast<uint32_t>(size),
                               size > 8 ? 0 : static_cast<uint64_t>(value)});
}

Layout map_game(uc_engine* uc, const Spec& spec, const Image& image,
                std::vector<HostRegion>& host_regions) {
    Layout out;
    const auto spans = merge_ranges(spec.regions());
    out.spans = spans;
    for (const auto& [lo, hi] : spans) {
        HostRegion region;
        region.lo = lo;
        region.hi = hi;
        region.bytes = alloc_host_bytes(hi - lo);
        // `uc_mem_map_ptr`：引擎直接用这块宿主内存，没有引擎侧的影子拷贝，于是 `try_rep_movsb`
        // 能拿裸指针把 128 MB 一次拷完。失败**只能报错**——静默退回 `uc_mem_map` 会让
        // 「为什么这轮还是 5 s」变成一个查不出来的谜。
        check(uc_mem_map_ptr(uc, lo, hi - lo, UC_PROT_ALL, region.bytes.get()),
              "用 uc_mem_map_ptr 映射游戏段 " + hex(lo) + "（地址与大小要是 4 KB 的倍数）");
        host_regions.push_back(std::move(region));
        ++out.regions;
        out.mapped_bytes += hi - lo;
    }

    // 代码区：host.py `_start_thread` 给 `len(code) + PAGE`，这里同几何（多出的一页是零，垫住
    // 产物数据段尾部可能的对齐/越界访问）。
    const uint64_t code_len = align_up(image.code.size() + kPage, kPage);
    check(uc_mem_map(uc, image.code_base, code_len, UC_PROT_ALL), "映射代码区 " + hex(image.code_base));
    check(uc_mem_write(uc, image.code_base, image.code.data(), image.code.size()),
          "写代码区 " + hex(image.code_base));

    // DLL 镜像：host.py 里那块是真加载的 `compile_real.dll`（`_load_dll`）—— 产物读的 `.bss`
    // （rv32im：rva 0x131280，13 处无名字站点里 10 处指它）是它的零页，产物调用的
    // rva 0x1350/0x1380 是它 `.text` 里的**真函数**，thunk 是它 `.text` 里的 `jmp [rip+rel32]`。
    // 所以这一块必须交出真字节、摆在 `baked` 上（和真进程里「DLL 加载在 baked」同形）：
    // 段内 raw 字节照写，raw 之外（`.bss`、节尾填充）保持零页。IAT 槽随后由 `Stubs::install`
    // 填成 magic 地址，DLL 自己那些 `call [rip+rel]` 因此也走到同一批桩上。
    if (!image.dll.image.empty()) {
        check(uc_mem_map(uc, image.dll.base, align_up(image.dll.image.size(), kPage), UC_PROT_ALL),
              "映射 DLL 镜像 " + hex(image.dll.base));
        check(uc_mem_write(uc, image.dll.base, image.dll.image.data(), image.dll.image.size()),
              "写 DLL 镜像 " + hex(image.dll.base));
    }

    return out;
}

PatchStats patch_dll_refs(uc_engine* uc, const Image& image,
                          const std::vector<std::pair<uint64_t, std::string>>& magic_table) {
    PatchStats out;
    for (const DllRef& ref : image.dll_refs) {
        if (ref.import.empty()) {
            ++out.untouched;  // 落在 DLL 镜像里但不在 thunk 表里：身份未查清，原样不动
            continue;
        }
        uint64_t magic = 0;
        size_t found = 0;
        for (const auto& [addr, name] : magic_table) {
            if (name != ref.import) continue;
            magic = addr;
            ++found;
        }
        const std::string at = hex(image.code_base + ref.code_offset);
        if (found != 1)
            throw std::runtime_error("站点 " + at + "（" + ref.import + "）的桩在 magic 表里出现 " +
                                     std::to_string(found) + " 次，无法确定该写哪个地址");
        check(uc_mem_write(uc, image.code_base + ref.code_offset, &magic, sizeof(magic)),
              "改写站点 " + at + "（" + ref.import + "）");
        ++out.patched;
    }
    return out;
}

RunOutcome run_to_cycle(uc_engine* uc, const Spec& spec, const Image& image, uint64_t target_cycle,
                        Seed seed, const std::string& boot_dump_path) {
    RunOutcome out;
    out.layout = map_game(uc, spec, image, out.host_regions);

    // Stubs 不拥有 uc，必须比它先析构 —— 所以它是本函数的局部变量，uc 由调用方 close。
    Stubs stubs(uc);
    // 整张导入表 + sidecar 的槽：`install` 把认得的那几个槽写成 `MAGIC_BASE + i`（编号的唯一处）。
    stubs.install(import_slots(image));
    out.refs = patch_dll_refs(uc, image, stubs.magic_table());
    require_stub_imports(image, stubs.magic_table());
    out.refs.stubs = image.stub_refs.size();

    const Protocol p = resolve_protocol(spec);
    out.settings_addr = p.settings;
    out.sim_running_index = p.running_index;
    out.sim_running_addr = p.running;
    seed_protocol(uc, spec, p);
    const SeedStats seeded = seed_reset_data(uc, spec, seed, out.layout.spans);
    out.seeded_blocks = seeded.blocks;
    out.seeded_bytes = seeded.bytes;

    check(uc_mem_map(uc, kStackBase, kStackSize, UC_PROT_ALL), "映射栈");
    set_reg(uc, UC_X86_REG_RSP, align_up(kStackBase + kStackSize, 16) - 16);

    RunCtx ctx;
    ctx.p = p;
    ctx.image = &image;
    ctx.host_regions = &out.host_regions;
    // `rep movsb` 特判只在启动期开（理由见 `try_rep_movsb`）。
    ctx.rep.enabled = true;
    ctx.rep.give_up.assign(image.code.size(), 0);
    // 「踩到」= 那条 movabs 指令被执行。`code_offset` 是 imm64 的偏移，指令起点在它前 2 字节
    // （REX + opcode）。
    for (const DllRef& ref : image.dll_refs)
        if (ref.import.empty()) ctx.unnamed_pcs.insert(image.code_base + ref.code_offset - 2);

    uc_hook hook = 0;
    check(uc_hook_add(uc, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(&check_hook), &ctx, 1, 0),
          "挂 driver code hook");

    // -- 阶段 A：跑到「等命令」-----------------------------------------------------
    WaitCtx wait;
    uc_hook read_hook = 0;
    check(uc_hook_add(uc, &read_hook, UC_HOOK_MEM_READ, reinterpret_cast<void*>(&wait_read_hook),
                      &wait, p.command_id, p.command_id + 7),
          "挂 ctl_command_id 读 hook");

    const uint64_t entry = image.code_base + image.entry;
    ctx.max_instrs = kMaxBootInstrs;
    uc_err err = uc_emu_start(uc, entry, 0, 0, 0);
    uc_hook_del(uc, read_hook);
    uc_hook_del(uc, hook);
    ctx.rep.enabled = false;  // 抓迹之前关：宿主写不进 store 序列
    out.rep_copies = ctx.rep.copies;
    out.rep_bytes = ctx.rep.bytes;
    out.warnings = ctx.rep.warnings;
    out.instrs_boot = ctx.instrs;
    out.wait_reads = wait.reads;
    out.unnamed_hits = ctx.unnamed_hits;
    out.unnamed_first_pc = ctx.unnamed_first_pc;

    if (err != UC_ERR_OK) {
        out.result.reason = std::string("uc_error: ") + uc_strerror(err) + "（" + where(uc) + "）";
        out.stop_pc = reg(uc, UC_X86_REG_RIP);
        return out;
    }
    if (!stubs.error().empty()) {
        out.result.reason = "stub_error: " + stubs.error();
        out.stop_pc = reg(uc, UC_X86_REG_RIP);
        return out;
    }
    if (!wait.reached) {
        out.result.reason = "no_wait_state（boot " + std::to_string(ctx.instrs) + " 条指令、" +
                            std::to_string(ctx.capped ? 1 : 0) + "=capped，ctl_command_id 只被读了 " +
                            std::to_string(wait.reads) + " 次；" + where(uc) + "）";
        out.stop_pc = reg(uc, UC_X86_REG_RIP);
        return out;
    }

    // 快照必须在这里拍：启动期的写不进 store 序列（阶段 B 才 attach），它是唯一能看见那段内存的
    // 窗口；位置在发命令之前 = 「初始化完了、还没跑」那一刻的状态。
    if (!boot_dump_path.empty()) {
        const BootDump d = dump_boot_memory(uc, spec, out.layout, boot_dump_path);
        out.boot_dump = boot_dump_path;
        out.boot_lo = d.lo;
        out.boot_hi = d.hi;
        out.boot_bytes = d.bytes;
    }

    // -- 阶段 B：发一次 run ------------------------------------------------------
    // 顺序照 host.py `_tick`：先 `sim_target_cycle`，再 `ctl_command`，最后 `ctl_command_id`。
    wr_u64(uc, p.target_cycle, target_cycle);
    wr_u64(uc, p.command, p.run_value);
    ctx.cmd_id = 1;
    wr_u64(uc, p.command_id, ctx.cmd_id);

    // 抓迹从发命令那一刻起：启动期的 `reset_sim` 每轮整块 memcpy 一次内存镜像（rv32im 128 MB），
    // 记下来只会把序列淹掉，而两边的启动期本来就等价（同源码地址、同桩返回值）。
    Trace trace;
    trace.attach(uc);

    const uint64_t resume = reg(uc, UC_X86_REG_RIP);
    ctx.instrs = 0;
    ctx.max_instrs = kMaxRunInstrs;
    ctx.stable = 0;

    // 监视窗口从这里开始：进 run 之前 `sim_running` 也是 0（初值），那是「还没开始」不是「跑完了」。
    uc_hook running_hook = 0;
    check(uc_hook_add(uc, &running_hook, UC_HOOK_MEM_WRITE,
                      reinterpret_cast<void*>(&running_write_hook), &ctx, p.running, p.running + 7),
          "挂 sim_running 写 hook");

    check(uc_hook_add(uc, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(&check_hook), &ctx, 1, 0),
          "挂 driver code hook（run）");
    err = uc_emu_start(uc, resume, 0, 0, 0);
    uc_hook_del(uc, hook);
    uc_hook_del(uc, running_hook);

    out.result.instrs = out.instrs_boot + ctx.instrs;
    out.result.stores = trace.stores().size();
    out.stores = trace.stores();
    out.unnamed_hits = ctx.unnamed_hits;
    out.unnamed_first_pc = ctx.unnamed_first_pc;
    out.running_writes = ctx.running_writes;
    out.stop_pc = reg(uc, UC_X86_REG_RIP);
    out.result.sim_cycle = rd_u64(uc, p.cycle);
    out.result.error_len = error_len(uc, p.error_buffer);

    if (err != UC_ERR_OK) {
        out.result.reason = std::string("uc_error: ") + uc_strerror(err) + "（" + where(uc) + "）";
    } else if (!stubs.error().empty()) {
        out.result.reason = "stub_error: " + stubs.error();
    } else if (!ctx.error.empty()) {
        out.result.reason = ctx.error;
    } else if (ctx.running_zero) {
        out.result.reason = "running_zero";
    } else if (ctx.done) {
        out.result.reason = "sampled";  // 兜底：`sim_running` 没被写 0，采样判据成立
    } else if (ctx.capped) {
        out.result.reason = "timeout（跑满 " + std::to_string(ctx.max_instrs) + " 条指令，判据未成立）";
    } else {
        out.result.reason = "no_done（仿真在判据成立前自己停了，且 sim_running 没被写 0）";
    }
    return out;
}

}  // namespace tc
