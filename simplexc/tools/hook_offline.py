#!python3
"""壳的失败路径 / 熔断 / 回退 / 超时的**离线判据**（一条命令跑完，入库版）。

判据语义与 `work/tmp/hook_test/harness.py`（一次性前身）一致；相对它的改动只有三处：
工作目录统一到 `work/tmp/hook_offline/`、每个 case 返回结构化结果（供
`simplexc/tools/check.py` 汇总）、替身改写器由本文件自己落盘（不再依赖 work/tmp 里的散件）。

跑什么
------
| case | 判据 |
|---|---|
| `ours-missing`     | mode=ours + 自研 blob 缺失 → 失败串 `ours: InternalError`、manifest `impl=ours_failed`、hook.log 的 `error` + `impl` 两条都在 |
| `fallback-missing` | mode=ours_with_fallback + blob 缺失 → 整单转交参考实现：产物与**同一进程里**参考实现的产物逐字节相同、manifest `impl=fallback` |
| `ref`              | mode=reference → manifest `impl=reference`、hook.log 有 `impl` 行（回退率的分母） |
| `ours-ok`          | mode=ours + 真 blob → 产物与「改动前」逐字节相同、无 `error` 行 |
| `refcmp`           | reference 不回归：新壳与「改动前」的产物逐字节相同 |
| `timeout-ours`     | blob 64 MB + 预算 0 → `ours: Timeout`、manifest `impl=ours_failed` |
| `timeout-fallback` | 同上但 ours_with_fallback → 转交参考、manifest `impl=fallback` |
| `big-ours-ok`      | （超时判据的对照）同一个 64 MB blob、预算走默认 5 s → ours 成功 |

`big-ours-ok` 是 `timeout-ours` 的对照：有了它才能说「超时那条的失败是超预算造成的」，
而不是大 blob 本身就走不通。

为什么一个 case 一个进程
------------------------
- 转发层 dll 在 `DllMain` 里**清空** OUT_DIR / OPT_DIR，「加载之后新增了哪些行」只能从
  一次加载算起（上一轮的残留会错位）；
- ctypes 加载的 dll 卸不掉（`FreeLibrary` 也不保证），同一进程里换不了第二份壳；
- 参考产物里烘焙着 `compile_real.dll` 的加载基址，「逐字节相同」只在同一进程、同一个
  `compile_real.dll` 实例上才谈得上 —— 两份壳因此同放 `bin/`，各自的 `ensure_real` 都
  解析到那一份。

「改动前」的基线
----------------
`git show HEAD:tools/compile_hook.c` + 只把 5 个路径宏包一层 `#ifndef`（值逐字符不变）
编成第二份 dll。测试构建要换路径，而 HEAD 版把路径写死；只包一层 `#ifndef` 不可能改变
任何运行期行为，所以它仍是「改动前」的语义。

⚠ 工作区干净时 HEAD 与工作区同源，`refcmp` / `ours-ok` 退化成「自己比自己」。结果里的
`baseline_differs` 会如实报 False —— 别把它当成「参考不回归」的证据。

⚠ 不碰 `work/hook/` 与 `work/opt/`：路径宏全部 `-D` 到 `work/tmp/hook_offline/` 下。
`HOOK_LOG_DIR`（hook.log 所在的目录，工作区壳的默认值是 OUT_DIR = `work/hook/`）也 `-D`
到 case 的 `opt/` —— `snapshot()` 就是从那里读日志的。

用法
----
    python simplexc/tools/hook_offline.py                 # 先 build，再每个 case 一个子进程
    python simplexc/tools/hook_offline.py --list
    python simplexc/tools/hook_offline.py --case ours-missing
    python simplexc/tools/hook_offline.py --json work/tmp/hook_offline/last.json

退出码 0 = 全过。
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
ROOT = os.path.dirname(SIMPLEXC)                       # reversing/

WORK = os.path.join(ROOT, "work", "tmp", "hook_offline")
BIN = os.path.join(WORK, "bin")                        # 两份壳 + compile_real.dll 同居一处
RESULT_DIR = os.path.join(WORK, "results")
STUB = os.path.join(WORK, "tools", "rewrite_stub.py")
BASE_SRC = os.path.join(WORK, "compile_hook_base.c")
HOOK_SRC = os.path.join(ROOT, "tools", "compile_hook.c")
BASE_BLOB = os.path.join(ROOT, "build", "blob_empty.bin")
SRC = os.path.join(SIMPLEXC, "corpus", "src", "058.si")
GAME = r"F:\opt\SteamLibrary\steamapps\common\Turing Complete"
W64DEVKIT = os.environ.get("W64DEVKIT_BIN", r"D:\bin\w64devkit\bin")
GCC = os.path.join(W64DEVKIT, "gcc.exe")

BIG_MB = 64                       # 超时判据用的大 blob（在壳的上限之内）
CASE_TIMEOUT = 300.0              # 单个 case 子进程的超时秒数
DEFAULT_BUDGET = 5000             # 壳里 ours 段的默认预算（毫秒），用于人读输出

VARIANTS = {
    "new": {"root": os.path.join(WORK, "new"), "dll": os.path.join(BIN, "compile_hook_new.dll")},
    "base": {"root": os.path.join(WORK, "base"), "dll": os.path.join(BIN, "compile_hook_base.dll")},
}

# 替身改写器：为什么不用真的 tools/optimize_image.py，见下面这段自己的 docstring。
STUB_SOURCE = '''#!python3
"""转发层离线判据的**替身改写器**（由 `simplexc/tools/hook_offline.py` 落盘）。

