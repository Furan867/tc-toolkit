// `save`：把当前聚合结果落成一个 run 目录（run.ini + samples.tsv），
// 并调用 config 里的外部 freq 命令取读数（spec §10 run.ini 键表）。
#pragma once

#include "config.hh"
#include "samples.hh"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hotspot {

struct FreqReading {
    std::optional<double> hz;
    std::string at; // ISO 时刻
    std::string error;
};

struct RunRecord {
    std::filesystem::path artifact_path;
    std::string artifact_sha256;
    std::uint64_t artifact_file_len = 0;
    std::uint64_t artifact_code_len = 0;
    std::optional<std::string> artifact_num;
    Backend backend = Backend::etw;
    std::vector<std::uintptr_t> bases;
    std::size_t feature_off = 0;
    int window_sec = 0;
    int interval_ms = 0;
    std::uint64_t sample_count = 0;
    // 暂停开销（suspend 后端才有真数，etw 零暂停恒 0）。旧 run.ini 没有这三个键，
    // 读的一侧按缺失处理（不能当 0 读：0 的意思是「没暂停」）。
    std::uint64_t pause_total_us = 0;
    std::uint64_t pause_avg_us = 0;
    std::uint64_t pause_max_us = 0;
    std::vector<std::uint32_t> tids;
    FreqReading freq_start;
    FreqReading freq_end;
    double freq_interval_sec = 1.0;
    std::string created_at;
};

std::string iso_timestamp_now();
// runs/<ts>_<产物名>_t<窗口秒>
std::string make_run_dir_name(const std::filesystem::path &artifact_path,
                              const std::string &timestamp, int window_sec);

std::string render_run_ini(const RunRecord &record);

struct CommandResult {
    int exit_code = 0;
    std::string output;
    bool timed_out = false;
};

// 跑一条外部命令（CreateProcess，不经过 shell），超时就终止并标 timed_out。
std::expected<CommandResult, std::string> run_command(std::string_view command_line,
                                                     std::chrono::milliseconds timeout);

// read_freq.py 的输出里取 sim_speed 的 Hz；读不到（游戏没跑/偏移过时）返回 nullopt。
std::optional<double> parse_sim_speed_hz(std::string_view output);

// 建 run 目录、写 samples.tsv 与 run.ini。目录已存在则报错（同名 run = 撞了）。
std::expected<std::filesystem::path, std::string>
write_run_dir(const std::filesystem::path &run_dir, const RunRecord &record,
              const SampleTable &samples);

} // namespace hotspot
