/* stubs.cc —— 导入函数桩的实现（接口与确定性要求见 stubs.hh）。
 *
 * 每个桩的语义都对着 `host.py` 的 `Detours` 一个个抄，抄错一个就等于给差分工具埋一个假分叉：
 *
 *   Sleep                           推进虚拟时钟，不真睡（真睡会让节流自旋次数取决于真实机器）
 *   GetSystemTimePreciseAsFileTime  写 116444736000000000 + clock()，每次 +10 ms
 *   VirtualAlloc                    K4：提交按请求顺序递增真映射；只 RESERVE 的一页都不提交
 *   VirtualFree                     记账、返回 1，不真释放（地址一复用两边序列就对不上）
 *   WriteFile / fwrite / _write / puts / __stdio_common_vfprintf   返回成功，只记前 8 字节
 *   CreateFileA                     一律 INVALID_HANDLE_VALUE（真开文件会拿真实磁盘，不可重复）
 *   CloseHandle                     返回 1
 *   ExitThread / ExitProcess        uc_emu_stop（真结束线程/进程就没有仿真了）
 *   memcpy                          真拷贝（这五份语料用它搬内存；host.py 跑真进程，不需要这个桩）
 */
#include "stubs.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace tc {
namespace {

constexpr uint64_t kPage = 0x1000;

/* host.py 的 arena：确定性 bump 分配器首址 4 GB（x64 用户空间内，游戏段与它不重叠）。 */
constexpr uint64_t kArenaBase = 0x100000000ull;
/* 只 RESERVE 的保留区首址：64 TiB，远高于游戏段（≈2.4 TB）与 arena，且不与两者相邻。 */
constexpr uint64_t kReserveBase = 0x400000000000ull;

constexpr uint64_t kTicksPerMs = 10000;                    /* FILETIME 用 100ns 单位 */
constexpr uint64_t kFiletimeEpoch = 116444736000000000ull; /* 1970-01-01 的 FILETIME */
constexpr uint64_t kInvalidHandle = 0xFFFFFFFFFFFFFFFFull;

constexpr uint64_t kMemCommit = 0x1000;
constexpr uint64_t kMemReserve = 0x2000;

/* magic 区用 `ret` 填：万一 hook 没拦住（地址落在识别表外），那一下会照常返回调用方，
 * 不会去执行一块垃圾填充。 */
constexpr uint8_t kMagicFiller = 0xC3;

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

Stubs::Stubs(uc_engine* uc) : uc_(uc), arena_next_(kArenaBase), reserve_next_(kReserveBase) {}

Stubs::~Stubs() {
    /* `uc` 必须活得比 `Stubs` 久：先 uc_close 再析构会踩已释放的内存（实测段错误），
     * 因为 uc_hook_del 要写引擎内部的 hook 表。这是调用方的责任 —— Stubs 不拥有 uc，
     * 也没办法判断那个句柄还有效。 */
    if (hooked_) {
        uc_hook_del(uc_, hook_);
        hooked_ = false;
    }
}

void Stubs::install(const std::vector<Thunk>& thunks) {
    if (thunks.empty()) return;

    /* 认不出的导入也占一个号，这样 magic 地址只由 thunks 的下标决定，而不由「认得出几个」决定
     * —— 否则同一个导入在两边跑到不同下标，桩地址就不同了。 */
    table_.clear();
    kinds_.clear();
    unknown_.clear();
    for (size_t i = 0; i < thunks.size(); ++i) {
        const uint64_t magic = MAGIC_BASE + i * MAGIC_STRIDE;
        const std::string& name = thunks[i].import;
        const Kind* kind = nullptr;
        static const std::pair<const char*, Kind> known[] = {
            {"Sleep", Kind::kSleep},
            {"GetSystemTimePreciseAsFileTime", Kind::kGetFileTime},
            {"VirtualAlloc", Kind::kVirtualAlloc},
            {"VirtualFree", Kind::kVirtualFree},
            {"WriteFile", Kind::kWriteFile},
            {"puts", Kind::kPuts},
            {"fwrite", Kind::kFwrite},
            {"_write", Kind::kWrite},
            {"__stdio_common_vfprintf", Kind::kVfprintf},
            {"CreateFileA", Kind::kCreateFileA},
            {"CloseHandle", Kind::kCloseHandle},
            {"ExitThread", Kind::kExitThread},
            {"ExitProcess", Kind::kExitProcess},
            /* 只是 CRT 的内存拷贝，但它落在 `unknown_` 里就不改写 IAT 槽 —— 执行时跳到 DLL 的
             * thunk、再跳真的 Windows 地址，Unicorn 里那不可执行（069/082/086/088/090 实测报
             * 「sidecar 的 …（stub memcpy）没有桩」）。`host.py` 不需要这个桩：它跑在真进程里。 */
            {"memcpy", Kind::kMemcpy},
        };
        for (const auto& k : known) {
            if (name == k.first) {
                kind = &k.second;
                break;
            }
        }
        if (kind == nullptr) {
            unknown_.push_back(name);
            continue;
        }
        table_.emplace_back(magic, name);
        kinds_.emplace(magic, *kind);

        const uint64_t slot = magic;  /* 槽里写的就是桩地址本身 */
        const uc_err err = uc_mem_write(uc_, thunks[i].slot_va, &slot, sizeof(slot));
        if (err != UC_ERR_OK) {
            throw std::runtime_error("Stubs::install: 写 IAT 槽 " + hex(thunks[i].slot_va) + "（" +
                                     name + "）失败: " + uc_strerror(err));
        }
    }
    if (table_.empty()) return;

    /* 一块 magic 区放得下所有下标（含认不出的号的空洞），填满 ret，再挂一个区间 hook。 */
    magic_len_ = align_up(thunks.size() * MAGIC_STRIDE, kPage);
    const uc_err err = uc_mem_map(uc_, MAGIC_BASE, magic_len_, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Stubs::install: 映射 magic 区 " + hex(MAGIC_BASE) + " 失败: " +
                                 uc_strerror(err));
    }
    std::vector<uint8_t> filler(magic_len_, kMagicFiller);
    if (uc_mem_write(uc_, MAGIC_BASE, filler.data(), filler.size()) != UC_ERR_OK) {
        throw std::runtime_error("Stubs::install: 填充 magic 区失败");
    }

