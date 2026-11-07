# tc-toolkit

逆向工程的**主工作区**（独立 git 仓库）。通用规则见 `../AGENTS.md`；这里只写本仓库的目录与分层。

## 目录

| 目录 | 是什么 | 依赖谁 |
|---|---|---|
| `libs/` | Python 库：环境发现、`winmem`、`term`、`hook_log`、`work/` 路径 | 只标准库 |
| `services/` | 被 ≥2 个产品共用的能力（如 `simctl`） | `libs` |
| `native/` | C 侧：`forward/`（机制）→ `compile/`（服务）→ `backend/`（`simplexc` / `ref`） | Windows API / 前一层 |
| `tools/` | 用户敲的命令；**不放库** | 上面三层 |
| `hotspot/`、`simplexc/` | 产品 | 同上 |

`tools/` 下：`perf_opt/`（转发层入口 + 编译后优化）、`freq/`（主频测量包）、`decompile/`（PE/符号工具，零产品依赖），以及几个薄壳入口。

## 规矩

- **依赖单向**：命令 → 服务 → 库。产品之间不 import；要共用就下沉到 `services/`。
- **`tools/` 里出现"被两个入口 import 的实现"就是放错了位置** —— 实现去 `libs/` 或 `services/`。
- **入口一律脚本式**：`python tools/x.py`。不要 `python -m`：C 壳的 `OPT_SCRIPT`、`opt.ini` / `config.ini` 里的命令串都写着脚本路径。
- **C 侧边界**：`native/forward/` 不许出现 `compile` / `simplexc` / `opt.ini` / `work/`（路径由壳注入）；`simplexc_core.h` 只许 `native/backend/simplexc.c` include。
- **跨语言只经文件与进程**：`work/`、`hook.log`、`opt.ini`。Python 不 import C，C 不 import Python。
- 改动后的判据：`python simplexc/tools/check.py`（7 步）；缺 CLI 先 `python simplexc/tools/build.py simplexc_cli tcblob`。

## 找东西

- 目录地图：`docs/map.md`；各层细则：对应目录的 `README.md`
- 工具清单与坑：`TOOLS.md`、`docs/pitfalls.md`
- 设计依据：`docs/specs/2026-11-05-toolkit-architecture.md`
- 当前在追什么：`../docs/status.md`