为什么不用真的 `tools/optimize_image.py`：它会 `import hook_log`，而 `hook_log.PATH`
是按自身位置算的 —— 写死在 `work/opt/hook.log`。离线判据不该往那份活日志里掺行
（bench_rewrite 靠数它的新行），所以测试构建把 OPT_SCRIPT 指到这个替身：它只做
「等长搬运」（raw → opt，逐字节相同），不碰任何共享状态。

接口与真脚本一致：`--in <raw> --out <opt> [--entry <hex>] [--mode <值>]`；
不写 `<num>.entry.txt`（等长搬运没有新入口偏移，转发层于是不动 Result[2]）。
"""
import argparse
import shutil
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", dest="dst", required=True)
    ap.add_argument("--entry")
    ap.add_argument("--mode")
    args = ap.parse_args()
    shutil.copyfile(args.src, args.dst)
    print(f"stub rewrite: {args.src} -> {args.dst} (mode={args.mode})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''


def utf8_stdout():
    """控制台是 GBK（chcp 936），中文/→/≠ 会花掉 —— 与 tools/deploy_compile_hook.py 同一处理。

    标准输出与标准错误都换：失败原因走 stderr，它比摘要更需要能读。
    """
    for stream in (sys.stdout, sys.stderr):         # 报错信息比摘要更需要能读
        rec = getattr(stream, "reconfigure", None)
        if rec:
            with contextlib.suppress(Exception):
                rec(encoding="utf-8", errors="replace")


class BuildError(RuntimeError):
    """构建前置条件不满足（gcc / 游戏目录 / build/blob_empty.bin）：整批判据都用不了。"""


# ---- 目录与构建 ---------------------------------------------------------
def vdirs(v):
    root = VARIANTS[v]["root"]
    return {"root": root, "out": os.path.join(root, "out"), "opt": os.path.join(root, "opt"),
            "blob": os.path.join(root, "blob", "blob_test.bin"),
            "ini": os.path.join(root, "opt.ini")}


def defs(v):
    d = vdirs(v)
    return [
        f'-DOUT_DIR="{d["out"].replace(chr(92), "/")}"',
        f'-DOPT_DIR="{d["opt"].replace(chr(92), "/")}"',
        # 工作区壳默认把 hook.log 写进 OUT_DIR；pin 到 opt/ 才与 snapshot() 读的位置一致
        # （HEAD 基线壳本来就写 OPT_DIR = opt/，多一个 -D 对它无害）。
        f'-DHOOK_LOG_DIR="{d["opt"].replace(chr(92), "/")}"',
        f'-DOURS_BLOB="{d["blob"].replace(chr(92), "/")}"',
        f'-DOPT_INI="{d["ini"].replace(chr(92), "/")}"',
        f'-DOPT_SCRIPT="{STUB.replace(chr(92), "/")}"',
    ]


def head_source_bytes():
    """HEAD 的 `tools/compile_hook.c` 原字节。"""
    try:
        return subprocess.check_output(["git", "-C", ROOT, "show", "HEAD:tools/compile_hook.c"])
    except (OSError, subprocess.CalledProcessError) as e:
        raise BuildError(f"取不到 HEAD:tools/compile_hook.c（{e}）—— 基线 dll 编不出来") from e


def baseline_source_text():
    """HEAD 的 compile_hook.c + **纯机械**的路径宏 #ifndef 化（默认值逐字符不变）。"""
    text = head_source_bytes().decode("utf-8")
    names = {"OUT_DIR", "OPT_DIR", "OPT_SCRIPT", "OPT_INI", "OURS_BLOB"}
    out, wrapped = [], 0
    for ln in text.split("\n"):
        parts = ln.split()
        if ln.startswith("#define ") and len(parts) > 1 and parts[1] in names:
            out.append(f"#ifndef {parts[1]}")
            out.append(ln)
            out.append("#endif")
            wrapped += 1
        else:
            out.append(ln)
    if wrapped != 5:
        raise BuildError(f"机械补丁只该命中 5 个路径宏，实得 {wrapped} 个 —— HEAD 版壳的结构变了")
    return "\n".join(out)


