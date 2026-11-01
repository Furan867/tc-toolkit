"""从 PE 的 .pdata 节提取每个函数的精确边界 (x64 RUNTIME_FUNCTION)。

用途
----
定向反汇编时给出精确的 --start-address / --stop-address，
不必再靠"下一个符号"猜函数结尾。

用法
----
    python tools/pdata_bounds.py                      # 生成 work/function_bounds.tsv
    python tools/pdata_bounds.py --symbols work/symbols_raw.txt   # 附带函数名
    python tools/pdata_bounds.py --lookup wrap_sleep  # 查函数边界 + 打印 objdump 命令
    python tools/pdata_bounds.py --exe "X:\\path\\to.exe"         # 指定其它二进制

输出
----
work/function_bounds.tsv，列为:
    start_va <TAB> end_va <TAB> size <TAB> name
地址为 16 位小写十六进制（与 nm 输出格式一致，便于 join）。
名列为 "-" 表示未提供符号表。
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # tc-toolkit/

DEFAULT_EXE = r"F:\opt\SteamLibrary\steamapps\common\Turing Complete\Turing Complete.exe"
DEFAULT_OUT = os.path.join(ROOT, "work", "function_bounds.tsv")


def parse_pe(path):
    """返回 (数据, ImageBase, {节名: 节信息})。"""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 0x40:
        raise ValueError("文件过小，不是有效 PE")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("未找到 PE 签名")

    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    optsz = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24

    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        raise ValueError(f"仅支持 PE32+ (x64)，当前 magic=0x{magic:X}")

    image_base = struct.unpack_from("<Q", data, opt + 24)[0]

    secs = {}
    for i in range(nsec):
        o = opt + optsz + i * 40
        name = data[o:o + 8].rstrip(b"\x00").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        secs[name] = {"vaddr": vaddr, "vsize": vsize,
                      "rawptr": rawptr, "rawsize": rawsize}
    return data, image_base, secs


def read_pdata(data, image_base, sec):
    """解析 RUNTIME_FUNCTION 数组 -> [(start_va, end_va, unwind_va)]。"""
    off, size = sec["rawptr"], sec["rawsize"]
    if not off or not size:
        raise ValueError(".pdata 节无文件数据")

    out = []
    for i in range(size // 12):
        begin, end, unwind = struct.unpack_from("<III", data, off + i * 12)
        if begin == 0 and end == 0:  # 尾部对齐填充
            continue
        out.append((image_base + begin, image_base + end, image_base + unwind))
    out.sort()
    return out


def load_symbols(path):
    """读取 nm 输出 -> {VA: 符号名}，仅收 T/t (函数)。"""
    syms = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3 or parts[1] not in ("T", "t"):
                continue
            try:
                syms[int(parts[0], 16)] = parts[2]
            except ValueError:
                continue
    return syms


def check_stale(exe, symfile):
    """符号表比 exe 旧时告警 —— 游戏被 Steam 更新后符号地址会整体位移。"""
    if not symfile:
        return
    exe_m = os.path.getmtime(exe)
    sym_m = os.path.getmtime(symfile)
    if sym_m < exe_m:
        import datetime
        print(f"⚠ 警告: 符号表比二进制旧", file=sys.stderr)
        print(f"    {symfile}\n      {datetime.datetime.fromtimestamp(sym_m)}",
              file=sys.stderr)
        print(f"    {exe}\n      {datetime.datetime.fromtimestamp(exe_m)}",
              file=sys.stderr)
        print(f"    Steam 更新会使全部函数地址位移，函数名与边界可能对不上。",
              file=sys.stderr)
        print(f"    建议先重新 nm 生成符号表。", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description="提取 PE .pdata 中的函数边界")
    ap.add_argument("--exe", default=DEFAULT_EXE, help="目标 PE 文件")
    ap.add_argument("--out", default=DEFAULT_OUT, help="输出 TSV 路径")
    ap.add_argument("--symbols", help="nm 输出文件，用于附加函数名")
    ap.add_argument("--lookup", help="按子串查函数，打印边界与 objdump 命令")
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print(f"错误: 找不到 {args.exe}", file=sys.stderr)
        return 1

    data, image_base, secs = parse_pe(args.exe)
    if ".pdata" not in secs:
        print("错误: 该 PE 无 .pdata 节（非 x64 或已剥离）", file=sys.stderr)
        return 1

    funcs = read_pdata(data, image_base, secs[".pdata"])
    syms = {}
    if args.symbols:
        if not os.path.isfile(args.symbols):
            print(f"错误: 找不到符号表 {args.symbols}", file=sys.stderr)
            return 1
        check_stale(args.exe, args.symbols)
        syms = load_symbols(args.symbols)

    # ---- 查询模式 ----
    if args.lookup:
        if not syms:
            print("错误: --lookup 需要同时提供 --symbols（否则无法按名字查找）",
                  file=sys.stderr)
            return 1
        needle = args.lookup.lower()
        hits = []
        for start, end, _ in funcs:
            name = syms.get(start)
            if name and needle in name.lower():
                hits.append((name, start, end))
        if not hits:
            print(f"未找到名字含 '{args.lookup}' 的函数（匹配的是 nm 的 mangled 名，"
                  f"可尝试模块片段如 component95menu）", file=sys.stderr)
            return 1
        for name, start, end in hits[:10]:
            print(name)
            print(f"  start = 0x{start:X}")
            print(f"  end   = 0x{end:X}   (size 0x{end - start:X} = {end - start} 字节)")
            print(f"  objdump -d --start-address=0x{start:X} "
                  f"--stop-address=0x{end:X} \"{args.exe}\"")
            print()
        if len(hits) > 10:
            print(f"... 另有 {len(hits) - 10} 个匹配（仅显示前 10）")
        return 0

    # ---- 导出模式 ----
    outdir = os.path.dirname(args.out)
    if outdir:
        os.makedirs(outdir, exist_ok=True)

    named = 0
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# start_va\tend_va\tsize\tname\n")
        f.write(f"# source: {args.exe}\n")
        f.write(f"# image base: 0x{image_base:X}\n")
        f.write(f"# functions: {len(funcs)}\n")
        for start, end, _ in funcs:
            name = syms.get(start, "-")
            if name != "-":
                named += 1
            f.write(f"{start:016x}\t{end:016x}\t{end - start}\t{name}\n")

    sizes = [e - s for s, e, _ in funcs]
    zero = sum(1 for s in sizes if s == 0)
    print(f"二进制      : {args.exe}")
    print(f"ImageBase   : 0x{image_base:X}")
    print(f"函数数      : {len(funcs)}")
    if zero:
        print(f"  其中零长度: {zero} 条（.pdata 特殊条目, 非真实函数, 可忽略）")
    print(f"地址范围    : 0x{funcs[0][0]:X} - 0x{funcs[-1][1]:X}")
    print(f"函数体大小  : 最小 {min(sizes)} 字节, 最大 {max(sizes)} 字节, "
          f"中位 {sorted(sizes)[len(sizes) // 2]} 字节")
    if syms:
        print(f"符号匹配    : {named}/{len(funcs)} 命中")
        nop = [v for v in syms if v not in {s for s, _, _ in funcs}]
        print(f"有符号无边界: {len(nop)} 个（叶子函数，无展开信息）")
    print(f"输出        : {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
