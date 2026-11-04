#include "backend_etw.hh"

#include "run_dir.hh"
#include "win_process.hh"

// evntrace.h/evntcons.h 要求先有 windows.h；Win7 是这批 API（实时模式 + 事件回调）的下限。
#ifndef WINVER
#define WINVER 0x0602
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hotspot {

std::string etw_fail_log_path()
{
    const char *override_path = std::getenv("HOTSPOT_ETW_FAIL_LOG");
    if (override_path != nullptr && *override_path != '\0') {
        return override_path;
    }
    return "work/tmp/etw_fail.log";
}

std::string format_etw_fail_line(std::string_view timestamp, std::string_view what,
                                 unsigned long code, std::string_view text)
{
    std::string line;
    line.reserve(timestamp.size() + what.size() + text.size() + 32);
    line.append(timestamp)
        .append(" etw：")
        .append(what)
        .append(" 失败（code=")
        .append(std::to_string(code));
    if (!text.empty()) {
        line.append(" ").append(text);
    }
    line.append("）");
    return line;
}

void append_etw_fail_log(const std::string &path, std::string_view line)
{
    const std::filesystem::path file{path};
    if (file.has_parent_path()) {
        // 目录已有/建不了都往下走：写不进去就只当日志丢了，不能挡住 abort。
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    std::ofstream out(file, std::ios::app | std::ios::binary);
    if (!out) {
        return;
    }
    out << line << '\n';
}

namespace {

// StartTraceW 的 EVENT_TRACE_PROPERTIES.Wnode.Guid：NT Kernel Logger 这个**会话**的控制 GUID
// （文档「NT Kernel Logger Constants」：SystemTraceControlGuid）。事件过滤不用它——内核事件头里
// 的 ProviderId 是逐个事件类（MOF 类）的 GUID，SampledProfile 属 PerfInfo 类，见
// kSampledProfileProviderGuid。
constexpr GUID kSystemTraceControlGuid = {0x9e814aad,
                                          0x3204,
                                          0x11d2,
                                          {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};

EventProviderGuid from_guid(const GUID &source)
{
    return EventProviderGuid{source.Data1,
                             source.Data2,
                             source.Data3,
                             {source.Data4[0], source.Data4[1], source.Data4[2], source.Data4[3],
                              source.Data4[4], source.Data4[5], source.Data4[6], source.Data4[7]}};
}

UnfilteredEventKey unfiltered_key(const EVENT_RECORD &record)
{
    return UnfilteredEventKey{from_guid(record.EventHeader.ProviderId),
                              record.EventHeader.EventDescriptor.Id,
                              record.EventHeader.EventDescriptor.Opcode,
                              record.EventHeader.EventDescriptor.Version,
                              record.UserDataLength};
}

// 手写 hex（不用格式化串）：GUID 文本里 Data1/Data2/Data3 是数值高位在前的 8/4/4 位十六进制，
// 而 Data4 是逐字节，位数全固定；格式化串在这里最容易与实参类型错配。
void append_hex(std::string &out, std::uint64_t value, int digits)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i) {
        out.push_back(kHexDigits[(value >> (i * 4)) & 0xF]);
    }
}

// 连字符分组按 SDK 的 GUID 文本形式（也与 ETW 日志/注册表里看到的一字不差），
// 拿到日志的人可以直接拿去搜。
std::string format_provider_id(const EventProviderGuid &guid)
{
    std::string out = "{";
    append_hex(out, guid.data1, 8);
    out.push_back('-');
    append_hex(out, guid.data2, 4);
    out.push_back('-');
    append_hex(out, guid.data3, 4);
    out.push_back('-');
    append_hex(out, guid.data4[0], 2);
    append_hex(out, guid.data4[1], 2);
    out.push_back('-');
    for (std::size_t i = 2; i < guid.data4.size(); ++i) {
        append_hex(out, guid.data4[i], 2);
    }
    out.push_back('}');
    return out;
}

std::string format_hex_bytes(std::span<const std::uint8_t> bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        append_hex(out, byte, 2);
    }
    return out;
}

std::uint32_t read_le32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_le64(const std::uint8_t *bytes)
{
    return static_cast<std::uint64_t>(read_le32(bytes)) |
           (static_cast<std::uint64_t>(read_le32(bytes + 4)) << 32);
}

// StartTraceW/OpenTraceW 的日志器名是 LPWSTR（非 const），字面量得给一份可写副本。
wchar_t *kernel_logger_name()
{
    static wchar_t name[] = KERNEL_LOGGER_NAMEW;
    return name;
}

std::string win32_error_text(DWORD code)
{
    wchar_t *text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<wchar_t *>(&text), 0,
                                        nullptr);
    if (length == 0 || text == nullptr) {
        return {};
    }
    std::string narrow;
    const int narrow_length = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr,
                                                 0, nullptr, nullptr);
    if (narrow_length > 0) {
        narrow.resize(static_cast<std::size_t>(narrow_length));
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), narrow.data(), narrow_length,
                            nullptr, nullptr);
    }
    LocalFree(text);
    while (!narrow.empty() && (narrow.back() == '\r' || narrow.back() == '\n')) {
        narrow.pop_back();
    }
    return narrow;
}

