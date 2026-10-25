// driver —— 把一份产物在 Unicorn 里跑到「一次 run 完成」（设计 §4 K1/K2、§6）。
//
// 环境的每一处都对着 `simplexc/tools/host/host.py` 抄：段几何（`merge_spans`：64 KB 页 +
// 「只有地址没有尺寸」给 1 MB）、协议区初值（`_seed_memory`：`ctl_cycle_speed_ms` 与数组 seq 头）、
// 完成判据（`sim_running` 被写回 0，即 `_tick` 里 `sim_running == 0` 那一刻）。
// 这三处差一点，比出来的 store 序列就不是同一件事。
//
// `trace`（写序列）在本文件里而不是单独的 trace.hh：写序列的 `pc` 只能靠 code hook 记的
// 「当前指令」（Unicorn 的 mem hook 不给 pc），与 driver 的计数/判停 hook 是同一次执行的数据。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <unicorn/unicorn.h>

#include "diff.hh"  // `Store` 的唯一定义处（写序列的判等也归它，driver 只产出序列）

struct Image;
struct Spec;

namespace tc {

// 写序列（设计 §2：只记 store，不记 load —— 一次 load 的值由之前的 store 序列与初始内存
// 决定，两边初始内存相同，比 store 就够）。
class Trace {
  public:
    void attach(uc_engine* uc);
    const std::vector<Store>& stores() const { return stores_; }
    uint64_t instrs() const { return instrs_; }
    uint64_t pc() const { return pc_; }  // 当前/最后一条指令的绝对地址

  private:
    static void code_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user_data);
    static void write_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value,
                           void* user_data);

    uc_engine* uc_ = nullptr;
    uc_hook code_hook_ = 0;
    uc_hook write_hook_ = 0;
    uint64_t pc_ = 0;
    uint64_t instrs_ = 0;
    std::vector<Store> stores_;
};

// 监视窗口内 `sim_running` 的一次写（`pc` 是写它的那条指令，mem hook 不给 pc，由 code hook 记）。
struct RunningWrite {
    uint64_t pc = 0;
    uint32_t size = 0;
    uint64_t value = 0;
};

// `#RESET_DATA_*` 的灌图案档位。`none` = 全零（＝不灌）；`seq` / `xorshift` 与 host.py
// `--seed-reset-data` 的图案逐字节相同 —— 两边传同一个档位才谈得上比 store 序列。
enum class Seed { kNone, kSeq, kXorshift };

// 环境准备的数字（诊断用：几何错了先看这里，不要先怀疑产物）。
struct Layout {
    size_t regions = 0;         // `spec.regions()` 合并后的段数
    uint64_t mapped_bytes = 0;  // 这些段的总字节数
    std::vector<std::pair<uint64_t, uint64_t>> spans;  // 合并后的 [lo, hi)，灌图案前的合法性判据
};

// `uc_mem_map_ptr` 映射进引擎的宿主内存（4 KB 对齐、零初始化）。Unicorn 直接读写这块，所以
// 宿主侧的原生 `memmove` 与 `uc_mem_write` 是同一件事 —— `rep movsb` 特判（`RepCopy`）靠这一点
// 省掉一次整块拷贝。必须活到 `uc_close`，所以挂在 `RunOutcome` 上（它比引擎活得久）。
struct HostRegion {
    uint64_t lo = 0;
    uint64_t hi = 0;
    std::shared_ptr<uint8_t> bytes;  // `hi - lo` 字节
};

// 站点计数：`dll_refs` 改写成 magic 的 / import 为空、原样不动的；`stub_refs`（sidecar）
// 改写成 magic 的。两套互斥（参考产物只后者为 0，我们的产物只前者为 0）。
struct PatchStats {
    size_t patched = 0;
    size_t untouched = 0;
    size_t stubs = 0;
};

// 1) 合并并映射 `spec.regions()`；2) 单独映射代码区并写入 `image.code`；3) 把整块 DLL 摆到
// `image.dll.base`（`Stubs::install` 要往 IAT 槽里写，槽就在这块里；host.py 里那是加载好的 DLL）。
Layout map_game(uc_engine* uc, const Spec& spec, const Image& image,
                std::vector<HostRegion>& host_regions);

