// hotspot-consumer：常驻采样会话的入口（spec §4.6）。命令集见 commands.hh。
#include "config.hh"
#include "elevate.hh"
#include "session.hh"

#include <windows.h>
#include <io.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage()
{
    std::cout << "用法：hotspot-consumer [--config <path>] [--pause-on-exit]\n"
                 "  --config        默认 hotspot/config.ini（不存在则用 spec §10 的默认值）\n"
                 "  --pause-on-exit 退出前等一次回车（提权重启时自动加；stdin 非 TTY 不等）\n"
                 "命令：open <artifact> [--pid N] [--interval-ms <ms>] / window <sec> [save] /\n"
                 "      status / locate / save [dir] / quit\n";
}

int run(const hotspot::Arguments &options, bool interactive)
{
    const std::filesystem::path config_path = options.config_path;
    const bool config_given = options.config_given;

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

    // 提权判据与时机：读完 config（后端才定得下来）、建 Session 之前（会话/锁都在 Session 里）。
    if (hotspot::needs_elevation(config.backend, hotspot::is_elevated())) {
        std::cout << "→ etw 后端需要管理员：请求提权（UAC）…" << std::endl;
        std::string elevation_error;
        switch (hotspot::relaunch_elevated(elevation_error)) {
        case hotspot::RelaunchResult::started:
            return 0;
        case hotspot::RelaunchResult::cancelled:
            std::cerr << "✗ 已取消提权：etw 后端要开内核日志会话（SeSystemProfilePrivilege）\n";
            return 3;
        case hotspot::RelaunchResult::failed:
            std::cerr << "✗ 提权重启失败：" << elevation_error << "\n";
            return 3;
        }
    }

    std::cout << "hotspot-consumer：后端 " << hotspot::backend_name(config.backend) << "，间隔 "
              << config.interval_ms << " ms，窗口默认 " << config.window_sec << " s\n"
              << "游戏进程名 " << config.game_exe_name << "；配置文件 "
              << (config_given || std::filesystem::is_regular_file(config_path) ? config_path.string()
                                                                                : "（无，用默认值）")
              << "\n";

    hotspot::Session session(config);
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

} // namespace

int main(int argc, char **argv)
{
    // 输出里有中文：控制台按 UTF-8 走，避免 GBK 代码页下变成乱码。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const hotspot::Arguments options =
        hotspot::parse_arguments(std::vector<std::string>(argv + 1, argv + argc));
    if (options.error.has_value()) {
        std::cerr << "✗ " << *options.error << "\n";
        usage();
        return 2;
    }
    if (options.help) {
        usage();
        return 0;
    }

    // stdin 是 TTY 才算有人守着窗口：管道调用（run.py / 单测）等到 EOF 就继续，等下去只会挂住。
    const bool interactive = _isatty(_fileno(stdin)) != 0;
    const int code = run(options, interactive);
    if (options.pause_on_exit && interactive) {
        hotspot::wait_for_enter();
    }
    return code;
}
