// 采样后端的共同接口：只负责产生 (IP, ThreadId)，分桶与计数走 samples.hh（spec §4.0）。
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace hotspot {

using SampleSink = std::function<void(std::uintptr_t ip, std::uint32_t thread_id)>;

struct WindowStats {
    std::uint64_t samples = 0;
    std::chrono::milliseconds elapsed{0};
    std::chrono::microseconds max_pause{0}; // suspend 后端的单样本暂停时长上界实测值
    std::vector<std::uint32_t> thread_ids;
    std::string note;
};

class SamplerBackend {
  public:
    virtual ~SamplerBackend() = default;

    virtual const char *name() const = 0;

    // 开窗前的一次性准备（线程探测 / 会话自检）。返回给操作者看的说明，失败给原因。
    virtual std::expected<std::string, std::string> prepare() = 0;

    virtual std::expected<WindowStats, std::string>
    run_window(std::chrono::milliseconds interval, std::chrono::seconds duration,
               const SampleSink &sink) = 0;
};

} // namespace hotspot
