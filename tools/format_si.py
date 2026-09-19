#!python3
"""格式化 Simplex 源码（.si）—— 只做三件不破坏语义的事。

1. 连续 >=2 个空行  ->  压成 1 个
2. 去掉行尾空白
3. 去掉文件末尾多余空行

**不做**：调整缩进、重排语句、改动任何非空行的内容。那些是语法级操作，需要
真正的解析器；VSCode 的 "TC Simplex" 插件（michai.tc-si）只提供 TextMate
正则高亮、不含 formatter 贡献点，借不到它的能力。

为什么这三条安全
----------------
空行在 Simplex 里不承载语义。唯一可能含多行空行的是 `asm x86_64 { }` 汇编块，
但汇编块里的空行同样不影响编译（空行不是指令）。双引号字符串按插件的语法定义
（string.quoted.double.si）是**单行**的，不含换行。

用法
----
    python tools/format_si.py --file work/hook_src_055.si      # 输出 .formatted.si 供查阅
    python tools/format_si.py --file x.si --inplace            # 原地改写（自动备份 .bak）
    python tools/format_si.py --dir work/hook                  # 批量处理目录下的 *.si
"""
from __future__ import annotations

import argparse
import glob
import os
import shutil
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

NL = chr(10)


def format_text(text):
    """返回 (格式化后的文本, 统计)。

    三条规则严格按顺序执行，且**只动空行与行尾**，任何非空行的内容一个字符都不改。
    """
    lines = text.replace("\r\n", NL).replace("\r", NL).split(NL)
    n_before = len(lines)
    trail = sum(1 for l in lines if l != l.rstrip())

    lines = [l.rstrip() for l in lines]          # 规则 2

    out, collapsed, prev_blank = [], 0, False
    for l in lines:                              # 规则 1
        blank = (l == "")
        if blank and prev_blank:
            collapsed += 1
            continue
        out.append(l)
        prev_blank = blank

    tail = 0
    while out and out[-1] == "":                 # 规则 3
        out.pop()
        tail += 1

    stats = {
        "原行数": n_before,
        "行尾有空白": trail,
        "压缩掉的空行": collapsed,
        "去掉的尾部空行": tail,
    }
    return NL.join(out) + NL, stats


def process(path, inplace=False):
    text = open(path, encoding="utf-8", errors="replace").read()
    new, stats = format_text(text)
    if inplace:
        shutil.copy2(path, path + ".bak")
        open(path, "w", encoding="utf-8", newline=NL).write(new)
        dst = path + "  （原文件已备份为 .bak）"
    else:
        base, ext = os.path.splitext(path)
        dst = base + ".formatted" + (ext or ".si")
        open(dst, "w", encoding="utf-8", newline=NL).write(new)
    print("%s" % os.path.basename(path))
    print("   %s" % "  ".join("%s %s" % (k, v) for k, v in stats.items()))
    print("   -> %s" % dst)
    return stats


def main():
    ap = argparse.ArgumentParser(description="格式化 Simplex 源码（压空行/去行尾空白）")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--file", help="单个 .si 文件")
    g.add_argument("--dir", help="批量处理该目录下的 *.si")
    ap.add_argument("--inplace", action="store_true",
                    help="原地改写（会先备份 .bak）；默认输出到 .formatted.si")
    args = ap.parse_args()

    if args.file:
        if not os.path.exists(args.file):
            print("找不到 %s" % args.file, file=sys.stderr)
            return 1
        process(args.file, args.inplace)
        return 0

    files = sorted(glob.glob(os.path.join(args.dir, "*.si")))
    files = [f for f in files if not f.endswith(".formatted.si") and not f.endswith(".bak")]
    if not files:
        print("目录下没有 .si 文件：%s" % args.dir, file=sys.stderr)
        return 1
    tot = {"原行数": 0, "行尾有空白": 0, "压缩掉的空行": 0, "去掉的尾部空行": 0}
    for f in files:
        st = process(f, args.inplace)
        for k in tot:
            tot[k] += st[k]
    print()
    print("合计 %d 个文件：%s" % (len(files),
          "  ".join("%s %s" % (k, v) for k, v in tot.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