def baseline_info():
    """HEAD 版壳与工作区是不是同一份（工作区干净时 refcmp/ours-ok 会退化成自比）。

    比较前把 CRLF 归一化：`core.autocrlf=true` 让 blob 存 LF、工作区是 CRLF，
    逐字节比会把「内容相同」误报成「改过」。
    """
    head = head_source_bytes().replace(b"\r\n", b"\n")
    with open(HOOK_SRC, "rb") as f:
        worktree = f.read().replace(b"\r\n", b"\n")
    return {"head_sha256": hashlib.sha256(head).hexdigest()[:16],
            "worktree_sha256": hashlib.sha256(worktree).hexdigest()[:16],
            "baseline_differs": head != worktree}


def build(verbose=True):
    """准备目录、落替身改写器、编两份 dll。返回结构化结果（ok / 命令 / rc / 基线信息）。"""
    t0 = time.perf_counter()
    if not os.path.isfile(GCC):
        raise BuildError(f"找不到 gcc：{GCC}（用 W64DEVKIT_BIN 覆盖）")
    if not os.path.isfile(BASE_BLOB) or not os.path.isfile(BASE_BLOB + ".entry"):
        raise BuildError(f"找不到 {BASE_BLOB}（情形 4 的真 blob）—— 先跑 simplexc/tools/build.py objs")
    for v in VARIANTS:
        d = vdirs(v)
        for key in ("out", "opt"):
            os.makedirs(d[key], exist_ok=True)
        os.makedirs(os.path.dirname(d["blob"]), exist_ok=True)
    os.makedirs(os.path.dirname(STUB), exist_ok=True)
    os.makedirs(BIN, exist_ok=True)
    with open(STUB, "w", encoding="utf-8", newline="\n") as f:
        f.write(STUB_SOURCE)
    for dep in ("compile_real.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"):
        src = os.path.join(GAME, dep)
        if not os.path.isfile(src):
            raise BuildError(f"游戏目录里没有 {dep}：{src}（Steam 更新后路径变了？）")
        shutil.copyfile(src, os.path.join(BIN, dep))
    with open(BASE_SRC, "w", encoding="utf-8", newline="") as f:
        f.write(baseline_source_text())

    commands, rcs = [], []
    for v, src in (("new", HOOK_SRC), ("base", BASE_SRC)):
        cmd = [GCC, "-shared", "-O2", "-static", "-o", VARIANTS[v]["dll"]] + defs(v) + [src]
        if verbose:
            print("$ " + " ".join(cmd), flush=True)
        rc = subprocess.call(cmd)
        if verbose:
            print(f"  rc={rc}", flush=True)
        commands.append(cmd)
        rcs.append(rc)
    res = {"ok": all(rc == 0 for rc in rcs), "seconds": round(time.perf_counter() - t0, 3),
           "commands": commands, "rc": rcs,
           "dll": {v: VARIANTS[v]["dll"] for v in VARIANTS},
           "workdir": WORK}
    res.update(baseline_info())
    return res


# ---- 调用与打印 ---------------------------------------------------------
class NimStr(ctypes.Structure):
    _fields_ = [("len", ctypes.c_ssize_t), ("ptr", ctypes.c_void_p)]


def load_dll(path):
    dll = ctypes.CDLL(path)
    dll.compile.restype = ctypes.c_void_p
    dll.compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    return dll


def load_and_snapshot(v):
    """**加载之后**再快照：DllMain 加载时会清 OUT_DIR / OPT_DIR，
    所以「新增行」必须从清完之后的空文件开始算（否则上一轮的残留会错位）。"""
    dll = load_dll(VARIANTS[v]["dll"])
    return dll, snapshot(v)


_KEEP = []


def call_compile(dll, source):
    keep = ctypes.create_string_buffer(8 + len(source) + 1)
    struct.pack_into("<q", keep, 0, len(source))
    keep[8:8 + len(source)] = source
    src = NimStr(len(source), ctypes.cast(keep, ctypes.c_void_p))
    out = ctypes.create_string_buffer(64)
    ctypes.memset(out, 0xCC, 64)
    ret = dll.compile(out, ctypes.byref(src), 0)
    _KEEP.append((keep, src, out))        # 缓冲必须活到进程结束（ctypes 不持有引用）
    return out.raw, ret


def unpack(raw):
    return struct.unpack_from("<5Q", raw, 0)


def err_bytes(raw):
    q = unpack(raw)
    if q[3] and q[4]:
        return ctypes.string_at(q[4], min(q[3], 4096))
    return b""


def product_bytes(raw):
    q = unpack(raw)
    if q[0] and q[1] and q[0] < (BIG_MB << 20):
        return ctypes.string_at(q[1], q[0])
    return None


def _lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return [ln.rstrip("\r\n") for ln in f if ln.strip()]
    except OSError:
        return []


