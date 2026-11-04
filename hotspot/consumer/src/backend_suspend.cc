#include "backend_suspend.hh"

#include <windows.h>
// timeapi.h 是 timeBeginPeriod/timeEndPeriod 的声明处，它依赖 windows.h 先到位。
#include <timeapi.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace hotspot {

namespace {

// 1 ms —— timeBeginPeriod 取的就是这个值。
constexpr UINT kTimerPeriodMs = 1;

// timeEndPeriod 不认「谁调的」：嵌套计数只能自己的账本记，否则内层析构会把外层想要的
// 精度一起丢掉。
struct TimerResolutionState {
    std::mutex mutex;
    int depth = 0;
};

TimerResolutionState &timer_resolution_state()
{
    static TimerResolutionState state;
    return state;
}

// 自旋的粒度上限：先按 500 µs 试，实际周期够不够 1 ms 看 run.ini 的 pause_*_us / 样本数。
constexpr std::chrono::microseconds kSpinMargin{500};

} // namespace

TimerResolutionGuard::TimerResolutionGuard()
{
    TimerResolutionState &state = timer_resolution_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.depth++ == 0) {
        // 失败（TIMERR_NOCANDO）只是精度没提升，退回原来的 15.625 ms 周期，不当作错误。
        timeBeginPeriod(kTimerPeriodMs);
    }
}

TimerResolutionGuard::~TimerResolutionGuard()
{
    TimerResolutionState &state = timer_resolution_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (--state.depth == 0) {
        timeEndPeriod(kTimerPeriodMs);
    }
}

int TimerResolutionGuard::depth()
{
    TimerResolutionState &state = timer_resolution_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.depth;
}

void wait_until(std::chrono::steady_clock::time_point target, std::chrono::microseconds spin_margin)
{
    const auto spin_start = target - spin_margin;
    if (std::chrono::steady_clock::now() < spin_start) {
        std::this_thread::sleep_until(spin_start);
    }
    while (std::chrono::steady_clock::now() < target) {
        std::this_thread::yield();
    }
}

SuspendBackend::SuspendBackend(std::uint32_t pid, std::vector<BlobCopy> copies, int probe_rounds)
    : pid_(pid)
    , copies_(std::move(copies))
    , probe_rounds_(probe_rounds > 0 ? probe_rounds : 1)
{
}

void SuspendBackend::set_copies(std::vector<BlobCopy> copies)
{
    copies_ = std::move(copies);
}

bool SuspendBackend::in_blob(std::uintptr_t ip) const
{
    for (const BlobCopy &copy : copies_) {
        if (ip >= copy.base && ip - copy.base < copy.code_size) {
            return true;
        }
    }
    return false;
}

std::expected<void, std::string> SuspendBackend::rescan_threads()
{
    auto ids = snapshot_thread_ids(pid_);
    if (!ids.has_value()) {
        return std::unexpected(ids.error());
    }
    std::vector<ThreadHandle> handles;
    std::vector<std::uint32_t> opened;
    handles.reserve(ids->size());
    opened.reserve(ids->size());
    for (const std::uint32_t id : *ids) {
        auto handle = ThreadHandle::open(id);
        if (!handle.has_value()) {
            continue; // 系统线程/受保护线程打不开：跳过，不当作"没有仿真线程"
        }
        handles.push_back(std::move(*handle));
        opened.push_back(id);
    }
    if (opened.empty()) {
        return std::unexpected("一个目标线程都打不开（权限不足？）");
    }
    thread_ids_ = std::move(opened);
    handles_ = std::move(handles);
    return {};
}

