# 热点分析工具（hotspot）设计

状态行：**v9（2026-11-05）：W1–W4 已落地；suspend 已真机跑通；etw 自写已实现（构建/单测），待管理员真机验证**。
取代：旧工具链（`tools/analyze_sim_hotspot.py` 等）的职能；代码关系：**全部新写**，
旧代码不复用、不复制，只有「跑法/指标口径」的遗产被沿用。

引用坐标约定：所有文件引用 = **git 根（tc-toolkit/）相对路径**，冒号后为 1 基行号，
编辑器可 ctrl-click 跳转。

参数口径约定：本 spec 里的数字/阈值除负责人原话确认过的（全局标注「负责人定」）外，
一律是**默认值（可议/可调）**；填白不是定案，对默认值提不同意见不算未决项。

## 0. 一句话

把游戏 JIT 出的电路机器码里「时间花在哪」定位到**元件 / 源码行**粒度，同时服务
我们的编译器（simplexc，主目标）与游戏原版编译器（ref，仅架构预留）。

## 1. 目标与非目标

**目标**：

- 对一份给定电路产物（blob），产出「元件类型 / 元件实例 / 源码行 / 未归属分桶」四张
  热度表；
- 呈现 = **静态交互报告**（analysis 产单文件 `report.html`，浏览器打开）+ 文本
  `report.md`（归档/引用）；桌面 GUI = 负责人**保留意见、暂不实现**（Tkinter 可做
  简单款；何时做、做什么形态届时再议，非否决——见本节范围外清单外）；
- 采样 = **双后端都做，ETW 为主**（`etw` 需管理员/零暂停，自写 Win32 实时消费；
  `suspend` 免管理员/每样本有暂停开销；共享聚合层，§4.0），以**常驻交互式会话**进行（§4.6）：进程/产物/线程
  识别只做一次，多窗口复用——不做「一次调用后退出」的一次性采样器（旧链每次找仿真
  线程数秒；一次性调用器形态作废）；
- 归因链不依赖 MSVC/PDB；归因数据全部来自**编译时产出的侧车文件**，运行时零现场
  解析（除内存定位）；
- 无硬编码路径：一切外部路径走命令行参数或单一 config 文件（`hotspot/config.toml`，
  键清单见 §10）。

**非目标 / 范围外**（正文即规范）：

- 游戏自动启动 / 自动切换电路不属本工具范围（负责人已定：脚本化困难，人工就绪
  是既定边界）；
- simctl/freq 粘合不做（负责人明确否决过该方向）；
- ref Provider 不在工具范围，只锁 schema（§6；双编译器对比需求出现后再议，§8-G3″）；
- 栈采样暂不做，但 samples.tsv 契约按版本化留位（§4.4）；
- 不改写 hook 的任何旧文件——hook 目录只是**只读数据源**；
- 不复用旧工具的代码。

（桌面 GUI 不属于"范围外"，单独状态：**暂缓（保留意见）**。）

## 2. 术语（一次定义，全文按此）

| 术语 | 含义 |
|---|---|
| blob | 编译产物机器码的一维字节串（游戏通过 compile() 拿到、JIT 拷贝执行）。**blob 文件前 8 字节是长度头**（`simplexc/assemble/assemble.hh:7,65`），游戏拷的是 `blob+8` 起的机器码 |
| **blob 偏移（全文坐标）** | RIP − **代码起点** = blob 文件下标 − 8。全文 sidecar/samples 的 offset 均按此坐标（与既有 `<blob>.stubs` 一致，`simplexc/cli/tcblob.cc:194`） |
| blob 基址 | 代码起点在目标进程内的加载地址，采样时定位（§4.3） |
| 元件 | `.si` 源码里一段连续的代码块（注释行引领，如 `work/hook/hook_src_001.si:355-361`），机器码可能被优化搬运，**不假定机器码连续** |
| 元件主键 | `(type, id)`：id = 注释第一段数字（同一元件「代码段+绑定段」两条注释共享，旧链验证过的合并键）；**同一 sidecar 内 id 必须唯一**（旧链实测 1234 段=617 实例；生成器违反即报错，不静默配对） |
| 元件行区间规则 | 区间 = **[元件注释行， 下一个元件注释行/顶层注释或块边界)**；末元件止于下一个非元件顶层注释或文件尾；每元件两区间（代码段+绑定段）。防「末元件吞公共代码」的历史坑（虚高 89%，`docs/facts/perf-tooling.md`） |
| 元件表 | 前端从源文件原文逐注释段产出的行式表（§5.3 有列定义），Provider 按 (type,id) 合并成台账 |
| 侧车 sidecar | 编译时产出的、blob 旁随附的独立映射文件（JSON，§6） |
| Provider | 侧车生成器，按编译器各一：`ours`（simplexc，路线 A）/ `ref`（原版，仅锁 schema） |
| 采样后端 | 产出 (IP, ThreadId) 样本流的两种实现：`etw`（内核 PMU，自写，主）与 `suspend`（自建，备用）；接口同构（§4.0） |
| 电路主循环 | 产物内驱动「跑一圈仿真」的代码（通过监听内存命令推进 tick），属于 blob → bucket=0 |
| 外部调用桩 | 产物里 `kernel32.'xxx'(...)` 等外部调用的转发段；**有现成普查**（`AsmResult::stubs` → `<blob>.stubs`），进侧车独立列出（§6） |
| 归因 | 样本 (bucket, blob偏移) × 侧车 → 热度表的过程（§7.1） |

