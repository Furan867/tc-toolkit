// `diff.*` 里判据核心的边界自测：按字节折叠、界标切段、逐 tick 状态比对。
//
// 为什么单独一个 main：判「两版等价」靠的就是这几个函数，而它们此前只有序列级的粗测。用例里每条
// 期望值都写死；对不上就是实现的问题，别回来改期望值 —— 那些值就是「等价」的定义。
//
// 用法：diff_selftest        退出码 0 = 全部 PASS；1 = 有用例 FAIL。
#include "diff.hh"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using tc::ByteDiff;
using tc::Memory;
using tc::StateDiff;
using tc::Store;
using tc::TickSpan;

// 界标地址：与用例里的数据地址错开，免得数据被误当界标。
constexpr uint64_t kTick = 0x1000;

int g_total = 0;
int g_failed = 0;

Store make_store(uint64_t addr, uint32_t size, uint64_t value) {
    Store s;
    s.pc = 0x400000;
    s.addr = addr;
    s.size = size;
    s.value = value;
    return s;
}

std::string hex_u64(uint64_t v) {
    std::ostringstream out;
    out << "0x" << std::hex << v << std::dec;
    return out.str();
}

std::string mem_str(const Memory& mem) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const std::pair<const uint64_t, uint8_t>& kv : mem) {
        if (!first) out << ", ";
        first = false;
        out << hex_u64(kv.first) << "=" << hex_u64(kv.second);
    }
    out << "}";
    return out.str();
}

std::string spans_str(const std::vector<TickSpan>& spans) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < spans.size(); ++i) {
        if (i != 0) out << ", ";
        out << "[" << spans[i].begin << "," << spans[i].end << ")";
    }
    out << "]";
    return out.str();
}

// 每段末尾那条写的地址：用来证明「界标写属于它结束的那一段」。
std::string span_tails_str(const std::vector<Store>& seq, const std::vector<TickSpan>& spans) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < spans.size(); ++i) {
        if (i != 0) out << ", ";
        if (spans[i].begin < spans[i].end && spans[i].end <= seq.size()) {
            out << hex_u64(seq[spans[i].end - 1].addr);
        } else {
            out << "空段";
        }
    }
    out << "]";
    return out.str();
}

std::string diffs_str(const std::vector<ByteDiff>& diffs) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < diffs.size(); ++i) {
        if (i != 0) out << ", ";
        out << hex_u64(diffs[i].first) << ": ";
        out << (diffs[i].second.first.has_value() ? hex_u64(*diffs[i].second.first)
                                                  : std::string("缺"));
        out << " vs ";
        out << (diffs[i].second.second.has_value() ? hex_u64(*diffs[i].second.second)
                                                   : std::string("缺"));
    }
    out << "]";
    return out.str();
}

std::string state_str(const std::optional<StateDiff>& diff) {
    if (!diff.has_value()) return "nullopt";
    std::ostringstream out;
    out << "{tick=" << diff->tick << ", mismatch_count=" << diff->mismatch_count;
    if (diff->tick_count.has_value()) {
        out << ", tick_count=(" << diff->tick_count->first << "," << diff->tick_count->second << ")";
    }
    out << ", mismatches=" << diffs_str(diff->mismatches) << "}";
    return out.str();
}

bool spans_equal(const std::vector<TickSpan>& got, const std::vector<TickSpan>& want) {
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].begin != want[i].begin || got[i].end != want[i].end) return false;
    }
    return true;
}

class Case {
  public:
    explicit Case(std::string name) : name_(std::move(name)) {}

    // 期望值与实际值都带上：FAIL 的明细要能直接下结论，不必回头加打印。
    void expect(bool ok, const std::string& what, const std::string& expected,
                const std::string& actual) {
        if (ok) return;
        ++failures_;
        std::cout << "     - " << what << "\n       期望: " << expected << "\n       实际: " << actual
                  << "\n";
    }

    void finish() {
        ++g_total;
        if (failures_ == 0) {
            std::cout << "PASS  " << name_ << "\n";
            return;
        }
        ++g_failed;
        std::cout << "FAIL  " << name_ << "\n";
    }

  private:
    std::string name_;
    int failures_ = 0;
};

// 1. 同地址写两次不同值 → 最后一次获胜。
void case01_fold_last_write_wins(Case& c) {
    const std::vector<Store> seq = {make_store(0x100, 1, 0x11), make_store(0x100, 1, 0x22)};
    const Memory got = tc::fold(seq, 0, seq.size());
    const Memory want = {{0x100, 0x22}};
    c.expect(got == want, "同地址写两次不同值，最后一次获胜", mem_str(want), mem_str(got));
}

