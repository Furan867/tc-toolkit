#!python3
"""`work/hook/hook.log` 的唯一读写实现（Python 侧）。

路径为什么不是常量：`hook.log` 于 2026-10-18 从 `work/opt/` 迁到 `work/hook/`（freq
设计 §7 决策 4），而 C 侧的新壳是**另一趟部署** —— 过渡期里旧壳仍写 `work/opt/hook.log`。
所以生效路径由 `resolve()` 决定（规则见它自己的 docstring），`hook_log.PATH` 取到的就是
它的结果。读写都走这一个入口：`impl`（转发层）与 `opt`（改写器）两行落在两个文件里，
归属就会静默错位。

要判「日志还没出现过」，用 `os.path.exists(hook_log.resolve())` —— 空的日志与「没编译」
不是一回事，别把前者当后者报成功。

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
| `impl`      | 转发层 | **这一次实际走了哪个实现**（与 `mode=` 里配的 `compiler` 可能不同：回退就是不同） | `impl` `mode` `result`；只在这趟起过改写器时额外带 `rewrite`（配的改写模式）；ours 行可选 `budget_ms`；首次加 `guard=veh` |
| `writeback` | 转发层 | 产物已就位写回原缓冲     | `len` |
| `entry`     | 转发层 | 缩短后锚点已同步更新     | `offset` |
| `error`     | 转发层 | 任一环节放弃             | `stage` + `kind`/`code`/`timeout`/`detail` |
| `opt`       | 改写器 | 这一趟改写的实际结果     | `mode` `rewritten` `cse` `reloc` `len` `sha256` `note` |

**两个写入者**：转发层（C，`compile_hook.c` 的 `hook_log()`）与本模块的
`append()`。格式是两边的契约，改一处必须改另一处。

`mode` 是**实际执行**的模式（`off` / `on` / `short` / `short_cse`）：
解析失败或缩短被防御挡回时它与 `impl` 行的 `rewrite` 不同，那正是要查的信号。
⚠️ `impl` 行的 `mode` 是**另一个东西** —— 它是 `opt.ini` 里配的 `compiler`（`ref` / `ours` /
`ours_with_fallback`），取值以 `docs/specs/2026-10-15-simplexc-interfaces.md` §5 的 `impl` 词表为准。
解析时按事件名区分：`opt` 行的 `mode` = 改写器实际做了什么，`impl` 行的 `mode` = 配了什么。
`impl` 词表（唯一一处定义在接口文档 §5）：`ref` / `ours` / `fallback` / `ours_failed`，
**每次编译都写一行**（含 ref —— 回退率需要分母）；解析：`fallback` = 配了
`ours_with_fallback` 但 ours 失败、整单转交了参考实现。
唯一没有 `impl` 行的情形：参考实现直接把进程 abort 掉（行写在它返回之后，
因为 `rewrite=` 得先说清有没有产物；那一趟同样没有 dump 与 manifest）。
"""
from __future__ import annotations

import os
import time
from collections.abc import Iterator
from typing import NamedTuple

from libs import paths

_NEW, _LEGACY = paths.hook_log_candidates()
NEW_PATH = os.fspath(_NEW)        # 迁移目标（C 侧新壳写这里）
LEGACY_PATH = os.fspath(_LEGACY)  # 迁移前的位置（旧壳还写这里）


def resolve() -> str:
    """当前生效的日志路径 —— 读、写、过滤全部走它。

    1. 只有一边存在 → 用它；
    2. 两边都存在 → 取 **mtime 更新**的那个（过渡期就这么闭合成一个文件：旧壳在写旧文件，
       于是旧文件 mtime 新，Python 跟着旧壳读旧、写旧；新壳部署后开始写新文件，自动切过去）；
    3. 都不存在 → 新路径（写入时按它创建，读就是空日志）；
    4. mtime 相同 → 新路径（确定性，不抖）。

    `append()` 也必须跟随解析结果：另建一个新文件就会让 `impl` 与 `opt` 分家。
    """
    if os.path.exists(NEW_PATH) and os.path.exists(LEGACY_PATH):
        try:
            if os.path.getmtime(LEGACY_PATH) > os.path.getmtime(NEW_PATH):
                return LEGACY_PATH
        except OSError:
            pass        # 取不到 mtime（刚被另一个写入者清掉）→ 按规则 4 落到新路径
        return NEW_PATH
    return LEGACY_PATH if os.path.exists(LEGACY_PATH) else NEW_PATH


def __getattr__(name: str) -> str:
    """`PATH` 是解析结果（不是常量）—— 长驻进程（watch / bench）因此也会跟着切换。"""
    if name == "PATH":
        return resolve()
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


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
    target = resolve()          # 写到**刚读到的那一个**（见 resolve 的 docstring）
    try:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        fd = os.open(target, os.O_APPEND | os.O_CREAT | os.O_WRONLY)
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
        with open(path or resolve(), encoding="utf-8", errors="replace") as f:
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
