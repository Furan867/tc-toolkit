"""`python -m tools.perf_opt` 的入口（`-m` 跑的是包的 `__main__`，不是 `cli`）。

`rewrite` 不挂进 `cli` 的 argparse：`cli` 是部署命令的稳定接口，薄壳
`tools/deploy_compile_hook.py` 直接转发它，加子命令会改它的 `--help`。
"""
from __future__ import annotations

import sys

from tools.perf_opt.cli import main as deploy_main

if __name__ == "__main__":
    if sys.argv[1:2] == ["rewrite"]:
        # 惰性导入：部署路径不该被改写器的导入成本（以及它的依赖）连累
        from tools.perf_opt.rewrite import main as rewrite_main
        sys.exit(rewrite_main(sys.argv[2:]))
    sys.exit(deploy_main())
