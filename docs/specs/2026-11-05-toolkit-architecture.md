# 设计：tools/ 的架构分层（libs / services / native / perf_opt）

- 日期：2026-11-05
- 状态：**草案 v2**（未提交、未批准）。只有 §7 的迁移计划，代码一行未动。
- 目标：把"库、服务、产品、一次性脚本、C 机制"五类混住的现状分开，使**移除某项能力是局部改动**（§8 的删除演练即验收判据）。
- 引用坐标：git 根（`tc-toolkit/`）相对路径，冒号后为 1 基行号。
- v2 变更（2026-11-05）：按独立审查的 20 条意见修正——失效引用、`cfg` 删除与演练预算的矛盾、读侧"未知即失败"与现有"只加不改"的冲突、`work/hook` 路径收敛与 `hook_log` 的 mtime 设计冲突、各步判据的可执行性。
- v3 变更（2026-11-05）：按"只读文件、不看代码"的复述测试修正 12 处歧义与 9 处卡点——箭头方向改写为文字、`combile()` 笔误、空值传递与单边键给出裁决、子命令数统一为四个、测试文件归属、演练 1a/1b 与步 3/3b 的映射、游戏未运行的判据。

## 1. 目标与非目标

**目标**

- 四个在用的产品各有入口与产物，彼此不 import：转发层（`tools/deploy_compile_hook.py` + `tools/compile_hook.c`）、热点分析（`hotspot/run.py`）、主频被动监听（`tools/watch_freq.py`）、判据（`simplexc/tools/check.py`）。
- 共用能力有归属：Python 库进 `libs/`，跨产品能力进 `services/`，C 机制与服务进 `native/`。
- 同一份格式只有一个读取点：配置由读它的那一侧读一次，其余靠传参（§5.3）。
- 编译前优化（选实现：`ref` / `ours`）与编译后优化（改写产物）都能自由组合，且各自可整体移除（§8）。

**非目标**

- 不改游戏侧行为：转发层照样通过 `compile.dll` 名字被加载，产物语义不变。**例外只有两处**：`rewrite=` 从 `cfg` 行搬到 `impl` 行；`cfg` 事件删除。
- 不动 `work/` 与 `hotspot/workspace/` 的目录布局；`runs/` 留在热点产品内。
- 不引入 `pip install`、不引入生成式代码。Python 侧只用自己的类型与常量声明契约。

## 2. 现状：五类东西混在 `tools/` 顶层

| 事实 | 证据 |
|---|---|
| 顶层 33 个 .py 平铺，没有 `tools/__init__.py`；互相靠 `sys.path.insert` + 裸 import | 全仓 `sys.path.insert` 共 19 处：`tools/` 15、`simplexc/tools/` 3、`work/tmp/` 1（临时脚本） |
| 同一个 `tc_common` 有三种加载方式：`sys.path` 裸 import、`importlib` 按文件路径、包内相对导入 | `tools/deploy_compile_hook.py:62`、`hotspot/run.py:85-92`、`tools/freq/cli_watch.py:27` |
| `simctl` 被两种形态使用：`hotspot` 以子进程调脚本，`freq` 以 import 调函数 | `hotspot/run.py:38`、`tools/freq/driver.py:22` |
| 一个 C 文件同时承载三件事：转发机制、compile 服务、改写器调用 | `tools/compile_hook.c:161`（机制）、`:895`（`compile` 导出）、`:709`（改写器） |
| C 依赖 simplexc 的 C 边界，路径靠相对目录数层级 | `tools/compile_hook.c:187` |
| 构建归属错位：`tools/` 的 C 由 `simplexc` 的 CMake 编（两处引用） | `simplexc/CMakeLists.txt:230`、`:483` |
| 配置记录与"编译后优化"耦合：`cfg` 日志在 `optimize_inplace()` 里写 | `tools/compile_hook.c:731`、`:1024` |
| 合法值集合有两处定义 | `tools/optimize_image.py:575`、`tools/freq/variants.py:24` |
| 兜底值两处不一致：`variants` 说 `on`，改写器说 `off` | `tools/freq/variants.py:27`、`tools/optimize_image.py:637` |
| `work/hook` 在 Python 侧是"一处常量 + 三处字面量/兜底"，且 `hook.log` 另有一套 NEW/LEGACY + mtime 选边 | `tools/freq/products.py:39`（常量）、`tools/tc_common.py:295`（兜底实参）、`hotspot/run.py:524`、`tools/hook_log.py:4-9`、`:63-78` |

## 3. 目标结构

