#!python3
"""自动测当前电路在各 rewrite 配置下的主频，一次跑完并出对照表。

前提
----
- 游戏在跑；
- 电路**有一次可撤销的编辑历史**（撤销栈非空）—— 重编译就是靠 simctl 交替发
  Ctrl+Z / Ctrl+Shift+Z 触发的（见 tools/simctl.py），栈空了会直接报错提示。
- 测的过程中会**抢游戏窗口前台**（SendInput 有前台校验，不抢就不发键），
  期间别切走窗口。

每个模式一遍
------------
    写 opt.ini  →  simctl.recompile()  →  等 NNN.opt.bin 出现
    →  simctl.run()  →  采样 N 秒  →  simctl.stop()

主频取自游戏自己每秒重算的 `sim_speed`（float32 Hz，偏移 0x624D618），
丢掉开头 warmup 秒的过渡值，记中位数与最大值。

用法
----
    py tools/bench_rewrite.py                      # off,on,short,short_cse 各 8s
    py tools/bench_rewrite.py --modes on,short
    py tools/bench_rewrite.py --seconds 12 --repeat 2
    py tools/bench_rewrite.py --dry-run            # 只打印计划

结果同时写到 `work/freq/bench_<时间戳>.tsv`（含每个采样点原始值）。
opt.ini 会停在**最后一个模式**，原文件备份在 `work/tmp/opt.ini.bak`。
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import statistics
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import simctl  # noqa: E402
import watch_freq as W  # noqa: E402（复用它的产物指纹 / 日志解析）
import winmem  # noqa: E402

ROOT = HERE.parent
OPT_DIR = ROOT / "work" / "opt"
BENCH_DIR = ROOT / "work" / "freq"
OPT_INI = ROOT / "opt.ini"
BACKUP = ROOT / "work" / "tmp" / "opt.ini.bak"

EXE = "Turing Complete.exe"
BASE_VA = 0x140000000
OFF_SIM_SPEED = 0x14624D618 - BASE_VA

ALL_MODES = ("off", "on", "short", "short_cse")
REWRITE_RE = re.compile(r"^(\s*)rewrite\s*=\s*(\S+)\s*$", re.IGNORECASE)
PRODUCT_RE = re.compile(r"^(\d+)\.opt\.bin$")


# ---------------------------------------------------------------- opt.ini

def set_rewrite_ini(mode: str) -> str | None:
    """把 opt.ini 里**第一条生效的** rewrite 行改成 mode，返回改前的值。

    只改这一行的值，不新增行 —— 否则文件里出现重复 key，转发层
    （GetPrivateProfileString 取第一个）与改写器可能读到不同的值。
    先用临时文件写完再原子替换，避免转发层读到写了一半的文件。
    """
    lines = OPT_INI.read_text(encoding="utf-8").splitlines(keepends=True)
    old = None
    for i, line in enumerate(lines):
        s = line.strip()
        if not s or s[0] in ";#[":
            continue
        m = REWRITE_RE.match(line)
        if m:
            old = m.group(2)
            lines[i] = f"{m.group(1)}rewrite = {mode}\n"
            break
    if old is None:
        raise SystemExit(f"opt.ini 里找不到生效的 rewrite 行：{OPT_INI}")
    tmp = OPT_INI.with_name(OPT_INI.name + ".new")
    tmp.write_text("".join(lines), encoding="utf-8")
    os.replace(tmp, OPT_INI)
    return old


# ---------------------------------------------------------------- 产物 / 主频

def latest_product() -> str | None:
    """work/opt 里编号最大的产物编号（如 "043"）。"""
    best = None
    for p in OPT_DIR.glob("*.opt.bin"):
        m = PRODUCT_RE.match(p.name)
        if m and (best is None or int(m.group(1)) > int(best)):
            best = m.group(1)
    return best


def wait_new_product(prev: str | None, timeout: float) -> str | None:
    """等一个比 prev 更大的产物编号出现（= 这次重编译走完了编译 + 改写）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        cur = latest_product()
        if cur and (prev is None or int(cur) > int(prev)):
            return cur
        time.sleep(0.2)
    return None


