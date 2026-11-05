#!python3
"""同会话闭环：一份捕获源码 → 一个可部署的 LLVM 产物（地址按**当前游戏会话**提取）。

一条命令做完五步
----------------
    1. `gen_addrs.py`：从 `--src` 抽出本会话的 11 个绝对地址 → `<workdir>/addrs.h`
    2. `simplexc/.deps/llvm/bin/clang.exe -O2` 编 `simplexc/probe/equiv_cc_and.c`（**不许 g++ 顶替**）
    3. `simplexc/build/tcblob.exe` 装配 → `<workdir>/ours.bin`（+ `.entry` + `.stubs`）
    4. host 跑参考产物（`--bin`）与我们的产物 → 逐 tick 比 pages / sim_cycle / extern
    5. 静态：两边机器码的指令条数与字节数
    结论打印出来，明细 JSON 落 `<workdir>/equiv_result.json`，两份快照 JSONL 也留在那儿。

为什么必须一条命令
------------------
生成源码里的游戏内存地址**按游戏进程实例变**（`docs/pitfalls.md` 最后一条）：拿上一次会话的产物
喂新进程 = 往未映射地址写。所以「抓源码 → 生成地址头 → 编产物 → 部署」必须在同一次会话内完成，
中间隔一次人工抄写就过期。

`--mark`
--------
额外编一份 `-DTC_MARK` 的（`ours.bin.marked`）：它进入 `tc_main` 时往固定路径追加一行，
用来在**游戏内**确认「我们的产物真的跑到了」。标记会进 extern 序列，所以它**不参与**逐 tick 比较；
本命令只做一件事 —— 在宿主里跑一遍，确认标记路径被执行（宿主记下 `CreateFileA`/`WriteFile` 记录；
宿主的 `CreateFileA` 桩恒返回 INVALID_HANDLE_VALUE，所以**不会**真的落盘，见 `host.py` 的 Detours）。

比较口径
--------
直接复用 `tools/host/selftest.py` 的 `compare`（pages / sim_cycle / extern 条数与逐条内容）——
判据只有一处定义。`PAGE_WHITELIST` 为空（006 那条路就是空的）；若哪天要列白，必须逐条写理由，
它们会原样进结果文件，并且**不**算差异。

用法
----
    python simplexc/tools/equiv_build.py --src work/hook/hook_src_040.si \
        --bin work/hook/hook_out_040.bin --entry 0x212F --workdir work/tmp/equiv --mark

`--entry` 可省：省略时从 `--bin` 同目录的 `manifest.tsv` 查（corpus 那套表叫 `entry` 列，
捕获那套表里是 `r2` 列 —— 同一件事：产物内锚点偏移）。地址变了这条命令一个参数都不用改。
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)
# LLVM 落点与 binutils 路径不写死（迁过一次目录）：`libs/tc_common.py` 是唯一一处定义。
# 把**仓库根**塞进 sys.path 再走 `libs.` 前缀 —— 与 hook_offline.py 同一拼法。
if REPO not in sys.path:
    sys.path.insert(0, REPO)
from libs.tc_common import llvm_root, tool  # noqa: E402

BUILD = os.path.join(SIMPLEXC, "build")
# 必须是 LLVM 资产里的 clang（**不许 g++ / 系统 clang 顶替**）：产物要与前面记的
# 代码生成质量对得上，换编译器就不是同一件事了。
CLANG = str(llvm_root() / "bin" / "clang.exe")
TCBLOB = os.path.join(BUILD, "tcblob.exe")
ENTRY_SHIM = os.path.join(BUILD, "objs", "entry_shim.o")
RT_MIN = os.path.join(BUILD, "objs", "rt_min.o")
EQUIV_C = os.path.join(SIMPLEXC, "probe", "equiv_cc_and.c")
GEN_ADDRS = os.path.join(HERE, "gen_addrs.py")
HOST_PY = os.path.join(HERE, "host", "host.py")
SELFTEST_PY = os.path.join(HERE, "host", "selftest.py")
# 环境变量优先（换调试用的 objdump 时不用改代码），否则按 PATH 找
OBJDUMP = os.environ.get("OBJDUMP") or tool("objdump")

# 差分白名单：{页地址（JSONL 里的十六进制小写）: 为什么可以不算差异}。
# 040 期望是**空的**；列白必须逐条写理由，且会原样进结果文件。
PAGE_WHITELIST: dict[str, str] = {}

# 标记版在宿主里应当被记下的三个调用（顺序即写入路径）
MARK_SYMBOLS = ("CreateFileA", "WriteFile", "CloseHandle")
MARK_LOG = os.path.join(REPO, "work", "tmp", "equiv", "equiv_cc_and.log")


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def rel(path):
    """报告里的路径尽量短；工作目录外的走绝对路径。"""
    try:
        r = os.path.relpath(path, REPO)
    except ValueError:
        return path
    return path if r.startswith("..") else r


def run(cmd, cwd=REPO, timeout=1800):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def force_utf8_stdout():
    """本机 stdout 默认是 gbk：输出经管道读回时中文会乱码 —— 判据输出要能直接贴。"""
    rec = getattr(sys.stdout, "reconfigure", None)
    if rec is not None:
        rec(encoding="utf-8", errors="replace")


# ---- 0. entry --------------------------------------------------------------
def resolve_entry(bin_path, explicit):
    """`--entry` 优先；否则从 `--bin` 同目录的 manifest.tsv 查（entry 列或捕获表的 r2 列）。"""
    if explicit is not None:
        return int(explicit, 0)
    manifest = os.path.join(os.path.dirname(bin_path), "manifest.tsv")
    if not os.path.isfile(manifest):
        raise SystemExit(f"没有 --entry，且 {manifest} 不存在 —— 入口偏移只能从这两处来")
    with open(manifest, encoding="utf-8") as f:
        rows = [ln.rstrip("\n").split("\t") for ln in f if ln.strip()]
    head, stem = rows[0], os.path.splitext(os.path.basename(bin_path))[0]
    cols = [c for c in ("entry", "r2") if c in head]
    if not cols:
        raise SystemExit(f"{manifest} 里既没有 entry 列也没有 r2 列")
    ci = head.index(cols[0])
    for row in rows[1:]:
        if any(os.path.splitext(os.path.basename(c))[0] == stem for c in row):
            return int(row[ci], 0)
    raise SystemExit(f"{os.path.basename(bin_path)} 在 {manifest} 里找不到（--entry 请显式给）")


# ---- 1/2/3. 地址头 + 编译 + 装配 -------------------------------------------
def gen_addrs(src, workdir, say):
    header = os.path.join(workdir, "addrs.h")
    cmd = [sys.executable, GEN_ADDRS, "--src", src, "-o", header]
    rc, out, err = run(cmd)
    if rc != 0:
        raise SystemExit(f"gen_addrs 失败 rc={rc}\n{out}\n{err}")
    say(f"[equiv] 地址头：{rel(header)}（{len(out.strip().splitlines()) - 1} 个地址）")
    return {"path": header, "command": " ".join(cmd), "stdout": out.strip().splitlines()}


def compile_obj(obj, extra_flags, workdir, say):
    if not os.path.isfile(CLANG):
        raise SystemExit(f"找不到 clang：{CLANG}（解包：python simplexc/tools/fetch_llvm.py --offline "
                         f"--tag llvmorg-23.1.3 --extra bin/clang.exe）")
    cmd = [CLANG, "--target=x86_64-pc-windows-gnu", "-O2", "-ffreestanding",
           "-fno-stack-protector", "-I", workdir, *extra_flags, "-c", "-o", obj, EQUIV_C]
    rc, out, err = run(cmd)
    if rc != 0:
        raise SystemExit(f"clang 编译失败 rc={rc}\n{out}\n{err}")
    say(f"[equiv] 编译：{rel(obj)}")
    return {"obj": obj, "compile_cmd": " ".join(cmd)}


def assemble(obj, blob):
    cmd = [TCBLOB, "--obj", obj, "--obj", ENTRY_SHIM, "--obj", RT_MIN,
           "--entry", "__tc_entry", "--out", blob]
    rc, out, err = run(cmd)
    if rc != 0:
        raise SystemExit(f"tcblob 装配失败 rc={rc}\n{out}\n{err}")
    with open(blob + ".entry", encoding="ascii") as f:
        entry = int(f.read().strip(), 16)
    stubs = []
    stubs_path = blob + ".stubs"
    stubs_text = ""
    if os.path.isfile(stubs_path):
        with open(stubs_path, encoding="utf-8") as f:
            stubs_text = f.read()
        for line in stubs_text.splitlines()[1:]:
            if line.strip():
                off, kind, sym = line.split("\t")
                stubs.append({"offset": off, "kind": kind, "symbol": sym})
    offs = [s["offset"] for s in stubs]
    dup = sorted({o for o in offs if offs.count(o) > 1})
    if dup:
        raise SystemExit(
            f"装配器写出的 {rel(stubs_path)} 自相矛盾：偏移 {dup} 各被两条记录占用。\n"
            "  已知成因（`assemble/assemble.cc` 的 `place_for`）：`__imp_X` 数据槽的地址在「遇到第一个引用」\n"
            "  时就算好了，之后再往代码桩块里追加会把已记下的槽地址挤歪 —— 既写歪 sidecar，也写歪产物里\n"
            "  的 `mov rax,[rip+槽]`。绕法：别让 `__imp_*` 与裸名 extern 混用（见 probe/equiv_cc_and.c）。")
    return {"blob": blob, "entry": entry, "stubs": stubs, "stubs_text": stubs_text.strip(),
            "assemble_cmd": " ".join(cmd), "assemble_stdout": out.strip()}


def build_variant(name, workdir, extra_flags, say):
    obj = os.path.join(workdir, f"equiv_cc_and{name}.o")
    blob = os.path.join(workdir, f"ours{name}.bin")
    info = compile_obj(obj, extra_flags, workdir, say)
    info.update(assemble(obj, blob))
    info["obj_sha256"] = sha256_of(obj)
    info["blob_sha256"] = sha256_of(blob)
    return info


# ---- 4. 两遍 host + 逐 tick 比较 -------------------------------------------
def run_host(src, bin_path, entry, ticks, out_jsonl, extra=None):
    cmd = [sys.executable, HOST_PY, "--src", src, "--bin", bin_path, "--ticks", str(ticks),
           "--out", out_jsonl, "--quiet"]
    if entry is not None:
        cmd += ["--entry", hex(entry)]
    if extra:
        cmd += extra
    rc, _out, err = run(cmd, timeout=max(300, ticks * 60))
    return rc, err.strip(), cmd


def read_jsonl(path):
    snaps = []
    if not os.path.isfile(path):
        return snaps
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                snaps.append(json.loads(line))
    return snaps


def observe_mark(src, bin_path, entry):
    """子进程跑一遍标记版，把**线程启动阶段**的 extern 记录拿回来。

    为什么不能用 CLI 的 tick 快照：快照里的 `extern` 只含「这一个 tick 窗口内」的调用，
    而标记发生在产物线程刚起来、还没收到第一个 `run` 命令之前 —— 它落不进任何 tick 窗口。
    所以这里直接起宿主（与 `probe_006.py --worker` 同一套做法，且**必须**另起进程：宿主的 VEH
    在异常时 `TerminateProcess`，留在本进程里会把 equiv_build 自己带走）。
    """
    cmd = [sys.executable, os.path.abspath(__file__), "--mark-probe-worker",
           "--src", src, "--bin", bin_path, "--entry", hex(entry)]
    rc, out, err = run(cmd, timeout=300)
    if rc != 0 or not out.strip():
        return {"error": f"观察子进程 rc={rc}", "stderr_tail": err.strip().splitlines()[-4:]}
    return json.loads(out.strip().splitlines()[-1])


def mark_probe_worker(argv=None):
    ap = argparse.ArgumentParser(description="（内部）观察标记版的线程启动阶段 extern 记录")
    ap.add_argument("--src", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--entry", required=True)
    ap.add_argument("--mark", action="store_true", help="占位：与主命令共用参数集")
    ap.add_argument("--mark-probe-worker", action="store_true", help="（内部）在子进程里跑观察")
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--ticks", type=int, default=0)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    hostmod = _load(HOST_PY, "hostmod")
    spec = hostmod.simspec.SimSpec.parse(os.path.abspath(a.src))
    with open(os.path.abspath(a.bin), "rb") as f:
        blob = f.read()
    entry = int(a.entry, 0)
    h = hostmod.Host(spec, blob, entry, verbose=False,
                     stub_path=hostmod.sidecar_path_for(os.path.abspath(a.bin)))
    hostmod.install_veh(lambda: h.code_alloc, lambda: h.dll_base)
    h.setup()
    out = {"entry": entry, "records": [], "alive": None, "close": None}
    try:
        deadline = time.time() + 10.0
        while time.time() < deadline and len(h.detours.records) < len(MARK_SYMBOLS):
            time.sleep(0.005)
        out["alive"] = h.alive()
        # 必须拷一份：`close()` 还会再追加一条 `ExitThread`（程序根收尾），而这份记录是在收尾**之前**取的
        out["records"] = [dict(r) for r in h.detours.records]
        out["pacing"] = dict(h.detours.pacing)
    finally:
        out["close"] = h.close()
    print(json.dumps(out))
    return 0


def compare_ticks(ref, ours, selftest):
    """判据就是 `selftest.compare`（pages / sim_cycle / extern）。

    白名单在这里生效：先把已列白的页从两边快照里摘掉，再交给同一个比较器 ——
    这样「比较口径」只有一处定义；列白命中了哪些页另行记账，写进结果文件。
    """
    def strip(snaps):
        return [{**s, "pages": {p: h for p, h in s["pages"].items() if p not in PAGE_WHITELIST}}
                for s in snaps]

    whitelisted = [{"tick": i + 1, "page": p, "ref": x["pages"][p], "ours": y["pages"].get(p),
                    "why": PAGE_WHITELIST[p]}
                   for i, (x, y) in enumerate(zip(ref, ours, strict=False))
                   for p in x["pages"] if p in PAGE_WHITELIST and y["pages"].get(p) != x["pages"][p]]
    diffs = selftest.compare(strip(ref), strip(ours))
    return diffs, whitelisted


# ---- 5. 静态指标 -----------------------------------------------------------
def static_metrics(blob_path, tag, outdir):
    with open(blob_path, "rb") as f:
        data = f.read()
    code = data[8:]
    if not os.path.isfile(OBJDUMP):
        return {"error": f"找不到 objdump：{OBJDUMP}", "bytes": len(data)}
    codefile = os.path.join(outdir, f"{tag}.code")
    with open(codefile, "wb") as f:
        f.write(code)
    rc, out, err = run([OBJDUMP, "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel", codefile])
    if rc != 0:
        return {"error": f"objdump rc={rc}", "stderr": err[-500:], "bytes": len(data)}
    instructions = decoded = 0
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f ]+)\t", line)
        if m:
            instructions += 1
            decoded += len(m.group(2).split())
    return {"bytes": len(data), "code_bytes": len(code),
            "out_len_in_header": int.from_bytes(data[:8], "little") & ((1 << 62) - 1),
            "instructions": instructions, "decoded_bytes": decoded}


# ---- main ------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="同会话闭环：捕获源码 → 地址头 → clang → 装配 → 与参考产物逐 tick 对账")
    ap.add_argument("--src", required=True, help="捕获的源码（例如 work/hook/hook_src_040.si）")
    ap.add_argument("--bin", required=True, help="同一份源码的参考产物（例如 work/hook/hook_out_040.bin）")
    ap.add_argument("--entry", default=None,
                    help="参考产物的入口偏移（省略时从 --bin 同目录 manifest.tsv 查）")
    ap.add_argument("--workdir", default=os.path.join(REPO, "work", "tmp", "equiv"),
                    help="中间产物与结果目录（默认 work/tmp/equiv）")
    ap.add_argument("--ticks", type=int, default=10, help="逐 tick 比较的 tick 数（≥10）")
    ap.add_argument("--mark", action="store_true",
                    help="额外编一份 -DTC_MARK 的（.marked，供游戏内验证；不参与逐 tick 比较）")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--mark-probe-worker", action="store_true",
                    help="（内部）观察标记版的线程启动阶段 extern 记录，主命令自己会拉起")
    a = ap.parse_args(argv)
    if a.mark_probe_worker:
        return mark_probe_worker(argv)
    force_utf8_stdout()

    def say(*args):
        if not a.quiet:
            print(*args, flush=True)

    src = os.path.abspath(a.src)
    ref_bin = os.path.abspath(a.bin)
    workdir = os.path.abspath(a.workdir)
    for path in (src, ref_bin):
        if not os.path.isfile(path):
            raise SystemExit(f"找不到 {path}")
    if a.ticks < 10:
        raise SystemExit(f"--ticks 至少 10（{a.ticks} 不够看出逐 tick 行为）")
    os.makedirs(workdir, exist_ok=True)
    os.makedirs(MARK_LOG.rsplit(os.sep, 1)[0], exist_ok=True)

    selftest = _load(SELFTEST_PY, "selftest")
    ref_entry = resolve_entry(ref_bin, a.entry)
    report = {
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        "src": rel(src), "src_sha256": sha256_of(src),
        "ref_bin": rel(ref_bin), "ref_bin_sha256": sha256_of(ref_bin), "ref_entry": ref_entry,
        "ticks": a.ticks, "page_whitelist": PAGE_WHITELIST,
        "clang_version": run([CLANG, "--version"])[1].strip().splitlines()[:1],
    }
    say(f"[equiv] 参考产物 {rel(ref_bin)} entry=0x{ref_entry:X} sha256={report['ref_bin_sha256'][:16]}…"
        f"（{'--entry' if a.entry else '从同目录 manifest.tsv 查得'}）")

    # 1/2/3. 地址头 → 编译 → 装配
    report["addrs"] = gen_addrs(src, workdir, say)
    with open(report["addrs"]["path"], encoding="utf-8") as f:
        report["addrs"]["header"] = f.read()
    ours = build_variant("", workdir, [], say)
    if not ours["stubs"]:
        say("[equiv] ⚠ 产物没有 extern 跳板 —— sleep/时钟那两个 extern 应该至少留几处桩")
    say(f"[equiv] 装配：{rel(ours['blob'])} sha256={ours['blob_sha256']} entry=0x{ours['entry']:X} "
        f"extern 跳板 {len(ours['stubs'])} 处")
    say(f"[equiv] .entry = 0x{ours['entry']:X}")
    say("[equiv] .stubs:")
    for line in ours["stubs_text"].splitlines():
        say("    " + line)

    # 4. 两遍 host + 逐 tick 比较
    ref_jsonl = os.path.join(workdir, "ref.jsonl")
    ours_jsonl = os.path.join(workdir, "ours.jsonl")
    rc_ref, err_ref, cmd_ref = run_host(src, ref_bin, ref_entry, a.ticks, ref_jsonl)
    rc_ours, err_ours, cmd_ours = run_host(src, ours["blob"], ours["entry"], a.ticks, ours_jsonl)
    ref_snaps, ours_snaps = read_jsonl(ref_jsonl), read_jsonl(ours_jsonl)
    diffs, whitelisted = compare_ticks(ref_snaps, ours_snaps, selftest)
    tick_ok = not diffs and len(ref_snaps) == a.ticks and len(ours_snaps) == a.ticks
    report["tick_compare"] = {
        "host_rc": [rc_ref, rc_ours],
        "ticks": [len(ref_snaps), len(ours_snaps)],
        "identical": not diffs,
        "semantic_diffs": diffs,
        "whitelisted": whitelisted,
        "pages_per_tick": [len(s["pages"]) for s in ref_snaps],
        "extern_per_tick": [len(s["extern"]) for s in ref_snaps],
        "commands": [" ".join(cmd_ref), " ".join(cmd_ours)],
        "jsonl": [rel(ref_jsonl), rel(ours_jsonl)],
        "host_stderr_tail": [err_ref.splitlines()[-3:] if err_ref else [],
                             err_ours.splitlines()[-3:] if err_ours else []],
    }
    say(f"[equiv] 逐 tick：{len(ref_snaps)}/{len(ours_snaps)} tick（要 {a.ticks}），"
        f"差异 {len(diffs)} 条，白名单 {len(whitelisted)} 条 → "
        f"{'全等' if tick_ok else '不一致（见结果文件）'}")
    for d in diffs[:8]:
        say("    " + str(d))

    # 5. --mark：另一个版本，只验证标记路径被执行
    if a.mark:
        if os.path.isfile(MARK_LOG):
            os.remove(MARK_LOG)                 # 旧痕迹会让「这次有没有写」说不清
        marked = build_variant("_marked", workdir, ["-DTC_MARK"], say)
        marked_jsonl = os.path.join(workdir, "marked.jsonl")
        rc_m, err_m, _cmd_m = run_host(src, marked["blob"], marked["entry"], 2, marked_jsonl)
        ticks_m = len(read_jsonl(marked_jsonl))
        obs = observe_mark(src, marked["blob"], marked["entry"])
        calls = obs.get("records", [])
        names = [c["name"] for c in calls]
        wrote = next((c for c in calls if c["name"] == "WriteFile"), None)
        mark_ok = (rc_m == 0 and ticks_m == 2 and names == list(MARK_SYMBOLS)
                   and wrote is not None and wrote["args"][1] > 16)
        report["mark"] = {
            "blob": rel(marked["blob"]), "blob_sha256": marked["blob_sha256"],
            "entry": marked["entry"], "stubs": marked["stubs"], "stubs_text": marked["stubs_text"],
            "compile_cmd": marked["compile_cmd"],
            "host_rc": rc_m, "host_ticks": ticks_m,
            "startup_calls": names, "startup_records": calls, "observe": obs,
            "log_path": rel(MARK_LOG), "log_exists": os.path.isfile(MARK_LOG),
            "compared_per_tick": False,
            "why_not_compared": "标记是额外的 extern 调用，必然进 extern 序列 → 不可能与参考产物逐 tick 全等",
            "ok": mark_ok,
        }
        say(f"[equiv] 标记版：{rel(marked['blob'])} sha256={marked['blob_sha256'][:16]}… "
            f"宿主 {ticks_m} tick rc={rc_m}（驱动协议正常）；启动阶段 extern 记录 {names}")
        if wrote is not None:
            data = wrote["args"][2].to_bytes(8, "little")
            say(f"[equiv] 标记内容：WriteFile 写了 {wrote['args'][1]} 字节，头 8 字节 = "
                f"{data!r}")
        say(f"[equiv] 标记路径 {rel(MARK_LOG)} 存在={'是' if os.path.isfile(MARK_LOG) else '否'}"
            "（宿主的 CreateFileA 桩恒返回 INVALID_HANDLE_VALUE → 不会真落盘；游戏内会写出真正的一行）")
        if not mark_ok:
            say(f"    ⚠ 标记路径没跑通：rc={rc_m} ticks={ticks_m} 记录={names}")
            if obs.get("error"):
                say("    " + obs["error"])
            for line in obs.get("stderr_tail") or []:
                say("    " + line)

    # 6. 静态
    report["static"] = {
        "ref": static_metrics(ref_bin, "cc_and_ref", workdir),
        "ours": static_metrics(ours["blob"], "cc_and_ours", workdir),
    }
    rep, our = report["static"]["ref"], report["static"]["ours"]
    if "error" not in rep and "error" not in our:
        say(f"[equiv] 静态：参考 {rep['instructions']} 条指令 / {rep['bytes']} B"
            f"（机器码 {rep['code_bytes']} B）；我们 {our['instructions']} 条 / {our['bytes']} B"
            f"（机器码 {our['code_bytes']} B）")
    else:
        say(f"[equiv] 静态指标不可用：{[v.get('error') for v in (rep, our)]}")

    report["ours"] = {k: ours[k] for k in ("blob", "blob_sha256", "entry", "obj", "obj_sha256",
                                          "compile_cmd", "assemble_cmd", "stubs_text")}
    report["ours"]["blob"] = rel(ours["blob"])
    report["ours"]["obj"] = rel(ours["obj"])
    report["passed"] = tick_ok and (report.get("mark", {}).get("ok", True))
    jpath = os.path.join(workdir, "equiv_result.json")
    with open(jpath, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    say(f"[equiv] 结果 → {rel(jpath)}；结论：{'通过' if report['passed'] else '未通过'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
