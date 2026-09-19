#!python3
"""建立「组件 → 机器码地址范围」映射表。

原理（2026-09-16 实测验证，坐标偏差恒等）：
    1. 在每个组件代码块前插入一条 store(唯一地址, 唯一魔数) 作为标记
    2. 编译带标记版本，在机器码里搜魔数立即数 → 锚点位置
    3. 标记的机器码恒为 17 字节
       （movabs rcx,addr / mov edi,imm32 / mov [rcx],edi）
    4. 坐标变换精确成立：pos_无标记 = pos_带标记 − 17×(i+1)

另在组件区之后插一条「终点标记」，用来界定最后一个组件的结束 —— 否则它的区间
会一路延伸到产物末尾，把循环尾部等公共代码全算进去（实测虚高到 89%）。

因为游戏内 JIT 代码与离线编译产物逐字节相同（已验证），本表可直接把
sample_threads.py 采到的 RIP 落到具体组件上。

输入：生成的 Simplex 源码（scan_game_mem.py 的产物）
输出：work/maps/component_map.tsv
      seg / comp_index / comp_id / type / src_line / jit_start / jit_end / size

⚠️ **一行是一个「注释段」，不是一行一个组件**：同一个组件（comp_id 相同）在源码里
有两条注释（组件代码 + 绑定）、各生成一段机器码，它们共享同一个 comp_index。
按行数数会把组件数翻倍（实测 1234 段 = 617 个组件 × 2，见 STATUS.md ⑥-A）；
要组件级的数字，必须按 comp_index 聚合。

注意 jit_* 是相对「JIT 代码起点」的偏移（产物开头那 8 字节 NimStringDesc
长度头不算在内），所以：绝对地址 = JIT 区基址 + jit_start
"""
from __future__ import annotations

import argparse
import ctypes
import os
import re
import struct
import sys
import time

# Windows 下 stdout 走管道时默认 GBK，输出的中文会变成乱码
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")

from try_compile_dll import load_dll, make_nim_string  # noqa: E402
from term import duration  # noqa: E402

MARK_SIZE = 17      # 标记的机器码长度，实测恒定
MAGIC_BASE = 0x13370000
OFFSET_BASE = 4095  # 标记写入的 #SIMULATION_STATE 偏移，避开真实状态区
NIMSTR_HDR = 8      # 产物开头的 NimStringDesc 长度头；游戏内 JIT 代码没有它


def parse_components(lines):
    """扫出组件注释，返回 [(行下标, 组件编号, 类型)]。"""
    comps = []
    for i, l in enumerate(lines):
        m = re.match(r"\s*//\s*(\d+)\s+(com_\w+)", l)
        if m:
            comps.append((i, int(m.group(1)), m.group(2)))
    return comps


def insert_marks(lines, comps):
    """每个组件注释行之前插一条标记；最后一个组件之后（跳过空行）再插一条终点标记。"""
    n = len(comps)
    end_at = -1
    if n:
        i = comps[-1][0] + 1
        while i < len(lines) and lines[i].strip() == "":
            i += 1
        end_at = i
    out, ci = [], 0
    for i, l in enumerate(lines):
        if ci < n and i == comps[ci][0]:
            out.append(f"    store(#SIMULATION_STATE + {OFFSET_BASE + ci}, "
                       f"U32 {MAGIC_BASE + ci})")
            ci += 1
        if i == end_at:
            out.append(f"    store(#SIMULATION_STATE + {OFFSET_BASE + n}, "
                       f"U32 {MAGIC_BASE + n})")
        out.append(l)
    return "\n".join(out)


def compile_to_bytes(dll, source: bytes) -> bytes:
    """调 compile.dll 编译，返回机器码（含开头 8 字节 NimStringDesc 长度头）。"""
    src, _keep = make_nim_string(source)
    out = ctypes.create_string_buffer(64)
    ctypes.memset(out, 0xCC, 64)
    dll.compile.restype = ctypes.c_void_p
    dll.compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    dll.compile(out, ctypes.byref(src), 0)
    raw = bytes(out)
    out_len, out_ptr = struct.unpack_from("<qQ", raw, 0)
    if out_len <= 0 or not out_ptr:
        raise RuntimeError("compile() 返回 out_len == 0（编译失败）")
    return ctypes.string_at(out_ptr, out_len)


