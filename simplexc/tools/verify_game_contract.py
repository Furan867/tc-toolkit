#!python3
"""Steam 更新后的重验：主设计 §10 的 5 条断言（按符号名定位，不硬编码地址）。

背景
----
游戏一更新，我们在 exe / `compile_real.dll` 上依赖的全部结论（调用契约、产物执行流程、失败判定、
bit62 门控、产物头语义）都要重新确认。本脚本把这些结论写成可重跑的判据：**先按符号名定位函数、
按 `.pdata` 取函数边界，再对该函数体做指令模式匹配**；任何一处都不硬编码地址，连 Nim 的 `_uNNNN`
唯一编号也只当可选后缀（放在更新后可能位移，不能当身份）。

5 条断言（编号同设计 §10）
--------------------------
    A  `compile` 导出仍存在 + exe 内唯一调用点 + flags 实参为 0
    B  `jit` 内存在 memcpy(dest, ptr+8, len) 与尾跳 jmp
    C  `handle_request_compile_and_run` 内存在对 out[3] 的 test/jcc 失败分支
    D  `eqdestroy` 内存在 testb $0x40,0x7(p) + rawDealloc（bit62 门控）
    E  产物头 8 字节的值仍不被读（除 bit62）—— **判不了，恒报 UNKNOWN**

E 为什么恒报 UNKNOWN：这是「全程序没有任何一处读头 word」的否定性命题，静态证明需要全程序数据流，
本脚本给不出；动态判定（放一份头字垃圾的产物进游戏看行为）要部署自研实现，超出本脚本范围。
所以 E 只做**能判伪的近似**：列出全 `.text` 里「从某指针 +8 处 memcpy」与「读头第 7 字节」的全部
出现点。找到 jit 之外的读者 → 近似判据被证伪（输出里 WARN）；找不到也不等于证成立。

定位方式（每条断言的「定位」行都会写出实际用的那一招）
------------------------------------------------------
1. PE 头 / 节表 / 导出表 / COFF 符号表 / `.pdata` 全部本脚本自己解析（纯 Python），
   不复用 `work/symbols/*`（那些是上一次会话的快照，Steam 更新后必然过时）。
2. 函数地址 = COFF 符号名正则匹配；函数长度 = `.pdata` 的 RUNTIME_FUNCTION 条目
   （没有对应条目时退化成「同一节里下一个函数符号」并在输出里注明）。
3. 函数体内的指令模式：`objdump -d --start-address/--stop-address` **只反汇编那一段**
   （不整段反汇编 9.7 MB 的 exe）。
4. 需要扫全 `.text` 的（间接调用点计数、memcpy 调用点）走原始字节模式，不经反汇编器。

退出码
------
    0  没有 FAIL（可能仍有 UNKNOWN —— 摘要里会醒目打印，UNKNOWN 不算 PASS）
    1  有 FAIL
    2  跑不起来（文件缺失 / 没有 objdump / PE 解析失败）

用法
----
    python simplexc/tools/verify_game_contract.py
    python simplexc/tools/verify_game_contract.py --game-dir "D:/symlink/steamapps_c2/common/Turing Complete"
    python simplexc/tools/verify_game_contract.py --json

⚠ 对游戏目录**只读**：本脚本不写、不改、不部署任何东西。
"""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from typing import Any, cast

# Windows 下 stdout 走管道时默认 GBK，输出的中文会变成乱码
for _s in (sys.stdout, sys.stderr):
    with contextlib.suppress(Exception):
        cast(Any, _s).reconfigure(encoding="utf-8", errors="replace")


_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load_by_path(path, name):
    """按文件路径加载模块（tools/ 不是包，静态 import 解析不到）。"""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TC = _load_by_path(os.path.join(_REPO, "tools", "tc_common.py"), "tc_common")

EXE_NAME = TC.GAME_EXE_NAME
REAL_DLL_NAME = TC.GAME_REAL_DLL_NAME
ACF_NAME = "appmanifest_1444480.acf"

PASS, FAIL, UNKNOWN = "PASS", "FAIL", "UNKNOWN"

# 符号名正则（`_uNNNN` 是 Nim 的唯一编号，更新后可能变，一律可选）
SYM_JIT = r"^jit__modelZsimulationZjitZjit(_u\d+)?$"
SYM_HANDLE = r"^handle_request_compile_and_run__modelZsimulationZsimulator95functions(_u\d+)?$"
SYM_EQDESTROY_TIMES = r"^eqdestroy___pureZtimes(_u\d+)?$"
SYM_RAWDALLOC = r"^rawDealloc__system(_u\d+)?$"
SYM_MEMCPY = r"^_?memcpy$"
SYM_NIMGETPROC = r"^nimGetProcAddr$"
SYM_SENDCMD = r"^send_command__modelZsimulator95types(_u\d+)?$"
SYM_CREATETHREAD = r"^createThread__modelZsimulationZsimulator95functions(_u\d+)?$"
SYM_CJIT = r"^c_jit(_u\d+)?$"

# flags 实参的置零写法（objdump 文本）
R8_ZERO = (
    re.compile(r"^xor\s+%r8d,%r8d$"),
    re.compile(r"^xor\s+%r8,%r8$"),
    re.compile(r"^mov\s+\$0x0,%r8d$"),
    re.compile(r"^mov\s+\$0x0,%r8$"),
    re.compile(r"^sub\s+%r8d,%r8d$"),
)
R8_WRITE = re.compile(r",%r8d?$")
BRANCHY = re.compile(r"^(j|call|ret|loop)")


class VerifyError(Exception):
    """跑不起来（不是断言不成立）。"""


# --------------------------------------------------------------------- PE

class Section:
    __slots__ = ("name", "va", "vsize", "raw", "rawsize")

    def __init__(self, name, va, vsize, raw, rawsize):
        self.name, self.va, self.vsize, self.raw, self.rawsize = name, va, vsize, raw, rawsize


class Symbol:
    __slots__ = ("name", "va", "typ", "scl")

    def __init__(self, name, va, typ, scl):
        self.name, self.va, self.typ, self.scl = name, va, typ, scl

    @property
    def is_func(self):
        return self.typ in (0x20, 0x21)


