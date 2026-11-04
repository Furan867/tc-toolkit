// etw 后端：开 NT Kernel Logger 的 PROFILE 事件，实时收 SystemTraceProvider 的
// SampledProfile（spec §4.1）。需要管理员；创建/收流失败直接 abort（开发期，不自动回退）。
// 内核日志会话在 REPL 生命周期内只建一次 —— TraceSetInformation 设的间隔只在 StartTrace
// 之后生效一次，改间隔只能重建会话。
//
// ETW 头（windows.h / evntrace.h / evntcons.h）留在 .cc：本文件只暴露解码函数与接口，
// 解码单测不必把 ETW 会话拉起来。
#pragma once

#include "backend.hh"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace hotspot {

// SampledProfile 负载里的一个采样点（spec §4.1 的固定布局）。
struct SampledProfile {
    std::uintptr_t ip = 0;
    std::uint32_t thread_id = 0;
};

enum class ProfileDecode {
    ok,
    unknown_version, // 不认识的负载版本：丢掉，不猜布局
    short_payload,   // 长度不够该版本的最小布局
};

// 版本号来自 EVENT_HEADER.EventDescriptor.Version：v1 = IP(4)+TID(4)、v2 = IP(8)+TID(4)，
// 一律小端。负载里后续字段（Count 之类）本工具不用。
ProfileDecode decode_sampled_profile(std::uint8_t version,
                                     std::span<const std::uint8_t> user_data,
                                     SampledProfile &out);

struct EtwSession;

class EtwBackend : public SamplerBackend {
  public:
    EtwBackend(std::uint32_t pid, int interval_ms);
    ~EtwBackend() override;
    EtwBackend(const EtwBackend &) = delete;
    EtwBackend &operator=(const EtwBackend &) = delete;

    const char *name() const override
    {
        return "etw";
    }

    // 建会话 + 设间隔 + 开实时 trace + 起 ProcessTrace 线程。可重复调用（已建则空转）。
    std::expected<std::string, std::string> prepare() override;

    // 只收 duration 内的样本；trace 不回停，窗口之间会话继续活着。
    std::expected<WindowStats, std::string>
    run_window(std::chrono::milliseconds interval, std::chrono::seconds duration,
               const SampleSink &sink) override;

    std::uint32_t pid() const
    {
        return pid_;
    }
    int interval_ms() const
    {
        return interval_ms_;
    }

  private:
    std::uint32_t pid_;
    int interval_ms_;
    std::unique_ptr<EtwSession> session_;
};

} // namespace hotspot
