#!python3
"""自动测当前电路在各变体（compiler × rewrite）下的主频 —— 薄壳。

实现全在 `tools/freq/`：变体与 `opt.ini` 在 `variants.py`，驱动游戏与前置断言在
`driver.py`，采样在 `sampler.py`，对照表 / TSV / JSON 在 `report.py`。
保留这个文件名是因为人和文档都在用它。**新增**（§4）`--variant compiler:rewrite`
（可重复）与 `--json FILE`；旧参数 `-m/--modes -s/--seconds -d/--drop -r/--repeat
-t/--compile-timeout -n/--dry-run` 全在。

    py tools/bench_rewrite.py                        # off,on,short,short_cse 各 8s
    py tools/bench_rewrite.py -m on,short
    py tools/bench_rewrite.py -n                     # 只打印计划
    py tools/bench_rewrite.py --variant reference:off --variant ours:off --json out.json

结果同时写到 `work/freq/bench_<时间戳>.tsv`；`--json` 另出一份 `schema=1` 报告。
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tools.freq.cli_bench import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
