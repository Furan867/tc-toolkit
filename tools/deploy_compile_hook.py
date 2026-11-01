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
    转调 `python simplexc/tools/build.py simplexc_dll` —— 转发层现在是「C 壳 + C++ 编译器核心」，
    构建命令只有 `CMakeLists.txt` 一处定义，本脚本只负责「部署前先把 DLL 重编一份」。

    ⚠️ 因为链了 `LLVM-C.dll`，**部署必须把它（build/LLVM-C.dll）一起拷进游戏目录**
    （否则游戏加载时解析依赖失败，接口 §7）。本脚本不替你做这一步。

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
# 游戏目录不写死（装在哪是机器的事）：`tools/tc_common.py` 是唯一一处定义，
# 游戏在跑没在跑另由 `winmem.find_pid` 判断。
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from tc_common import game_dir, game_dll  # noqa: E402

HOOK = os.path.join(ROOT, "build", "compile_hook.dll")


def game_paths():
    """`(compile.dll, compile_real.dll)` —— **用的时候现算**。

    不在模块级算：导入本文件不应要求这台机器上装着游戏（`build` 子命令根本不碰游戏目录，
    而 `game_dir()` 找不到时会抛）。
    """
    return game_dll(), game_dir() / "compile_real.dll"


def find_pid():
    try:
        from winmem import find_pid as fp
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
    print("转发层 (build/)  : " + ("存在" if os.path.exists(HOOK) else "不存在"))
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


def build():
    """编译转发层到 build/compile_hook.dll —— 转调 build.py 的 `simplexc_dll` 目标。

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
    print("⚠️ 部署时必须把 build/LLVM-C.dll 一起拷进游戏目录（否则游戏解析依赖失败）")
    return 0


def ensure_opt_ini() -> None:
    """补一份 `opt.ini` 模板。

    转发层读 `opt.ini` 的 `[opt] compiler=` 等键，读不到就用内置默认值，**不会自动建文件** ——
    所以新 clone 的仓库里这些开关是隐形的。部署时从模板补一份，让它们可见。
    """
    ini = os.path.join(ROOT, "opt.ini")
    example = os.path.join(ROOT, "opt.ini.example")
    if os.path.exists(ini) or not os.path.exists(example):
        return
    try:
        shutil.copy2(example, ini)
    except OSError as exc:
        print(f"⚠️ 没能生成 opt.ini（不影响部署）：{exc}", file=sys.stderr)
        return
    print("ℹ️ 已从模板生成 opt.ini（切换 compiler / stdlib / rewrite 时改它）")


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
