#!python3
"""判据工具：产物里的绝对地址（`movabs r64,imm64`）落在哪个模块里。

为什么需要它：离线重编译的产物与 work/hook/all 里游戏内捕获的参考产物**逐字节不相同**，
差异全部落在这些立即数上。要么证明「差的就是同一个模块镜像的加载基址」（→ 两边语义等价），
要么承认产物不可复现 —— 这是判据，不能只写在笔记里。

结论（可用本工具重跑复现）：产物里的绝对地址指向 **compile_real.dll 自己的镜像**
（偏移落在它自己的节里，部分像 call 目标、部分像 .bss 槽位）。该基址按 boot 随机、
boot 内跨进程恒定 → 跨 boot 逐字节不可复现；把基址归一化后两边逐字节相同。

用法：
    python simplexc/tools/classify_abs_addrs.py --bin simplexc/corpus/ref/005.bin
    python simplexc/tools/classify_abs_addrs.py --pair simplexc/corpus/ref/005.bin work/hook/all/hook_out_005.bin
    python simplexc/tools/classify_abs_addrs.py --all

⚠ 依赖活的进程映射：VirtualQuery 只能命中「本 boot 里加载过 compile_real.dll」的那些地址。
本工具会先自己加载一次 compile_real.dll，所以在同一 boot 内重跑结论稳定；换 boot 后两侧地址
都会报成「未映射」，此时用 PE 节表 + 包含性检查给出无映射情况下的结论（命令仍可跑）。
"""
from __future__ import annotations

import argparse
import ctypes
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)


def _load_by_path(path, name):
    """按文件路径加载模块（reversing/tools/ 不是包，静态 import 解析不到）。"""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


T = _load_by_path(os.path.join(REPO, "tools", "try_compile_dll.py"), "try_compile_dll")

GAME = r"F:\opt\SteamLibrary\steamapps\common\Turing Complete"
DLL_REAL = os.path.join(GAME, "compile_real.dll")
DEFAULT_OURS = os.path.join(SIMPLEXC, "corpus", "ref")
DEFAULT_REFS = os.path.join(REPO, "work", "hook", "all")

IMAGE_LO, IMAGE_HI = 0x7F0000000000, 0x800000000000
ALLOC_GRANULARITY = 0x10000          # 镜像基址按 64K 对齐
MEM_FREE = 0x10000                   # VirtualQuery 对未分配区间也返回成功，State=MEM_FREE
SECTION_EXECUTABLE = 0x20000000


def read(path):
    with open(path, "rb") as f:
        return f.read()


def hexs(v):
    return f"0x{v:X}"


# ---------------------------------------------------------------- PE（不依赖加载，跨 boot 可用）

def read_pe(path):
    """只读 PE 头，取 ImageBase / SizeOfImage / 节表。"""
    data = read(path)
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit(f"{path} 不是 PE 文件")
    n_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise SystemExit(f"{path} 不是 PE32+")
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    image_size = struct.unpack_from("<I", data, opt + 56)[0]
    sections = []
    off = opt + opt_size
    for _ in range(n_sections):
        name = data[off:off + 8].split(b"\0")[0].decode("ascii", "replace")
        vsize, va = struct.unpack_from("<II", data, off + 8)
        raw_size = struct.unpack_from("<I", data, off + 16)[0]
        chars = struct.unpack_from("<I", data, off + 36)[0]
        sections.append({"name": name, "va": va, "size": max(vsize, raw_size),
                         "executable": bool(chars & SECTION_EXECUTABLE)})
        off += 40
    return {"image_base": image_base, "image_size": image_size, "sections": sections}


def section_of(pe, offset):
    for s in pe["sections"]:
        if s["va"] <= offset < s["va"] + s["size"]:
            return s
    return None


# ---------------------------------------------------------------- 产物里的 movabs r64,imm64

def imm64_starts(buf):
    """所有 `movabs r64,imm64` 立即数的起始偏移（立即数本身，前 2 字节是 REX.W + B8+r）。"""
    starts = []
    for i in range(2, len(buf) - 8):
        if 0x48 <= buf[i - 2] <= 0x4F and 0xB8 <= buf[i - 1] <= 0xBF:
            starts.append(i)
    return starts


