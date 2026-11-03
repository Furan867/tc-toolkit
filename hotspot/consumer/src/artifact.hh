// 产物（blob）文件：前 8 字节是长度头，游戏 JIT 拷贝的是 code()（= blob+8）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hotspot {

struct Artifact {
    std::filesystem::path path;
    std::vector<std::uint8_t> file; // 整份文件，含 8 字节头
    std::size_t header_len = 0;     // 8 = 长度头识别成功；0 = 没识别出（按整份文件当机器码）
    std::string sha256;             // 整份文件（含头），与 hook manifest 的 out_sha256 同口径
    std::optional<std::string> num; // hook 编译点编号

    std::span<const std::uint8_t> code() const
    {
        return std::span<const std::uint8_t>(file).subspan(header_len);
    }
    std::size_t code_size() const
    {
        return file.size() - header_len;
    }
};

std::expected<Artifact, std::string> load_artifact(const std::filesystem::path &path);

// "hook_out_013" -> "013"；不是这个形态就 nullopt。
std::optional<std::string> hook_num_from_stem(std::string_view stem);

} // namespace hotspot