std::optional<std::pair<std::uintptr_t, std::chrono::microseconds>>
SuspendBackend::sample_once(ThreadHandle &thread)
{
    const auto before = std::chrono::steady_clock::now();
    if (SuspendThread(static_cast<HANDLE>(thread.raw())) == static_cast<DWORD>(-1)) {
        return std::nullopt;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL; // 只要 Rip
    const BOOL ok = GetThreadContext(static_cast<HANDLE>(thread.raw()), &context);
    ResumeThread(static_cast<HANDLE>(thread.raw()));
    const auto paused = std::chrono::steady_clock::now() - before;
    if (ok == FALSE) {
        return std::nullopt;
    }
    return std::make_pair(static_cast<std::uintptr_t>(context.Rip),
                          std::chrono::duration_cast<std::chrono::microseconds>(paused));
}

std::expected<std::string, std::string> SuspendBackend::prepare()
{
    if (copies_.empty()) {
        return std::unexpected("还没有定位到产物副本，先 open 或 locate");
    }
    if (auto scanned = rescan_threads(); !scanned.has_value()) {
        return std::unexpected(scanned.error());
    }

    // 粗采锁定仿真线程：命中一次就锁定。未命中说明这段时间电路没在跑（停着/在 UI），
    // 退回全线程采样并告警 —— 样本的归因没错，只是多出非仿真线程的 IMAGE 样本。
    std::unordered_map<std::uint32_t, int> hits;
    // 粗采也是 1 ms 间隔的 sleep：不提升精度的话实际周期被量化到 ~7.8 ms。
    {
        TimerResolutionGuard timer_resolution;
        for (int round = 0; round < probe_rounds_; ++round) {
            for (std::size_t i = 0; i < handles_.size(); ++i) {
                const auto sample = sample_once(handles_[i]);
                if (!sample.has_value()) {
                    continue;
                }
                max_pause_ = std::max(max_pause_, sample->second);
                if (in_blob(sample->first)) {
                    ++hits[thread_ids_[i]];
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (hits.empty()) {
        locked_ = false;
        return "粗采 " + std::to_string(probe_rounds_) + " 轮没命中产物区间（电路没在跑？）：" +
               "退回全线程采样，bucket=2 份额会偏高";
    }

    std::vector<std::uint32_t> simulation;
    for (const auto &[id, count] : hits) {
        if (count > 0) {
            simulation.push_back(id);
        }
    }
    std::sort(simulation.begin(), simulation.end());

    std::vector<ThreadHandle> handles;
    std::vector<std::uint32_t> ids;
    for (std::size_t i = 0; i < thread_ids_.size(); ++i) {
        if (std::binary_search(simulation.begin(), simulation.end(), thread_ids_[i])) {
            handles.push_back(std::move(handles_[i]));
            ids.push_back(thread_ids_[i]);
        }
    }
    handles_ = std::move(handles);
    thread_ids_ = std::move(ids);
    locked_ = true;
    return "锁定仿真线程 " + std::to_string(thread_ids_.size()) + " 个";
}

std::expected<WindowStats, std::string> SuspendBackend::run_window(std::chrono::milliseconds interval,
                                                                  std::chrono::seconds duration,
                                                                  const SampleSink &sink)
{
    if (handles_.empty()) {
        return std::unexpected("没有可用线程句柄，先 prepare");
    }

    // 开窗时轻量校验线程集合：锁定后只校验锁定线程还在 —— 不能因数量不等就 rescan 回全线程，
    // 否则 74 个非仿真线程的 IMAGE 样本会把 bucket2 稀释成 98%（真机踩过）。
    auto current = snapshot_thread_ids(pid_);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (locked_) {
        for (auto id : thread_ids_) {
            if (std::find(current->begin(), current->end(), id) == current->end()) {
                return std::unexpected("锁定的仿真线程 " + std::to_string(id) +
                                      " 已消失，请重新 locate/open");
            }
        }
    } else if (current->size() != thread_ids_.size()) {
        if (auto scanned = rescan_threads(); !scanned.has_value()) {
            return std::unexpected(scanned.error());
        }
    }

    WindowStats stats;
    stats.thread_ids = thread_ids_;
    // 暂停统计按窗口算：prepare 的粗采与上一个窗口的上界都不能带进来。
    max_pause_ = std::chrono::microseconds{0};
    std::uint64_t pause_total_us = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + duration;
    auto next_round = start;
    {
        TimerResolutionGuard timer_resolution;
        while (std::chrono::steady_clock::now() < deadline) {
            for (std::size_t i = 0; i < handles_.size(); ++i) {
                const auto sample = sample_once(handles_[i]);
                if (!sample.has_value()) {
                    continue;
                }
                pause_total_us += static_cast<std::uint64_t>(sample->second.count());
                max_pause_ = std::max(max_pause_, sample->second);
                sink(sample->first, thread_ids_[i]);
                ++stats.samples;
            }
            next_round += interval;
            // 落后（采样本身超过了间隔）就不追补也不重置：立即采下一轮，欠账由后续轮自己还上。
            // 把 next_round 重置成 now 等于把每轮周期硬拉长成「采样耗时 + 完整间隔」，
            // 10 s / 1 ms 的样本数会被一路拉低。
            wait_until(next_round, kSpinMargin);
        }
    }
    stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    stats.pause_total_us = pause_total_us;
    stats.pause_max_us = static_cast<std::uint64_t>(max_pause_.count());
    if (stats.samples > 0) {
        stats.pause_avg_us = pause_total_us / stats.samples;
    }
    if (!locked_) {
        stats.note = "全线程采样（未锁定仿真线程）";
    }
    return stats;
}

} // namespace hotspot
