# 设计：difftrace —— 基于 Unicorn 的访存序列差分

- 日期：2026-10-25
- 状态：**设计中**（负责人 2026-10-25 批准方向：C++、只记 store、按模块并行）
- 取代关系：本设施**不取代** `simplexc/tools/host/`（它是物理 CPU 宿主，保留作 golden 校准），
  但**取代**它作为日常差分的主判据——因为后者每个 tick 都要对整块游戏内存做全量扫描与哈希，
  成本与内存规模成正比，与「这一 tick 改了多少」无关。

## 1. 为什么换

实测（rv32im，`corpus/rv32im`，278 MB 游戏内存）：

| 项 | 每 tick |
| --- | --- |
| 全段 sha256（278 MB，`region_hash_every=1`） | 148 ms |
| 全量页扫描 × 2（`_settled_pages` 最少 2 遍） | 54 ms |
| 其余（轮询、快照） | ~32 ms |
| **合计** | **~234 ms** |

而同期一次 `run` 跑到 target=100 只花 0.2 s——**执行本身几乎不占时间**（1 tick ≈ 1000 条指令）。
1000 tick 因此要 13 分钟，其中 99% 是宿主的观测开销。

Unicorn 实测（`work/tmp/unicorn_sdk/bench.cc`，x86-64，`UC_HOOK_MEM_WRITE` C 回调）：
**36.4 M store/s = 27 ns/store**。按 1 tick ~200 次写估算，1000 tick 约 7 ms。

## 2. 判据

**比 store 序列，不比内存快照。**

一次 load 的值 = 之前的 store 序列 + 初始内存。初始内存两边相同（同源码地址、同 seed 图案、
同 API 桩返回值），所以 **store 序列等价 ⇔ 状态轨迹等价**。load 不必记录。

```cpp
struct Store {
    uint64_t pc;      // 写指令的绝对地址（诊断用：反汇编 + 源映射）
    uint64_t addr;    // 目标地址
    uint32_t size;    // 1/2/4/8/16
    uint64_t value;   // 实际写入的值（大端截断后按 size 取低位）
};
```

判据分三层，成本递增：

- **L0** 序列长度相等（最便宜，先比）
- **L1** 逐条比 `(pc, addr, size, value)`，第一个不同即分叉
- **L2** 分叉点 → 反汇编 → 源码行（后续，不在 MVP）

## 3. 架构

```text
difftrace/
├─ spec        .si → 地址表 / Enum / [lo,hi) 区间        （语义抄 simspec.py）
├─ image       blob → rebase → 代码区与各段布局          （语义抄 host.py）
├─ stubs       IAT 目标改成 magic 地址 + UC_HOOK_CODE    （新写）
├─ clock       虚拟时间（Sleep / GetSystemTimePreciseAsFileTime）
├─ trace       UC_HOOK_MEM_WRITE → Store 序列
└─ driver      发 run 命令 / 判完成 / 停
```

三块新写（stubs、clock、driver），其余是「照抄 Python 版的语义」，不重新发明。

## 4. 关键决策

### K1 执行单元是「一次 run」，不是「一个 tick」

程序协议（`.si` 里的 `run_sim` 主循环）：宿主写 `ctl_command = run` 与 `sim_target_cycle = N`，
程序进入 `mode_run(N)`，跑完 N 个 tick 后回到等命令的循环。所以设 N=1000 跑一次，
就拿到 1000 个 tick 的完整序列，**中间不停**。这正是旧宿主每 tick 同步一次所浪费掉的部分。

### K2 「跑完」的判据

`sim_last_command_id == cmd_id && sim_running == 0`。

在 `UC_HOOK_CODE` 里每 1000 条指令检查一次这两个 `u64`（成本可忽略），满足就 `uc_emu_stop`。
**不能要求 `sim_cycle == N`**：被测试结果 halt 的 run 会提前结束（旧宿主的注释已记录这一条）。