def snapshot(v):
    d = vdirs(v)
    return {"manifest": _lines(os.path.join(d["out"], "manifest.tsv")),
            "log": _lines(os.path.join(d["opt"], "hook.log"))}


def manifest_rows(lines):
    """manifest.tsv 的数据行（第一行是表头）。壳在加载后新建文件，所以每次都是新表头。"""
    if not lines:
        return []
    hdr = lines[0].split("\t")
    return [dict(zip(hdr, ln.split("\t"), strict=False)) for ln in lines[1:]]


def log_records(lines):
    """hook.log 的 `时间 \\t 编号 \\t 事件 \\t key=value ...` 行。"""
    out = []
    for ln in lines:
        parts = ln.split("\t")
        if len(parts) < 3:
            continue
        kv = {}
        for item in parts[3:]:
            if "=" in item:
                k, val = item.split("=", 1)
                kv[k] = val
        out.append({"time": parts[0], "seq": parts[1], "event": parts[2], "kv": kv})
    return out


def find_log(records, event, **want):
    return [r for r in records
            if r["event"] == event and all(r["kv"].get(k) == val for k, val in want.items())]


def set_mode(v, mode, extra=""):
    d = vdirs(v)
    with open(d["ini"], "w", encoding="ascii") as f:
        f.write(f"[opt]\ncompiler = {mode}\n{extra}")


def set_blob(v, kind):
    """kind: 'none' | 'real' | 'big' —— 先清上一轮残留，再落一份。"""
    d = vdirs(v)
    for p in (d["blob"], d["blob"] + ".entry"):
        if os.path.exists(p):
            os.remove(p)
    if kind == "none":
        return
    if kind == "real":
        shutil.copyfile(BASE_BLOB, d["blob"])
        shutil.copyfile(BASE_BLOB + ".entry", d["blob"] + ".entry")
        return
    size = BIG_MB << 20
    with open(d["blob"], "wb") as f:
        f.write(struct.pack("<Q", size | (1 << 62)))
        f.write(b"\0" * (size - 8))
    with open(d["blob"] + ".entry", "w", encoding="ascii") as f:
        f.write("200")


def source_bytes():
    with open(SRC, "rb") as f:
        return f.read()


def cleanup_blob():
    """假 blob 是造出来的（最大那份 64 MB），整套跑完就删 —— 留着只占地盘。"""
    for v in VARIANTS:
        d = vdirs(v)
        for p in (d["blob"], d["blob"] + ".entry"):
            if os.path.exists(p):
                with contextlib.suppress(OSError):
                    os.remove(p)


class Case:
    """一个 case 的记录器：checks 决定通过与否，facts 只供人读与 JSON 汇总。"""

    def __init__(self, name, title):
        self.name = name
        self.title = title
        self.checks = OrderedDict()
        self.facts = OrderedDict()
        self.output = []

    def say(self, msg=""):
        print(msg, flush=True)
        self.output.append(msg)

    def check(self, key, ok, detail=""):
        ok = bool(ok)
        self.checks[key] = ok
        tail = f"：{detail}" if detail else ""
        self.say(f"  [{'OK  ' if ok else 'FAIL'}] {key}{tail}")
        return ok

    def fact(self, key, value):
        self.facts[key] = value
        return value

    @property
    def passed(self):
        return bool(self.checks) and all(self.checks.values())

    def to_dict(self):
        return {"name": self.name, "title": self.title, "passed": self.passed,
                "checks": dict(self.checks), "facts": dict(self.facts),
                "output": self.output}


def show_result(c, tag, raw, ret=None):
    q = unpack(raw)
    retxt = "NULL" if not ret else f"0x{ret:X}"
    c.say(f"  {tag}: out_len={q[0]} out_ptr=0x{q[1]:X} entry=0x{q[2]:X} "
          f"err_len={q[3]} err_ptr=0x{q[4]:X} 返回={retxt}")
    eb = err_bytes(raw)
    if eb:
        c.say(f"    失败串：{eb!r}")
    prod = product_bytes(raw)
    if prod is not None:
        hdr = struct.unpack_from("<Q", prod, 0)[0]
        c.say(f"    产物：头=0x{hdr:016X} bit62={(hdr >> 62) & 1} "
              f"sha256={hashlib.sha256(prod).hexdigest()[:16]}")
    return q, prod


def show_delta(c, v, before, tag=""):
    """打印并返回「加载之后新增」的 manifest 数据行与 hook.log 记录。"""
    now = snapshot(v)
    rows = manifest_rows(now["manifest"][len(before["manifest"]):])
    recs = log_records(now["log"][len(before["log"]):])
    c.say(f"  {tag}新增 manifest 数据行 {len(rows)}、hook.log 记录 {len(recs)}")
    for r in rows:
        c.say(f"    manifest: impl={r.get('impl')} out_len={r.get('out_len')} "
              f"r3={r.get('r3')} r4={r.get('r4')}")
    for r in recs:
        kv = " ".join(f"{k}={val}" for k, val in r["kv"].items())
        c.say(f"    log: {r['seq']} {r['event']} {kv}")
    return rows, recs