// 2. `size=4` 写 0 与 `size=1` 写 0 → 字节层面等价（键数不同，缺的当 0）。
void case02_fold_byte_level_equivalence(Case& c) {
    const std::vector<Store> wide = {make_store(0x200, 4, 0)};
    const std::vector<Store> narrow = {make_store(0x200, 1, 0)};
    const Memory mw = tc::fold(wide, 0, wide.size());
    const Memory mn = tc::fold(narrow, 0, narrow.size());
    c.expect(mw.size() == 4, "size=4 展开成 4 个字节键", "4 个键",
             std::to_string(mw.size()) + " 个键 " + mem_str(mw));
    c.expect(mn.size() == 1, "size=1 只有 1 个字节键", "1 个键",
             std::to_string(mn.size()) + " 个键 " + mem_str(mn));
    std::vector<ByteDiff> diffs;
    const size_t count = tc::memory_diff(mw, mn, 0, diffs);
    c.expect(count == 0 && diffs.empty(), "缺键按 0 算 → 两者字节层面等价", "0 条差异 []",
             std::to_string(count) + " 条差异 " + diffs_str(diffs));
}

// 3. 小端展开。
void case03_fold_little_endian(Case& c) {
    const std::vector<Store> seq = {make_store(0x300, 4, 0x11223344)};
    const Memory got = tc::fold(seq, 0, seq.size());
    const Memory want = {{0x300, 0x44}, {0x301, 0x33}, {0x302, 0x22}, {0x303, 0x11}};
    c.expect(got == want, "size=4 按小端展开", mem_str(want), mem_str(got));
}

// 4. `size=8` 展开满 8 字节；高位 0 的字节也要出现在结果里。
void case04_fold_size8_high_zero_bytes(Case& c) {
    const std::vector<Store> seq = {make_store(0x400, 8, 0x11)};
    const Memory got = tc::fold(seq, 0, seq.size());
    const Memory want = {{0x400, 0x11}, {0x401, 0}, {0x402, 0}, {0x403, 0},
                         {0x404, 0},    {0x405, 0}, {0x406, 0}, {0x407, 0}};
    c.expect(got == want, "size=8 展开 8 字节，高位 0 也在结果里", mem_str(want), mem_str(got));

    // 高位的 0 是**真写**：先写 1 字节再被 8 字节写盖掉，那个字节必须回到 0。
    const std::vector<Store> overwrite = {make_store(0x403, 1, 0x77), make_store(0x400, 8, 0x11)};
    const Memory o = tc::fold(overwrite, 0, overwrite.size());
    c.expect(o == want, "高位 0 覆盖既有值", mem_str(want), mem_str(o));
}

// 5. 空序列 → 空 map。
void case05_fold_empty(Case& c) {
    const std::vector<Store> empty;
    const Memory e = tc::fold(empty, 0, empty.size());
    c.expect(e.empty(), "空序列 → 空 map", "{}", mem_str(e));

    const std::vector<Store> seq = {make_store(0x500, 4, 0x1)};
    const Memory r = tc::fold(seq, 1, 1);
    c.expect(r.empty(), "空区间 [1,1) → 空 map", "{}", mem_str(r));
}

// 6. 界标出现 k 次 → 恰好 k 段，且每段以它那条界标写收尾。
void case06_split_landmark_count(Case& c) {
    const std::vector<Store> seq = {make_store(0x10, 1, 1),  make_store(kTick, 1, 1),
                                    make_store(0x11, 1, 1),  make_store(kTick, 1, 1),
                                    make_store(0x12, 1, 1),  make_store(kTick, 1, 1)};
    const std::vector<TickSpan> got = tc::split_ticks(seq, kTick);
    const std::vector<TickSpan> want = {{0, 2}, {2, 4}, {4, 6}};
    c.expect(got.size() == 3, "界标出现 3 次 → 3 段", "3 段",
             std::to_string(got.size()) + " 段 " + spans_str(got));

    bool every_span_ends_on_landmark = true;
    for (const TickSpan& s : got) {
        const bool ok = s.begin < s.end && s.end <= seq.size() && seq[s.end - 1].addr == kTick;
        every_span_ends_on_landmark = every_span_ends_on_landmark && ok;
    }
    c.expect(every_span_ends_on_landmark, "每段的最后一条写就是它的界标写", "[0x1000, 0x1000, 0x1000]",
             span_tails_str(seq, got));
    c.expect(spans_equal(got, want), "段边界：首段含界标之前的写、段间无缝隙", spans_str(want),
             spans_str(got));
}

