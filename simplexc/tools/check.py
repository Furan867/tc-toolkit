#!python3
"""一条命令跑完整的**离线**判据（主设计 §7.5 的「一条命令」入口）。

步骤表
------
| 名字 | 跑什么 |
|---|---|
| `corpus`        | `python simplexc/tools/recompile_corpus.py --check`（M0：语料/manifest 一致 + 同源三条断言）|
| `host_selftest` | `python simplexc/tools/host/selftest.py --src …006.si --bin …006.bin --ticks 3`（M2 宿主自比对）|
| `hook_offline`  | `hook_offline.run_all()`：壳的四种情形 + refcmp + 超时（8 个 case）|
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
import subprocess
import sys
import time
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
ROOT = os.path.dirname(SIMPLEXC)                       # reversing/
WORK = os.path.join(ROOT, "work", "tmp", "check")

CORPUS = os.path.join(HERE, "recompile_corpus.py")
SELFTEST = os.path.join(HERE, "host", "selftest.py")
HOOK_OFFLINE = os.path.join(HERE, "hook_offline.py")
PROBE = os.path.join(HERE, "probe_006.py")

CORPUS_SRC = os.path.join("simplexc", "corpus", "src", "006.si")
CORPUS_BIN = os.path.join("simplexc", "corpus", "ref", "006.bin")

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
    """输出里一律用相对 reversing/ 的路径（换机器/换盘符也读得顺）。"""
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


@step("hook_offline", "壳的失败路径/熔断/回退/超时（hook_offline.run_all，8 个 case）")
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
    print(f"判据入口 · reversing/（HEAD {head}{dirty}）· Python {sys.version.split()[0]} · "
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
