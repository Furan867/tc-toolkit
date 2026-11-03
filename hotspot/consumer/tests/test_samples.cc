// 事件注入单测：合成 (IP, TID) 序列喂聚合层，断言分桶、偏移换算、多副本合并与 tsv 格式。
#include "samples.hh"
#include "test_support.hh"

#include <array>
#include <sstream>

using hotspot::BlobCopy;
using hotspot::Bucket;
using hotspot::classify;
using hotspot::ExecRegion;
using hotspot::SampleKey;
using hotspot::SampleTable;

namespace {

constexpr std::uintptr_t kCopyA = 0x10000000;
constexpr std::uintptr_t kCopyB = 0x20000000;
constexpr std::uintptr_t kOtherJit = 0x30000000;
constexpr std::uintptr_t kImage = 0x140000000;
constexpr std::size_t kCodeSize = 0x1000;

struct Fixture {
    std::vector<BlobCopy> copies{{kCopyA, kCodeSize}, {kCopyB, kCodeSize}};
    std::vector<ExecRegion> regions{{kOtherJit, 0x4000, false}, {kImage, 0x20000, true}};
};

} // namespace

TEST_CASE(classify_reports_blob_bucket_with_offset)
{
    const Fixture fixture;
    const auto key = classify(kCopyA + 0x10, 42, fixture.copies, fixture.regions);
    CHECK(key.has_value());
    CHECK(key->bucket == Bucket::blob);
    CHECK_EQ(key->blob_offset, std::uint64_t{0x10});
    CHECK_EQ(key->thread_id, std::uint32_t{42});

    // 半开区间：起点算命中，code_size 处不算。
    const auto at_base = classify(kCopyA, 42, fixture.copies, fixture.regions);
    CHECK(at_base.has_value());
    CHECK_EQ(at_base->blob_offset, std::uint64_t{0});
    const auto past_end = classify(kCopyA + kCodeSize, 42, fixture.copies, fixture.regions);
    CHECK(!past_end.has_value());
}

TEST_CASE(classify_uses_region_kind_for_other_buckets)
{
    const Fixture fixture;
    const auto jit = classify(kOtherJit + 0x100, 7, fixture.copies, fixture.regions);
    CHECK(jit.has_value());
    CHECK(jit->bucket == Bucket::other_exec);
    CHECK_EQ(jit->blob_offset, std::uint64_t{0}); // v1 只有 blob_offset，非 blob 桶没有坐标

    const auto image = classify(kImage + 0x20, 7, fixture.copies, fixture.regions);
    CHECK(image.has_value());
    CHECK(image->bucket == Bucket::image);
}

TEST_CASE(classify_drops_samples_outside_executable_regions)
{
    const Fixture fixture;
    // 栈/堆里的地址：不属于任何桶，直接丢。
    CHECK(!classify(0x9000000, 7, fixture.copies, fixture.regions).has_value());
}

TEST_CASE(classify_rejects_overlapping_copies)
{
    // 副本区间重叠说明定位错了：样本不能算到别的副本上，宁可丢弃。
    std::vector<BlobCopy> copies{{kCopyA, kCodeSize}, {kCopyA + 0x100, kCodeSize}};
    std::vector<ExecRegion> regions;
    CHECK(!classify(kCopyA + 0x200, 1, copies, regions).has_value());
}