    const uc_err herr = uc_hook_add(uc_, &hook_, UC_HOOK_CODE, reinterpret_cast<void*>(&Stubs::code_hook),
                                    this, MAGIC_BASE, MAGIC_BASE + magic_len_ - 1);
    if (herr != UC_ERR_OK) {
        throw std::runtime_error(std::string("Stubs::install: 挂 UC_HOOK_CODE 失败: ") + uc_strerror(herr));
    }
    hooked_ = true;
}

std::vector<std::pair<uint64_t, std::string>> Stubs::magic_table() const {
    std::vector<std::pair<uint64_t, std::string>> out = table_;
    std::sort(out.begin(), out.end());
    return out;
}

void Stubs::code_hook(uc_engine* /*uc*/, uint64_t address, uint32_t /*size*/, void* user_data) {
    static_cast<Stubs*>(user_data)->dispatch(address);
}

void Stubs::dispatch(uint64_t address) {
    const auto it = kinds_.find(address);
    if (it == kinds_.end()) return;  /* hook 范围比识别表大（含空洞），落在空洞上就什么也不做 */

    const uint64_t a0 = arg(0), a1 = arg(1), a2 = arg(2), a3 = arg(3);
    if (!error_.empty()) return;

    uint64_t rax = 0;
    bool stop = false;
    switch (it->second) {
        case Kind::kSleep:
            /* 节奏类：不记录（条数取决于真实耗时），但要推进时钟，否则程序按「时间没走」自旋。 */
            clock_ += a0 * kTicksPerMs;
            break;

        case Kind::kGetFileTime:
            clock_ += 10 * kTicksPerMs;
            write_u64(a0, kFiletimeEpoch + clock_);
            break;

        case Kind::kVirtualAlloc:
            rax = virtual_alloc(a0, a1, a2);
            note("VirtualAlloc", {a0, a1, a2, a3}, rax);
            break;

        case Kind::kMemcpy: {
            /* `void* memcpy(void* dst, const void* src, size_t n)`：Win64 前三个实参在 rcx/rdx/r8。
             * 必须真拷贝 —— 这几份语料拿它搬大块内存，不拷贝后续读到的就是旧值。
             * 上限只为防一个坏长度把宿主吃光，不是语义的一部分。 */
            static constexpr uint64_t kMaxCopy = 64ull << 20;
            if (a2 > kMaxCopy) {
                error_ = "memcpy 的长度 " + std::to_string(a2) + " 超出 64 MB 上限";
                uc_emu_stop(uc_);
                return;
            }
            if (a2 != 0) {
                std::vector<uint8_t> buf(static_cast<size_t>(a2));
                const uc_err rerr = uc_mem_read(uc_, a1, buf.data(), a2);
                if (rerr != UC_ERR_OK) {
                    fail("memcpy 读源", rerr);
                    return;
                }
                const uc_err werr = uc_mem_write(uc_, a0, buf.data(), a2);
                if (werr != UC_ERR_OK) {
                    fail("memcpy 写目的", werr);
                    return;
                }
            }
            rax = a0;
            note("memcpy", {a0, a1, a2}, rax);
            break;
        }

        case Kind::kVirtualFree:
            /* 记账但不真释放：真释放会让后面的 VirtualAlloc 复用地址，两边复用时机一差就分叉。 */
            rax = 1;
            note("VirtualFree", {a0, a1, a2}, rax);
            break;

        case Kind::kWriteFile:
            if (a3 != 0) write_u64(a3, a1);
            rax = 1;
            note("WriteFile", {0, a1, peek_u64(a2, a1)}, rax);
            break;

        case Kind::kPuts:
            rax = 0;
            note("puts", {peek_u64(a0, 32)}, rax);
            break;

        case Kind::kFwrite:
            rax = a2;
            note("fwrite", {peek_u64(a0, a1), a1, a2}, rax);
            break;

        case Kind::kWrite:
            rax = a2;
            note("_write", {peek_u64(a1, a2), a2}, rax);
            break;

        case Kind::kVfprintf:
            rax = 0;
            note("__stdio_common_vfprintf", {}, rax);
            break;

        case Kind::kCreateFileA:
            rax = kInvalidHandle;
            note("CreateFileA", {}, rax);
            break;

        case Kind::kCloseHandle:
            rax = 1;
            note("CloseHandle", {a0}, rax);
            break;

        case Kind::kExitThread:
            note("ExitThread", {a0}, 0);
            stop = true;
            break;

        case Kind::kExitProcess:
            /* 真 ExitProcess 会杀掉整个仿真；这里与 ExitThread 同形：停下这次 run。 */
            note("ExitProcess", {a0}, 0);
            stop = true;
            break;
    }
    if (!error_.empty()) return;

    set_rax(rax);
    if (stop) {
        uc_emu_stop(uc_);
        return;
    }
    pop_return();
}

