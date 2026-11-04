// 提权重启：etw 要开 NT Kernel Logger（SeSystemProfilePrivilege），非管理员必然失败；
// suspend 免管理员，绝不动它 —— 一次 runas 会把进程挪到**新控制台**，stdin/stdout 上的管道
// 全断，run.py 喂命令和读统计都落空。所以提权判据只有一条：etw 且当前非管理员。
//
// 新控制台还有第二个后果：窗口跟着进程一起关，报错和报表没等人看就没了。所以提权重启
// 一律给新进程加 pause_on_exit_flag，让它在退出前停一下 —— 只有 stdin 是 TTY 才停，管道
// 调用（run.py / 单测）等下去会挂住。
#pragma once

#include "config.hh"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hotspot {

// 提权重启与被重启进程约定的参数名（run.py 那一层用同一个拼法）。
inline constexpr std::string_view pause_on_exit_flag = "--pause-on-exit";

// 按 CommandLineToArgvW / MSVC CRT 的规则拼一个命令行串：含空格或引号才包引号，
// 引号前的反斜杠翻倍，末尾的反斜杠也翻倍（否则它会吃掉后面那个收尾引号）。
std::wstring quote_argument(std::wstring_view argument);
std::wstring build_command_line(const std::vector<std::wstring> &arguments);

// 重启时的参数 = 原参数 + pause_on_exit_flag（已经带着就不重复加）。
std::vector<std::wstring> relaunch_arguments(const std::vector<std::wstring> &arguments);

// main.cc 的命令行解析抽成纯函数：main.cc 不在单测目标里，解析规则得能被测。
struct Arguments {
    std::string config_path = "hotspot/config.ini";
    bool config_given = false;
    bool pause_on_exit = false;
    bool help = false;
    std::optional<std::string> error; // 有值 = 参数不合法：main 打印它再退出
};

Arguments parse_arguments(const std::vector<std::string> &arguments);

// 等一次回车。要不要等由调用方判（TTY 才等），这里只管停。
void wait_for_enter();

// 当前进程是否已在管理员令牌下（TokenElevation；UAC 关掉时管理员账户这里也是 false，
// 因为进程确实没有被提升）。
bool is_elevated();

// 只有 etw 需要提权；已经是管理员就不提 —— 否则白弹一次 UAC 还把控制台换掉。
bool needs_elevation(Backend backend, bool already_elevated);

enum class RelaunchResult {
    started,   // 提权进程已起来，原进程该立刻退出（两个进程抢同一个会话名会互相踢）
    cancelled, // 用户在 UAC 上点了「否」
    failed,
};

// 用 ShellExecuteExW("runas") 重启自身：命令行参数与工作目录照搬，另加 pause_on_exit_flag。
// 失败时 error 是人话。
RelaunchResult relaunch_elevated(std::string &error);

} // namespace hotspot