注释行格式口径（元件主键唯一数据源）：`// <第一段数字> <com_\w+> <第三段=位宽>
<第四段=大数字> [可选第五段语义词（如 BANK/Input/Output），`simplexc/corpus/src/177.si:1190`
实测存在]。第一段数字 = id（用途已定），**其语义与第四段大数字一样未证**（§13）；
段数可变、有 LATE / 负 ID / ID=0 变体——解析器**宽容取值**：第一段整数 + 首个
`com_*` 词 + 第三段整数（位宽 en）+ 首个 ≥2^32 的 token（=第四段，作 tail 原样存），
不做格式断言。

## 3. 总体架构

```
①常驻采样会话 (C++23)           ②内存定位（consumer 内）        ③Provider                 ④分析 (Python)
采样后端×2：**etw(主,需管理员)** 代码起点定位（可能多候选）      ours: 行表×元件表 join     samples + sidecar
/ suspend(免管理员)              会话内多窗口复用（§4.6）        ref: 魔数标记链（未实现）   → report.md + report.html
        └────── profile bundle（一个 run 目录，§10；由 consumer `save` 落盘）──────┘
```

- ①②③④ 之间以**文件**为唯一接口（samples.tsv / sidecar.json / run 目录），任一环节
  可单独重跑；
- 换编译器 = 换 Provider；换采样后端 = 同一 consumer 内换数据源（samples.tsv 有版本
  号，§4.4）；
- etw 后端的事件全系统可见 → 进程过滤是**架构的一环**；`suspend` 后端本来就只枚举
  目标进程线程，不需要这层。

目录骨架（**同一仓库 tc-toolkit/ 内的新增目录**，不建新 git 仓库）：

```
tc-toolkit/hotspot/
  consumer/     # C++23：采样后端×2（etw 主）+ 共享聚合层 + 常驻 REPL → samples.tsv
  tools/        # ours join 脚本（侧车生成，W3；只读装配器产物）+ config.toml
  analysis/     # Python（标准库）：归因引擎 + 四张热度表 + G3′/G4 基线
  workspace/    # 运行期工作目录（产物引用、run 归档、baseline），.gitignore（§10）