// 失败直接终止（spec §4.1：开发期 assert，不做自动回退）——但错误码必须带系统文本，
// 光看 87 分不清是「参数不对」还是「环境不对」。abort 之前先落一行日志：崩溃现场只有一次。
[[noreturn]] void fail_fast(const std::string &what, DWORD code)
{
    const std::string text = win32_error_text(code);
    std::fprintf(stderr, "✗ etw：%s 失败（code=%lu%s%s）\n", what.c_str(),
                 static_cast<unsigned long>(code), text.empty() ? "" : " ", text.c_str());
    std::fflush(stderr);
    append_etw_fail_log(etw_fail_log_path(),
                        format_etw_fail_line(iso_timestamp_now(), what, code, text));
    std::abort();
}

void warn(const std::string &what, DWORD code)
{
    const std::string text = win32_error_text(code);
    std::fprintf(stderr, "⚠ etw：%s 失败（code=%lu%s%s）\n", what.c_str(),
                 static_cast<unsigned long>(code), text.empty() ? "" : " ", text.c_str());
}

// 开内核会话时最常见的两个失败要给出可直接照做的提示，否则只有 code 5/183 看不出怎么办。
std::string trace_start_hint(ULONG status)
{
    if (status == ERROR_ACCESS_DENIED) {
        return "（需要管理员终端：SeSystemProfilePrivilege）";
    }
    if (status == ERROR_ALREADY_EXISTS) {
        return "（NT Kernel Logger 已被占用，先 `logman stop \"NT Kernel Logger\" -ets`）";
    }
    return {};
}

// SeSystemProfilePrivilege 默认是「已禁用」的，不显式启用的话 StartTraceW 开 NT Kernel Logger
// 会拿到 ERROR_ACCESS_DENIED。AdjustTokenPrivileges 在特权没分配时也返回 TRUE，真正的结论在
// GetLastError() == ERROR_NOT_ALL_ASSIGNED（非提升进程就在这里停住，别再往下走）。
void enable_system_profile_privilege()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == 0) {
        fail_fast("OpenProcessToken", GetLastError());
    }

    LUID luid{};
    // 不能用 SE_SYSTEM_PROFILE_NAME：它是 TEXT()，本项目不定义 UNICODE，展开成窄字符对不上 W 版接口。
    if (LookupPrivilegeValueW(nullptr, L"SeSystemProfilePrivilege", &luid) == 0) {
        const DWORD code = GetLastError();
        CloseHandle(token);
        fail_fast("LookupPrivilegeValueW(SeSystemProfilePrivilege)", code);
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) == 0) {
        const DWORD code = GetLastError();
        CloseHandle(token);
        fail_fast("AdjustTokenPrivileges(SeSystemProfilePrivilege)", code);
    }
    // 必须在 CloseHandle 之前读：后面的调用会把 last error 覆盖掉。
    const DWORD status = GetLastError();
    CloseHandle(token);
    if (status != ERROR_SUCCESS) {
        fail_fast("启用 SeSystemProfilePrivilege", status);
    }
}

