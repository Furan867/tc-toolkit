"""产物改写的薄入口 —— 实现在 `tools/optimize_image.py`（C 壳的 `OPT_SCRIPT` 仍指向它）。"""
from __future__ import annotations

import argparse
import os
import sys

# `tools/optimize_image.py` 里有 `import analyze_jit_code`（同级模块的裸导入），
# 只有把 `tools/` 放进 sys.path，`-m tools.perf_opt` 这种方式才导得进它。
TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
from tools.optimize_image import run  # noqa: E402

PROG = "python -m tools.perf_opt rewrite"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog=PROG,
                                 description="产物改写器（等长 / 缩短两模式）")
    ap.add_argument("--in", dest="src", required=True, help="转发层落盘的原始产物")
    ap.add_argument("--out", dest="dst", required=True, help="要写回游戏的产物")
    ap.add_argument("--entry", help="产物锚点偏移（转发层从 Result[2] 取来，十六进制）——"
                    "缩短模式必须同步更新它，否则游戏拿到错位置"
                    "（见 tools/optimize_image.py 的模块 docstring）")
    ap.add_argument("--mode", help="改写模式，由转发层从 opt.ini 读来后传进来；"
                    "不传则自己读 opt.ini。优先级：--mode > OPT_REWRITE > opt.ini")
    args = ap.parse_args(argv)
    entry = int(args.entry, 16) if args.entry else None
    return run(args.src, args.dst, entry, args.mode)
