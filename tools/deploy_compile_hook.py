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
    build/compile_hook.dll → 复制为 compile.dll

恢复做的事
----------
    删掉我们的 compile.dll，把 compile_real.dll 改名回 compile.dll

编译做的事（build）
------------------
    调 w64devkit 的 gcc 把 tools/compile_hook.c 编译成 build/compile_hook.dll
    （gcc 路径可用环境变量 W64DEVKIT_GCC 覆盖）—— 免去手敲那条长命令。

命令
----
    status        看当前状态（是否已部署、游戏是否在跑）
    build         只编译转发层
    deploy        只部署（把 build/ 的 dll 盖上去；游戏在跑会拒绝）
    build-deploy  编译后立刻部署 —— 改完 compile_hook.c 的常规流程
    restore       还原为原始 compile.dll

典型流程：`build-deploy` → 启动游戏。

⚠️ 游戏运行时 DLL 文件被锁、无法改名 —— 脚本检测到游戏在跑会直接拒绝执行。
"""
from __future__ import annotations

import argparse
import contextlib
import os
import shutil
import subprocess
import sys

# Windows 下 stdout 走管道时默认 GBK，输出里的 ✅/❌ 会抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    _rec = getattr(_stream, "reconfigure", None)
    if _rec is not None:
        with contextlib.suppress(Exception):
            _rec(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = r"F:\opt\SteamLibrary\steamapps\common\Turing Complete"
TARGET = os.path.join(GAME, "compile.dll")
REAL = os.path.join(GAME, "compile_real.dll")
HOOK = os.path.join(ROOT, "build", "compile_hook.dll")


def find_pid():
    sys.path.insert(0, HERE)
    try:
        from winmem import find_pid as fp
        return fp()
    except Exception:
        return None


def status():
    pid = find_pid()
    print("游戏进程         : " + (f"PID {pid}" if pid else "未运行"))
    print("compile.dll      : " + (f"存在（{os.path.getsize(TARGET)} B）"
                                  if os.path.exists(TARGET) else "不存在"))
    print("compile_real.dll : " + ("存在" if os.path.exists(REAL) else "不存在"))
    print("转发层 (build/)  : " + ("存在" if os.path.exists(HOOK) else "不存在"))
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


def build():
    """编译转发层到 build/compile_hook.dll。"""
    src = os.path.join(ROOT, "tools", "compile_hook.c")
    dst = os.path.join(ROOT, "build", "compile_hook.dll")
    gcc = os.environ.get("W64DEVKIT_GCC", r"D:\bin\w64devkit\bin\gcc.exe")
    if not os.path.exists(gcc):
        print(f"❌ 找不到 gcc：{gcc}\n   （可用环境变量 W64DEVKIT_GCC 指定）",
              file=sys.stderr)
        return 1
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
    except OSError as exc:
        print(f"❌ 建不出 build 目录：{exc}", file=sys.stderr)
        return 1
    cmd = [gcc, "-shared", "-O2", "-static", "-o", dst, src]
    print("$ " + " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print(f"❌ 编译失败（返回 {r.returncode}）", file=sys.stderr)
        return r.returncode
    print(f"✅ 已编译 {dst}（{os.path.getsize(dst)} B）")
    return 0


def deploy():
    if not guard():
        return 1
    if not os.path.exists(HOOK):
        print(f"❌ 转发层不存在：{HOOK}\n   先编译：py tools/deploy_compile_hook.py build-deploy",
              file=sys.stderr)
        return 1
    if os.path.exists(REAL):
        # 已处于部署状态：只把新的转发层 DLL 盖上去（原 DLL 在 compile_real.dll，
        # 千万不要动它）。这样更新流程就是 --build → --deploy 两下，不必先还原。
        print("ℹ️ 已处于部署状态，只更新转发层 DLL")
        try:
            shutil.copy2(HOOK, TARGET)
        except OSError as exc:
            print(f"❌ 复制失败：{exc}", file=sys.stderr)
            return 1
        print(f"✅ 已更新 {TARGET}（{os.path.getsize(TARGET)} B）")
        return 0
    try:
        shutil.move(TARGET, REAL)
    except OSError as exc:
        print(f"❌ 改名失败（原 DLL 未被改动）：{exc}", file=sys.stderr)
        return 1
    print("✅ compile.dll → compile_real.dll")
    try:
        shutil.copy2(HOOK, TARGET)
    except OSError as exc:
        # 复制失败就把原件改回去，不要留下半部署状态（游戏会干脆加载不了编译器）
        with contextlib.suppress(OSError):
            shutil.move(REAL, TARGET)
        print(f"❌ 复制失败（已还原原 DLL）：{exc}", file=sys.stderr)
        return 1
    print("✅ work/compile_hook.dll → compile.dll")
    print("\n部署完成。现在启动游戏、切换或编辑一次电路，")
    print("源码会写到 work/hook/hook_src_NNN.si，产物写到 work/hook/hook_out_NNN.bin。")
    return 0


def restore():
    if not guard():
        return 1
    if not os.path.exists(REAL):
        print("⚠️ compile_real.dll 不存在 —— 看起来没部署过，无需还原。", file=sys.stderr)
        return 1
    try:
        if os.path.exists(TARGET):
            os.remove(TARGET)
            print("✅ 已删除转发层 compile.dll")
        shutil.move(REAL, TARGET)
    except OSError as exc:
        print(f"❌ 还原失败：{exc}", file=sys.stderr)
        return 1
    print("✅ compile_real.dll → compile.dll（已还原为原始文件）")
    return 0


def main():
    ap = argparse.ArgumentParser(description="部署/恢复 compile.dll 转发层")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="查看当前状态")
    sub.add_parser("build", help="编译 tools/compile_hook.c → build/compile_hook.dll")
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