### K3 API 桩用 magic 地址

Unicorn 里没有真函数地址。做法：把 IAT 槽写成一个 `MAGIC_BASE + i` 的值，
在那些地址上挂 `UC_HOOK_CODE`；回调里按 **Win64（MS ABI，产物 triple 是 `x86_64-pc-windows-gnu`）**
读参数：`rcx, rdx, r8, r9`（第 5/6 个在 callee 入口的 `[rsp+0x28]`/`[rsp+0x30]`），
执行语义、设 `rax`，然后 `rip = *(uint64_t*)rsp; rsp += 8`。

需要**真语义**的只有 4 个：

| 导入 | 语义 |
| --- | --- |
| `VirtualAlloc` | 真 `uc_mem_map`，返回按请求顺序递增的确定性地址（K4） |
| `VirtualFree` | 解映射或忽略（记录以便审计） |
| `GetSystemTimePreciseAsFileTime` | 写虚拟时间到 `rdi` 指向的 `FILETIME` |
| `ExitThread` / `ExitProcess` | `uc_emu_stop` |

其余（`Sleep`、`WriteFile`、`fwrite`、`_write`、`puts`、`CloseHandle`、`CreateFile`、
`__stdio_common_vfprintf`）返回成功即可。`Sleep` 要推进虚拟时钟，否则程序的自旋节流行为不同。

### K4 `VirtualAlloc` 必须确定性

参考产物有自己的 arena 分配器，两边会拿到不同地址 → 序列必然错位。
桩必须返回**按请求顺序递增**的固定地址序列，两边因此一致。

### K5 内存按算出来的区间映射，不用常量

`regions()` 的语义：`memory_copy`/`memory_clear` 的实参（程序真会碰的范围）+ 地址声明
（标量退化成零长、数组扩 `(a-8, a)` 因为 `.len()` 读 `p-8`），合并后页对齐。
rv32im 算出 278 MB，别的电路是别的数。**任何字面常量都不许出现。**

### K6 `host.py` 保留作 golden

先用「rv32im 100 tick」这个**已知 equal** 的样本校准：新设施若判 equal，说明与旧设施结论一致；
若判 diff，说明新设施有 bug。这是新设施的自检，比任何单元测试都硬。

## 5. 必须先验证的未知项

| # | 未知 | 为什么致命 | 怎么验 |
| --- | --- | --- | --- |
| U1 | rv32im 跑到 `sim_cycle=13` 就满核自旋，`sim_running` 归 0，参考产物与我们**行为完全相同** | 若程序本身卡住，Unicorn 里同样卡，判据无从谈起 | 同一段代码跑到停，看 pc 落点；对照 `.si` 的 `run_sim` 主循环判断它在等什么 |
| U2 | blob 的 thunk / 重定位在 Unicorn 里是否照常工作 | rebase 错则全盘错位 | 单 blob 跑到第一条外部调用 |
| U3 | blob 用到的指令是否都在 Unicorn 覆盖内 | 不覆盖则直接停 | 观察是否报 `UC_ERR_INSN_INVALID` |
| U4 | 278 MB 映射对速度的影响 | 会不会把 36 M store/s 拖垮 | 实测每 tick 耗时 |

**U1 是前置**：它决定「跑到停」这个设计是否成立。

## 6. 模块接口（并行开发的契约）