```text
tc-toolkit/
├── libs/                      # Python 库：被所有层依赖，自己不依赖任何工具
│   ├── tc_common.py           # 环境发现：游戏目录 / 版本 / 工具链
│   ├── winmem.py              # 进程与内存原语
│   ├── term.py                # 终端输出
│   ├── paths.py               # work/ 路径的唯一解析点（含 hook.log 的 NEW/LEGACY 与 mtime 规则）
│   └── hook_log.py            # hook.log 的读写 + 契约声明（§6.1）
├── services/                  # Python 服务：被 ≥2 个产品共用
│   └── simctl.py              # 仿真控制（热点与主频都要）
├── native/                    # C 侧：一个 dll 一个 DllMain，内部按依赖分三层
│   ├── forward/               # 机制：取原实现、落盘、日志、哈希、清理、配置读取
│   ├── compile/               # compile 服务：ABI、锚点、manifest、后端分流、处理器链
│   └── backend/               # 后端实现：simplexc.c（含 simplexc_core.h）、ref.c（转发真实 dll）
├── tools/                     # 用户命令（薄壳）
│   ├── perf_opt/              # 面向用户：构建 / 部署 / 配置 / 编译后优化（§4）
│   ├── freq/                  # 主频测量包（现状保留，§5 只加入口定性）
│   └── watch_freq.py          # 产品入口；read_freq.py 为服务壳；bench_rewrite.py 为实验脚本
├── decompile/                 # 逆向脚本：零产品依赖，不参与依赖规则
├── hotspot/                   # 产品（现有独立 CMake）
└── simplexc/                  # 产品（现有）
```

`native/` 与 `libs/` 不是同层对等物：`libs/` 是 import 边界（Python 包），`native/` 是编译/链接边界。对等物是"`libs/` 里的 Python 库"与"`native/` 里的 C 库"。

`libs/paths.py` 只集中**解析逻辑**，不把路径变成常量：`hook.log` 有 NEW/LEGACY 两个候选、按 mtime 选边（`tools/hook_log.py:4-9`、`:63-78` 的既有设计保留），其它模块调用它取路径。

## 4. `tools/perf_opt/` 的内部结构

```text
tools/perf_opt/
├── cli.py         # 用户命令：build / deploy / restore / status
├── build.py       # 转调 simplexc/tools/build.py（现状 tools/deploy_compile_hook.py:111-116）
├── deploy.py      # 拷贝 dll、备份还原、opt.ini 模板
├── config.py      # opt.ini 的唯一读取点
└── rewrite.py     # 编译后优化入口（可删单元，§8 演练 2）
```

`perf_opt` 只做**产物装配**与配置读写，不拼接源码：`native/compile/` 直接 include simplexc 的 C 边界，二者同属一张 CMake 图（§5.2）。

## 5. 依赖规则与机制

### 5.1 方向

- 单向：产品 → 服务 → 库。C 侧：`native/forward` 不依赖另两层；`native/compile` 依赖 `forward`；`native/backend` 实现 `compile` 用的后端接口。
- 产品之间不 import；需要共用就下沉到 `services/`（这是"层数会长出来"的唯一机制）。
- 跨语言只经文件（`work/`、`hook.log`、`opt.ini`）与进程；Python 不 import C，C 不 import Python。

### 5.2 C 侧三个接口

```c
/* native/forward/forward.h —— 机制 */
void *forward_real_symbol(const char *dll, const char *name);   /* 原实现 */
int   forward_dump(const char *tag, const char *ext, const void *p, long long n, long seq);
void  forward_log(long seq, const char *event, const char *kv);
int   forward_config_get(const char *key, char *buf, size_t cap);  /* 进程内唯一读取点 */

/* native/compile/backend.h —— 编译器后端（可换 simplexc） */
typedef struct {
    const char *name;
    const char *(*compile)(const char *src, long long len, const char *stdlib,
                           void **blob, long long *n, long long *entry);
    void        (*free_blob)(void *blob);
    const char *(*last_error)(void);
    const char *(*last_warnings)(void);
} compiler_backend;

/* native/compile/processor.h —— 产物处理器（可换 / 可删） */
typedef struct {
    const char *name;                    /* "rewrite" */
    void (*init)(const char *mode);      /* 由服务读一次配置后传入，处理器不读文件 */
    void (*process)(void *buf, long long len, long seq, void *out);
} forward_processor;
```

`native/backend/simplexc.c` 是唯一 include `simplexc/shell/simplexc_core.h` 的文件（现在这行在 `tools/compile_hook.c:187`）。更换编译器实现 = 换这个文件 + 注册一行。

