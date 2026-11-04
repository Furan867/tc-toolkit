// SampledProfile 负载解码：固定字节 → (IP, TID)；未知版本与短负载一律丢弃，不猜布局。
// 另外钉住两件纯逻辑：事件过滤用的 provider GUID 与失败日志（路径可 env 覆盖）。
#include "backend_etw.hh"
#include "test_support.hh"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using hotspot::decode_sampled_profile;
using hotspot::ProfileDecode;
using hotspot::SampledProfile;

namespace {

// _putenv 不拷贝字符串（缓冲区要活到环境变量被换掉），所以用 static 存赋值。
void set_fail_log_override(const std::string &value)
{
    static std::string assignment;
    assignment = "HOTSPOT_ETW_FAIL_LOG=" + value;
    CHECK(_putenv(assignment.c_str()) == 0);
}

} // namespace

TEST_CASE(etw_sampled_profile_filter_is_the_perfinfo_class_guid)
{
    // 出处：文档「PerfInfo class」页 [Guid("{ce1dbfb4-137e-4da6-87b0-3f59aa102cbc}")] class
    // PerfInfo，且 Event type 46 = sampled profile；「SampledProfile class」页
    // class SampledProfile : PerfInfo。会话控制 GUID（{9e814aad-…}）只给 StartTraceW 的
    // Wnode.Guid 用，不当过滤条件。
    const hotspot::EventProviderGuid &guid = hotspot::kSampledProfileProviderGuid;
    CHECK_EQ(guid.data1, std::uint32_t{0xce1dbfb4});
    CHECK_EQ(guid.data2, std::uint16_t{0x137e});
    CHECK_EQ(guid.data3, std::uint16_t{0x4da6});
    const std::array<std::uint8_t, 8> tail{0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc};
    CHECK(guid.data4 == tail);
    CHECK_EQ(hotspot::kSampledProfileEventId, std::uint16_t{46});
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
