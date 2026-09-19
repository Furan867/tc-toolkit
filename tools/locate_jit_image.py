#!python3
"""在游戏进程里定位某个 JIT 产物的加载地址。

为什么需要它
------------
hook 到 compile() 的产物之后，就有了一条**精确判据**来找仿真代码：

    RIP 落在 [base, base + len(产物)) 内的线程，才是在跑这份电路

这比原来那个判据（`is_exec_non_image`：RIP 落在「非 IMAGE 且可执行」的区）
精确得多 —— 后者会把旧电路残留、其它用途的 JIT 代码一起算进来，所以线程
「探测」总是时灵时不灵。有了 base，判据就是确定的字节范围。

做法
----
从产物**中段**取一段特征字节，在目标进程的可执行区里搜；每命中一次都做
**整段比对**确认，避免特征碰撞。

⚠️ **不能用产物开头做特征** —— 实测所有产物的开头都是同一段固定前缀，
拿它去搜会「全部命中」，等于没判据（这个坑踩过）。

用法
----
    python tools/locate_jit_image.py --img work/hook_out_013.bin
    python tools/locate_jit_image.py --img work/hook_out_013.bin --all
"""
from __future__ import annotations

import argparse
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# Windows 下 stdout 走管道时默认 GBK，输出里的符号会抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

from winmem import (
    k32, find_pid, rpm, PROCESS_QUERY_INFO, PROCESS_VM_READ,
    MBI, iter_exec_regions,
)

MEM_COMMIT = 0x1000
PAGE_EXEC_MASK = 0x10 | 0x20 | 0x40 | 0x80      # EXECUTE / EXECUTE_READ / _RW / _WC
PAGE_GUARD = 0x100
MEM_IMAGE = 0x1000000

# 特征取自产物中段：跳过开头（固定前缀）和末尾（可能被对齐填充）
FEATURE_OFF = 0x400
FEATURE_LEN = 256
MAX_REGION = 128 * 1024 * 1024                 # 超过这个大小的区不整体读


def locate(h, body, want_all=False, feature_off=FEATURE_OFF):
    """在目标进程的可执行区里找 body 的加载地址。

    body 必须是**已去掉 8 字节 NimStringDesc 头**的机器码 —— 游戏拷进 JIT 区的
    是去掉头之后的部分。带着头去搜，特征偏移会整体错位 8 字节，症状是
    「特征多次命中、整段校验全失败」（实测踩到，4 个区里偏移齐刷刷差 8）。

    返回 [(加载地址, 是否整段校验通过, 区类型)]。
    """
    if len(body) < feature_off + FEATURE_LEN:
        off = max(0, len(body) // 3)     # 太小就退而求其次，从中间取
    else:
        off = feature_off
    feat = body[off:off + FEATURE_LEN]
    hits = []
    for base, ty, data in iter_exec_regions(h):
        pos = 0
        while True:
            i = data.find(feat, pos)
            if i < 0:
                break
            cand = base + i - off
            whole = rpm(h, cand, len(body))
            if whole == body:
                hits.append((cand, True, ty))
                if not want_all:
                    return hits
            else:
                hits.append((cand, False, ty))
            pos = i + 1
    return hits


def main():
    ap = argparse.ArgumentParser(description="定位 JIT 产物在目标进程里的加载地址")
    ap.add_argument("--img", required=True, help="产物文件（hook_out_*.bin）")
    ap.add_argument("--pid", type=int, help="进程 PID（默认自动查找）")
    ap.add_argument("--all", action="store_true", help="列出全部候选，不只第一个")
    ap.add_argument("--feature-off", type=int, default=FEATURE_OFF,
                    help=f"特征在产物中的偏移（默认 {FEATURE_OFF}，避开固定前缀）")
    args = ap.parse_args()

    img = open(args.img, "rb").read()
    # 产物文件的前 8 字节是长度头，不是机器码 —— 游戏拷进 JIT 区的是去掉头之后的部分。
    # ⚠️ 头字段的值等于**文件总长（含头自己）**，不是数据长度。按
    #    「head == len(img) - 8」判断会判不出来，于是带着头去搜、特征偏移整体错 8 字节，
    #    症状是「特征命中很多次、整段校验全失败」（实测踩到：35 个候选全废）。
    head = struct.unpack("<q", img[:8])[0] if len(img) >= 8 else -1
    body = img[8:] if head == len(img) else img

    pid = args.pid or find_pid()
    if not pid:
        print("❌ 游戏没在运行", file=sys.stderr)
        return 1

    print(f"产物 {os.path.basename(args.img)}  文件 {len(img)} 字节"
          + (f" → 去掉 8 字节头后机器码 {len(body)} 字节" if body is not img else ""))
    print(f"特征偏移 0x{args.feature_off:X}，长度 {FEATURE_LEN} 字节")
    print(f"在 PID {pid} 的可执行区里搜索 ...")

    h = k32.OpenProcess(PROCESS_QUERY_INFO | PROCESS_VM_READ, False, pid)
    if not h:
        print("❌ OpenProcess 失败", file=sys.stderr)
        return 1
    try:
        hits = locate(h, body, want_all=args.all, feature_off=args.feature_off)
    finally:
        k32.CloseHandle(h)

    ok = [x for x in hits if x[1]]
    print()
    if not ok:
        print(f"❌ 没找到（候选 {len(hits)} 个，全部未通过整段校验）")
        print("   可能：这份产物还没被加载（电路没在跑），或它在超过 "
              f"{MAX_REGION // 1024 // 1024}MB 的区里被跳过了")
        return 1
    for addr, verified, ty in hits:
        mark = "✅ 整段一致" if verified else "⚠ 特征命中但整段不符（忽略）"
        print(f"  base = 0x{addr:X}   {mark}   区类型 {ty}")
    print()
    b = ok[0][0]
    print(f"→ 机器码加载于 0x{b:X} .. 0x{b + len(body):X}")
    print(f"  仿真判据：RIP 落在这个范围内 = 在跑这份电路")
    return 0


if __name__ == "__main__":
    sys.exit(main())
