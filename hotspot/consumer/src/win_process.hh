// 目标进程的 Win32 薄壳：句柄 RAII、按名字找 PID、线程枚举、可执行区枚举、读内存。
#pragma once

#include "samples.hh"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace hotspot {

class ProcessHandle {
  public:
    ProcessHandle() = default;
    ProcessHandle(const ProcessHandle &) = delete;
    ProcessHandle &operator=(const ProcessHandle &) = delete;
    ProcessHandle(ProcessHandle &&other) noexcept;
    ProcessHandle &operator=(ProcessHandle &&other) noexcept;
    ~ProcessHandle();

    static std::expected<ProcessHandle, std::string> open(std::uint32_t pid);

    void *raw() const
    {
        return handle_;
    }

  private:
    explicit ProcessHandle(void *handle)
        : handle_(handle)
    {
    }
    void close();

    void *handle_ = nullptr;
};

class ThreadHandle {
  public:
    ThreadHandle() = default;
    ThreadHandle(const ThreadHandle &) = delete;
    ThreadHandle &operator=(const ThreadHandle &) = delete;
    ThreadHandle(ThreadHandle &&other) noexcept;
    ThreadHandle &operator=(ThreadHandle &&other) noexcept;
    ~ThreadHandle();

    // 取上下文 + 查询 + 暂停三件套都申请：只能暂停的句柄取不到 RIP。
    static std::expected<ThreadHandle, std::string> open(std::uint32_t thread_id);

    void *raw() const
    {
        return handle_;
    }

  private:
    explicit ThreadHandle(void *handle)
        : handle_(handle)
    {
    }

    void *handle_ = nullptr;
};

// 进程名唯一匹配才算找到；0 个 / 多个都报错（同名多开时样本归属会错，spec §4.1）。
std::expected<std::uint32_t, std::string> find_pid_by_name(std::string_view exe_name);

std::expected<std::vector<std::uint32_t>, std::string> snapshot_thread_ids(std::uint32_t pid);

std::expected<bool, std::string> is_process_alive(std::uint32_t pid);

std::expected<std::vector<ExecRegion>, std::string> executable_regions(const ProcessHandle &process);

std::expected<void, std::string> read_memory(const ProcessHandle &process, std::uintptr_t address,
                                             void *destination, std::size_t length);

} // namespace hotspot