// EVENT_TRACE_PROPERTIES 后面必须跟日志器名，且 Wnode.BufferSize 要覆盖整块；
// 少给尺寸 StartTrace 返回 87。
std::vector<std::byte> make_properties_buffer()
{
    std::vector<std::byte> buffer(sizeof(EVENT_TRACE_PROPERTIES) + 2 * sizeof(KERNEL_LOGGER_NAMEW));
    auto *properties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    properties->LoggerNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES));
    return buffer;
}

} // namespace

bool matches_sampled_profile(const SampledProfileHeader &header)
{
    return header.provider == kSampledProfileProviderGuid && header.opcode == kSampledProfileOpcode;
}

ProfileDecode decode_sampled_profile(std::uint8_t version, std::span<const std::uint8_t> user_data,
                                    SampledProfile &out)
{
    switch (version) {
    case 1:
        if (user_data.size() < 4 + 4) {
            return ProfileDecode::short_payload;
        }
        out.ip = read_le32(user_data.data());
        out.thread_id = read_le32(user_data.data() + 4);
        return ProfileDecode::ok;
    case 2:
        if (user_data.size() < 8 + 4) {
            return ProfileDecode::short_payload;
        }
        out.ip = static_cast<std::uintptr_t>(read_le64(user_data.data()));
        out.thread_id = read_le32(user_data.data() + 8);
        return ProfileDecode::ok;
    default:
        return ProfileDecode::unknown_version;
    }
}

void UnfilteredEventTally::record(const UnfilteredEventKey &key,
                                  std::span<const std::uint8_t> user_data)
{
    for (Kind &kind : kinds_) {
        if (kind.key == key) {
            ++kind.count;
            return;
        }
    }
    if (kinds_.size() >= kMaxKinds) {
        ++other_;
        return;
    }
    Kind kind;
    kind.key = key;
    kind.count = 1;
    kind.prefix_size = std::min(user_data.size(), kUserDataPrefixBytes);
    std::copy_n(user_data.begin(), kind.prefix_size, kind.prefix.begin());
    kinds_.push_back(kind);
}

std::string UnfilteredEventTally::section() const
{
    if (kinds_.empty()) {
        return {};
    }
    std::ostringstream out;
    out << "未通过 SampledProfile 过滤的事件明细（按 ProviderId/Id/Opcode/Version/Size 去重，"
        << "列出前 " << kMaxKinds << " 种）：";
    for (const Kind &kind : kinds_) {
        out << "\n  " << format_provider_id(kind.key.provider) << " Id=" << kind.key.event_id
            << " Opcode=" << kind.key.opcode
            << " Version=" << static_cast<unsigned>(kind.key.version)
            << " Size=" << kind.key.user_data_length << " ×" << kind.count;
        if (kind.prefix_size != 0) {
            const std::span<const std::uint8_t> prefix{kind.prefix.data(), kind.prefix_size};
            out << " UserData=" << format_hex_bytes(prefix);
        }
    }
    if (other_ != 0) {
        out << "\n  其余组合共 " << other_ << " 个（只展开前 " << kMaxKinds << " 种）";
    }
    return out.str();
}

std::string etw_window_note(const EtwWindowCounters &counters, std::uint64_t outside_snapshot)
{
    std::vector<std::string> parts;
    if (counters.ignored_events != 0) {
        parts.push_back("非 SampledProfile 事件 " + std::to_string(counters.ignored_events) +
                        " 个（已丢弃）");
    }
    if (counters.unknown_versions != 0) {
        parts.push_back("未知负载版本 " + std::to_string(counters.unknown_versions) +
                        " 个（已丢弃）");
    }
    if (counters.short_payloads != 0) {
        parts.push_back("负载过短 " + std::to_string(counters.short_payloads) + " 个（已丢弃）");
    }
    if (outside_snapshot != 0) {
        parts.push_back("TID 不在首尾快照内 " + std::to_string(outside_snapshot) +
                        " 个（窗口中途新开线程，已丢弃）");
    }
    // 实时投递靠 FlushTimer（本会话给 1 s）把内核缓冲区推过来：窗口末尾不到 1 s 的样本
    // 可能还在缓冲区里，窗口一收就丢。真机量到影响后再决定要不要加 drain 尾巴。
    parts.push_back("ETW 实时投递有 ~1 s flush 延迟，窗口末尾的样本可能还没收到（未计入）");

    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out << (i == 0 ? "" : "；") << parts[i];
    }
    // 明细单独成行：一种组合一行，才能一眼看出真实 GUID / Id / Opcode / Version / Size。
    const std::string detail = counters.unfiltered.section();
    if (!detail.empty()) {
        out << "\n" << detail;
    }
    return out.str();
}