```

## 4. 采样层（consumer，C++23）

### 4.0 两个后端（负责人定：双后端都做，ETW 为主；接口同构，共享聚合层）

两者产出**同一份** samples.tsv v1，共享聚合层（TID 快滤 → blob 归属/分桶 → 聚合计
数）；差异只在「样本从哪来」。**为主的是 etw**（suspend 是免管理员备用通道）：

| | **`etw`（内核 PMU / ETW，主）** | `suspend`（自建 SuspendThread，备用） |
|---|---|---|
| 提权 | **需要管理员**（开内核日志会话） | **不需要**（同用户进程 OpenProcess 即可，旧链走通验证；单样本暂停时长上界 W2 实测后回填） |
| 每样本扰动 | 中断打点，被测线程零暂停 | 每 SuspendThread 数 µs 起的暂停（跨进程 GetThreadContext 可能达几十 µs），**上界未测** |
| 直取线程 | 事件全系统 → 样本须按 TID 过滤（快照层，§4.1） | 直接持目标进程线程句柄，**无需系统过滤** |
| 插件量 | 会话管理 + 实时事件解码（自写） | ~200 行 |
| 精细用途 | 主力；时延/抖动类研究也用它 | 单一频率，作为免管理员环境通道 |

接口纪律：两后端对聚合层暴露的中间结构相同（IP、ThreadId、来源窗口时长）；
samples.tsv v1 契约（§4.4）不变；「换后端」是 consumer 内部战术，不落任何相邻一层。

### 4.1 etw 后端链路（自写；suspend 后端无此节）

- 只用 Win32 C API（`evntrace.h` / `evntcons.h`），不引入第三方库、不用 TDH；只解析
  SystemTraceProvider 的 SampledProfile 事件，负载按固定布局解 IP/TID；
- 链路：`StartTraceW(KERNEL_LOGGER_NAME)` 建内核日志会话 →
  `TraceSetInformation(TraceSampledProfileIntervalInfo)` 设间隔（100 ns 单位，
  1 ms=10000；下限实测回填，§13）→ `EnableTraceEx2` 开 `EVENT_TRACE_FLAG_PROFILE` →
  `OpenTraceW`（实时模式）+ `ProcessTrace` 在专用线程收事件；
- 事件过滤：`EventHeader.ProviderId` 匹配 SystemTraceProvider、`EventDescriptor.Id`
  匹配 SampledProfile（GUID 与 ID 由探针实测钉死，§13）；`EventHeader.ThreadId` 即
  采样时线程；负载 v1 = IP(4)+TID(4)、v2 = IP(8)+TID(4)，未知版本丢弃并计数，不猜；
- 提权：开内核日志会话需管理员令牌；ETW 非管理员时弹 UAC 重启自身，取消则退出；
  suspend 不提权；
- 会话生命周期：整个 REPL 只建一次会话，多次 `window` 复用；`quit`/EOF 时停止并
  关闭，退出后不持有会话；改间隔重建会话（`open --interval-ms`，§4.6）；
- 进程过滤：ETW 事件全系统可见 → 窗口起点与终点各做一次目标进程全部 TID 枚举快照，
  解析时只留两快照并集里的 TID；同名进程多开/PID 复用显式报错，不猜样本归属。⚠️
  采样中途新开启的线程不在快照内，其样本**直接丢失、不可计数**；G4 对此的告警信号
  是 **bucket 份额下降 + 首尾快照差集非空**（§8）；
- 失败路径：`StartTrace`/`TraceSetInformation`/`EnableTraceEx2`/`OpenTrace`/
  `ProcessTrace` 任一失败直接 assert（开发期，不做自动回退）；
- 解码与聚合解耦：`backend_etw` 只产出 `(ip, tid)` 流，聚合/分桶不感知 ETW；
  版本分派表独立，新增事件/版本不改聚合层。

### 4.2 参数（默认，可调；两后端共用）

| 参数 | 默认 | 依据 |
|---|---|---|
| 窗口时长 | **10 s**（默认，可调） | 10 MHz 电路 → 窗口内约 1e8 tick |
| 采样间隔 | **1 ms**（可调，支持缩短） | 相位均匀性**不靠**「间隔远大于 tick 周期」这种比值（1 ms = 10⁴ × 100 ns 是整数倍，稳态可能锁相）；均匀性来自**频率漂移/调度抖动**。若怀疑周期性结构（如某元件每 N tick 才跑一次），**缩短间隔复跑对比 = G3′ 变体** |
| 样本量预期 | 数万（随核数） | 够 G3′ 重合率评估 |

`etw` 的间隔经 §4.1 入口设置；`suspend` 的间隔就是 consumer 内部节拍（同样的值）。

### 4.3 内存定位（代码起点基址；两后端共用）

采用旧链验证过的方法（**方法沿用，代码新写**）：

1. 特征字节取**代码段中段固定窗**（旧规则 code 内 off=0x400、窗长 256 的量级；产物
   过短退回中段或整段）；⚠️ 两个旧坑：**不能取产物开头**（所有产物开头是同一段固定
   前缀，会全数命中）；**特征窗偏移按 blob 偏移（去头后）计**，带头计算会让特征
   整体错 8 字节、候选全废（旧工具实测踩过：`tools/locate_jit_image.py:104-110`）；
2. 在目标进程的可执行内存区搜特征；每次命中与**产物文件的代码段**（blob 文件 +8
   起的 code_size 字节）做整段逐字节比对确认——比对通过即同时完成「产物文件 ==
   内存中代码」的哈希级校验，防「分析的是这份、跑的是另一份」错配。⚠️ 文件头是
   `文件总长 | (1<<62)`（bit62 = 游戏不 free，`simplexc/assemble/assemble.cc:490-502`），
   长度判据必须先掩位；`head == len(img)` 对带 bit62 的产物失效；
3. **多候选处理（历史实测同一电路一次编译 ≤19 副本共存）**：全部比对成功的候选都
   记为 blob 内命中，折算到同一坐标；候选集合合并计数，独立副本不区分；
4. 归因窗口 = 每个 `[base, base + code_size)`。

前提事实：**同一游戏进程实例内，离线编译产物与游戏 JIT 加载的字节逐字节相同**
（历史验证，含 8 字节头的 sha 口径见 §6；跨进程实例重启则可能漂 extern 槽值，
靠 §8-G4 水位自证؛ 跨 boot 对齐用归一化 sha（掩外部槽）备选）。

### 4.4 samples.tsv 契约（版本化）

consumer 聚合输出（同一 (bucket, blob_offset, thread_id) 合并计数），首行头：

```
# samples.tsv v1
bucket <TAB> blob_offset <TAB> thread_id <TAB> count
```

- `bucket=0`：落在**本产物全部副本**内，blob_offset 有效；
- `bucket=1`：其他非 IMAGE 可执行区（旧电路 JIT 残留、其它用途 JIT）；
- `bucket=2`：IMAGE 区（exe/dll，游戏引擎/调度/输入等代码；电路主循环在产物内，归 bucket=0）；
- `thread_id` 为 v1 附带信息，analysis 大多忽略（为 v2 栈采样留位，v2 加列）。

接口冻结点：换采样层（如未来栈采样）= 落 `v2` 新列，analysis 加适配，禁止静默改
v1 语义。

### 4.5 工具链

- C++23 / GCC 14.2（`docs/specs/2026-10-15-simplexc-interfaces.md:317` 有据）；
  cmake / ninja PATH 实测在位（2026-11-01）；**编译选项一律 `-Wall -Werror`**
 （hotspot 全部 C++ 目标；0 warning 属规范结果，不是第二个目标）；
- `etw` 后端**自写**：纯 Win32 C API（`evntrace.h` / `evntcons.h`），不引入第三方库、
 不用 TDH；只解析 SampledProfile（固定布局，§4.1）。GCC 即可编译；失败直接 assert
 （开发期，§4.1）。

### 4.6 常驻交互式会话（形态：REPL；不做「一次调用后退出」的采样器）

背景：旧链一次采样里「认仿真线程」就有数秒级开销（70 线程逐个探测，旧工具实测）；
一次性调用器每采一次付一次。consumer 为**常驻 REPL**，状态在会话内缓存：

- 会话状态：目标 PID/全部线程句柄、当前产物+基址候选（§4.3）、已产出的样本聚合；
  `open` 后多窗口复用，不重复定位/枚举；
- 线程集合在 `window` 开头轻量校验（线程数/存活变化则重枚举，毫秒级代价）；
- 最小命令集：`open <artifact> [--pid N] [--interval-ms <ms>]` 定位并缓存 /
  `window <sec> [save]` 采一个窗口 / `status` / `locate` 重跑定位 / `save [dir]` 落
  run 目录（建目录、写 samples.tsv+run.toml、调外部 freq 命令取首尾读数）/ `quit`；
- run 目录的创建与 `run.toml` 初写归 `save`（W2 范围）；analysis 只回写 buckets 与
  baseline（§10）；
- 外部频率读数（§9）经 config 给出的外部命令调用，路径不硬编码（§10）；
- etw 后端在同一 REPL 内保持会话存活（管理员锁一次）；改间隔需重建会话（§4.1），
 `open` 参数形态已落；`save`/退出时停会话；
- 各环仍可单飞：`report.py` 等离线环节不依赖 REPL。

## 5. 编译器侧调试信息（W1；改 `simplexc/**`）

前提说明：simplexc 目前**没有任何调试信息实现**（grep 不到 DIBuilder/DebugLoc/Dwarf）。
主设计 §7.6 计划了但未排工期；Temp 旧方案总结被部分吸收（走得的是行号表路线，词法块
方案降为备胎，见本节末）。

**前端→LLVM 一切经 C API**（仓库冻结约定：`LLVM-C.dll` + C API，
`docs/specs/2026-10-15-simplexc-interfaces.md` §7、`simplexc/codegen/llvm_ffi.hh:1-11`；
`simplexc/.deps/llvm/` 有 `lib/libLLVM-C.a` 与 `include/`；**不引入 C++ API**）。
`nm` 实测已导出可用（LLVM 23.1.3）：
`LLVMCreateDIBuilder`、`LLVMDIBuilderCreateCompileUnit`（`.deps/llvm/include/llvm-c/DebugInfo.h:311`）、
`LLVMDIBuilderCreateDebugLocation`（`:507`）、`LLVMSetCurrentDebugLocation2`（`llvm-c/Core.h:4638`）、
`LLVMAddModuleFlag`（`llvm-c/Core.h:1009`）、`LLVMDIBuilderFinalize`（`:270`）。

1. **DIBuilder 基建（C API 调用序）**：CompileUnit → DIFile / DISubprogram 元数据链；
   module 前列 flags：`"Debug Info Version"=3`（缺失则后端静默丢 debug info，最
   常见坑）；**Dwarf Version 由 module flag 显式置 4**（C API 无 DwarfVersion 形参；
   LLVM 23 默认 DWARF 5，`clang/Driver/ToolChain.h:662`，不显式置位会静默变 5）；收尾
   `LLVMDIBuilderFinalize`。
2. **DebugLoc 喂行号**：每条 IR 指令挂 `DebugLoc{line}`。只喂行号，不喂列号（元件按
   **行区间**归并，元件边界全在注释行侧，同一行不跨两元件 → 列信息无必要）。
3. **元件表产出（源文件原文路径，不经 codegen）**：实测 `mask_source` 把 `//` 与
  `/* */` 在 raw/code 两份里**都抹成空格**（`simplexc/frontend/decls.cc:400-419`），
  `raw_lines` 不含注释原文；`decls.cc:719-723` 的 `line_class` 的 comment 分支是
  多行字符串续行，不是注释行。W1 的实现点：CLI 从 `--src` 源文件原文解析元件注释
  行（§2 宽容规则；行号与 raw_lines 等长对齐，`decls.hh:27`），新增
  `--elements-out <path>`，**逐注释段**输出元件：
  `el.tsv` 首行 `# el.tsv v1`，之后每行 = `id\ttype\twidth\ttail\tline_lo\tline_hi`
  （该段自己的行区间；缺 tail 记空字段）。**行号是源文件内的 1 基行号，不加 `--stdlib`
  前缀的行数**：一个行号属于哪份文件由 `<blob>.offsets` 的 `sources` 说（§6），两边都加
  一遍前缀就会重复扣减。（type,id) 台账合并与行区间数组化在
  Provider（§6）完成，承接 §2 行区间规则。交 Provider。codegen 只喂行号，不承
  元件语义。
