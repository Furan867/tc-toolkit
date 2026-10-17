#!python3
"""从捕获的 `.si` 源码里抽出「按游戏进程实例变化」的绝对地址常量，写成 C 头。

为什么是脚本而不是手抄
----------------------
生成源码里那些 `Ptr 2270676779008` / `raw_cast(<Arr[U64]>, …)` 是**本次游戏会话**的游戏内存地址，
换一个进程实例就整套换掉（实测同一进程内关卡与元件工坊同段，下一次会话整段平移，见
`docs/pitfalls.md` 最后一条）。手抄一次就过期，所以等价物一律 `#include` 这里生成的头，
再由 `equiv_build.py` 在同一次会话里把它编进产物。

只认这 11 个名字（`EXPECTED`）：多一个、少一个、同一个名字出现两次都**报错退出** ——
静默生成一个缺项的头，编出来的产物会往未映射地址写，那种错比缺文件难查得多。

用法
----
    python simplexc/tools/gen_addrs.py --src work/hook/hook_src_040.si -o work/tmp/equiv/addrs.h

判据：头里的 11 个值与源码逐字一致；换一份源码进去得到的就是那一份的地址。
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys

# 源码里的名字 → 头里的宏名。顺序 = 头里的顺序。
# 前 8 个沿用 `probe/006_equiv.c` 的宏名（那边是手抄的同名常量），后 3 个是本脚本新增的。
EXPECTED = (
    ("commands", "A_COMMANDS"),
    ("settings", "A_SETTINGS"),
    ("input_replay", "A_INPUT_REPLAY"),
    ("output_history_pins", "A_OUTPUT_HISTORY_PINS"),
    ("error_buffer", "A_ERROR_BUFFER"),
    ("ui_buffer", "A_UI_BUFFER"),
    ("ctl_input_replay_reset", "A_CTL_RESET"),
    ("#SIMULATION_STATE", "A_SIM_STATE"),
    ("#SIMULATION_KEYBOARD_CHARACTER", "A_KB_CHAR"),
    ("#SIMULATION_KEYBOARD_COORDINATE", "A_KB_COORD"),
    ("#SIMULATION_PUSH_BUTTON", "A_KB_PUSH_BUTTON"),
)

# 两种声明形态：`var X = Ptr N` / `const #X = Ptr N` / `var X = raw_cast(<Arr[U64]>, N)`
DECL_RE = re.compile(
    r"^[ \t]*(?:var[ \t]+(?P<var>\w+)|const[ \t]+#(?P<const>\w+))[ \t]*=[ \t]*"
    r"(?:Ptr|raw_cast\([ \t]*<[^>]+>[ \t]*,)[ \t]*(?P<addr>\d+)[ \t]*\)?[ \t]*$",
    re.M,
)

MIN_ADDR = 0x10000                     # 比这小的「地址」只可能是写错的数字


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_addrs(path):
    """→ {源码名: 地址}（顺序不保证；调用方按 EXPECTED 排）。解析出 0 个名字也报错。"""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        raise SystemExit(f"[gen_addrs] 读不了源码 {path}：{e}") from e
    found, dups = {}, []
    for m in DECL_RE.finditer(text):
        name = m.group("var") or ("#" + m.group("const"))
        addr = int(m.group("addr"))
        if name in found:
            dups.append((name, found[name], addr))
            continue
        found[name] = addr
    if dups:
        msg = "\n".join(f"  {n}: {a} 与 {b}" for n, a, b in dups)
        raise SystemExit(f"[gen_addrs] {path} 里同一个名字被声明了多次（不知道该信哪个）：\n{msg}")
    if not found:
        raise SystemExit(f"[gen_addrs] {path} 里一个地址常量都没找到 —— 源码形态变了？")
    return found


def check_names(found, path):
    """名字集合必须与 `EXPECTED` 完全一致 —— 多、少都报错。"""
    want = {name for name, _macro in EXPECTED}
    missing = [n for n, _m in EXPECTED if n not in found]
    extra = sorted(set(found) - want)
    problems = []
    if missing:
        problems.append(f"缺 {len(missing)} 个：{missing}")
    if extra:
        problems.append(f"多 {len(extra)} 个：{extra}（若确属新常量，把它加进 EXPECTED）")
    if problems:
        raise SystemExit(f"[gen_addrs] {path} 的地址常量与预期不符（预期 {len(EXPECTED)} 个）：\n  "
                         + "\n  ".join(problems))
    small = [(n, a) for n, a in found.items() if a < MIN_ADDR or a >= (1 << 64)]
    if small:
        raise SystemExit(f"[gen_addrs] {path} 里有不像地址的值：{small}")


def render_header(entries, src_path, src_sha):
    src = os.path.basename(src_path)
    lines = [
        f"/* 自动生成，别手改 —— 源头：{src}（sha256 {src_sha[:16]}…）。",
        "   重新生成：python simplexc/tools/gen_addrs.py --src <那份 .si> -o <这个文件>",
        "",
        "   为什么不能提交/复用：这些地址按**游戏进程实例**变，换一次会话就整套换掉",
        "   （docs/pitfalls.md 最后一条）→ 产物必须在抓到源码的同一次会话里编出来。 */",
        "#ifndef TC_GAME_ADDRS_H",
        "#define TC_GAME_ADDRS_H",
        "",
    ]
    for name, macro in EXPECTED:
        lines.append(f"#define {macro:<23} 0x{entries[name]:X}ULL   /* {name} */")
    lines += ["", "#endif /* TC_GAME_ADDRS_H */", ""]
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="从捕获的 .si 提取本会话的 11 个绝对地址 → C 头")
    ap.add_argument("--src", required=True, help="捕获的源码（例如 work/hook/hook_src_040.si）")
    ap.add_argument("-o", "--out", required=True, help="写出的 C 头路径（等价物 #include 它）")
    a = ap.parse_args(argv)

    found = parse_addrs(a.src)
    check_names(found, a.src)
    entries = {name: found[name] for name, _macro in EXPECTED}
    text = render_header(entries, a.src, sha256_of(a.src))

    out_dir = os.path.dirname(os.path.abspath(a.out))
    try:
        os.makedirs(out_dir, exist_ok=True)
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    except OSError as e:
        raise SystemExit(f"[gen_addrs] 写不了 {a.out}：{e}") from e

    print(f"[gen_addrs] {len(entries)} 个地址 ← {a.src} → {a.out}")
    for name, macro in EXPECTED:
        print(f"  {macro:<23} 0x{entries[name]:<14X} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