```cpp
// spec.hh —— .si 的静态解析结果
struct Spec {
    static Spec parse(const std::string& path);

    // 宿主该映射的绝对区间（未页对齐，可能重叠，调用方负责合并与对齐）
    std::vector<std::pair<uint64_t, uint64_t>> regions() const;

    std::optional<uint64_t> addr(const std::string& name) const;   // #NAME = Ptr <num>
    std::optional<std::vector<uint64_t>> enum_values(const std::string& type) const;
    std::map<std::string, std::vector<std::string>> names() const;  // type X Enum[a, b]
    uint64_t game_lo() const;
    uint64_t game_hi() const;
};

// image.hh —— blob 的加载与识别（**不做 rebase**）
struct Thunk  { uint64_t slot_va; std::string import; };          // IAT 槽 VA → 导入名
struct DllRef { uint32_t code_offset; uint64_t baked_value; std::string import; };

struct Image {
    std::vector<uint8_t> code;      // **原始字节**，一个 movabs 都没改
    uint64_t code_base;             // 调用方指定的加载地址（Unicorn 里是常数）
    uint64_t entry;                 // 相对 code 的偏移
    std::vector<Thunk>  thunks;     // IAT 槽 → 导入名（stubs 用）
    std::vector<DllRef> dll_refs;   // 落在 DLL 区的 movabs 站点（23 处 / 6 个值，rv32im）

    // DLL 不存在则抛异常——不许静默退化成空表
    static Image load(const std::string& bin, const Spec& spec, uint64_t entry_off,
                      const std::string& dll_path = kDllReal);
};
```

**为什么不 rebase**：`host.py` 的 rebase 是为了「把捕获时的 DLL 地址换成本进程真实的 DLL 地址」。
Unicorn 里没有真 `compile_real.dll`（那里是 stubs），所以「rebase 到 `dll_base + offset`」没有意义。
因此：`delta = 调用方给的 dll_base − baked` 只用来**筛出「哪些 movabs 是 DLL 址」**；
把这些 movabs 改写成 `MAGIC_BASE + i` 是 **stubs 模块**的职责。

// stubs.hh —— API 桩（magic 地址表 + hook）
class Stubs {
public:
    explicit Stubs(uc_engine* uc);
    void install(const std::vector<Thunk>& thunks);   // 改 IAT 槽 + 挂 hook
    uint64_t clock() const;                           // 虚拟时间（100ns 单位）
private:
    uc_engine* uc_;
};

// trace.hh —— 写序列
struct Store { uint64_t pc, addr; uint32_t size; uint64_t value; };

class Trace {
public:
    void attach(uc_engine* uc);
    const std::vector<Store>& stores() const;
};

// driver —— 发命令 / 等完成 / 停
struct RunResult { uint64_t instrs, stores, sim_cycle; const char* reason; };
RunResult run_to_cycle(uc_engine* uc, const Spec& spec, uint64_t target);
```

## 7. 阶段

| 阶段 | 内容 | 判据 |
| --- | --- | --- |
| S0 | 验 U1~U4：一个 blob 跑到停 | 报出「执行多少条指令、停在哪、为什么停」 |
| S1 | store 记录 | 序列里能看到 `#SIMULATION_STATE + 2663` 的自增 |
| S2 | 两个 blob 逐条比 | 在 rv32im 上判 equal（与 `host.py` 100 tick 的结论一致） |
| S3 | 分叉 → 反汇编 → 源码行 | 能复现 sb 的 `x & 0xFFFFFFFF + 1` 定位 |

## 8. 明确不做

tick 级快照、段哈希、页扫描、VEH、真实线程模型、`--dump-pages`。
这些在旧宿主里存在，是因为它必须在**真进程**里伪造游戏环境；Unicorn 里没有存在理由。

## 9. 依赖

- Unicorn 2.1.4（`work/tmp/unicorn_sdk/`，来自 PyPI wheel 的 C SDK：
  `unicorn/include/unicorn/*.h`、`unicorn/lib/unicorn.dll`、`libunicorn.a` 由 `gendef`+`dlltool` 生成）。
  **不进 git**（16 MB 二进制）。
- 参考样例：`work/tmp/unicorn_sdk/bench.cc`（已编译验证 x86-64 + 写 hook 可用）。
- 编译：`g++ -O2 -std=c++17 -I <sdk>/unicorn/include ... -L <sdk>/unicorn/lib -lunicorn`，
  运行前 `unicorn.dll` 要在 exe 旁。
