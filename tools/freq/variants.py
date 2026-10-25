"""变体轴（compiler × rewrite）与 `opt.ini` 的读写 —— **合法值集合的唯一一处**。

控制通道仍是 `opt.ini`：转发层每次编译前读它（`compile_hook.c` 的
`GetPrivateProfileString` 取 `[opt]` 段里**第一条**匹配行，重复 key 不报错），
改写器也照同一条规则读（`optimize_image.py` 的 `rewrite_mode`）。所以这里也必须
取第一条、并且**在原位改值、绝不新增行** —— 新旧两行并存时，两个读者各读一行，
2026-10-05 那次 `short_cse` 被静默当成 `on` 就是这么来的。

值合法性与兜底（设计 §4）：

    键缺失      compiler → ref（fail-safe：绝不能把「没配」当成「我们的实现」）
                rewrite  → on（与转发层/改写器现行的兜底一致）
    值非法      报错。读到的值会原样进报告当「配了什么」，静默归一等于把 A/B 的
                标注指向另一个变体，那比报错难查得多。
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

COMPILERS = ("ref", "ours", "ours_with_fallback")
REWRITES = ("off", "on", "short", "short_cse")

FALLBACK_COMPILER = "ref"
FALLBACK_REWRITE = "on"

OPT_SECTION = "opt"
OPT_INI = Path(__file__).resolve().parents[2] / "opt.ini"

_SECTION_RE = re.compile(r"^\s*\[(.+?)\]\s*$")
_KEY_RE = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\S+)\s*$")


@dataclass(frozen=True)
class Variant:
    """A/B 对照的一格：用哪个实现（`compiler`）× 哪种改写（`rewrite`）。

    构造即校验 —— 非法值在**进入测量流程之前**就该炸掉，而不是等产物的归属被算错。
    """

    compiler: str = FALLBACK_COMPILER
    rewrite: str = FALLBACK_REWRITE

    def __post_init__(self) -> None:
        if self.compiler not in COMPILERS:
            raise ValueError(f"compiler 不在 {COMPILERS}：{self.compiler!r}")
        if self.rewrite not in REWRITES:
            raise ValueError(f"rewrite 不在 {REWRITES}：{self.rewrite!r}")

    @classmethod
    def parse(cls, text: str) -> Variant:
        """`compiler:rewrite`（如 `ours:off`）—— 判据入口 `--variant` 的形态。"""
        parts = text.split(":")
        if len(parts) != 2:
            raise ValueError(f"变体要写成 compiler:rewrite（得到 {text!r}）")
        return cls(parts[0].strip().lower(), parts[1].strip().lower())

    def __str__(self) -> str:
        return f"{self.compiler}:{self.rewrite}"


def _effective_opt_lines(lines: list[str]) -> list[tuple[int, str, str, str]]:
    """`[opt]` 段里生效的行：(行号, 缩进, 小写键, 值)。

    生效 = 不在别的段里、既不是空行也不是 `;`/`#` 注释、且整行就是 `key = value`。
    整行匹配是刻意的：带尾巴（`rewrite = off  ; 备注`）的行按「没有这个键」处理，
    与转发层取不到值时的行为一致，而不是猜一个值出来。
    """
    found: list[tuple[int, str, str, str]] = []
    in_opt = False
    for i, line in enumerate(lines):
        s = line.strip()
        sec = _SECTION_RE.match(s)
        if sec:
            in_opt = sec.group(1).strip().lower() == OPT_SECTION
            continue
        if not in_opt or not s or s[0] in ";#":
            continue
        m = _KEY_RE.match(line)
        if m:
            found.append((i, m.group(1), m.group(2).lower(), m.group(3)))
    return found


def ini_values(path: Path | None = None) -> dict[str, str]:
    """`[opt]` 段里**第一条**生效行给出的键值（键小写、值小写）。读不到文件返回空。"""
    try:
        text = (path or OPT_INI).read_text(encoding="utf-8")
    except OSError:
        return {}
    values: dict[str, str] = {}
    for _i, _indent, key, value in _effective_opt_lines(text.splitlines()):
        values.setdefault(key, value.lower())
    return values


def read_variant(path: Path | None = None) -> Variant:
    """读当前变体；缺键按 fail-safe 兜底（见模块说明），值非法由 `Variant` 报错。"""
    values = ini_values(path)
    return Variant(values.get("compiler", FALLBACK_COMPILER),
                   values.get("rewrite", FALLBACK_REWRITE))


def set_variant(variant: Variant, path: Path | None = None) -> None:
    """把 `compiler` 与 `rewrite` 就地改成 `variant` 的值（不新增行、不删行）。

    先写临时文件再原子替换：转发层每次编译都读这个文件，读到写了一半的 `opt.ini`
    会拿到半截值。两个键都定位得到才落盘 —— 找不到键的生效行直接报错，追加一行
    会让「取第一条」的读者继续读旧值（静默失效）。
    """
    target = path or OPT_INI
    try:
        lines = target.read_text(encoding="utf-8").splitlines(keepends=True)
    except OSError as exc:
        raise ValueError(f"读不到 opt.ini：{target}（{exc}）") from exc

    for key, value in (("compiler", variant.compiler), ("rewrite", variant.rewrite)):
        hit = next((h for h in _effective_opt_lines(lines) if h[2] == key), None)
        if hit is None:
            raise ValueError(
                f"opt.ini 里没有生效的 {key} 行：{target} —— 先把模板里 "
                f"`; {key} = {value}` 那行的注释去掉（模板见 opt.ini.example）")
        lines[hit[0]] = f"{hit[1]}{key} = {value}\n"

    tmp = target.with_name(target.name + ".new")
    tmp.write_text("".join(lines), encoding="utf-8")
    os.replace(tmp, target)
