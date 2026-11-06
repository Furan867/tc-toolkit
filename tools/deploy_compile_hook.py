#!python3
"""部署 / 恢复 compile.dll 转发层。

背景
----
游戏 exe 用 Nim 的 dynlib **按名字**取编译器（LoadLibrary("compile.dll") +
GetProcAddress("compile")，符号 loadLib__pureZdynlib_u3 / symAddr__pureZdynlib_u74）。
既然是按名字取，只要让 "compile.dll" 这个名字指向我们的转发层，就能拿到
compile() 的输入（电路源码）与产物 —— 不需要调试器、不改进程内存、不影响性能。

部署做的事
----------
    compile.dll        → 改名成 compile_real.dll   （原文件完整保留，随时可还原）
    simplexc/build/compile_hook.dll → 复制为 compile.dll

恢复做的事
----------
    删掉我们的 compile.dll，把 compile_real.dll 改名回 compile.dll

编译做的事（build）
------------------
    转调 `python simplexc/tools/build.py simplexc_dll` —— 转发层现在是「C 壳 + C++ 编译器核心」，
    构建命令只有 `CMakeLists.txt` 一处定义，本脚本只负责「部署前先把 DLL 重编一份」。

    ⚠️ 因为链了 `LLVM-C.dll`，**部署必须把它（simplexc/build/LLVM-C.dll）一起拷进游戏目录**
    （否则游戏加载时解析依赖失败，接口 §7）。本脚本不替你做这一步。

命令
----
    status        看当前状态（是否已部署、游戏是否在跑）
    build         只编译转发层
    deploy        只部署（把 simplexc/build/ 的 dll 盖上去；游戏在跑会拒绝）
    build-deploy  编译后立刻部署 —— 改完 compile_hook.c 的常规流程
    restore       还原为原始 compile.dll

典型流程：`build-deploy` → 启动游戏。

⚠️ 游戏运行时 DLL 文件被锁、无法改名 —— 脚本检测到游戏在跑会直接拒绝执行。
"""

from __future__ import annotations

import os
import sys

# 直接跑本文件时 sys.path[0] 是 `tools/`，`tools.perf_opt` 与 `libs` 都得靠仓库根才找得到
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
from tools.perf_opt.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
