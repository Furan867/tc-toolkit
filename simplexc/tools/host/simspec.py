#!python3
"""从捕获的 .si 里读出差分宿主需要的「环境规格」。

宿主不是编译器：它只需要知道**程序期待看见什么内存** —— 命令块/设置块在哪个地址、枚举成员各是几号、
哪些地址是数组（要写 Nim seq 头）。

⚠ 枚举按**声明顺序从 0 起**取值：语料里没出现过显式值。这不是已证结论 —— 但猜错的表现很直接
（发 `run` 后 `sim_running` 不变、tick 等不到 → 宿主超时报错），不会静默错。
"""
from __future__ import annotations

import contextlib
import re

ENUM_RE = re.compile(r"^type\s+(\w+)\s+Enum\[([^\]]*)\]", re.M)
PTR_RE = re.compile(r"^\s*(?:var\s+(\w+)|const\s+#(\w+))\s*=\s*Ptr\s+(\d{10,})\s*(?://.*)?$", re.M)
# 生成器给块地址的行尾注释里写着这个块有多大：
#   const #DATA_…  = Ptr … // """ & $(c_alloc(134217728) + 2048) & "\n" & """
# `c_alloc(N)` 的 N 是块字节数，`+ 2048` 是块首地址比记录地址小的偏移。
# 不读这两个数，宿主就只按「地址」映射一小块，而参考产物的 reset_sim **每 tick** 整块
# memcpy 一遍内存镜像（RV32IM：128 MB，产物 +0x2391 的 `rep movs`）→ 越界 AV（实测）。
CALLOC_RE = re.compile(r"c_alloc\((\d+)\)")
CALLOC_BACK_RE = re.compile(r"c_alloc\(\d+\)\s*\+\s*(\d+)")
PTR_LINE_RE = re.compile(r"^\s*(?:var\s+(\w+)|const\s+#(\w+))\s*=\s*Ptr\s+(\d{10,})([^\n]*)$", re.M)
# 宿主该映射多大，以**程序实际碰的区间**为准：`reset_sim` 里的 memory_copy/memory_clear 实参。
# 不能用 `c_alloc(N)` —— 实测 1 号组 `c_alloc(134217728)` 但 `memory_copy(…, 134221824)`，
# 差 4096 字节；按 N 映射就会在拷贝末尾越界 AV（AV 地址正好落在映射末尾，实测）。
MEMCLEAR_RE = re.compile(r"memory_clear\(\s*#(\w+)\s*(?:-\s*(\d+))?\s*,\s*(\d+)\s*\)")
MEMCOPY_RE = re.compile(r"memory_copy\(\s*#(\w+)\s*(?:-\s*(\d+))?\s*,\s*#(\w+)\s*(?:-\s*(\d+))?"
                        r"\s*,\s*(\d+)\s*\)")
ARR_RE = re.compile(r"^\s*(?:var\s+(\w+)|const\s+#(\w+))\s*=\s*raw_cast\(\s*<Arr\[(\w+)\]>\s*,\s*(\d{10,})\s*\)",
                    re.M)
NUM_RE = re.compile(r"0[xX][0-9a-fA-F]+|\b\d+\b")

# 元素宽度（字节）：只列语料里 Arr[...] 出现过的类型
ELEM_SIZE = {"U64": 8, "Int": 8, "U32": 4, "U16": 2, "U8": 1, "Char": 1, "Bool": 1, "S64": 8,
             "S32": 4, "S16": 2, "S8": 1, "Uint": 8, "Sint": 8}


