// suspend 后端里不依赖目标进程的部分：定时器精度守卫的嵌套/恢复、节拍等待的"不早返回"。
// 真实周期是否从 ~7.8 ms 降到 ~1 ms 只能真机跑窗口看，单测只断言不变量（不断言上界）。
#include "backend_suspend.hh"
#include "test_support.hh"

#include <stdexcept>

using hotspot::TimerResolutionGuard;
using hotspot::wait_until;

TEST_CASE(timer_guard_nests_and_restores)
{
    CHECK_EQ(TimerResolutionGuard::depth(), 0);
    {
        TimerResolutionGuard outer;
        CHECK_EQ(TimerResolutionGuard::depth(), 1);
        {
            TimerResolutionGuard inner;
            CHECK_EQ(TimerResolutionGuard::depth(), 2);
        }
        CHECK_EQ(TimerResolutionGuard::depth(), 1);
    }
    CHECK_EQ(TimerResolutionGuard::depth(), 0);
}

TEST_CASE(wait_until_returns_at_or_after_target)
{
    // 只有下界是硬的：sleep 的唤醒只会晚不会早，所以「早于 target 返回」一定是错（周期会被压短）。
    const auto target = std::chrono::steady_clock::now() + std::chrono::milliseconds{30};
    wait_until(target, std::chrono::microseconds{500});
    CHECK(std::chrono::steady_clock::now() >= target);
}

TEST_CASE(wait_until_returns_immediately_when_target_passed)
{
    // 落后（一轮采样就超了间隔）时不补睡也不自旋：时间已经过去，再等就是把周期硬拉长。
    const auto target = std::chrono::steady_clock::now() - std::chrono::milliseconds{1};
    const auto before = std::chrono::steady_clock::now();
    wait_until(target, std::chrono::microseconds{500});
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds{20});
}

TEST_CASE(wait_until_does_not_sleep_past_target)
{
    // spin_margin 比待等时长还大：不能按 target - spin_margin 去 sleep（那是过去时刻，
    // 写成无条件 sleep 就会睡过头），必须自己等到 target 为止。
    const auto target = std::chrono::steady_clock::now() + std::chrono::milliseconds{2};
    wait_until(target, std::chrono::seconds{1});
    const auto after = std::chrono::steady_clock::now();
    CHECK(after >= target);
    CHECK(after - target < std::chrono::milliseconds{50});
}

TEST_CASE(timer_guard_restores_on_early_exit)
{
    CHECK_EQ(TimerResolutionGuard::depth(), 0);
    try {
        TimerResolutionGuard guard;
        CHECK_EQ(TimerResolutionGuard::depth(), 1);
        throw std::runtime_error("提前离开");
    } catch (const std::runtime_error &) {
    }
    CHECK_EQ(TimerResolutionGuard::depth(), 0);
}