4. **G1 门（挂进 simplexc 判据流程）**：同一 IR 开/关 `--debug-info` 各过一遍后端，
   `objdump -d` 全量 diff（只比 `.text`）必须为空；非空先排除 LLVM 特定 bug 再
   继续——**调试信息不允许改变机器码**。flag 本体是 W1 新增（现 CLI 无此 flag；
   `simplexc/cli/simplexc_cli.cc:11` 现入口 `--obj-out/--out`）。

**备胎留档（路线 B；满足触发条件才启用）**：原 Temp 旧方案——每元件包
`DILexicalBlock`（作用域机器码地址范围）+ 入口 `llvm.dbg.label(elm_<id>)`（label↔
块配对靠名），在 DWARF `.debug_info` 直产「机器码区间→元件」（支持不连续多段，
`DW_AT_ranges`）；抽取脚本扫 DIE 树。触发条件：**元件机器码大量逃出 §2 行区间规则的
覆盖**（行号表归因失效）时启用；累积风险（老 GDB/pyelftools 对 v5
rnglists 支持参差）届时实测。侧车 schema 不变。

## 6. 侧车（sidecar.json schema 与生成）

两个 Provider 填同一份 schema，分析层只认格式：

```jsonc
{
  "version": 1,
  "compiler": "ours",                 // ours | ref
  "artifact": {   // 指纹口径 = hook manifest 的 out_sha256/out_len（整份文件、含 8 字节头，
                  // simplexc_cli/compile_hook.c:539 一脉）；ours 产物不过改写器，
                  // 与 hook.log 的 opt 行 sha 无关（后者只属于改写过的 ref 产物）
    "len": <int>, "sha256": "<hex>"
  },
  "meta": {                       // 产出：make_sidecar 从 simplexc_cli --timing 的 stderr
     "opt_level": ...,            // （前端/lowering/LLVM/装配四段，无 sema 段则标 null）
     "pass_pipeline": "...",      // opt_level/pass_pipeline 取编译入口常量（default<O2>）
     "stages": { "frontend": …, "sema": null, "lowering": …, "llvm": …, "assemble": … },
     "sources": [                  // 原样拄自 offsets：行号的来源文件表（参与拼接的文件）
       { "file": "stdlib_prefix.si", "lines": 1503, "role": "stdlib", "path": "…" },
       { "file": "hook_src_009.si",  "lines": 9053, "role": "source", "path": "…" } ],
     "functions_version": 1        // `functions[]` 与 `func_id` 的 schema 版本；无函数表的旧侧车没有这个键
  },                              // 用途：主设计 §7.6「LLVM 化了没变快」可归因
  "elements": [                        // 元件台账：按 (type,id) 去重（Provider 合并 el.tsv）；行区间数组（§2 规则）
     { "id": 9, "type": "com_register_word", "width": 8,
       "lines": [[355,361], [802,809]], "tail": 1985710402811285152,
       "source": "hook_src_009.si" },  // 行区间属于哪份文件（= sources 里 role=source 的 file）
  ],
  "segments": [ { "start": 0, "end": 122, "id": 9, "func_id": 17 } ],    // 主查询表：blob 偏移区间 → 元件
  "lines":    [ { "start": 0, "end": 44, "line": 1234, "file": "hook_src_009.si",
                 "func_id": 17 } ],  // 全部有行号信息的机器段（含元件段与未归属段）；
                                      // `line` 是**文件内**行号，`file` 说它是哪份文件；
                                      // 表 3（行级明细）与「未归属段」都从它查
  "functions": [                  // 函数体台账（.debug_info 的 DW_TAG_subprogram / inlined_subroutine）
     { "func_id": 17,             // ranges 与其他函数互不重叠（已按「最内层 DIE」切过）
       "ranges": [[0, 122], [900, 940]],   // 抽象实例没有函数体 → 空数组
       "name": "mode_run", "decl_line": 7049, "file": "hook_src_009.si",
       "tag": "subprogram",       // subprogram | inline
       "depth": 1,                // DIE 嵌套深度（最内层判定用的就是它）
       "origin": null } ],        // 内联实例指向抽象实例的 func_id；没有 abstract_origin 则 null
  "stubs":    [ { "start": 412, "end": 440, "kind": "stub", "name": "kernel32.VirtualAlloc" } ]
}
```

