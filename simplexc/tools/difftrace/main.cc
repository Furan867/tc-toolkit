/* difftrace —— 把一份产物在 Unicorn 里跑到「一次 run 完成」，打诊断数字（设计 §7 的 S0/S1）。
 *
 * 用法见 `--help`。判据都来自驱动（driver.hh）：执行指令数、store 条数、`sim_cycle`、停止原因；
 * 另外两样是这次要核的现场 —— `#SIMULATION_STATE + 2663` 的写次数与值、`import` 为空的
 * `dll_refs` 站点有没有被踩到（那 13 处身份未查清，踩到就会落在未映射地址上）。
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unicorn/unicorn.h>

#include "diff.hh"
#include "disasm.hh"
#include "driver.hh"
#include "image.hh"
#include "spec.hh"

namespace {

constexpr const char* kState2663Offset = "2663";  // 相对 `#SIMULATION_STATE` 的诊断偏移

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "用法: %s --src <si> --bin <bin> --entry <off> [--target <cycles>] [--seed <p>]"
                 " [--save <seq>]\n"
                 "      %s --compare <a.seq> <b.seq>\n"
                 "      %s --compare-state <a.seq> <b.seq> --sim-cycle-addr <addr> --src <si>\n"
                 "      %s --where <addr> --seq <seq> [--src <si> --bin <bin> --entry <off>]\n"
                 "  --src     .si（拿协议区地址与数组声明）\n"
                 "  --bin     blob（8 字节头 + 机器码）\n"
                 "  --entry   entry 偏移，相对机器码起点；写成 0x… 或十进制\n"
                 "  --target  sim_target_cycle，默认 100\n"
                 "  --seed    `#RESET_DATA_*` 灌什么：seq（默认）/ xorshift / none（全零）\n"
                 "  --save    跑完后把 store 序列落盘（`tc::save` 的格式）\n"
                 "  --compare 不跑模拟，只比两份落盘序列；给了它就不能再给上面那些\n"
                 "  --compare-state\n"
                 "            不跑模拟，按内存状态比（逐条比只对同一编译器的两版成立）；"
                 "判据是最终状态\n"
                 "  --sim-cycle-addr\n"
                 "            每 tick 写一次的地址，切逐 tick 线索靠它当界标\n"
                 "  --where   不跑模拟：列出落盘序列里写 <addr>（或它所在的 8 字节窗口）的每一条写；\n"
                 "            连带 --src / --bin / --entry 就把每条写的 pc 反汇编出来（缺一个就只列写）\n"
                 "  --seq     `--where` 要读的落盘序列\n",
                 argv0, argv0, argv0, argv0);
}

tc::Seed parse_seed(const std::string& s) {
    if (s == "none") return tc::Seed::kNone;
    if (s == "seq") return tc::Seed::kSeq;
    if (s == "xorshift") return tc::Seed::kXorshift;
    throw std::runtime_error("--seed 只有 none / seq / xorshift，给了: " + s);
}

const char* seed_name(tc::Seed s) {
    switch (s) {
        case tc::Seed::kNone:
            return "none";
        case tc::Seed::kSeq:
            return "seq";
        case tc::Seed::kXorshift:
            return "xorshift";
    }
    return "?";
}

uint64_t parse_u64(const char* s, const char* what) {
    std::size_t used = 0;
    const std::string text(s);
    const uint64_t v = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::runtime_error(std::string(what) + " 不是整数: " + text);
    return v;
}

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

void print_store(const char* tag, size_t index, const tc::Store& s) {
    std::printf("  %s[%zu] pc=%s addr=%s size=%u value=%s\n", tag, index, hex(s.pc).c_str(),
                hex(s.addr).c_str(), s.size, hex(s.value).c_str());
}

// 「哪些地址在分歧」比「哪一条先分叉」更早能看出原因：分叉点常常只是一个地址上的值先裂开。
void print_addr_histogram(const char* tag, const std::vector<tc::Store>& stores) {
    std::printf("histogram_addr top 5（%s）：\n", tag);
    for (const auto& [addr, n] : tc::histogram_addr(stores, 5))
        std::printf("  %s  %zu\n", hex(addr).c_str(), n);
}

// `--where` 的窗口宽度。判据是「覆盖」，不是「相等」：一次写可能比地址宽（8 字节写盖住窗口里的
// 一个字），也可能比地址窄（1 字节写落在窗口里的第二个字节上）；只比 `s.addr == addr` 会把
// 同一个块里除首条之外的所有写全漏掉，而那正是要看的东西。
constexpr uint64_t kWhereWindow = 8;

// 一个 pc 的反汇编块（`before` / `after` 只是让上手能看清前后文）。
void print_pc_disasm(const tc::Disasm& disasm, const Image& image, uint64_t pc) {
    const uint64_t off = pc - image.code_base;
    if (pc < image.code_base || off >= image.code.size()) {
        std::printf("    （pc 不在 code 内，跳过反汇编）\n");
        return;
    }
    const std::string text = disasm.range(image.code, image.code_base, off, 1, 2, 1);
    std::printf("%s", text.c_str());
}

// 从落盘序列里筛出写某个地址（或它所在的 8 字节窗口）的全部 store，按序列顺序打印；
// 给了 `--bin` 就把每个不同的 pc 反汇编一次。
int where_writes(uint64_t addr, const std::string& seq_path, const std::string& bin,
                 const std::string& src, uint64_t entry) {
    const std::vector<tc::Store> seq = tc::load(seq_path);
    const uint64_t window_lo = addr - addr % kWhereWindow;
    const uint64_t window_hi = window_lo + kWhereWindow;

    std::vector<std::pair<size_t, tc::Store>> hits;
    for (size_t i = 0; i < seq.size(); ++i) {
        const tc::Store& s = seq[i];
        if (s.addr < window_hi && window_lo < s.addr + s.size) hits.emplace_back(i, s);
    }

    std::printf("序列             : %s（%zu 条写）\n", seq_path.c_str(), seq.size());
    std::printf("窗口             : [%s, %s)（`--where %s` 所在的 %llu 字节窗口）\n",
                hex(window_lo).c_str(), hex(window_hi).c_str(), hex(addr).c_str(),
                static_cast<unsigned long long>(kWhereWindow));
    std::printf("命中             : %zu 条\n", hits.size());
    for (const auto& [index, s] : hits) print_store("", index, s);

    if (bin.empty()) {
        std::printf("\n反汇编           : 没给 --src / --bin / --entry，只列写\n");
        return hits.empty() ? 1 : 0;
    }

    const Image image = Image::load(bin, Spec::parse(src), entry);
    const tc::Disasm disasm;
    std::printf("\n反汇编（每条不同的 pc，before=2 after=1）：\n");
    std::vector<uint64_t> seen;
    for (const auto& [index, s] : hits) {
        if (std::find(seen.begin(), seen.end(), s.pc) != seen.end()) continue;
        seen.push_back(s.pc);
        std::printf("  pc=%s（code+0x%llx）\n", hex(s.pc).c_str(),
                    static_cast<unsigned long long>(s.pc - image.code_base));
        print_pc_disasm(disasm, image, s.pc);
    }
    return hits.empty() ? 1 : 0;
}

int compare_sequences(const std::string& path_a, const std::string& path_b) {
    const std::vector<tc::Store> a = tc::load(path_a);
    const std::vector<tc::Store> b = tc::load(path_b);
    const std::optional<tc::Divergence> div = tc::first_divergence(a, b);

    std::printf("A                : %s\n", path_a.c_str());
    std::printf("B                : %s\n", path_b.c_str());
    std::printf("条数             : A %zu / B %zu\n", a.size(), b.size());
    if (!div) {
        std::printf("比对             : equal\n");
    } else if (div->length_only) {
        std::printf("比对             : 只是长度差（index %zu = 短的一边条数，短的是长的前缀）\n",
                    div->index);
    } else {
        std::printf("比对             : 分叉 @ index %zu\n", div->index);
        print_store("A", div->index, div->a);
        print_store("B", div->index, div->b);
    }

    std::printf("\n");
    print_addr_histogram("A", a);
    print_addr_histogram("B", b);
    return div ? 1 : 0;
}

// 只留落在 `spec.regions()` 里的写。被排除的两类固定下来：
//   - 栈区（0x4000_0000 起那 8 MB，与 driver.cc 的 kStackBase/kStackSize 同）：两边栈帧布局
//     本来就不会一样；
//   - 我们代码区里被改写成 magic 的导入槽：它们本来就应该不同，永远比不等。
// 两类都不在 regions 里，所以一条规则就够了。
constexpr size_t kMaxMismatchPrint = 20;

std::vector<tc::Store> keep_regions(const std::vector<tc::Store>& seq,
                                   const std::vector<std::pair<uint64_t, uint64_t>>& regions,
                                   size_t& dropped) {
    std::vector<tc::Store> out;
    out.reserve(seq.size());
    for (const tc::Store& s : seq) {
        bool inside = false;
        for (const auto& [lo, hi] : regions) {
            if (s.addr >= lo && s.addr < hi) {
                inside = true;
                break;
            }
        }
        if (inside) out.push_back(s);
        else ++dropped;
    }
    return out;
}

std::string byte_text(const std::optional<uint8_t>& v) {
    if (!v) return "-(没写)";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02x", static_cast<unsigned>(*v));
    return buf;
}

void print_byte_diffs(const std::vector<tc::ByteDiff>& rows, size_t count) {
    const size_t shown = std::min(rows.size(), kMaxMismatchPrint);
    std::printf("  差异字节数     : %zu（列前 %zu 条）\n", count, shown);
    for (size_t i = 0; i < shown; ++i) {
        const auto& [addr, vals] = rows[i];
        std::printf("    addr=%s A=%s B=%s\n", hex(addr).c_str(), byte_text(vals.first).c_str(),
                    byte_text(vals.second).c_str());
    }
}

int compare_states(const std::string& path_a, const std::string& path_b, uint64_t tick_addr,
                   const Spec& spec) {
    const std::vector<std::pair<uint64_t, uint64_t>> regions = spec.regions();
    size_t dropped_a = 0;
    size_t dropped_b = 0;
    const std::vector<tc::Store> raw_a = tc::load(path_a);
    const std::vector<tc::Store> raw_b = tc::load(path_b);
    const std::vector<tc::Store> a = keep_regions(raw_a, regions, dropped_a);
    const std::vector<tc::Store> b = keep_regions(raw_b, regions, dropped_b);

    // 判据：折完整序列的终态。不靠界标，所以界标选错也不会把结论带偏，所以它是第一行。
    const tc::Memory final_a = tc::fold(a, 0, a.size());
    const tc::Memory final_b = tc::fold(b, 0, b.size());
    std::vector<tc::ByteDiff> final_diff;
    const size_t final_count = tc::memory_diff(final_a, final_b, kMaxMismatchPrint, final_diff);
    std::printf("最终状态         : %s\n", final_count == 0 ? "equal（判据通过）" : "不等（判据不过）");
    if (final_count != 0) print_byte_diffs(final_diff, final_count);

    std::printf("A                : %s（%zu 条写，留下 %zu）\n", path_a.c_str(), raw_a.size(),
                a.size());
    std::printf("B                : %s（%zu 条写，留下 %zu）\n", path_b.c_str(), raw_b.size(),
                b.size());
    std::printf("排除项           : spec.regions() 之外的写不比（栈区 / 我们代码区的导入槽）；"
                "丢掉 A %zu / B %zu 条\n",
                dropped_a, dropped_b);
    std::printf("tick 界标        : %s（`--sim-cycle-addr`：每 tick 写一次的地址，只用来切逐 tick 线索）\n",
                hex(tick_addr).c_str());

    // 以下只是定位线索：第一个分叉的 tick 比「终态不等」更能指出从哪开始。
    const std::vector<tc::TickSpan> ticks_a = tc::split_ticks(a, tick_addr);
    const std::vector<tc::TickSpan> ticks_b = tc::split_ticks(b, tick_addr);
    std::printf("逐 tick 线索     : tick 数 A %zu / B %zu\n", ticks_a.size(), ticks_b.size());
    const std::optional<tc::StateDiff> diff = tc::first_state_diff(a, b, tick_addr, kMaxMismatchPrint);
    if (!diff) {
        std::printf("                    每个 tick 折叠后都是 0 差异\n");
    } else if (diff->tick_count) {
        std::printf("                    第一个不同的 tick %zu：两边 tick 数不同（%zu vs %zu），"
                    "之后无从逐 tick 比\n",
                    diff->tick, diff->tick_count->first, diff->tick_count->second);
    } else {
        std::printf("                    第一个不同的 tick %zu（0 起）\n", diff->tick);
        print_byte_diffs(diff->mismatches, diff->mismatch_count);
    }
    return final_count == 0 ? 0 : 1;
}

int run(int argc, char** argv) {
    std::string src, bin;
    uint64_t entry = 0;
    uint64_t target = 100;
    tc::Seed seed = tc::Seed::kSeq;  // 默认灌 seq：全零等于「reset_sim 的拷贝等于没拷」
    bool have_entry = false;
    std::string save_path;
    std::optional<std::pair<std::string, std::string>> compare;
    std::optional<std::pair<std::string, std::string>> compare_state;
    std::optional<uint64_t> sim_cycle_addr;
    std::optional<uint64_t> where_addr;
    std::string seq_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else if (a == "--src" && more) {
            src = argv[++i];
        } else if (a == "--bin" && more) {
            bin = argv[++i];
        } else if (a == "--entry" && more) {
            entry = parse_u64(argv[++i], "--entry");
            have_entry = true;
        } else if (a == "--target" && more) {
            target = parse_u64(argv[++i], "--target");
        } else if (a == "--seed" && more) {
            seed = parse_seed(argv[++i]);
        } else if (a == "--save" && more) {
            save_path = argv[++i];
        } else if (a == "--compare") {
            if (i + 2 >= argc) throw std::runtime_error("--compare 要两个路径：<a.seq> <b.seq>");
            compare.emplace(argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (a == "--compare-state") {
            if (i + 2 >= argc)
                throw std::runtime_error("--compare-state 要两个路径：<a.seq> <b.seq>");
            compare_state.emplace(argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (a == "--sim-cycle-addr" && more) {
            sim_cycle_addr = parse_u64(argv[++i], "--sim-cycle-addr");
        } else if (a == "--where" && more) {
            where_addr = parse_u64(argv[++i], "--where");
        } else if (a == "--seq" && more) {
            seq_path = argv[++i];
        } else {
            usage(argv[0]);
            throw std::runtime_error("认不出的参数: " + a);
        }
    }
    if (compare && compare_state) {
        usage(argv[0]);
        throw std::runtime_error("--compare 与 --compare-state 只能给一个");
    }
    if (!seq_path.empty() && !where_addr) {
        usage(argv[0]);
        throw std::runtime_error("--seq 只跟 --where 一起用");
    }
    if (where_addr) {
        if (compare || compare_state) {
            usage(argv[0]);
            throw std::runtime_error("--where 不与 --compare / --compare-state 同时给");
        }
        if (seq_path.empty()) {
            usage(argv[0]);
            throw std::runtime_error("--where 要 --seq：查的是落盘序列里的写");
        }
        if (!save_path.empty()) {
            usage(argv[0]);
            throw std::runtime_error("--where 不跑模拟，不能与 --save 同时给");
        }
        // 反汇编要机器码，所以要整组；只给一半会让「为什么没反汇编」变成猜的。
        const size_t code_args = (src.empty() ? 0 : 1) + (bin.empty() ? 0 : 1) + (have_entry ? 1 : 0);
        if (code_args != 0 && code_args != 3) {
            usage(argv[0]);
            throw std::runtime_error("--where 的反汇编要么不给 --src / --bin / --entry，要么三个都给");
        }
        return where_writes(*where_addr, seq_path, bin, src, entry);
    }
    if (compare_state) {
        if (!bin.empty() || have_entry || !save_path.empty()) {
            usage(argv[0]);
            throw std::runtime_error("--compare-state 不跑模拟，不能与 --bin / --entry / --save 同时给");
        }
        if (!sim_cycle_addr) {
            usage(argv[0]);
            throw std::runtime_error("--compare-state 要 --sim-cycle-addr（每 tick 写一次的地址）");
        }
        // 比的就是 `spec.regions()` 覆盖的状态，所以 `--src` 必需。
        if (src.empty()) {
            usage(argv[0]);
            throw std::runtime_error("--compare-state 要 --src：比哪些地址由 spec.regions() 定");
        }
        return compare_states(compare_state->first, compare_state->second, *sim_cycle_addr,
                             Spec::parse(src));
    }
    if (sim_cycle_addr) {
        usage(argv[0]);
        throw std::runtime_error("--sim-cycle-addr 只跟 --compare-state 一起用");
    }
    if (compare) {
        if (!src.empty() || !bin.empty() || have_entry || !save_path.empty()) {
            usage(argv[0]);
            throw std::runtime_error("--compare 不跑模拟，不能与 --src / --bin / --entry / --save 同时给");
        }
        return compare_sequences(compare->first, compare->second);
    }
    if (src.empty() || bin.empty() || !have_entry) {
        usage(argv[0]);
        throw std::runtime_error("--src / --bin / --entry 都得给");
    }

    const Spec spec = Spec::parse(src);
    const Image image = Image::load(bin, spec, entry);

    uc_engine* uc = nullptr;
    const uc_err err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (err != UC_ERR_OK) throw std::runtime_error(std::string("uc_open 失败: ") + uc_strerror(err));

    tc::RunOutcome out;
    try {
        out = tc::run_to_cycle(uc, spec, image, target, seed);
    } catch (...) {
        uc_close(uc);
        throw;
    }
    uc_close(uc);

    if (!save_path.empty()) {
        tc::save(save_path, out.stores);
        std::printf("序列已落盘       : %s（%zu 条）\n", save_path.c_str(), out.stores.size());
    }

    const uint64_t state = *spec.addr("SIMULATION_STATE");
    const uint64_t watched = state + std::stoull(kState2663Offset);
    std::vector<uint64_t> watched_values;
    for (const tc::Store& s : out.stores)
        if (s.addr == watched) watched_values.push_back(s.value);

    std::printf("段 / 字节        : %zu 段、%llu B\n", out.layout.regions,
                static_cast<unsigned long long>(out.layout.mapped_bytes));
    std::printf("DLL 镜像         : base %s、%zu B、导入槽 %zu\n", hex(image.dll.base).c_str(),
                image.dll.image.size(), image.dll.iat.size());
    std::printf("RESET_DATA 图案  : %s（%zu 块、%llu B）\n", seed_name(seed), out.seeded_blocks,
                static_cast<unsigned long long>(out.seeded_bytes));
    std::printf("dll_refs 站点    : 改写 %zu、原样 %zu（共 %zu）\n", out.refs.patched,
                out.refs.untouched, image.dll_refs.size());
    size_t stub_code = 0;
    for (const StubRef& ref : image.stub_refs)
        if (ref.kind == StubKind::Code) ++stub_code;
    std::printf("stub_refs 站点   : 改写 %zu（code 桩 %zu / slot %zu）\n", out.refs.stubs,
                stub_code, image.stub_refs.size() - stub_code);
    std::printf("执行指令数       : %llu（启动 %llu + run %llu）\n",
                static_cast<unsigned long long>(out.result.instrs),
                static_cast<unsigned long long>(out.instrs_boot),
                static_cast<unsigned long long>(out.result.instrs - out.instrs_boot));
    std::printf("store 条数       : %llu\n", static_cast<unsigned long long>(out.result.stores));
    std::printf("settings 地址    : %s（StateIndex[sim_running] = %llu → 监视 %s）\n",
                hex(out.settings_addr).c_str(),
                static_cast<unsigned long long>(out.sim_running_index),
                hex(out.sim_running_addr).c_str());
    std::printf("sim_running 写   : %zu 次（只在发命令之后监视）", out.running_writes.size());
    if (!out.running_writes.empty()) {
        std::printf("，值：");
        for (const tc::RunningWrite& w : out.running_writes)
            std::printf("%s%s(size=%u @pc=%s)", &w == &out.running_writes.front() ? "" : " ",
                        hex(w.value).c_str(), w.size, hex(w.pc).c_str());
    }
    std::printf("\n");
    std::printf("sim_cycle        : %llu\n", static_cast<unsigned long long>(out.result.sim_cycle));
    std::printf("停止原因         : %s\n", out.result.reason.c_str());
    std::printf("error_buffer len : %u\n", static_cast<unsigned>(out.result.error_len));
    std::printf("停下的 pc        : %s\n", hex(out.stop_pc).c_str());

    std::printf("\n前 20 条 store（pc / addr / size / value）：\n");
    const size_t n = out.stores.size() < 20 ? out.stores.size() : 20;
    for (size_t i = 0; i < n; ++i) {
        const tc::Store& s = out.stores[i];
        std::printf("  [%2zu] pc=%s addr=%s size=%u value=%s\n", i, hex(s.pc).c_str(),
                    hex(s.addr).c_str(), s.size, hex(s.value).c_str());
    }

    std::printf("\n#SIMULATION_STATE + %s 被写 %zu 次", kState2663Offset, watched_values.size());
    if (!watched_values.empty()) {
        std::printf("，值：");
        for (size_t i = 0; i < watched_values.size(); ++i)
            std::printf("%s%s", i ? " " : "", hex(watched_values[i]).c_str());
    }
    std::printf("\n");

    std::printf("import 为空的站点 : 踩到 %llu 次", static_cast<unsigned long long>(out.unnamed_hits));
    if (out.unnamed_first_pc != 0) std::printf("，首次 pc=%s", hex(out.unnamed_first_pc).c_str());
    std::printf("\n");
    // 正常收工只有这两种停法（`running_zero` = 精确、`sampled` = 兜底）。
    const std::string& reason = out.result.reason;
    return reason == "running_zero" || reason == "sampled" ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "difftrace: %s\n", e.what());
        return 2;
    }
}