class Pe:
    """够用的 PE 解析：节表 / 导出表 / COFF 符号表 / .pdata。RVA 一律换算成 VA 使用。"""

    def __init__(self, path):
        self.path = path
        try:
            with open(path, "rb") as f:
                self.data = f.read()
        except OSError as exc:
            raise VerifyError(f"读不了 {path}: {exc}") from exc
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            raise VerifyError(f"{path} 不是 PE（缺 MZ）")
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if e + 24 > len(d) or d[e:e + 4] != b"PE\0\0":
            raise VerifyError(f"{path}: e_lfanew(0x{e:x}) 处不是 PE 签名")
        self.coff = e + 4
        (nsec,) = struct.unpack_from("<H", d, self.coff + 2)
        (optsz,) = struct.unpack_from("<H", d, self.coff + 16)
        self.opt = self.coff + 20
        (self.magic,) = struct.unpack_from("<H", d, self.opt)
        # ⚠ SizeOfOptionalHeader 在 COFF 头里（e_lfanew+20），不在 optional 头里
        if self.magic == 0x20B:
            (self.image_base,) = struct.unpack_from("<Q", d, self.opt + 24)
            self.dd_off = self.opt + 112
        elif self.magic == 0x10B:
            (self.image_base,) = struct.unpack_from("<I", d, self.opt + 28)
            self.dd_off = self.opt + 96
        else:
            raise VerifyError(f"{path}: 不认识的 optional magic 0x{self.magic:x}")
        self.sections = []
        for i in range(nsec):
            o = self.opt + optsz + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append(Section(name, va, vsize, raw, rawsize))
        self._symoff, self._nsym = struct.unpack_from("<II", d, self.coff + 8)
        self._symbols = None
        self._pdata = None

    # -- 节 / 地址换算

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def va2off(self, va):
        rva = va - self.image_base
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vsize, s.rawsize):
                delta = rva - s.va
                if delta < s.rawsize:          # .bss 这类 rawsize=0 的节在这里被挡掉
                    return s.raw + delta
                return None
        return None

    def read(self, va, n):
        off = self.va2off(va)
        if off is None or off + n > len(self.data):
            return None
        return self.data[off:off + n]

    def u64(self, va):
        b = self.read(va, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def cstring(self, va, maxlen=128):
        off = self.va2off(va)
        if off is None:
            return None
        end = self.data.find(b"\0", off, off + maxlen)
        if end < 0:
            return None
        return self.data[off:end].decode("utf-8", "replace")

    # -- 符号表

    def symbols(self):
        if self._symbols is None:
            self._symbols = self._parse_symbols()
        return self._symbols

    def _str_at(self, strtab, off):
        end = self.data.find(b"\0", strtab + off)
        if end < 0:
            return None
        return self.data[strtab + off:end].decode("utf-8", "replace")

    def _parse_symbols(self):
        """COFF 符号表。长名字走字符串表，偏移相对字符串表起点（含那 4 字节长度字段）。"""
        d = self.data
        symoff, nsym = self._symoff, self._nsym
        if not symoff or not nsym or symoff + nsym * 18 > len(d):
            return []
        strtab = symoff + nsym * 18
        out = []
        i = 0
        while i < nsym:
            off = symoff + i * 18
            if off + 18 > len(d):
                break
            nb, val, sec, typ, scl, naux = struct.unpack_from("<8sIHHBB", d, off)
            name = None
            if nb[:4] == b"\0\0\0\0":
                name = self._str_at(strtab, struct.unpack_from("<I", nb, 4)[0])
            elif nb[:1] == b"/":
                try:
                    name = self._str_at(strtab, int(nb[1:].split(b"\0")[0]))
                except ValueError:
                    name = None
            else:
                name = nb.rstrip(b"\0").decode("utf-8", "replace")
            if name and 0 < sec <= len(self.sections) and scl in (2, 3):
                va = self.image_base + self.sections[sec - 1].va + val
                out.append(Symbol(name, va, typ, scl))
            i += 1 + naux
        return out

    def func_symbols(self):
        return sorted((s for s in self.symbols() if s.is_func), key=lambda s: s.va)

    def find_symbols(self, pattern):
        rx = re.compile(pattern)
        return [s for s in self.func_symbols() if rx.match(s.name)]

    def all_named(self, pattern):
        rx = re.compile(pattern)
        return [s for s in self.symbols() if rx.match(s.name)]

    # -- .pdata 函数边界

    def pdata_bounds(self):
        if self._pdata is None:
            m = {}
            s = self.section(".pdata")
            if s and s.rawsize >= 12:
                for i in range(s.rawsize // 12):
                    begin, end, _unwind = struct.unpack_from("<III", self.data, s.raw + i * 12)
                    if begin and end > begin:
                        m[self.image_base + begin] = self.image_base + end
            self._pdata = m
        return self._pdata

    def func_range(self, va):
        """VA 所在函数的 (start, end, 定位方式)。"""
        best = None
        for begin, end in self.pdata_bounds().items():
            if begin <= va < end and (best is None or begin > best[0]):
                best = (begin, end)
        if best:
            return best[0], best[1], "pdata"
        nxt = [s.va for s in self.func_symbols() if s.va > va]
        if nxt:
            return va, min(nxt), "next-symbol"
        raise VerifyError(f"0x{va:X} 既不在 .pdata 里，后面也没有函数符号，取不到边界")

    def func_name(self, start):
        for s in self.func_symbols():
            if s.va == start:
                return s.name
        return None

    # -- 导出表

    def export_names(self):
        if len(self.data) < self.dd_off + 8:
            return []
        exp_rva, exp_size = struct.unpack_from("<II", self.data, self.dd_off)
        if not exp_rva:
            return []
        base = self.image_base + exp_rva
        head = self.read(base, 40)
        if head is None:
            raise VerifyError(f"{self.path}: 导出目录 0x{base:X} 读不出来")
        (_char, _ts, _maj, _min, _name, _ordbase, nfun, nnam) = struct.unpack_from("<IIHHIIII", head)
        names_rva = struct.unpack_from("<I", head, 32)[0]
        out = []
        for i in range(nnam):
            b = self.read(self.image_base + names_rva + 4 * i, 4)
            if b is None:
                break
            s = self.cstring(self.image_base + struct.unpack("<I", b)[0])
            if s:
                out.append(s)
        return out


# ------------------------------------------------------------- 反汇编（objdump）

class Instr:
    __slots__ = ("addr", "raw", "text")

    def __init__(self, addr, raw, text):
        self.addr, self.raw, self.text = addr, raw, text

    @property
    def size(self):
        return len(self.raw.split())

    def show(self):
        return f"0x{self.addr:X}  {self.raw:<24s} {self.text}"


_OBJDUMP_LINE = re.compile(r"^[0-9a-f]{4,16}:$")


def objdump_instructions(objdump, exe_path, start, end):
    cmd = [objdump, "-d", f"--start-address=0x{start:x}", f"--stop-address=0x{end:x}", exe_path]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise VerifyError(f"objdump 退出码 {proc.returncode}: {proc.stderr.strip()[:200]}")
    out = []
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        head = parts[0].strip()
        if not _OBJDUMP_LINE.match(head):        # objdump 的地址列带冒号，如 "   14021cc60:"
            continue
        try:
            addr = int(head[:-1], 16)
        except ValueError:
            continue
        raw = parts[1].strip()
        if not raw or any(len(t) != 2 for t in raw.split()):
            continue
        text = parts[2].strip() if len(parts) > 2 else ""
        if text == "" and out:                       # 长指令的续行：把字节并回上一条
            out[-1].raw += " " + raw
            continue
        if text == "":
            continue
        out.append(Instr(addr, raw, text))
    return out


def direct_call_target(text):
    m = re.match(r"^(?:rex\.W\s+)?(?:call|jmp)\s+([0-9a-f]+)\s*<", text)
    return int(m.group(1), 16) if m else None


def indirect_call_slot(text):
    m = re.search(r"\*\S*\(%rip\)\s+#\s+([0-9a-f]+)", text)
    return int(m.group(1), 16) if m else None


def jcc_target(text):
    m = re.match(r"^(j\w+)\s+([0-9a-f]+)\s+<", text)
    if not m:
        return None
    return m.group(1), int(m.group(2), 16)


def snippet(instrs, idx, before=2, after=2):
    lo, hi = max(0, idx - before), min(len(instrs), idx + after + 1)
    return [instrs[i].show() for i in range(lo, hi)]


# ------------------------------------------------------- 原始字节模式扫描

def iter_pattern(buf, pat):
    i = 0
    while True:
        i = buf.find(pat, i)
        if i < 0:
            return
        yield i
        i += 1


class TextScan:
    """全 .text 字节扫描：只需要「算出来一个 VA」的判据走这里，不请反汇编器。"""

    def __init__(self, pe):
        s = pe.section(".text")
        if s is None:
            raise VerifyError(f"{pe.path} 没有 .text 节")
        self.pe = pe
        self.base = pe.image_base + s.va
        self.buf = pe.data[s.raw:s.raw + s.rawsize]

    def va(self, off):
        return self.base + off

    def calls_to(self, target):
        """直接 `call rel32` 到 target 的调用点。"""
        out = []
        rel = None
        for off in iter_pattern(self.buf, b"\xe8"):
            site = self.base + off
            rel = struct.unpack_from("<i", self.buf, off + 1)[0]
            if site + 5 + rel == target:
                out.append(site)
        return out

    def indirect_calls_via_slot(self, slot):
        """`call *slot(%rip)` 与 `mov slot(%rip),%rXX; call *%rXX` 两种形态。"""
        direct, via_reg = [], []
        for off in iter_pattern(self.buf, b"\xff\x15"):
            site = self.base + off
            disp = struct.unpack_from("<i", self.buf, off + 2)[0]
            if site + 6 + disp == slot:
                direct.append(site)
        for off in iter_pattern(self.buf, b"\x48\x8b"):
            # 48 8B /r，mod=00 rm=101 → RIP 相对
            modrm = self.buf[off + 2]
            if (modrm & 0xC7) != 0x05:
                continue
            site = self.base + off
            disp = struct.unpack_from("<i", self.buf, off + 3)[0]
            if site + 7 + disp != slot:
                continue
            reg = (modrm >> 3) & 7
            for j in range(off + 7, min(off + 7 + 32, len(self.buf) - 2)):
                b0, b1 = self.buf[j], self.buf[j + 1]
                if b1 == 0xD0 + reg and b0 in (0xFF, 0x41, 0x49, 0x4C):   # call *%rXX
                    via_reg.append(self.base + j)
                    break
        return direct, via_reg

    def lea_rip_before(self, va, max_back=0x40):
        """窗口内所有 `lea disp(%rip),%rXX`，返回 (指令 VA, 目标 VA, 目的寄存器号)。"""
        out = []
        off = va - self.base
        lo = max(0, off - max_back)
        i = lo
        while True:
            j = self.buf.find(b"\x48\x8d", i, off)
            if j < 0:
                break
            modrm = self.buf[j + 2]
            if (modrm & 0xC7) == 0x05:
                site = self.base + j
                disp = struct.unpack_from("<i", self.buf, j + 3)[0]
                out.append((site, site + 7 + disp, (modrm >> 3) & 7))
            i = j + 1
        return out

    def mov_rip_after(self, va, max_fwd=0x40):
        """窗口内所有 `mov %rXX,disp(%rip)`，返回 (指令 VA, 目标 VA, 源寄存器号)。"""
        out = []
        off = va - self.base
        hi = min(len(self.buf) - 7, off + max_fwd)
        i = off
        while True:
            j = self.buf.find(b"\x48\x89", i, hi)
            if j < 0:
                break
            modrm = self.buf[j + 2]
            if (modrm & 0xC7) == 0x05:
                site = self.base + j
                disp = struct.unpack_from("<i", self.buf, j + 3)[0]
                out.append((site, site + 7 + disp, (modrm >> 3) & 7))
            i = j + 1
        return out

    def memcpy_source_plus8_sites(self, memcpy_va):
        """`(add|lea) $0x8,<第2实参寄存器>` 紧挨着的 memcpy 调用点。

        找的是「从某个指针 +8 处开始拷」这个形状（jit 里就是 ptr+8 = 机器码起点）。
        """
        out = []
        # add $0x8,%rdx / lea 0x8(%rdx),%rdx / lea 0x8(%rdx),%rcx / add $0x8,%rcx
        add_pats = (b"\x48\x83\xc2\x08", b"\x48\x8d\x52\x08",
                    b"\x48\x8d\x4a\x08", b"\x48\x83\xc1\x08")
        for off in iter_pattern(self.buf, b"\xe8"):
            site = self.base + off
            rel = struct.unpack_from("<i", self.buf, off + 1)[0]
            if site + 5 + rel != memcpy_va:
                continue
            for pat in add_pats:
                j = off - len(pat)
                if j >= 0 and self.buf[j:j + len(pat)] == pat:
                    out.append((site, j))
                    break
        return out

    def test_disp7_sites(self):
        """`testb $imm8,0x7(reg)` 的所有出现点 + 立即数（Bit62 门控的形状）。"""
        out = []
        for off in iter_pattern(self.buf, b"\xf6"):
            base = off
            j = off + 1
            if j + 1 >= len(self.buf):
                continue
            modrm = self.buf[j]
            if (modrm >> 6) != 1 or ((modrm >> 3) & 7) != 0:
                continue
            k = j + 1
            if (modrm & 7) == 4:      # 有 SIB
                k += 1
            if k + 1 >= len(self.buf):
                continue
            if self.buf[k] != 0x07:
                continue
            out.append((self.base + base, self.buf[k + 1]))
        return out


# --------------------------------------------------------------- 断言结果

class Assertion:
    def __init__(self, key, title, status, locate, evidence, notes=None):
        self.key, self.title, self.status = key, title, status
        self.locate = locate
        self.evidence = list(evidence)
        self.notes = list(notes or [])

    def as_dict(self):
        return {"id": self.key, "title": self.title, "status": self.status,
                "locate": self.locate, "evidence": self.evidence, "notes": self.notes}


def _fail(key, title, locate, evidence, notes=None):
    return Assertion(key, title, FAIL, locate, evidence, notes)


# ------------------------------------------------------------------ 断言 A

def check_a(ctx):
    title = "compile 导出仍存在 + exe 内唯一调用点 + flags 实参为 0"
    ev = []
    dll_exports = ctx["dll"].export_names()
    if "compile" not in dll_exports:
        return _fail("A", title, "compile_real.dll 导出表", [f"导出符号 = {dll_exports}"],
                     ["compile 不在导出表里"])
    ev.append(f"compile_real.dll 导出表 = {sorted(dll_exports)}")

    nimget = ctx["exe"].find_symbols(SYM_NIMGETPROC)
    if len(nimget) != 1:
        return _fail("A", title, "exe 符号 nimGetProcAddr", [f"匹配 {len(nimget)} 个"],
                     ["定位不到唯一的 nimGetProcAddr"])
    sites = ctx["scan"].calls_to(nimget[0].va)
    ev.append(f"nimGetProcAddr @0x{nimget[0].va:X}，调用点 {len(sites)} 处")

    # 在 nimGetProcAddr 调用点的窗口里找 `lea <名字>(%rip),%rdx`，名字是 "compile" 的那个；
    # 紧接着的 `mov %rax,<全局>(%rip)` 就是 compile 的函数指针槽。
    found = None
    for site in sites:
        for lea_va, str_va, reg in ctx["scan"].lea_rip_before(site, 0x40):
            if reg != 2:                      # %rdx = 第二个实参（过程名）
                continue
            if ctx["exe"].cstring(str_va, 64) != "compile":
                continue
            for mov_va, slot_va, sreg in ctx["scan"].mov_rip_after(site, 0x40):
                if sreg == 0:                 # %rax = nimGetProcAddr 的返回值
                    found = (site, lea_va, str_va, mov_va, slot_va)
                    break
            if found:
                break
        if found:
            break
    if found is None:
        return _fail("A", title, "exe 内 nimGetProcAddr + 字符串 \"compile\" + 间接调用扫描",
                     ev, ["找不出 compile 的函数指针槽（nimGetProcAddr 调用点或字符串已变）"])
    site, lea_va, str_va, mov_va, slot = found
    ctx["a_slot"] = slot                      # C 要用它锚定 compile 调用点
    ns, ne, nhow = ctx["exe"].func_range(site)
    nwin = [s for s in objdump_instructions(ctx["objdump"], ctx["exe"].path, ns, ne)
            if lea_va - 4 <= s.addr <= mov_va + 8]
    ev.append(f"nimGetProcAddr(\"compile\") @0x{site:X} → 指针槽 0x{slot:X}（在 {ctx['exe'].func_name(ns) or '?'}，"
              f"边界来自 {nhow}）：")
    ev.extend("    " + s.show() for s in nwin)
    direct, via_reg = ctx["scan"].indirect_calls_via_slot(slot)
    ev.append(f"经该槽的间接调用：直接 FF15 {len(direct)} 处 {[hex(x) for x in direct]}；"
              f"经寄存器 {len(via_reg)} 处 {[hex(x) for x in via_reg]}")
    calls = sorted(direct + via_reg)
    if len(calls) != 1:
        return _fail("A", title, "exe 内间接调用扫描（FF15 / mov(rip)+call *reg）", ev,
                     [f"调用点应为 1 处，实测 {len(calls)} 处"])
    site = calls[0]

    start, end, how = ctx["exe"].func_range(site)
    fname = ctx["exe"].func_name(start)
    ev.append(f"调用点 0x{site:X} 落在 {fname or '?'}（0x{start:X}–0x{end:X}，边界来自 {how}）")

    instrs = objdump_instructions(ctx["objdump"], ctx["exe"].path, start, end)
    idx = next((i for i, ins in enumerate(instrs) if ins.addr == site), None)
    if idx is None:
        return _fail("A", title, "调用点所在函数反汇编", ev, ["objdump 里找不到该调用点"])

    last_write = None
    for i in range(idx):
        if R8_WRITE.search(instrs[i].text) and not instrs[i].text.startswith("cmp"):
            last_write = i
    ev.append(f"调用点反汇编（0x{site:X}）：")
    ev.extend("    " + s for s in snippet(instrs, idx, before=3, after=0))

    if last_write is None:
        return _fail("A", title, "调用点前对 r8d 的写", ev + ["    调用点前没有任何对 r8d 的写"],
                     ["flags 实参没有置零的写"])
    zw = instrs[last_write]
    ev.append(f"    调用点前最后一次写 r8d：{zw.show()}")
    if not any(rx.match(zw.text) for rx in R8_ZERO):
        return _fail("A", title, "调用点前对 r8d 的写", ev, [f"最后一次写 r8d 不是置零：{zw.text}"])
    branchy = [x.text for x in instrs[last_write + 1:idx] if BRANCHY.match(x.text)]
    if branchy:
        return Assertion("A", title, UNKNOWN, "exe 内间接调用扫描 + r8d 数据流", ev,
                         [f"置零与调用点之间有控制流指令 {branchy}，置零不一定支配调用点"])
    ev.append(f"    置零与调用点之间 {idx - last_write - 1} 条指令，无分支 → 置零支配调用点")
    return Assertion("A", title, PASS, "compile_real.dll 导出表 + exe 内 nimGetProcAddr/字符串定位指针槽 + 全 .text 间接调用扫描", ev)


# ------------------------------------------------------------------ 断言 B

def check_b(ctx):
    title = "jit 内存在 memcpy(dest, ptr+8, len) 与尾跳 jmp"
    syms = ctx["exe"].find_symbols(SYM_JIT)
    if len(syms) != 1:
        return _fail("B", title, f"符号正则 {SYM_JIT}", [f"匹配 {len(syms)} 个：{[s.name for s in syms]}"])
    sym = syms[0]
    start, end, how = ctx["exe"].func_range(sym.va)
    ev = [f"{sym.name} @0x{sym.va:X}–0x{end:X}（边界来自 {how}）"]

    mc = ctx["exe"].find_symbols(SYM_MEMCPY)
    if not mc:
        return _fail("B", title, "exe 符号表", ev, ["找不到 memcpy 符号"])
    memcpy_va = mc[0].va
    instrs = objdump_instructions(ctx["objdump"], ctx["exe"].path, start, end)
    ev.append(f"memcpy @0x{memcpy_va:X}")
    idx = next((i for i, ins in enumerate(instrs) if direct_call_target(ins.text) == memcpy_va), None)
    if idx is None:
        return _fail("B", title, "函数体反汇编里的直接 call", ev + ["", "  函数体："] +
                     ["    " + s.show() for s in instrs[:20]], ["函数体内没有对 memcpy 的调用"])
    ev.append("memcpy 调用点：")
    ev.extend("    " + s for s in snippet(instrs, idx, before=3, after=1))

    # 第 2 实参（rdx）在调用前 +8 —— 即源指针 = out_ptr + 8（跳过 8 字节头）。
    # 只认「加法/取址」两种形态：`mov 0x8(%rbx),%rdx`（从 Result 取 out_ptr）不算，
    # 它取的是字段、不是 +8 偏移，拿它当判据会漏判掉「跳过点被改成 +0」。
    PLUS8 = re.compile(r"^(add\s+\$0x8,%rdx|lea\s+0x8\(%r\w+\),%rdx)$")
    plus8 = [i for i in range(max(0, idx - 4), idx) if PLUS8.match(instrs[i].text)]
    if not plus8:
        return _fail("B", title, "memcpy 调用点前 4 条指令里的 +8 调整", ev,
                     ["memcpy 源的 +8 调整没找到（产物头 8 字节的跳过点变了）"])
    ev.append(f"    源指针 +8 的调整：{instrs[plus8[-1]].show()}")

    tail = [i for i in range(idx + 1, len(instrs)) if re.search(r"jmp\s+\*%r", instrs[i].text)]
    if not tail:
        return _fail("B", title, "memcpy 之后的间接尾跳", ev, ["没找到 `jmp *%reg`（尾跳进 entry）"])
    ev.append("尾跳：")
    ev.extend("    " + s for s in snippet(instrs, tail[-1], before=4, after=0))
    if not any(re.search(r"lea\s+\(%r\w+,%r\w+,1\),%r\w+$", x.text) for x in instrs[idx:tail[-1]]):
        return _fail("B", title, "尾跳前的基址+entry 计算", ev, ["没找到 `lea (%a,%b,1),%c`"])
    ev.append("    尾跳前有 `lea (%dest,%entry,1),%rax` 的基址+entry 计算")
    cjit = ctx["exe"].find_symbols(SYM_CJIT)
    if cjit and any(direct_call_target(x.text) == cjit[0].va for x in instrs[idx:tail[-1]]):
        ev.append(f"    中间还有一次 `call c_jit` @0x{cjit[0].va:X}（每次进 entry 前的钩子调用点仍在）")
    return Assertion("B", title, PASS,
                     f"符号正则 {SYM_JIT} 定位函数 + .pdata 取边界 + 函数体内 call/memcpy 模式", ev)


# ------------------------------------------------------------------ 断言 C

def check_c(ctx):
    title = "handle_request_compile_and_run 内存在对 out[3] 的 test/jcc 失败分支"
    syms = ctx["exe"].find_symbols(SYM_HANDLE)
    if len(syms) != 1:
        return _fail("C", title, f"符号正则 {SYM_HANDLE}", [f"匹配 {len(syms)} 个：{[s.name for s in syms]}"])
    sym = syms[0]
    start, end, how = ctx["exe"].func_range(sym.va)
    instrs = objdump_instructions(ctx["objdump"], ctx["exe"].path, start, end)
    ev = [f"{sym.name} @0x{sym.va:X}–0x{end:X}（边界来自 {how}）"]

    compile_slot = ctx["a_slot"]
    if compile_slot is None:
        return _fail("C", title, "compile 调用点", ev,
                     ["定位不到 compile 的指针槽（看 A 的证据），无法锚定失败判定在调用之后"])
    call_idx = next((i for i, ins in enumerate(instrs) if indirect_call_slot(ins.text) == compile_slot
                     and ins.text.startswith("call")), None)
    if call_idx is None:
        return _fail("C", title, "函数体内对 compile 槽的间接调用", ev,
                     [f"函数体内没有 `call *0x...(%rip)` 指向 0x{compile_slot:X}"])
    ev.append(f"compile 调用点 {instrs[call_idx].show()}")

    send = ctx["exe"].find_symbols(SYM_SENDCMD)
    thr = ctx["exe"].find_symbols(SYM_CREATETHREAD)
    if not send or not thr:
        return _fail("C", title, "符号表里的 send_command / createThread", ev,
                     [f"send_command {len(send)} 个 / createThread {len(thr)} 个"])
    send_idx = next((i for i, ins in enumerate(instrs) if direct_call_target(ins.text) == send[0].va), None)
    thr_idx = next((i for i, ins in enumerate(instrs) if direct_call_target(ins.text) == thr[0].va), None)
    if send_idx is None or thr_idx is None:
        return _fail("C", title, "成功路径的 send_command / createThread 调用", ev,
                     [f"send_command 调用点 {send_idx} / createThread 调用点 {thr_idx}"])

    pair = None
    for i in range(call_idx, send_idx):
        m = re.match(r"^test\s+(%r\w+),%r\w+$", instrs[i].text)
        if not m:
            continue
        jt = jcc_target(instrs[i + 1].text) if i + 1 < len(instrs) else None
        if jt and jt[0] in ("jne", "jz", "je", "jnz"):
            pair = (i, i + 1, jt)
            break
    if pair is None:
        return _fail("C", title, "compile 调用点与 send_command 之间的 test/jcc", ev,
                     ["compile 调用之后、send_command 之前没有 test+jcc 对"])
    ti, _, (mnem, target) = pair
    ev.append("失败判定：")
    ev.extend("    " + s for s in snippet(instrs, ti, before=3, after=1))
    if not (target > instrs[thr_idx].addr and target > instrs[send_idx].addr):
        return _fail("C", title, "失败分支的目标", ev,
                     [f"{mnem} 目标 0x{target:X} 没有跳过 send_command(0x{instrs[send_idx].addr:X})"
                      f" 与 createThread(0x{instrs[thr_idx].addr:X})"])
    ev.append(f"    {mnem} 目标 0x{target:X} > send_command 0x{instrs[send_idx].addr:X} "
              f"与 createThread 0x{instrs[thr_idx].addr:X} → 失败分支确实跳过它们")

    # 被 test 的寄存器从栈上取（Result 落在调用者的栈上）
    mreg = re.match(r"^test\s+(%r\w+),", instrs[ti].text)
    if mreg is None:                       # pair 是从同一段指令文本里挑出来的，正常到不了这里
        return _fail("C", title, "失败判定的指令文本", ev, [f"解析不了 test：{instrs[ti].text}"])
    reg = mreg.group(1)
    prov = None
    for i in range(ti - 1, max(-1, ti - 4), -1):
        m = re.match(r"^mov\s+(0x[0-9a-f]+)\(%rsp\),(%r\w+)$", instrs[i].text)
        if m and m.group(2).replace("d", "").replace("e", "") == reg.replace("d", "").replace("e", ""):
            prov = (i, int(m.group(1), 16))
            break
    if prov is None:
        return _fail("C", title, "被 test 的寄存器的来源", ev,
                     [f"{reg} 不是从栈槽直接 load 的（判定值来源不明）"])
    ev.append(f"    判定值来源：{instrs[prov[0]].show()}（栈槽 0x{prov[1]:x}）")

    # 槽级污点：把「栈槽 ← 栈槽 / xmm ← 栈槽」的搬运跟到底，看这个槽是不是 Result 的 out[3]。
    # 为什么必须这样做：只有证明被 test 的值来自 out[3]，这条断言才等价于「err_len != 0 即失败」；
    # 只看「有个 test/jcc 跳过成功路径」的话，任何别的判定都会被误认成它。
    base = _result_base(instrs, call_idx)
    base_txt = f"0x{base:x}(%rsp)" if base is not None else "没溯出来"
    ev.append(f"    Result 基址：{base_txt}")
    taint = _slot_taint(instrs, prov[1])
    ev.append(f"    该栈槽的污染集（回溯到入口处的原始槽，\"?\" = 跟不动）："
              f"{sorted(hex(x) if isinstance(x, int) else x for x in taint)}")
    if base is not None:
        want = base + 0x18
        if want in taint:
            ev.append(f"    out[3] 槽 = 0x{base:x}+0x18 = 0x{want:x} 在污染集里 → 判的就是 out[3]")
            return Assertion("C", title, PASS, "符号正则定位函数 + .pdata 边界 + compile 调用点锚定 + 槽级污点回溯", ev)
        why = (f"污染集里没有 out[3] 槽 0x{want:x}" if taint else "污染集为空")
        return Assertion("C", title, UNKNOWN, "符号正则定位函数 + .pdata 边界 + compile 调用点锚定 + 槽级污点回溯", ev,
                         [why + "——要么更新后改成了判别的字段，要么这个污点模型过时了，人工核对"])
    return Assertion("C", title, UNKNOWN, "符号正则定位函数 + .pdata 边界 + compile 调用点锚定", ev,
                     ["没溯出 Result 基址，无法确认被 test 的槽就是 out[3]"])


def _result_base(instrs, call_idx):
    """compile 的第 1 实参 rcx 是指向 Result 的指针；顺 `mov %rX,%rcx` 找它的 `lea d(%rsp),%rX`。"""
    for i in range(call_idx - 1, max(-1, call_idx - 8), -1):
        m = re.match(r"^mov\s+(%r\w+),%rcx$", instrs[i].text)
        if not m:
            continue
        src = m.group(1)
        for j in range(i - 1, max(-1, i - 8), -1):
            m2 = re.match(r"^lea\s+(0x[0-9a-f]+)\(%rsp\),(%r\w+)$", instrs[j].text)
            if m2 and m2.group(2) == src:
                return int(m2.group(1), 16)
        return None
    return None


def _slot_taint(instrs, slot):
    """栈槽 → 「函数入口处的原始槽」集合，单遍前向传播。

    只跟三种搬运：xmm ← 栈槽（16 字节）、栈槽 ← xmm（16 字节）、标量寄存器 ← / → 栈槽。
    为什么非要这么跟：只有证明被 test 的值源自 Result 的 out[3] 槽，C 才等价于「err_len != 0 即失败」；
    只看「有个 test/jcc 跳过成功路径」的话，任何别的判定都会被误认成它。
    跟不动的来源记成 "?" —— 调用方据此报 UNKNOWN 而不是断言不成立。
    """
    UNK = "?"
    slots = {}
    xmm = {}
    regs = {}

    def read_slot(off):
        return set(slots[off]) if off in slots else {off}

    def write_slot(off, srcs):
        slots.setdefault(off, set()).update(srcs)

    for ins in instrs:
        t = ins.text
        # xmm ← 栈槽（16 字节）
        m = re.match(r"^mov(?:dqu|aps|ups|dqa)?\s+(0x[0-9a-f]+)\(%rsp\),(%xmm\d+)$", t)
        if m:
            a = int(m.group(1), 16)
            xmm[m.group(2)] = (read_slot(a), read_slot(a + 8))
            continue
        # 栈槽 ← xmm（16 字节）
        m = re.match(r"^mov(?:dqu|aps|ups|dqa)?\s+(%xmm\d+),(0x[0-9a-f]+)\(%rsp\)$", t)
        if m:
            pair = xmm.get(m.group(1), ({UNK}, {UNK}))
            a = int(m.group(2), 16)
            write_slot(a, pair[0])
            write_slot(a + 8, pair[1])
            continue
        # 寄存器 ← 栈槽（标量/movzx 类）
        m = re.match(r"^mov(?:zx|sx)?\s+(0x[0-9a-f]+)\(%rsp\),(%r\w+)$", t)
        if m:
            regs[m.group(2)] = read_slot(int(m.group(1), 16))
            continue
        # 栈槽 ← 寄存器
        m = re.match(r"^mov\s+(%r\w+),(0x[0-9a-f]+)\(%rsp\)$", t)
        if m:
            write_slot(int(m.group(2), 16), regs.get(m.group(1), {UNK}))
            continue
        # 其它碰 rsp 的写法（lea 取地址、add 调栈指针……）：值语义跟不动，一律记未知
        if "%rsp" in t:
            m = re.match(r"^\S+\s+.*\(%rsp\)[^,]*,\s*(%r\w+)$", t)
            if m:
                regs[m.group(1)] = {UNK}
    return slots.get(slot, {slot})


# ------------------------------------------------------------------ 断言 D

def check_d(ctx):
    title = "eqdestroy 内存在 testb $0x40,0x7(p) + rawDealloc（bit62 门控）"
    cands = ctx["exe"].find_symbols(SYM_EQDESTROY_TIMES)
    if not cands:
        return _fail("D", title, f"符号正则 {SYM_EQDESTROY_TIMES}", ["一个都没匹配到"],
                     ["更新后 eqdestroy 的模块路径或命名变了"])
    raw = ctx["exe"].find_symbols(SYM_RAWDALLOC)
    if not raw:
        return _fail("D", title, "符号正则 " + SYM_RAWDALLOC, ["找不到 rawDealloc__system"])
    raw_va = raw[0].va
    ev = [f"候选 {len(cands)} 个：" + ", ".join(f"{s.name}@0x{s.va:X}" for s in cands),
          f"rawDealloc__system @0x{raw_va:X}"]

    # 哪个候选是「compile 结果串」的销毁者？在 handle_request_compile_and_run 里被调用的那个优先。
    handle = ctx["exe"].find_symbols(SYM_HANDLE)
    called_in_handle = set()
    if handle:
        hs, he, _ = ctx["exe"].func_range(handle[0].va)
        for ins in objdump_instructions(ctx["objdump"], ctx["exe"].path, hs, he):
            t = direct_call_target(ins.text)
            if t:
                called_in_handle.add(t)
    ordered = [c for c in cands if c.va in called_in_handle] + [c for c in cands if c.va not in called_in_handle]

    hits = []
    for sym in ordered:
        start, end, how = ctx["exe"].func_range(sym.va)
        instrs = objdump_instructions(ctx["objdump"], ctx["exe"].path, start, end)
        gi = next((i for i, ins in enumerate(instrs)
                   if re.match(r"^testb\s+\$0x40,0x7\(%r\w+\)$", ins.text)), None)
        di = next((i for i, ins in enumerate(instrs) if direct_call_target(ins.text) == raw_va), None)
        tag = "（handle_request_compile_and_run 内被调用）" if sym.va in called_in_handle else ""
        if gi is not None and di is not None:
            hits.append((sym, instrs, gi, di, how, tag))
        else:
            ev.append(f"  {sym.name}@0x{sym.va:X}{tag}：testb {'有' if gi is not None else '无'} / "
                      f"rawDealloc {'有' if di is not None else '无'} → 不算命中")
    if not hits:
        return _fail("D", title, "候选函数逐个反汇编 + 指令模式匹配", ev,
                     ["没有任何 eqdestroy 候选同时有 testb $0x40,0x7 + rawDealloc"])
    sym, instrs, gi, di, how, tag = hits[0]
    ev.append(f"命中 {sym.name}@0x{sym.va:X}–（边界来自 {how}）{tag}")
    ev.append("bit62 门控：")
    ev.extend("    " + s for s in snippet(instrs, gi, before=2, after=2))
    ev.append("释放：")
    ev.extend("    " + s for s in snippet(instrs, di, before=1, after=0))
    # 门控的形状：「bit62 清位→释放，置位→不释放」应当写成一个前向跳转：
    #   testb $0x40,0x7(p)
    #   je <释放路径>          <- 清位（ZF=1）才跳
    #   ... ret                <- 置位就落空到 ret
    #   <释放路径>: ... jmp rawDealloc
    # 三个条件都得满足，否则就是形状/方向变了。
    nxt = instrs[gi + 1]
    jt = jcc_target(nxt.text)
    note = ""
    if jt is None or jt[0] != "je":
        status = UNKNOWN
        note = f"testb 之后是 `{nxt.text}`，不是 `je`，门控形状与预期不同，人工核对"
    else:
        fall = [x for x in instrs if nxt.addr < x.addr < jt[1]]
        onto = [x for x in instrs if jt[1] <= x.addr < instrs[di].addr]
        if not any(x.text.startswith("ret") for x in fall):
            return _fail("D", title, "bit62 门控的形状", ev,
                         [f"je 的落空路径（0x{nxt.addr:X}–0x{jt[1]:X}）里没有 ret —— "
                          "bit62 置位时不会停下来，门控已被绕过"])
        if any(x.text.startswith("ret") for x in onto):
            return _fail("D", title, "bit62 门控的方向", ev,
                         [f"je 目标 0x{jt[1]:X} 到 rawDealloc（0x{instrs[di].addr:X}）之间有 ret —— "
                          "目标不是释放路径，门控方向可能反了"])
        status = PASS
        ev.append(f"    testb 之后 `{nxt.text}`：bit62 清位（ZF=1）才跳到释放路径 0x{jt[1]:X}；"
                  f"置位则落到 ret 不 free → 走到 rawDealloc（0x{instrs[di].addr:X}）的条件是 bit62 未置位")
    return Assertion("D", title, status,
                     f"符号正则 {SYM_EQDESTROY_TIMES} 定位候选 + 逐个函数体模式匹配", ev,
                     [note] if note else None)


# ------------------------------------------------------------------ 断言 E

def check_e(ctx):
    title = "产物头 8 字节的值仍不被读（除 bit62）"
    ev = []
    mc = ctx["exe"].find_symbols(SYM_MEMCPY)
    if not mc:
        return _fail("E", title, "符号 memcpy", ["找不到 memcpy 符号"])
    plus8 = ctx["scan"].memcpy_source_plus8_sites(mc[0].va)
    jit = ctx["exe"].find_symbols(SYM_JIT)
    jit_range = None
    if jit:
        s, e, _ = ctx["exe"].func_range(jit[0].va)
        jit_range = (s, e)
    in_jit = [a for a, _ in plus8 if jit_range and jit_range[0] <= a < jit_range[1]]
    others = [a for a, _ in plus8 if a not in in_jit]
    ev.append(f"「源指针 +8 的 memcpy」调用点：{len(plus8)} 处，jit 内 {len(in_jit)} 处"
              f"（{[hex(a) for a in in_jit]}）；其余 {len(others)} 处散布全 exe（例 "
              + ", ".join(hex(a) for a in others[:6]) + "）。")
    ev.append(f"    这 {len(others)} 处与 jit 那处的**指令形状完全相同**，而「哪个指针指向产物」是运行时性质，"
              "静态分不出来 —— 这就是 E 判不了的根本原因。")

    tests = ctx["scan"].test_disp7_sites()
    by_imm = {}
    for va, imm in tests:
        by_imm.setdefault(imm, []).append(va)
    ev.append(f"「读头第 7 字节」的 testb（bit62 门控形状）出现点 {len(tests)} 处，按立即数分组：")
    for imm, vas in sorted(by_imm.items(), key=lambda kv: -len(kv[1])):
        mark = "" if imm == 0x40 else "  <-- 立即数不是 0x40"
        ev.append(f"    $0x{imm:02x}: {len(vas)} 处，例 0x{vas[0]:X}{mark}")

    odd = sorted(imm for imm in by_imm if imm != 0x40)
    notes = []
    if odd:
        notes.append(f"证伪项触发：存在立即数不是 0x40 的「读头第 7 字节」（{['0x' + format(i, '02x') for i in odd]}）"
                     "—— 头 word 的别的位可能也被读。")
    notes += ["E 是**否定性命题**（全程序没有别处读头 word），静态证明需要全程序数据流，本脚本给不出，"
              "所以恒报 UNKNOWN —— 不是 PASS。",
              "这就是唯一的证伪项（全 exe 里读头第 7 字节的 testb 立即数是否都是 0x40），它没触发；"
              "但它挡不住「用 mov/movzx 把整字读走」那类读者，所以不能据此判 PASS。",
              "要真判定只有动态：放一份头字垃圾（只置 bit62）的产物进游戏跑一轮，看是否崩/走错分支；"
              "那要部署自研产物，不在本脚本范围（见 docs/notes 里本脚本的记录）。"]
    return Assertion("E", title, UNKNOWN,
                     "全 .text 字节模式扫描（memcpy 源 +8 形态 / testb $imm,0x7(reg) 形态）", ev, notes)


# --------------------------------------------------------------------- 主流程

def build_context(args):
    game_dir = args.game_dir
    exe_path = os.path.join(game_dir, EXE_NAME)
    dll_path = os.path.join(game_dir, REAL_DLL_NAME)
    for p in (exe_path, dll_path):
        if not os.path.isfile(p):
            raise VerifyError(f"找不到 {p}（--game-dir 给对了吗）")
    objdump = shutil.which("objdump")
    if not objdump:
        raise VerifyError("PATH 里没有 objdump（w64devkit 的 binutils），函数体反汇编做不了")
    exe = Pe(exe_path)
    if not exe.func_symbols():
        raise VerifyError(f"{exe_path} 没有 COFF 符号表 —— 本脚本按符号名定位，符号被 strip 就只能改用模式定位")
    ctx = {"game_dir": game_dir, "exe_path": exe_path, "dll_path": dll_path,
           "exe": exe, "dll": Pe(dll_path), "objdump": objdump,
           "scan": TextScan(exe), "a_slot": None}
    return ctx


def version_info(ctx, acf_path):
    st = os.stat(ctx["exe_path"])
    info = {"exe": os.path.basename(ctx["exe_path"]), "exe_bytes": st.st_size,
            "exe_mtime": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st.st_mtime)),
            "func_symbols": len(ctx["exe"].func_symbols()),
            "pdata_entries": len(ctx["exe"].pdata_bounds()),
            "objdump": ctx["objdump"],
            "buildid": None, "last_updated": None, "acf": acf_path}
    if os.path.isfile(acf_path):
        with open(acf_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        m = re.search(r'"buildid"\s+"(\d+)"', text)
        if m:
            info["buildid"] = m.group(1)
        m = re.search(r'"LastUpdated"\s+"(\d+)"', text)
        if m:
            info["last_updated"] = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(m.group(1))))
    return info


