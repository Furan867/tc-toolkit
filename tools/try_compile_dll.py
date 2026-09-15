#!/usr/bin/env python3
"""试验：独立加载 compile.dll，看能否脱离游戏进程调用它。

目的：为「自己编译电路源码 → 获得编译产物 → 精确测量组件成本」探路。

分两步，逐步验证，每步都可能失败/崩溃：
    python tools/try_compile_dll.py --load       # ① 只加载 + 调 NimMain 初始化
    python tools/try_compile_dll.py --compile    # ② 再试调用 compile()

已知的 compile() 调用契约（从反汇编读出）：
    Result* compile(Result* out, NimStringDesc* src, int flags)
      rcx = out    输出结构指针（40 字节，sret 约定）
      rdx = src    NimStringDesc {len @0; data @8}
      r8d = flags  含义未知，先用 0

⚠ 调用第三方 dll 可能导致进程崩溃，所以本脚本只应在独立进程中运行，
  不要嵌进别的程序里。
"""
from __future__ import annotations

import argparse
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = r"F:\opt\SteamLibrary\steamapps\common\Turing Complete"
DLL = os.path.join(GAME, "compile.dll")


def load_dll(call_nimmain=True):
    """加载 compile.dll。

    注意：Nim 编译的 dll 通常已经在 DllMain 里自行初始化。实测调用
    NimMain() 会触发 `types.nim(1515) source_buffer.len == 0 / Only call
    init once` 断言——所以默认**不**调它，只在需要时显式指定。
    """
    if not os.path.isfile(DLL):
        print(f"找不到 {DLL}", file=sys.stderr)
        return None

    # compile.dll 依赖同目录的 libgcc_s_seh-1.dll / libwinpthread-1.dll，
    # 必须先把游戏目录加进 dll 搜索路径，否则 LoadLibrary 会失败。
    os.add_dll_directory(GAME)

    print(f"加载 {DLL} ...")
    dll = ctypes.CDLL(DLL)
    print("  加载成功")

    if call_nimmain:
        print("调用 NimMain() 初始化运行时 ...")
        dll.NimMain.restype = None
        dll.NimMain()
        print("  初始化完成")
    else:
        print("  跳过 NimMain（假定 DllMain 已完成初始化）")
    return dll


def make_nim_string(text: bytes):
    """构造 NimStringDesc：offset 0 = len(int64)，offset 8 = data（结尾补 \\0）。"""
    n = len(text)
    buf = ctypes.create_string_buffer(8 + n + 1)
    struct.pack_into("<q", buf, 0, n)
    buf[8:8 + n] = text
    buf[8 + n] = b"\x00"
    return buf


def try_compile(dll, source: bytes):
    """调用 compile()，返回 40 字节原始结果。"""
    src = make_nim_string(source)
    out = ctypes.create_string_buffer(64)      # 结构只占 40，留点余量
    ctypes.memset(out, 0xCC, 64)               # 先填哨兵，方便看哪些字节被写过
    dll.compile.restype = ctypes.c_void_p
    dll.compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    print(f"调用 compile(src={len(source)} 字节) ...")
    ret = dll.compile(out, src, 0)
    print(f"  返回 0x{ret:X}（out 缓冲在 0x{ctypes.addressof(out):X}）")
    return out.raw


def main():
    ap = argparse.ArgumentParser(description="独立调用 compile.dll")
    ap.add_argument("--load", action="store_true", help="只加载 + NimMain")
    ap.add_argument("--compile", action="store_true", help="再试调用 compile()")
    ap.add_argument("--src", help="源码文件（默认用一小段测试文本）")
    ap.add_argument("--nimmain", action="store_true",
                    help="显式调用 NimMain()（默认不调，见 load_dll 注释）")
    args = ap.parse_args()

    dll = load_dll(call_nimmain=args.nimmain)
    if dll is None:
        return 1
    if args.load and not args.compile:
        print("\n① 通过：可以独立加载并初始化。")
        return 0

    # ② 试调 compile
    if args.src:
        source = open(args.src, "rb").read()
    else:
        source = b"\n"      # 空源码，先看 API 是否接受
    print()
    raw = try_compile(dll, source)
    print()
    print("输出结构原始字节（sentinel 0xCC = 未被写过）:")
    for i in range(0, 48, 16):
        chunk = raw[i:i + 16]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        sentinel = "  <未写>" if chunk == b"\xcc" * len(chunk) else ""
        print(f"  +0x{i:02X}: {hexs}{sentinel}")
    print()
    print("按 5 个 qword 解读:")
    for i in range(5):
        v = struct.unpack_from("<Q", raw, i * 8)[0]
        note = ""
        if v == 0xCCCCCCCCCCCCCCCC:
            note = "  (未被写过)"
        elif 0x10000 < v < 0x800000000000:
            note = "  ← 像指针或长度"
        print(f"  [{i}] 0x{v:016X}{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