class SimSpec:
    def __init__(self):
        self.enums = {}          # 名字 → {成员: 序号}
        self.scalars = []        # [(名字, 地址)]
        self.blocks = []         # [(名字, 地址, 块首回退字节, c_alloc 声明的字节数)]
        self.ranges = []         # [(lo, hi)]：memory_copy/memory_clear 实参算出的绝对区间
        self.arrays = []         # [(名字, 地址, 元素字节)]
        self.literals = set()    # 源码里出现过的整数（判"这个立即数是常量不是地址"）
        self.path = ""

    # -- 解析 ------------------------------------------------------------
    @classmethod
    def parse(cls, path):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as e:
            raise SystemExit(f"读不了 {path}：{e}") from e
        self = cls()
        self.path = path
        for m in ENUM_RE.finditer(text):
            members = [x.strip() for x in m.group(2).split(",") if x.strip()]
            self.enums[m.group(1)] = {name: i for i, name in enumerate(members)}
        for m in PTR_LINE_RE.finditer(text):
            name = m.group(1) or m.group(2)
            addr = int(m.group(3))
            self.scalars.append((name, addr))
            tail = m.group(4)
            cm = CALLOC_RE.search(tail)
            if cm:
                bm = CALLOC_BACK_RE.search(tail)
                self.blocks.append((name, addr, int(bm.group(1)) if bm else 0, int(cm.group(1))))
        for m in ARR_RE.finditer(text):
            name = m.group(1) or m.group(2)
            elem = ELEM_SIZE.get(m.group(3))
            if elem is None:
                raise SystemExit(f"{path}: 数组 {name} 的元素类型 Arr[{m.group(3)}] 宽度未知")
            self.arrays.append((name, int(m.group(4)), elem))
        for m in NUM_RE.finditer(text):
            with contextlib.suppress(ValueError):
                self.literals.add(int(m.group(0), 0))
        self._collect_ranges(text)
        return self

    def _collect_ranges(self, text):
        """从 memory_copy/memory_clear 的实参里算出程序真会碰到的绝对区间。"""
        namemap = dict(self.scalars)
        for m in MEMCLEAR_RE.finditer(text):
            a = namemap.get(m.group(1))
            if a is None:
                continue
            lo = a - int(m.group(2) or 0)
            self.ranges.append((lo, lo + int(m.group(3))))
        for m in MEMCOPY_RE.finditer(text):
            n = int(m.group(5))
            for name, back in ((m.group(1), m.group(2)), (m.group(3), m.group(4))):
                a = namemap.get(name)
                if a is None:
                    continue
                lo = a - int(back or 0)
                self.ranges.append((lo, lo + n))

    # -- 查询 ------------------------------------------------------------
    def enum(self, name):
        if name not in self.enums:
            raise SystemExit(f"{self.path}: 找不到 type {name} Enum[...]")
        return self.enums[name]

    def addr(self, name):
        for n, a in self.scalars:
            if n == name:
                return a
        for n, a, _s in self.arrays:
            if n == name:
                return a
        raise SystemExit(f"{self.path}: 找不到地址 {name}")

    def all_addrs(self):
        return [a for _n, a in self.scalars] + [a for _n, a, _s in self.arrays]

    def regions(self):
        """[(lo, hi)] —— 宿主该映射的绝对区间（未页对齐）。

        优先用 `memory_copy`/`memory_clear` 的实参（那是程序真会碰的范围）；
        没有 memop 指向的地址，退化成 `[addr, addr)`（宿主会给默认空间）。
        """
        out = list(self.ranges)
        for _n, a in self.scalars:
            out.append((a, a))
        for _n, a, _e in self.arrays:
            out.append((a, a))
        return out

    @property
    def game_lo(self):
        return min(self.all_addrs())

    @property
    def game_hi(self):
        return max(self.all_addrs())


if __name__ == "__main__":
    import sys
    s = SimSpec.parse(sys.argv[1])
    print(f"枚举：{ {k: len(v) for k, v in s.enums.items()} }")
    print(f"标量地址 {len(s.scalars)} 个，数组 {len(s.arrays)} 个，跨度 "
          f"0x{s.game_lo:X}..0x{s.game_hi:X}")
    print(f"SimCommand: {s.enums.get('SimCommand')}")
    print(f"CommandIndex: {s.enums.get('CommandIndex')}")
    print(f"StateIndex: {s.enums.get('StateIndex')}")
    print(f"数组: {[(n, hex(a), e) for n, a, e in s.arrays]}")