// 内核日志会话的全部状态：句柄、消费线程、窗口内收到的样本。
struct EtwSession {
    struct WindowData {
        std::vector<SampledProfile> pending;
        EtwWindowCounters counters;
    };

    ~EtwSession()
    {
        stop();
    }

    void start(int interval_ms);
    void stop();
    void ensure_alive();
    void begin_window();
    WindowData end_window();

    static void WINAPI on_event(PEVENT_RECORD record);
    void handle_event(const EVENT_RECORD &record);

    TRACEHANDLE trace_handle = 0;
    TRACEHANDLE consumer_handle = INVALID_PROCESSTRACE_HANDLE;
    std::thread consumer_thread;
    ULONG process_trace_result = ERROR_SUCCESS;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> exited_before_stop{false};

    std::mutex mutex;
    bool window_open = false;
    std::vector<SampledProfile> pending;
    EtwWindowCounters counts;
};

void EtwSession::start(int interval_ms)
{
    // 先启特权再开会话：少了它 StartTraceW 只会给一个 code=5，看不出是权限没启用还是没分配。
    enable_system_profile_privilege();

    std::vector<std::byte> buffer = make_properties_buffer();
    auto *properties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer.data());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1; // QPC 时间戳
    properties->Wnode.Guid = kSystemTraceControlGuid;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->FlushTimer = 1;
    properties->EnableFlags = EVENT_TRACE_FLAG_PROFILE;
    std::memcpy(buffer.data() + properties->LoggerNameOffset, KERNEL_LOGGER_NAMEW,
                sizeof(KERNEL_LOGGER_NAMEW));

    ULONG status = StartTraceW(&trace_handle, kernel_logger_name(), properties);
    if (status != ERROR_SUCCESS) {
        fail_fast("StartTraceW(NT Kernel Logger)" + trace_start_hint(status), status);
    }

    TRACE_PROFILE_INTERVAL interval{};
    interval.Source = 0; // ProfileTime
    interval.Interval = static_cast<ULONG>(static_cast<std::uint64_t>(interval_ms) * 10'000);
    // 文档：这个信息类的 SessionHandle 与会话无关，必须为 0；传会话句柄会得到 87。
    status = TraceSetInformation(0, TraceSampledProfileIntervalInfo, &interval,
                                sizeof(interval));
    if (status != ERROR_SUCCESS) {
        const ULONG stop_status =
            ControlTraceW(trace_handle, kernel_logger_name(), properties, EVENT_TRACE_CONTROL_STOP);
        if (stop_status != ERROR_SUCCESS) {
            warn("ControlTrace(EVENT_TRACE_CONTROL_STOP)", stop_status);
        }
        trace_handle = 0;
        fail_fast("TraceSetInformation(TraceSampledProfileIntervalInfo)", status);
    }

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = kernel_logger_name();
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = &EtwSession::on_event;
    logfile.Context = this;
    consumer_handle = OpenTraceW(&logfile);
    if (consumer_handle == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD code = GetLastError();
        const ULONG stop_status =
            ControlTraceW(trace_handle, kernel_logger_name(), properties, EVENT_TRACE_CONTROL_STOP);
        if (stop_status != ERROR_SUCCESS) {
            warn("ControlTrace(EVENT_TRACE_CONTROL_STOP)", stop_status);
        }
        trace_handle = 0;
        fail_fast("OpenTraceW", code);
    }

    consumer_thread = std::thread([this] {
        const ULONG result = ProcessTrace(&consumer_handle, 1, nullptr, nullptr);
        process_trace_result = result;
        // 没请求停就返回 = 会话自己没了（别的工具停掉内核 logger 之类），样本会静默变少。
        exited_before_stop = !stop_requested.load();
    });
}