// 7. 界标一次都没出现 → 1 段（整段），不是 0 段。
void case07_split_no_landmark(Case& c) {
    const std::vector<Store> seq = {make_store(0x10, 1, 1), make_store(0x11, 1, 2),
                                    make_store(0x12, 1, 3), make_store(0x13, 1, 4)};
    const std::vector<TickSpan> got = tc::split_ticks(seq, kTick);
    const std::vector<TickSpan> want = {{0, 4}};
    c.expect(spans_equal(got, want), "界标一次没出现 → 整条序列 1 段", spans_str(want), spans_str(got));

    const std::vector<Store> empty;
    const std::vector<TickSpan> none = tc::split_ticks(empty, kTick);
    c.expect(none.empty(), "空序列 → 0 段（「0 段 = 空」与「1 段 = 整条」不是一回事）", "[]",
             spans_str(none));
}

// 8. 界标写开头 / 结尾 / 连续 / 尾巴：都不能产生空段。
void case08_split_edges(Case& c) {
    // 以界标写开头：第一段只含这条界标写，不能因为「它之前没东西」而少一段或空一段。
    const std::vector<Store> head = {make_store(kTick, 1, 1), make_store(0x20, 1, 2),
                                     make_store(0x21, 1, 3)};
    const std::vector<TickSpan> hgot = tc::split_ticks(head, kTick);
    const std::vector<TickSpan> hwant = {{0, 1}, {1, 3}};
    c.expect(spans_equal(hgot, hwant), "界标写开头", spans_str(hwant), spans_str(hgot));

    // 以界标写结尾：最后一段就结束在它身上，不额外产生尾巴段。
    const std::vector<Store> tail = {make_store(0x20, 1, 2), make_store(0x21, 1, 3),
                                     make_store(kTick, 1, 1)};
    const std::vector<TickSpan> tgot = tc::split_ticks(tail, kTick);
    const std::vector<TickSpan> twant = {{0, 3}};
    c.expect(spans_equal(tgot, twant), "界标写结尾", spans_str(twant), spans_str(tgot));

    // 连续界标 + 末个界标之后的写：后者自成最后一段（否则 run 结束时的状态被扔掉）。
    const std::vector<Store> dense = {make_store(kTick, 1, 1), make_store(kTick, 1, 2),
                                      make_store(0x30, 1, 3)};
    const std::vector<TickSpan> dgot = tc::split_ticks(dense, kTick);
    const std::vector<TickSpan> dwant = {{0, 1}, {1, 2}, {2, 3}};
    c.expect(spans_equal(dgot, dwant), "连续界标 + 尾巴", spans_str(dwant), spans_str(dgot));

    bool no_empty_span = true;
    for (const TickSpan& s : dgot) no_empty_span = no_empty_span && s.begin < s.end;
    c.expect(no_empty_span, "不产生空段", "每段 begin < end", spans_str(dgot));
}

// 9. 两边 tick 数不同 → tick = min(个数)，并标出长度差异。
void case09_state_tick_count(Case& c) {
    const std::vector<Store> a = {make_store(0x10, 1, 1), make_store(kTick, 1, 1),
                                  make_store(0x11, 1, 2), make_store(kTick, 1, 2)};
    const std::vector<Store> b = {make_store(0x10, 1, 1), make_store(kTick, 1, 1),
                                  make_store(0x11, 1, 2), make_store(kTick, 1, 2),
                                  make_store(0x12, 1, 3), make_store(kTick, 1, 3)};
    const std::optional<StateDiff> d = tc::first_state_diff(a, b, kTick);
    c.expect(d.has_value(), "tick 数不同（2 vs 3）→ 判为有差异", "有差异", state_str(d));
    if (d.has_value()) {
        c.expect(d->tick == 2, "tick = min(A 的 tick 数, B 的 tick 数)", "2", std::to_string(d->tick));
        const bool marked = d->tick_count.has_value() && d->tick_count->first == 2 &&
                            d->tick_count->second == 3;
        const std::string got_mark = d->tick_count.has_value()
                                         ? "(" + std::to_string(d->tick_count->first) + "," +
                                               std::to_string(d->tick_count->second) + ")"
                                         : std::string("未标记");
        c.expect(marked, "tick_count 标出长度差异 (A 的, B 的)", "(2,3)", got_mark);
        c.expect(d->mismatches.empty() && d->mismatch_count == 0, "长度差异不带逐字节明细", "0 条 []",
                 std::to_string(d->mismatch_count) + " 条 " + diffs_str(d->mismatches));
    }

    const std::vector<Store> empty;
    const std::vector<Store> one = {make_store(0x10, 1, 1)};
    const std::optional<StateDiff> de = tc::first_state_diff(empty, one, kTick);
    const bool marked_empty = de.has_value() && de->tick == 0 && de->tick_count.has_value() &&
                              de->tick_count->first == 0 && de->tick_count->second == 1;
    c.expect(marked_empty, "空序列（0 tick）vs 1 tick → tick=0、tick_count=(0,1)", "tick=0, (0,1)",
             state_str(de));
}

