// SampledProfile 过滤判据（PerfInfo 类 GUID + Opcode 46，Id 不参与）与负载解码：固定字节 →
// (IP, TID)；未知版本与短负载一律丢弃，不猜布局。
// 另外钉住两件纯逻辑：窗口诊断账本的文本、失败日志（路径可 env 覆盖）。
#include "backend_etw.hh"
#include "test_support.hh"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

using hotspot::decode_sampled_profile;
using hotspot::ProfileDecode;
using hotspot::SampledProfile;
using hotspot::UnfilteredEventKey;
using hotspot::UnfilteredEventTally;

namespace {

// 合成事件头：诊断路径不碰 ETW 会话，直接用事件头五元组喂账本。
UnfilteredEventKey make_key(std::uint16_t event_id, std::uint16_t opcode, std::uint8_t version,
                            std::uint16_t user_data_length)
{
    return UnfilteredEventKey{hotspot::kSampledProfileProviderGuid, event_id, opcode, version,
                              user_data_length};
}

std::uint64_t count_occurrences(const std::string &text, std::string_view needle);

// _putenv 不拷贝字符串（缓冲区要活到环境变量被换掉），所以用 static 存赋值。
void set_fail_log_override(const std::string &value)
{
    static std::string assignment;
    assignment = "HOTSPOT_ETW_FAIL_LOG=" + value;
    CHECK(_putenv(assignment.c_str()) == 0);
}

std::uint64_t count_occurrences(const std::string &text, std::string_view needle)
{
    std::uint64_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE(etw_sampled_profile_provider_guid_is_the_perfinfo_class_guid)
{
    // 出处：文档「PerfInfo class」页 [Guid("{ce1dbfb4-137e-4da6-87b0-3f59aa102cbc}")] class
    // PerfInfo，且 Event type 46 = sampled profile；「SampledProfile class」页
    // class SampledProfile : PerfInfo。会话控制 GUID（{9e814aad-…}）只给 StartTraceW 的
    // Wnode.Guid 用，不当过滤条件。
    // 判断的是 ProviderId + Opcode：真机上按 Id == 46 过滤时样本数为 0，46 落在 Opcode。
    const hotspot::EventProviderGuid &guid = hotspot::kSampledProfileProviderGuid;
    CHECK_EQ(guid.data1, std::uint32_t{0xce1dbfb4});
    CHECK_EQ(guid.data2, std::uint16_t{0x137e});
    CHECK_EQ(guid.data3, std::uint16_t{0x4da6});
    const std::array<std::uint8_t, 8> tail{0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc};
    CHECK(guid.data4 == tail);
    CHECK_EQ(hotspot::kSampledProfileOpcode, std::uint16_t{46});
}

TEST_CASE(etw_filter_needs_perfinfo_guid_and_opcode_46)
{
    using hotspot::matches_sampled_profile;
    using hotspot::SampledProfileHeader;

    const hotspot::EventProviderGuid &guid = hotspot::kSampledProfileProviderGuid;
    const std::uint16_t opcode = hotspot::kSampledProfileOpcode;

    // 接受：ProviderId 是 PerfInfo 类 GUID、Opcode 是 46。
    CHECK(matches_sampled_profile(SampledProfileHeader{guid, 0, opcode}));

    // 拒绝：GUID 对、Opcode 不对（46 以外都不是 SampledProfile）。
    CHECK(!matches_sampled_profile(SampledProfileHeader{guid, 0, 0}));

    // 拒绝：Id 字面上是 46 也不算 —— 46 在 Opcode，Id 不参与判断。
    CHECK(!matches_sampled_profile(SampledProfileHeader{guid, opcode, 0}));

    // 拒绝：Opcode 对、ProviderId 换成会话控制 GUID（{9e814aad-…}，不是事件类 GUID）。
    const hotspot::EventProviderGuid control_guid{0x9e814aad, 0x3204, 0x11d2,
                                                  {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
    CHECK(!matches_sampled_profile(SampledProfileHeader{control_guid, 0, opcode}));
}

TEST_CASE(etw_decode_v1_reads_ip32_and_tid)
{
    // spec §4.1：v1 = IP(4)+TID(4)，小端。
    const std::array<std::uint8_t, 8> payload{0x10, 0x20, 0x30, 0x40, 0x2a, 0x00, 0x00, 0x00};

    SampledProfile profile;
    CHECK(decode_sampled_profile(1, payload, profile) == ProfileDecode::ok);
    CHECK_EQ(profile.ip, std::uintptr_t{0x40302010});
    CHECK_EQ(profile.thread_id, std::uint32_t{42});
}

TEST_CASE(etw_decode_v2_reads_ip64_and_tid)
{
    // spec §4.1：v2 = IP(8)+TID(4)，后面还有别的字段但本工具不用。
    const std::array<std::uint8_t, 16> payload{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                                               0xd2, 0x04, 0x00, 0x00, 0xff, 0xee, 0xdd, 0xcc};

    SampledProfile profile;
    CHECK(decode_sampled_profile(2, payload, profile) == ProfileDecode::ok);
    CHECK_EQ(profile.ip, std::uintptr_t{0x1122334455667788});
    CHECK_EQ(profile.thread_id, std::uint32_t{1234});
}

TEST_CASE(etw_decode_unknown_version_is_dropped)
{
    const std::array<std::uint8_t, 16> payload{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                                               0xd2, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    SampledProfile profile;
    profile.ip = 0x1234;
    profile.thread_id = 7;
    CHECK(decode_sampled_profile(3, payload, profile) == ProfileDecode::unknown_version);
    // 未知版本不写 out：留着哨兵值才能证明「没猜布局」。
    CHECK_EQ(profile.ip, std::uintptr_t{0x1234});
    CHECK_EQ(profile.thread_id, std::uint32_t{7});
}

TEST_CASE(etw_decode_short_payload_is_dropped)
{
    const std::array<std::uint8_t, 11> payload{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                                               0xd2, 0x04, 0x00};

    SampledProfile profile;
    CHECK(decode_sampled_profile(1, std::array<std::uint8_t, 7>{0x10, 0x20, 0x30, 0x40, 0x2a, 0x00,
                                                                0x00},
                                 profile) == ProfileDecode::short_payload);
    CHECK(decode_sampled_profile(2, payload, profile) == ProfileDecode::short_payload);
    CHECK(decode_sampled_profile(2, std::array<std::uint8_t, 0>{}, profile) ==
          ProfileDecode::short_payload);
}

TEST_CASE(etw_fail_log_path_defaults_under_work_tmp)
{
    set_fail_log_override("");
    CHECK_EQ(hotspot::etw_fail_log_path(), std::string("work/tmp/etw_fail.log"));
}

TEST_CASE(etw_fail_log_path_follows_env_override)
{
    set_fail_log_override("work/tmp/custom/etw_fail.log");
    CHECK_EQ(hotspot::etw_fail_log_path(), std::string("work/tmp/custom/etw_fail.log"));
    set_fail_log_override("");
}

TEST_CASE(etw_fail_line_has_time_what_code_and_system_text)
{
    CHECK_EQ(hotspot::format_etw_fail_line("2026-11-05T10:00:00", "启用 SeSystemProfilePrivilege",
                                           1300, "客户端没有所需的特权。"),
             std::string("2026-11-05T10:00:00 etw：启用 SeSystemProfilePrivilege 失败"
                         "（code=1300 客户端没有所需的特权。）"));
    // 系统文本取不到时不留多余空格。
    CHECK_EQ(hotspot::format_etw_fail_line("t", "StartTraceW", 5, ""),
             std::string("t etw：StartTraceW 失败（code=5）"));
}

TEST_CASE(etw_fail_log_appends_lines_and_creates_directory)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "hotspot_etw_fail_log";
    std::filesystem::remove_all(root);
    const std::filesystem::path file = root / "nested" / "etw_fail.log";

    hotspot::append_etw_fail_log(file.string(), "first");
    hotspot::append_etw_fail_log(file.string(), "second");
    {
        std::ifstream in(file);
        CHECK(in.good());
        std::ostringstream text;
        text << in.rdbuf();
        CHECK_EQ(text.str(), std::string("first\nsecond\n"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(etw_unfiltered_tally_dedups_identical_headers)
{
    // 同一 (ProviderId, Id, Opcode, Version, Size) 只留一份，重复只加计数：上万条事件不能逐条记。
    const std::array<std::uint8_t, 4> payload{0x11, 0x22, 0x33, 0x44};
    UnfilteredEventTally tally;
    tally.record(make_key(1, 0, 0, 4), payload);
    tally.record(make_key(1, 0, 0, 4), payload);
    tally.record(make_key(1, 0, 0, 4), payload);
    tally.record(make_key(2, 0, 0, 4), payload);

    const std::string section = tally.section();
    CHECK_EQ(count_occurrences(section, "Id="), std::uint64_t{2});
    CHECK_EQ(count_occurrences(section, "\n"), std::uint64_t{2});
    CHECK(section.find("Id=1 Opcode=0 Version=0 Size=4 ×3") != std::string::npos);
    CHECK(section.find("Id=2 Opcode=0 Version=0 Size=4 ×1") != std::string::npos);
    CHECK_EQ(tally.other_events(), std::uint64_t{0});
    // 相同 GUID 前缀每种组合都要写全，不能只写第一行。
    CHECK_EQ(count_occurrences(section, "{ce1dbfb4-137e-4da6-87b0-3f59aa102cbc}"),
             std::uint64_t{2});
}

TEST_CASE(etw_unfiltered_tally_keeps_only_first_eight_kinds)
{
    // 第 9 种起只累计：日志要能看出「还有别的组合、但不展开」，不能无限长。
    UnfilteredEventTally tally;
    for (std::uint16_t id = 1; id <= 10; ++id) {
        tally.record(make_key(id, 0, 0, 0), {});
    }
    CHECK_EQ(count_occurrences(tally.section(), "Id="), std::uint64_t{8});
    CHECK_EQ(tally.other_events(), std::uint64_t{2});
    CHECK(tally.section().find("其余组合共 2 个") != std::string::npos);

    // 满了之后已有的组合照旧计数，不进「其他」。
    tally.record(make_key(1, 0, 0, 0), {});
    CHECK_EQ(tally.other_events(), std::uint64_t{2});
    CHECK(tally.section().find("Id=1 Opcode=0 Version=0 Size=0 ×2") != std::string::npos);
}

TEST_CASE(etw_unfiltered_tally_line_shows_guid_id_opcode_version_size_and_prefix)
{
    // 固定布局的十六进制：Data1 按小端写成 8 位十六进制，后面按 GUID 分组。
    const hotspot::EventProviderGuid guid{0x9e814aad, 0x3204, 0x11d2,
                                          {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
    const std::array<std::uint8_t, 20> payload{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                              0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                              0x10, 0x11, 0x12, 0x13};
    UnfilteredEventTally tally;
    tally.record(UnfilteredEventKey{guid, 46, 46, 2, 20}, payload);

    // 超过 16 字节的部分不打印：Size 已经写了全长，前缀只为看布局。
    CHECK_EQ(tally.section(),
             std::string("未通过 SampledProfile 过滤的事件明细"
                         "（按 ProviderId/Id/Opcode/Version/Size 去重，列出前 8 种）："
                         "\n  {9e814aad-3204-11d2-9a82-006008a86939} Id=46 Opcode=46 Version=2"
                         " Size=20 ×1"
                         " UserData=000102030405060708090a0b0c0d0e0f"));
}

TEST_CASE(etw_unfiltered_tally_omits_userdata_when_payload_is_empty)
{
    UnfilteredEventTally tally;
    CHECK(tally.section().empty());
    tally.record(make_key(7, 0, 1, 0), {});
    CHECK(tally.section().find("UserData=") == std::string::npos);
    CHECK(tally.section().find("Id=7 Opcode=0 Version=1 Size=0 ×1") != std::string::npos);
}

TEST_CASE(etw_window_note_keeps_counts_and_appends_detail_lines)
{
    // note 是操作者唯一能看到的东西（run.py 捕 stdout），诊断必须全部写进去。
    hotspot::EtwWindowCounters counters;
    counters.ignored_events = 2;
    counters.unknown_versions = 3;
    counters.short_payloads = 4;
    counters.unfiltered.record(make_key(46, 46, 9, 8), std::array<std::uint8_t, 1>{0xab});
    counters.unfiltered.record(make_key(46, 46, 9, 8), std::array<std::uint8_t, 1>{0xab});

    const std::string note = hotspot::etw_window_note(counters, 5);
    CHECK_EQ(note.substr(0, note.find('\n')),
             std::string("非 SampledProfile 事件 2 个（已丢弃）；未知负载版本 3 个（已丢弃）；"
                         "负载过短 4 个（已丢弃）；TID 不在首尾快照内 5 个"
                         "（窗口中途新开线程，已丢弃）；"
                         "ETW 实时投递有 ~1 s flush 延迟，窗口末尾的样本可能还没收到（未计入）"));
    CHECK(note.find("\n  {ce1dbfb4-137e-4da6-87b0-3f59aa102cbc} Id=46 Opcode=46 Version=9"
                    " Size=8 ×2 UserData=ab") != std::string::npos);

    // 没有未过滤事件时不加明细节，避免每窗口都多两行噪声。
    const hotspot::EtwWindowCounters clean;
    const std::string clean_note = hotspot::etw_window_note(clean, 0);
    CHECK_EQ(count_occurrences(clean_note, "\n"), std::uint64_t{0});
}