def read_hz(h: int, base: int) -> float | None:
    raw = winmem.rpm(h, base + OFF_SIM_SPEED, 4)
    return None if raw is None else struct.unpack_from("<f", raw)[0]


def sample_freq(h: int, base: int, seconds: float, warmup: float) -> list[float]:
    """采样 N 秒，丢掉开头 warmup 秒（主频每秒重算一次，切换点读到的是混合值）。"""
    out: list[float] = []
    t0 = time.time()
    while True:
        t = time.time() - t0
        if t >= seconds:
            break
        time.sleep(0.4)
        hz = read_hz(h, base)
        if hz is None:
            break
        if t >= warmup and hz > 0:      # 0 = 仿真没在跑（游戏的哨兵值）
            out.append(hz)
    return out


# ---------------------------------------------------------------- 单模式

def bench_one(mode: str, args, pid: int, base: int, h: int, tag: str) -> dict:
    """走一遍：换配置 → 重编译 → 运行 → 采样 → 停止。"""
    rec: dict = {"mode": mode, "num": None, "hz": [], "note": ""}
    prev = latest_product()
    old = set_rewrite_ini(mode)
    print(f"  [{tag}] opt.ini: {old} → {mode}", flush=True)

    dlg = simctl.blocking_dialog(pid)
    if dlg:
        rec["note"] = f"游戏有弹窗：{dlg[1]}"
        return rec

    r = simctl.recompile()
    if not r.ok:
        rec["note"] = f"重编译失败：{r.detail}"
        return rec
    num = wait_new_product(prev, args.compile_timeout)
    if num is None:
        rec["note"] = f"等了 {args.compile_timeout:.0f}s 没等到新产物（编译失败？）"
        return rec
    rec["num"] = num
    print(f"  [{tag}] 新产物 #{num}", flush=True)

    r = simctl.run()
    if not r.ok:
        rec["note"] = f"启动失败：{r.detail}"
        return rec
    try:
        rec["hz"] = sample_freq(h, base, args.seconds, args.warmup)
    finally:
        simctl.stop()
    if not rec["hz"]:
        rec["note"] = "没有有效采样（仿真没跑起来？）"
    return rec


# ---------------------------------------------------------------- 主流程

def summarize(rows: list[dict]) -> tuple[list[str], float | None]:
    """把每个模式的多轮采样合成一行；相对基线的百分比按中位数算。"""
    base_hz = None
    for r in rows:
        if r["hz"]:
            base_hz = statistics.median(r["hz"])
            break                       # 第一行（默认是 off）当基线
    out: list[str] = []
    head = (f"{'模式':<10}{'主频(中位)':>12}{'最大':>10}{'样本':>6}"
            f"{'指纹':>11}{'改写':>8}  相对基线")
    out.append(head)
    for r in rows:
        if not r["hz"]:
            out.append(f"{r['mode']:<10}{'—':>12}{'—':>10}{0:>6}"
                       f"{'—':>11}{'—':>8}  {r['note'] or '无数据'}")
            continue
        med = statistics.median(r["hz"])
        mx = max(r["hz"])
        rel = "" if base_hz in (None, 0) else f"{(med / base_hz - 1) * 100:+.1f}%"
        fp, rewritten = "—", "—"
        if r["num"]:
            fp = W.product_fp(r["num"])
            _f, rewritten, _w = W.opt_info(r["num"])
        out.append(f"{r['mode']:<10}{med / 1e6:>9.3f}MHz{mx / 1e6:>7.3f}MHz"
                   f"{len(r['hz']):>6}{fp:>11}{rewritten:>8}  {rel}")
        if r["note"]:
            out.append(f"{'':<10}{r['note']}")
    return out, base_hz