// 10. A 在某个 tick 多写一个字节 → 报那个 tick，且能区分「A 有、B 无」。
void case10_state_extra_byte(Case& c) {
    const std::vector<Store> a = {make_store(0x10, 1, 0x11), make_store(kTick, 1, 1),
                                  make_store(0x20, 1, 0xaa), make_store(kTick, 1, 2)};
    const std::vector<Store> b = {make_store(0x10, 1, 0x11), make_store(kTick, 1, 1),
                                  make_store(kTick, 1, 2)};
    const std::optional<StateDiff> d = tc::first_state_diff(a, b, kTick);
    c.expect(d.has_value() && d->tick == 1, "A 在第 1 个 tick 多写一个字节 → 报 tick 1", "tick=1",
             state_str(d));
    if (d.has_value()) {
        c.expect(d->mismatch_count == 1 && d->mismatches.size() == 1, "恰好 1 条差异", "1 条",
                 std::to_string(d->mismatch_count) + " 条 / 明细 " +
                     std::to_string(d->mismatches.size()) + " 条");
        if (d->mismatches.size() == 1) {
            const ByteDiff& x = d->mismatches[0];
            c.expect(x.first == 0x20, "差异地址是被多写的那个字节", hex_u64(0x20), hex_u64(x.first));
            const bool one_sided = x.second.first.has_value() && *x.second.first == 0xaa &&
                                   !x.second.second.has_value();
            c.expect(one_sided, "A 那侧有值 0xaa、B 那侧没写过（nullopt）", "{0x20: 0xaa vs 缺}",
                     diffs_str(d->mismatches));
        }
    }
}

// 11. 某个 tick 里两边值不同 → 报那个 tick，两边值都对。
void case11_state_value_diff(Case& c) {
    const std::vector<Store> a = {make_store(0x10, 1, 0x11), make_store(kTick, 1, 1),
                                  make_store(0x20, 1, 0xaa), make_store(kTick, 1, 2)};
    const std::vector<Store> b = {make_store(0x10, 1, 0x11), make_store(kTick, 1, 1),
                                  make_store(0x20, 1, 0xbb), make_store(kTick, 1, 2)};
    const std::optional<StateDiff> d = tc::first_state_diff(a, b, kTick);
    c.expect(d.has_value() && d->tick == 1, "第 1 个 tick 里值不同 0xaa vs 0xbb → 报 tick 1", "tick=1",
             state_str(d));
    if (d.has_value()) {
        c.expect(d->mismatch_count == 1 && d->mismatches.size() == 1, "恰好 1 条差异", "1 条",
                 std::to_string(d->mismatch_count) + " 条 / 明细 " +
                     std::to_string(d->mismatches.size()) + " 条");
        if (d->mismatches.size() == 1) {
            const ByteDiff& x = d->mismatches[0];
            const bool both = x.second.first.has_value() && x.second.second.has_value() &&
                              *x.second.first == 0xaa && *x.second.second == 0xbb;
            c.expect(x.first == 0x20 && both, "地址 0x20、A=0xaa、B=0xbb", "{0x20: 0xaa vs 0xbb}",
                     diffs_str(d->mismatches));
        }
    }
}

