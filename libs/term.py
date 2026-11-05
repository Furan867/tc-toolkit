#!python3
"""终端着色 —— **只在输出目标是终端时**生效。

判断顺序（先命中者为准）：

    NO_COLOR 环境变量      -> 永不着色（社区约定，见 https://no-color.org）
    FORCE_COLOR 环境变量   -> 总是着色（测试/演示用）
    stdout.isatty()        -> 终端里着色；重定向到文件、被管道接走时不着色

为什么用环境变量而不是 `--color` 参数：一键脚本要拉**子进程**（采样、建映射、
出报告各是一个进程），环境变量会自动继承下去，参数得逐个转发。另外
`profile_by_image` 在自身 stdout 不是终端时会**主动设 NO_COLOR**，保证父子
一致 —— 否则父进程无色、子进程有色，日志文件里会混进转义码。

**不该着色时所有函数原样返回文本**，调用方不必写分支。
"""
from __future__ import annotations

import os
import sys

_CODES = {
    "reset": "\033[0m", "bold": "\033[1m", "dim": "\033[2m", "ul": "\033[4m",
    "red": "\033[31m", "green": "\033[32m", "yellow": "\033[33m",
    "blue": "\033[34m", "magenta": "\033[35m", "cyan": "\033[36m",
    "gray": "\033[90m",
}


def enabled(stream=None):
    """当前输出是否应该着色。"""
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("FORCE_COLOR"):
        return True
    try:
        return (stream or sys.stdout).isatty()
    except Exception:
        return False


def paint(text, *styles, stream=None):
    """给 text 上色；不该着色时原样返回。

    ⚠️ 只用于**已经排好版**的字符串。ANSI 转义不占显示宽度，但宽度对齐函数
    （如 analyze_sim_hotspot.cell）会把它算进长度 —— 先着色再对齐，表格就歪了。
    正确顺序是：先 cell() 对齐，再 paint()/heat() 上色。
    """
    if not styles or not enabled(stream):
        return text
    return "".join(_CODES.get(s, "") for s in styles) + text + _CODES["reset"]


_FIELD = {
    "count": ("bold", "cyan"),     # 采样数、次数
    "pct":   ("bold", "green"),    # 百分比
    "n":     ("yellow",),          # 个数：组件数、副本数、线程数
    "bytes": ("magenta",),         # 字节数
    "name":  ("bold",),            # 标识：组件类型名
    "addr":  ("blue",),            # 地址
    "dim":   ("gray",),            # 次要信息：路径、耗时
}


def field(kind, text):
    """按**数据类别**上色 —— 同一类数据在哪儿都是同一个颜色，便于扫读。

    与「按数值大小渐变」不同：组件表本来就按消耗排好序了，再按大小变色不带来
    额外信息；按类别配色则让终端不再千篇一律，一眼分得清这是采样数、字节数
    还是个数。表头也用同一个 field()，列与表头颜色呼应。

    `text` 应当**已对齐**（见 paint 的说明）。kind 未定义时原样返回。
    """
    return paint(text, *_FIELD.get(kind, ()))


def duration(sec, text):
    """按耗时上色 —— 正常淡出，异常才醒目。

    这个是给编译打的：正常约 0.8s；若看到十几秒，多半是解释器选错了
    （docs/pitfalls.md 的「py 启动器按 shebang 选解释器」）。
    """
    if sec >= 10:
        return paint(text, "bold", "red")
    if sec >= 3:
        return paint(text, "yellow")
    return paint(text, "gray")