三部分编进**同一个** `SHARED` target：一个 dll 只能有一个 `DllMain`，它归 `native/forward`；构建定义仍在 simplexc 的 CMake 图里（要链 C++ 核心与 LLVM-C）。

### 5.3 配置读取：一处读、其余传参

- C 服务读 `opt.ini`（`forward_config_get`），把值传给处理器与后端；处理器与后端不打开文件。
- 游戏内链路**在配置值非空时**已经是这样：C 读到 `rewrite` 后通过 `--mode` 传给改写器（`tools/compile_hook.c:736`），改写器随即不读文件（`tools/optimize_image.py:579` 的优先级 `--mode > OPT_REWRITE > opt.ini`）。
- **空值缺口，按此裁决**：C 读到 `rewrite` 为空时按 `off` 处理，并**显式传 `--mode off`**。改写器只要收到 `--mode` 就永不读文件；这样"单一读取点"才闭合（现状是空值不传、改写器回退读 `opt.ini`，见 `tools/optimize_image.py:592`）。
- `perf_opt` 离线操作时同理：`config.py` 读一次，传参给 `rewrite.py`。
- 兜底值只有一处：`tools/freq/variants.py:27` 的 `on` 改为与改写器一致的 `off`。

### 5.4 迁移期的 import 方式

`libs/` 在根目录后，`python tools/x.py` 的搜索路径是 `tools/`，找不到 `libs`。迁移期统一用"每个入口补一行 `sys.path.insert(0, ROOT)`"（现有 19 处这种做法），先把目录摆对；统一成 `python -m` 或安装形态属于简化期，不在本 spec 范围。

## 6. 契约

### 6.1 `hook.log`

- 权威表放进 `libs/hook_log.py`（现在只有一段 docstring 描述，`:13-33`）：事件名、每个事件的键集、**以及每条事件在什么条件下写**。
- 全收现状：`cfg`、`impl`（含 `stdlib`、`stdlib_raw`）、`opt`（含 `cse`、`reloc`、`note`）、`entry`、`warn`、`error`、`writeback`。
- **未知的处理分两级**（与现有 `tools/hook_log.py:24-26`「只加不改，未知事件与键一律忽略」兼容，不能一刀切 raise）：
  - 未知**事件名** → `raise`：事件是有限集合，出现未知的就是契约漂移，必须暴露。
  - 未知**键** → 首次 `warn` 并继续解析：C 侧加键是常态（`stdlib_raw`、`guard=veh` 都是后加的），raise 会让每次加键都炸掉所有消费者。
- **`rewrite=` 并进 `impl` 行**，`cfg` 事件删除。关键约束：`rewrite=` **只在起过改写器的那一趟出现**——现在"有 `cfg` 行 = 起了改写器"是消费方判据（`tools/freq/products.py:112`、`tools/freq/test_freq.py:260`），并入 `impl` 后必须保留这个区分度，否则归因逻辑失效。
- 语义也进表。`impl` 行有一条没写明但被依赖的约定——「只有我们的实现会把写完记在 `impl` 行上」（`tools/freq/products.py:205`）。不写进表，哪天 `ref` 路径也开始写 `impl` 行，归属会静默错且测试覆盖不到。
- 迁移顺序：先把 `cfg` 的写点从 `optimize_inplace()` 移到编译出口（不删、不改消费者），再并入 `impl`、删事件（§7 步 3 与步 3b）。

### 6.2 `opt.ini`

- 不拆文件。**本期只做"`[opt]` 段内注释分区**（"转发层读的键"/"改写器读的键"）。将来若拆成 `[compile]` / `[rewrite]` 两段，改动面固定为：C 侧 5 处 `GetPrivateProfileStringA("opt", …)`（`tools/compile_hook.c:173`、`:204`、`:223`、`:281`、`:726`）与 `tools/freq/variants.py:29` 的 `OPT_SECTION`。本期不动。
- 键：`compiler`（ref/ours/ours_with_fallback）、`stdlib`（on/off）、`rewrite`（off/on/short/short_cse）、`toolkit_root`、`ours_timeout_ms`。
- 单边键的裁决（待批准）：`stdlib` 保留（只影响编译行为，Python 侧从 manifest 列读结果）；`ours_timeout_ms` 保留但补进 `opt.ini.example` 与注释；`toolkit_root` **删除**——Python 全部按仓库相对路径，保留它只会制造“设了它、C 写到别处、Python 静默读旧日志”的陷阱。验证：删后 `grep -rn toolkit_root --include=*.py --include=*.c .` 无输出，且 `python -m unittest libs/test_tc_common.py` 与 `check.py --only hook_offline` 通过。

