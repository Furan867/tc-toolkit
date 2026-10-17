#!python3
"""读游戏当前实测的仿真主频（UI 显示的那个 MHz）—— 薄壳。

实现全在 `tools/freq/cli_read.py`（偏移与读法在 `tools/freq/probe.py`）。
保留这个文件名是因为人和文档都在用它，参数与输出与旧版一致：

    py tools/read_freq.py                # 采一次给读数，再采一次给自算对照
    py tools/read_freq.py --watch        # 持续打印
    py tools/read_freq.py --samples 5
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tools.freq.cli_read import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
