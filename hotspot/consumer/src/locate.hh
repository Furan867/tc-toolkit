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
