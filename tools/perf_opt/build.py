"""编译转发层到 `simplexc/build/compile_hook.dll`。"""
from __future__ import annotations

import os
import subprocess
import sys

from tools.perf_opt.config import HOOK, ROOT


def build():
    """编译转发层到 simplexc/build/compile_hook.dll —— 转调 build.py 的 `simplexc_dll` 目标。

    为什么不再自己拼命令：转发层是「C 壳 + C++ 核心 + LLVM-C」，构建定义（flags、源文件表、
    objs 依赖）只有 CMakeLists.txt 一处；本脚本只保留部署相关的动作。
    """
    script = os.path.join(ROOT, "simplexc", "tools", "build.py")
    if not os.path.exists(script):
        print(f"❌ 找不到构建脚本：{script}", file=sys.stderr)
        return 1
    cmd = [sys.executable, script, "simplexc_dll"]
    print("$ " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        print(f"❌ 编译失败（返回 {r.returncode}）", file=sys.stderr)
        return r.returncode
    if not os.path.exists(HOOK):
        print(f"❌ 构建脚本报成功，但 {HOOK} 不存在", file=sys.stderr)
        return 1
    print(f"✅ 已编译 {HOOK}（{os.path.getsize(HOOK)} B）")
    print("⚠️ 部署时必须把 simplexc/build/LLVM-C.dll 一起拷进游戏目录（否则游戏解析依赖失败）")
    return 0