def error_records(recs):
    return [r for r in recs if r["event"] == "error"]


def last_impl(rows):
    return rows[-1].get("impl") if rows else "（没有新行）"


def note_baseline(c):
    """把「基线是否同源」写进 facts，退化时人读输出里也要看得见。"""
    info = baseline_info()
    c.fact("baseline_differs", info["baseline_differs"])
    if not info["baseline_differs"]:
        c.say("  ⚠ 工作区与 HEAD 同源：这一条退化成自己比自己（只有工作区改了壳才有判别力）")
    return info


# ---- cases --------------------------------------------------------------
def case_ours_missing(c):
    """情形 1：mode=ours + blob 缺失 → 失败串 / manifest impl=ours_failed / error+impl 两条日志。"""
    set_mode("new", "ours")
    set_blob("new", "none")
    os.environ.pop("TC_OURS_TIMEOUT_MS", None)
    dll, before = load_and_snapshot("new")
    raw, ret = call_compile(dll, source_bytes())
    q, _prod = show_result(c, "ours-missing", raw, ret)
    rows, recs = show_delta(c, "new", before)
    eb = err_bytes(raw)
    c.fact("err_text", eb.decode("utf-8", "replace"))
    c.check("out_len_zero", q[0] == 0, f"out_len={q[0]}")
    c.check("err_len_nonzero", q[3] != 0, f"err_len={q[3]}")
    c.check("err_text_internal_error", eb.startswith(b"ours: ") and b"InternalError" in eb, repr(eb))
    c.check("manifest_impl_ours_failed", bool(rows) and rows[-1].get("impl") == "ours_failed",
            f"impl={last_impl(rows)}")
    c.check("log_error_stage_ours",
            bool(find_log(recs, "error", stage="ours", kind="InternalError")))
    c.check("log_impl_ours_failed", bool(find_log(recs, "impl", impl="ours_failed")))


def case_fallback_missing(c):
    """情形 2：mode=ours_with_fallback + blob 缺失 → 整单转交参考实现（产物逐字节相同）。"""
    set_blob("new", "none")
    os.environ.pop("TC_OURS_TIMEOUT_MS", None)
    # 先把这份源码的参考产物在**同一进程、同一个 compile_real.dll 实例**里拿一份作对照
    set_mode("new", "reference")
    dll, before = load_and_snapshot("new")
    raw_ref, ret_ref = call_compile(dll, source_bytes())
    q_ref, prod_ref = show_result(c, "reference（先跑，作对照）", raw_ref, ret_ref)
    show_delta(c, "new", before, "reference ")
    # 再换成 ours_with_fallback：ours 拿不到 blob → 应当整单转交参考
    set_mode("new", "ours_with_fallback")
    raw_fb, ret_fb = call_compile(dll, source_bytes())
    q_fb, prod_fb = show_result(c, "ours_with_fallback（blob 缺失）", raw_fb, ret_fb)
    rows, recs = show_delta(c, "new", before, "fallback ")
    len_ref = None if prod_ref is None else len(prod_ref)
    len_fb = None if prod_fb is None else len(prod_fb)
    c.check("reference_ok", q_ref[3] == 0 and prod_ref is not None,
            f"参考产物 err_len={q_ref[3]}")
    c.check("fallback_err_len_zero", q_fb[3] == 0, f"err_len={q_fb[3]}")
    c.check("fallback_product_equals_reference", prod_fb is not None and prod_fb == prod_ref,
            f"参考长度 {len_ref} / 回退长度 {len_fb}")
    c.check("manifest_impl_fallback", bool(rows) and rows[-1].get("impl") == "fallback",
            f"impl={last_impl(rows)}")
    c.check("log_error_stage_ours", bool(find_log(recs, "error", stage="ours")))
    c.check("log_impl_fallback", bool(find_log(recs, "impl", impl="fallback",
                                              mode="ours_with_fallback")))


def case_ref(c):
    """情形 3：mode=reference → manifest impl=reference / hook.log impl 行（回退率的分母）。"""
    set_mode("new", "reference")
    dll, before = load_and_snapshot("new")
    raw, ret = call_compile(dll, source_bytes())
    q, prod = show_result(c, "reference", raw, ret)
    rows, recs = show_delta(c, "new", before)
    bit62 = ((struct.unpack_from("<Q", prod, 0)[0] >> 62) & 1) if prod else -1
    c.check("err_len_zero", q[3] == 0, f"err_len={q[3]}")
    c.check("product_not_empty", prod is not None and q[0] > 0, f"out_len={q[0]}")
    c.check("product_not_ours", bit62 == 0, f"产物头 bit62={bit62}（我们给游戏的缓冲才置位）")
    c.check("manifest_impl_reference", bool(rows) and rows[-1].get("impl") == "reference",
            f"impl={last_impl(rows)}")
    c.check("log_impl_reference", bool(find_log(recs, "impl", impl="reference")))


