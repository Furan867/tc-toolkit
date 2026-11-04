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
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    // C++20 不会凭空给 class 生成 ==，所以诊断用的比较要显式说一声。
    bool operator==(const EventProviderGuid &) const = default;
};

// SampledProfile 的 provider 是它所属的 MOF 事件类 PerfInfo 的类 GUID，不是会话控制 GUID
// （{9e814aad-…} 那份只给 StartTraceW 的 Wnode.Guid 用，见 backend_etw.cc 的 kSystemTraceControlGuid）。
// 出处：文档「PerfInfo class」页 `[Guid("{ce1dbfb4-137e-4da6-87b0-3f59aa102cbc}")] class PerfInfo`
// 且「Event type value, 46」= sampled profile；「SampledProfile class」页 `class SampledProfile : PerfInfo`；
// 「EVENT_TRACE_HEADER」页把 Guid 定义成「event trace class GUID」——ProviderId 就是它。
inline constexpr EventProviderGuid kSampledProfileProviderGuid{
    0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
// SampledProfile 的判别值 46 落在 EventDescriptor.**Opcode**，不在 Id：文档里 PerfInfo 的
// 「Event type value, 46」是 classic 事件头的 Class.Type 字段，ETW 把这个头转成
// EVENT_DESCRIPTOR 时放进 Opcode；Id 是另一套编号。真机上按 Id == 46 过滤时样本数为 0。
inline constexpr std::uint16_t kSampledProfileOpcode = 46;

// 过滤判据的输入：事件头里辨认 SampledProfile 用得上的字段。id 不参与接受/拒绝，留着只为
// 让「Id=46 且 Opcode=0 也要拒」这件事在单测里有一处能写死。
struct SampledProfileHeader {
    EventProviderGuid provider;
    std::uint16_t id = 0;
    std::uint16_t opcode = 0;
};

// 接受条件：ProviderId 是 PerfInfo 类 GUID 且 Opcode 是 46。
bool matches_sampled_profile(const SampledProfileHeader &header);

// 一条没通过过滤的事件的事件头五元组。过滤只看 ProviderId + Opcode，Id 不参与判断但照样
// 记下来；Version 与 UserDataLength 也是为了回答「来的到底是什么」—— 只有「非 SampledProfile
// 事件 15242 个」这一句时，看不出是 provider 不对、opcode 不对还是布局换了。
struct UnfilteredEventKey {
    EventProviderGuid provider;
    std::uint16_t event_id = 0;
    std::uint16_t opcode = 0;
    std::uint8_t version = 0;
    std::uint16_t user_data_length = 0;

    bool operator==(const UnfilteredEventKey &) const = default;
};

// 窗口内未通过过滤事件的诊断账本。组合种类通常是个位数，逐种留全（计数 + UserData 前 16
// 字节）才看得出过滤条件错在哪；但事件本身能上万条，所以只留前 8 种，其余只累计。
class UnfilteredEventTally {
  public:
    static constexpr std::size_t kMaxKinds = 8;
    static constexpr std::size_t kUserDataPrefixBytes = 16;

    // 同一组合只留一份，重复出现只加计数；满 kMaxKinds 种之后再来的进 other_。
    void record(const UnfilteredEventKey &key, std::span<const std::uint8_t> user_data);

    // 明细文本：一种组合一行（真实 GUID / Id / Opcode / Version / Size、计数、UserData 前缀
    // hex）。
    // 没有任何记录时返回空串，note 里就不会多出一节。
    std::string section() const;

    // 超过 kMaxKinds 种之后被合并掉的事件数。
    std::uint64_t other_events() const
    {
        return other_;
    }

  private:
    struct Kind {
        UnfilteredEventKey key;
        std::uint64_t count = 0;
        std::array<std::uint8_t, kUserDataPrefixBytes> prefix{};
        std::size_t prefix_size = 0;
    };

    std::vector<Kind> kinds_;
    std::uint64_t other_ = 0;
};

// 一个窗口里的丢弃计数。纯数据，单测直接构造；ETW 回调只负责往里加。
struct EtwWindowCounters {
    std::uint64_t ignored_events = 0;
    std::uint64_t unknown_versions = 0;
    std::uint64_t short_payloads = 0;
    UnfilteredEventTally unfiltered;
};

// 窗口 note：丢弃计数一行 + 未通过过滤事件的组合明细（多行）。诊断要全，因为终端输出由
// run.py 的日志自己捕获，操作者不会去复制它。
std::string etw_window_note(const EtwWindowCounters &counters, std::uint64_t outside_snapshot);

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