旧 sidecar（没有 `file`/`meta.sources`/`functions`）按拼接后行号显示、不做文件分类，也没有
函数归因：分析层不因缺字段报错，输出保持旧样子。

- 表不变式与排序约束：`segments.id ⊆ elements.id`；**同一 sidecar 内 id 唯一**（§2）；
  `segments`/`lines`/`stubs` 均按 start 升序；**区间 half-open `[start,end)`**，且
  「互不重叠」仅指**同一数组内部**——`segments` 与 `lines` 天然重叠（同一机器段既
  是元件段又有行号），查询顺序 **segments → lines → stubs**，`segments` 优先；
- `segments`/`stubs` 的 `end` 换算：`stubs.end = start + (kind=="stub" ? 12 : 8)`
 （`assemble.hh:80-83` stub=12 B / slot=8 B；`simplexc_cli.cc:246` 的 `.stubs` 只有
 offset/kind/symbol 三列，end 是 Provider 的 8 行换算，非「现成」）；
- `functions[]` 的 `ranges` **全局**互不重叠（已按 depth 最大者切成最内层区间）：一个 blob
 偏移必须只能对应一个函数，否则「哪个函数在跑」取决于实现。`lines`/`segments` 的
  `func_id` 是同一结论的冗余列：只有整段落在**同一个**函数里才填 id，跨函数边界与「没有
  函数信息」都写 null —— 分析层的函数归因不查这一列，直接查 `functions[].ranges`；
- `elements.width` = 注释第三段=位宽（负责人确证），供展示；`tail` = 注释第四段
 （首个 ≥2^32 整数），语义未证，原样存。

**ours Provider**（`hotspot/tools/` 的 join 脚本，构建后离线跑）：

- 输入：LLVM 产对象文件（`.debug_line` 行表）+ 前端元件表 `el.tsv`（§5.3）；承接
  §2 行区间规则合并出 `elements` 台账；
- 先把行表展开成 (blob 偏移 → 行号) 有序表：据此给**全部**有行号信息的机器段产
  `lines`；再按元件行区间 join 出 `segments`（widget 内未被任何元件行区间覆盖的
  机器段不进 `segments`，只留在 `lines` 里）；行号先按 offsets 的 `line_offset`
  绑回 (file, 文件内行)，且**只有 `role=source` 的行能 join** —— stdlib 前缀没有元件
  注释，它的行号与源文件同从 1 数，两者混在一个行号空间里就会错配；