def main():
    ap = argparse.ArgumentParser(description="建立「组件 → 机器码地址范围」映射表")
    ap.add_argument("--src", required=True, help="生成的 Simplex 源码（scan_game_mem 产物）")
    ap.add_argument("--out", default=os.path.join(WORK, "maps", "component_map.tsv"))
    ap.add_argument("--keep", action="store_true", help="保留带标记的源码与产物（调试用）")
    args = ap.parse_args()

    text = open(args.src, encoding="utf-8", errors="replace").read()
    lines = text.split("\n")
    comps = parse_components(lines)
    if not comps:
        print("源码里没找到组件注释（// <编号> com_xxx）", file=sys.stderr)
        return 1
    # ⚠️ 同一个组件在源码里有**两条**注释（组件代码 + 绑定），**两条各生成一段机器码**。
    # 按注释段建表会让组件数翻倍 —— 实测 1234 段 = 617 个组件 × 2（STATUS.md ⑥-A）。
    # 所以按 comp_id 首次出现的顺序分配组件序号，多段归到同一个组件；
    # 段仍是段（各有自己的机器码区间），聚合留给 analyze 侧按 comp_index 做。
    comp_of = {}
    for _, cid, _ in comps:
        if cid not in comp_of:
            comp_of[cid] = len(comp_of)
    print(f"  源码 {len(lines)} 行 → {len(comp_of)} 个组件")

    marked_text = insert_marks(lines, comps)
    dll = load_dll(call_nimmain=False, quiet=True)
    if dll is None:
        return 1

    # end="" + flush=True：让"正在编译"立刻可见，结果接着打在同一行。
    # 不 flush 的话中途那 0.8 秒没有任何输出，看起来像卡住（踩过）。
    # 耗时按标尺上色：正常约 0.8s（淡灰）；十几秒多半是解释器选错了，
    # 见 TOOLS.md 坑 5 —— 那时它是唯一能一眼看出异常的地方
    print("  编译无标记版本 ...", end="", flush=True)
    t = time.time()
    plain = compile_to_bytes(dll, text.encode("utf-8"))
    dt = time.time() - t
    print(f" 产物 {len(plain)} 字节（{duration(dt, format(dt, '.2f') + 's')}）")
    print("  编译带标记版本 ...", end="", flush=True)
    t = time.time()
    marked = compile_to_bytes(dll, marked_text.encode("utf-8"))
    dt = time.time() - t
    print(f" 产物 {len(marked)} 字节（{duration(dt, format(dt, '.2f') + 's')}）")

    anchors = []
    for i in range(len(comps) + 1):          # 最后一枚是终点标记
        p = marked.find(struct.pack("<I", MAGIC_BASE + i))
        if p < 0:
            print(f"标记 {i}（魔数 0x{MAGIC_BASE + i:X}）未命中", file=sys.stderr)
            return 1
        anchors.append(p)
    end_anchor = anchors.pop()
    print(f"  锚点 {len(anchors)}/{len(comps)} 全部命中")

    jit_len = len(plain) - NIMSTR_HDR
    rows = []
    for i in range(len(comps)):
        s_marked = anchors[i] + 6                            # 该组件的标记结束处
        e_marked = anchors[i + 1] - 11 if i + 1 < len(comps) else end_anchor - 11
        shift = MARK_SIZE * (i + 1)
        s = max(0, s_marked - shift - NIMSTR_HDR)
        e = min(e_marked - shift - NIMSTR_HDR, jit_len)
        rows.append((i, comp_of[comps[i][1]], comps[i][1], comps[i][2],
                     comps[i][0] + 1, s, e))

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("seg\tcomp_index\tcomp_id\ttype\tsrc_line\tjit_start\tjit_end\tsize\n")
        for r in rows:
            f.write(f"{r[0]}\t{r[1]}\t{r[2]}\t{r[3]}\t{r[4]}\t{r[5]}\t{r[6]}\t{r[6] - r[5]}\n")

    covered = max(r[6] for r in rows)
    print(f"  组件覆盖 0..{covered} 字节；其后 {jit_len - covered} 字节是公共代码"
          f"（循环尾部等，不属于任何组件）")
    print(f"  映射表 → {os.path.relpath(args.out, ROOT)}")

    if args.keep:
        for path, data in ((os.path.join(WORK, "maps", "marked.si"), marked_text.encode("utf-8")),
                           (os.path.join(WORK, "maps", "marked.bin"), marked),
                           (os.path.join(WORK, "maps", "plain.bin"), plain)):
            with open(path, "wb") as f:
                f.write(data)
        print(f"  中间产物 → {os.path.relpath(os.path.join(WORK, 'maps'), ROOT)}/"
              f"marked.si, marked.bin, plain.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
