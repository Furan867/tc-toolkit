#!python3
"""一条命令跑完整的**离线**判据（主设计 §7.5 的「一条命令」入口）。

步骤表
------
| 名字 | 跑什么 |
|---|---|
| `corpus`        | `python simplexc/tools/recompile_corpus.py --check`（M0：语料/manifest 一致 + 同源三条断言）|
| `host_selftest` | `python simplexc/tools/host/selftest.py --src …006.si --bin …006.bin --ticks 3`（M2 宿主自比对）|
| `lower_equiv`   | `simplexc/build/simplexc_cli.exe`（**C++ 产品路径**）出 rv32im 产物 → 宿主与参考逐 tick 比对（10 tick，灌 RESET_DATA）|
| `blob_baseline` | `simplexc/tools/blob_baseline.py --verify simplexc/tools/baseline`（98 份语料的装配产物逐字段比，重构前后必须等价）|
| `debug_info_equiv` | G1：177.si 开/关 `--debug-info` 各出一份 COFF，`objdump -d` 只比 `.text`，diff 必须为空 |
| `hook_offline`  | `hook_offline.run_all()`：壳的四种情形 + refcmp + 超时（case 数见摘要 cases=N/M）|
| `probe_006`     | `simplexc/tools/probe_006.py`（M1.5 探针，**可选**：缺依赖/脚本在改 → SKIP）|

游戏内的部分（部署转发层、主频采样）一律 manual/slow，不在这里 —— 判据里任何一步都不
应该要求游戏在跑。

状态约定
--------
- `PASS` = 这一步的 rc 为 0（或步骤自己报了通过）；
- `FAIL` = rc≠0 或步骤自己报失败 —— 决定整体退出码；
- `SKIP` = 前置条件不满足（例如 `probe_006` 依赖的 clang 还没解出来、或它正被别人改）。
  **SKIP 既不是 PASS 也不是 FAIL**：既不能当通过，也不该把别人的半成品算成回归。

退出码：0 = 没有 FAIL（可以带 SKIP）；1 = 有 FAIL；2 = 参数或环境错误（未知步骤名等）。
打印顺序固定是「人读摘要在前」；机器读的 JSON 由 `--json` 落盘（run 结束才写）。

一步失败不影响其它步：每步独立跑，只有 `hook_offline` 内部有「先编 dll 再跑 case」这种
自带的先后关系（它自己会报 skipped）。

用法
----
    python simplexc/tools/check.py                       # 全部步骤
    python simplexc/tools/check.py --list
    python simplexc/tools/check.py --only corpus --only hook_offline
    python simplexc/tools/check.py --json work/tmp/check/last.json
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
ROOT = os.path.dirname(SIMPLEXC)                       # tc-toolkit/
WORK = os.path.join(ROOT, "work", "tmp", "check")

CORPUS = os.path.join(HERE, "recompile_corpus.py")
SELFTEST = os.path.join(HERE, "host", "selftest.py")
HOOK_OFFLINE = os.path.join(HERE, "hook_offline.py")
PROBE = os.path.join(HERE, "probe_006.py")
BLOB_BASELINE = os.path.join(HERE, "blob_baseline.py")
BASELINE_DIR = os.path.join(HERE, "baseline")
BASELINE_BLOBS = os.path.join(ROOT, "work", "tmp", "baseline-blobs")

CORPUS_SRC = os.path.join("simplexc", "corpus", "src", "006.si")
CORPUS_BIN = os.path.join("simplexc", "corpus", "ref", "006.bin")
# G1（调试信息不改机器码）的样本：语料里元件注释段最多的一份，行号覆盖面最宽。
DEBUG_INFO_SRC = os.path.join("simplexc", "corpus", "src", "177.si")
BUILD = os.path.join(SIMPLEXC, "build")
SIMPLEXC_CLI = os.path.join(BUILD, "simplexc_cli.exe")
HOST = os.path.join(HERE, "host", "host.py")
RV32IM_SRC = os.path.join("simplexc", "corpus", "rv32im", "rv32im.si")
RV32IM_BIN = os.path.join("simplexc", "corpus", "rv32im", "rv32im.bin")
RV32IM_REF_ENTRY = "0x215B"   # manifest-capture.tsv seq 002 的 r2
LOWER_TICKS = 10

STEP_TIMEOUT = 900.0        # 普通子进程步骤的超时秒数
PROBE_TIMEOUT = 900.0       # probe_006 是可选步骤，超时就是 SKIP（不重试、不拉长）
HOOK_CASE_TIMEOUT = 300.0   # hook_offline 单个 case 的超时秒数
SCHEMA = 1

STEPS = OrderedDict()


def step(name, title):
    def register(fn):
        STEPS[name] = {"title": title, "run": fn}
        return fn
    return register


def utf8_stdout():
    """控制台是 GBK（chcp 936），中文/→/≠ 会花掉 —— 与 tools/deploy_compile_hook.py 同一处理。

    标准输出与标准错误都换：失败原因走 stderr，它比摘要更需要能读。
    """
    for stream in (sys.stdout, sys.stderr):         # 报错信息比摘要更需要能读
        rec = getattr(stream, "reconfigure", None)
        if rec:
            with contextlib.suppress(Exception):
                rec(encoding="utf-8", errors="replace")


def rel(path):
    """输出里一律用相对 tc-toolkit/ 的路径（换机器/换盘符也读得顺）。"""
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:
        return path


def child_env():
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"       # 子进程输出按 UTF-8 回来，摘要才不是乱码
    env.pop("TC_OURS_TIMEOUT_MS", None)     # 判据自己决定预算，不从调用者继承
    return env


def run_cmd(cmd, *, timeout, cwd=ROOT):
    """跑一条命令，返回 rc / 耗时 / 合并输出。超时与找不到程序都折成「这一步失败」。"""
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              env=child_env())
    except FileNotFoundError as e:
        secs = round(time.perf_counter() - t0, 3)
        return {"rc": None, "seconds": secs, "output": str(e),
                "status": "FAIL", "reason": f"找不到程序：{e}"}
    except subprocess.TimeoutExpired:
        secs = round(time.perf_counter() - t0, 3)
        return {"rc": None, "seconds": secs, "output": "",
                "status": "FAIL", "reason": f"超时（{timeout:.0f} s）"}
    secs = round(time.perf_counter() - t0, 3)
    out = ((proc.stdout or "") + (proc.stderr or "")).strip()
    status = "PASS" if proc.returncode == 0 else "FAIL"
    return {"rc": proc.returncode, "seconds": secs, "output": out, "status": status,
            "reason": "" if proc.returncode == 0 else f"rc={proc.returncode}"}


def _load_by_path(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tail_lines(text, n=40):
    return list((text or "").splitlines())[-n:]


def _num(pattern, text):
    m = re.search(pattern, text or "")
    return int(m.group(1)) if m else None


# ---- 步骤 ---------------------------------------------------------------
@step("corpus", "M0 语料/manifest 一致（recompile_corpus.py --check）")
def step_corpus(ctx):
    r = run_cmd([sys.executable, CORPUS, "--check"], timeout=STEP_TIMEOUT)
    out = r["output"]
    summary = {}
    for key, pattern in (("manifest_rows", r"manifest 数据行=(\d+)"),
                         ("diff_sites", r"差异站点 (\d+) 处"),
                         ("unclassified", r"未分类差异 (\d+) 处")):
        val = _num(pattern, out)
        if val is not None:
            summary[key] = val
    m = re.search(r"归一化后逐字节相同 (\d+)/(\d+) 份", out)
    if m:
        summary["rebased_identical"] = f"{m.group(1)}/{m.group(2)}"
    m = re.search(r"恒定差值 (0x[0-9a-f]+|-)", out)
    if m:
        summary["rebased_delta"] = m.group(1)
    summary["verdict"] = "校验通过" if "校验通过" in out else "未通过"
    return {**r, "summary": summary}


@step("host_selftest", "M2 差分宿主自比对（host/selftest.py，006，3 tick）")
def step_host_selftest(ctx):
    workdir = os.path.join(WORK, "host_selftest")
    report = os.path.join(workdir, "report.json")
    if os.path.exists(report):
        os.remove(report)
    cmd = [sys.executable, SELFTEST, "--src", CORPUS_SRC, "--bin", CORPUS_BIN,
           "--ticks", "3", "--workdir", workdir, "--report", report]
    r = run_cmd(cmd, timeout=STEP_TIMEOUT)
    summary = {}
    try:
        with open(report, encoding="utf-8") as f:
            rep = json.load(f)
        checks = rep.get("checks") or {}
        summary["ticks"] = f"{rep.get('ticks_a')}/{rep.get('ticks_b')}"
        summary["tick_exact"] = bool(checks.get("tick_exact"))
        summary["sim_made_work"] = bool(checks.get("sim_made_work"))
        summary["extern"] = int(rep.get("extern_total") or 0)
        summary["report_passed"] = bool(rep.get("passed"))
    except (OSError, ValueError) as e:
        summary["report"] = f"读不到 {rel(report)}：{e}"
    return {**r, "summary": summary}


@step("lower_equiv", "M4.5：C++ 产物 vs 参考产物逐 tick 等价（rv32im，10 tick，灌 RESET_DATA）")
def step_lower_equiv(ctx):
    """生产者是**我们自己的**编译器（`simplexc/build/simplexc_cli.exe`，C++17），不是原版编译器 ——
    这一步才是「C++ 前端 + lowering 能独立产出等价产物」的判据。

    没有 CLI 就是 SKIP（不是 FAIL）：它要先 `python simplexc/tools/build.py simplexc_cli`。
    """
    if not os.path.exists(SIMPLEXC_CLI):
        return {"rc": 0, "seconds": 0.0, "output": "", "status": "SKIP", "reason": "缺 CLI",
                "summary": {"verdict": "SKIP：先跑 python simplexc/tools/build.py simplexc_cli"}}
    workdir = os.path.join(WORK, "lower_equiv")
    os.makedirs(workdir, exist_ok=True)
    blob = os.path.join(workdir, "ours.bin")
    for suffix in (".entry", ".stubs"):
        with contextlib.suppress(OSError):
            os.remove(blob + suffix)
    r = run_cmd([SIMPLEXC_CLI, "--src", RV32IM_SRC, "--out", blob, "--timing"],
                timeout=STEP_TIMEOUT)
    summary = {"cli_rc": r.get("rc"), "blob": rel(blob)}
    m = re.search(r"前端=(\d+) ms\s+lowering=(\d+) ms\s+LLVM\(O2\+代码生成\)=(\d+) ms", r["output"])
    if m:
        summary["ms_frontend"], summary["ms_lowering"], summary["ms_llvm"] = m.group(1), m.group(2), m.group(3)
    if r["rc"] != 0 or not os.path.exists(blob):
        summary["verdict"] = "CLI 没产出 blob"
        return {**r, "summary": summary}
    summary["blob_bytes"] = os.path.getsize(blob)
    try:
        with open(blob + ".entry", encoding="ascii") as f:
            ours_entry = "0x" + f.read().strip()
    except OSError as e:
        summary["verdict"] = f"读不到 .entry：{e}"
        return {**r, "summary": summary}
    summary["ours_entry"] = ours_entry

    logs = {}
    for tag, binpath, entry in (("ref", RV32IM_BIN, RV32IM_REF_ENTRY),
                               ("ours", blob, ours_entry)):
        out = os.path.join(workdir, f"{tag}.jsonl")
        with contextlib.suppress(OSError):
            os.remove(out)
        hr = run_cmd([sys.executable, HOST, "--src", RV32IM_SRC, "--bin", binpath,
                      "--entry", entry, "--ticks", str(LOWER_TICKS),
                      "--seed-reset-data", "seq", "--out", out], timeout=STEP_TIMEOUT)
        if hr["rc"] != 0 or not os.path.exists(out):
            summary["verdict"] = f"{tag} 侧宿主失败：{(hr['output'] or '')[-400:]}"
            return {**r, "output": r["output"] + "\n" + hr["output"], "summary": summary}
        with open(out, encoding="utf-8") as f:
            logs[tag] = [json.loads(line) for line in f if line.strip()]

    mod = _load_by_path(SELFTEST, "selftest")
    try:
        diff = mod.compare(logs["ref"], logs["ours"])
    except Exception as e:                                     # 比对器自己炸也要算 FAIL
        summary["verdict"] = f"比对器抛异常：{e}"
        return {**r, "rc": 1, "status": "FAIL", "summary": summary}
    summary["ticks"] = f"{len(logs['ref'])}/{len(logs['ours'])}"
    summary["diff_count"] = len(diff)
    if diff:
        summary["first_diff"] = str(diff[0])[:300]
        summary["verdict"] = f"差异 {len(diff)} 条"
        return {**r, "rc": 1, "status": "FAIL", "reason": "有差异", "summary": summary}
    summary["verdict"] = "逐 tick 全等"
    return {**r, "summary": summary}


@step("blob_baseline", "产物基线回归（98 份语料的装配产物逐字段比，blob_baseline.py --verify）")
def step_blob_baseline(ctx):
    """重构（`lower/` → `sema/` + `codegen/`）的硬判据：每份语料的装配产物在重构前后等价。

    缺产物目录是 SKIP 不是 FAIL：`work/tmp/baseline-blobs/` 体积大不进 git，别的机器 / 别的
    worktree 本来就没有 —— 那说明这台机器还没 record，不是重构把产物改坏了。判等清单在 git 里。
    """
    record_hint = f"python simplexc/tools/blob_baseline.py --record {rel(BASELINE_DIR)}"
    if not os.path.isdir(BASELINE_BLOBS):
        why = f"缺产物目录 {rel(BASELINE_BLOBS)}/（不进 git）：先跑 {record_hint}"
        return {"status": "SKIP", "rc": None, "seconds": 0.0,
                "summary": {"reason": why}, "output": "", "reason": why}
    if not os.path.exists(SIMPLEXC_CLI):
        why = "缺 CLI：先跑 python simplexc/tools/build.py"
        return {"status": "SKIP", "rc": None, "seconds": 0.0,
                "summary": {"reason": why}, "output": "", "reason": why}
    r = run_cmd([sys.executable, BLOB_BASELINE, "--verify", BASELINE_DIR,
                 "--blobs", BASELINE_BLOBS, "--workdir", os.path.join(WORK, "blob_baseline")],
                timeout=STEP_TIMEOUT)
    summary = {"blobs": rel(BASELINE_BLOBS)}
    same, diff = _num(r"一致 (\d+) 份", r["output"]), _num(r"差异 (\d+) 份", r["output"])
    if same is not None and diff is not None:
        summary["same"] = f"{same}/{same + diff}"
        summary["diff"] = str(diff)
    return {**r, "summary": summary}


def _text_disasm(objdump, obj):
    """`objdump -d --section=.text` 的指令行（去掉带文件名的头行）。

    为什么要去：两份 obj 的路径不同，而 objdump 把路径印在第一行 —— 那行不是机器码。
    """
    r = run_cmd([objdump, "-d", "--section=.text", obj], timeout=STEP_TIMEOUT)
    keep = [ln for ln in r["output"].splitlines()
            if "file format" not in ln and not ln.startswith("Disassembly of section")]
    return r, keep


def _first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b, strict=False)):
        if x != y:
            return f"第 {i + 1} 行：{x.strip()!r} vs {y.strip()!r}"
    if len(a) != len(b):
        longer = a if len(a) > len(b) else b
        return f"行数不同（{len(a)} vs {len(b)}），多出：{longer[min(len(a), len(b))].strip()!r}"
    return ""


@step("debug_info_equiv", "G1：调试信息不改机器码（177.si 开/关 --debug-info，objdump -d 只比 .text）")
def step_debug_info_equiv(ctx):
    """hotspot W1 的 G1（`docs/specs/2026-11-01-hotspot-profiler-design.md` §5/§8）。

    为什么要常驻：调试信息一旦混进 `dbg.value`/`dbg.declare`，或者某个 pass 因为看见 `!dbg`
    而改主意，机器码就变了 —— 那是「分析的不是跑的那份」的另一种形态，从产物字节上看不出来。
    比两样：`.text` 反汇编文本（去掉 objdump 的文件名头）必须逐行相同；产物整文件必须逐字节
    相同（装配器丢掉 `.debug*`，所以调试信息不该动 blob 一个字节）。另查 g1 的行表：
    `objdump --dwarf=decodedline` 非空，且行号落在 1..源文件行数（0 = 行程序序列结尾，另计）。
    """
    objdump = shutil.which("objdump")
    if objdump is None:
        why = "PATH 里没有 objdump（w64devkit 的 binutils）"
        return {"status": "SKIP", "rc": None, "seconds": 0.0, "summary": {"reason": why},
                "output": "", "reason": why}
    if not os.path.exists(SIMPLEXC_CLI):
        why = "缺 CLI：先跑 python simplexc/tools/build.py simplexc_cli"
        return {"status": "SKIP", "rc": None, "seconds": 0.0, "summary": {"reason": why},
                "output": "", "reason": why}
    workdir = os.path.join(ctx["work"], "debug_info")
    os.makedirs(workdir, exist_ok=True)
    try:
        with open(DEBUG_INFO_SRC, encoding="utf-8", errors="replace") as f:
            src_lines = len(f.read().splitlines())
    except OSError as e:
        return {"status": "FAIL", "rc": None, "seconds": 0.0,
                "summary": {"error": f"读不到 {rel(DEBUG_INFO_SRC)}：{e}"},
                "output": "", "reason": str(e)}

    t0 = time.perf_counter()
    disasm, blobs, logs = {}, {}, []
    for tag, extra in (("g0", []), ("g1", ["--debug-info"])):
        obj = os.path.join(workdir, tag + ".obj")
        bin_path = os.path.join(workdir, tag + ".bin")
        r = run_cmd([SIMPLEXC_CLI, "--src", DEBUG_INFO_SRC, "--out", bin_path,
                     "--obj-out", obj, *extra], timeout=STEP_TIMEOUT)
        logs.append(r["output"])
        if r["status"] != "PASS":
            why = f"{tag} 编译失败（{r['reason'] or 'rc≠0'}）"
            return {"status": "FAIL", "rc": r["rc"], "seconds": round(time.perf_counter() - t0, 3),
                    "summary": {"variant": tag, "reason": why},
                    "output": "\n".join(logs), "reason": why}
        od, disasm[tag] = _text_disasm(objdump, obj)
        if od["status"] != "PASS":
            why = f"objdump -d {tag}.obj 失败（rc={od['rc']}）"
            return {"status": "FAIL", "rc": od["rc"], "seconds": round(time.perf_counter() - t0, 3),
                    "summary": {"variant": tag, "reason": why},
                    "output": "\n".join(logs + [od["output"]]), "reason": why}
        with open(bin_path, "rb") as f:
            blobs[tag] = f.read()

    dl = run_cmd([objdump, "--dwarf=decodedline", os.path.join(workdir, "g1.obj")],
                 timeout=STEP_TIMEOUT)
    if dl["status"] != "PASS":
        why = f"objdump --dwarf=decodedline 失败（rc={dl['rc']}）"
        return {"status": "FAIL", "rc": dl["rc"], "seconds": round(time.perf_counter() - t0, 3),
                "summary": {"reason": why}, "output": "\n".join(logs + [dl["output"]]),
                "reason": why}
    # 行表行形如：`177.si   13519   0x3a   x`（列：文件 行号 地址 标志）
    line_nums = [int(m.group(1)) for m in
                 re.finditer(r"^\s*\S+\s+(\d+)\s+(?:0x[0-9a-fA-F]+|\d+)\s", dl["output"], re.M)]
    located = [n for n in line_nums if n > 0]
    out_of_range = [n for n in located if n > src_lines]

    summary = {
        "insn": sum(1 for ln in disasm["g1"] if re.search(r"^\s+[0-9a-f]+:", ln)),
        "text_diff": "空" if disasm["g0"] == disasm["g1"] else _first_diff(disasm["g0"], disasm["g1"]),
        "blob_identical": blobs["g0"] == blobs["g1"],
        "line_rows": len(located),
        "end_of_seq_rows": len(line_nums) - len(located),
        "line_range": f"1..{src_lines}",
    }
    reasons = []
    if disasm["g0"] != disasm["g1"]:
        reasons.append("`.text` 反汇编有差异（调试信息改了机器码）")
    if not located:
        reasons.append("g1 没有行表（`--dwarf=decodedline` 为空 —— module flag / finalize 顺序）")
    if out_of_range:
        reasons.append(f"行号越界：{out_of_range[:5]}（源文件 {src_lines} 行）")
    if reasons:
        return {"status": "FAIL", "rc": 0, "seconds": round(time.perf_counter() - t0, 3),
                "summary": summary, "output": "\n".join(logs), "reason": "；".join(reasons)}
    return {"status": "PASS", "rc": 0, "seconds": round(time.perf_counter() - t0, 3),
            "summary": summary, "output": "\n".join(logs)}


@step("hook_offline", "壳的失败路径/熔断/回退/超时（hook_offline.run_all，case 数见摘要）")
def step_hook_offline(ctx):
    t0 = time.perf_counter()
    try:
        mod = _load_by_path(HOOK_OFFLINE, "hook_offline")
        res = mod.run_all(verbose=False, timeout=HOOK_CASE_TIMEOUT)
    except Exception as e:                  # 器材本身坏了也是「这一步失败」
        return {"status": "FAIL", "rc": None, "seconds": round(time.perf_counter() - t0, 3),
                "summary": {"error": f"{type(e).__name__}: {e}"}, "output": "",
                "reason": f"run_all 抛异常：{type(e).__name__}: {e}"}
    secs = round(time.perf_counter() - t0, 3)
    cases = res.get("cases") or []
    skipped = res.get("skipped") or []
    build = res.get("build") or {}
    n_ok = sum(1 for c in cases if c["passed"])
    summary = {"cases": f"{n_ok}/{len(cases) + len(skipped)}",
               "build_ok": bool(build.get("ok"))}
    if "baseline_differs" in build:
        summary["baseline_differs"] = bool(build["baseline_differs"])
    if res.get("failed"):
        summary["failed"] = ",".join(res["failed"])
    if skipped:
        summary["not_run"] = ",".join(skipped)
    if build.get("error"):
        summary["build_error"] = build["error"]
    lines = []
    if build.get("error"):
        lines.append(f"构建：{build['error']}")
    for c in cases:
        bad = [k for k, ok in c["checks"].items() if not ok]
        verdict = "PASS" if c["passed"] else "FAIL"
        lines.append(f"{c['name']:<18} {verdict} rc={c['rc']} {c['seconds']:.2f}s"
                     + ("  未过：" + "、".join(bad) if bad else ""))
        if bad:
            lines += [f"  | {ln}" for ln in c.get("output_tail") or []]
    summary["seconds"] = res.get("seconds")
    return {"status": "PASS" if res.get("ok") else "FAIL",
            "rc": 0 if res.get("ok") else 1,
            "seconds": secs, "summary": summary, "output": "\n".join(lines),
            "reason": "" if res.get("ok") else "有 case 未过：" + ",".join(res.get("failed") or [])}


@step("probe_006", "M1.5 收益探针（可选步骤：缺依赖/脚本在改 → SKIP）")
def step_probe_006(ctx):
    """可选步骤：脚本不存在、--help 就挂、依赖缺失、超时、rc≠0 一律 SKIP 并写清原因。

    它由另一个 agent 在写，且可能正在跑长测量 —— 所以只试一次：先 `--help` 探 CLI
    （不改它的词表），按 help 里出现的选项拼一条**不做吞吐测量**的快速调用（吞吐是
    manual 的长测量，不该塞进一条判据命令里）。
    """
    if not os.path.isfile(PROBE):
        why = f"脚本不存在：{rel(PROBE)}"
        return {"status": "SKIP", "rc": None, "seconds": 0.0,
                "summary": {"reason": why}, "output": "", "reason": why}
    helpr = run_cmd([sys.executable, PROBE, "--help"], timeout=120.0)
    if helpr["rc"] != 0:
        why = f"{rel(PROBE)} --help 就失败（rc={helpr['rc']}，脚本在改？）"
        return {"status": "SKIP", "rc": helpr["rc"], "seconds": helpr["seconds"],
                "summary": {"reason": why}, "output": helpr["output"], "reason": why}
    flags = helpr["output"]
    probe_work = os.path.join(WORK, "probe")
    cmd = [sys.executable, PROBE]
    picked = []
    if "--no-throughput" in flags:
        cmd.append("--no-throughput")
        picked.append("--no-throughput")
    if "--ticks" in flags:
        cmd += ["--ticks", "3"]
        picked.append("--ticks 3")
    for opt in ("--outdir", "--results"):
        if opt in flags:                    # 别动它在 simplexc/probe/results 的正式结果
            cmd += [opt, probe_work]
            picked.append(f"{opt} {rel(probe_work)}")
    if "--quiet" in flags:
        cmd.append("--quiet")
        picked.append("--quiet")
    r = run_cmd(cmd, timeout=PROBE_TIMEOUT)
    invoked = " ".join(picked) or "（默认参数）"
    if r["status"] != "PASS":
        why = f"{r['reason'] or '调用失败'}（依赖缺失/脚本在改/被测项失败，只试一次不重试）"
        return {"status": "SKIP", "rc": r["rc"], "seconds": r["seconds"],
                "summary": {"reason": why, "invoked_as": invoked},
                "output": r["output"], "reason": why}
    summary = {"invoked_as": invoked, "rc": r["rc"]}
    payload_path = os.path.join(probe_work, "006_fold.json")
    try:
        with open(payload_path, encoding="utf-8") as f:
            rep = json.load(f)
        cmp_ = rep.get("tick_compare") or {}
        summary["ticks"] = f"{len(cmp_.get('pages_per_tick') or [])}"
        summary["identical"] = bool(cmp_.get("identical"))
        summary["semantic_diffs"] = len(cmp_.get("semantic_diffs") or [])
        ratio = (rep.get("throughput") or {}).get("ratio") or {}
        if ratio.get("speedup"):
            summary["speedup"] = round(float(ratio["speedup"]), 3)
    except (OSError, ValueError, AttributeError, TypeError):
        summary["result_json"] = f"未取到 {rel(payload_path)}"
    return {"status": "PASS", "rc": r["rc"], "seconds": r["seconds"],
            "summary": summary, "output": r["output"]}


# ---- 汇总与打印 ---------------------------------------------------------
def summary_text(summary):
    items = []
    for key, val in summary.items():
        if val is None or val == "":
            continue
        items.append(f"{key}={val}")
    return " · ".join(items)


def print_table(results):
    print(f"{'':2}{'步骤':<14} {'状态':<5} {'rc':>4} {'耗时':>8}  摘要")
    for r in results:
        rc = "-" if r["rc"] is None else str(r["rc"])
        print(f"  {r['name']:<14} {r['status']:<5} {rc:>4} {r['seconds']:>7.2f}s  "
              f"{summary_text(r['summary'])}")


def print_details(results):
    interesting = [r for r in results if r["status"] != "PASS" and (r["output"] or r["summary"])]
    if not interesting:
        return
    print("\n--- 非 PASS 详情 ---")
    for r in interesting:
        head = r["status"] + (f"：{r['reason']}" if r.get("reason") else "")
        print(f"[{r['name']}] {head}")
        for ln in tail_lines(r["output"], 40):
            print("  " + ln)


def git_info():
    info: dict[str, str | bool | None] = {"head": None, "branch": None, "dirty": None}
    try:
        info["head"] = subprocess.check_output(
            ["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
            text=True, encoding="utf-8", errors="replace").strip()
        info["branch"] = subprocess.check_output(
            ["git", "-C", ROOT, "rev-parse", "--abbrev-ref", "HEAD"],
            text=True, encoding="utf-8", errors="replace").strip()
        status = subprocess.check_output(
            ["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
            text=True, encoding="utf-8", errors="replace")
        info["dirty"] = bool(status.strip())
    except (OSError, subprocess.CalledProcessError):
        pass
    return info


def write_json(path, payload):
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    return path


def main(argv=None):
    utf8_stdout()
    ap = argparse.ArgumentParser(
        description="一条命令跑完整的离线判据（corpus / host_selftest / hook_offline / probe_006）")
    ap.add_argument("--list", action="store_true", help="列出步骤名（按默认执行顺序）")
    ap.add_argument("--only", action="append", metavar="NAME",
                    help="只跑这些步骤（可重复；顺序按默认顺序）")
    ap.add_argument("--json", default=None, help="把机器可读结果写到这个 JSON（run 结束才写）")
    args = ap.parse_args(argv)

    if args.list:
        for name, spec in STEPS.items():
            print(f"{name:<14} {spec['title']}")
        return 0

    if args.only:
        unknown = [n for n in args.only if n not in STEPS]
        if unknown:
            print(f"未知步骤：{'、'.join(unknown)}", file=sys.stderr)
            print(f"可用步骤：{'、'.join(STEPS)}（--list 看说明）", file=sys.stderr)
            return 2
        names = [n for n in STEPS if n in set(args.only)]
    else:
        names = list(STEPS)

    os.makedirs(WORK, exist_ok=True)
    info = git_info()
    head = info["head"] or "?"
    dirty = "，工作区有未提交改动" if info["dirty"] else ""
    print(f"判据入口 · tc-toolkit/（HEAD {head}{dirty}）· Python {sys.version.split()[0]} · "
          f"{len(names)} 个步骤")
    print(f"工作目录：{rel(WORK)}/\n")

    t0 = time.perf_counter()
    results = []
    for name in names:                      # 一步失败不影响其它步：每步独立跑
        spec = STEPS[name]
        t1 = time.perf_counter()
        try:
            r = spec["run"]({"work": WORK})
        except Exception as e:              # 步骤实现自己炸了 → 这一步 FAIL，别人继续
            r = {"status": "FAIL", "rc": None, "seconds": round(time.perf_counter() - t1, 3),
                 "summary": {"error": f"{type(e).__name__}: {e}"}, "output": "",
                 "reason": f"步骤抛异常：{type(e).__name__}: {e}"}
        r.setdefault("reason", "")
        r["name"] = name
        r["title"] = spec["title"]
        r.setdefault("summary", {})
        r.setdefault("output", "")
        results.append(r)

    total = round(time.perf_counter() - t0, 3)
    print_table(results)
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("PASS", "FAIL", "SKIP")}
    print(f"\n总计 {len(results)} 步：{counts['PASS']} PASS · {counts['FAIL']} FAIL · "
          f"{counts['SKIP']} SKIP   用时 {total:.2f}s")
    if counts["SKIP"]:
        skipped = "、".join(r["name"] for r in results if r["status"] == "SKIP")
        print(f"⚠ SKIP 不算通过也不算失败（前置条件不满足）：{skipped}")
    print_details(results)

    payload = {
        "schema": SCHEMA,
        "tool": "simplexc/tools/check.py",
        "generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "git": info,
        "root": ROOT,
        "ok": counts["FAIL"] == 0,
        "seconds": total,
        "counts": counts,
        "steps": [{"name": r["name"], "title": r["title"], "status": r["status"],
                   "rc": r["rc"], "seconds": r["seconds"], "summary": r["summary"],
                   "reason": r["reason"], "output_tail": tail_lines(r["output"], 40)}
                  for r in results],
    }
    print()
    if args.json:
        path = write_json(args.json, payload)
        print(f"JSON → {rel(path)}")
    else:
        print("（未指定 --json：没有落机器可读结果；加 --json <path> 可留档）")
    return 0 if counts["FAIL"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