uint64_t Stubs::arg(int i) const {
    static const int regs[4] = {UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9};
    uint64_t v = 0;
    const uc_err err = uc_reg_read(uc_, regs[i], &v);
    if (err != UC_ERR_OK) {
        /* 回调里不能抛：穿过 C 栈帧的异常是 UB。 */
        const_cast<Stubs*>(this)->fail("读参数寄存器失败", err);
        return 0;
    }
    return v;
}

void Stubs::set_rax(uint64_t v) {
    const uc_err err = uc_reg_write(uc_, UC_X86_REG_RAX, &v);
    if (err != UC_ERR_OK) fail("写 rax 失败", err);
}

void Stubs::pop_return() {
    /* 手动返回：调用点(`call *__imp_X(%rip)`)把返回地址压在 [rsp] 上，callee 不执行 `ret`，
     * 所以这里自己弹 —— 只改 rsp/rip，不动别的寄存器（rax 已写）。 */
    uint64_t rsp = 0;
    if (uc_reg_read(uc_, UC_X86_REG_RSP, &rsp) != UC_ERR_OK) {
        fail("读 rsp 失败", UC_ERR_HOOK);
        return;
    }
    uint64_t ret = 0;
    const uc_err err = uc_mem_read(uc_, rsp, &ret, sizeof(ret));
    if (err != UC_ERR_OK) {
        fail("读返回地址失败", err);
        return;
    }
    rsp += sizeof(ret);
    if (uc_reg_write(uc_, UC_X86_REG_RSP, &rsp) != UC_ERR_OK ||
        uc_reg_write(uc_, UC_X86_REG_RIP, &ret) != UC_ERR_OK) {
        fail("写 rsp/rip 失败", UC_ERR_HOOK);
    }
}

