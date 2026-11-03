// 产物在目标进程里的定位（spec §4.3）。纯函数部分（特征窗、扫描）单独出来给单测，
// 进程部分只负责 VirtualQueryEx / ReadProcessMemory。
#pragma once

#include "artifact.hh"
#include "samples.hh"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace hotspot {

// 特征取代码段中段固定窗：不能取开头（所有产物开头同一段固定前缀，会全数命中）。
inline constexpr std::size_t kFeatureOffset = 0x400;
inline constexpr std::size_t kFeatureLength = 256;
// 比这还大的区不整体读（读进来只为搜特征，128MB 已远超任何 JIT 产物所在区）。
inline constexpr std::size_t kMaxRegionSize = 128u * 1024u * 1024u;

// 产物过短（feature_off + 窗长放不下）退回 code_size/3。
std::size_t feature_offset(std::size_t code_size);
std::span<const std::uint8_t> feature_window(std::span<const std::uint8_t> code);

// 尾部容许差异的长度：产物末尾是 .bss/外部槽（stub 之前那段）的存储区，游戏运行时会把
// 电流值/指针写进去（实测 hook_out_007.bin：内存 0x56b0 处 0x0a/0x0d/0x10/0x13/0x17，
// 文件里是 0x00）；该产物 .bss 约 960 B（起点 0x5300，body_end 0x56c0），所以窗口按整段 .bss
// 取 1024 B。逐字节严格比对会把**正在跑的**副本判成错配，真机永远定位不到。
inline constexpr std::size_t kRuntimeTailLength = 1024;
// 尾部允许的差异字节数上限：够容 .bss 被写的位置，又不至于让「另一份产物」混进来。
inline constexpr std::size_t kRuntimeTailMaxDiff = 64;

struct CodeMatch {
    bool matched = false;
    std::size_t tail_differences = 0;
};

// 内存里这份副本 == 产物机器码？判据：
//   1. 特征窗区间必须严格相同；
//   2. 主体 [0, code_size − kRuntimeTailLength) 逐字节严格相同；
//   3. 尾部 kRuntimeTailLength 字节允许差异，但总数 ≤ kRuntimeTailMaxDiff。
// 差异出现在主体内，或尾部差异超过阈值，一律拒绝。
CodeMatch verify_code_copy(std::span<const std::uint8_t> memory, std::span<const std::uint8_t> code);

// 在一段区内容里找特征命中，返回候选代码起点（= 命中位置 − 特征偏移）。
// 命中位置不够靠前（< feature_off）时没有候选——那说明副本起点在区外，本来就不是副本。
std::vector<std::uintptr_t> feature_candidates(std::uintptr_t region_base,
                                               std::span<const std::uint8_t> region_data,
                                               std::size_t feature_off,
                                               std::span<const std::uint8_t> feature);

struct LocateResult {
    std::vector<std::uintptr_t> bases; // 全部整段校验通过的副本基址，升序、去重
    std::vector<ExecRegion> regions;   // 快照：目标进程的可执行区（分桶用）
    std::size_t feature_off = 0;
};

std::expected<LocateResult, std::string> locate_in_process(std::uint32_t pid,
                                                           const Artifact &artifact);

} // namespace hotspot
