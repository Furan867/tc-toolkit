// REPL 命令解析（spec §4.6 的最小命令集）。纯函数，单测不需要游戏进程。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hotspot {

struct OpenArgs {
    std::filesystem::path artifact;
    std::optional<std::uint32_t> pid;
    std::optional<int> interval_ms;
};

struct Command {
    enum class Kind { open, window, status, locate, save, quit };

    Kind kind = Kind::status;
    OpenArgs open;
    int window_sec = 0;
    bool window_save = false;
    std::optional<std::filesystem::path> save_dir;
};

// 解析失败返回可读原因（未知命令 / 缺参数 / 参数不是数字 / 多余的参数）。
std::expected<Command, std::string> parse_command(std::string_view line);

// 按空白切分，保留引号内的空格（路径带空格时用得上）。
std::vector<std::string> split_arguments(std::string_view line);

} // namespace hotspot
