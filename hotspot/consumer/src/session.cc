#include "session.hh"


#include <sstream>

namespace hotspot {
namespace {

std::string hex(std::uintptr_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string join_thread_ids(const std::vector<std::uint32_t> &ids)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        out << (i == 0 ? "" : ",") << ids[i];
    }
    return out.str();
}

} // namespace

Session::Session(Config config)
    : config_(std::move(config))
{
}

std::expected<std::string, std::string> Session::execute(std::string_view line)
{
    auto command = parse_command(line);
    if (!command.has_value()) {
        return std::unexpected(command.error());
    }
    switch (command->kind) {
    case Command::Kind::open:
        return do_open(command->open);
    case Command::Kind::window:
        return do_window(command->window_sec, command->window_save);
    case Command::Kind::status:
        return do_status();
    case Command::Kind::locate:
        return do_locate();
    case Command::Kind::save:
        return do_save(command->save_dir);
    case Command::Kind::quit:
        quit_ = true;
        return std::string("退出");
    }
    return std::unexpected("未实现的命令");
}

std::expected<std::string, std::string> Session::do_open(const OpenArgs &args)
{
    auto artifact = load_artifact(args.artifact);
    if (!artifact.has_value()) {
        return std::unexpected(artifact.error());
    }

    std::uint32_t pid = 0;
    if (args.pid.has_value()) {
        pid = *args.pid;
    } else {
        auto found = find_pid_by_name(config_.game_exe_name);
        if (!found.has_value()) {
            return std::unexpected(found.error());
        }
        pid = *found;
    }

    // 采样期间独占游戏（config lock.dir）。已经有锁就是同一个会话在换产物，不用重拿。
    if (!lock_.held()) {
        auto lock = GameLock::acquire(std::filesystem::path(config_.lock_dir) / "game.lock");
        if (!lock.has_value()) {
            return std::unexpected(lock.error());
        }
        lock_ = std::move(*lock);
    }

    auto located = locate_in_process(pid, *artifact);
    if (!located.has_value()) {
        return std::unexpected(located.error());
    }
    if (located->bases.empty()) {
        return std::unexpected("在 PID " + std::to_string(pid) + " 里没找到产物副本（" +
                               args.artifact.string() +
                               "）：电路没在跑，或者分析的不是正在跑的那份产物");
    }

    artifact_ = std::move(*artifact);
    located_ = std::move(*located);
    pid_ = pid;
    interval_ms_ = args.interval_ms.value_or(config_.interval_ms);
    samples_.clear();
    last_window_.reset();

    auto backend = create_backend();
    if (!backend.has_value()) {
        return std::unexpected(backend.error());
    }

    std::ostringstream out;
    out << "产物 " << args.artifact.string() << "（" << artifact_->code_size() << " 字节机器码，sha256 "
        << artifact_->sha256.substr(0, 12) << "…）\n";
    out << "PID " << pid << "，副本 " << located_->bases.size() << " 份：";
    for (std::size_t i = 0; i < located_->bases.size(); ++i) {
        out << (i == 0 ? "" : " ") << hex(located_->bases[i]);
    }
    out << "\n特征窗偏移 " << hex(located_->feature_off) << "，后端 " << backend_->name() << "，间隔 "
        << interval_ms_ << " ms";
    return out.str();
}

std::expected<void, std::string> Session::create_backend()
{
    if (!artifact_.has_value() || !located_.has_value() || !pid_.has_value()) {
        return std::unexpected("还没有 open 成功");
    }
    const auto copies = blob_copies();
    if (config_.backend == Backend::suspend) {
        suspend_view_ = nullptr;
        backend_ = std::make_unique<SuspendBackend>(*pid_, copies, config_.suspend_probe_rounds);
        suspend_view_ = static_cast<SuspendBackend *>(backend_.get());
    } else {
        return std::unexpected("etw 后端未实现（当前只有 suspend）");
    }
    auto prepared = backend_->prepare();
    if (!prepared.has_value()) {
        backend_.reset();
        suspend_view_ = nullptr;
        return std::unexpected(prepared.error());
    }
    last_window_.reset();
    return {};
}

std::vector<BlobCopy> Session::blob_copies() const
{
    std::vector<BlobCopy> copies;
    if (!located_.has_value() || !artifact_.has_value()) {
        return copies;
    }
    for (const std::uintptr_t base : located_->bases) {
        copies.push_back(BlobCopy{base, artifact_->code_size()});
    }
    return copies;
}

std::expected<std::string, std::string> Session::do_locate()
{
    if (!artifact_.has_value() || !pid_.has_value()) {
        return std::unexpected("还没有 open 成功");
    }
    auto located = locate_in_process(*pid_, *artifact_);
    if (!located.has_value()) {
        return std::unexpected(located.error());
    }
    if (located->bases.empty()) {
        return std::unexpected("这次定位没找到产物副本（电路可能已经切走/重编）");
    }
    located_ = std::move(*located);
    if (suspend_view_ != nullptr) {
        suspend_view_->set_copies(blob_copies());
        if (auto prepared = suspend_view_->prepare(); !prepared.has_value()) {
            return std::unexpected(prepared.error());
        }
    }
    std::ostringstream out;
    out << "重新定位：副本 " << located_->bases.size() << " 份";
    for (std::size_t i = 0; i < located_->bases.size(); ++i) {
        out << (i == 0 ? " " : " ") << hex(located_->bases[i]);
    }
    return out.str();
}

