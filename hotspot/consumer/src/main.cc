// hotspot-consumer：常驻采样会话的入口（spec §4.6）。命令集见 commands.hh。
#include "config.hh"
#include "session.hh"

#include <windows.h>
#include <io.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void usage()
{
    std::cout << "用法：hotspot-consumer [--config <path>]\n"
                 "  --config  默认 hotspot/config.toml（不存在则用 spec §10 的默认值）\n"
                 "命令：open <artifact> [--pid N] [--interval-ms <ms>] / window <sec> [save] /\n"
                 "      status / locate / save [dir] / quit\n";
}

} // namespace

int main(int argc, char **argv)
{
    // 输出里有中文：控制台按 UTF-8 走，避免 GBK 代码页下变成乱码。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::filesystem::path config_path = "hotspot/config.toml";
    bool config_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "✗ --config 后面缺路径\n";
                return 2;
            }
            config_path = argv[++i];
            config_given = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        }
        std::cerr << "✗ 不认识的参数：" << argument << "\n";
        usage();
        return 2;
    }

    hotspot::Config config;
    std::error_code error;
    if (std::filesystem::is_regular_file(config_path, error)) {
        auto loaded = hotspot::load_config(config_path);
        if (!loaded.has_value()) {
            std::cerr << "✗ " << loaded.error() << "\n";
            return 2;
        }
        config = std::move(*loaded);
    } else if (config_given) {
        std::cerr << "✗ 配置文件不存在：" << config_path.string() << "\n";
        return 2;
    }

    std::cout << "hotspot-consumer：后端 " << hotspot::backend_name(config.backend) << "，间隔 "
              << config.interval_ms << " ms，窗口默认 " << config.window_sec << " s\n"
              << "游戏进程名 " << config.game_exe_name << "；配置文件 "
              << (config_given || std::filesystem::is_regular_file(config_path) ? config_path.string()
                                                                                : "（无，用默认值）")
              << "\n";

    hotspot::Session session(config);
    const bool interactive = _isatty(_fileno(stdin)) != 0;
    std::string line;
    for (;;) {
        if (interactive) {
            std::cout << "> " << std::flush;
        }
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        try {
            auto result = session.execute(line);
            if (result.has_value()) {
                std::cout << *result << "\n";
            } else {
                std::cerr << "✗ " << result.error() << "\n";
            }
        } catch (const std::exception &exception) {
            // 兜底：会话不能因为一条命令把整个 REPL 带走。
            std::cerr << "✗ 内部错误：" << exception.what() << "\n";
        }
        if (session.should_quit()) {
            break;
        }
    }
    return 0;
}