// 12. 完全相同 → nullopt（写条数与写宽度不同也算相同）。
void case12_state_equal(Case& c) {
    const std::vector<Store> a = {make_store(0x10, 4, 0), make_store(kTick, 1, 1),
                                  make_store(0x20, 1, 0x55), make_store(kTick, 1, 2)};
    const std::vector<Store> b = {make_store(0x10, 1, 0), make_store(kTick, 1, 1),
                                  make_store(0x20, 1, 0x55), make_store(kTick, 1, 2)};
    const std::optional<StateDiff> d = tc::first_state_diff(a, b, kTick);
    c.expect(!d.has_value(), "逐 tick 状态等价（4 字节 0 vs 1 字节 0）→ nullopt", "nullopt",
             state_str(d));

    const std::vector<Store> same = {make_store(0x40, 8, 0x0102030405060708ull),
                                     make_store(kTick, 1, 1)};
    const std::optional<StateDiff> ds = tc::first_state_diff(same, same, kTick);
    c.expect(!ds.has_value(), "同一条序列自比 → nullopt", "nullopt", state_str(ds));
}

// 13. `max_mismatches` 生效：明细被截断，总数仍然报全。
void case13_state_max_mismatches(Case& c) {
    std::vector<Store> a = {make_store(kTick, 1, 1)};
    for (uint32_t i = 0; i < 10; ++i) a.push_back(make_store(0x100 + i, 1, 1 + i));
    a.push_back(make_store(kTick, 1, 2));
    const std::vector<Store> b = {make_store(kTick, 1, 1), make_store(kTick, 1, 2)};

    const std::optional<StateDiff> d = tc::first_state_diff(a, b, kTick, 3);
    c.expect(d.has_value() && d->tick == 1, "第 1 个 tick 有 10 条差异 → 报 tick 1", "tick=1",
             state_str(d));
    if (d.has_value()) {
        c.expect(d->mismatches.size() == 3, "max_mismatches=3 → 明细只留 3 条", "3 条",
                 std::to_string(d->mismatches.size()) + " 条");
        c.expect(d->mismatch_count == 10, "差异总数仍是 10", "10",
                 std::to_string(d->mismatch_count));
        bool order_ok = d->mismatches.size() == 3;
        for (size_t i = 0; order_ok && i < d->mismatches.size(); ++i) {
            order_ok = d->mismatches[i].first == 0x100 + i;
        }
        c.expect(order_ok, "留的是按地址升序的前 3 条", "[0x100, 0x101, 0x102]",
                 diffs_str(d->mismatches));
    }

    const std::optional<StateDiff> full = tc::first_state_diff(a, b, kTick, 0);
    const bool untruncated = full.has_value() && full->mismatches.size() == 10 &&
                             full->mismatch_count == 10;
    c.expect(untruncated, "max_mismatches=0 = 不截断", "10 条明细 / 总数 10",
             full.has_value() ? std::to_string(full->mismatches.size()) + " 条明细 / 总数 " +
                                    std::to_string(full->mismatch_count)
                              : std::string("nullopt"));
}

// 14. `memory_diff` 的缺失当 0：缺键不是差异；有键非 0 才是。
void case14_memory_diff_missing_is_zero(Case& c) {
    const Memory empty;

    std::vector<ByteDiff> out;
    const Memory b_zero = {{0x500, 0}};
    const size_t n_zero = tc::memory_diff(empty, b_zero, 0, out);
    c.expect(n_zero == 0 && out.empty(), "A 没有该键、B 有且为 0 → 不算差异", "0 条 []",
             std::to_string(n_zero) + " 条 " + diffs_str(out));

    out.clear();
    const Memory b_nonzero = {{0x500, 7}};
    const size_t n_nonzero = tc::memory_diff(empty, b_nonzero, 0, out);
    const bool one_sided = out.size() == 1 && out[0].first == 0x500 &&
                           !out[0].second.first.has_value() && out[0].second.second.has_value() &&
                           *out[0].second.second == 7;
    c.expect(n_nonzero == 1 && one_sided, "A 没有该键、B 有且非 0 → 算差异（A 侧 nullopt）",
             "1 条 {0x500: 缺 vs 0x7}",
             std::to_string(n_nonzero) + " 条 " + diffs_str(out));

    out.clear();
    const Memory a_zero = {{0x600, 0}};
    const size_t n_rev = tc::memory_diff(a_zero, empty, 0, out);
    c.expect(n_rev == 0 && out.empty(), "反向：A 有 0、B 没有 → 也不算差异", "0 条 []",
             std::to_string(n_rev) + " 条 " + diffs_str(out));
}

