/* stubs.hh —— 导入函数桩：IAT 槽改写成 magic 地址，在 magic 地址上挂 UC_HOOK_CODE 模拟语义
 * （设计 docs/specs/2026-10-25-difftrace-design.md §4 K3/K4）。
 *
 * 语义抄 `simplexc/tools/host/host.py` 的 `class Detours`：同一个导入给同一个返回值，两边才会得到
 * 同一个 store 序列。Unicorn 里没有真线程、真文件、真时钟，所以这些桩不必伪造进程环境，
 * 但必须保证**确定性** —— 不确定的值会直接让两边序列分叉：
 *
 *   - `VirtualAlloc` 的提交地址：从 `ARENA_BASE` 起按请求顺序页对齐递增，并且真 `uc_mem_map`（K4）；
 *   - 虚拟时钟 `clock()`：初值 0，`GetSystemTimePreciseAsFileTime` 每次 +10 ms，`Sleep(ms)` +ms，
 *     写进 FILETIME 的值是 `116444736000000000 + clock()`（Unix 纪元 → FILETIME 的固定偏移，
 *     100ns 单位）；
 *   - `calls()` 里不放调用方的栈地址、不放真实时间（`GetSystemTimePreciseAsFileTime` 的参数是栈地址，
 *     两遍跑必然不同；`Sleep`/读时钟属于节奏类，条数取决于真实耗时，不进记录）。
 *
 * 认不出的导入**不动它的 IAT 槽**，名字记进 `unknown()`：那一跳会落在未映射地址上并报
 * `UC_ERR_FETCH_UNMAPPED`，比让它静默按「返回 0」跑一遍错语义好查得多。
 *
 * `Stubs` 不拥有 `uc`：`uc_engine` 必须比 `Stubs` 活得久（析构里要 `uc_hook_del`）。
 */
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unicorn/unicorn.h>

/* `Thunk`（IAT 槽地址 + 导入名）的唯一一份定义在 `image.hh`：数据来源方是 image，
 * 而且 image 不依赖 Unicorn。反向 include 编不过 —— stubs.hh 要 `<unicorn/unicorn.h>`。 */
#include "image.hh"

namespace tc {

/* 第 i 个导入的桩地址 = MAGIC_BASE + i * MAGIC_STRIDE。步长 0x10 让地址表一眼能算出来
 * （`0xDEAD0000 + 0x10*i`），也让一趟 magic 区够放 256 个导入。 */
inline constexpr uint64_t MAGIC_BASE = 0xDEAD0000ull;
inline constexpr uint64_t MAGIC_STRIDE = 0x10ull;

class Stubs {
  public:
    /* 一次非节奏类导入调用。形状与 `host.py` 的 `Detours.records` 一致：
     * 参数是 Win64 顺序（rcx, rdx, r8, r9），`ret` 是写进 rax 的值。 */
    struct Call {
        std::string name;
        std::vector<uint64_t> args;
        uint64_t ret;
    };

    explicit Stubs(uc_engine* uc);
    ~Stubs();

    Stubs(const Stubs&) = delete;
    Stubs& operator=(const Stubs&) = delete;

    /* 把 thunks 里认得的导入的 IAT 槽写成 magic 值，并在整块 magic 区挂一个 UC_HOOK_CODE。
     * 位置按 thunks 的下标算（认不出的也占一个号），所以调用方给的顺序必须稳定。
     * uc 层面的失败抛 std::runtime_error —— 此时还没进回调，抛出去是安全的。 */
    void install(const std::vector<Thunk>& thunks);

    uint64_t clock() const { return clock_; }            /* 100ns 单位 */
    uint64_t alloc_next() const { return arena_next_; }  /* 下一次 VirtualAlloc 提交的地址 */

    /* magic 地址 → 导入名，按地址升序。诊断与测试用。 */
    std::vector<std::pair<uint64_t, std::string>> magic_table() const;

    /* thunks 里没有桩的导入名（槽没被改写）。 */
    const std::vector<std::string>& unknown() const { return unknown_; }

    const std::vector<Call>& calls() const { return calls_; }

    /* 回调里不能抛（穿过 C 栈帧），出错就记在这里并 uc_emu_stop。非空 = 这次运行作废。 */
    const std::string& error() const { return error_; }

  private:
    enum class Kind {
        kSleep,
        kGetFileTime,
        kVirtualAlloc,
        kVirtualFree,
        kWriteFile,
        kPuts,
        kFwrite,
        kWrite,
        kVfprintf,
        kCreateFileA,
        kCloseHandle,
        kExitThread,
        kExitProcess,
        kMemcpy,
    };

    static void code_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user_data);
    void dispatch(uint64_t address);

    /* Win64 实参：arg(0..3) = rcx, rdx, r8, r9。本模块没有 >4 参的导入，
     * 第 5/6 个（[rsp+0x28], [rsp+0x30]）因此不读。 */
    uint64_t arg(int i) const;
    void set_rax(uint64_t v);
    void pop_return();

    void write_u64(uint64_t va, uint64_t v);
    uint64_t peek_u64(uint64_t va, uint64_t n) const;
    void note(const char* name, std::initializer_list<uint64_t> args, uint64_t ret);
    void fail(const char* what, uc_err err);

    uint64_t virtual_alloc(uint64_t addr, uint64_t size, uint64_t type);
    uint64_t arena_alloc(uint64_t size);      /* 提交：真映射，地址顺序递增 */
    uint64_t reserve_alloc(uint64_t size);    /* 只 RESERVE：留地址，一页都不映射 */

    uc_engine* uc_;
    uc_hook hook_ = 0;
    bool hooked_ = false;
    uint64_t magic_len_ = 0;

    std::unordered_map<uint64_t, Kind> kinds_;  /* magic 地址 → 语义 */

    uint64_t clock_ = 0;
    uint64_t arena_next_ = 0;
    uint64_t reserve_next_ = 0;

    std::vector<std::pair<uint64_t, std::string>> table_;
    std::vector<std::string> unknown_;
    std::vector<Call> calls_;
    std::string error_;
};

}  // namespace tc
