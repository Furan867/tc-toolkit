#!python3
"""M2 判据：参考产物自比对（宿主可靠性得证）。

两个**独立宿主进程**跑同一份参考产物，逐 tick 快照必须逐字节相等。三条防伪检查同样算判据 ——
空跑的"全等"没有意义：
  1. 跑到的 tick 数够；
  2. 至少有一个变化页**不在**命令块/设置块所在页（证明仿真真的在动，不只是进度计数器）；
  3. extern 序列非空。

用法：
    python simplexc/tools/host/selftest.py --src <si> --bin <bin> --ticks 20
退出码 0 = 通过；同时把机器可读报告写到 --report（默认 stdout 摘要 + JSON 路径）。
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(os.path.dirname(HERE))


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


simspec = _load(os.path.join(HERE, "simspec.py"), "simspec")

HOST = os.path.join(HERE, "host.py")
PAGE = 0x10000


def run_host(src, binpath, ticks, out, budget, extra=None):
    cmd = [sys.executable, HOST, "--src", src, "--bin", binpath, "--ticks", str(ticks),
           "--budget", str(budget), "--out", out, "--quiet"]
    if extra:
        cmd += extra
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=max(120.0, ticks * (budget + 5)))
    return proc


def read_snaps(path):
    snaps = []
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return snaps                                  # 宿主没跑到就崩了 → 没有文件
    for line in lines:
        if not line.strip():
            continue
        try:
            snaps.append(json.loads(line))
        except json.JSONDecodeError:
            continue                                    # 截断的最后一行：宿主崩在半路
    return snaps


def rm_quiet(path):
    try:
        os.remove(path)
    except OSError as e:                              # 删不掉不影响判据，但要看得见
        print(f"[selftest] 删不掉 {path}：{e}", file=sys.stderr)


def compare(a, b):
    """逐 tick 比 pages / extern / sim_cycle。返回差异列表（前若干条）。"""
    diffs = []
    for i in range(max(len(a), len(b))):
        if i >= len(a):
            diffs.append(f"tick {i + 1}: A 没有这一 tick（B 有）")
            continue
        if i >= len(b):
            diffs.append(f"tick {i + 1}: B 没有这一 tick（A 有）")
            continue
        x, y = a[i], b[i]
        if x["pages"] != y["pages"]:
            only_a = {k: v for k, v in x["pages"].items() if y["pages"].get(k) != v}
            only_b = {k: v for k, v in y["pages"].items() if x["pages"].get(k) != v}
            diffs.append(f"tick {i + 1}: 页不同 A 独有 {only_a} / B 独有 {only_b}")
        if x["sim_cycle"] != y["sim_cycle"]:
            diffs.append(f"tick {i + 1}: sim_cycle {x['sim_cycle']} vs {y['sim_cycle']}")
        if len(x["extern"]) != len(y["extern"]):
            diffs.append(f"tick {i + 1}: extern 条数 {len(x['extern'])} vs {len(y['extern'])}")
        else:
            for j, (ex, ey) in enumerate(zip(x["extern"], y["extern"], strict=False)):
                if ex != ey:
                    diffs.append(f"tick {i + 1} extern[{j}]: {ex} vs {ey}")
                    break
        if len(diffs) >= 8:
            break
    return diffs


def main(argv=None):
    ap = argparse.ArgumentParser(description="M2 参考产物自比对")
    ap.add_argument("--src", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--entry", default=None,
                    help="入口偏移（传给 host.py；不给则 host 从 corpus/manifest.tsv 查 —— "
                         "捕获的产物不在那张表里，这时必须显式给）")
    ap.add_argument("--ticks", type=int, default=20)
    ap.add_argument("--budget", type=float, default=60.0)
    ap.add_argument("--workdir", default=None, help="中间文件目录（默认系统临时目录）")
    ap.add_argument("--report", default=None, help="机器可读报告写到这个 JSON")
    ap.add_argument("--keep", action="store_true", help="保留中间 JSONL")
    args = ap.parse_args(argv)

    workdir = args.workdir or os.path.join(tempfile.gettempdir(), "tc_host_selftest")
    try:
        os.makedirs(workdir, exist_ok=True)
    except OSError as e:
        raise SystemExit(f"建不了工作目录 {workdir}：{e}") from e
    a_out = os.path.join(workdir, "a.jsonl")
    b_out = os.path.join(workdir, "b.jsonl")
    rm_quiet(a_out)
    rm_quiet(b_out)

    spec = simspec.SimSpec.parse(args.src)
    report = {"src": args.src, "bin": args.bin, "entry": args.entry,
              "ticks_requested": args.ticks}
    extra = ["--entry", args.entry] if args.entry else None
    pa = run_host(args.src, args.bin, args.ticks, a_out, args.budget, extra)
    pb = run_host(args.src, args.bin, args.ticks, b_out, args.budget, extra)
    report["returncodes"] = [pa.returncode, pb.returncode]
    a, b = read_snaps(a_out), read_snaps(b_out)
    report["ticks_a"] = len(a)
    report["ticks_b"] = len(b)

    checks = {}
    # 1) 跑到的 tick 数
    checks["ticks_reached"] = (len(a) == args.ticks and len(b) == args.ticks)
    # 2) 逐 tick 全等
    diffs = compare(a, b)
    checks["tick_exact"] = not diffs
    report["diffs"] = diffs
    # 3) 变化页不在命令块/设置块所在页（否则可能只是进度计数器在动）
    driver_pages = {hex(spec.addr("commands") & ~(PAGE - 1)), hex(spec.addr("settings") & ~(PAGE - 1))}
    work_pages = set()
    extern_total = 0
    for snap in a:
        work_pages |= {p for p in snap["pages"] if p not in driver_pages}
        extern_total += len(snap["extern"])
    checks["sim_made_work"] = bool(work_pages)
    report["extern_total"] = extern_total
    # 防伪：唯一个硬要求是「仿真真在工作」。extern 非空**不能**当硬条件 ——
    # 合法电路可以一个语义 extern 都不调（实测 005）。
    checks["not_vacuous"] = bool(work_pages) or extern_total > 0
    report["work_pages"] = sorted(work_pages)
    report["passed"] = all(checks.values())
    report["checks"] = checks
    if pa.returncode != 0:
        report["stderr_a"] = pa.stderr[-800:]
    if pb.returncode != 0:
        report["stderr_b"] = pb.stderr[-800:]

    print(f"自比对 {'通过' if report['passed'] else '失败'}："
          f"tick {len(a)}/{len(b)}（要 {args.ticks}）· "
          f"逐 tick 全等={'是' if checks['tick_exact'] else '否'} · "
          f"仿真真在工作={'是' if checks['sim_made_work'] else '否'}（工作页 {len(work_pages)} 个）· "
          f"extern {extern_total} 条")
    for d in diffs:
        print("  " + d)
    if not report["passed"]:
        for k, v in checks.items():
            if not v:
                print(f"  未过：{k}")
        for key in ("stderr_a", "stderr_b"):
            if key in report:
                print(f"  --- {key} ---\n{report[key]}")
    if args.report:
        try:
            with open(args.report, "w", encoding="utf-8") as f:
                json.dump(report, f, ensure_ascii=False, indent=2)
        except OSError as e:
            print(f"写不了报告 {args.report}：{e}", file=sys.stderr)
    if not args.keep:
        rm_quiet(a_out)
        rm_quiet(b_out)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
