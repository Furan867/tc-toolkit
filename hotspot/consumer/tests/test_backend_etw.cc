// SampledProfile 负载解码：固定字节 → (IP, TID)；未知版本与短负载一律丢弃，不猜布局。
#include "backend_etw.hh"
#include "test_support.hh"

#include <array>
#include <cstdint>

using hotspot::decode_sampled_profile;
using hotspot::ProfileDecode;
using hotspot::SampledProfile;

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