def run_checks(ctx):
    results = []
    results.append(check_a(ctx))
    results.append(check_b(ctx))
    results.append(check_c(ctx))
    results.append(check_d(ctx))
    results.append(check_e(ctx))
    return results


def print_report(ctx, version, results):
    line = "=" * 78
    print(line)
    print("Steam 更新后契约重验（主设计 §10）")
    print(line)
    print(f"游戏目录   : {ctx['game_dir']}")
    print(f"exe        : {version['exe']}  {version['exe_bytes']} B  mtime {version['exe_mtime']}")
    print(f"参考编译器 : {os.path.basename(ctx['dll_path'])}  {os.path.getsize(ctx['dll_path'])} B")
    print(f"buildid    : {version['buildid'] or '未读到'}   (appmanifest: {version['acf']}"
          + (f", LastUpdated {version['last_updated']}" if version['last_updated'] else "") + ")")
    print(f"符号表     : {version['func_symbols']} 个函数符号 / {version['pdata_entries']} 条 .pdata 条目"
          f"（本脚本自己解析 PE，不用 work/symbols 快照）")
    print(f"objdump    : {version['objdump']}")
    print()
    for r in results:
        print("-" * 78)
        print(f"[{r.key}] {r.title}")
        print(f"状态 : {r.status}")
        print(f"定位 : {r.locate}")
        print("证据 :")
        for e in r.evidence:
            print("   " + e)
        for n in r.notes:
            print("说明 : " + n)
    print("-" * 78)
    n_pass = sum(1 for r in results if r.status == PASS)
    n_fail = sum(1 for r in results if r.status == FAIL)
    n_unk = sum(1 for r in results if r.status == UNKNOWN)
    print(f"摘要 : PASS={n_pass}  FAIL={n_fail}  UNKNOWN={n_unk}   "
          + "  ".join(f"{r.key}={r.status}" for r in results))
    if n_unk:
        why = ", ".join(f"{r.key}（{r.notes[0] if r.notes else '原因见上'}）"
                       for r in results if r.status == UNKNOWN)
        print(f"!! {n_unk} 条 UNKNOWN —— **不算通过**：{why}")
    if n_fail:
        print("!! 有 FAIL：上面标 FAIL 的断言对应的结论已经不能照用，先别依赖它们。")
    return n_fail