void Stubs::write_u64(uint64_t va, uint64_t v) {
    const uc_err err = uc_mem_write(uc_, va, &v, sizeof(v));
    if (err != UC_ERR_OK) fail(("写 guest 内存 " + hex(va) + " 失败").c_str(), err);
}

uint64_t Stubs::peek_u64(uint64_t va, uint64_t n) const {
    /* 记录里的「前 8 字节」：只作诊断，读不到就给 0（不因此判这次运行作废）。 */
    uint8_t buf[8] = {};
    const uint64_t len = std::min<uint64_t>(n, sizeof(buf));
    if (va == 0 || len == 0) return 0;
    if (uc_mem_read(uc_, va, buf, len) != UC_ERR_OK) return 0;
    uint64_t v = 0;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

void Stubs::note(const char* name, std::initializer_list<uint64_t> args, uint64_t ret) {
    calls_.push_back(Call{name, std::vector<uint64_t>(args), ret});
}

void Stubs::fail(const char* what, uc_err err) {
    if (error_.empty()) error_ = std::string(what) + ": " + uc_strerror(err);
    uc_emu_stop(uc_);
}

uint64_t Stubs::virtual_alloc(uint64_t addr, uint64_t size, uint64_t type) {
    /* 三个分支照抄 host.py 的 `_virtual_alloc`：参考产物（Nim 分配器）启动就
     * `VirtualAlloc(NULL, 10^10, MEM_RESERVE, PAGE_NOACCESS)` ×N 当 arena，之后按需 COMMIT 里面的
     * 小片。把「只 RESERVE」当提交走，就得为 10 GB ×N 真映射 —— 那是另一条路，不是这里的事。 */
    if (size == 0) return 0; /* 真 API 也失败 */
    if (addr != 0) {
        if (type & kMemCommit) {
            const uint64_t lo = addr & ~(kPage - 1);
            const uint64_t hi = align_up(addr + size, kPage);
            const uc_err err = uc_mem_map(uc_, lo, hi - lo, UC_PROT_ALL);
            /* 已提交的区间再 COMMIT 在真 API 里是成功的 → 映射冲突不当失败。 */
            if (err != UC_ERR_OK && err != UC_ERR_MAP) {
                fail(("VirtualAlloc 指定地址 " + hex(addr) + " 映射失败").c_str(), err);
                return 0;
            }
        }
        return addr;
    }
    if ((type & kMemReserve) && !(type & kMemCommit)) return reserve_alloc(size);
    return arena_alloc(size);
}

uint64_t Stubs::arena_alloc(uint64_t size) {
    const uint64_t addr = arena_next_;
    const uint64_t len = align_up(size, kPage);
    const uc_err err = uc_mem_map(uc_, addr, len, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fail(("VirtualAlloc 提交 " + hex(addr) + "+" + hex(len) + " 映射失败").c_str(), err);
        return 0;
    }
    arena_next_ += len;
    return addr;
}

uint64_t Stubs::reserve_alloc(uint64_t size) {
    /* 只留地址空间：不 uc_mem_map，之后那块里的 COMMIT 才真映射（见上面的 addr != 0 分支）。 */
    const uint64_t addr = reserve_next_;
    reserve_next_ = addr + align_up(size, kPage);
    return addr;
}

}  // namespace tc