// 把 import 非空的 `dll_refs` 立即数改写成 `magic_table` 里同名的桩地址；import 为空的
// 原样不动（身份未查清，不许猜）。同名桩不止一个 → 抛异常（不猜是哪一个）。
// `stub_refs`（sidecar）不走这里：它们的落点本身就是「导入地址存在这里」，和 IAT 槽同一概念，
// 由 `Stubs::install` 的槽清单一并写（见 driver.cc 的 `import_slots`）。
PatchStats patch_dll_refs(uc_engine* uc, const Image& image,
                          const std::vector<std::pair<uint64_t, std::string>>& magic_table);

// 启动期被原生 `memmove` 代替的一次 `rep movsb`：`count` / `src` / `dst` 是执行到那条指令时
// rcx / rsi / rdi 的值。特判只在启动期开（抓迹窗口里做会跳过 mem write hook，见 driver.cc）。
struct RepCopy {
    uint64_t pc = 0;
    uint64_t count = 0;
    uint64_t src = 0;
    uint64_t dst = 0;
};

// 一次 run 的结果（设计 §6 的 RunResult）。
struct RunResult {
    uint64_t instrs = 0;     // 两个阶段合计（启动 + run）
    uint64_t stores = 0;
    uint64_t sim_cycle = 0;
    // 停止原因。正常收工只两种："running_zero"（`sim_running` 被写 0，精确停）/
    // "sampled"（兜底：采样判据成立）；"timeout" 等带括号说明的是没按判据收工。
    std::string reason;
    // `error_buffer` 的字节数（它前 2 字节是小端 U16）。非 0 = 程序自己报过错。
    uint16_t error_len = 0;
};

struct RunOutcome {
    RunResult result;
    Layout layout;
    PatchStats refs;
    uint64_t instrs_boot = 0;       // 跑到「等命令」为止执行了多少条
    uint64_t stop_pc = 0;
    uint64_t wait_reads = 0;        // 判「在等命令」时看到的 ctl_command_id 读次数
    uint64_t unnamed_hits = 0;      // import 为空的 dll_refs 站点被踩到几次
    uint64_t unnamed_first_pc = 0;  // 第一次踩到的 pc（0 = 没踩到）
    size_t seeded_blocks = 0;       // 灌了图案的 `#RESET_DATA_*` 块数
    uint64_t seeded_bytes = 0;
    // 完成判据用到的三个数（全部从 `Spec` 算出来：`settings` 地址、`StateIndex` 里 sim_running
    // 的下标、监视地址 = settings + 下标*8）。诊断先看这三个数，别看代码。
    uint64_t settings_addr = 0;
    uint64_t sim_running_index = 0;
    uint64_t sim_running_addr = 0;
    // 监视窗口（发出 run 命令之后）内该地址的全部写，按发生顺序。
    std::vector<RunningWrite> running_writes;
    std::vector<Store> stores;      // 发命令之后的全部写序列
    std::vector<HostRegion> host_regions;  // `uc_mem_map_ptr` 的宿主内存（`rep movsb` 特判直接写它）
    std::vector<RepCopy> rep_copies;       // 启动期每条被特判的 `rep movsb` 一行
    uint64_t rep_bytes = 0;                // 上面这些拷贝的字节数合计
    std::vector<std::string> warnings;     // 特判退化成按字节执行的原因（DF=1 / 不在宿主映射里）
};

// 从 `code_base + entry` 起跑：先跑到「等命令」状态，再发一次 run（target_cycle 个 tick）
// 并跑到 K2 判据成立。失败不猜、不硬凑 —— 原因写进 `result.reason`，pc/rsp 由调用方打。
// `seed` 决定 `#RESET_DATA_*` 灌什么（host.py 的 `--seed-reset-data` 同语义）。
RunOutcome run_to_cycle(uc_engine* uc, const Spec& spec, const Image& image, uint64_t target_cycle,
                        Seed seed);

}  // namespace tc
