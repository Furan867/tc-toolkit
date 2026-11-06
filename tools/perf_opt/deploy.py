"""部署 / 恢复 / 状态查询（游戏目录里的 compile.dll 与 compile_real.dll）。"""
from __future__ import annotations

import contextlib
import os
import shutil
import sys

from libs.tc_common import game_dir, game_dll
from tools.perf_opt.config import HOOK, ensure_opt_ini


def game_paths():
    """`(compile.dll, compile_real.dll)` —— **用的时候现算**。

    不在模块级算：导入本文件不应要求这台机器上装着游戏（`build` 子命令根本不碰游戏目录，
    而 `game_dir()` 找不到时会抛）。
    """
    return game_dll(), game_dir() / "compile_real.dll"


def find_pid():
    try:
        from libs.winmem import find_pid as fp
        return fp()
    except Exception:
        return None


def status():
    target, real = game_paths()
    pid = find_pid()
    print("游戏进程         : " + (f"PID {pid}" if pid else "未运行"))
    print("compile.dll      : " + (f"存在（{target.stat().st_size} B）"
                                  if target.exists() else "不存在"))
    print("compile_real.dll : " + ("存在" if real.exists() else "不存在"))
    print("转发层 (simplexc/build/) : " + ("存在" if os.path.exists(HOOK) else "不存在"))
    print("当前状态         : " + ("**已部署**（原 DLL 在 compile_real.dll）"
                                  if real.exists() else "**未部署**（原始状态）"))
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
    ensure_opt_ini()
    target, real = game_paths()
    if not os.path.exists(HOOK):
        print(f"❌ 转发层不存在：{HOOK}\n   先编译：py tools/deploy_compile_hook.py build-deploy",
              file=sys.stderr)
        return 1
    if real.exists():
        # 已处于部署状态：只把新的转发层 DLL 盖上去（原 DLL 在 compile_real.dll，
        # 千万不要动它）。这样更新流程就是 --build → --deploy 两下，不必先还原。
        print("ℹ️ 已处于部署状态，只更新转发层 DLL")
        try:
            shutil.copy2(HOOK, target)
        except OSError as exc:
            print(f"❌ 复制失败：{exc}", file=sys.stderr)
            return 1
        print(f"✅ 已更新 {target}（{target.stat().st_size} B）")
        return 0
    try:
        shutil.move(target, real)
    except OSError as exc:
        print(f"❌ 改名失败（原 DLL 未被改动）：{exc}", file=sys.stderr)
        return 1
    print("✅ compile.dll → compile_real.dll")
    try:
        shutil.copy2(HOOK, target)
    except OSError as exc:
        # 复制失败就把原件改回去，不要留下半部署状态（游戏会干脆加载不了编译器）
        with contextlib.suppress(OSError):
            shutil.move(real, target)
        print(f"❌ 复制失败（已还原原 DLL）：{exc}", file=sys.stderr)
        return 1
    print("✅ work/compile_hook.dll → compile.dll")
    print("\n部署完成。现在启动游戏、切换或编辑一次电路，")
    print("源码会写到 work/hook/hook_src_NNN.si，产物写到 work/hook/hook_out_NNN.bin。")
    return 0


def restore():
    if not guard():
        return 1
    target, real = game_paths()
    if not real.exists():
        print("⚠️ compile_real.dll 不存在 —— 看起来没部署过，无需还原。", file=sys.stderr)
        return 1
    try:
        if target.exists():
            target.unlink()
            print("✅ 已删除转发层 compile.dll")
        shutil.move(real, target)
    except OSError as exc:
        print(f"❌ 还原失败：{exc}", file=sys.stderr)
        return 1
    print("✅ compile_real.dll → compile.dll（已还原为原始文件）")
    return 0