def main() -> int:
    ap = argparse.ArgumentParser(
        description="自动测当前电路在各 rewrite 配置下的主频",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(__doc__ or "").split("用法\n----\n")[-1])
    ap.add_argument("--modes", default=",".join(ALL_MODES),
                    help=f"要测的模式与顺序，逗号分隔（默认 {','.join(ALL_MODES)}）")
    ap.add_argument("--seconds", type=float, default=8.0,
                    help="每个模式采样几秒（默认 8）")
    ap.add_argument("--warmup", type=float, default=1.5,
                    help="丢掉开头几秒的过渡值（默认 1.5）")
    ap.add_argument("--repeat", type=int, default=1,
                    help="每个模式重复几轮（默认 1）")
    ap.add_argument("--compile-timeout", type=float, default=60.0,
                    help="等一次重编译完成的超时秒数（默认 60）")
    ap.add_argument("--dry-run", action="store_true", help="只打印计划，不动游戏/配置")
    args = ap.parse_args()

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    bad = [m for m in modes if m not in ALL_MODES]
    if bad:
        print(f"不认识的模式：{', '.join(bad)}（可选 {', '.join(ALL_MODES)}）",
              file=sys.stderr)
        return 2

    print(f"计划：{' → '.join(modes)}，每个 {args.seconds:g}s（丢掉前 {args.warmup:g}s）"
          f"，重复 {args.repeat} 轮")
    if args.dry_run:
        for m in modes:
            for i in range(args.repeat):
                print(f"  [第{i + 1}轮] rewrite={m} → 重编译 → 运行 {args.seconds:g}s → 停止")
        print("（--dry-run，未执行）")
        return 0

    pid = winmem.find_pid(EXE)
    if not pid:
        print(f"{EXE} 没在运行", file=sys.stderr)
        return 1
    base, _path = winmem.find_module(pid, EXE)
    if base is None:
        print("找不到游戏模块（Steam 更新过？符号偏移要重新取）", file=sys.stderr)
        return 1
    h = winmem.open_process(pid)
    print(f"游戏 PID {pid}   模块基址 0x{base:X}")

    if read_hz(h, base) is None:
        print("读不到 sim_speed（权限或偏移过时）", file=sys.stderr)
        return 1

    OPT_INI.parent.mkdir(parents=True, exist_ok=True)
    BACKUP.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(OPT_INI, BACKUP)
    print(f"opt.ini 已备份到 {BACKUP}")

    rows: list[dict] = []
    t_start = time.time()
    for m in modes:
        for i in range(args.repeat):
            tag = f"{m} {i + 1}/{args.repeat}" if args.repeat > 1 else m
            if winmem.find_pid(EXE) != pid:
                print("⚠ 游戏进程变了（重启过？）—— 内存偏移全部失效，中止",
                      file=sys.stderr)
                rows.append({"mode": m, "hz": [], "num": None,
                             "note": "游戏进程变了，中止"})
                break
            try:
                rows.append(bench_one(m, args, pid, base, h, tag))
            except KeyboardInterrupt:
                print("\n中断，输出已完成的部分：", file=sys.stderr)
                break
    print(f"\n耗时 {time.time() - t_start:.0f}s\n")

    lines, _base_hz = summarize(rows)
    print("\n".join(lines))

    BENCH_DIR.mkdir(parents=True, exist_ok=True)
    out = BENCH_DIR / f"bench_{datetime.now():%Y%m%d_%H%M%S}.tsv"
    try:
        with open(out, "w", encoding="utf-8") as f:
            f.write("# bench_rewrite.py —— 每个模式一次编译 → 一次采样\n")
            f.write("mode\tnum\tfp\tmedian_hz\tmax_hz\tsample_n\tnote\tsamples\n")
            for r in rows:
                fp = W.product_fp(r["num"]) if r["num"] else ""
                med = statistics.median(r["hz"]) if r["hz"] else 0
                mx = max(r["hz"]) if r["hz"] else 0
                samples = ";".join(f"{v:.0f}" for v in r["hz"])
                f.write(f"{r['mode']}\t{r['num'] or ''}\t{fp}\t{med:.0f}\t{mx:.0f}\t"
                        f"{len(r['hz'])}\t{r['note']}\t{samples}\n")
    except OSError as e:
        print(f"写结果失败：{e}", file=sys.stderr)
        return 1
    print(f"\n结果已写入 {out}")
    print(f"opt.ini 现为最后一个模式（原文件备份在 {BACKUP}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
