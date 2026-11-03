#include "locate.hh"

#include "win_process.hh"

#include <algorithm>

namespace hotspot {
namespace {

constexpr std::size_t kFeatureSearchEnd = std::numeric_limits<std::size_t>::max();

} // namespace

std::size_t feature_offset(std::size_t code_size)
{
    if (code_size < kFeatureOffset + kFeatureLength) {
        return code_size / 3;
    }
    return kFeatureOffset;
}

std::span<const std::uint8_t> feature_window(std::span<const std::uint8_t> code)
{
    const std::size_t off = feature_offset(code.size());
    const std::size_t len = std::min(kFeatureLength, code.size() - off);
    return code.subspan(off, len);
}

CodeMatch verify_code_copy(std::span<const std::uint8_t> memory, std::span<const std::uint8_t> code)
{
    CodeMatch result;
    if (memory.size() < code.size()) {
        return result;
    }
    memory = memory.first(code.size());

    // 1. 特征窗：定位就是靠它命中的，它必须严格相同（过短产物的特征窗可能落在尾部）。
    const std::size_t feature_off = feature_offset(code.size());
    const std::size_t feature_len = std::min(kFeatureLength, code.size() - feature_off);
    if (!std::equal(code.begin() + static_cast<std::ptrdiff_t>(feature_off),
                    code.begin() + static_cast<std::ptrdiff_t>(feature_off + feature_len),
                    memory.begin() + static_cast<std::ptrdiff_t>(feature_off))) {
        return result;
    }

    // 2. 主体：必须逐字节严格相同。
    const std::size_t body_len = code.size() > kRuntimeTailLength ? code.size() - kRuntimeTailLength : 0;
    if (!std::equal(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(body_len),
                    memory.begin())) {
        return result;
    }

    // 3. 尾部：允许运行时写入，但有上限。
    std::size_t differences = 0;
    for (std::size_t i = body_len; i < code.size(); ++i) {
        if (code[i] != memory[i]) {
            ++differences;
        }
    }
    result.tail_differences = differences;
    result.matched = differences <= kRuntimeTailMaxDiff;
    return result;
}

std::vector<std::uintptr_t> feature_candidates(std::uintptr_t region_base,
                                               std::span<const std::uint8_t> region_data,
                                               std::size_t feature_off,
                                               std::span<const std::uint8_t> feature)
{
    std::vector<std::uintptr_t> candidates;
    if (feature.empty() || feature.size() > region_data.size()) {
        return candidates;
    }
    std::size_t pos = 0;
    while (pos <= kFeatureSearchEnd) {
        const auto found = std::search(region_data.begin() + static_cast<std::ptrdiff_t>(pos),
                                       region_data.end(), feature.begin(), feature.end());
        if (found == region_data.end()) {
            break;
        }
        const std::size_t hit = static_cast<std::size_t>(found - region_data.begin());
        if (hit >= feature_off) {
            candidates.push_back(region_base + (hit - feature_off));
        }
        pos = hit + 1;
    }
    return candidates;
}

std::expected<LocateResult, std::string> locate_in_process(std::uint32_t pid,
                                                           const Artifact &artifact)
{
    auto process = ProcessHandle::open(pid);
    if (!process.has_value()) {
        return std::unexpected(process.error());
    }

    auto regions = executable_regions(*process);
    if (!regions.has_value()) {
        return std::unexpected(regions.error());
    }

    const auto code = artifact.code();
    const auto feature = feature_window(code);
    if (feature.empty()) {
        return std::unexpected("产物太短，取不出特征窗");
    }
    const std::size_t feature_off = feature_offset(code.size());

    std::vector<std::uintptr_t> bases;
    for (const ExecRegion &region : *regions) {
        if (region.size > kMaxRegionSize) {
            continue;
        }
        std::vector<std::uint8_t> data(region.size);
        if (auto read = read_memory(*process, region.base, data.data(), data.size());
            !read.has_value()) {
            continue; // 区被释放/不可读：跳过，不当作定位失败
        }
        for (const std::uintptr_t candidate :
             feature_candidates(region.base, data, feature_off, feature)) {
            // 每次命中都要整段比对确认：特征碰撞不能算副本，且比对通过即完成
            // 「产物文件 == 内存里跑的代码」校验（spec §4.3 第 2 条）。
            // 尾部 .bss 会被游戏写，所以容差，见 verify_code_copy。
            std::vector<std::uint8_t> whole(code.size());
            if (auto read = read_memory(*process, candidate, whole.data(), whole.size());
                !read.has_value()) {
                continue;
            }
            if (verify_code_copy(whole, code).matched) {
                bases.push_back(candidate);
            }
        }
    }

    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());

    LocateResult result;
    result.bases = std::move(bases);
    result.regions = std::move(*regions);
    result.feature_off = feature_off;
    return result;
}

} // namespace hotspot
