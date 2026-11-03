#include "artifact.hh"

#include "sha256.hh"

#include <fstream>
#include <iterator>

namespace hotspot {
namespace {

// bit62 = 游戏不 free 这份产物（spec §4.3）。长度判据必须先掩掉它，否则
// 「head == 文件长度」对所有带 bit62 的产物失效，特征偏移会整体错 8 字节。
constexpr std::uint64_t kNotFreedFlag = 1ull << 62;

} // namespace

std::optional<std::string> hook_num_from_stem(std::string_view stem)
{
    std::size_t digits = 0;
    while (digits < stem.size() && stem[stem.size() - 1 - digits] >= '0' &&
           stem[stem.size() - 1 - digits] <= '9') {
        ++digits;
    }
    if (digits == 0) {
        return std::nullopt;
    }
    return std::string(stem.substr(stem.size() - digits));
}

std::expected<Artifact, std::string> load_artifact(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::unexpected("产物不存在：" + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("打不开产物：" + path.string());
    }
    Artifact artifact;
    artifact.path = path;
    artifact.file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in.eof() && in.fail()) {
        return std::unexpected("读产物失败：" + path.string());
    }
    if (artifact.file.size() < 8) {
        return std::unexpected("产物太短，连 8 字节长度头都不够：" + path.string());
    }

    std::uint64_t head = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        head |= static_cast<std::uint64_t>(artifact.file[i]) << (i * 8);
    }
    const std::uint64_t length = head & ~kNotFreedFlag;
    if (length == artifact.file.size()) {
        artifact.header_len = 8;
    }
    if (artifact.code_size() == 0) {
        return std::unexpected("产物没有机器码（只有头）：" + path.string());
    }
    artifact.sha256 = sha256_hex(artifact.file.data(), artifact.file.size());
    if (const auto stem = path.stem().string(); !stem.empty()) {
        artifact.num = hook_num_from_stem(stem);
    }
    return artifact;
}

} // namespace hotspot