- **偏移基准重映射**：实测 `.debug_line` 的地址是**对象 `.text` 节内偏移**（对象
  节 VMA=0；行程序首址处一条 `ADDR64 → .text` 重定位；`objdump` 不重定位），而装配
  器把 `.debug*` 全部丢弃（`simplexc/assemble/assemble.cc:163-172`）→ 必须靠 W1 另吐
  `<blob>.offsets`；W3 **只读不碰装配器**。格式（JSON，W3 独占消费）：
  ```jsonc
  { "version": 1, "artifact": {"len": …, "sha256": …}, "header_len": 8,
    "code_size": …, "entry": …, "body_end": …, "stubs_base": …, "slots_base": …,
    "line_offset": 1503,            // `--stdlib` 前缀的换行数：拼接后源码第 1 行 = line_offset+1
    "sources": [                     // 参与拼接的文件（无 --stdlib 时只有角色 source 一条）
      { "file": "stdlib_prefix.si", "lines": 1503, "role": "stdlib", "path": "…" },
      { "file": "hook_src_009.si",  "lines": 9053, "role": "source", "path": "…" } ],
    "sections": [ { "obj": 0, "obj_path": "module.obj", "sec": 1, "name": ".text",
                    "class": "text|rdata|data|bss", "blob_off": 0, "size": …, "align": 16 } ],
    "debug_lines": [ { "obj": 0, "obj_path": "module.obj",
                       "anchors": [ { "text_sec": 1, "blob_off": 0 } ] } ] }
  ```
  `sources[].file` = 文件名（W3 按它写 `lines[].file`），`lines` = 该文件的行数，`path` 可为空；
  `role=stdlib` 的 `lines` 取 `line_offset`（前缀占拼接坐标第 1..line_offset 行；前缀不以换行
  结尾时其末行与源码首行同占一行，这一行算前缀）。旧 offsets 没有这两个字段时 W3 按拼接后
  行号出 sidecar（不写 `file`）。
  `obj` = 装配入参下标（0 基），`sec` = COFF 节下标（1 基）；`anchors` 每条 = 一个
  带 `.debug_line` 的对象及其 `.text` 基址（首版单 `.text` 对象只有一条；多 `.text`
  按重定位锚点展开）。不变式：`sections` 按 `blob_off` 升序互不重叠、
  `blob_off+size <= body_end`；`code_size == len-8`；`stubs_base == body_end`。
  回退（W3 已验证可行）：从 COFF 节表复刻装配规则，唯一不可推导的是对象入参顺序；
  仅作降级路径，不与 offsets 同时启用；
- `stubs` 取现成 `<blob>.stubs` + 8 行换算（无新增装配工作）；
- 命令入口：`simplexc_cli --src x.si --out blob --obj-out coff`（现成）。

**ref Provider**（不在工具范围，只锁 schema）：魔数标记链（往 `.si` 每元件前插
store 魔数 → 机器码搜立即数锚点 → 「标记恒 17 字节」坐标变换；终点标记防末元件
越界）已有旧验证，翻成统一 schema（segments 直填、无 lines）。

## 7. 分析与报表（analysis，Python 标准库）

### 7.1 归因（无状态纯函数）

`samples.tsv` + `sidecar.json` + run 元数据 → 表。流程：bucket=0 的样本 → 二分
`segments` → 元件；二分 `lines` → 行（**全机器段行级明细的来源，表 3**）；未归属
桶由「有 stubs 切桩 / 剩余为公共根」二分得出。归因结果可整段落盘，换任何 sidecar
重算无需重采样——采样只跑一次。语言定案：**Python**——归因与报表迭代需求同源
（加视图/修口径频繁，跨语言边界成本远高于计算成本）。

### 7.2 四张热度表（全部新写；分母显式、口径沿用旧工具）

样本分母语义：**类型/实例/行级表（表 1–3）分母 = bucket=0 已归属样本**（沿用
`analyze_sim_hotspot.py:190` 的 `in_jit` 口径）；**分桶表（表 4）分母 = 全样本**。

1. **元件类型表**：`type` 聚合，占比（分母如上）；**按侧车台账列全部类型，含零值**
  （旧工具教训：只列采到的会让零采样类型整行消失）；
2. **最热 N 元件实例**：`type #id`、行区间、样本数、占比；N 可调；
3. **热点元件行级明细**：对最热元件展开行热点分布（路线 A 免费能力）；
4. **未归属分桶**：三桶总览（bucket0 已归属 / bucket0 内未归属=「外部调用桩」
 （由 `stubs` 切出，细分）+「公共/根代码」/ bucket1 残区 / bucket2 游戏引擎/调度/
   输入），含零值。

每张带：总样本数、窗口时长、artifact sha256、样本量置信提示、近似口径（freq，§9）。

### 7.3 主键与聚合纪律

- 实例主键 `(type, id)`；跨注释段聚合用 **id 合并键**（实例 = 台账条目，不按段聚合；
  段级数据始终在 `segments` 里保留）；id 唯一性见 §2（违反即报错）；
- 报表显示 `com_register_word #9`；中文名映射属**展示层**翻译，数据层不塞中文名。

## 8. 判据（工具自身的验收）

| # | 判据 | 跑法（命令级） | 时点 |
|---|---|---|---|
| G1 | 调试信息不改机器码 | `simplexc_cli --src simplexc/corpus/src/177.si --out g0.bin --obj-out g0.obj`，重复一次带 `--debug-info` 得 g1；`objdump -d g0.obj` vs `g1.obj` 只比 `.text`，diff 为空 | simplexc 判据流程常驻 |
| G2 | 侧车正确性三方抽查 | 抽样总体 = sidecar `segments` 均匀取 N 段（N 默认 20，可调）；对每段：`objdump --dwarf=decodedline <obj>` 输出 / sidecar 行 / `objdump -dl <obj>` 该段首指令行注，三方对齐（坐标按 §2；⚠️ llvm-dwarfdump 不在 `.deps/llvm/bin`——备选：W3 前置 `tools/fetch_llvm.py --extra` 拉一份，或全程 objdump） | ours Provider 构建后 |
| G3′ | 重复性 | 同一产物同参数背靠背两个 run；top-5 **按 (type,id) 集合重合数**（阈值验收时由负责人现场定；不比顺序）；同一脚本对两份 report 计算 | 每份产物首测 |
| G3″ | 回归对照 | ours 与 ref 各编同电路各采一次，热点分配显著差异人工审定 | 依赖 ref Provider（不在范围，暂缓） |
| G4 | 覆盖水位 | 三桶份额写入 run.toml（analysis 回写）；基线 = `workspace/baseline.json`（首跑建）：bucket=0 份额低于基线（阈值验收时由负责人现场定）→ 告警；etw 侧途中新开线程**样本丢失不可计数**，告警信号 = bucket 份额下降 + 首尾快照差集非空（§4.1） | 每次报表附带 |

