# 地图

项目由哪些东西组成、东西在哪。**找不到东西时读这里**。靠自觉维护——路径指错了，一用就撞上。

## 环境

- 游戏：`python tools/tc_common.py` 打印本机目录
- 架构：Nim 编译的 exe + `game_engine.dll` 宿主 + 自研编译器 `compile.dll`（exe 里取不到版本串，别凭记忆写）
- ⚠️ **Steam 更新可能让全部函数地址偏移**—— 动手前先比对 `tc_common.game_build_id()` 与 `game_fingerprint()` 有没有变
- 工具链：`g++` / `objdump` / `nm` 从 **PATH** 找（MinGW-w64 GCC 14+）、Python 3.13
- 构建脚本在各自程序目录：`simplexc/CMakeLists.txt` + `simplexc/CMakePresets.json`；将来每个工具一套，互不共享 CMakeLists
- `cmake/` 是共享构建模块（Find 模块 + toolchain），各工具通过 `${CMAKE_CURRENT_SOURCE_DIR}/../cmake` 引用，不要复制

## 目录

**分层与依赖规则见各目录的 `README.md`**：`libs/`、`services/`、`native/`、`tools/`、`tools/perf_opt/`、`tools/decompile/`。根 `docs/map.md` 只回答“东西在哪”。

```text
libs/         被所有层依赖的 Python 库（环境发现 / 内存原语 / 终端 / hook.log / work 路径）
services/     被 ≥2 个产品共用的 Python 能力（simctl）
native/       C 侧：forward/（机制）+ compile/（服务）+ backend/（编译器实现）
tools/        用户敲的命令；实现去 libs/ 与 services/
  perf_opt/     转发层用户入口（构建 / 部署 / 配置 / 编译后优化）
  freq/         主频测量包（watch_freq / read_freq / bench_rewrite 的实现）
  decompile/    反编译与 PE 解析脚本（零产品依赖）
cmake/        共享构建模块（Find 模块 + toolchain），各工具共用，不要复制
docs/         本套文档（specs/ 设计文档 · facts/ 分类事实账本，分流入口 facts.md · bugs/ 语料差异与判据事故 · notes/ 过程记录）
simplexc/     自研编译器（**接口权威 = docs/specs/2026-10-15-simplexc-interfaces.md**，冻结版 1.4）
  frontend/ sema/ codegen/ runtime/ assemble/ shell/ cli/   代码（四层已分开：frontend → sema → codegen → assemble）
  tools/     语料重编译、地址分类、判据入口、差分宿主等脚本    corpus/ 98 份语料 + manifest（ref/ 不进 git）
             M3 的靶子固定为 corpus/rv32im/（RV32IM 电路的源码 + 参考产物快照，见该目录 README）
  dossier/   语义档案：访存宽度、div 符号、UL 各档（加载存储 / 窄化 / 提升截断 / 移位除法）、M3 构造表
  work/      本机中间产物（不进 git）
  build/     编译产物：compile_hook.dll / simplexc_cli.exe / objs/ / shell/ / cmake/（不进 git）
work/hook/    转发层捕获：hook_src_NNN.si ↔ hook_out_NNN.bin（同号即一对）
work/opt/     转发层工作区：NNN.raw.bin / NNN.opt.bin / hook.log / opt_stderr.log
work/freq/    主频采样输出：freq_log.tsv（事件）与 freq_summary.txt（给人读）
work/symbols/ 符号表（旧工具产物，不再生成）        work/maps/   组件映射表（旧热点管道产物，不再生成）
work/reports/ 分析报告（历史）      work/tmp/    一次性中间产物
recovered/    反编译产物（不进 git，可由 tools/decompile/ 重新生成）
```

`work/hook/` 与 `work/opt/` 在**转发层加载时**清空（`native/compile/compile_hook.c` 的 `purge_captures` / `purge_workdir`）；
`work/freq/` 跨会话保留。

## 七条工作线

按依赖顺序排列（左边是下层，给右边供货）：

```
sim_ctl ──→ 机器码改写 ──→ 主频监听 ──→ 热点分析 ──→ compile.dll 转发 ──→ 游戏反编译
（让上面能自动跑）  （改产物）    （测效果）    （用产物做组件级分析）（供源码+产物）（已降级）
```

| 线 | 做什么 | 状态 | 结论在哪 |
|---|---|---|---|
| **sim_ctl** | 脚本化「重编译 / 运行 / 停止」，供实验脚本 import | 已建成 | `facts/corpus-host.md` §simctl |
| **机器码改写** | `perf_opt/optimizer/core.py` 改写产物：四态、两条规则、锚点协议 | 已建成（默认 `rewrite = off`） | `facts/perf-tooling.md` §机器码改写 |
| **主频监听** | `read_freq` / `watch_freq` 被动记录实测主频 | 已建成 | `facts/perf-tooling.md` §主频读数 |
| **热点分析** | `hotspot/run.py` 一条命令：编译 → sidecar → 采样 → 报表（组件级时间归因） | 已建成；新工具设计中，见 `docs/specs/2026-11-01-hotspot-profiler-design.md` | `facts/perf-tooling.md` §组件映射、§采样 |
| **compile.dll 转发** | 转发层劫持 `compile()`，拿源码与产物 | 已建成 | `facts/forwarding.md` §compile.dll 转发层 |
| **游戏反编译** | 反编译 exe/dll、符号表、字符串 | **已降级，非主线** | `facts/perf-tooling.md` §静态逆向 |
| **simplexc** | 自研编译器：Simplex → LLVM IR → x64 装配，替代 `compile.dll` | **暂停** | `docs/notes/2026-11-02-simplexc-progress.md`、`facts/ref-compiler.md` |

## 判据入口

| 判据 | 命令 |
|---|---|
| 一条命令跑现有全部（7 步，`--list` 有清单） | `python simplexc/tools/check.py` |
| Steam 更新后契约重验（主设计 §10 的 A–E） | `python simplexc/tools/verify_game_contract.py` |
| M1.5 单跑（逐 tick 等价 + 吞吐） | `python simplexc/tools/probe_006.py --ticks 10 [--variant hot]` |
| 壳的离线失败路径（11 个 case） | `python simplexc/tools/hook_offline.py` |

## 工具清单

工具的用途、状态与技术债见 `TOOLS.md`（工具文档，按主题组织）。
