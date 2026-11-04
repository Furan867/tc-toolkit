#include "run_dir.hh"

#include <windows.h>

#include <array>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace hotspot {
namespace {

std::string json_string(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            // 反斜杠路径在 JSON 字符串里是转义，必须成对写出（上面已处理）。
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string json_number(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::wstring utf8_to_wide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

} // namespace

std::string iso_timestamp_now()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::string make_run_dir_name(const std::filesystem::path &artifact_path,
                              const std::string &timestamp, int window_sec)
{
    std::string stamp = timestamp;
    for (char &c : stamp) {
        if (c == ':' || c == '-' || c == 'T') {
            c = c == 'T' ? '_' : (c == ':' ? '-' : '-');
        }
    }
    std::string name = stamp + "_" + artifact_path.stem().string() + "_t" +
                       std::to_string(window_sec);
    return name;
}

std::string render_run_ini(const RunRecord &record)
{
    std::ostringstream out;
    out << "# consumer 初写；analysis 只回写 [buckets]（spec §10）\n";
    out << "\n[run]\n";
    out << "schema = 1\n";
    out << "created_at = " << json_string(record.created_at) << "\n";
    out << "backend = " << json_string(backend_name(record.backend)) << "\n";

    out << "\n[artifact]\n";
    out << "path = " << json_string(record.artifact_path.generic_string()) << "\n";
    out << "sha256 = " << json_string(record.artifact_sha256) << "\n";
    out << "file_len = " << record.artifact_file_len << "\n";
    out << "code_len = " << record.artifact_code_len << "\n";
    // 产物名里没编号就不写这个键：null 不进 INI，读的一方按缺失处理。
    if (record.artifact_num.has_value()) {
        out << "num = " << json_string(*record.artifact_num) << "\n";
    }

    out << "\n[locate]\n";
    out << "bases = [";
    for (std::size_t i = 0; i < record.bases.size(); ++i) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "0x%llx",
                      static_cast<unsigned long long>(record.bases[i]));
        out << (i == 0 ? "" : ", ") << json_string(buffer);
    }
    out << "]\n";
    out << "feature_off = " << record.feature_off << "\n";

    out << "\n[window]\n";
    out << "sec = " << record.window_sec << "\n";
    out << "interval_ms = " << record.interval_ms << "\n";
    out << "sample_count = " << record.sample_count << "\n";
    out << "avg_pause_us = " << record.pause_avg_us << "\n";
    out << "max_pause_us = " << record.pause_max_us << "\n";
    out << "total_pause_us = " << record.pause_total_us << "\n";
    out << "tids = [";
    for (std::size_t i = 0; i < record.tids.size(); ++i) {
        out << (i == 0 ? "" : ", ") << record.tids[i];
    }
    out << "]\n";

    out << "\n[freq]\n";
    out << "interval_sec = " << json_number(record.freq_interval_sec) << "\n";
    if (record.freq_start.hz.has_value()) {
        out << "start_hz = " << json_number(*record.freq_start.hz) << "\n";
        out << "start_at = " << json_string(record.freq_start.at) << "\n";
    } else {
        out << "start_hz = 0.0\n";
        out << "start_error = " << json_string(record.freq_start.error) << "\n";
    }
    if (record.freq_end.hz.has_value()) {
        out << "end_hz = " << json_number(*record.freq_end.hz) << "\n";
        out << "end_at = " << json_string(record.freq_end.at) << "\n";
    } else {
        out << "end_hz = 0.0\n";
        out << "end_error = " << json_string(record.freq_end.error) << "\n";
    }

    out << "\n# 三桶份额（analysis 回写，consumer 不填）\n[buckets]\n";
    return out.str();
}

std::expected<CommandResult, std::string> run_command(std::string_view command_line,
                                                     std::chrono::milliseconds timeout)
{
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &attributes, 0) == FALSE) {
        return std::unexpected("CreatePipe 失败");
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION process{};
    std::wstring command = utf8_to_wide(command_line);
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(write_end);
    if (started == FALSE) {
        CloseHandle(read_end);
        return std::unexpected("启动外部命令失败：" + std::string(command_line));
    }

    CommandResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer{};
    bool running = true;
    while (running) {
        DWORD available = 0;
        if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            break;
        }
        if (available > 0) {
            DWORD got = 0;
            if (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                         nullptr) == FALSE) {
                break;
            }
            result.output.append(buffer.data(), got);
            continue;
        }
        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
            running = false;
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(process.hProcess, 1);
            result.timed_out = true;
            running = false;
        }
    }
    // 收尾：进程退出后管道里可能还有尾巴。
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) == FALSE ||
            available == 0) {
            break;
        }
        DWORD got = 0;
        if (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) ==
            FALSE) {
            break;
        }
        result.output.append(buffer.data(), got);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(read_end);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

std::optional<double> parse_sim_speed_hz(std::string_view output)
{
    // read_freq.py 的读数行：`sim_speed   100.000 MHz   (100000000 Hz)`
    const std::size_t marker = output.find("sim_speed");
    if (marker == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = output.substr(marker + std::string_view("sim_speed").size());
    const std::size_t open = rest.find('(');
    const std::size_t close = rest.find("Hz)", open == std::string_view::npos ? 0 : open);
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return std::nullopt;
    }
    std::string digits;
    for (const char c : rest.substr(open + 1, close - open - 1)) {
        if ((c >= '0' && c <= '9') || c == '.') {
            digits.push_back(c);
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    try {
        const double hz = std::stod(digits);
        if (hz <= 0.0) {
            return std::nullopt;
        }
        return hz;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::expected<std::filesystem::path, std::string>
write_run_dir(const std::filesystem::path &run_dir, const RunRecord &record,
              const SampleTable &samples)
{
    std::error_code error;
    if (std::filesystem::exists(run_dir, error)) {
        return std::unexpected("run 目录已存在，拒绝覆盖：" + run_dir.string());
    }
    std::filesystem::create_directories(run_dir, error);
    if (error) {
        return std::unexpected("建 run 目录失败：" + run_dir.string() + "：" + error.message());
    }

    {
        std::ofstream samples_out(run_dir / "samples.tsv", std::ios::binary);
        if (!samples_out) {
            return std::unexpected("写 samples.tsv 失败：" + (run_dir / "samples.tsv").string());
        }
        samples.write_tsv(samples_out);
        samples_out.flush();
        if (!samples_out) {
            return std::unexpected("写 samples.tsv 失败：" + (run_dir / "samples.tsv").string());
        }
    }
    {
        std::ofstream ini_out(run_dir / "run.ini", std::ios::binary);
        if (!ini_out) {
            return std::unexpected("写 run.ini 失败：" + (run_dir / "run.ini").string());
        }
        ini_out << render_run_ini(record);
        ini_out.flush();
        if (!ini_out) {
            return std::unexpected("写 run.ini 失败：" + (run_dir / "run.ini").string());
        }
    }
    return run_dir;
}

} // namespace hotspot
