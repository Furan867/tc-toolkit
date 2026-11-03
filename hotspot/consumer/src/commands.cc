#include "commands.hh"

#include <charconv>

namespace hotspot {
namespace {

std::optional<int> parse_positive_int(std::string_view text)
{
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value <= 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> parse_pid(std::string_view text)
{
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::vector<std::string> split_arguments(std::string_view line)
{
    std::vector<std::string> arguments;
    std::string current;
    bool in_quotes = false;
    for (const char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && (c == ' ' || c == '\t')) {
            if (!current.empty()) {
                arguments.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        arguments.push_back(current);
    }
    return arguments;
}

std::expected<Command, std::string> parse_command(std::string_view line)
{
    const std::vector<std::string> arguments = split_arguments(line);
    if (arguments.empty()) {
        return std::unexpected("空命令（可用：open / window / status / locate / save / quit）");
    }

    const std::string &verb = arguments.front();
    Command command;

    if (verb == "quit" || verb == "exit") {
        if (arguments.size() != 1) {
            return std::unexpected("quit 不接受参数");
        }
        command.kind = Command::Kind::quit;
        return command;
    }
    if (verb == "status") {
        if (arguments.size() != 1) {
            return std::unexpected("status 不接受参数");
        }
        command.kind = Command::Kind::status;
        return command;
    }
    if (verb == "locate") {
        if (arguments.size() != 1) {
            return std::unexpected("locate 不接受参数");
        }
        command.kind = Command::Kind::locate;
        return command;
    }
    if (verb == "open") {
        command.kind = Command::Kind::open;
        bool have_artifact = false;
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const std::string &argument = arguments[i];
            if (argument == "--pid" || argument == "--interval-ms") {
                if (i + 1 >= arguments.size()) {
                    return std::unexpected(argument + " 后面缺值");
                }
                const std::string &value = arguments[++i];
                if (argument == "--pid") {
                    command.open.pid = parse_pid(value);
                    if (!command.open.pid.has_value()) {
                        return std::unexpected("--pid 需要正整数，收到 \"" + value + "\"");
                    }
                } else {
                    command.open.interval_ms = parse_positive_int(value);
                    if (!command.open.interval_ms.has_value()) {
                        return std::unexpected("--interval-ms 需要正整数，收到 \"" + value + "\"");
                    }
                }
                continue;
            }
            if (have_artifact) {
                return std::unexpected("open 只接受一个产物路径，多余的参数：\"" + argument + "\"");
            }
            command.open.artifact = argument;
            have_artifact = true;
        }
        if (!have_artifact) {
            return std::unexpected("用法：open <artifact> [--pid N] [--interval-ms <ms>]");
        }
        return command;
    }
    if (verb == "window") {
        command.kind = Command::Kind::window;
        if (arguments.size() < 2) {
            return std::unexpected("用法：window <sec> [save]");
        }
        const std::optional<int> seconds = parse_positive_int(arguments[1]);
        if (!seconds.has_value()) {
            return std::unexpected("window 的时长需要正整数秒，收到 \"" + arguments[1] + "\"");
        }
        command.window_sec = *seconds;
        for (std::size_t i = 2; i < arguments.size(); ++i) {
            if (arguments[i] == "save") {
                if (command.window_save) {
                    return std::unexpected("window 里 save 写了两遍");
                }
                command.window_save = true;
                continue;
            }
            return std::unexpected("window 不认识参数 \"" + arguments[i] + "\"（只接受 save）");
        }
        return command;
    }
    if (verb == "save") {
        command.kind = Command::Kind::save;
        if (arguments.size() > 2) {
            return std::unexpected("用法：save [dir]");
        }
        if (arguments.size() == 2) {
            command.save_dir = arguments[1];
        }
        return command;
    }
    return std::unexpected("未知命令 \"" + verb + "\"（可用：open / window / status / locate / save / quit）");
}

} // namespace hotspot
