// blob 的加载与识别：8 字节头 → 机器码、entry、DLL 绝对地址站点、每个站点是不是导入。
//
// 语义逐条对照 `simplexc/tools/host/host.py`（宿主版是 golden）：`read_bytes` 的 8 字节头、
// `movabs_sites` 的站点扫描、`recover_baked_base` 的投票、`_scan_product`/`_rebase` 共用的
// 「这个立即数落在 DLL 镜像里吗」判定。名字来自 DLL 的导入表 + thunk 扫描（`abs_scan.Pe`）。
//
// 与设计 §6 的一处偏离（负责人 2026-10-25 决定）：image **不做 rebase**，`code` 就是原始字节，
// 只做识别。Unicorn 里没有真的 `compile_real.dll`，「改成实际加载基址」在那里没有意义；
// 把站点的立即数换成 magic 地址是 stubs 模块的活。`baked` 保留下来当判据：
// `delta = kCodeBase - baked` 只是「按 rebase 到 kCodeBase 看」这个视角的名字，筛站点用的是
// 立即数本身所在的 baked 空间 `[baked, baked + dll_size)`。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Spec;

// 与 `abs_scan.DLL_REAL` 同一个文件：导入名与 baked 基址都只能从它读出来（见 .cc 里的理由）。
//
// 路径不写在源码里：游戏目录一台机器一个值，由 CMake 传 `-DTC_GAME_DIR=<游戏目录>`
// （`SIMPLEXC_GAME_DIR`，默认取环境变量 `TC_GAME_DIR`）。clangd 不走 CMake、拿不到 `-D`，
// 所以缺省值是能静态解析的占位 `.`（= 进程当前目录），不能 `#error`。
// 运行期还有两条覆盖：`--dll-dir` > `DIFFTRACE_DLL_DIR` > 这个默认值（见 main.cc）。
#ifndef TC_GAME_DIR
#define TC_GAME_DIR "."
#endif
inline constexpr const char* kDllReal = TC_GAME_DIR "\\compile_real.dll";

// Unicorn 里给代码页挑的加载地址。blob 位置无关（自引用走 RIP-relative），写死只是为了
// 让站点 VA 可复现。
inline constexpr uint64_t kCodeBase = 0x10000000;

// `slot_va` = IAT 槽的绝对地址（baked 空间）。`import` 只有过程名（`KERNEL32.dll!` 前缀去掉）：
// 与 host.py 的 `detour_name` 同一理由——符号名只允许一处拼法，两处不一致会「同一个符号
// 一半命中一半不命中」。
struct Thunk {
    uint64_t slot_va;
    std::string import;
};

// DLL 的内存映像。为什么非得有：产物除了走 thunk，还会**直接**调 DLL 里不是 thunk 的函数
// （rv32im 的 6 个不同值里有 3 个指 rva 0x1350/0x1380），那层壳自己要 `call [rip+rel32]`
// 走到 IAT 槽上。所以 Unicorn 里必须把整块 DLL 摆出来、并把**整张**导入表打桩：
//
//   - `image` 是 `LoadLibrary` 之后的字节（头 + 各节按 VA 摆，节外与 `.bss` 是 0）；
//   - `iat` 是**整张**导入表（不只产物引用到的那几个 —— 那些见 `Image::thunks`）：DLL 自己的
//     代码会调别的导入（实测 rva 0x1350 那层调 `GetSystemTimePreciseAsFileTime`），少一个就死在那里；
//   - `base` 是期望加载地址（= 识别出来的 `baked`）。
struct DllImage {
    uint64_t base = 0;
    std::vector<uint8_t> image;
    std::vector<Thunk> iat;  // 按槽 VA 升序
};

// 一处指向 DLL 镜像的 `movabs` 立即数。`import` 为空 = 落在镜像里但不在 thunk 表里
// （DLL 自己的数据/未被扫到的跳板），身份**未查清**，不许猜。
struct DllRef {
    uint64_t code_offset;
    uint64_t baked_value;
    std::string import;
};

// `.stubs` sidecar 的一行。golden 是 `simplexc/tools/host/host.py` 的 `read_stub_sidecar`：
// 我们的产物不走 DLL thunk，而是把编译期 `GetProcAddress` 的绝对地址直接烤进 blob，同时写一份
// `<bin>.stubs` 说明这些地址在哪。参考产物走 thunk、没有 sidecar，我们的产物没有 `dll_refs`
// —— 两条导入机制互斥。
//
// `code_offset` 相对机器码起点（= `Image::code` 的起点）；`Code` 是 12 字节代码桩
// `movabs rax, imm64 ; jmp rax`（要改的是 offset+2 处的 imm64），`Slot` 是 `__imp_X` 的
// 8 字节数据槽（要改的是 offset 处的槽本身）。
enum class StubKind { Code, Slot };

struct StubRef {
    uint64_t code_offset;
    StubKind kind;
    std::string import;  // 去掉 `__imp_` 前缀的过程名：与 `Thunk::import` 同一拼法
};

// `movabs r64, imm64` 立即数的 (偏移, 值)，按偏移升序。是站点扫描唯一的定义处。
std::vector<std::pair<uint64_t, uint64_t>> movabs_sites(const std::vector<uint8_t>& code);

struct Image {
    std::vector<uint8_t> code;   // blob[8:]，原始字节
    uint64_t code_base;          // 调用方该把 code 映射到的地址
    uint64_t entry;              // 相对 code 的偏移
    std::vector<DllRef> dll_refs;  // 指向 DLL 镜像的站点（按 code_offset 升序）
    std::vector<Thunk> thunks;     // 被引用的 IAT 槽（按 slot_va 升序、去重）
    DllImage dll;                  // 整块 DLL 的内存映像 + 整张导入表（装桩用这个）
    // `<bin>.stubs` 的每一行，按文件顺序（装桩的编号跟着下标走，顺序必须稳定）。没有 sidecar 就是空。
    std::vector<StubRef> stub_refs;

    // 识别过程的两个判据数字：投票出的捕获期 DLL 基址、DLL 镜像大小（`SizeOfImage`）。
    uint64_t baked;
    uint64_t dll_size;

    // 读不了 blob / DLL / sidecar、头里的总长与文件长度不符、entry 越界、sidecar 语法坏 →
    // 抛 std::runtime_error。
    static Image load(const std::string& bin, const Spec& spec, uint64_t entry_off,
                      const std::string& dll_path = kDllReal);
};
