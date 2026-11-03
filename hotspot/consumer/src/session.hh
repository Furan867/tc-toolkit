// 常驻会话：识别/定位一次，多窗口复用（spec §4.6）。REPL 只是这个类的读行壳子，
// 所以「无游戏时 open 报错不崩」可以直接对 execute() 断言。
#pragma once

#include "backend_suspend.hh"
#include "commands.hh"
#include "config.hh"
#include "lock.hh"
#include "locate.hh"
#include "run_dir.hh"
#include "samples.hh"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace hotspot {

class Session {
  public:
    explicit Session(Config config);

    // 返回给操作者的一行/多行结果；失败给原因（不抛异常给 REPL）。
    std::expected<std::string, std::string> execute(std::string_view line);

    bool should_quit() const
    {
        return quit_;
    }

  private:
    std::expected<std::string, std::string> do_open(const OpenArgs &args);
    std::expected<std::string, std::string> do_window(int seconds, bool save);
    std::expected<std::string, std::string> do_locate();
    std::expected<std::string, std::string> do_save(const std::optional<std::filesystem::path> &dir);
    std::string do_status() const;

    std::expected<void, std::string> create_backend();
    std::vector<BlobCopy> blob_copies() const;
    std::expected<FreqReading, std::string> read_freq();
    std::expected<std::filesystem::path, std::string>
    write_run(const std::optional<std::filesystem::path> &dir, const FreqReading &freq_end);

    Config config_;
    GameLock lock_;
    std::optional<Artifact> artifact_;
    std::optional<LocateResult> located_;
    std::optional<std::uint32_t> pid_;
    int interval_ms_ = 1;

    std::unique_ptr<SamplerBackend> backend_;
    SuspendBackend *suspend_view_ = nullptr; // backend_ 是 suspend 时指向它

    SampleTable samples_;
    FreqReading window_start_freq_;
    std::optional<WindowStats> last_window_;
    int last_window_sec_ = 0;
    bool quit_ = false;
};

} // namespace hotspot
