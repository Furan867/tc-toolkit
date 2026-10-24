// 差分判据的数据层：store 序列的比对、落盘与统计（设计 §2 的 L0/L1）。纯数据操作，不碰 Unicorn。
//
// `Store` 的定义归这里：独立进程拿两份落盘序列做离线比对时，不该为了一个结构体去链接 Unicorn。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tc {

// 一次写：写指令的绝对地址 + 目标 + 宽度 + 值（设计 §2）。load 不记 —— 一次 load 的值由之前的
// store 序列与初始内存决定，两边初始内存相同（同源码地址、同桩返回值），比 store 就够。
struct Store {
    uint64_t pc = 0;
    uint64_t addr = 0;
    uint32_t size = 0;   // 1/2/4/8/16
    uint64_t value = 0;  // 大端截断后按 size 取低位的实际写入值
};

// `length_only` = 短的一边是长的一边的前缀、没有逐条差异；此时 `index` 是短的一边的条数，
// `a`/`b` 无意义（保持全零）。
struct Divergence {
    size_t index = 0;
    bool length_only = false;
    Store a;
    Store b;
};

// 逐条比 `(pc, addr, size, value)`，第一个不同即返回（设计 §2 的 L1）；完全相同返回 nullopt。
std::optional<Divergence> first_divergence(const std::vector<Store>& a, const std::vector<Store>& b);

// 落盘格式：magic `"difftr01"`(8) + u32 版本(1) + u64 条数 + 每条 28 字节（pc/addr/size/value），
// 全部小端。逐字段编码而不是整块 memcpy：结构体有 4 字节填充，字节表示不能跟着编译器变。
void save(const std::string& path, const std::vector<Store>& stores);

// magic / 版本不对、被截断、尾部多余、`size` 不是 1、2、4、8、16 → 抛 std::runtime_error。
std::vector<Store> load(const std::string& path);

// 盯一个绝对地址的全部写，顺序保持原样。
std::vector<Store> filter_addr(const std::vector<Store>& stores, uint64_t addr);

// 按写指令地址计数（哪条写指令最热）。计数降序，计数相同按 key 升序（保证确定性）；
// `top_n == 0` 表示全部。
std::vector<std::pair<uint64_t, size_t>> histogram_pc(const std::vector<Store>& stores, size_t top_n);

// 按被写地址计数（哪些状态被写得最多）。排序与 `top_n` 同 `histogram_pc`。
std::vector<std::pair<uint64_t, size_t>> histogram_addr(const std::vector<Store>& stores,
                                                        size_t top_n);

// ---- 按 tick 折叠成内存状态再比 --------------------------------------------
//
// 逐条比两条轨迹只在「同一编译器前后两版」成立：DCE 让两边的写条数差三倍，按条对齐必然在
// index 0 就分叉，而那不代表状态不同。判「与参考等价」要的是内存状态。

// 内存镜像：字节级。键写成 `(addr, size)` 会在「1 字节 0」与「4 字节 0」之间分叉，而字节层面
// 那两件事是同一件事（后者覆盖的另外 3 个字节也是 0）。
using Memory = std::map<uint64_t, uint8_t>;

// 一段写序列 → 最终内存，最后一次写获胜。`Store::value` 只有 8 字节，`size == 16` 的写只能展开
// 低 8 字节（语料里的写宽度只有 1/2/4/8）。
Memory fold(const std::vector<Store>& seq, size_t begin, size_t end);

// 一个字节的差异：`addr` → `(A 的字节, B 的字节)`，`nullopt` = 那一边没写过这个地址。
using ByteDiff = std::pair<uint64_t, std::pair<std::optional<uint8_t>, std::optional<uint8_t>>>;

// 两份内存逐字节比：缺的键按 0 算（一边没写过 vs 另一边写了 0 不是差异）。差异总数是返回值，
// 写进 `out` 的条数受 `max` 限制（`max == 0` = 不截断）。
size_t memory_diff(const Memory& a, const Memory& b, size_t max, std::vector<ByteDiff>& out);

// 一个 tick 占写的下标区间 `[begin, end)`。
struct TickSpan {
    size_t begin = 0;
    size_t end = 0;
};

// 以「写 `tick_addr`」为界标切：界标那条写属于它结束的那一段；首个界标之前的写并入第一段，
// 末个界标之后不满一段的写自成最后一段（扔尾巴等于扔掉 run 结束时的状态）。
// 全程没有界标 → 整条序列一段（空序列 → 0 段）。
std::vector<TickSpan> split_ticks(const std::vector<Store>& seq, uint64_t tick_addr);

// 逐 tick 比的第一个差异。`tick` 从 0 起。两边 tick 数不同 → `tick_count` 是 `(A 的 tick 数,
// B 的 tick 数)`、`mismatches` 为空：此后无从逐 tick 比。完全等价返回 nullopt。
struct StateDiff {
    size_t tick = 0;
    std::vector<ByteDiff> mismatches;
    size_t mismatch_count = 0;  // 该 tick 的差异字节总数（`mismatches` 可能被截断）
    std::optional<std::pair<size_t, size_t>> tick_count;
};

// 逐 tick `fold` 后比 `Memory` 的全集。`max_mismatches == 0` = 不截断。
std::optional<StateDiff> first_state_diff(const std::vector<Store>& a, const std::vector<Store>& b,
                                          uint64_t tick_addr, size_t max_mismatches = 20);

}  // namespace tc