aliasing 怀疑的处理路径：缩短间隔复跑比一份（§4.2）——G3′ 的变体用法。

## 9. 归一化（能量口径）

近似口径（首版）：`tools/read_freq.py`（`tools/freq/`）读的是游戏内存主频显示值
（被平滑、1 s 更新——负责人确证），run 目录记录 `freq_start / freq_end`（窗口首尾
各一次，两独立样本夹逼）+ 窗口时长 → 粗 tick 估算；结论精度口径在报表上标注
「近似」。

**待实验项**（不阻塞主线）：找游戏内部仿真 tick 计数器偏移（内存取证，与 freq 同套
方法），窗口前后取差值 → 精确 tick；存在与否不预设，一次实验即定；偏移随游戏进程
重启失效（老约束，进程占用自检语义）。

## 10. 数据存放与磁盘纪律

```
tc-toolkit/hotspot/
  config.example.toml  # 入库的默认/示例 config（键见下）
  config.toml          # 运行时配置，不入库（.gitignore）：artifact 目录、freq 外部命令、
                       # 游戏进程名、ETW 会话名、锁目录（game.lock 协议）、workspace 路径
  workspace/
    runs/<ts>_<artname>_t<sec>/     # profile bundle = 一个 run
       run.toml    # consumer `save` 初写（产物引用 path+sha256——**引用而不拷贝** hook 的
                   # .bin；采样参数；freq_start/freq_end），analysis 补写 buckets{}
       samples.tsv
       sidecar.json（同产物多 run 可引用共享侧车；亦允许拷入以便离线）
       report.md + report.html   # analysis 产出：文本归档版 + 静态交互报告（双击即看）
    baseline.json                # G4 基线（analysis 读写）
    index.html                   # 工作区索引：列全部 run（时间/产物/freq/bucket0 份额）→ 链到各报告
  <产物库同步>                   # hook 落盘的 .si/.bin 依旧在 work/hook/，工具**只读**
```

**`config.toml` 键（默认值；W2 冻结；运行时配置不入库，仓库只放 `config.example.toml`）**：

| 键 | 类型 | 默认 | 语义 |
|---|---|---|---|
| `game.exe_name` | str | `"Turing Complete.exe"` | 目标进程名 |
| `sampler.backend` | enum | `"etw"` | `etw` \| `suspend` |
| `sampler.interval_ms` | int | `1` | 采样间隔（etw 量纲待实测） |
| `sampler.window_sec` | int | `10` | 窗口默认时长 |
| `sampler.suspend_probe_rounds` | int | `40` | suspend 锁定仿真线程的粗采轮数 |
| `etw.session_name` | str | `"hotspot"` | ETW 会话名 |
| `lock.dir` | str | `work/tmp/locks` | `game.lock` 所在目录 |
| `paths.workspace` | str | `hotspot/workspace` | run 归档根 |
| `paths.artifacts` | str | `work/hook` | 只读产物库 |
| `freq.command` | str | `python tools/read_freq.py --samples 1` | 外部频率读数命令 |
| `freq.timeout_sec` | float | `5` | 单次读数超时 |

**`run.toml` 键（consumer 初写，analysis 只回写 `buckets`）**：

| 键 | 类型 | 语义 |
|---|---|---|
| `schema` | int | run 格式版本（=1） |
| `created_at` | str(ISO) | 落盘时刻 |
| `artifact.path` / `artifact.sha256` / `artifact.file_len` / `artifact.code_len` | str/int | 产物引用（**不拷贝**；sha 含 8 字节头；code_len = file_len-8） |
| `artifact.num` | str\|null | hook 编译点编号（三位） |
| `locate.bases` | list[str] | 全部整段校验通过的副本基址（hex） |
| `locate.feature_off` | int | 特征窗偏移（默认 `0x400`） |
| `window.sec` / `window.interval_ms` / `window.sample_count` | num | 实际窗口参数与样本数 |
| `window.tids` | list[int] | etw 首尾快照并集 |
| `freq.start_hz` / `freq.end_hz` / `freq.start_at` / `freq.end_at` | float | 窗口首尾各一次读数与时刻 |
| `freq.interval_sec` | float | 两次读数间隔基准（默认 1.0） |
| `buckets` | map | analysis 回写三桶份额 |
| `tick.start` / `tick.end` | int\|null | 精确 tick（待实验，未定） |

- runs/ 与 `config.toml` 不进 git（.gitignore）；仓库只放 `config.example.toml`；
- 全部路径（产物、freq 命令、游戏进程名、会话名）走命令行参数或 `config.toml`
 键（上表），**工具内无硬编码路径**；路径 layout 迁移只改参数、不改代码。

## 11. 使用方式（人工流程与 CLI 形状；命令参数名待负责人过目）

