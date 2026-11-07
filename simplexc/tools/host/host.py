#!python3
"""离线差分宿主（M2）：按游戏的方式执行产物，逐 tick 出快照。

游戏侧的执行方式（`docs/facts.md`）：分配可执行内存 → `memcpy(dest, blob+8, out_len)` → `jmp dest+entry`。
本宿主做同一件事，只是「游戏内存」换成自己按产物里写死的地址摆出来的假数据段。

⚠ 与 spec §6 的一处偏离（实测逼出来的，见 `docs/notes/2026-10-17-m2-host-design.md`）：
   参考产物会**直接调 `compile_real.dll` 里的运行时函数**并读写它的 `.bss`，所以宿主必须
   **加载真 DLL**、再把产物里 baked 基址重定位到实际加载基址 —— 光把地址摆出来不够。

用法：
    python simplexc/tools/host/host.py --src <si> --bin <bin> --ticks 20 --out snap.jsonl
    python simplexc/tools/host/host.py --src <si> --bin <bin> --seed-reset-data seq   # 先给 `#RESET_DATA_*` 灌图案
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import ctypes
import hashlib
import importlib.util
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
SIMPLEXC = os.path.dirname(TOOLS)


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


abs_scan = _load(os.path.join(TOOLS, "abs_scan.py"), "abs_scan")
simspec = _load(os.path.join(HERE, "simspec.py"), "simspec")

GAME_DIR = abs_scan.GAME
DLL_PATH = abs_scan.DLL_REAL
CORPUS_MANIFEST = os.path.join(SIMPLEXC, "corpus", "manifest.tsv")

PAGE = 0x10000
REGION = 0x100000                 # 每个地址给 1 MB（含前后 slack）：够放数组载荷与 UI 文本
HASH_CHUNK = 1 << 20              # 段哈希的读块大小：rv32im 的 6 段共 278.5 MB，一次 string_at 到堆上白拷一份
BAND = 1 << 26                    # 「像游戏地址」的邻域（±64 MB）—— 落在里面的 movabs 必须映射到

# `--seed-reset-data` 的图案表（合法值只在这一处定义，见 docs/pitfalls.md）：
#   seq      byte[i] = i & 0xFF
#   xorshift xorshift64（13/7/17）固定种子的字节流，每步取 8 字节小端
# 图案是**块内偏移**的函数：每个块都从同一个种子重新起（与块的顺序、个数无关），
# 所以加/减一个块不会改变别的块的字节。
SEED_PATTERNS = ("seq", "xorshift")
XORSHIFT_SEED = 0x2545F4914F6CDD1D
U64_MASK = (1 << 64) - 1
MEM_COMMIT, MEM_RESERVE, MEM_RELEASE = 0x1000, 0x2000, 0x8000
PAGE_NOACCESS, PAGE_RW, PAGE_RWX = 0x01, 0x04, 0x40
ARENA_BASE = 0x100000000          # 确定性 bump 分配器的固定首址（4 GB，x64 用户空间内）
ARENA_SIZE = 0x10000000           # 256 MB
RSV_BASE = 0x400000000000         # 只 RESERVE 的保留区首址（64 TiB：远高于游戏段 ≈2.4 TB 与 arena 4 GB，向上按块长）
RSV_BLOCK = 0x4000000000          # 保留区一块 256 GB（够装 27 个参考产物的 10 GB arena，用时才留）
ARR_LEN, ARR_CAP_LOG2 = 4096, 13  # 宿主给数组的 len/cap（与游戏给的值可能不同，两遍一致即可）
EXIT_MAPPED_MOVABS = 2            # 地址表外已映射 movabs：与 tick 失败同码（非 0 即判据失败）
EXIT_STUB_CONTRACT = 3            # sidecar 与产物对不上 / extern 符号没有宿主桩

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.VirtualAlloc.restype = ctypes.c_void_p
k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
k32.VirtualFree.restype = ctypes.c_int
k32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]
k32.VirtualProtect.restype = ctypes.c_int
k32.VirtualProtect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong,
                               ctypes.POINTER(ctypes.c_ulong)]
k32.LoadLibraryExW.restype = ctypes.c_void_p
k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_ulong]
k32.AddDllDirectory.restype = ctypes.c_void_p
k32.AddDllDirectory.argtypes = [ctypes.c_wchar_p]
k32.CreateThread.restype = ctypes.c_void_p
k32.CreateThread.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
                             ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
k32.WaitForSingleObject.restype = ctypes.c_ulong
k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
k32.TerminateThread.restype = ctypes.c_int
k32.TerminateThread.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
k32.ExitThread.restype = None
k32.ExitThread.argtypes = [ctypes.c_ulong]
k32.GetCurrentProcess.restype = ctypes.c_void_p
k32.TerminateProcess.restype = ctypes.c_int
k32.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]


# ---- VEH：把崩溃位置记下来（宿主崩了也得留下线索）-----------------------------
class _EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [("ExceptionCode", ctypes.c_ulong), ("ExceptionFlags", ctypes.c_ulong),
                ("ExceptionRecord", ctypes.c_void_p), ("ExceptionAddress", ctypes.c_void_p),
                ("NumberParameters", ctypes.c_ulong), ("_pad", ctypes.c_ulong),
                ("ExceptionInformation", ctypes.c_uint64 * 15)]


class _EXCEPTION_POINTERS(ctypes.Structure):
    _fields_ = [("ExceptionRecord", ctypes.POINTER(_EXCEPTION_RECORD)),
                ("ContextRecord", ctypes.c_void_p)]


_VEH = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.POINTER(_EXCEPTION_POINTERS))

# ⚠ 必须留引用：ctypes 回调注册给系统后，Python 对象被回收会让系统跳到已释放内存
# （表现为没有任何信息的静默崩溃，极难查）。见 docs/pitfalls.md。
_VEH_KEEPALIVE = []


def install_veh(code_alloc_holder, dll_base_getter):
    """第一现场：异常码 + 地址 + 它落在产物还是 DLL 里，然后让进程干净退出。"""
    def handler(ptrs):
        try:
            rec = ptrs.contents.ExceptionRecord.contents
            addr = rec.ExceptionAddress or 0
            where = "?"
            code_alloc = code_alloc_holder()
            dll_base = dll_base_getter()
            if code_alloc and code_alloc <= addr < code_alloc + 0x100000:
                where = f"产物 +0x{addr - code_alloc:X}"
            elif dll_base and dll_base <= addr < dll_base + 0x200000:
                where = f"compile_real.dll +0x{addr - dll_base:X}"
            print(f"[host] 异常 code=0x{rec.ExceptionCode:X} addr=0x{addr:X}（{where}）"
                  f" params={[rec.ExceptionInformation[i] for i in range(min(rec.NumberParameters, 4))]}",
                  file=sys.stderr, flush=True)
        finally:
            k32.TerminateProcess(k32.GetCurrentProcess(), 3)
        return -1
    cb = _VEH(handler)
    _VEH_KEEPALIVE.append(cb)
    k32.AddVectoredExceptionHandler.restype = ctypes.c_void_p
    k32.AddVectoredExceptionHandler.argtypes = [ctypes.c_ulong, _VEH]
    handle = k32.AddVectoredExceptionHandler(1, cb)
    if not handle:
        raise SystemExit(f"AddVectoredExceptionHandler 失败 err={ctypes.get_last_error()}")
    print(f"[host] VEH 已装 handle=0x{handle:X}", file=sys.stderr, flush=True)
    return handle

# Win64 前 6 个整型参数；声明满 6 个即可覆盖 DLL 导入的常见形状（多出的读栈，ABI 位置一样）
SIG = ctypes.CFUNCTYPE(ctypes.c_uint64, *([ctypes.c_uint64] * 6))


def mem_alloc(addr, size, protect=PAGE_RW):
    p = k32.VirtualAlloc(ctypes.c_void_p(addr), size, MEM_COMMIT | MEM_RESERVE, protect)
    if not p:
        raise SystemExit(f"VirtualAlloc({hex(addr)}, {hex(size)}) 失败 err={ctypes.get_last_error()}")
    return p


def rd_u64(addr):
    return ctypes.c_uint64.from_address(addr).value


def wr_u64(addr, val):
    ctypes.c_uint64.from_address(addr).value = val & 0xFFFFFFFFFFFFFFFF


def pattern_bytes(pattern, size):
    """`size` 字节的确定性图案（两边传同一个 `--seed-reset-data` 就得到同一串字节）。

    大小不是 8 的倍数时，xorshift 的尾部用下一步的低位字节补 —— 语料里实际都是 8 的倍数。
    """
    if pattern == "seq":
        base = bytes(range(256))
        reps, tail = divmod(size, 256)
        return base * reps + base[:tail]
    if pattern == "xorshift":
        out = bytearray(size)
        mv = memoryview(out)
        x = XORSHIFT_SEED
        body = size - size % 8
        for i in range(0, body, 8):
            x ^= (x << 13) & U64_MASK
            x ^= x >> 7
            x ^= (x << 17) & U64_MASK
            mv[i:i + 8] = x.to_bytes(8, "little")
        if body < size:
            x ^= (x << 13) & U64_MASK
            x ^= x >> 7
            x ^= (x << 17) & U64_MASK
            mv[body:size] = x.to_bytes(8, "little")[:size - body]
        return bytes(out)
    raise SystemExit(f"未知图案 {pattern!r}（只有 {SEED_PATTERNS}）")


def merge_spans(ranges):
    """ranges: [(lo, hi)] 绝对区间（未页对齐）→ [(lo, size)]，页对齐并合并重叠。

    `hi <= lo` = 「只知道有这么个地址、不知道多大」→ 给默认 REGION。
    有尺寸的区间来自源码 `memory_copy`/`memory_clear` 的实参（见 `simspec._collect_ranges`）：
    参考产物的 `reset_sim` **每 tick** 整块拷一遍内存镜像（RV32IM 是 128 MB 的 `rep movs`），
    映射小一点就在拷贝末尾越界 AV（实测：按 128 MB 给、实际拷 128 MB + 4096 → 崩）。
    """
    spans = []
    for lo, hi in ranges:
        lo2 = lo & ~(PAGE - 1)
        end = max(hi, lo + (REGION if hi <= lo else 0))
        hi2 = (end + PAGE - 1) & ~(PAGE - 1)
        spans.append((lo2, max(hi2, lo2 + PAGE)))
    spans.sort()
    out = []
    for lo, hi in spans:
        if out and lo <= out[-1][1]:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return [(lo, hi - lo) for lo, hi in out]


def movabs_sites(buf):
    """`movabs r64,imm64` 立即数的 (偏移, 值)。"""
    for i in range(2, len(buf) - 8):
        if 0x48 <= buf[i - 2] <= 0x4F and 0xB8 <= buf[i - 1] <= 0xBF:
            yield i, struct.unpack_from("<Q", buf, i)[0]


def read_stub_sidecar(path):
    """读 `<blob>.stubs`（`tcblob` 写的 extern 跳板清单，接口冻结版 1.1 增量）。

    → [(offset, kind, symbol)]，offset 相对机器码起点；缺文件返回 []（老产物照旧跑）。
    kind 只有两种：`stub` = 12 字节代码桩（`movabs rax,imm64 ; jmp rax`，改写 offset+2 处的 imm64），
    `slot` = `__imp_X` 的 8 字节数据槽（改写 offset 处的槽内容）。
    """
    if not path or not os.path.isfile(path):
        return []
    out = []
    try:
        with open(path, encoding="utf-8") as f:
            rows = [ln.rstrip("\n").split("\t") for ln in f if ln.strip()]
    except OSError as e:
        raise SystemExit(f"读不了 {path}：{e}") from e
    if not rows or rows[0][:3] != ["offset", "kind", "symbol"]:
        raise SystemExit(f"{path}: 表头不是 offset/kind/symbol")
    for r in rows[1:]:
        if len(r) != 3:
            raise SystemExit(f"{path}: 这一行列数不是 3：{r}")
        try:
            off = int(r[0], 16)
        except ValueError as e:
            raise SystemExit(f"{path}: offset 不是十六进制：{r[0]}") from e
        if r[1] not in ("stub", "slot"):
            raise SystemExit(f"{path}: 未知 kind {r[1]}")
        out.append((off, r[1], r[2]))
    return out


def sidecar_path_for(bin_path):
    """与产物同名同目录：`<blob>.stubs`（游戏内路径不用它 —— 壳自己就是解析器）。"""
    return bin_path + ".stubs"


def mapped_region(addr):
    """addr 在本进程里是否落在**已提交**区间 —— 产物里这样的 movabs 能解引用（spec §6）。

    `classify_abs_addrs.query_mapping`（经 abs_scan 复用）已挡掉两个「VirtualQuery 成功但不是映射」
    的形状：未分配区间（`State=MEM_FREE`、`AllocationBase=0`，见 docs/pitfalls.md）与 MEM_RESERVE-only
    区间（`Protect=0`，实测）。能解引用的区间 `Protect` 必非 0。
    """
    m = abs_scan.CAA.query_mapping(addr)
    if m is None or not m["protect"]:
        return None
    return m


def detour_name(symbol):
    """sidecar 里的符号名 → detour 表里的名字：`__imp_X` 要去掉前缀。

    与 `cli/tcblob.cc` 的 `Win32Resolver::resolve` 同一规则 —— 两处不一致会让
    「同一个符号一半命中一半不命中」，那种错极难查。
    """
    return symbol[6:] if symbol.startswith("__imp_") else symbol


def recover_baked_base(code, thunks, game_lo, game_hi):
    """产物里指向 DLL 的地址 = baked_base + thunk_rva → 用投票反推 baked_base。"""
    votes = collections.Counter()
    for _off, v in movabs_sites(code):
        if game_lo - BAND <= v <= game_hi + BAND:
            continue
        for t in thunks:
            if (v - t) % PAGE == 0:
                votes[v - t] += 1
    if not votes:
        return 0, []
    return votes.most_common(1)[0][0], votes.most_common(4)


class Detours:
    """把危险的导入换成宿主桩，并记录调用序列（快照里的 extern 就是这个）。

    节奏类调用（`Sleep` / 读时钟）**不进可观测序列**：它们条数取决于真实时间（程序在两次命令之间
    的轮询次数），对仿真状态没有语义影响。主设计 §7.1 允许把不可观测差异显式列白；这里是那一列，
    而且只记计数放在诊断字段里，看得见。
    """

    PACING = frozenset({"Sleep", "GetSystemTimePreciseAsFileTime"})

    def __init__(self):
        self.records = []
        self.pacing = collections.Counter()
        self.clock = 0
        self.arena = 0
        self.arena_end = 0
        self.rsv_cur = RSV_BASE           # RESERVE-only 的保留区（只留地址空间，不提交页）
        self.rsv_end = RSV_BASE
        self._cbs = {}
        self._make_handlers()

    # -- 基础设施 --------------------------------------------------------
    def _cb(self, fn):
        cb = SIG(fn)
        self._cbs[fn.__name__] = cb
        return cb

    def addr_for(self, name):
        """宿主桩的地址（0 = 这个符号没有桩）。

        给 `Host._patch_externs` 用：产物自己的 extern 跳板要指到这里，走的才与 IAT 槽路径
        **完全同一条**（同一个 handler → 同一个记录格式），两遍跑的 extern 序列才能直接比。
        """
        cb = self.handlers.get(name)
        return ctypes.cast(cb, ctypes.c_void_p).value if cb else 0

    def _rec(self, name, args, ret):
        if name in self.PACING:
            self.pacing[name] += 1
            return
        self.records.append({"name": name,
                             "args": [int(a) & 0xFFFFFFFFFFFFFFFF for a in args],
                             "ret": int(ret) & 0xFFFFFFFFFFFFFFFF})

    def _bump(self, size):
        size = (int(size) + 0xFFF) & ~0xFFF
        if not self.arena:
            self.arena = mem_alloc(ARENA_BASE, ARENA_SIZE)
            self.arena_end = self.arena + ARENA_SIZE
        if self.arena + size > self.arena_end:
            return k32.VirtualAlloc(None, size, MEM_COMMIT | MEM_RESERVE, PAGE_RW) or 0
        p = self.arena
        self.arena += size
        return p

    def _reserve(self, size):
        """只保留地址空间、一页都不提交 —— `VirtualAlloc(MEM_RESERVE)` 的语义。

        返回值进 `extern` 记录（两遍跑要逐字节比），所以地址必须确定：从固定首址顺序切，
        而不是让系统自己挑（`VirtualAlloc(NULL, …)` 的返回值随机器负载变）。
        保留区按需一块块留，请求多大就至少留多大。
        """
        size = (int(size) + 0xFFF) & ~0xFFF
        if self.rsv_cur + size > self.rsv_end:
            block = max(size, RSV_BLOCK)
            p = k32.VirtualAlloc(ctypes.c_void_p(self.rsv_end), block, MEM_RESERVE, PAGE_NOACCESS)
            if not p or p != self.rsv_end:
                # 跑在产物线程上，抛异常会被 ctypes 吞掉 → 只能吵 + 返回 0（与真实 API 失败同形）
                print(f"[host] 硬错误：保留区 0x{self.rsv_end:X}+0x{block:X} 的 "
                      f"VirtualAlloc(MEM_RESERVE) 失败（返回 0x{(p or 0):X}，"
                      f"err={ctypes.get_last_error()}）—— 宿主环境问题，不是产物的差异",
                      file=sys.stderr, flush=True)
                return 0
            self.rsv_cur, self.rsv_end = self.rsv_end, self.rsv_end + block
        p = self.rsv_cur
        self.rsv_cur += size
        return p

    # -- 各桩 ------------------------------------------------------------
    def _exit_thread(self, code, *_):
        self._rec("ExitThread", [code], 0)
        k32.ExitThread(code & 0xFFFFFFFF)
        return 0

    def _exit_process(self, code, *_):
        # 真 ExitProcess 会杀整个宿主进程（差分就没了）→ 只结束本线程
        self._rec("ExitProcess", [code], 0)
        k32.ExitThread(code & 0xFFFFFFFF)
        return 0

    def _sleep(self, ms, *_):
        self._rec("Sleep", [ms], 0)
        return 0

    def _filetime(self, ptr, *_):
        self.clock += 10000                      # 每次调用 +10 ms（100ns 单位），确定化
        wr_u64(ptr, 116444736000000000 + self.clock)
        # 只记时钟值：ptr 是**调用方的栈地址**，两遍跑必不一样（记了就永远对不上）
        self._rec("GetSystemTimePreciseAsFileTime", [self.clock], 0)
        return 0

    def _virtual_alloc(self, addr, size, typ, prot, *_):
        """`typ` 里的 MEM_RESERVE 与 MEM_COMMIT 必须分开：只有 RESERVE 时一页都不许提交。

        参考产物（Nim 分配器）启动就 `VirtualAlloc(NULL, 10^10, MEM_RESERVE, PAGE_NOACCESS)` ×N
        当 arena，之后按需 COMMIT 里面的小片。宿主原来把 `addr == 0` 一律当 COMMIT 走 `_bump`
        （arena 装不下就**真提交 10 GB**）：提交到 ≈40 GB 时失败返回 0，参考产物不查返回值、
        拿它当 seq 数据指针写头 → 写 AV，20 份语料的参考产物因此在宿主里判不了
        （`docs/notes/2026-10-18-tickdiff-diagnosis.md` §2）。
        """
        if addr:
            out = k32.VirtualAlloc(ctypes.c_void_p(addr), size, typ or MEM_COMMIT | MEM_RESERVE,
                                   prot or PAGE_RW) or 0
        elif typ & MEM_RESERVE and not typ & MEM_COMMIT:
            out = self._reserve(size)
        else:
            out = self._bump(size)
        self._rec("VirtualAlloc", [addr, size, typ, prot], out)
        return out

    def _virtual_free(self, addr, size, typ, *_):
        self._rec("VirtualFree", [addr, size, typ], 1)
        return 1

    def _write_file(self, _fh, buf, n, written, *_):
        data = ctypes.string_at(buf, min(n, 32)) if buf and n else b""
        if written:
            wr_u64(written, n)
        self._rec("WriteFile", [0, n, int.from_bytes(data, "little")], n)
        return 1

    def _puts(self, s, *_):
        txt = ctypes.string_at(s) if s else b""
        self._rec("puts", [int.from_bytes(txt[:8], "little")], 0)
        return 0

    def _fwrite(self, buf, size, n, _stream, *_):
        data = ctypes.string_at(buf, min(size, 32)) if buf and size else b""
        self._rec("fwrite", [int.from_bytes(data, "little"), size, n], n)
        return n

    def _write(self, _fd, buf, n, *_):
        data = ctypes.string_at(buf, min(n, 32)) if buf and n else b""
        self._rec("_write", [int.from_bytes(data, "little"), n], n)
        return n

    def _vfprintf(self, *_args):
        self._rec("__stdio_common_vfprintf", [], 0)
        return 0

    def _create_file(self, _path, *_args):
        # rt_min.cc 的 spike 记录器用（没它就没有代码桩可接）→ 一律失败：那玩意儿我们不需要，
        # 而真去开文件会拿真实时间/真实磁盘，两遍跑不可重复。
        self._rec("CreateFileA", [], 0xFFFFFFFFFFFFFFFF)
        return 0xFFFFFFFFFFFFFFFF                # INVALID_HANDLE_VALUE

    def _close_handle(self, _h, *_args):
        self._rec("CloseHandle", [_h], 1)
        return 1

    def _make_handlers(self):
        self.handlers = {
            "ExitThread": self._cb(self._exit_thread),
            "ExitProcess": self._cb(self._exit_process),
            "Sleep": self._cb(self._sleep),
            "GetSystemTimePreciseAsFileTime": self._cb(self._filetime),
            "VirtualAlloc": self._cb(self._virtual_alloc),
            "VirtualFree": self._cb(self._virtual_free),
            "WriteFile": self._cb(self._write_file),
            "puts": self._cb(self._puts),
            "fwrite": self._cb(self._fwrite),
            "_write": self._cb(self._write),
            "__stdio_common_vfprintf": self._cb(self._vfprintf),
            "CreateFileA": self._cb(self._create_file),
            "CloseHandle": self._cb(self._close_handle),
        }


class Host:
    def __init__(self, spec, blob, entry_off, budget=120.0, dump_dir=None, verbose=True,
                 stub_path=None, region_hash_every=1, seed_reset_data=None):
        self.spec = spec
        self.blob = blob
        self.entry = entry_off
        self.budget = budget
        self.dump_dir = dump_dir
        self.verbose = verbose
        # `#RESET_DATA_*` 的图案（None = 不灌，保持全零的历史行为）
        self.seed_pattern = seed_reset_data
        # 段哈希每几个 tick 取一次（1 = 每次都取）。N > 1 只是为了在慢靶子上压耗时，
        # 跳过的那一 tick 快照里 `regions` = null（`selftest.compare` 只在两边都是字典时比）。
        self.region_hash_every = region_hash_every
        # extern 跳板清单（`<blob>.stubs`，可能不存在 → 老行为：产物里不许有 extern 桩）
        self.stub_path = stub_path
        self.stub_decl = read_stub_sidecar(stub_path)

        self.regions = []
        self.spec_regions = []
        self.baseline = {}
        self.detours = Detours()
        self.thread = None
        self.cmd_id = 0
        self.tick_no = 0
        self.code_alloc = 0
        self.thunks = {}
        self.imports = []
        self.baked = 0
        self.dll_base = 0
        self.rebased = 0
        self.stats = {"game": 0, "dll": 0, "const": 0, "unknown": 0, "stubs": 0,
                      "slots": 0}
        self.game_like = []
        self.unexplained = []
        self.stub_hits = []              # [(offset, 桩地址, 符号, 0)]（`_scan_product` 里填）
        self.stub_patched = []           # [(offset, kind, 符号, 宿主桩地址)]（`_patch_externs` 里填）
        self.extern_patched = 0

    # -- 准备 ------------------------------------------------------------
    def setup(self):
        self._load_dll()
        self._scan_product()
        self._map_game_memory()
        self._install_detours()
        self._seed_memory()
        self._seed_reset_data()
        self._snapshot_baseline()
        self._start_thread()
        return self

    def _load_dll(self):
        k32.AddDllDirectory(GAME_DIR)
        h = k32.LoadLibraryExW(DLL_PATH, None, 0x100 | 0x200)   # DEFAULT_DIRS | USER_DIRS
        if not h:
            raise SystemExit(f"加载 {DLL_PATH} 失败 err={ctypes.get_last_error()}")
        self.dll_base = h
        self.pe = abs_scan.Pe(DLL_PATH)
        self.thunks = self.pe.import_thunks()
        self.imports = self.pe.imports()
        if self.verbose:
            print(f"[host] DLL 加载 @0x{self.dll_base:X}，导入 {len(self.imports)}、"
                  f"thunk {len(self.thunks)}、镜像 0x{self.pe.image_size:X}", file=sys.stderr)

    def _scan_product(self):
        code = self.blob[8:]
        lo, hi = self.spec.game_lo, self.spec.game_hi
        self.baked, cands = recover_baked_base(code, self.thunks, lo, hi)
        if self.verbose and self.baked:
            print(f"[host] 产物 baked 基址 0x{self.baked:X}"
                  f"（候选 {[(hex(b), n) for b, n in cands[:3]]}），"
                  f"实际 0x{self.dll_base:X}，delta 0x{self.dll_base - self.baked:X}", file=sys.stderr)

        game_like, unexplained = [], []
        stub_at = {off: sym for off, kind, sym in self.stub_decl if kind == "stub"}
        for off, v in movabs_sites(code):
            sym = stub_at.get(off - 2)
            if sym is not None:
                # extern 代码桩：它是产物**自己**造的跳板，不是「游戏地址」也不是未知地址。
                # 地址由 `_patch_externs` 换成宿主桩（不换就是真 kernel32 调用，两遍跑必不同）。
                self.stats["stubs"] += 1
                self.stub_hits.append((off, v, sym, 0))
                continue
            if lo - BAND <= v <= hi + BAND:
                game_like.append(v)
            elif self.baked and self.baked <= v < self.baked + self.pe.image_size:
                self.stats["dll"] += 1
            elif v in self.spec.literals:
                self.stats["const"] += 1
            else:
                unexplained.append((off, v))
        self.stats["game"] = len(game_like)
        self.stats["unknown"] = len(unexplained)
        self.game_like = game_like
        self.unexplained = sorted({v for _off, v in unexplained})
        self.stats["slots"] = sum(1 for _o, k, _s in self.stub_decl if k == "slot")
        if self.verbose:
            extra = (f"、extern 跳板 {self.stats['stubs']} 桩/{self.stats['slots']} 槽"
                     f"（{self.stub_path}）" if self.stub_decl else "")
            print(f"[host] movabs：游戏段 {self.stats['game']}、DLL {self.stats['dll']}、"
                  f"常量 {self.stats['const']}、未解释 {self.stats['unknown']}{extra}"
                  + (f"（例 {[hex(v) for v in self.unexplained[:4]]}）"
                     if unexplained else ""), file=sys.stderr)
        self._check_stub_contract()
        self._reject_mapped_movabs(unexplained)

    def _check_stub_contract(self):
        """sidecar 说了有 `stub` 跳板，就得到那个偏移上真的找到 `movabs rax,imm64`。

        对不上说明 sidecar 与产物不是一对（旧的 `.stubs` 配新产物之类）—— 那种情况下
        「哪些 movabs 是跳板」的判定已经错了，必须吵，不能接着跑。
        """
        found = {off - 2 for off, _v, _s, _a in self.stub_hits}
        missing = [(o, s) for o, kind, s in self.stub_decl if kind == "stub" and o not in found]
        if missing:
            for o, s in missing:
                print(f"[host] 硬错误：sidecar 声明 0x{o:X} 是 {s} 的代码桩，那里没有 movabs",
                      file=sys.stderr, flush=True)
            raise SystemExit(EXIT_STUB_CONTRACT)

    def _reject_mapped_movabs(self, sites):
        """地址表外 + 本进程已映射的立即数 = 能解引用 → 会静默读错内存 → 宿主报错退出（spec §6）。

        只在**未映射**时才退化成统计：实测每份参考产物都有 4 处大整数常量
        （`0x2540be400`、`0x19db1ded53e8000` 之类），它们本来就该被忽略。
        """
        hits = [(off, v, m) for off, v in sites if (m := mapped_region(v)) is not None]
        if not hits:
            return
        for off, v, m in hits:
            where = (f"{m['mapped']} alloc_base=0x{m['alloc_base']:X}" if m["mapped"]
                     else f"私有区 type=0x{m['type']:X} protect=0x{m['protect']:X}"
                          f" alloc_base=0x{m['alloc_base']:X}")
            print(f"[host] 硬错误：机器码 +0x{off:X}（指令 +0x{off - 2:X}）的 movabs 立即数 "
                  f"0x{v:X} 落在本进程已映射区间（{where}，区间 0x{m['base']:X}+0x{m['size']:X}）",
                  file=sys.stderr, flush=True)
        print(f"[host] 共 {len(hits)} 处地址表外且已映射的 movabs —— 能解引用、会静默读错内存"
              f"（spec §6），宿主拒绝运行", file=sys.stderr, flush=True)
        raise SystemExit(EXIT_MAPPED_MOVABS)

    def _map_game_memory(self):
        items = self.spec.regions() + [(a, a) for a in self.game_like]
        self.regions = merge_spans(items)
        # 段哈希要比的是**同一些字节**，所以不能用 `self.regions`：它的几何随产物变
        # （产物 movabs 里的游戏地址也各给 1 MB，合并边界跟着变 —— 实测 278.6 vs 278.7 MB，
        #  两个产物永远比不出「相等」）。`spec_regions` 只由源码算：地址表 + `memory_copy`/
        # `memory_clear` 实参 —— 同一份 `.si` 的两个产物几何逐字节相同（含 `#DATA_*` 的 128 MB）。
        self.spec_regions = merge_spans(self.spec.regions())
        for lo, size in self.regions:
            mem_alloc(lo, size)
        if self.verbose:
            print(f"[host] 游戏内存：{len(self.regions)} 段、"
                  f"{sum(s for _l, s in self.regions) / 1048576:.1f} MB（段哈希按源码算的 "
                  f"{len(self.spec_regions)} 段、"
                  f"{sum(s for _l, s in self.spec_regions) / 1048576:.1f} MB）", file=sys.stderr)

    def _install_detours(self):
        seen = set()
        for slot_rva, full in self.imports:
            proc = full.split("!", 1)[1]
            cb = self.detours.handlers.get(proc)
            if cb is None or proc in seen:
                continue
            seen.add(proc)
            addr = self.dll_base + slot_rva
            old = ctypes.c_ulong()
            if not k32.VirtualProtect(ctypes.c_void_p(addr), 8, PAGE_RW, ctypes.byref(old)):
                raise SystemExit(f"VirtualProtect({hex(addr)}) 失败")
            wr_u64(addr, ctypes.cast(cb, ctypes.c_void_p).value)
        if self.verbose:
            print(f"[host] detour 已装：{sorted(seen)}", file=sys.stderr)

    def _seed_memory(self):
        self.CTL = self.spec.enum("CommandIndex")
        self.ST = self.spec.enum("StateIndex")
        self.CMDV = self.spec.enum("SimCommand")
        self.cmd_block = self.spec.addr("commands")
        self.state = self.spec.addr("settings")
        for i in range(len(self.CTL)):
            wr_u64(self.cmd_block + i * 8, 0)
        wr_u64(self.cmd_block + self.CTL["ctl_cycle_speed_ms"] * 8, 10 ** 13)
        for i in range(len(self.ST)):
            wr_u64(self.state + i * 8, 0)
        for _name, addr, _elem in self.spec.arrays:
            wr_u64(addr - 8, (ARR_LEN << 16) | ARR_CAP_LOG2)

    def _seed_reset_data(self):
        """跑第一个 tick **之前**把 `#RESET_DATA_*` 的块填成确定性图案。

        为什么要有它：`reset_sim` 每 tick 把 `#RESET_DATA_*` 整块拷进 `#DATA_*`，而宿主把两边
        都按零页摆出来 —— 这份拷贝等于没拷，rv32im 那 272.25 MB 的 RAM 镜像（游戏里是 ROM/
        程序映像）从没被演练（见 `docs/facts.md` 同名边界）。

        只灌 `#RESET_DATA_*`：`#DATA_*` 由程序自己的 `reset_sim` 灌，正是要走一遍的那条路径；
        也不碰 `commands`/`settings`/`#SIMULATION_STATE`/`error_buffer` 这些协议区 —— 区间只认
        `simspec.reset_data_blocks`（`memory_copy` 的源实参），不是「整个地址表」，还有一条硬检查：
        灌的那段里不许出现**任何别的已声明对象**（包住就报错退出）。

        时机：在 `_seed_memory` 之后、`_snapshot_baseline` 之前 —— 基线就带上了图案，
        所以 `pages` 仍然是「程序相对 pre-run 基线改动了什么」，语义没变。

        两个进程必须用**同一个**图案（`--seed-reset-data` 只收一个值、两条宿主命令都传它）；
        图案不同的话段哈希与页都会报差异，那不算合法判据。
        """
        if not self.seed_pattern:
            return
        blocks = self.spec.reset_data_blocks
        if not blocks:
            print(f"[host] {self.spec.path} 里没有 `#RESET_DATA_*` 块 —— "
                  f"--seed-reset-data {self.seed_pattern} 无事可做", file=sys.stderr)
            return
        t0 = time.perf_counter()
        total = 0
        declared = self.spec.named_addrs()
        for name, lo, size in blocks:
            hi = lo + size
            if not any(rlo <= lo and hi <= rlo + rsize for rlo, rsize in self.regions):
                raise SystemExit(f"[host] 硬错误：#RESET_DATA 块 {name} 的 [0x{lo:X}, 0x{hi:X}) "
                                 f"不在已映射区间里 —— 灌图案会写坏宿主，拒绝")
            # 硬检查：灌的那段里不许有别的已声明对象（否则就是把图案写进了协议区）
            others = [(n, a) for n, a in declared if lo <= a < hi and n != name]
            if others:
                raise SystemExit(f"[host] 硬错误：#RESET_DATA 块 {name} 的 [0x{lo:X}, 0x{hi:X}) "
                                 f"盖住了别的已声明对象 {[(n, hex(a)) for n, a in others]} "
                                 f"—— 灌图案只允许碰 `#RESET_DATA_*`，拒绝")
            ctypes.memmove(lo, pattern_bytes(self.seed_pattern, size), size)
            total += size
            if self.verbose:
                print(f"[host] seed {name} [0x{lo:X}, 0x{hi:X}) {size / 1048576:.2f} MB "
                      f"= {self.seed_pattern}", file=sys.stderr)
        dt = time.perf_counter() - t0
        print(f"[host] `#RESET_DATA_*` 已灌：{len(blocks)} 块 / {total / 1048576:.2f} MB / "
              f"图案 {self.seed_pattern} / {dt:.2f} s", file=sys.stderr)

    def _snapshot_baseline(self):
        for lo, size in self.regions:
            for p in range(lo, lo + size, PAGE):
                self.baseline[p] = ctypes.string_at(p, PAGE)

    def _rebase(self, code):
        """把产物里指向 DLL 的绝对地址从 baked 基址改到**实际**加载基址。

        产物里那些 movabs 是捕获时那一 boot 的 `baked_base + RVA`；本进程里 DLL 加载在别处，
        不改就会读到未映射区（实测就是这个 AV：读 `baked+0x131278` = DLL 的 .bss）。
        """
        if not self.baked or self.baked == self.dll_base:
            return code, 0
        delta = self.dll_base - self.baked
        buf = bytearray(code)
        n = 0
        for off, v in movabs_sites(code):
            if self.baked <= v < self.baked + self.pe.image_size:
                struct.pack_into("<Q", buf, off, v + delta)
                n += 1
        return bytes(buf), n

    def _patch_externs(self, code):
        """把产物**自己造的** extern 跳板里的真实地址换成宿主桩地址（sidecar 说了哪几处）。

        为什么必须改：不改就是真的 `kernel32!Sleep` / 真 `ExitThread` —— 前者引入真实时间
        （两遍跑必不同），后者直接杀掉宿主线程。改完之后产物走的是与 IAT 槽路径**同一个**
        handler，所以两边的 extern 序列可以直接逐条比。
        未命中 detour 表 → 报符号名退出（宁可吵，不把不可控的真调用放进去）。
        """
        if not self.stub_decl:
            return code, 0
        buf = bytearray(code)
        n = 0
        miss = []
        hits = []
        for off, kind, sym in self.stub_decl:
            proc = detour_name(sym)
            addr = self.detours.addr_for(proc)
            if not addr:
                miss.append((off, kind, sym))
                continue
            at = off + 2 if kind == "stub" else off     # `movabs rax,imm64` 的 imm64 / 槽内容
            if at + 8 > len(buf):
                raise SystemExit(f"[host] sidecar 的 0x{off:X} {sym} 落在产物外")
            struct.pack_into("<Q", buf, at, addr)
            hits.append((off, kind, sym, addr))
            n += 1
        if miss:
            for off, kind, sym in miss:
                print(f"[host] extern 跳板没有宿主桩：0x{off:X} ({kind}) {sym}" + "\n" +
                      "       → 不能放真实调用进去（两遍跑的确定性会没）；补 detour 或别链它。",
                      file=sys.stderr, flush=True)
            raise SystemExit(EXIT_STUB_CONTRACT)
        # 记回明细：扫到的那批（在 `_scan_product` 里已有偏移/桩地址）对上宿主桩地址
        self.stub_patched = hits
        return bytes(buf), n

    def _start_thread(self):
        code, self.rebased = self._rebase(self.blob[8:])
        code, self.extern_patched = self._patch_externs(code)
        self.code_alloc = mem_alloc(0, len(code) + PAGE, PAGE_RWX)
        ctypes.memmove(self.code_alloc, code, len(code))
        self.entry_addr = self.code_alloc + self.entry
        # 游戏是 **jmp** 进来的（rsp 16 对齐、没有返回地址）；CreateThread 直接用 entry 是 **call**
        # （rsp ≡ 8），产物里对齐敏感的 SSE 指令会直接 AV。所以走一段 trampoline 复现 jmp 语义：
        #   and rsp, -16 ; mov rax, entry ; jmp rax
        tramp = mem_alloc(0, PAGE, PAGE_RWX)
        stub = (b"\x48\x83\xE4\xF0"                       # and rsp, -16
                + b"\x48\xB8" + struct.pack("<Q", self.entry_addr)   # mov rax, imm64
                + b"\xFF\xE0")                            # jmp rax
        ctypes.memmove(tramp, stub, len(stub))
        self._entry_fn = ctypes.CFUNCTYPE(None, ctypes.c_void_p)(tramp)
        tid = ctypes.c_ulong()
        self.thread = k32.CreateThread(None, 0, ctypes.cast(self._entry_fn, ctypes.c_void_p),
                                       None, 0, ctypes.byref(tid))
        if not self.thread:
            raise SystemExit(f"CreateThread 失败 err={ctypes.get_last_error()}")
        if self.verbose:
            extra = [f"{k} {s}" for _o, k, s, _a in self.stub_patched]
            print(f"[host] 线程已起：代码 @0x{self.code_alloc:X}，entry +0x{self.entry:X} → "
                  f"0x{self.entry_addr:X}（经 trampoline @0x{tramp:X}），重定位 {self.rebased} 处"
                  + (f"，extern 跳板换桩 {self.extern_patched} 处 {extra}"
                     if self.extern_patched else ""), file=sys.stderr)

    # -- 执行 ------------------------------------------------------------
    def alive(self):
        return k32.WaitForSingleObject(ctypes.c_void_p(self.thread), 0) != 0

    def tick(self, n):
        """把 sim_target_cycle 设成 n 并递增命令号发 run，等它跑完。返回快照。"""
        before = len(self.detours.records)
        wr_u64(self.state + self.ST["sim_target_cycle"] * 8, n)
        wr_u64(self.cmd_block + self.CTL["ctl_command"] * 8, self.CMDV["run"])
        self.cmd_id += 1
        wr_u64(self.cmd_block + self.CTL["ctl_command_id"] * 8, self.cmd_id)

        t0 = time.monotonic()
        reason = "ok"
        stable = 0
        while True:
            if not self.alive():
                reason = "thread_exit"
                break
            cyc = rd_u64(self.state + self.ST["sim_cycle"] * 8)
            running = rd_u64(self.state + self.ST["sim_running"] * 8)
            last = rd_u64(self.state + self.ST["sim_last_command_id"] * 8)
            done = last == self.cmd_id and running == 0
            # 完成的判据**不能**要求 `sim_cycle == n`：被测试结果 halt 的 run 会提前结束
            # （实测 120：`check_output` 返回 win → halt → sim_cycle 停在 0）。也不能只看一次
            # `last==id && running==0`：程序写 last_command_id 与置 sim_running=1 之间有个极短窗口
            # 也是这个形状 —— 所以要求连续两次采样都如此（窗口只有几条内存操作，跨不过两次轮询）。
            if done:
                stable += 1
                if stable >= 2 or cyc >= n:
                    break
            else:
                stable = 0
            if time.monotonic() - t0 > self.budget:
                reason = "timeout"
                break
            time.sleep(0.0005)
        changed, stabilized = self._settled_pages()
        # 段哈希紧跟稳定判定取：它只多花一次读，但断的是「跑完这一 tick 之后的整段内容」
        regions = self._region_hashes() if n % self.region_hash_every == 0 else None
        return self._snapshot(n, reason, self.detours.records[before:], changed, stabilized,
                              regions)

    def _changed_pages(self, dump=False):
        changed = {}
        for lo, size in self.regions:
            for p in range(lo, lo + size, PAGE):
                cur = ctypes.string_at(p, PAGE)
                if cur != self.baseline[p]:
                    changed[hex(p)] = hashlib.sha256(cur).hexdigest()[:16]
                    if dump:
                        self._dump_page(self.tick_no, p, cur)
        return changed

    def _region_hashes(self):
        """`self.spec_regions` 每段的**绝对内容** sha256 → `{hex(lo): 64 位小写十六进制}`。

        `_changed_pages` 给的是「相对 pre-run 基线变过的页」—— 基线不进快照，所以那份判据
        只证明「两边的**增量**一样」。段哈希补上的是绝对内容：整段（rv32im 是 6 段、
        278.5 MB，含 `#DATA_*` 的 128 MB RAM 镜像）逐字节一致，与基线是什么无关。
        """
        out = {}
        for lo, size in self.spec_regions:
            h = hashlib.sha256()
            for off in range(0, size, HASH_CHUNK):
                h.update(ctypes.string_at(lo + off, min(HASH_CHUNK, size - off)))
            out[hex(lo)] = h.hexdigest()
        return out

    def _settled_pages(self, tries=20):
        """等内存稳定后再取快照。

        跑完一个 run 后程序并没停：它在自己的外层循环里刷 UI、更新状态，也在写内存 ——
        任取一个时刻去拍，可能拍到刷到一半的中间态。连续两次采样一致才认定稳定；
        等不到就用最后一次（但记下来，不假装稳定）。
        """
        prev = None
        for _ in range(tries):
            cur = self._changed_pages()
            if cur == prev:
                return cur, True
            prev = cur
            time.sleep(0.001)
        return prev or {}, False

    def _snapshot(self, tick, reason, recs, changed, stabilized, regions):
        self.tick_no = tick
        if self.dump_dir:
            self._changed_pages(dump=True)          # 只在最终快照时落盘
        return {
            "tick": tick,
            "reason": reason,
            "stabilized": stabilized,
            "alive": self.alive(),
            "sim_cycle": rd_u64(self.state + self.ST["sim_cycle"] * 8),
            "sim_target_cycle": rd_u64(self.state + self.ST["sim_target_cycle"] * 8),
            "sim_last_command_id": rd_u64(self.state + self.ST["sim_last_command_id"] * 8),
            "sim_running": rd_u64(self.state + self.ST["sim_running"] * 8),
            "sim_test_result": rd_u64(self.state + self.ST["sim_test_result"] * 8),
            "pages": changed,
            "regions": regions,
            "extern": recs,
            "pacing": dict(self.detours.pacing),
        }

    def _dump_page(self, tick, page, data):
        d = self.dump_dir or ""
        try:
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, f"t{tick:04d}_{page:012x}.bin"), "wb") as f:
                f.write(data)
        except OSError as e:
            print(f"[host] 写页转储失败：{e}", file=sys.stderr)

    def close(self):
        """发 quit_simulation 让程序自己退出；它就是不走就硬杀。"""
        try:
            wr_u64(self.cmd_block + self.CTL["ctl_command"] * 8, self.CMDV["quit_simulation"])
            self.cmd_id += 1
            wr_u64(self.cmd_block + self.CTL["ctl_command_id"] * 8, self.cmd_id)
            if k32.WaitForSingleObject(ctypes.c_void_p(self.thread), 3000) == 0:
                return "exited_clean"
            k32.TerminateThread(ctypes.c_void_p(self.thread), 0)
            return "terminated"
        except Exception as exc:                            # 收尾路径：不往外抛，但异常本身要看见
            print(f"[host] close 异常：{exc}", file=sys.stderr)
            return "unknown"


def entry_from_manifest(bin_path):
    name = os.path.basename(bin_path)
    stem = os.path.splitext(name)[0]
    try:
        with open(CORPUS_MANIFEST, encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f]
    except OSError as e:
        raise SystemExit(f"读不了 {CORPUS_MANIFEST}：{e}") from e
    head = rows[0]
    if "entry" not in head:
        raise SystemExit(f"{CORPUS_MANIFEST} 没有 entry 列")
    ei, fi = head.index("entry"), head.index("file")
    for row in rows[1:]:
        if os.path.splitext(os.path.basename(row[fi]))[0] == stem:
            return int(row[ei], 0)
    raise SystemExit(f"{name} 在 manifest 里找不到")


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise SystemExit(f"读不了 {path}：{e}") from e


def main(argv=None):
    ap = argparse.ArgumentParser(description="离线差分宿主")
    ap.add_argument("--src", required=True, help="源码 .si（脚本/地址表/枚举都从这里读）")
    ap.add_argument("--bin", required=True, help="产物 .bin（含 8 字节头）")
    ap.add_argument("--entry", default=None, help="入口偏移（默认从 corpus/manifest.tsv 查）")
    ap.add_argument("--ticks", type=int, default=20, help="跑多少个 tick（sim_target_cycle = 1..N）")
    ap.add_argument("--out", default=None, help="快照 JSONL 输出路径")
    ap.add_argument("--budget", type=float, default=120.0, help="每 tick 预算（秒）")
    ap.add_argument("--dump-pages", default=None, help="把变化页的字节也落盘到这个目录")
    ap.add_argument("--region-hash-every", type=int, default=1, metavar="N",
                    help="每 N 个 tick 给每个映射段算一次内容 sha256（默认 1 = 每个 tick 都算；"
                         "跳过的 tick 快照里 regions=null）")
    ap.add_argument("--seed-reset-data", default=None, choices=SEED_PATTERNS, metavar="PATTERN",
                    help="跑第一个 tick 之前，把源码里 `#RESET_DATA_*` 指向的块填成确定性图案："
                         "seq = byte[i] = i & 0xFF；xorshift = 固定种子 xorshift64 字节流。"
                         "默认不灌（保持全零的历史行为）。**差分判据要求两边传同一个值**。")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    if args.region_hash_every < 1:
        raise SystemExit(f"--region-hash-every 要 >= 1（给了 {args.region_hash_every}）："
                         "0 等于把段哈希判据关掉，判据只能加强不能放宽")

    spec = simspec.SimSpec.parse(args.src)
    blob = read_bytes(args.bin)
    entry = int(args.entry, 0) if args.entry else entry_from_manifest(args.bin)

    host = Host(spec, blob, entry, budget=args.budget, dump_dir=args.dump_pages,
                verbose=not args.quiet, stub_path=sidecar_path_for(args.bin),
                region_hash_every=args.region_hash_every,
                seed_reset_data=args.seed_reset_data)
    install_veh(lambda: host.code_alloc, lambda: host.dll_base)   # 必须早于起线程
    host.setup()
    reason = "ok"
    with contextlib.ExitStack() as stack:
        out = None
        if args.out:
            try:
                out = stack.enter_context(open(args.out, "w", encoding="utf-8"))
            except OSError as e:
                raise SystemExit(f"写不了 {args.out}：{e}") from e
        try:
            for n in range(1, args.ticks + 1):
                snap = host.tick(n)
                if not args.quiet:
                    print(f"tick {n:3d}: {snap['reason']:11} 页 {len(snap['pages']):4d} "
                          f"extern {len(snap['extern']):3d} sim_cycle={snap['sim_cycle']} "
                          f"alive={snap['alive']}")
                if out:
                    out.write(json.dumps(snap, ensure_ascii=False) + "\n")
                    out.flush()
                if snap["reason"] != "ok":
                    reason = snap["reason"]
                    break
        finally:
            end = host.close()
            if not args.quiet:
                print(f"[host] 收尾：{end}；detour 记录 {len(host.detours.records)} 条", file=sys.stderr)
    return 0 if reason == "ok" else 2


if __name__ == "__main__":
    sys.exit(main())
