// 定位的纯函数部分（特征窗、候选扫描）+ 产物头解析。进程那一面要游戏才能测。
#include "artifact.hh"
#include "locate.hh"
#include "test_support.hh"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

using hotspot::feature_candidates;
using hotspot::feature_offset;
using hotspot::feature_window;
using hotspot::hook_num_from_stem;
using hotspot::load_artifact;

namespace {

std::filesystem::path make_temp_dir(const std::string &tag)
{
    const auto dir = std::filesystem::temp_directory_path() / ("hotspot_test_" + tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> make_blob(std::uint64_t head, const std::vector<std::uint8_t> &code)
{
    std::vector<std::uint8_t> bytes(8);
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(head >> (i * 8));
    }
    bytes.insert(bytes.end(), code.begin(), code.end());
    return bytes;
}

} // namespace

TEST_CASE(feature_offset_uses_middle_window_and_falls_back_for_short_code)
{
    CHECK_EQ(feature_offset(0x10000), std::size_t{0x400});
    // 放不下 0x400 + 256 时退到 code_size/3（旧链同一口径）。
    CHECK_EQ(feature_offset(300), std::size_t{100});
}

TEST_CASE(feature_window_is_256_bytes_at_the_blob_offset)
{
    std::vector<std::uint8_t> code(0x2000);
    for (std::size_t i = 0; i < code.size(); ++i) {
        code[i] = static_cast<std::uint8_t>(i);
    }
    const auto window = feature_window(code);
    CHECK_EQ(window.size(), std::size_t{256});
    CHECK_EQ(window.front(), code[0x400]);
    CHECK_EQ(window.back(), code[0x400 + 255]);
}

TEST_CASE(feature_candidates_subtract_the_feature_offset)
{
    // 区里放两份特征：偏移 0x500 与 0x800；特征偏移 0x400 → 候选基址在 0x100 与 0x400。
    constexpr std::uintptr_t kBase = 0x10000000;
    constexpr std::size_t kFeatureOff = 0x400;
    std::vector<std::uint8_t> data(0x2000, 0xAA);
    std::vector<std::uint8_t> feature(256);
    for (std::size_t i = 0; i < feature.size(); ++i) {
        feature[i] = static_cast<std::uint8_t>(0x40 + i % 0x40);
    }
    std::copy(feature.begin(), feature.end(), data.begin() + 0x500);
    std::copy(feature.begin(), feature.end(), data.begin() + 0x800);

    const auto candidates = feature_candidates(kBase, data, kFeatureOff, feature);
    CHECK_EQ(candidates.size(), std::size_t{2});
    CHECK_EQ(candidates[0], static_cast<std::uintptr_t>(kBase + 0x100));
    CHECK_EQ(candidates[1], static_cast<std::uintptr_t>(kBase + 0x400));
}

TEST_CASE(feature_candidates_skip_hits_before_the_feature_offset)
{
    // 命中位置不够靠前 → 副本起点在区外，不是副本，不给候选。
    constexpr std::uintptr_t kBase = 0x10000000;
    std::vector<std::uint8_t> data(0x1000, 0x11);
    std::vector<std::uint8_t> feature(64, 0x77);
    std::copy(feature.begin(), feature.end(), data.begin() + 0x10);
    const auto candidates = feature_candidates(kBase, data, 0x400, feature);
    CHECK(candidates.empty());
}

TEST_CASE(artifact_header_recognised_with_bit62_set)
{
    const auto dir = make_temp_dir("artifact_ok");
    const std::filesystem::path path = dir / "hook_out_013.bin";
    std::vector<std::uint8_t> code(0x800);
    for (std::size_t i = 0; i < code.size(); ++i) {
        code[i] = static_cast<std::uint8_t>(i * 7);
    }
    const std::uint64_t total_len = code.size() + 8;
    const std::uint64_t head = total_len | (1ull << 62); // bit62 = 游戏不 free
    auto bytes = make_blob(head, code);
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    const auto artifact = load_artifact(path);
    CHECK(artifact.has_value());
    CHECK_EQ(artifact->header_len, std::size_t{8});
    CHECK_EQ(artifact->code_size(), code.size());
    CHECK(artifact->num.has_value());
    CHECK_EQ(*artifact->num, std::string{"013"});
    CHECK_EQ(artifact->sha256.size(), std::size_t{64});
    // sha256 覆盖整份文件（含头），与 hook manifest 同口径：改一个字节就变。
    const std::string before = artifact->sha256;
    bytes[bytes.size() - 1] ^= 0xFF;
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    const auto changed = load_artifact(path);
    CHECK(changed.has_value());
    CHECK(!(changed->sha256 == before));
    std::filesystem::remove_all(dir);
}

TEST_CASE(artifact_without_length_header_keeps_whole_file_as_code)
{
    const auto dir = make_temp_dir("artifact_nohead");
    const std::filesystem::path path = dir / "plain.bin";
    const std::vector<std::uint8_t> code{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    // 头字段既不等于文件长度、也不等于「长度 | bit62」：认不出头，整份当机器码。
    const auto bytes = make_blob(0xDEAD, code);
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    const auto artifact = load_artifact(path);
    CHECK(artifact.has_value());
    CHECK_EQ(artifact->header_len, std::size_t{0});
    CHECK_EQ(artifact->code_size(), bytes.size());
    CHECK(!artifact->num.has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE(artifact_missing_file_reports_reason)
{
    const auto artifact = load_artifact("no/such/artifact.bin");
    CHECK(!artifact.has_value());
    CHECK(artifact.error().find("产物不存在") != std::string::npos);
}

TEST_CASE(hook_num_from_stem_takes_trailing_digits)
{
    CHECK(hook_num_from_stem("hook_out_013").has_value());
    CHECK_EQ(*hook_num_from_stem("hook_out_013"), std::string{"013"});
    CHECK(!hook_num_from_stem("plain").has_value());
}
