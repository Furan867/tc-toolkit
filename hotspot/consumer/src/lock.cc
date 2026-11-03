#include "lock.hh"

#include <windows.h>

#include <fstream>
#include <system_error>
#include <utility>

namespace hotspot {
namespace {

std::string lock_owner(std::uint32_t pid)
{
    return "pid=" + std::to_string(pid) + "\n";
}

std::string last_error_text(std::string_view what)
{
    const DWORD code = GetLastError();
    return std::string(what) + "（错误码 " + std::to_string(code) + "）";
}

} // namespace

GameLock::GameLock(GameLock &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr))
    , path_(std::move(other.path_))
    , owner_pid_(std::exchange(other.owner_pid_, 0))
{
}

GameLock &GameLock::operator=(GameLock &&other) noexcept
{
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
        owner_pid_ = std::exchange(other.owner_pid_, 0);
    }
    return *this;
}

GameLock::~GameLock()
{
    release();
}

std::expected<GameLock, std::string> GameLock::acquire(const std::filesystem::path &lock_file)
{
    std::error_code error;
    if (lock_file.has_parent_path()) {
        std::filesystem::create_directories(lock_file.parent_path(), error);
        if (error) {
            return std::unexpected("建锁目录失败：" + lock_file.parent_path().string() + "：" +
                                   error.message());
        }
    }

    const std::wstring wide_path = lock_file.wstring();
    HANDLE handle = CreateFileW(wide_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // dwShareMode = 0 时别的句柄也打不开（含本进程），所以占用者 PID 只能尽力读。
        std::string owner = "占用者未知（文件被独占打开）";
        std::ifstream existing(lock_file, std::ios::binary);
        std::string line;
        if (std::getline(existing, line); !line.empty()) {
            owner = line;
        }
        return std::unexpected("game.lock 被别的采样会话占着：" + lock_file.string() + "（" +
                               owner + "）；同一条游戏进程只能有一个采样者");
    }

    const std::string content = lock_owner(GetCurrentProcessId());
    const auto write = [&content, handle]() {
        SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(handle);
        DWORD written = 0;
        return WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written,
                         nullptr) != FALSE;
    };
    if (!write()) {
        CloseHandle(handle);
        return std::unexpected(last_error_text("写 game.lock 失败"));
    }

    // 校验：从同一个句柄读回来必须是自己刚写的。独占打开期间第二个句柄打不开，
    // 所以校验只能走本句柄（半开状态/文件系统把独占降级时要当场发现）。
    SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
    char readback[64]{};
    DWORD got = 0;
    const BOOL read_ok = ReadFile(handle, readback, sizeof(readback) - 1, &got, nullptr);
    const std::string expected = "pid=" + std::to_string(GetCurrentProcessId()) + "\n";
    if (read_ok == FALSE || std::string(readback, got) != expected) {
        CloseHandle(handle);
        return std::unexpected("game.lock 校验失败：读回的内容不是本进程写入的");
    }

    return GameLock(handle, lock_file);
}

void GameLock::release()
{
    if (handle_ == nullptr) {
        return;
    }
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    std::error_code error;
    std::filesystem::remove(path_, error);
}

} // namespace hotspot