// 15. `size=16` 的写：`Store::value` 只有 8 字节，高半截没有真值来源 → 必须抛，不许静默截断。
// 静默截断的后果是判据对高 8 字节的差异视而不见、却照旧报 equal。
void case15_fold_rejects_size16(Case& c) {
    const std::vector<Store> seq = {make_store(0x1100, 16, 0x1122334455667788ull)};
    bool threw = false;
    std::string message;
    try {
        const Memory got = tc::fold(seq, 0, seq.size());
        (void)got;
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    c.expect(threw, "size=16 → fold 抛 std::runtime_error（不许只展开低 8 字节）", "抛异常",
             threw ? "抛了：" + message : std::string("没抛，静默折出了一份内存"));
    const std::string quoted = "消息：\"" + message + "\"";
    c.expect(message.find("size=16") != std::string::npos, "消息里带 size=16", "含 \"size=16\"",
             quoted);
    c.expect(message.find("addr=0x1100") != std::string::npos, "消息里带 addr",
             "含 \"addr=0x1100\"", quoted);
}

// 16. `size=0`：定下的契约是「也抛」，不是「不产生键」——后者是静默的，而 `load` 本来就不接受 0，
// 所以走到这里的 0 只可能是 Store 造错了。
void case16_fold_rejects_size0(Case& c) {
    const std::vector<Store> seq = {make_store(0x1200, 0, 0)};
    bool threw = false;
    std::string message;
    try {
        const Memory got = tc::fold(seq, 0, seq.size());
        (void)got;
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    c.expect(threw, "size=0 → fold 抛 std::runtime_error（不是静默不产生键）", "抛异常",
             threw ? "抛了：" + message : std::string("没抛，静默折出了一份内存"));
    c.expect(message.find("size=0") != std::string::npos, "消息里带 size=0", "含 \"size=0\"",
             "消息：\"" + message + "\"");

    // 邻近的合法宽度不能被这条规则误伤。
    const std::vector<Store> ok = {make_store(0x1210, 8, 0x1122334455667788ull)};
    const Memory got_ok = tc::fold(ok, 0, ok.size());
    c.expect(got_ok.size() == 8, "size=8 仍然正常展开 8 字节", "8 个键",
             std::to_string(got_ok.size()) + " 个键 " + mem_str(got_ok));
}

}  // namespace

int main() {
    std::cout << "diff_selftest: fold / split_ticks / first_state_diff / memory_diff 边界用例\n";

    const std::vector<std::pair<std::string, void (*)(Case&)>> cases = {
        {"1  fold 同地址写两次，最后一次获胜", case01_fold_last_write_wins},
        {"2  fold size=4/0 与 size=1/0 字节层面等价", case02_fold_byte_level_equivalence},
        {"3  fold 小端展开", case03_fold_little_endian},
        {"4  fold size=8 高位 0 的字节也在结果里", case04_fold_size8_high_zero_bytes},
        {"5  fold 空序列 / 空区间 → 空 map", case05_fold_empty},
        {"6  split_ticks 界标出现 k 次 → k 段", case06_split_landmark_count},
        {"7  split_ticks 无界标 → 1 段（空序列 0 段）", case07_split_no_landmark},
        {"8  split_ticks 界标在头/尾/连续 + 尾巴", case08_split_edges},
        {"9  first_state_diff tick 数不同", case09_state_tick_count},
        {"10 first_state_diff A 多写一个字节", case10_state_extra_byte},
        {"11 first_state_diff 同一 tick 值不同", case11_state_value_diff},
        {"12 first_state_diff 完全相同 → nullopt", case12_state_equal},
        {"13 first_state_diff max_mismatches 截断", case13_state_max_mismatches},
        {"14 memory_diff 缺失当 0", case14_memory_diff_missing_is_zero},
        {"15 fold 拒绝 size=16 的写（抛异常）", case15_fold_rejects_size16},
        {"16 fold 拒绝 size=0 的写（抛异常）", case16_fold_rejects_size0},
    };

    for (const std::pair<std::string, void (*)(Case&)>& one : cases) {
        Case c(one.first);
        one.second(c);
        c.finish();
    }

    std::cout << "\n总计 " << g_total << " 条用例：PASS " << (g_total - g_failed) << "，FAIL " << g_failed
              << "\n";
    if (g_failed == 0) {
        std::cout << "结论：全部 PASS\n";
        return 0;
    }
    std::cout << "结论：有 " << g_failed << " 条 FAIL —— 先查 diff.* 的实现，别改用例里的期望值\n";
    return 1;
}
