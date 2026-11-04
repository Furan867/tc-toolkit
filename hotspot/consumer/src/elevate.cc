#include "elevate.hh"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <iostream>

namespace hotspot {
namespace {

// FormatMessage 的系统文本转 UTF-8，去掉结尾换行。backend_etw.cc 里有一份同样的：
// 那条链不暴露头，复制这十几行比为了共用去动它的接口更小。
std::string win32_error_text(DWORD code)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    std::string text;
    if (length != 0 && message != nullptr) {
        std::wstring detail(message, length);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' ||
                                   detail.back() == L' ' || detail.back() == L'.')) {
            detail.pop_back();
        }
        const int utf8_length =
            WideCharToMultiByte(CP_UTF8, 0, detail.data(), static_cast<int>(detail.size()),
                                nullptr, 0, nullptr, nullptr);
        if (utf8_length > 0) {
            text.resize(static_cast<std::size_t>(utf8_length));
            WideCharToMultiByte(CP_UTF8, 0, detail.data(), static_cast<int>(detail.size()),
                                text.data(), utf8_length, nullptr, nullptr);
        }
    }
    if (message != nullptr) {
        LocalFree(message);
    }
    return text.empty() ? "错误码 " + std::to_string(code) : text;
}

std::wstring this_process_path()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        // 返回值等于缓冲区长度 = 截断了（老系统不报错，只能靠长度相等看出来）。
        path.resize(path.size() * 2);
    }
}

// 原始命令行里的宽字符 argv[1..]：走 ANSI argv 再转宽会把非本机代码页的参数弄坏。
std::vector<std::wstring> this_process_arguments()
{
    int count = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr) {
        return {};
    }
    std::vector<std::wstring> arguments;
    for (int i = 1; i < count; ++i) {
        arguments.emplace_back(argv[i]);
    }
    LocalFree(argv);
    return arguments;
}

std::wstring current_directory()
{
    const DWORD needed = GetCurrentDirectoryW(0, nullptr);
    if (needed == 0) {
        return {};
    }
    std::wstring directory(needed, L'\0');
    const DWORD written = GetCurrentDirectoryW(needed, directory.data());
    if (written == 0 || written >= needed) {
        return {};
    }
    directory.resize(written);
    return directory;
}

} // namespace

std::wstring quote_argument(std::wstring_view argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring quoted{L'"'};
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring build_command_line(const std::vector<std::wstring> &arguments)
{
    std::wstring line;
    for (const std::wstring &argument : arguments) {
        if (!line.empty()) {
            line.push_back(L' ');
        }
        line += quote_argument(argument);
    }
    return line;
}

Arguments parse_arguments(const std::vector<std::string> &arguments)
{
    Arguments parsed;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string &argument = arguments[i];
        if (argument == "--config") {
            if (i + 1 >= arguments.size()) {
                parsed.error = "--config 后面缺路径";
                return parsed;
            }
            parsed.config_path = arguments[++i];
            parsed.config_given = true;
            continue;
        }
        if (argument == pause_on_exit_flag) {
            parsed.pause_on_exit = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            parsed.help = true;
            return parsed;
        }
        parsed.error = "不认识的参数：" + argument;
        return parsed;
    }
    return parsed;
}

std::vector<std::wstring> relaunch_arguments(const std::vector<std::wstring> &arguments)
{
    std::vector<std::wstring> relaunch{arguments};
    const std::wstring pause{pause_on_exit_flag.begin(), pause_on_exit_flag.end()};
    if (std::find(relaunch.begin(), relaunch.end(), pause) == relaunch.end()) {
        relaunch.push_back(pause);
    }
    return relaunch;
}

void wait_for_enter()
{
    std::cout << "按回车退出…" << std::flush;
    std::string line;
    std::getline(std::cin, line);
}

bool is_elevated()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried = GetTokenInformation(token, TokenElevation, &elevation,
                                             sizeof(elevation), &returned);
    CloseHandle(token);
    return queried != FALSE && elevation.TokenIsElevated != 0;
}

bool needs_elevation(Backend backend, bool already_elevated)
{
    return backend == Backend::etw && !already_elevated;
}

RelaunchResult relaunch_elevated(std::string &error)
{
    const std::wstring path = this_process_path();
    if (path.empty()) {
        error = "取不到自己的可执行文件路径：" + win32_error_text(GetLastError());
        return RelaunchResult::failed;
    }
    const std::wstring parameters =
        build_command_line(relaunch_arguments(this_process_arguments()));
    const std::wstring directory = current_directory();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // NOASYNC：本进程拿到结果就退出，不能让 shell 的异步收尾落在一个已经没了的进程上。
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&info) != FALSE) {
        return RelaunchResult::started;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_CANCELLED) {
        return RelaunchResult::cancelled;
    }
    error = win32_error_text(code);
    return RelaunchResult::failed;
}

} // namespace hotspot