### 6.3 `work/` 与产物

- 布局不动。`work/hook` 的路径解析集中到 `libs/paths.py` 一处（保留 NEW/LEGACY + mtime 选边），`tools/freq/products.py:39`、`tools/tc_common.py:295` 改为调用它。
- 转发层加载时清空 `work/hook` 与 `work/opt` 的规则保留（`tools/compile_hook.c:826`、`:861`）。

## 7. 迁移计划（先迁移，后简化）

| 步 | 内容 | 判据（全部不需要游戏在运行） |
|---|---|---|
| 1 | 建 `libs/`、`services/`；原样搬 `tc_common`、`winmem`、`term`、`hook_log`、`simctl`，并把 `tools/test_tc_common.py` 随 `tc_common` 搬入 `libs/`。改动面：`sys.path.insert` 19 处（`tools/` 15、`simplexc/tools/` 3、`work/tmp/` 1）；`simplexc/tools/` 的 `tc_common` 引用 8 处（3 处 `from tools.tc_common import`：`simplexc/tools/equiv_build.py:59`、`simplexc/tools/hook_offline.py:114`、`simplexc/tools/probe_006.py:58`；5 处 `_load_by_path`：`simplexc/tools/abs_scan.py:50`、`simplexc/tools/classify_abs_addrs.py:46`、`simplexc/tools/recompile_corpus.py:57`、`simplexc/tools/m5/batch.py:76`、`simplexc/tools/verify_game_contract.py:82`）；`simplexc/CMakeLists.txt:376` 的 `import tc_common`；`tools/freq/test_freq.py:25-26` 的 `from tools import hook_log` | `python -m unittest libs/test_tc_common.py tools/freq/test_freq.py`、`python -m unittest hotspot/tests/test_run.py`、`python simplexc/tools/check.py --list`、`python tools/watch_freq.py --help`、`python hotspot/run.py --help` |
| 2 | `hotspot/run.py` 由"子进程调 `tools/simctl.py`"改为 import `services.simctl` | `python -m unittest hotspot/tests/test_run.py` |
| 3 | `cfg` 的写点从 `optimize_inplace()` 移到编译出口（`compile()` 内写 `impl` 行处附近），**不改消费者**。对应演练 1a | `python simplexc/tools/check.py --only hook_offline`（离线壳也走这条路径，能覆盖） |
| 3b | `rewrite=` 并入 `impl` 行，删 `cfg` 事件；改 `tools/freq/products.py`、`report.py`、`cli_watch.py`、`test_freq.py`、`tools/freq/__init__.py:7` 的文档行。对应演练 1b | 同一批 unittest + `check.py --only hook_offline` |
| 4 | 建 `decompile/`，搬逆向 7 个：`nim_demangle`、`demangle_dll`、`coff_symbols`、`organize_strings`、`pe_overlay`、`find_sym`、`extract_nim_strings`（`pdata_bounds` 例外：`simplexc/tools/verify_game_contract.py` 依赖它） | 7 个脚本各自 `--help`；`python simplexc/tools/check.py` |
| 5 | 建 `native/{forward,compile,backend}`；`compile_hook.c` 原样搬进 `compile`；`#include` 改 include 目录；CMake 改两处源路径（`simplexc/CMakeLists.txt:230`、`:483`）；同步 `simplexc/tools/hook_offline.py:121`（`HOOK_SRC`）与 `:127`（`BASELINE_SRC` 的 `tools/compile_hook.c` 路径） | 游戏**未运行**时：`python tools/deploy_compile_hook.py build-deploy`、`python simplexc/tools/check.py --only hook_offline`、`python simplexc/tools/check.py`（全量，确认步 4 的判据没被步 5 破坏） |
| 6 | 建 `tools/perf_opt/`，把部署/配置/改写入口并进去 | 四个子命令 `build` / `deploy` / `restore` / `status` 的 `--help` 与 `status` 的输出与现状一致；跑对：重跑 `check.py` 全量 |
| 7 | 简化：抽 `forward.h` / `backend.h` / `processor.h`，拆 `compile_hook.c` 三部分 | 编译通过；§8 的 1b 与 2 成立（1a 已在步 3 完成） |

"游戏内编译一次并观察产物与 `hook.log`" 是人工验收，不写成自动判据（转发层 dll 有文件锁，部署要求游戏不在运行）。"游戏是否在运行"的判据：`tasklist /FI "IMAGENAME eq Turing Complete.exe" /NH` 无输出即未运行。

