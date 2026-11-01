#!python3
"""把产物里的 `movabs r64,imm64` 立即数分类 —— 差分宿主的第一块地基。

宿主必须「按产物写死的地址」把内存摆出来，所以先得知道产物里到底有哪些绝对地址、各属于谁：

  game   —— 游戏数据段（源码里 `Ptr <digits>` / `raw_cast(<T>, <digits>)` 写死的地址）：宿主自己 VirtualAlloc。
  thunk  —— `compile_real.dll` 的导入 thunk（RVA 已知）→ 正是要中和成宿主桩的那些。
  dll    —— DLL 镜像内但在 thunk 之外（数据/IAT/…）：宿主若不加载真 DLL，就得自己造一块私有镜像。
  bigint —— 其余（真的大整数常量）。

DLL 基址按 boot 随机，但**可以只从产物反推**：真基址必须让尽可能多的 movabs 落在已知 thunk RVA 集合里
—— 取「所有 (v - t) 里出现次数最多的那个」即可（v = 产物 movabs，t = DLL 的 thunk RVA）。

用法：
    python simplexc/tools/abs_scan.py --src simplexc/corpus/src/020.si --bin simplexc/corpus/ref/020.bin
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)

# 源码里地址的写法（实测：这三类是全部；`#DATA_*` 是符号名不是字面量）
ADDR_PATTERNS = [
    re.compile(r"\bPtr\s+(\d{10,})\b"),
    re.compile(r"raw_cast\s*\(\s*<[^>]*>\s*,\s*(\d{10,})\s*\)"),
    re.compile(r"=\s*Ptr\s+(\d{10,})\b"),
]
ANY_BIG_NUM = re.compile(r"(?<![\w.])(\d{10,})(?![\w.])")


def _load_by_path(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


CAA = _load_by_path(os.path.join(HERE, "classify_abs_addrs.py"), "classify_abs_addrs")
TC = _load_by_path(os.path.join(REPO, "tools", "tc_common.py"), "tc_common")

GAME = str(TC.game_dir())
DLL_REAL = os.path.join(GAME, TC.GAME_REAL_DLL_NAME)


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError as e:
        raise SystemExit(f"读不了 {path}：{e}") from e


# ---------------------------------------------------------------- 源码地址表

def to_int(s):
    try:
        return int(s)
    except ValueError:
        return None


def src_addrs(path):
    """源码里出现过的所有字面量地址 → (地址集合, 未归类的大数字计数)。"""
    text = read_text(path)
    addrs = set()
    for pat in ADDR_PATTERNS:
        for m in pat.finditer(text):
            v = to_int(m.group(1))
            if v is not None:
                addrs.add(v)
    others = collections.Counter()
    for m in ANY_BIG_NUM.finditer(text):
        v = to_int(m.group(1))
        if v is not None and v not in addrs:
            others[v] += 1
    return addrs, others


# ---------------------------------------------------------------- PE：节与导入表

class Pe:
    def __init__(self, path):
        self.data = CAA.read(path)
        self.meta = CAA.read_pe(path)
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        n_sections = struct.unpack_from("<H", self.data, e_lfanew + 6)[0]
        # ⚠ SizeOfOptionalHeader 在 COFF 头里（e_lfanew+20），不是可选头里（e_lfanew+24+20）
        self.opt_size = struct.unpack_from("<H", self.data, e_lfanew + 20)[0]
        self.opt = e_lfanew + 24
        self.sections = []
        off = self.opt + self.opt_size
        for _ in range(n_sections):
            name = self.data[off:off + 8].split(b"\0")[0].decode("ascii", "replace")
            vsize, va = struct.unpack_from("<II", self.data, off + 8)
            raw_size, raw_ptr = struct.unpack_from("<II", self.data, off + 16)
            chars = struct.unpack_from("<I", self.data, off + 36)[0]
            self.sections.append({"name": name, "va": va, "vsize": vsize,
                                  "raw_ptr": raw_ptr, "raw_size": raw_size, "chars": chars})
            off += 40

    @property
    def image_size(self):
        return self.meta["image_size"]

    def rva_to_off(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["raw_size"]):
                if rva - s["va"] >= s["raw_size"]:
                    return None
                return s["raw_ptr"] + (rva - s["va"])
        return None

    def imports(self):
        """→ [(IAT 槽 RVA, 'dll!proc')]，顺序即 ILT 顺序。"""
        dirs = self.opt + 112
        imp_rva, imp_size = struct.unpack_from("<II", self.data, dirs + 8)
        out = []
        if not imp_rva:
            return out
        for i in range(imp_size // 20):
            base = self.rva_to_off(imp_rva + i * 20)
            if base is None:
                break
            ilt, _ts, _fc, name_rva, iat_rva = struct.unpack_from("<IIIII", self.data, base)
            if not name_rva and not iat_rva:
                break
            if not iat_rva:
                continue
            noff = self.rva_to_off(name_rva) if name_rva else None
            dll = (self.data[noff:self.data.index(b"\0", noff)].decode("ascii", "replace")
                   if noff is not None else "?")
            src_off = self.rva_to_off(ilt or iat_rva)
            if src_off is None:
                continue
            for k in range(4096):
                v = struct.unpack_from("<Q", self.data, src_off + k * 8)[0]
                if v == 0:
                    break
                if v & (1 << 63):
                    name = f"#{v & 0xFFFF}"
                else:
                    hn = self.rva_to_off(v & 0x7FFFFFFF)
                    name = (self.data[hn + 2:self.data.index(b"\0", hn + 2)]
                            .decode("ascii", "replace") if hn is not None else "?")
                out.append((iat_rva + k * 8, f"{dll}!{name}"))
        return out

    def import_thunks(self):
        """扫可执行节里的 `jmp QWORD PTR [rip+rel32]`（FF 25 + rel32）→ {thunk RVA: 'dll!proc'}。

        判定：跳转目标落在导入表 IAT 槽上（那个槽才是真正的函数地址）。
        """
        slot2name = dict(self.imports())
        thunks = {}
        if not slot2name:
            return thunks
        for s in self.sections:
            if not (s["chars"] & 0x20000000) or not s["raw_size"]:
                continue
            blob = self.data[s["raw_ptr"]:s["raw_ptr"] + s["raw_size"]]
            for j in range(len(blob) - 6):
                if blob[j] != 0xFF or blob[j + 1] != 0x25:
                    continue
                # ⚠ MinGW 发的是 `48 FF 25 rel32`（REX.W 前缀）—— 指令起点在 FF 前一个字节，
                # 而产物 movabs 引用的是**指令起点**（实测 0x1310 / 0x1320，16 字节一格）。
                start = j - 1 if j > 0 and blob[j - 1] == 0x48 else j
                target = s["va"] + j + 6 + struct.unpack_from("<i", blob, j + 2)[0]
                if target in slot2name:
                    thunks[s["va"] + start] = slot2name[target]
        return thunks


# ---------------------------------------------------------------- 分类

def classify(src, binpath, examples=6):
    addrs, others = src_addrs(src)
    lo, hi = (min(addrs), max(addrs)) if addrs else (0, 0)
    pe = Pe(DLL_REAL)
    thunks = pe.import_thunks()
    buf = CAA.read(binpath)
    sites = CAA.iter_imm64_sites(buf)
    vals = collections.Counter(v for _o, v in sites)

    # 用「出现次数最多的 (v - t)」反推 DLL 基址（真基址必须让最多的 movabs 命中 thunk 表）
    base_votes = collections.Counter()
    for v in vals:
        if addrs and lo <= v <= hi:
            continue
        for t in thunks:
            if (v - t) % 0x10000 == 0:          # 模块基址按 64K 对齐 → 先筛再投票
                base_votes[v - t] += 1
    cands = base_votes.most_common(4)
    base = cands[0][0] if cands else 0

    buckets = collections.defaultdict(list)
    for v in vals:
        if addrs and lo <= v <= hi:
            buckets["game"].append(v)
        elif base and v - base in thunks:
            buckets["thunk"].append(v)
        elif base and 0 <= v - base < pe.image_size:
            buckets["dll"].append(v)
        else:
            buckets["bigint"].append(v)

    print(f"源码 {os.path.basename(src)}：字面量地址 {len(addrs)} 个，"
          f"跨度 0x{lo:X}..0x{hi:X}（{(hi - lo) / 1048576:.1f} MB）")
    if others:
        print(f"  未归类的 10+ 位大数字：{len(others)} 个，例 "
              f"{[hex(v) for v, _c in others.most_common(3)]}")
    print(f"产物 {os.path.basename(binpath)}：movabs 站点 {len(sites)} 个，不同立即数 {len(vals)} 个")
    print(f"DLL：导入 {len(pe.imports())} 个符号，thunk {len(thunks)} 个，镜像 0x{pe.image_size:X}")
    print(f"反推基址候选（页对齐、按票数）：{[(hex(b), n) for b, n in cands[:3]]}")
    print("分类：")
    for k in ("game", "thunk", "dll", "bigint"):
        vs = buckets[k]
        uniq = sorted(set(vs))
        print(f"  {k:6} 站点 {len(vs):5}  不同值 {len(uniq):5}"
              + (f"  例 {[hex(v) for v in uniq[:examples]]}" if uniq else ""))
    if buckets["dll"]:
        cnt = collections.Counter()
        for v in set(buckets["dll"]):
            s = CAA.section_of(pe.meta, v - base)
            cnt[s["name"] if s else "?"] += 1
        print(f"    dll 那些落在的节：{dict(cnt)}")
    if buckets["thunk"]:
        names = collections.Counter(thunks[v - base] for v in buckets["thunk"])
        print(f"    thunk 目标：{dict(names.most_common(12))}")
    return buckets


def main(argv=None):
    ap = argparse.ArgumentParser(description="产物 movabs 立即数分类")
    ap.add_argument("--src", required=True, help="源码 .si（取地址表）")
    ap.add_argument("--bin", required=True, help="产物 .bin")
    ap.add_argument("--examples", type=int, default=6)
    args = ap.parse_args(argv)
    classify(args.src, args.bin, args.examples)
    return 0


if __name__ == "__main__":
    sys.exit(main())