TEST_CASE(samples_from_multiple_copies_merge_into_one_row)
{
    // 同一份产物多副本共存：按「RIP − 代码起点」合并计数，不是挑最大丢弃其余。
    Fixture fixture;
    SampleTable table;
    // 副本 A：偏移 0x10 两次、0x20 一次
    CHECK(table.add_sample(kCopyA + 0x10, 42, fixture.copies, fixture.regions));
    CHECK(table.add_sample(kCopyA + 0x20, 42, fixture.copies, fixture.regions));
    CHECK(table.add_sample(kCopyA + 0x10, 42, fixture.copies, fixture.regions));
    // 副本 B：偏移 0x10 一次（与 A 合并）、0x30 一次
    CHECK(table.add_sample(kCopyB + 0x10, 42, fixture.copies, fixture.regions));
    CHECK(table.add_sample(kCopyB + 0x30, 42, fixture.copies, fixture.regions));
    // 其他非 IMAGE 可执行区一次、IMAGE 一次、区外一次（丢弃）
    CHECK(table.add_sample(kOtherJit + 0x100, 7, fixture.copies, fixture.regions));
    CHECK(table.add_sample(kImage + 0x20, 7, fixture.copies, fixture.regions));
    CHECK(!table.add_sample(0x9000000, 7, fixture.copies, fixture.regions));

    CHECK_EQ(table.total(), std::uint64_t{7});
    CHECK_EQ(table.count_in(Bucket::blob), std::uint64_t{5});
    CHECK_EQ(table.count_in(Bucket::other_exec), std::uint64_t{1});
    CHECK_EQ(table.count_in(Bucket::image), std::uint64_t{1});

    const auto &rows = table.rows();
    CHECK_EQ(rows.size(), std::size_t{5});
    CHECK_EQ(rows.at(SampleKey{Bucket::blob, 0x10, 42}), std::uint64_t{3});
    CHECK_EQ(rows.at(SampleKey{Bucket::blob, 0x20, 42}), std::uint64_t{1});
    CHECK_EQ(rows.at(SampleKey{Bucket::blob, 0x30, 42}), std::uint64_t{1});
    CHECK_EQ(rows.at(SampleKey{Bucket::other_exec, 0, 7}), std::uint64_t{1});
    CHECK_EQ(rows.at(SampleKey{Bucket::image, 0, 7}), std::uint64_t{1});
}

TEST_CASE(tsv_header_and_row_order)
{
    Fixture fixture;
    SampleTable table;
    // 故意乱序注入：输出必须按 (bucket, blob_offset, thread_id) 升序。
    table.add_sample(kImage + 0x20, 7, fixture.copies, fixture.regions);
    table.add_sample(kCopyB + 0x30, 42, fixture.copies, fixture.regions);
    table.add_sample(kOtherJit + 0x100, 7, fixture.copies, fixture.regions);
    table.add_sample(kCopyA + 0x10, 43, fixture.copies, fixture.regions);
    table.add_sample(kCopyA + 0x10, 42, fixture.copies, fixture.regions);

    const std::string tsv = table.to_tsv();
    std::istringstream lines(tsv);
    std::string line;

    CHECK(std::getline(lines, line));
    CHECK_EQ(line, std::string{"# samples.tsv v1"});
    CHECK(std::getline(lines, line));
    CHECK_EQ(line, std::string{"bucket\tblob_offset\tthread_id\tcount"});

    // 逐行拼出期望的四个字段（不手抄整段文本：数字由上面的注入决定）。
    const std::vector<std::array<std::uint64_t, 4>> expected{
        {0, 0x10, 42, 1}, {0, 0x10, 43, 1}, {0, 0x30, 42, 1}, {1, 0, 7, 1}, {2, 0, 7, 1}};
    for (const auto &row : expected) {
        CHECK(std::getline(lines, line));
        std::istringstream fields(line);
        std::string bucket_text;
        std::string offset_text;
        std::string tid_text;
        std::string count_text;
        CHECK(std::getline(fields, bucket_text, '\t'));
        CHECK(std::getline(fields, offset_text, '\t'));
        CHECK(std::getline(fields, tid_text, '\t'));
        CHECK(std::getline(fields, count_text, '\t'));
        CHECK_EQ(std::stoull(bucket_text), row[0]);
        CHECK_EQ(std::stoull(offset_text), row[1]);
        CHECK_EQ(std::stoull(tid_text), row[2]);
        CHECK_EQ(std::stoull(count_text), row[3]);
    }
    CHECK(!std::getline(lines, line));
}
