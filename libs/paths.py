"""`work/` 目录的唯一解析点：谁要 `work/` 下的路径都从这里拿。

`work/` 下的布局是 Python 服务、freq 工具、hotspot 与 C 侧转发层共用的约定；
散着写 `Path(...) / "work" / ...` 时，迁一次目录就要全仓找一遍（`hook.log` 从
`work/opt/` 迁到 `work/hook/` 就吃过这个亏）。所以路径只在这里算，别处一律引用。

本模块**只依赖 `pathlib`** —— 它被 `libs/` 内外几乎所有模块 import，引别的
`libs`/`tools` 模块会成环。

只给路径，不给语义：两个候选之间的选择规则（`hook_log` 的 mtime 选边）留在
`libs/hook_log.py`，那是解析语义，不是路径。
"""
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WORK = REPO_ROOT / "work"


def hook_dir() -> Path:
    """转发层捕获（`hook_src_NNN.si` / `hook_out_NNN.bin` / `manifest.tsv`）与 `hook.log`。"""
    return WORK / "hook"


def opt_dir() -> Path:
    """改写器工作区（`NNN.raw.bin` / `NNN.opt.bin`）—— 改写前与改写后的中间产物。"""
    return WORK / "opt"


def freq_dir() -> Path:
    """主频采样数据 —— 跨会话保留，别当临时目录清。"""
    return WORK / "freq"


def tmp_dir() -> Path:
    """可随时删的临时文件（run 日志、配置备份、一次性探查脚本）。"""
    return WORK / "tmp"


def hook_log_candidates() -> tuple[Path, Path]:
    """`hook.log` 的候选路径，**顺序即优先级**：先是新位置，后是迁移前的位置。

    两个都在时按 mtime 选边 —— 规则见 `libs/hook_log.resolve()`。
    """
    return hook_dir() / "hook.log", opt_dir() / "hook.log"
