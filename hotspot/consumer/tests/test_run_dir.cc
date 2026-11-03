// run 目录：run.toml 键（spec §10）与 samples.tsv 落盘。
#include "run_dir.hh"
#include "test_support.hh"

#include <filesystem>
#include <fstream>
#include <sstream>

using hotspot::Bucket;
using hotspot::make_run_dir_name;
using hotspot::RunRecord;
using hotspot::SampleTable;
using hotspot::write_run_dir;

namespace {

std::filesystem::path temp_root(const std::string &tag)
{
    const auto dir = std::filesystem::temp_directory_path() / ("hotspot_run_" + tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

RunRecord sample_record()
{
    RunRecord record;
    record.artifact_path = "work/hook/hook_out_013.bin";
    record.artifact_sha256 = std::string(64, 'a');
    record.artifact_file_len = 4096;
    record.artifact_code_len = 4088;
    record.artifact_num = std::string{"013"};
    record.bases = {0x10000000, 0x20000000};
    record.feature_off = 0x400;
    record.window_sec = 10;
    record.interval_ms = 1;
    record.sample_count = 3;
    record.tids = {11, 12};
    record.freq_start.hz = 1.0e8;
    record.freq_start.at = "2026-11-05T10:00:00";
    record.freq_end.hz = 1.01e8;
    record.freq_end.at = "2026-11-05T10:00:10";
    record.created_at = "2026-11-05T10:00:10";
    return record;
}

} // namespace

TEST_CASE(run_toml_has_the_spec_keys)
{
    const RunRecord record = sample_record();
    const std::string text = hotspot::render_run_toml(record);
    for (const char *key : {"schema = 1",
                            "[artifact]",
                            "path = \"work/hook/hook_out_013.bin\"",
                            "sha256 = ",
                            "file_len = 4096",
                            "code_len = 4088",
                            "num = \"013\"",
                            "[locate]",
                            "bases = [\"0x10000000\", \"0x20000000\"]",
                            "feature_off = 1024",
                            "[window]",
                            "sec = 10",
                            "interval_ms = 1",
                            "sample_count = 3",
                            "tids = [11, 12]",
                            "[freq]",
                            "interval_sec = 1",
                            "start_hz = 1e+08",
                            "end_hz = 1.01e+08",
                            "start_at = \"2026-11-05T10:00:00\"",
                            "[buckets]"}) {
        if (text.find(key) == std::string::npos) {
            ::testing_support::fail(__FILE__, __LINE__, std::string("run.toml 缺少 ") + key);
        }
    }
}

TEST_CASE(run_toml_records_missing_freq_reading_as_error)
{
    RunRecord record = sample_record();
    record.freq_start = hotspot::FreqReading{};
    record.freq_start.error = "freq 命令超时（5.0 s）";
    const std::string text = hotspot::render_run_toml(record);
    // 读数拿不到时留 0.0 + 原因：0 不会被当成真实频率，原因能直接看出是超时。
    CHECK(text.find("start_hz = 0.0") != std::string::npos);
    CHECK(text.find("start_error = \"freq 命令超时（5.0 s）\"") != std::string::npos);
}

TEST_CASE(write_run_dir_writes_samples_and_toml)
{
    const auto root = temp_root("write");
    const auto run_dir = root / "runs" / "20261105_100010_hook_out_013_t10";
    SampleTable table;
    std::vector<hotspot::BlobCopy> copies{{0x10000000, 0x1000}};
    std::vector<hotspot::ExecRegion> regions{{0x20000000, 0x4000, false}};
    table.add_sample(0x10000010, 11, copies, regions);
    table.add_sample(0x20000040, 12, copies, regions);

    const auto result = write_run_dir(run_dir, sample_record(), table);
    CHECK(result.has_value());
    CHECK(std::filesystem::is_regular_file(run_dir / "samples.tsv"));
    CHECK(std::filesystem::is_regular_file(run_dir / "run.toml"));

    std::string written;
    {
        // 读句柄要在这里关掉：下面 remove_all 时 Windows 会因句柄未关而失败。
        std::ifstream samples_in(run_dir / "samples.tsv", std::ios::binary);
        std::ostringstream buffer;
        buffer << samples_in.rdbuf();
        written = buffer.str();
    }
    CHECK_EQ(written, table.to_tsv());
    CHECK_EQ(table.count_in(Bucket::blob), std::uint64_t{1});
    CHECK_EQ(table.count_in(Bucket::other_exec), std::uint64_t{1});

    // 同名 run 目录不覆盖：撞名等于两条 run 抢同一个目录。
    const auto again = write_run_dir(run_dir, sample_record(), table);
    CHECK(!again.has_value());
    CHECK(again.error().find("已存在") != std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE(run_dir_name_is_timestamp_artifact_and_window)
{
    const std::string name =
        make_run_dir_name("work/hook/hook_out_013.bin", "2026-11-05T10:00:10", 10);
    // 冒号和 T 在 Windows 路径里不安全，换成 - 与 _。
    CHECK_EQ(name, std::string{"2026-11-05_10-00-10_hook_out_013_t10"});
}
