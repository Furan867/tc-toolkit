#!python3
"""`work/opt/hook.log` 的唯一读写实现（Python 侧）。

格式（制表符分隔，一行一条记录）：

    时间 \\t 编号 \\t 事件 \\t key=value ...

- 第 1 列 本地时间 `YYYY-MM-DD HH:MM:SS`（含空格，所以解析先按 \\t 切）
- 第 2 列 编译编号，三位零填充，与 `hook_src_NNN.si` / `NNN.raw.bin` 同号
- 第 3 列 事件名（见下表）
- 之后任意个 `key=value`；值内允许空格与 `=`（解析用 `split("=", 1)`），
  不允许 \\t / \\r / \\n —— `append()` 会把它们洗成空格

扩容方式 = **只加不改**：新增事件名或新增 key 即可，消费者按 事件+键 取值，
未知事件与未知键一律忽略（本模块不维护白名单，也不校验键名）。

| 事件 | 写入者 | 含义 | 键 |
|---|---|---|---|
| `cfg`       | 转发层 | 起改写子进程前读到的配置 | `rewrite` |
| `impl`      | 转发层 | **这一次实际走了哪个实现**（与 `cfg` 的 `compiler` 可能不同：回退就是不同） | `impl` `result` |
| `writeback` | 转发层 | 产物已就位写回原缓冲     | `len` |
| `entry`     | 转发层 | 缩短后锚点已同步更新     | `offset` |
| `error`     | 转发层 | 任一环节放弃             | `stage` + `code`/`timeout`/`detail` |
| `opt`       | 改写器 | 这一趟改写的实际结果     | `mode` `rewritten` `cse` `reloc` `len` `sha256` `note` |

**两个写入者**：转发层（C，`compile_hook.c` 的 `hook_log()`）与本模块的
`append()`。格式是两边的契约，改一处必须改另一处。

`mode` 是**实际执行**的模式（`off` / `on` / `short` / `short_cse`）：
解析失败或缩短被防御挡回时它与 `cfg` 的 `rewrite` 不同，那正是要查的信号。
"""
from __future__ import annotations

import os
import time
from collections.abc import Iterator
from typing import NamedTuple

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.normpath(os.path.join(HERE, os.pardir, "work", "opt", "hook.log"))


class Line(NamedTuple):
    time: str
    seq: str
    event: str
    kv: dict


def _clean(value) -> str:
    return str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def _seq3(seq: int | str) -> str:
    """三位零填充的编号 —— 与捕获文件名 `hook_src_NNN.si` / `NNN.raw.bin` 对齐。"""
    try:
        return f"{int(seq):03d}"
    except (TypeError, ValueError):
        return _clean(seq)


def append(seq: int | str, event: str, **kv) -> None:
    """追加一条记录。写不进去就静默放弃 —— 日志绝不该拖垮调用方。

    单次 `O_APPEND` 写整行，多进程（转发层与改写子进程）并发追加时不会互相截断。
    """
    cols = [time.strftime("%Y-%m-%d %H:%M:%S"), _seq3(seq), event]
    cols += [f"{k}={_clean(v)}" for k, v in kv.items() if v is not None]
    line = ("\t".join(cols) + "\n").encode("utf-8")
    try:
        os.makedirs(os.path.dirname(PATH), exist_ok=True)
        fd = os.open(PATH, os.O_APPEND | os.O_CREAT | os.O_WRONLY)
    except OSError:
        return
    try:
        os.write(fd, line)      # 一次写完，避免与另一个写入者的行互相穿插
    except OSError:
        pass                    # 有意静默：记录失败不影响编译
    finally:
        os.close(fd)


def read(path: str | None = None) -> Iterator[Line]:
    """逐行解析。列数不足的行跳过 —— 并发追加时可能读到只写了一半的行。"""
    try:
        with open(path or PATH, encoding="utf-8", errors="replace") as f:
            for raw in f:
                parts = raw.rstrip("\r\n").split("\t")
                if len(parts) < 3:
                    continue
                kv = {}
                for item in parts[3:]:
                    if "=" in item:
                        k, v = item.split("=", 1)
                        kv[k] = v
                yield Line(parts[0], parts[1], parts[2], kv)
    except OSError:
        return                  # 文件还没出现过：等同于空日志


def find(event: str, seq: int | str | None = None,
         path: str | None = None) -> Iterator[Line]:
    """按事件名（可选再按编号）过滤，文件顺序产出。"""
    want = None if seq is None else _seq3(seq)
    for ln in read(path):
        if ln.event == event and (want is None or ln.seq == want):
            yield ln
