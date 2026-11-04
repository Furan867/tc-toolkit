// etw 后端：开 NT Kernel Logger 的 PROFILE 事件，实时收 SystemTraceProvider 的
// SampledProfile（spec §4.1）。需要管理员；创建/收流失败直接 abort（开发期，不自动回退）。
// 内核日志会话在 REPL 生命周期内只建一次 —— TraceSetInformation 设的间隔只在 StartTrace
// 之后生效一次，改间隔只能重建会话。
//
// ETW 头（windows.h / evntrace.h / evntcons.h）留在 .cc：本文件只暴露解码函数与接口，
// 解码单测不必把 ETW 会话拉起来。
#pragma once

#include "backend.hh"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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

// EVENT_HEADER::ProviderId。ETW 头只留在 .cc，所以这里用与 GUID 同形的字段（Data1..Data4），
// 由 .cc 转成 GUID 去比较；单测也能直接核对取值。
struct EventProviderGuid {
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::array<std::uint8_t, 8> data4{};
};

// SampledProfile 的 provider 是它所属的 MOF 事件类 PerfInfo 的类 GUID，不是会话控制 GUID
// （{9e814aad-…} 那份只给 StartTraceW 的 Wnode.Guid 用，见 backend_etw.cc 的 kSystemTraceControlGuid）。
// 出处：文档「PerfInfo class」页 `[Guid("{ce1dbfb4-137e-4da6-87b0-3f59aa102cbc}")] class PerfInfo`
// 且「Event type value, 46」= sampled profile；「SampledProfile class」页 `class SampledProfile : PerfInfo`；
// 「EVENT_TRACE_HEADER」页把 Guid 定义成「event trace class GUID」——ProviderId 就是它。
inline constexpr EventProviderGuid kSampledProfileProviderGuid{
    0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
inline constexpr std::uint16_t kSampledProfileEventId = 46;

// abort 之前要把失败原因落盘：「为什么崩」只在终端上留不住（被 GUI 拉起、窗口一闪就没），
// 崩溃后没有第二次机会。HOTSPOT_ETW_FAIL_LOG 覆盖路径给单测用。
std::string etw_fail_log_path();

// 一行失败记录：`<时间> etw：<what> 失败（code=<错误码> <系统错误文本>）`。
std::string format_etw_fail_line(std::string_view timestamp, std::string_view what,
                                 unsigned long code, std::string_view text);

// 追加一行（换行由本函数加）；建目录、写文件失败一律忽略 —— 调用方马上要 abort，
// 收尾路径上不能再抛失败。
void append_etw_fail_log(const std::string &path, std::string_view line);

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
