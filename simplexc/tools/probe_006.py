#!python3
"""M1.5 收益探针：`006` 的 clang -O2 等价物 vs 参考产物（一条命令跑完整条链）。

跑什么
------
    1. `simplexc/.deps/llvm/bin/clang.exe -O2` 编 `simplexc/probe/006_equiv.c` → COFF（**不许 g++ 顶替**）
    2. `build/tcblob.exe` 装配（→ `<blob>` / `<blob>.entry` / `<blob>.stubs`）
    3. host 跑参考产物 `simplexc/corpus/ref/006.bin`
    4. host 跑我们的产物，逐 tick 比 pages / 状态字段 / extern 序列
    5. 吞吐：宿主正常驱动（每 tick 墙钟）+ 每 cycle 成本（fresh host 单次 `tick(N)` 差分）
    6. 静态：两边机器码的指令条数与字节长度
结果 JSON/TSV 落 `simplexc/probe/results/`。

为什么每 cycle 成本要另算
------------------------
宿主把 `sim_target_cycle` 依次设成 1..N，所以**每个 tick 只推进 1 个 cycle** ——
「每 tick 墙钟」量到的基本是宿主自己的轮询粒度，不是电路内层环。真正要比的是
「一次 run 里跑 N 个 cycle」的成本：

    T(N) = c + a·k(N) + b·N

    k(N) = 爆发次数（`burst_cycles` 从 1 每次 +1 → k(k+1)/2 ≥ N+1，cycle 初值 -1）
    a    = 每次爆发固定的开销（参考产物那边还含宿主把时钟换成 ctypes 回调的代价）
    b    = **每 cycle 的机器码成本** ← 要报的就是它
    c    = 每 tick 固定开销（宿主轮询 + 进程内状态）

`c` 被差分消掉，`a` 与 `b` 用四个 N 的最小二乘解出来（原始数据与残差都写进结果文件）。

用法
----
    python simplexc/tools/probe_006.py                     # 默认 fold 变体
    python simplexc/tools/probe_006.py --variant nofold     # 电路状态变量加 volatile（禁折叠）
    python simplexc/tools/probe_006.py --rounds 5 --ticks 10 --quiet
    python simplexc/tools/probe_006.py --keep-tmp

`--variant nofold` 编的是**同一份源码**加 `-DTC_006_NO_FOLD`：它让 clang 不能把「输入恒 0」
的电路体常量折叠掉，于是两边做的是「同样的每周期工作量、不同代码生成质量」——
fold 变体的收益里有一部分来自常量折叠，两档数字合起来才看得清收益的来源。
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)
# LLVM 落点与 binutils 路径不写死（迁过一次目录）：`tools/tc_common.py` 是唯一一处定义。
# 把**仓库根**塞进 sys.path 再走 `tools.` 前缀 —— 与 hook_offline.py 同一拼法。
if REPO not in sys.path:
    sys.path.insert(0, REPO)
from tools.tc_common import llvm_root, tool  # noqa: E402

BUILD = os.path.join(REPO, "build")
HOST_PY = os.path.join(SIMPLEXC, "tools", "host", "host.py")
SRC_SI = os.path.join(SIMPLEXC, "corpus", "src", "006.si")
REF_BIN = os.path.join(SIMPLEXC, "corpus", "ref", "006.bin")
EQUIV_C = os.path.join(SIMPLEXC, "probe", "006_equiv.c")
# 必须是 LLVM 资产里的 clang（**不许 g++ / 系统 clang 顶替**）：编的正是「LLVM -O2 能
# 把这份电路折成什么样」这个结论，换编译器结论就不可比了。
CLANG = str(llvm_root() / "bin" / "clang.exe")
TCBLOB = os.path.join(BUILD, "tcblob.exe")
ENTRY_SHIM = os.path.join(BUILD, "objs", "entry_shim.o")
RT_MIN = os.path.join(BUILD, "objs", "rt_min.o")
WORK = os.path.join(REPO, "work", "tmp", "m15")
RESULTS = os.path.join(SIMPLEXC, "probe", "results")
OBJDUMP = tool("objdump")

# 差分白名单：{页地址（JSONL 里的十六进制小写）: 为什么可以不算差异}
# 006 的实测结论是**空的** —— 逐 tick 全等，连一个页都不需要列白。
PAGE_WHITELIST: dict[str, str] = {}
# 逐 tick 单次测量的目标 cycle 数（四个点解三个未知量 c / a / b）
# 为什么要这么大：宿主把时钟/Sleep 换成了 Python 回调，回调要抢 GIL → **每次回调**要付
# 好几微秒；一次 run 里回调次数 k ≈ sqrt(2N)。N 太小时 b·N 与 a·k 同量级、甚至被 a·k 淹没，
# b 就解不出来。取到 10 亿个 cycle 时每 cycle 摊销的 a·k/N 只有 0.3 ns，b 才浮得出来。
TARGETS = (1_000_000, 10_000_000, 100_000_000, 1_000_000_000)


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


def run(cmd, cwd=REPO, timeout=1800):
    """跑一条命令，返回 (rc, stdout, stderr)。命令与输出都进结果文件（可审计）。"""
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout)
    return p.returncode, p.stdout, p.stderr


# ---- 1/2. 编译 + 装配 -------------------------------------------------------
def build_ours(variant, outdir):
    if not os.path.isfile(CLANG):
        raise SystemExit(f"找不到 clang：{CLANG}（解包：python simplexc/tools/fetch_llvm.py "
                         f"--offline --tag llvmorg-23.1.3 --extra bin/clang.exe）")
    os.makedirs(outdir, exist_ok=True)
    obj = os.path.join(outdir, f"006_equiv_{variant}.o")
    blob = os.path.join(outdir, f"006_ours_{variant}.bin")
    cflags = ["--target=x86_64-pc-windows-gnu", "-O2", "-ffreestanding",
              "-fno-stack-protector", "-c"]
    if variant == "nofold":
        cflags.append("-DTC_006_NO_FOLD")
    elif variant == "hot":
        cflags.append("-DTC_006_HOT_CYCLE")
    compile_cmd = [CLANG, *cflags, "-o", obj, EQUIV_C]
    rc, out, err = run(compile_cmd)
    if rc != 0:
        raise SystemExit(f"clang 编译失败 rc={rc}\n{out}\n{err}")
    asm_cmd = [TCBLOB, "--obj", obj, "--obj", ENTRY_SHIM, "--obj", RT_MIN,
               "--entry", "__tc_entry", "--out", blob]
    rc2, out2, err2 = run(asm_cmd)
    if rc2 != 0:
        raise SystemExit(f"tcblob 装配失败 rc={rc2}\n{out2}\n{err2}")
    with open(blob + ".entry", encoding="utf-8") as f:
        entry = int(f.read().strip(), 16)
    stubs_path = blob + ".stubs"
    stubs = []
    if os.path.isfile(stubs_path):
        with open(stubs_path, encoding="utf-8") as f:
            for line in f.read().splitlines()[1:]:
                if line.strip():
                    off, kind, sym = line.split("\t")
                    stubs.append({"offset": off, "kind": kind, "symbol": sym})
    return {"obj": obj, "blob": blob, "entry": entry, "stubs": stubs,
            "compile_cmd": compile_cmd, "assemble_cmd": asm_cmd,
            "assemble_stdout": out2.strip()}


# ---- 3/4. 两遍 host + 逐 tick 比较 -----------------------------------------
def run_host_cli(bin_path, entry, ticks, out_jsonl):
    """用 host.py 的 CLI 跑一遍（与 selftest 同一条路径），返回 (rc, stderr, wall)。"""
    cmd = [sys.executable, HOST_PY, "--src", SRC_SI, "--bin", bin_path,
           "--ticks", str(ticks), "--out", out_jsonl, "--quiet"]
    if entry is not None:
        cmd += ["--entry", hex(entry)]
    t0 = time.perf_counter()
    rc, _out, err = run(cmd, timeout=max(600, ticks * 30))
    return rc, err.strip(), time.perf_counter() - t0, cmd


def read_jsonl(path):
    snaps = []
    if not os.path.isfile(path):
        return snaps
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                snaps.append(json.loads(line))
    return snaps


def compare_ticks(ref, ours, whitelist):
    """逐 tick 比「状态页 + 状态字段 + extern 序列」。

    主判据是**状态**：`pages`（页哈希）与 settings/SIMULATION_STATE 里那几个字段。
    publisher 允许的白名单只有 `whitelist` 里逐条写明的页 —— 列表为空就是「一个页都不许差」。
    """
    diffs, whitelisted = [], []
    if len(ref) != len(ours):
        diffs.append(f"tick 数不同：参考 {len(ref)}、我们 {len(ours)}")
    fields = ("sim_cycle", "sim_target_cycle", "sim_last_command_id", "sim_running",
              "sim_test_result", "reason", "stabilized")
    for i in range(min(len(ref), len(ours))):
        x, y = ref[i], ours[i]
        for p, h in x["pages"].items():
            if y["pages"].get(p) == h:
                continue
            item = {"tick": i + 1, "page": p, "ref": h, "ours": y["pages"].get(p)}
            (whitelisted if p in whitelist else diffs).append(
                {**item, "why": whitelist.get(p, "")} if p in whitelist else item)
        for p in y["pages"]:
            if p not in x["pages"]:
                item = {"tick": i + 1, "page": p, "ref": None, "ours": y["pages"][p]}
                (whitelisted if p in whitelist else diffs).append(
                    {**item, "why": whitelist.get(p, "")} if p in whitelist else item)
        for f in fields:
            if x[f] != y[f]:
                diffs.append({"tick": i + 1, "field": f, "ref": x[f], "ours": y[f]})
        if len(x["extern"]) != len(y["extern"]):
            diffs.append({"tick": i + 1, "extern_count": [len(x["extern"]), len(y["extern"])]})
        else:
            for j, (ex, ey) in enumerate(zip(x["extern"], y["extern"], strict=False)):
                if ex != ey:
                    diffs.append({"tick": i + 1, "extern": j, "ref": ex, "ours": ey})
    return diffs, whitelisted


# ---- 5. 吞吐 ---------------------------------------------------------------
def burst_count(target):
    """一次 run 从 cycle=-1 跑到 target 要多少次爆发（burst_cycles 每次 +1）。"""
    cycle, burst, k = -1, 1, 0
    while cycle < target and k < 10_000_000:
        cycle = min(target, cycle + burst)
        burst += 1
        k += 1
    return k


def spin_tick(h, target, budget=120.0):
    """与 `Host.tick` 同一套内存操作，只把「每 0.5 ms 采一次」换成紧等。

    为什么不直接用 `Host.tick`：它的轮询粒度是 0.5 ms，而这里要量的信号本身只有
    几十微秒~几毫秒 —— 那样量到的全是宿主自己的 sleep。仍然要求**连续两次采样一致**
    （程序写 `sim_last_command_id` 与置 `sim_running=1` 之间有个极短窗口形状相同），
    采样间隔下限 20 µs，那个窗口跨不过去。
    """
    cmd_id = h.cmd_id + 1
    rd_u64, wr_u64 = hostmod.rd_u64, hostmod.wr_u64
    wr_u64(h.state + h.ST["sim_target_cycle"] * 8, target)
    wr_u64(h.cmd_block + h.CTL["ctl_command"] * 8, h.CMDV["run"])
    wr_u64(h.cmd_block + h.CTL["ctl_command_id"] * 8, cmd_id)
    h.cmd_id = cmd_id
    last_field = h.state + h.ST["sim_last_command_id"] * 8
    running_field = h.state + h.ST["sim_running"] * 8

    t0 = time.perf_counter()
    stable, reason, last_poll = 0, "ok", t0
    while True:
        if not h.alive():
            reason = "thread_exit"
            break
        done = rd_u64(last_field) == cmd_id and rd_u64(running_field) == 0
        stable = stable + 1 if done else 0
        if stable >= 2:
            break
        if time.perf_counter() - t0 > budget:
            reason = "timeout"
            break
        deadline = last_poll + 2e-5
        while (now := time.perf_counter()) < deadline:
            pass
        last_poll = now
    dt = time.perf_counter() - t0
    return dt, {"reason": reason,
                "sim_cycle": rd_u64(h.state + h.ST["sim_cycle"] * 8)}


def worker(a):
    """量测子进程：**每次量测一个新进程**（VEH/线程/分配都只装一次）。"""
    global hostmod
    # ⚠ 宿主把产物的时钟/Sleep 换成了 Python 回调，而回调要抢 GIL；本进程主线程正在
    # 紧等目标周期（占着 GIL）→ 默认 5 ms 的切换间隔会把**每次回调**拖到几百微秒。
    # 实测：参考产物 63 次回调 = 12 ms（占了它整个 run 的时间）。调小到 10 µs 就只黍一点。
    sys.setswitchinterval(1e-6)
    hostmod = _load(HOST_PY, "hostmod")
    simspec = hostmod.simspec
    spec = simspec.SimSpec.parse(SRC_SI)
    with open(a.bin, "rb") as f:
        blob = f.read()
    entry = int(a.entry, 0)
    h = hostmod.Host(spec, blob, entry, verbose=False,
                     stub_path=hostmod.sidecar_path_for(a.bin))
    hostmod.install_veh(lambda: h.code_alloc, lambda: h.dll_base)
    h.setup()
    out = {"bin": a.bin, "entry": entry, "target": a.target, "kind": "target"}
    try:
        if a.target:
            dt, info = spin_tick(h, a.target)
            out.update({"seconds": dt, "bursts": burst_count(a.target), **info})
        else:
            times = []
            for n in range(1, a.ticks + 1):
                t = time.perf_counter()
                snap = h.tick(n)
                times.append(time.perf_counter() - t)
                if snap["reason"] != "ok":
                    out["last_reason"] = snap["reason"]
                    break
            out.update({"kind": "ticks", "tick_seconds": times,
                        "ticks": len(times)})
        out["pacing"] = dict(h.detours.pacing)      # 节奏类计数（诊断用：谁在空转一目了然）
        out["extern_records"] = len(h.detours.records)
    finally:
        out["close"] = h.close()
    print(json.dumps(out))
    return 0


def measure_worker(bin_path, entry, target, ticks):
    cmd = [sys.executable, os.path.abspath(__file__), "--worker",
           "--bin", bin_path, "--entry", hex(entry)]
    if target:
        cmd += ["--target", str(target)]
    else:
        cmd += ["--ticks", str(ticks)]
    rc, out, err = run(cmd)
    if rc != 0 or not out.strip():
        raise SystemExit(f"量测子进程失败 rc={rc}\n{err[-2000:]}")
    return json.loads(out.strip().splitlines()[-1])


def fit_cycle_cost(points):
    """最小二乘解 T = c + a·k + b·N（points: [(N, k, T_seconds)]）→ (a,b,c,resid,ns)。"""
    rows = [(k, n, 1.0, t) for n, k, t in points]        # a·k + b·N + c·1 = T
    # 正规方程 3x3
    mat = [[sum(r[i] * r[j] for r in rows) for j in range(3)] for i in range(3)]
    rhs = [float(sum(r[i] * r[3] for r in rows)) for i in range(3)]
    # 高斯消元（3x3，无主元选取也够用）
    for i in range(3):
        p = max(range(i, 3), key=lambda r: abs(mat[r][i]))
        if abs(mat[p][i]) < 1e-18:
            return None
        mat[i], mat[p] = mat[p], mat[i]
        rhs[i], rhs[p] = rhs[p], rhs[i]
        for r in range(i + 1, 3):
            f = mat[r][i] / mat[i][i]
            for c in range(i, 3):
                mat[r][c] -= f * mat[i][c]
            rhs[r] -= f * rhs[i]
    sol = [0.0, 0.0, 0.0]
    for i in (2, 1, 0):
        sol[i] = (rhs[i] - sum(mat[i][j] * sol[j] for j in range(i + 1, 3))) / mat[i][i]
    a, b, c = sol
    resid = [t - (a * k + b * n + c) for n, k, t in points]
    n_max, _k_max, t_max = points[-1]
    return {"a_ns_per_burst": a * 1e9, "b_ns_per_cycle": b * 1e9, "c_ns_per_tick": c * 1e9,
            "residual_ns": [r * 1e9 for r in resid],
            # 最大那个点的「总时间/cycle」：含每爆发开销，是 b 的上界（句柄：两者差太远说明模型不成立）
            "largest_point_ns_per_cycle": t_max / n_max * 1e9}


# ---- 6. 静态指标 -----------------------------------------------------------
def static_metrics(blob_path, tag, outdir):
    with open(blob_path, "rb") as f:
        data = f.read()
    code = data[8:]
    codefile = os.path.join(outdir, f"{tag}.code")
    with open(codefile, "wb") as f:
        f.write(code)
    cmd = [OBJDUMP, "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "intel", codefile]
    rc, out, err = run(cmd)
    if rc != 0:
        return {"error": f"objdump rc={rc}", "stderr": err[-500:]}
    ins = 0
    nbytes = 0
    import re
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f ]+)\t", line)
        if m:
            ins += 1
            nbytes += len(m.group(2).split())
    return {"bytes": len(data), "code_bytes": len(code),
            "out_len_in_header": int.from_bytes(data[:8], "little") & ((1 << 62) - 1),
            "instructions": ins, "decoded_bytes": nbytes,
            "objdump_cmd": " ".join(cmd)}


# ---- main ------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="M1.5 收益探针（006：clang -O2 等价物 vs 参考产物）")
    ap.add_argument("--variant", choices=("fold", "nofold", "hot"), default="fold",
                    help="fold=默认；nofold=电路状态加 volatile（禁折叠）；"
                         "hot=把每周期计数器拿到寄存器（语义等价，见 006_equiv.c）")
    ap.add_argument("--ticks", type=int, default=10, help="逐 tick 比较的 tick 数（≥10）")
    ap.add_argument("--rounds", type=int, default=5, help="吞吐量测轮数（取中位数）")
    ap.add_argument("--outdir", default=None, help="中间产物目录（默认 work/tmp/m15）")
    ap.add_argument("--results", default=RESULTS)
    ap.add_argument("--no-throughput", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    # 内部：量测子进程
    ap.add_argument("--worker", action="store_true")
    ap.add_argument("--bin", default=None)
    ap.add_argument("--entry", default=None)
    ap.add_argument("--target", type=int, default=0)
    a = ap.parse_args(argv)
    if a.worker:
        return worker(a)

    def say(*args):
        if not a.quiet:
            print(*args, flush=True)

    outdir = a.outdir or WORK
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(a.results, exist_ok=True)
    ref_entry = _load(HOST_PY, "hostmod").entry_from_manifest(REF_BIN)   # corpus/manifest.tsv
    report = {"variant": a.variant, "ticks": a.ticks, "rounds": a.rounds,
              "src_si": os.path.relpath(SRC_SI, REPO), "ref_bin": os.path.relpath(REF_BIN, REPO),
              "page_whitelist": PAGE_WHITELIST, "when": time.strftime("%Y-%m-%d %H:%M:%S")}

    # 0. 工具链凭据
    rc, out, _err = run([CLANG, "--version"])
    report["clang_version"] = out.strip().splitlines()[0] if rc == 0 else f"rc={rc}"
    report["clang_sha256"] = sha256_of(CLANG)
    report["ref_sha256"] = sha256_of(REF_BIN)

    # 1/2. 编译 + 装配
    ours = build_ours(a.variant, outdir)
    say(f"[probe] clang+tcblob 完成：{os.path.relpath(ours['blob'], REPO)} "
        f"entry=0x{ours['entry']:X} stubs={len(ours['stubs'])}")
    report["ours"] = dict(ours)
    report["ours"]["obj_sha256"] = sha256_of(ours["obj"])
    report["ours"]["blob_sha256"] = sha256_of(ours["blob"])

    # 3/4. 两遍 host + 逐 tick 比较
    ref_jsonl = os.path.join(a.results, f"006_{a.variant}.ref.jsonl")
    ours_jsonl = os.path.join(a.results, f"006_{a.variant}.ours.jsonl")
    rc_ref, err_ref, wall_ref, cmd_ref = run_host_cli(REF_BIN, None, a.ticks, ref_jsonl)
    rc_ours, err_ours, wall_ours, cmd_ours = run_host_cli(ours["blob"], ours["entry"], a.ticks,
                                                          ours_jsonl)
    ref_snaps, ours_snaps = read_jsonl(ref_jsonl), read_jsonl(ours_jsonl)
    diffs, whitelisted = compare_ticks(ref_snaps, ours_snaps, PAGE_WHITELIST)
    report["tick_compare"] = {
        "host_rc": [rc_ref, rc_ours],
        "ticks": [len(ref_snaps), len(ours_snaps)],
        "identical": not diffs,
        "semantic_diffs": diffs,
        "whitelisted": whitelisted,
        "pages_per_tick": [len(s["pages"]) for s in ref_snaps],
        "extern_per_tick": [len(s["extern"]) for s in ref_snaps],
        "commands": [" ".join(cmd_ref), " ".join(cmd_ours)],
        "jsonl": [os.path.relpath(ref_jsonl, REPO), os.path.relpath(ours_jsonl, REPO)],
        "host_stderr_tail": [err_ref.splitlines()[-3:] if err_ref else [],
                             err_ours.splitlines()[-3:] if err_ours else []],
    }
    say(f"[probe] 逐 tick 比较：{len(ref_snaps)}/{a.ticks} tick，差异 {len(diffs)} 条，"
        f"已列白 {len(whitelisted)} 条 → {'全等' if not diffs else '有差别（见结果文件）'}")

    # 5. 吞吐
    if not a.no_throughput:
        sides = {"ref": (REF_BIN, ref_entry), "ours": (ours["blob"], ours["entry"])}
        report["throughput"] = {}
        for name, (bpath, bentry) in sides.items():
            ticks_runs = [measure_worker(bpath, bentry, 0, a.ticks) for _ in range(a.rounds)]
            per_tick = sorted(t for r in ticks_runs for t in r["tick_seconds"])
            med_tick = per_tick[len(per_tick) // 2]
            points = []
            for n in TARGETS:
                runs = [measure_worker(bpath, bentry, n, 0) for _ in range(a.rounds)]
                times = sorted(r["seconds"] for r in runs)
                points.append((n, burst_count(n), times[len(times) // 2]))
                report["throughput"].setdefault(name + "_raw", []).append(
                    {"target": n, "seconds_median": times[len(times) // 2],
                     "seconds_all": times, "pacing": runs[0].get("pacing"),
                     "sim_cycle": runs[0].get("sim_cycle"), "reason": runs[0].get("reason")})
            fit = fit_cycle_cost(points)
            if fit is None:
                raise SystemExit(f"吞吐拟合失败（{name} 的三个未知量解不出来）")
            report["throughput"][name] = {
                "per_tick_seconds_median": med_tick,
                "per_tick_rounds": ticks_runs,
                "points": [{"target": n, "bursts": k, "seconds": t} for n, k, t in points],
                "fit": fit,
            }
            say(f"[probe] {name}: 每 tick 中位数 {med_tick * 1e6:.1f} µs；"
                f"拟合每 cycle {fit['b_ns_per_cycle']:.2f} ns（a {fit['a_ns_per_burst']:.0f} ns/爆发，"
                f"b·N≪a·k 时 a/b 会互分，看 paired）；最大点 T/N "
                f"{fit['largest_point_ns_per_cycle']:.2f} ns/cycle")
        b_ref = report["throughput"]["ref"]["fit"]["b_ns_per_cycle"]
        b_ours = report["throughput"]["ours"]["fit"]["b_ns_per_cycle"]
        # 配对差：两边的爆发次数 k 本就相同（同一个 target）→ 每次爆发的开销 a 在差里**直接消掉**，
        # 比拟合出来的 b 更稳（拟合里 a 与 b 会互相分配，取决于噪声）。
        points_ours = report["throughput"]["ours"]["points"]
        points_ref = report["throughput"]["ref"]["points"]
        n_big = points_ours[-1]["target"]
        t_ours = points_ours[-1]["seconds"]
        t_ref = points_ref[-1]["seconds"]
        report["throughput"]["ratio"] = {
            # ⚠ 拟合出来的 b 在 b·N ≪ a·k 时会与 a 互相分配（实测热变体解出 0.12 ns/cycle，
            # 物理上不可能：一圈至少 1 个周期 ≈ 0.3 ns）→ 只作参考。
            # **配对差与 T/N 才是稳健的**：两边的 k 本就相同，a·k 在差里直接消掉。
            "fit_ours_over_ref_percent": (b_ours / b_ref * 100.0) if b_ref else None,
            "fit_b_note": "a/b 互相分配，b 只作参考；见 paired",
            "paired": {
                "target": n_big,
                "ns_per_cycle_ref": t_ref / n_big * 1e9,
                "ns_per_cycle_ours": t_ours / n_big * 1e9,
                "delta_ns_per_cycle": (t_ours - t_ref) / n_big * 1e9,
                "ours_over_ref_percent": t_ours / t_ref * 100.0,
                "speedup_ours_over_ref": t_ref / t_ours,
                "how": "同一 N、同一个 k → a·k 抵消；(T_ours − T_ref)/N",
            },
            "per_tick_ours_over_ref_percent":
                report["throughput"]["ours"]["per_tick_seconds_median"]
                / report["throughput"]["ref"]["per_tick_seconds_median"] * 100.0,
        }

    # 6. 静态
    report["static"] = {
        "ref": static_metrics(REF_BIN, "006_ref", outdir),
        "ours": static_metrics(ours["blob"], f"006_ours_{a.variant}", outdir),
    }

    jpath = os.path.join(a.results, f"006_{a.variant}.json")
    with open(jpath, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    say(f"[probe] 结果 → {os.path.relpath(jpath, REPO)}")
    if report.get("throughput", {}).get("points") or report.get("throughput"):
        tsv = os.path.join(a.results, f"006_{a.variant}.tsv")
        with open(tsv, "w", encoding="utf-8") as f:
            f.write("side\ttarget\tbursts\tseconds\tns_per_cycle\tns_per_burst\tns_per_tick\n")
            for name in ("ref", "ours"):
                tp = report["throughput"][name]
                fit = tp["fit"]
                for p in tp["points"]:
                    f.write(f"{name}\t{p['target']}\t{p['bursts']}\t{p['seconds']:.9f}\t"
                            f"{fit['b_ns_per_cycle']:.3f}\t{fit['a_ns_per_burst']:.1f}\t"
                            f"{fit['c_ns_per_tick']:.1f}\n")
        say(f"[probe] 吞吐表 → {os.path.relpath(tsv, REPO)}")
    return 0 if not report["tick_compare"]["semantic_diffs"] else 1


if __name__ == "__main__":
    sys.exit(main())