def case_ours_ok(c):
    """情形 4：mode=ours + 真 blob → 与改动前一致、无 error 行。"""
    set_blob("new", "real")
    set_blob("base", "real")
    os.environ.pop("TC_OURS_TIMEOUT_MS", None)
    set_mode("new", "ours")
    set_mode("base", "ours")
    note_baseline(c)
    dll_new, before_new = load_and_snapshot("new")
    raw_new, ret_new = call_compile(dll_new, source_bytes())
    q_new, prod_new = show_result(c, "new(ours)", raw_new, ret_new)
    rows_new, recs_new = show_delta(c, "new", before_new, "new ")
    dll_base, before_base = load_and_snapshot("base")
    raw_base, ret_base = call_compile(dll_base, source_bytes())
    q_base, prod_base = show_result(c, "base(改动前, ours)", raw_base, ret_base)
    show_delta(c, "base", before_base, "base ")
    c.fact("out_len", q_new[0])
    c.fact("product_sha256", hashlib.sha256(prod_new or b"").hexdigest()[:16])
    c.check("err_len_zero_both", q_new[3] == 0 and q_base[3] == 0,
            f"new={q_new[3]} base={q_base[3]}")
    c.check("out_len_same", q_new[0] == q_base[0], f"new={q_new[0]} base={q_base[0]}")
    c.check("entry_same", q_new[2] == q_base[2], f"new=0x{q_new[2]:X} base=0x{q_base[2]:X}")
    c.check("product_bytes_same", prod_new is not None and prod_new == prod_base)
    c.check("manifest_impl_ours", bool(rows_new) and rows_new[-1].get("impl") == "ours",
            f"impl={last_impl(rows_new)}")
    c.check("no_error_log", not error_records(recs_new),
            f"{len(error_records(recs_new))} 条 error")


def case_refcmp(c):
    """reference 不回归：改动前 vs 改动后，同一进程、同一个 compile_real.dll 实例。"""
    set_mode("new", "reference")
    set_mode("base", "reference")
    note_baseline(c)
    dll_new, before_new = load_and_snapshot("new")
    raw_new, ret_new = call_compile(dll_new, source_bytes())
    q_new, prod_new = show_result(c, "new", raw_new, ret_new)
    show_delta(c, "new", before_new, "new ")
    dll_base, before_base = load_and_snapshot("base")
    raw_base, ret_base = call_compile(dll_base, source_bytes())
    q_base, prod_base = show_result(c, "base", raw_base, ret_base)
    show_delta(c, "base", before_base, "base ")
    sha_new = hashlib.sha256(prod_new or b"").hexdigest()
    sha_base = hashlib.sha256(prod_base or b"").hexdigest()
    c.fact("sha256_new", sha_new[:16])
    c.fact("sha256_base", sha_base[:16])
    c.check("both_products", prod_new is not None and prod_base is not None)
    c.check("err_len_zero_both", q_new[3] == 0 and q_base[3] == 0,
            f"new={q_new[3]} base={q_base[3]}")
    c.check("byte_identical", prod_new is not None and prod_new == prod_base)


def _case_timeout(c, mode, budget):
    """超时预算：blob 64 MB + 预算 budget（None = 走壳的默认 5000）。"""
    set_blob("new", "big")
    if budget is None:
        os.environ.pop("TC_OURS_TIMEOUT_MS", None)
    else:
        os.environ["TC_OURS_TIMEOUT_MS"] = str(budget)
    set_mode("new", mode)
    dll, before = load_and_snapshot("new")
    raw, ret = call_compile(dll, source_bytes())
    q, _prod = show_result(c, f"timeout({mode})", raw, ret)
    rows, recs = show_delta(c, "new", before)
    c.fact("mode", mode)
    c.fact("budget_ms", f"默认 {DEFAULT_BUDGET}" if budget is None else budget)
    c.fact("blob_mb", BIG_MB)
    eb = err_bytes(raw)
    c.fact("err_text", eb.decode("utf-8", "replace"))
    c.check("err_len_zero" if mode != "ours" else "err_len_nonzero",
            (q[3] == 0) if mode != "ours" else (q[3] != 0), f"err_len={q[3]}")
    if mode == "ours":
        c.check("err_text_timeout", b"Timeout" in eb, repr(eb))
        c.check("manifest_impl_ours_failed", bool(rows) and rows[-1].get("impl") == "ours_failed",
                f"impl={last_impl(rows)}")
        c.check("log_impl_ours_failed", bool(find_log(recs, "impl", impl="ours_failed")))
    else:
        c.check("out_len_nonzero", q[0] > 0, f"out_len={q[0]}")
        c.check("manifest_impl_fallback", bool(rows) and rows[-1].get("impl") == "fallback",
                f"impl={last_impl(rows)}")
        c.check("log_impl_fallback", bool(find_log(recs, "impl", impl="fallback")))
    c.check("log_error_ours_timeout",
            bool(find_log(recs, "error", stage="ours_timeout", kind="Timeout")))