void EtwSession::stop()
{
    if (trace_handle == 0) {
        return;
    }
    {
        const std::lock_guard lock(mutex);
        window_open = false;
    }
    stop_requested = true;
    // 顺序不能反：ControlTrace(STOP) 让阻塞在 ProcessTrace 里的消费线程返回，之后才能 join，
    // 最后才 CloseTrace（句柄在 ProcessTrace 跑着的时候关会把它打断成失败返回）。
    std::vector<std::byte> buffer = make_properties_buffer();
    auto *properties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer.data());
    const ULONG stop_status =
        ControlTraceW(trace_handle, kernel_logger_name(), properties, EVENT_TRACE_CONTROL_STOP);
    trace_handle = 0;
    if (consumer_thread.joinable()) {
        consumer_thread.join();
    }
    ULONG close_status = ERROR_SUCCESS;
    if (consumer_handle != INVALID_PROCESSTRACE_HANDLE) {
        close_status = CloseTrace(consumer_handle);
        consumer_handle = INVALID_PROCESSTRACE_HANDLE;
    }
    // 收尾失败只告警不在退出路径上崩：会话已经不用了。
    if (stop_status != ERROR_SUCCESS) {
        warn("ControlTrace(EVENT_TRACE_CONTROL_STOP)", stop_status);
    }
    if (close_status != ERROR_SUCCESS) {
        warn("CloseTrace", close_status);
    }
    ensure_alive();
}

// 消费线程自己结束了（别人停掉内核 logger 之类）：不能接着报「0 样本」，要明确终止。
void EtwSession::ensure_alive()
{
    // exited_before_stop 的写入与 process_trace_result 的写入有 happens-before（原子存储/读取）。
    if (exited_before_stop.load()) {
        fail_fast("ProcessTrace（会话自己结束了）", process_trace_result);
    }
}

void EtwSession::begin_window()
{
    const std::lock_guard lock(mutex);
    pending.clear();
    window_open = true;
}

EtwSession::WindowData EtwSession::end_window()
{
    WindowData window;
    const std::lock_guard lock(mutex);
    window_open = false;
    window.pending = std::move(pending);
    pending.clear();
    window.counters = counts;
    return window;
}

void WINAPI EtwSession::on_event(PEVENT_RECORD record)
{
    if (record == nullptr || record->UserContext == nullptr) {
        return;
    }
    static_cast<EtwSession *>(record->UserContext)->handle_event(*record);
}

// 即便过了过滤，也别把未知布局当样本：解码失败的那两种进丢弃计数，不猜字节含义。
void EtwSession::handle_event(const EVENT_RECORD &record)
{
    // 只开了 EVENT_TRACE_FLAG_PROFILE，理论上只该来 SampledProfile；来了别的就计数，
    // 不静默当成样本（别的 opcode 的负载布局不同）。
    const SampledProfileHeader header{from_guid(record.EventHeader.ProviderId),
                                      record.EventHeader.EventDescriptor.Id,
                                      record.EventHeader.EventDescriptor.Opcode};
    if (!matches_sampled_profile(header)) {
        const std::lock_guard lock(mutex);
        ++counts.ignored_events;
        // 组合明细只在前 8 种上花内存；上万条事件不会把日志灌满。
        counts.unfiltered.record(unfiltered_key(record),
                                 std::span<const std::uint8_t>{
                                     static_cast<const std::uint8_t *>(record.UserData),
                                     static_cast<std::size_t>(record.UserDataLength)});
        return;
    }

    SampledProfile profile;
    const std::span<const std::uint8_t> user_data{
        static_cast<const std::uint8_t *>(record.UserData),
        static_cast<std::size_t>(record.UserDataLength)};
    const ProfileDecode decoded =
        decode_sampled_profile(record.EventHeader.EventDescriptor.Version, user_data, profile);

    const std::lock_guard lock(mutex);
    switch (decoded) {
    case ProfileDecode::ok:
        // 窗口外的事件直接丢：窗口边界靠这里的开关定，不靠时间戳。
        if (window_open) {
            pending.push_back(profile);
        }
        return;
    case ProfileDecode::unknown_version:
        ++counts.unknown_versions;
        return;
    case ProfileDecode::short_payload:
        ++counts.short_payloads;
        return;
    }
}