def iter_imm64_sites(buf):
    """返回 [(立即数起始偏移, 立即数值)]，按偏移升序。"""
    return [(s, struct.unpack_from("<Q", buf, s)[0]) for s in imm64_starts(buf)]


def covering_site(starts, index):
    """返回覆盖字节位置 index 的那个立即数起始偏移；没有则 None。"""
    for s in range(max(0, index - 9), index - 1):
        if s in starts:
            return s
    return None


def is_image(v):
    return IMAGE_LO <= v < IMAGE_HI


# ---------------------------------------------------------------- 活进程映射

_k32 = ctypes.WinDLL("kernel32")
_psapi = ctypes.WinDLL("psapi")


class _MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", ctypes.c_ulong), ("RegionSize", ctypes.c_size_t),
                ("State", ctypes.c_ulong), ("Protect", ctypes.c_ulong), ("Type", ctypes.c_ulong)]


_k32.VirtualQuery.restype = ctypes.c_size_t
_k32.VirtualQuery.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
_psapi.GetMappedFileNameW.restype = ctypes.c_ulong
_psapi.GetMappedFileNameW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                                      ctypes.c_ulong]


def query_mapping(addr):
    """VirtualQuery + GetMappedFileNameW：地址 → 映射区信息；未映射返回 None。

    ⚠ VirtualQuery 对**未分配**的地址也返回成功（State=MEM_FREE，AllocationBase=0），
    所以必须显式判 MEM_FREE，否则会把空闲区当成已映射。
    """
    mbi = _MBI()
    if not _k32.VirtualQuery(ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        return None
    if not mbi.BaseAddress or not mbi.AllocationBase or mbi.State == MEM_FREE:
        return None
    buf = ctypes.create_unicode_buffer(600)
    n = _psapi.GetMappedFileNameW(ctypes.c_void_p(-1), ctypes.c_void_p(mbi.AllocationBase),
                                  buf, 600)
    return {"base": int(mbi.BaseAddress), "alloc_base": int(mbi.AllocationBase),
            "size": int(mbi.RegionSize), "protect": int(mbi.Protect), "type": int(mbi.Type),
            "mapped": os.path.basename(buf.value) if n else None}


def classify_value(value, pe):
    """单个地址的分类：返回 (说明, 镜像内偏移或 None)。"""
    m = query_mapping(value)
    if m and m["mapped"]:
        return f"活进程映射 {m['mapped']}（alloc_base={hexs(m['alloc_base'])}）", value - m["alloc_base"]
    if m:
        return f"活进程映射 私有区（protect={hexs(m['protect'])}, type={m['type']}）", None
    return "未映射", None


def implied_bases(values, pe):
    """无映射时的兜底：能装下全部地址的 64K 对齐镜像基址候选。"""
    if not values:
        return []
    lo = -(-(max(values) - pe["image_size"] + 1) // ALLOC_GRANULARITY)
    hi = min(values) // ALLOC_GRANULARITY
    return [b * ALLOC_GRANULARITY for b in range(max(lo, 0), hi + 1)][:8]


# ---------------------------------------------------------------- 三种模式

def load_compiler():
    dll = T.load_dll(call_nimmain=False, quiet=True, prefer_real=True)
    if dll is None:
        raise SystemExit("加载 compile_real.dll 失败（活进程分类需要它）")
    return dll


def describe_bin(path, pe, sample):
    buf = read(path)
    sites = iter_imm64_sites(buf)
    image = [(s, v) for s, v in sites if is_image(v)]
    other = [v for s, v in sites if not is_image(v)]
    print(f"{os.path.basename(path)}：{len(buf)} B，movabs r64,imm64 共 {len(sites)} 处"
          f"（image 空间 {len(image)} 处，其它 {len(other)} 处）")
    by_value = {}
    for s, v in image:
        by_value.setdefault(v, []).append(s)
    print(f"  image 空间去重后 {len(by_value)} 个不同地址，示例：")
    for v in sorted(by_value)[:sample]:
        where, offset = classify_value(v, pe)
        sec = section_of(pe, offset) if offset is not None else None
        suffix = f"  偏移={hexs(offset)}" + (f" 节={sec['name']}" if sec else "")
        print(f"    {hexs(v)} ×{len(by_value[v])}  {where}{suffix}")
    bases = implied_bases(sorted(by_value), pe)
    if bases:
        span = (hexs(bases[0]) if len(bases) == 1
                else f"{hexs(bases[0])}..{hexs(bases[-1])}（{len(bases)} 个 64K 候选）")
        print(f"  无映射兜底：compile_real.dll SizeOfImage={hexs(pe['image_size'])}，"
              f"能装下全部地址的基址={span}")
    for v in other[:sample]:
        where, _ = classify_value(v, pe)
        print(f"    {hexs(v)}（非 image 空间）{where}")


def pair_analysis(ours_path, ref_path, pe):
    """一对产物：差异是否全部落在 image 空间 movabs 立即数上 + 偏移一致性 + 基址推导。

    返回 (结果 dict 或 None, 说明)。结果里 sites = [(偏移, 本进程值, 参考值)]。
    """
    ours, ref = read(ours_path), read(ref_path)
    if len(ours) != len(ref):
        return None, f"长度不同：{len(ours)} vs {len(ref)}"
    diff = [i for i in range(len(ours)) if ours[i] != ref[i]]
    if not diff:
        return {"sites": [], "deltas": set(), "delta": None, "rebased_equal": True,
                "bases": [], "unclassified": []}, "逐字节相同"
    starts = set(imm64_starts(ours))
    runs, sites, unclassified = [], [], []
    for i in diff:
        if runs and i == runs[-1][1] + 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    for a, b in runs:
        found = None
        for i in range(a, b + 1):
            s = covering_site(starts, i)
            if s is not None:
                found = s
                break
        if found is None:
            unclassified.append((a, b))
            continue
        vo = struct.unpack_from("<Q", ours, found)[0]
        vr = struct.unpack_from("<Q", ref, found)[0]
        if not (is_image(vo) and is_image(vr)):
            unclassified.append((a, b))
            continue
        sites.append((found, vo, vr))
    deltas = {vo - vr for _, vo, vr in sites}
    # 差值恒定 + 无未分类差异 → 归一化（把差值减掉）后必须回到逐字节相同
    delta = next(iter(deltas)) if len(deltas) == 1 and not unclassified else None
    rebased_equal = False
    bases = []
    if delta is not None:
        patched = bytearray(ours)
        for s, vo, _ in sites:
            struct.pack_into("<Q", patched, s, vo - delta)
        rebased_equal = bytes(patched) == ref
        m = query_mapping(sites[0][1]) if sites else None
        our_base = m["alloc_base"] if m and m["mapped"] else None
        if our_base:
            ref_base = our_base - delta
            bases = [("本进程 compile_real.dll 基址", our_base),
                     ("参考产物推导出的基址", ref_base),
                     ("参考地址都落在同一镜像区间内",
                      all(ref_base <= vr < ref_base + pe["image_size"] for _, _, vr in sites)),
                     ("镜像内偏移两侧一致",
                      all(vo - our_base == vr - ref_base for _, vo, vr in sites))]
    return {"sites": sites, "deltas": deltas, "delta": delta, "rebased_equal": rebased_equal,
            "bases": bases, "unclassified": unclassified}, ""


def cmd_pair(args, pe):
    ours_path, ref_path = args.pair
    res, note = pair_analysis(ours_path, ref_path, pe)
    if res is None:
        print(note)
        return 1
    if note:
        print(f"{note}（无需分类）")
        return 0
    print(f"{os.path.basename(ours_path)} vs {os.path.basename(ref_path)}："
          f"差异站点 {len(res['sites'])} 个（movabs 立即数），未分类 {len(res['unclassified'])} 处，"
          f"差值 {hexs(res['delta']) if res['delta'] is not None else '-'}，"
          f"归一化后逐字节相同：{'yes' if res['rebased_equal'] else 'no'}")
    for label, val in res["bases"]:
        print(f"  {label}：{val if isinstance(val, bool) else hexs(val)}")
    print(f"  示例（前 {args.sample} 个站点）：")
    for s, vo, vr in sorted(res["sites"])[:args.sample]:
        where, offset = classify_value(vo, pe)
        sec = section_of(pe, offset) if offset is not None else None
        suffix = f" 节={sec['name']}" if sec else ""
        print(f"    立即数偏移 {s:>7}：本进程 {hexs(vo)} / 参考 {hexs(vr)}"
              f"  差值 {hexs(vo - vr)}  {where}{suffix}")
    if res["bases"]:
        hist = section_hist(res["sites"], res["bases"][0][1])
        print("  差异站点落在 compile_real.dll 的节：" + fmt_hist(hist))
    return 0 if res["delta"] is not None and res["rebased_equal"] else 1


def section_hist(sites, our_base):
    """站点按 compile_real.dll 的节归类 → {节名: 站点数}。"""
    hist = {}
    for _, vo, _ in sites:
        sec = section_of(PE, vo - our_base)
        key = sec["name"] if sec else "?"
        hist[key] = hist.get(key, 0) + 1
    return hist


def fmt_hist(hist):
    return "、".join(f"{k} {v} 处" for k, v in sorted(hist.items(), key=lambda kv: -kv[1])) or "-"


def cmd_all(args, pe):
    ids = sorted(f[:3] for f in os.listdir(args.ours_dir) if f.endswith(".bin"))
    total_sites, all_deltas, offsets_ok, identical, clean, rebase_eq, heap = \
       0, set(), 0, 0, 0, 0, 0
    secs, bad = {}, []
    for cid in ids:
        ours_path = os.path.join(args.ours_dir, f"{cid}.bin")
        ref_path = os.path.join(args.refs_dir, f"hook_out_{cid}.bin")
        if not os.path.exists(ref_path):
            bad.append(f"{cid}: 缺 {ref_path}")
            continue
        res, note = pair_analysis(ours_path, ref_path, pe)
        if res is None:
            bad.append(f"{cid}: {note}")
            continue
        if note:
            identical += 1
            continue
        total_sites += len(res["sites"])
        all_deltas |= res["deltas"]
        clean += not res["unclassified"]
        rebase_eq += bool(res["rebased_equal"])
        if res["bases"] and res["bases"][-1][1] is True:
            offsets_ok += 1
        if res["bases"]:
            for k, v in section_hist(res["sites"], res["bases"][0][1]).items():
                secs[k] = secs.get(k, 0) + v
        heap += sum(1 for _, v in iter_imm64_sites(read(ours_path)) if not is_image(v))
    n_diff = len(ids) - identical
    print(f"逐对分析 {len(ids)} 对产物（{args.ours_dir} vs {args.refs_dir}/hook_out_*.bin）")
    print(f"  逐字节相同 {identical} 对；有差异的站点合计 {total_sites} 处")
    print(f"  差值集合 {'、'.join(hexs(d) for d in sorted(all_deltas)) or '-'}")
    print(f"  未分类差异 0 处：{clean}/{n_diff} 对；归一化后逐字节相同：{rebase_eq}/{n_diff} 对")
    print(f"  参考地址落在同一镜像区间且偏移一致：{offsets_ok}/{n_diff} 对")
    print("  差异站点落在 compile_real.dll 的节：" + fmt_hist(secs))
    print(f"  产物里非 image 空间的 movabs（大整数常量、堆 / JIT 地址等）合计 {heap} 处")
    for b in bad:
        print("  ✗ " + b)
    return 1 if bad else 0


PE = {}


def main():
    global PE
    ap = argparse.ArgumentParser(description="产物里的绝对地址落在哪个模块里")
    ap.add_argument("--bin", help="单份产物")
    ap.add_argument("--pair", nargs=2, metavar=("OURS", "REF"), help="一对产物（本次 vs 参考）")
    ap.add_argument("--all", action="store_true", help="遍历 corpus/ref 全部产物逐对分析")
    ap.add_argument("--ours-dir", default=DEFAULT_OURS)
    ap.add_argument("--refs-dir", default=DEFAULT_REFS)
    ap.add_argument("--sample", type=int, default=6, help="示例打印几条（默认 6）")
    args = ap.parse_args()
    PE = read_pe(DLL_REAL)
    print(f"compile_real.dll：ImageBase={hexs(PE['image_base'])} "
          f"SizeOfImage={hexs(PE['image_size'])} "
          f"节={','.join(s['name'] for s in PE['sections'])}")
    load_compiler()
    if args.bin:
        describe_bin(args.bin, PE, args.sample)
        return 0
    if args.pair:
        return cmd_pair(args, PE)
    if args.all:
        return cmd_all(args, PE)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