def main(argv=None):
    ap = argparse.ArgumentParser(description="Steam 更新后重验主设计 §10 的断言（按符号名定位）")
    ap.add_argument("--game-dir", default=None,
                    help="游戏目录（默认 TC_GAME_DIR，否则从 Steam 库里找 Turing Complete）")
    ap.add_argument("--acf", default=None,
                    help=f"appmanifest 路径（默认从游戏目录推 <game-dir>/../../{ACF_NAME}）")
    ap.add_argument("--json", action="store_true", help="末尾额外打印一份 JSON 总结")
    ap.add_argument("--only", default=None, help="只跑某几条断言，逗号分隔，如 A,C")
    args = ap.parse_args(argv)

    try:
        args.game_dir = args.game_dir or str(TC.game_dir())
    except RuntimeError as exc:
        print(f"跑不起来：{exc}", file=sys.stderr)
        return 2

    try:
        ctx = build_context(args)
    except VerifyError as exc:
        print(f"跑不起来：{exc}", file=sys.stderr)
        return 2

    acf = args.acf or os.path.normpath(os.path.join(args.game_dir, "..", "..", ACF_NAME))
    version = version_info(ctx, acf)
    try:
        results = run_checks(ctx)
    except VerifyError as exc:
        print(f"跑不起来：{exc}", file=sys.stderr)
        return 2
    if args.only:
        want = {x.strip().upper() for x in args.only.split(",")}
        results = [r for r in results if r.key in want]

    n_fail = print_report(ctx, version, results)
    if args.json:
        print()
        print(json.dumps({
            "game_dir": ctx["game_dir"],
            "version": version,
            "assertions": [r.as_dict() for r in results],
            "summary": {r.key: r.status for r in results},
            "counts": {"pass": sum(1 for r in results if r.status == PASS),
                       "fail": sum(1 for r in results if r.status == FAIL),
                       "unknown": sum(1 for r in results if r.status == UNKNOWN)},
            "exit_code": 1 if n_fail else 0,
        }, ensure_ascii=False, indent=2))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