def case_timeout_ours(c):
    """timeout-ours：mode=ours + 预算 0 → 超时失败（不是悄悄成功）。"""
    _case_timeout(c, "ours", 0)


def case_timeout_fallback(c):
    """timeout-fallback：mode=ours_with_fallback + 预算 0 → 超时后整单转交参考。"""
    _case_timeout(c, "ours_with_fallback", 0)


def case_big_ours_ok(c):
    """big-ours-ok：同一个 64 MB blob、预算走默认 5 s → ours 应当成功（超时判据的对照）。"""
    set_blob("new", "big")
    os.environ.pop("TC_OURS_TIMEOUT_MS", None)
    set_mode("new", "ours")
    dll, before = load_and_snapshot("new")
    raw, ret = call_compile(dll, source_bytes())
    q, _prod = show_result(c, "big-ours-ok", raw, ret)
    rows, recs = show_delta(c, "new", before)
    c.fact("mode", "ours")
    c.fact("budget_ms", f"默认 {DEFAULT_BUDGET}")
    c.fact("blob_mb", BIG_MB)
    c.check("err_len_zero", q[3] == 0, f"err_len={q[3]}")
    c.check("out_len_is_blob", q[0] == (BIG_MB << 20), f"out_len={q[0]}（blob {BIG_MB << 20} 字节）")
    c.check("manifest_impl_ours", bool(rows) and rows[-1].get("impl") == "ours",
            f"impl={last_impl(rows)}")
    c.check("no_error_log", not error_records(recs),
            f"{len(error_records(recs))} 条 error")


CASES = OrderedDict([
    ("ours-missing", ("情形 1：mode=ours + blob 缺失 → 失败串 + impl=ours_failed + error/impl 两条日志",
                      case_ours_missing)),
    ("fallback-missing", ("情形 2：ours_with_fallback + blob 缺失 → 转交参考、产物逐字节相同",
                          case_fallback_missing)),
    ("ref", ("情形 3：mode=reference → manifest impl=reference + hook.log 的 impl 行（分母）",
             case_ref)),
    ("ours-ok", ("情形 4：mode=ours + 真 blob → 与改动前一致、无 error 行", case_ours_ok)),
    ("refcmp", ("reference 不回归：新壳与改动前的产物逐字节相同", case_refcmp)),
    ("timeout-ours", ("超时：mode=ours + 预算 0 → ours: Timeout / impl=ours_failed",
                      case_timeout_ours)),
    ("timeout-fallback", ("超时：ours_with_fallback + 预算 0 → 转交参考 / impl=fallback",
                          case_timeout_fallback)),
    ("big-ours-ok", ("对照：64 MB blob + 默认预算 5 s → ours 成功", case_big_ours_ok)),
])


def run_case(name):
    """在本进程里跑一个 case。加载 dll 会清目录，所以一个进程只能跑一个。"""
    title, fn = CASES[name]
    c = Case(name, title)
    c.say(f"== [{name}] {title}")
    try:
        fn(c)
    except Exception as e:                              # 器材本身出错也是「这一条没过」
        c.say(f"  [FAIL] 异常：{type(e).__name__}: {e}")
        c.checks["no_exception"] = False
    bad = [k for k, ok in c.checks.items() if not ok]
    c.say("  → " + ("通过" if c.passed else "失败：" + "、".join(bad)))
    return c


def _child_env():
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"                   # 控制台是 GBK，子进程输出按 UTF-8 回来
    env.pop("TC_OURS_TIMEOUT_MS", None)                 # 由 case 自己决定，别从调用者继承
    return env