EtwBackend::EtwBackend(std::uint32_t pid, int interval_ms)
    : pid_(pid)
    , interval_ms_(interval_ms)
{
}

EtwBackend::~EtwBackend() = default; // 停会话在 ~EtwSession（会话活在 unique_ptr 里）

std::expected<std::string, std::string> EtwBackend::prepare()
{
    // TraceSetInformation 的间隔单位是 100 ns，ULONG 装不下更大的值。
    if (interval_ms_ <= 0 || static_cast<std::uint64_t>(interval_ms_) * 10'000 > 0xFFFFFFFFULL) {
        return std::unexpected("etw 的间隔必须是 1..429496 ms，实际 " +
                               std::to_string(interval_ms_) + " ms");
    }
    if (session_ != nullptr) {
        return "NT Kernel Logger 会话已在运行（SampledProfile 间隔 " +
               std::to_string(interval_ms_) + " ms）";
    }
    session_ = std::make_unique<EtwSession>();
    session_->start(interval_ms_);
    return "NT Kernel Logger 会话已开，SampledProfile 间隔 " + std::to_string(interval_ms_) + " ms";
}

std::expected<WindowStats, std::string>
EtwBackend::run_window(std::chrono::milliseconds interval, std::chrono::seconds duration,
                       const SampleSink &sink)
{
    if (session_ == nullptr) {
        return std::unexpected("etw 会话还没建：先 open");
    }
    session_->ensure_alive();
    // 间隔在建会话时就钉死了；会话层换了间隔却没重建，收到的样本会是旧间隔的。
    if (interval.count() != interval_ms_) {
        return std::unexpected("窗口间隔 " + std::to_string(interval.count()) +
                               " ms 与 etw 会话间隔 " + std::to_string(interval_ms_) +
                               " ms 不一致：改间隔要重建会话");
    }

    auto start_ids = snapshot_thread_ids(pid_);
    if (!start_ids.has_value()) {
        return std::unexpected(start_ids.error());
    }
    session_->begin_window();

    const auto begin = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(duration);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    const EtwSession::WindowData window = session_->end_window();
    auto end_ids = snapshot_thread_ids(pid_);
    if (!end_ids.has_value()) {
        return std::unexpected(end_ids.error());
    }

    WindowStats stats;
    stats.elapsed = elapsed;
    stats.thread_ids = *start_ids;
    stats.thread_ids.insert(stats.thread_ids.end(), end_ids->begin(), end_ids->end());
    std::sort(stats.thread_ids.begin(), stats.thread_ids.end());
    stats.thread_ids.erase(std::unique(stats.thread_ids.begin(), stats.thread_ids.end()),
                           stats.thread_ids.end());
    // 目标进程一个线程都没有：样本会全被过滤成 0，报「0 样本」会看着像采样坏了。
    if (stats.thread_ids.empty()) {
        return std::unexpected("PID " + std::to_string(pid_) +
                               " 一个线程都没枚举到（进程已退出？）");
    }
    const std::unordered_set<std::uint32_t> allowed(stats.thread_ids.begin(),
                                                    stats.thread_ids.end());

    // 事件全系统可见：只留两个快照并集里的 TID（spec §4.1 的进程过滤）。快照外的事件
    // 属于窗口中途新开的线程，丢了也不猜归属。
    std::uint64_t outside_snapshot = 0;
    for (const SampledProfile &sample : window.pending) {
        if (!allowed.contains(sample.thread_id)) {
            ++outside_snapshot;
            continue;
        }
        sink(sample.ip, sample.thread_id);
        ++stats.samples;
    }

    stats.note = etw_window_note(window.counters, outside_snapshot);
    return stats;
}

} // namespace hotspot
