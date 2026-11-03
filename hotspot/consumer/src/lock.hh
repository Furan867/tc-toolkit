// game.lock：采样期间独占游戏进程（spec §10 lock.dir）。
// 独占靠 CreateFileW 的 dwShareMode = 0 —— 文件只要是「被别人独占打开」的，第二次打开就
// 失败并给出占用者 PID；不是靠「文件存在与否」判（崩溃残留的文件会让工具永远开不了机）。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace hotspot {

class GameLock {
  public:
    GameLock() = default;
    GameLock(const GameLock &) = delete;
    GameLock &operator=(const GameLock &) = delete;
    GameLock(GameLock &&other) noexcept;
    GameLock &operator=(GameLock &&other) noexcept;
    ~GameLock();

    // 目录不存在会建。被别的进程占着时返回原因（含占用者 PID），不静默抢占。
    static std::expected<GameLock, std::string> acquire(const std::filesystem::path &lock_file);

    void release();

    const std::filesystem::path &path() const
    {
        return path_;
    }
    bool held() const
    {
        return handle_ != nullptr;
    }

  private:
    GameLock(void *handle, std::filesystem::path path)
        : handle_(handle)
        , path_(std::move(path))
    {
    }

    void *handle_ = nullptr;
    std::filesystem::path path_;
    std::uint32_t owner_pid_ = 0;
};

} // namespace hotspot
