"""命令行入口：解析子命令并分发到 build / deploy。"""
from __future__ import annotations

import argparse
import contextlib
import os
import sys

# 直接跑本文件时 sys.path[0] 是 tools/perf_opt/；仓库根得自己补
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

# Windows 下 stdout 走管道时默认 GBK，输出里的 ✅/❌ 会抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    _rec = getattr(_stream, "reconfigure", None)
    if _rec is not None:
        with contextlib.suppress(Exception):
            _rec(encoding="utf-8", errors="replace")

from tools.perf_opt.build import build  # noqa: E402
from tools.perf_opt.deploy import deploy, restore, status  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description="部署/恢复 compile.dll 转发层")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="查看当前状态")
    sub.add_parser("build", help="编译 native/compile/compile_hook.c → simplexc/build/compile_hook.dll")
    sub.add_parser("deploy", help="部署转发层（游戏在跑则拒绝）")
    sub.add_parser("build-deploy", help="编译后立刻部署（改完 compile_hook.c 的常规流程）")
    sub.add_parser("restore", help="还原为原始 compile.dll")
    cmd = ap.parse_args().cmd
    if cmd == "status":
        return status()
    if cmd == "build":
        return build()
    if cmd == "build-deploy":
        rc = build()
        return rc if rc else deploy()
    return deploy() if cmd == "deploy" else restore()


if __name__ == "__main__":
    sys.exit(main())
