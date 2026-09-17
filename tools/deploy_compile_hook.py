#!/usr/bin/env python3
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
    work/compile_hook.dll → 复制为 compile.dll

恢复做的事
----------
    删掉我们的 compile.dll，把 compile_real.dll 改名回 compile.dll

⚠️ 游戏运行时 DLL 文件被锁、无法改名 —— 脚本检测到游戏在跑会直接拒绝执行。
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys

# Windows 下 stdout 走管道时默认 GBK，输出里的 ✅/❌ 会抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = r"F:\opt\SteamLibrary\steamapps\common\Turing Complete"
TARGET = os.path.join(GAME, "compile.dll")
REAL = os.path.join(GAME, "compile_real.dll")
HOOK = os.path.join(ROOT, "work", "compile_hook.dll")


def find_pid():
    sys.path.insert(0, HERE)
    try:
        from catch_compile_source import find_pid as fp
        return fp()
    except Exception:
        return None


def status():
    pid = find_pid()
    print("游戏进程         : " + (f"PID {pid}" if pid else "未运行"))
    print("compile.dll      : " + (f"存在（{os.path.getsize(TARGET)} B）"
                                  if os.path.exists(TARGET) else "不存在"))
    print("compile_real.dll : " + ("存在" if os.path.exists(REAL) else "不存在"))
    print("转发层 (work/)   : " + ("存在" if os.path.exists(HOOK) else "不存在"))
    print("当前状态         : " + ("**已部署**（原 DLL 在 compile_real.dll）"
                                  if os.path.exists(REAL) else "**未部署**（原始状态）"))
    return 0


def guard():
    pid = find_pid()
    if pid:
        print(f"❌ 游戏正在运行（PID {pid}）—— DLL 被锁住，改不了名。请先关闭游戏。",
              file=sys.stderr)
        return False
    return True


def deploy():
    if not guard():
        return 1
    if not os.path.exists(HOOK):
        print(f"❌ 转发层不存在：{HOOK}\n   先编译：gcc -shared -O2 -static "
              f"-o work/compile_hook.dll tools/compile_hook.c", file=sys.stderr)
        return 1
    if os.path.exists(REAL):
        print("⚠️ compile_real.dll 已存在，看起来已经部署过了。要用 --restore 先还原。",
              file=sys.stderr)
        return 1
    shutil.move(TARGET, REAL)
    print("✅ compile.dll → compile_real.dll")
    shutil.copy2(HOOK, TARGET)
    print("✅ work/compile_hook.dll → compile.dll")
    print("\n部署完成。现在启动游戏、切换或编辑一次电路，")
    print("源码会写到 work/hook_src_NNN.bin，产物写到 work/hook_out_NNN.bin。")
    return 0


def restore():
    if not guard():
        return 1
    if not os.path.exists(REAL):
        print("⚠️ compile_real.dll 不存在 —— 看起来没部署过，无需还原。", file=sys.stderr)
        return 1
    if os.path.exists(TARGET):
        os.remove(TARGET)
        print("✅ 已删除转发层 compile.dll")
    shutil.move(REAL, TARGET)
    print("✅ compile_real.dll → compile.dll（已还原为原始文件）")
    return 0


def main():
    ap = argparse.ArgumentParser(description="部署/恢复 compile.dll 转发层")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--deploy", action="store_true", help="部署转发层")
    g.add_argument("--restore", action="store_true", help="还原为原始 compile.dll")
    g.add_argument("--status", action="store_true", help="查看当前状态")
    a = ap.parse_args()
    if a.status:
        return status()
    return deploy() if a.deploy else restore()


if __name__ == "__main__":
    sys.exit(main())