流程假设游戏已人工就绪（启动→载入电路→电路在跑），全程四步：

```bash
# (0) 编译产物 + 调试信息 + 元件表 + 重映射基准（后三者均为 W1 新增）
simplexc_cli --src circuit.si --out c.bin --obj-out c.obj \
             --debug-info --elements-out el.tsv --offsets-out c.offsets

# (1) 生成侧车（W3，纯离线；--offsets 为 W1 产出的基准文件，回退模式可省）
python hotspot/tools/make_sidecar.py --blob c.bin --obj c.obj \
       --elements el.tsv --offsets c.offsets --out c.sidecar.json

# (2) 常驻采样会话（W2）——持 work/tmp/locks/game.lock；识别/定位一次，多窗口复用
hotspot-consumer                        # 进入 REPL（准确参数形状以 W2 落地为准）
> open c.bin [--pid N] [--interval-ms 1]  # 定位 + 枚举线程并缓存
> window 10 save                        # 采 10 s 并落 run 目录（samples.tsv+run.toml）
> window 10 save                        # 再采一次（G3′ 重复性 = 两次 save 对比，§8）
> quit

# (3) 报表（W4）：对一个 run 目录产出两份报告并回写 buckets
python hotspot/analysis/report.py --run workspace/runs/<run目录>

# (4) 查看：浏览器打开 workspace/runs/<run>/report.html（静态交互）；
#     workspace/index.html 是全部 run 的索引页（W4 产/更新）
```

- 报告双形态：`report.md`（文本归档/引用）+ `report.html`（交互：四表 tab、排序/
  筛选、占比条、点元件展开行级热点；页面集合为草案，开工前经负责人过目）；
  差异对比页（两个 run 并排）同属 W4；
- 桌面 GUI = 保留意见、暂不实现（§1 同口径）；动态交互只存在于静态 HTML 与常驻
  REPL 两处；
- 所有单环可用对应命令单飞重跑（report.py 对 run 目录幂等）。

## 12. 工作分解（W；不随本 spec 开工）

| 步 | 内容 | 检收 |
|---|---|---|
| W1 | 调试信息实现（C API）：DIBuilder 基建（module flag 关键：Debug Info Version=3、**Dwarf Version=4 显式置位**）/ DebugLoc 行号 / 元件表 el.tsv（源文件原文解析，每注释段一行；`--elements-out`）/ `--debug-info` flag / 装配器吐 `<blob>.offsets`（`--offsets-out`）/ G1 挂判据 | G1 绿 + 元件表抽查 |
| W2 | consumer：共享聚合层 / suspend 后端（已落地）/ etw 自写（实时，§4.1）/ 常驻 REPL（open/window/save，含 `open --interval-ms`）+ 外部 freq 命令进 run.toml + 单样本暂停时长实测回填（§4.0） | 编译跑通；解码单测（固定字节→IP/TID）+ 事件注入单测（合成 (IP,TID) 序列喂聚合/分桶函数）；探针确认真实事件；真样本冒烟需持 `work/tmp/locks/game.lock` |
| W3 | ours join 脚本（读 obj × el.tsv × `.offsets`，产 lines/segments/stubs/meta；**只读装配器产物**）+ G2 抽查（objdump decodedline 优先） | G2 绿 |
| W4 | 归因 + report.md / report.html（四表交互页 + 对比页，页面集草案待负责人过目）+ workspace index.html + G3′/G4 + baseline.json | G3′ 重合数由负责人现场定 + 四表人工抽认 |

依赖：W1 → W3（无行表就没有侧车）；W2 独立可与 W1 并行；W4 靠 W2（run 目录）与
W3（侧车）产物。采样后端已定双后端、etw 为主（§4.0）。

## 13. 未证项与遗留验证

- 注释第一段数字（= id，用途已定）：**其语义未证**，与末段同口径；
- 注释第四段大数字：语义未证，原样存侧车 `tail`；
- 注释可选第五段（BANK/Input/Output 这类词）：语义未证，主规则不依赖；
- 采样间隔下限与单位：`TraceSetInformation` 量纲暂按 100 ns 折算（1 ms=10000），**下限实测回填**（§4.1）；
- etw 途中新线程样本不可计数（§4.1）——影响 G4 信号形态，不影响归因正确性；
- suspend 单样本暂停时长上界（µs 或几十 µs）W2 实测回填（§4.0）；
- tick 计数器：存在与否不预设，一次内存取证实验即定（§9）；

## 14. 风险清单

1. etw 自写的事件 GUID/ID/布局/API 行为需探针实测（§4.1）；未钉死前不接产品；
2. `.debug_line` → blob 偏移重映射跨段修正不平凡——已划 W1 吐基准（§6），W3 有
   独立回退（自行从 COFF 装载地址推导）；
3. aliasing 锁相（1 ms = 10⁴ × tick 是整数倍）——相位依赖调度抖动；有疑缩短间隔
   复跑（§4.2、G3′ 变体）；
4. TID 复用/同名多开进程导致样本归属错（etw 快照层报错优先，不猜）；
5. 多 blob 副本对归因桶的污染（§4.3 全算 blob 内，G4 水位自证）；
6. 元件行区间末端吞公共代码（§2 行区间规则 + 终点截断，历史 89% 虚高坑）；
7. suspend 每样本暂停开销在 ‰–% 量级（上界未测，§4.0）——G3′ 重合率作兜底；
8. 修改间隔需重建 etw 会话（§4.1）——把「逐窗口调节」需求挡在会话外。
