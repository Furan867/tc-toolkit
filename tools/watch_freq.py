#!python3
"""常驻采样器：被动记录游戏里实测的仿真主频，并标出每次编译点 —— 薄壳。

实现全在 `tools/freq/`：采样循环与分段在 `sampler.py`，总结表与
`freq_summary.txt` 在 `report.py`，产物身份/归属在 `products.py`。
保留这个文件名是因为人和文档都在用它，**参数与输出与旧版逐字符一致**：

    py tools/watch_freq.py                  # Ctrl-C 停
    py tools/watch_freq.py --seconds 60     # 记 60 秒后自动收尾
    py tools/watch_freq.py -v               # 总结里附上逐段明细

读哪里：**只看退出时那份总结**（终端上就是，另外还落一份无色的
`work/freq/freq_summary.txt`）。逐事件的 `work/freq/freq_log.tsv` 只给脚本用。
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tools.freq.cli_watch import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
