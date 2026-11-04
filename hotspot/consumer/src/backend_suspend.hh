// suspend 后端：SuspendThread × GetThreadContext(RIP) → ResumeThread。
// 免管理员，代价是每个样本一次暂停（真实暂停消耗按窗口累计进 WindowStats::pause_*_us）。
#pragma once

#include "backend.hh"
#include "samples.hh"
#include "win_process.hh"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hotspot {

// 1 ms 定时器精度守卫（timeBeginPeriod / timeEndPeriod，见 backend_suspend.cc）。
// 精度是进程级全局状态，所以只包住采样循环本身；析构即恢复，提前 return / 异常都走析构。
// 可嵌套：timeEndPeriod 不认调用者，成对计数只能自己记，只有最外层真正调 Win32。
class TimerResolutionGuard {
  public:
    TimerResolutionGuard();
    ~TimerResolutionGuard();
    TimerResolutionGuard(const TimerResolutionGuard &) = delete;
    TimerResolutionGuard &operator=(const TimerResolutionGuard &) = delete;
    TimerResolutionGuard(TimerResolutionGuard &&) = delete;
    TimerResolutionGuard &operator=(TimerResolutionGuard &&) = delete;

    // 当前嵌套层数（测试用它验证成对与嵌套时序）。
    static int depth();
};

// 睡到 target：先 sleep_until(target - spin_margin)，剩下的自旋到目标。
// 只 sleep 的话，Windows 的唤醒按 timeBeginPeriod 的周期对齐，实际周期比 interval 长零点几毫秒
// —— 10 s / 1 ms 就只有五六千个样本而不是 10000 个。target 已经过去时立即返回（落后不补睡）。
void wait_until(std::chrono::steady_clock::time_point target, std::chrono::microseconds spin_margin);

class SuspendBackend : public SamplerBackend {
  public:
    SuspendBackend(std::uint32_t pid, std::vector<BlobCopy> copies, int probe_rounds);

    // 重新定位后更新产物区间（探测判据跟着变）。
    void set_copies(std::vector<BlobCopy> copies);

    const char *name() const override
    {
        return "suspend";
    }
    std::expected<std::string, std::string> prepare() override;
    std::expected<WindowStats, std::string> run_window(std::chrono::milliseconds interval,
                                                       std::chrono::seconds duration,
                                                       const SampleSink &sink) override;

    bool simulation_thread_locked() const
    {
        return locked_;
    }
    std::chrono::microseconds max_pause() const
    {
        return max_pause_;
    }
    const std::vector<std::uint32_t> &thread_ids() const
    {
        return thread_ids_;
    }

  private:
    std::expected<void, std::string> rescan_threads();
    // 取一次 RIP；失败返回 nullopt（线程恰好退出、句柄失效）。
    std::optional<std::pair<std::uintptr_t, std::chrono::microseconds>> sample_once(ThreadHandle &thread);
    bool in_blob(std::uintptr_t ip) const;

    std::uint32_t pid_;
    std::vector<BlobCopy> copies_;
    int probe_rounds_;
    std::vector<std::uint32_t> thread_ids_;
    std::vector<ThreadHandle> handles_;
    bool locked_ = false;
    std::chrono::microseconds max_pause_{0};
};

} // namespace hotspot