def run_all(only=None, *, verbose=False, timeout=CASE_TIMEOUT):
    """跑一整套（或 only 指定的几个）。返回结构化结果，供 check.py 或人读摘要使用。"""
    names = list(CASES) if not only else list(only)
    unknown = [n for n in names if n not in CASES]
    if unknown:
        raise KeyError(f"未知 case：{'、'.join(unknown)}（可用：{'、'.join(CASES)}）")
    t0 = time.perf_counter()
    res = {"schema": 1, "workdir": WORK, "seconds": 0.0, "ok": False,
           "build": None, "cases": [], "failed": [], "skipped": []}
    try:
        b = build(verbose=verbose)
    except BuildError as e:
        res["build"] = {"ok": False, "error": str(e)}
        res["skipped"] = names
        res["seconds"] = round(time.perf_counter() - t0, 3)
        if verbose:
            print(f"构建失败：{e}", flush=True)
        return res
    res["build"] = b
    if not b["ok"]:
        res["skipped"] = names                         # dll 编不出来 → 全部没法跑
        res["seconds"] = round(time.perf_counter() - t0, 3)
        return res

    os.makedirs(RESULT_DIR, exist_ok=True)
    for name in names:
        path = os.path.join(RESULT_DIR, name + ".json")
        if os.path.exists(path):
            os.remove(path)
        cmd = [sys.executable, os.path.abspath(__file__), "--child", name, "--result", path]
        t1 = time.perf_counter()
        rc, out = None, ""
        try:
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=timeout,
                                  env=_child_env())
            rc = proc.returncode
            out = ((proc.stdout or "") + (proc.stderr or "")).strip()
        except subprocess.TimeoutExpired:
            out = f"case {name} 超时（{timeout:.0f} s）"
        entry = {"name": name, "title": CASES[name][0], "rc": rc,
                 "seconds": round(time.perf_counter() - t1, 3),
                 "passed": False, "checks": {}, "facts": {}, "output_tail": []}
        if rc == 0 and os.path.isfile(path):
            try:
                with open(path, encoding="utf-8") as f:
                    got = json.load(f)
                entry.update({"passed": bool(got["passed"]), "checks": got["checks"],
                              "facts": got["facts"]})
            except (OSError, ValueError, KeyError) as e:
                out += f"\n读不到 case 结果 {path}：{e}"
        entry["output_tail"] = out.splitlines()[-30:]
        if verbose and out:
            print(out, flush=True)
        if verbose:
            verdict = "通过" if entry["passed"] else "失败"
            print(f"  子进程 rc={rc}  {entry['seconds']:.2f}s  → {verdict}", flush=True)
        res["cases"].append(entry)
    failed = [c["name"] for c in res["cases"] if not c["passed"]]
    res["failed"] = failed
    res["ok"] = bool(res["cases"]) and not failed
    res["seconds"] = round(time.perf_counter() - t0, 3)
    cleanup_blob()
    return res


# ---- CLI ----------------------------------------------------------------
def _dump(path, payload):
    """只落盘不打印 —— 子进程要静默（它的输出会被父进程当摘要收走）。"""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def _write_json(path, payload):
    _dump(path, payload)
    print(f"JSON → {path}")


def main(argv=None):
    utf8_stdout()
    ap = argparse.ArgumentParser(
        description="壳的离线判据：四种情形 + refcmp + 超时（工作目录 work/tmp/hook_offline/）")
    ap.add_argument("--list", action="store_true", help="列出 case 名")
    ap.add_argument("--case", default=None, help="只跑一个 case（本进程内）")
    ap.add_argument("--json", default=None, help="把结构化结果写到这个 JSON")
    ap.add_argument("--timeout", type=float, default=CASE_TIMEOUT, help="单个 case 子进程的超时秒数")
    ap.add_argument("--child", default=None, help=argparse.SUPPRESS)     # run_all 内部用
    ap.add_argument("--result", default=None, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    if args.list:
        for name, (title, _fn) in CASES.items():
            print(f"{name:<16} {title}")
        return 0

    if args.child:
        c = run_case(args.child)
        if args.result:
            _dump(args.result, c.to_dict())
        return 0 if c.passed else 1

    if args.case:
        if args.case not in CASES:
            print(f"未知 case：{args.case}（可用：{'、'.join(CASES)}）", file=sys.stderr)
            return 2
        try:
            b = build(verbose=True)
        except BuildError as e:
            print(f"构建失败：{e}", file=sys.stderr)
            return 1
        if not b["ok"]:
            print("dll 编译失败，判据跑不了", file=sys.stderr)
            return 1
        c = run_case(args.case)
        if args.json:
            _write_json(args.json, c.to_dict())
        return 0 if c.passed else 1

    res = run_all(verbose=True, timeout=args.timeout)
    n_ok = sum(1 for c in res["cases"] if c["passed"])
    print(f"\n{len(CASES)} 个 case：{n_ok} 通过 · {len(res['failed'])} 失败 · "
          f"{len(res['skipped'])} 未跑（构建/编译失败）  用时 {res['seconds']:.2f}s")
    if res["failed"]:
        print("失败：" + "、".join(res["failed"]))
    if not res["build"].get("ok"):
        detail = res["build"].get("error") or f"rc={res['build'].get('rc')}"
        print(f"构建：{detail}")
    if res["build"].get("baseline_differs") is False:
        print("⚠ 工作区与 HEAD 的 compile_hook.c 同源 → refcmp / ours-ok 是自比"
              "（判别力只对未提交的改动有效）")
    if args.json:
        _write_json(args.json, res)
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
