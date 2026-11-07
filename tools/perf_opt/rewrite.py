"""产物改写的 CLI 入口 —— 本体在 `tools/perf_opt/optimizer/core.py`。

调用者是 C 壳（`OPT_SCRIPT`）：直接 `python tools\\perf_opt\\rewrite.py --in ... --out ...`，
没有包上下文，相对导入不可用。于是这里固定把仓库根放进 `sys.path`，本体一律用**绝对名**
导入 —— 只有脚本式这一条路，所以 `core` 不会被导入成两份。
"""
from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.perf_opt.optimizer import core  # noqa: E402

PROG = "python tools/perf_opt/rewrite.py"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog=PROG,
                                 description="产物改写器（等长 / 缩短两模式）")
    ap.add_argument("--in", dest="src", required=True, help="转发层落盘的原始产物")
    ap.add_argument("--out", dest="dst", required=True, help="要写回游戏的产物")
    ap.add_argument("--entry", help="产物锚点偏移（转发层从 Result[2] 取来，十六进制）——"
                    "缩短模式必须同步更新它，否则游戏拿到错位置"
                    "（见 tools/perf_opt/optimizer/core.py 的模块 docstring）")
    ap.add_argument("--mode", help="改写模式，由转发层从 opt.ini 读来后传进来；"
                    "不传则自己读 opt.ini。优先级：--mode > OPT_REWRITE > opt.ini")
    args = ap.parse_args(argv)
    entry = int(args.entry, 16) if args.entry else None
    return core.run(args.src, args.dst, entry, args.mode)


if __name__ == "__main__":
    sys.exit(main())
