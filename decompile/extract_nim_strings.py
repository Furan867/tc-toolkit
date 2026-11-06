#!python3
"""从 PE 镜像里抽取 Nim 的字符串字面量池与「字符串指针表」。

用途
----
`compile_real.dll`（Nim 编译）把编译器内部的名字表（`mac_*` 内建/宏名、`com_*` 组件名、
`exp_*` 表达式种类…）作为字符串字面量放在 `.rdata` 里。要判断「编译器认哪些内建名」
（主设计 §11 U5），这份池子是最权威的一手来源 —— 比从前缀/语料反推可靠。

原理
----
1. NimStringDesc 布局：`{ u64 len | (1<<62) ; char data[len] ; NUL }`，`bit62 = strlit 标志`
   （与产物头 bit62 门控同源，见 `docs/facts.md` §「产物头 8 字节」）；
2. 字面量池里的条目按**声明顺序**紧邻排列 → 同一张表的名字会连续出现；
3. 文件里的内部指针写成 `ImageBase + RVA`（PE 重定位前形态）→ 可以据此找到
   「连续 ≥N 个指向字面量的指针」（Nim `seq[string]` 载荷的静态形态）。

两条子命令
----------
    py decompile/extract_nim_strings.py <dll> lits  [--out FILE]
        按偏移顺序 dump 全部字面量（`偏移<TAB>内容`）。找名字表用这个。
    py decompile/extract_nim_strings.py <dll> ptrs  [--min N]
        找「连续字符串指针」构成的名字表。

输出量可能到千行级 —— 重定向到文件再看，别直接灌进上下文。
"""
from __future__ import annotations

import argparse
import struct
import sys

BIT62 = 1 << 62


def parse_pe(path):
    try:
        with open(path, 'rb') as f:
            d = f.read()
    except OSError as exc:
        raise SystemExit(f'读不到 {path}：{exc}') from exc
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe+4] != b'PE\0\0':
        raise SystemExit('不是 PE 文件')
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    if magic == 0x20B:
        image_base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
    else:
        image_base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    off = pe + 24 + optsz
    for _ in range(nsec):
        name = d[off:off+8].rstrip(b'\0').decode('latin1')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', d, off + 8)
        secs.append((name, va, vsize, rawptr, rawsize))
        off += 40
    return d, image_base, secs


def rva_to_off(secs, rva):
    for name, va, vsize, rawptr, rawsize in secs:
        if va <= rva < va + max(vsize, rawsize) and rawptr:
            return rawptr + (rva - va), name
    return None, None


def read_str_desc(d, off):
    """off 指向描述符起点；成功返回文本，失败 None。"""
    if off < 0 or off + 8 > len(d):
        return None
    hdr = struct.unpack_from('<Q', d, off)[0]
    if not (hdr & BIT62):
        return None
    ln = hdr & ~BIT62
    if ln == 0 or ln > 8192 or off + 8 + ln >= len(d):
        return None
    s = d[off+8:off+8+ln]
    if any(c < 32 or c > 126 for c in s) or d[off+8+ln] != 0:
        return None
    return s.decode('ascii')


def literals(d, secs):
    """按偏移顺序产出 (文件偏移, 文本)。"""
    lo = min((rawptr for _, _, _, rawptr, _ in secs if rawptr), default=0)
    hi = max((rawptr + rawsize for _, _, _, rawptr, rawsize in secs if rawptr), default=len(d))
    out = []
    off = lo
    while off < hi - 8:
        hdr = struct.unpack_from('<Q', d, off)[0]
        if hdr & BIT62:
            ln = hdr & ~BIT62
            if 0 < ln <= 8192 and off + 8 + ln < len(d):
                s = d[off+8:off+8+ln]
                if all(32 <= c <= 126 for c in s) and d[off+8+ln] == 0:
                    out.append((off, s.decode('ascii')))
                    off += 8 + ln + 1
                    off = (off + 7) & ~7
                    continue
        off += 8
    return out


def pointer_tables(d, image_base, secs, min_items):
    image_size = sum(max(vs, rs) for _, _, vs, _, rs in secs)
    ptr2text = {}
    for pos in range(0, len(d) - 8, 8):
        v = struct.unpack_from('<Q', d, pos)[0]
        if not (image_base <= v < image_base + image_size):
            continue
        soff, _ = rva_to_off(secs, v - image_base)
        if soff is None:
            continue
        s = read_str_desc(d, soff)
        if s is not None:
            ptr2text[pos] = s
    tables, pos_list, i = [], sorted(ptr2text), 0
    while i < len(pos_list):
        j = i
        while j + 1 < len(pos_list) and pos_list[j+1] == pos_list[j] + 8:
            j += 1
        if j - i + 1 >= min_items:
            tables.append((pos_list[i], [ptr2text[pos_list[k]] for k in range(i, j + 1)]))
        i = j + 1
    return tables


def emit(fh, mode, min_items, d, image_base, secs):
    if mode == 'lits':
        n = 0
        for off, s in literals(d, secs):
            fh.write(f'0x{off:06X}\t{s}\n')
            n += 1
        print(f'# 字面量 {n} 条', file=sys.stderr)
    else:
        print(f'# ImageBase=0x{image_base:X}', file=fh)
        for off, names in pointer_tables(d, image_base, secs, min_items):
            head = ', '.join(names[:6])
            fh.write(f'0x{off:06X}\t{len(names)}\t{head}{" …" if len(names) > 6 else ""}\n')


def main():
    ap = argparse.ArgumentParser(description='抽 Nim 字符串字面量池 / 指针表')
    ap.add_argument('dll')
    ap.add_argument('mode', choices=['lits', 'ptrs'])
    ap.add_argument('--out', help='写到文件（默认 stdout）')
    ap.add_argument('--min', type=int, default=3, help='ptrs 模式：最少连续条目（默认 3）')
    a = ap.parse_args()

    d, image_base, secs = parse_pe(a.dll)
    if a.out:
        try:
            with open(a.out, 'w', encoding='utf-8') as fh:
                emit(fh, a.mode, a.min, d, image_base, secs)
        except OSError as exc:
            raise SystemExit(f'写不到 {a.out}：{exc}') from exc
        print(f'# 已写 {a.out}', file=sys.stderr)
    else:
        emit(sys.stdout, a.mode, a.min, d, image_base, secs)


if __name__ == '__main__':
    sys.exit(main())