第 1、5 步是全量的（改路径），其余是局部的。每步一个提交，判据不过就 `git revert`。

## 8. 验收：删除演练

| 演练 | 允许改动的文件 | 通过条件 |
|---|---|---|
| 1a 把 `cfg` 写点移出 `optimize_inplace`（= 步 3 的验收） | `native/compile/` 内 1–2 个翻译单元 | 改动落在 1–2 个翻译单元内；`check.py --only hook_offline` 通过；消费者零改动 |
| 1b 删 `cfg`、`rewrite=` 并入 `impl`（= 步 3b 的验收） | `libs/hook_log.py`、`native/compile/` 内 2 个文件、`tools/freq/{products,report,cli_watch,test_freq}.py`、`tools/freq/__init__.py` | 逐文件计 ≤ 8 个文件（含测试与文档行）；`hook.log` 完整解析；rewrite 列仍取到值；"有 `rewrite=` = 起了改写器"的判据仍成立 |
| 2 删编译后优化（= 步 7 之后的演练） | `native/compile/processor_rewrite.c`（+ 一行注册）、`tools/perf_opt/rewrite.py`、`opt.ini` 的 `rewrite` 键 | ≤ 5 个文件；转发层编译通过；`compiler = ref` 与 `ours` 都能编译；`hook.log` 不缺必需事件 |

现状过不了 1a：`cfg` 的写点在 `optimize_inplace()` 内（`tools/compile_hook.c:731`）。

## 9. 已确认的决策

| 决策 | 结论 |
|---|---|
| `libs/`、`services/` 位置 | 仓库根，与 `tools/` 平级 |
| 逆向脚本目录 | `decompile/` |
| `work/` 与 `hotspot/workspace/` | 不动；`runs/` 留在热点产品内 |
| `tools/freq/` 是否拆包 | 不拆。产品只有 `watch_freq`；`read_freq` 是服务壳；`bench_rewrite` 是实验脚本 |
| `perf_opt` 定位 | 面向用户的程序：构建 simplexc、部署转发层、读写配置、编译后优化 |
| 编译后优化 | 保留、默认可组合（`ref + rewrite` 仍支持），但默认 `off` |
| `cfg` | 删；`rewrite=` 并入 `impl` 行，且只在起过改写器时出现 |
| 处理器读配置 | 服务读一次传参（方案 A）；处理器不读文件 |
| `native/` 位置 | 仓库根 |
| 静态检查 | 本期不做，靠 §8 的删除演练与现有的 `check.py` 判据 |

## 10. 待定

- `backend.h` / `processor.h` 是第 5 步就抽，还是第 7 步再抽；第 5 步至少要保证 simplexc 的调用集中在一个文件里。
- `decompile/` 里的 7 个脚本哪些还要维护、哪些只是存档。
- `work/hook` 路径集中到 `libs/paths.py` 后，`tools/profile_by_image.py`（旧热点管道）是否随之删除。
- `opt.ini` 拆成 `[compile]`/`[rewrite]` 两段：本期不做，改动面已列在 §6.2，将来单独一次改动。

## 11. 证据索引

- `tools/compile_hook.c`：`:119` OPT_SCRIPT、`:161` 路径初始化、`:187` include 核心头、`:709` 改写器、`:731` `cfg` 行、`:736` 空值不传 `--mode`、`:895` `compile` 导出、`:1004` `impl` 行、`:1024` 改写器调用点
- `tools/deploy_compile_hook.py:111-116`：转调 `simplexc/tools/build.py`
- `simplexc/CMakeLists.txt:230`、`:483`：`hook` 与 `hook_offline_new` 两个 target 引用 `tools/compile_hook.c`；`:376` 的 `import tc_common`；`:466-480` 离线壳路径宏
- `simplexc/tools/hook_offline.py:121`（`HOOK_SRC`）、`:127`（`BASELINE_SRC`）、`:119`（`STUB`）
- `hotspot/run.py:37-38`、`:85-92`；`hotspot/config.ini:25`；`hotspot/consumer/src/session.cc:288`
- `tools/freq/variants.py:24`、`:27`、`:29`、`:102-103`；`tools/optimize_image.py:575`、`:579`、`:592`、`:637`
- `tools/freq/products.py:39-40`、`:112`、`:205`、`:266-268`；`tools/freq/report.py:83-94`；`tools/freq/cli_watch.py:44-63`
- `tools/hook_log.py:4-9`、`:13-33`、`:24-26`、`:55-57`、`:63-78`；`tools/tc_common.py:295`
- `simplexc/tools/verify_game_contract.py`（依赖 `pdata_bounds`）
