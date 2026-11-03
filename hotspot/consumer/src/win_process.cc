#include "win_process.hh"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <utility>

namespace hotspot {
namespace {

constexpr DWORD kExecProtectMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                   PAGE_EXECUTE_WRITECOPY;

// 取 W 版系统消息再转 UTF-8：A 版在中文 Windows 上给 GBK，直接进 UTF-8 控制台是乱码。
std::string last_error_text(std::string_view what)
{
    wchar_t *message = nullptr;
    const DWORD code = GetLastError();
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);

    std::string text(what);
    text += "（错误码 " + std::to_string(code);
    if (length != 0 && message != nullptr) {
        std::wstring detail(message, length);
        while (!detail.empty() && std::iswspace(detail.back())) {
            detail.pop_back();
        }
        const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, detail.data(),
                                                    static_cast<int>(detail.size()), nullptr, 0,
                                                    nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(utf8_length), 0);
        WideCharToMultiByte(CP_UTF8, 0, detail.data(), static_cast<int>(detail.size()), utf8.data(),
                            utf8_length, nullptr, nullptr);
        text += "：" + utf8;
    }
    text += "）";
    if (message != nullptr) {
        LocalFree(message);
    }
    return text;
}

std::string to_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

} // namespace

ProcessHandle::ProcessHandle(ProcessHandle &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr))
{
}

ProcessHandle &ProcessHandle::operator=(ProcessHandle &&other) noexcept
{
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

ProcessHandle::~ProcessHandle()
{
    close();
}

void ProcessHandle::close()
{
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

std::expected<ProcessHandle, std::string> ProcessHandle::open(std::uint32_t pid)
{
    // 只要查询与读内存：不给 PROCESS_VM_WRITE / PROCESS_SUSPEND_RESUME，最小权限。
    HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (handle == nullptr) {
        return std::unexpected(last_error_text("OpenProcess(" + std::to_string(pid) + ") 失败"));
    }
    return ProcessHandle(handle);
}

ThreadHandle::ThreadHandle(ThreadHandle &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr))
{
}

ThreadHandle &ThreadHandle::operator=(ThreadHandle &&other) noexcept
{
    if (this != &other) {
        if (handle_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

ThreadHandle::~ThreadHandle()
{
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

std::expected<ThreadHandle, std::string> ThreadHandle::open(std::uint32_t thread_id)
{
    HANDLE handle = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME,
                               FALSE, thread_id);
    if (handle == nullptr) {
        // 受限进程上 THREAD_QUERY_INFORMATION 会被拒，换 *LIMITED* 再试一次。
        handle = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION |
                                THREAD_SUSPEND_RESUME,
                            FALSE, thread_id);
    }
    if (handle == nullptr) {
        return std::unexpected(last_error_text("OpenThread(" + std::to_string(thread_id) + ") 失败"));
    }
    return ThreadHandle(handle);
}

std::expected<std::uint32_t, std::string> find_pid_by_name(std::string_view exe_name)
{
    const std::string wanted = to_lower(std::string(exe_name));
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::unexpected(last_error_text("CreateToolhelp32Snapshot(进程) 失败"));
    }
    std::vector<std::uint32_t> matches;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            const std::wstring wide(entry.szExeFile);
            const std::string name(wide.begin(), wide.end());
            if (to_lower(name) == wanted) {
                matches.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);

    if (matches.empty()) {
        return std::unexpected("没找到进程 " + std::string(exe_name) + "（游戏没在运行？）");
    }
    if (matches.size() > 1) {
        std::string pids;
        for (const std::uint32_t pid : matches) {
            pids += (pids.empty() ? "" : ", ") + std::to_string(pid);
        }
        return std::unexpected("进程 " + std::string(exe_name) + " 有多个实例（PID " + pids +
                               "）：样本归属会错，拒绝猜。用 open --pid N 指定");
    }
    return matches.front();
}

std::expected<std::vector<std::uint32_t>, std::string> snapshot_thread_ids(std::uint32_t pid)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::unexpected(last_error_text("CreateToolhelp32Snapshot(线程) 失败"));
    }
    std::vector<std::uint32_t> ids;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                ids.push_back(entry.th32ThreadID);
            }
        } while (Thread32Next(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::expected<bool, std::string> is_process_alive(std::uint32_t pid)
{
    auto process = ProcessHandle::open(pid);
    if (!process.has_value()) {
        return false;
    }
    DWORD exit_code = 0;
    if (GetExitCodeProcess(static_cast<HANDLE>(process->raw()), &exit_code) == FALSE) {
        return std::unexpected(last_error_text("GetExitCodeProcess 失败"));
    }
    return exit_code == STILL_ACTIVE;
}

std::expected<std::vector<ExecRegion>, std::string> executable_regions(const ProcessHandle &process)
{
    std::vector<ExecRegion> regions;
    auto address = reinterpret_cast<std::uintptr_t>(nullptr);
    MEMORY_BASIC_INFORMATION info{};
    while (VirtualQueryEx(static_cast<HANDLE>(process.raw()), reinterpret_cast<LPCVOID>(address),
                          &info, sizeof(info)) == sizeof(info)) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const std::size_t size = info.RegionSize;
        if (size == 0) {
            break;
        }
        const bool committed = (info.State & MEM_COMMIT) != 0;
        const bool executable = (info.Protect & kExecProtectMask) != 0;
        const bool guarded = (info.Protect & PAGE_GUARD) != 0;
        if (committed && executable && !guarded) {
            regions.push_back(ExecRegion{base, size, (info.Type & MEM_IMAGE) != 0});
        }
        address = base + size;
        if (address < base) {
            break; // 地址回绕：只剩最后一页
        }
    }
    std::sort(regions.begin(), regions.end(),
              [](const ExecRegion &left, const ExecRegion &right) { return left.base < right.base; });
    return regions;
}

std::expected<void, std::string> read_memory(const ProcessHandle &process, std::uintptr_t address,
                                             void *destination, std::size_t length)
{
    SIZE_T read = 0;
    if (ReadProcessMemory(static_cast<HANDLE>(process.raw()), reinterpret_cast<LPCVOID>(address),
                          destination, length, &read) == FALSE) {
        return std::unexpected(last_error_text("ReadProcessMemory 失败"));
    }
    if (read != length) {
        return std::unexpected("ReadProcessMemory 只读到 " + std::to_string(read) + "/" +
                               std::to_string(length) + " 字节");
    }
    return {};
}

} // namespace hotspot