std::expected<std::string, std::string> Session::do_window(int seconds, bool save)
{
    if (backend_ == nullptr) {
        return std::unexpected("还没有 open 成功");
    }

    window_start_freq_ = read_freq().value_or(FreqReading{});

    const auto copies = blob_copies();
    const auto regions = located_->regions;
    const SampleSink sink = [this, &copies, &regions](std::uintptr_t ip, std::uint32_t thread_id) {
        samples_.add_sample(ip, thread_id, copies, regions);
    };
    auto stats = backend_->run_window(std::chrono::milliseconds(interval_ms_),
                                      std::chrono::seconds(seconds), sink);
    if (!stats.has_value()) {
        return std::unexpected(stats.error());
    }
    last_window_ = *stats;
    last_window_sec_ = seconds;

    std::ostringstream out;
    out << "窗口 " << seconds << " s：样本 " << stats->samples << "（bucket0 "
        << samples_.count_in(Bucket::blob) << " / bucket1 " << samples_.count_in(Bucket::other_exec)
        << " / bucket2 " << samples_.count_in(Bucket::image) << "），实际 " << stats->elapsed.count()
        << " ms";
    if (stats->max_pause.count() > 0) {
        out << "，单样本暂停上界 " << stats->max_pause.count() << " µs";
    }
    if (!stats->note.empty()) {
        out << "\n注意：" << stats->note;
    }
    if (save) {
        auto saved = write_run(std::nullopt, read_freq().value_or(FreqReading{}));
        if (!saved.has_value()) {
            return std::unexpected(saved.error());
        }
        out << "\n已落盘 " << saved->string();
    }
    return out.str();
}

std::expected<std::string, std::string> Session::do_save(
    const std::optional<std::filesystem::path> &dir)
{
    if (backend_ == nullptr) {
        return std::unexpected("还没有 open 成功");
    }
    if (samples_.total() == 0) {
        return std::unexpected("还没有样本可落盘：先跑 window");
    }
    auto saved = write_run(dir, read_freq().value_or(FreqReading{}));
    if (!saved.has_value()) {
        return std::unexpected(saved.error());
    }
    return "已落盘 " + saved->string();
}

std::expected<std::filesystem::path, std::string>
Session::write_run(const std::optional<std::filesystem::path> &dir, const FreqReading &freq_end)
{
    RunRecord record;
    record.artifact_path = artifact_->path;
    record.artifact_sha256 = artifact_->sha256;
    record.artifact_file_len = artifact_->file.size();
    record.artifact_code_len = artifact_->code_size();
    record.artifact_num = artifact_->num;
    record.backend = config_.backend;
    record.bases = located_->bases;
    record.feature_off = located_->feature_off;
    record.window_sec = last_window_sec_;
    record.interval_ms = interval_ms_;
    record.sample_count = samples_.total();
    if (last_window_.has_value()) {
        record.tids = last_window_->thread_ids;
    }
    record.freq_start = window_start_freq_;
    record.freq_end = freq_end;
    record.created_at = iso_timestamp_now();

    std::filesystem::path run_dir;
    if (dir.has_value()) {
        run_dir = *dir;
    } else {
        run_dir = std::filesystem::path(config_.workspace) / "runs" /
                  make_run_dir_name(artifact_->path, record.created_at, record.window_sec);
    }
    return write_run_dir(run_dir, record, samples_);
}

std::expected<FreqReading, std::string> Session::read_freq()
{
    FreqReading reading;
    reading.at = iso_timestamp_now();
    const auto timeout =
        std::chrono::milliseconds(static_cast<std::int64_t>(config_.freq_timeout_sec * 1000.0));
    auto result = run_command(config_.freq_command, timeout);
    if (!result.has_value()) {
        reading.error = result.error();
        return std::unexpected(reading.error);
    }
    if (result->timed_out) {
        reading.error = "freq 命令超时（" + std::to_string(config_.freq_timeout_sec) + " s）";
        return std::unexpected(reading.error);
    }
    reading.hz = parse_sim_speed_hz(result->output);
    if (!reading.hz.has_value()) {
        reading.error = result->output.empty() ? "freq 命令没有输出" : "freq 读数解析不出来";
        return std::unexpected(reading.error);
    }
    return reading;
}

std::string Session::do_status() const
{
    std::ostringstream out;
    if (backend_ == nullptr) {
        out << "会话空闲：open <artifact> [--pid N] [--interval-ms <ms>] 开始";
        return out.str();
    }
    out << "产物 " << artifact_->path.string() << "\n";
    out << "  sha256 " << artifact_->sha256 << "\n";
    out << "  机器码 " << artifact_->code_size() << " 字节（文件 " << artifact_->file.size() << "）\n";
    out << "PID " << *pid_ << "，后端 " << backend_->name() << "，间隔 " << interval_ms_ << " ms\n";
    out << "副本 " << located_->bases.size() << " 份：";
    for (std::size_t i = 0; i < located_->bases.size(); ++i) {
        out << (i == 0 ? "" : " ") << hex(located_->bases[i]);
    }
    out << "\n可执行区 " << located_->regions.size() << " 个（IMAGE "
        << std::count_if(located_->regions.begin(), located_->regions.end(),
                         [](const ExecRegion &region) { return region.image; })
        << "）\n";
    out << "样本 " << samples_.total() << "（bucket0 " << samples_.count_in(Bucket::blob)
        << " / bucket1 " << samples_.count_in(Bucket::other_exec) << " / bucket2 "
        << samples_.count_in(Bucket::image) << "）\n";
    if (suspend_view_ != nullptr) {
        out << "线程 " << suspend_view_->thread_ids().size() << " 个"
            << (suspend_view_->simulation_thread_locked() ? "（已锁定仿真线程）" : "（未锁定，全线程）")
            << "，单样本暂停上界 " << suspend_view_->max_pause().count() << " µs\n";
    } else if (last_window_.has_value()) {
        out << "窗口 TID 并集：" << join_thread_ids(last_window_->thread_ids) << "\n";
    }
    return out.str();
}

} // namespace hotspot
