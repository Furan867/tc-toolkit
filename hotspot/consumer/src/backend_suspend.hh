// suspend 后端：SuspendThread → GetThreadContext(RIP) → ResumeThread。
// 免管理员，代价是每个样本一次暂停（上界实测值回填进 WindowStats::max_pause）。
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
