#!python3
"""自动测当前电路在各 rewrite 配置下的主频，一次跑完并出对照表。

前提
----
- 游戏在跑；
- **撤销栈**：直接用 simctl 的 auto 交替 `recompile()`（不传 key）—— 它按净消耗
  在 undo/redo 之间交替，退出时 restore_stack 把账补平，栈守恒（长测试中途也
  不会一路推到底）。不需要调用方自己做配平。
- 但栈仍需**非空**：实测 Ctrl+Z 在栈空后就不再触发编译（此前“栈空也能触发”的判断是
  错的，那时栈里还有货）。栈空了先去游戏里做几次编辑。
- 测的过程中会**抢游戏窗口前台**（SendInput 有前台校验，不抢就不发键），
  期间别切走窗口。

每个模式一遍
------------
    写 opt.ini  →  stop() 保底  →  recompile()  →  等 NNN.opt.bin 出现
    →  stop() 保底  →  simctl.run()  →  采样  →  simctl.stop()

⚠ 每次 opt.ini 都改过，所以 auto 交替的 undo / redo **两个方向都会真的重编译**
（游戏的编译缓存是按电路内容存的，内容没变才命中）—— 一次切换一次编译。

⚠ simctl 的 run() 是注入 **F6（运行/暂停切换）**，不是幂等的「确保在运行」：
所以每次开跑前先按 F4（stop，幂等）把状态摁到「停止」，再发 F6 才等于「运行」。
连发两次 F6 等于没发 —— 这是写内存那版（commands[0]=0/3）与按键版的语义差别。

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

def latest_product() -> tuple[str | None, float]:
    """work/opt 里编号最大的产物：返回 (编号, 它的 mtime)。"""
    best, bt = None, 0.0
    for p in OPT_DIR.glob("*.opt.bin"):
        m = PRODUCT_RE.match(p.name)
        if m and (best is None or int(m.group(1)) > int(best)):
            best, bt = m.group(1), p.stat().st_mtime
    return best, bt


def hook_lines() -> list[str]:
    """转发层日志（每发生一次编译多 3 行：rewrite= / 已就地写回 / 入口偏移已更新）。

    用它判断「发出去的按键到底有没有触发编译」—— 光看 simctl.recompile() 的 ok
    不行，它只表示「键发出去了」（SendInput 是异步的，游戏那边不反馈）。
    """
    try:
        return (OPT_DIR / "hook.log").read_text(
            encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def wait_new_product(prev: str | None, timeout: float, hook_before: int,
                     tag: str, since: float,
                     dead_after: float = 6.0) -> tuple[str | None, str]:
    """等**本次发键之后**产生的新产物，返回 (编号, 失败原因)。

    必须同时满足「编号更大」与「mtime >= since」：只比编号会误判 —— 上一次运行
    遗留的产物编号就比 prev 大，会被 0.0s 认领（实测 #031 就这样被认成 off 那轮的
    产物，其实那轮根本没编译，游戏没重新加载电路，采样全 0）。
    另外一路盯 hook.log：发键后 dead_after 秒内一条新编译行都没有，就断定按键没
    生效（焦点不在电路编辑区 / 抢前台后第一次发键丢掉），立刻失败。
    """
    t0 = time.time()
    last = 0.0
    while True:
        cur, mt = latest_product()
        if cur and (prev is None or int(cur) > int(prev)) and mt >= since:
            print(f"  [{tag}] 产物 #{cur} 已就绪（{time.time() - t0:.1f}s）", flush=True)
            return cur, ""
        t = time.time() - t0
        if t >= timeout:
            return None, f"等了 {timeout:.0f}s 没等到本次编译的产物"
        new_lines = len(hook_lines()) - hook_before
        if new_lines <= 0 and t >= dead_after:
            return None, (f"发键后 {dead_after:.0f}s 内 hook.log 没有新编译行 —— "
                          "三种可能：① 该（电路内容 × opt.ini 模式）组合已经编译过，"
                          "游戏有编译缓存、同一组合第二次不再编译（换模式，或先在游戏里"
                          "做一次编辑来产生新内容）；② 撤销栈空了；"
                          "③ 抢前台后第一次发键丢了（会自动重试）")
        if t - last >= 3.0:
            last = t
            what = (f"编译已触发（hook.log +{new_lines} 行），等改写子进程"
                    if new_lines > 0 else "还没看到编译行")
            print(f"  [{tag}] 等产物 {t:4.0f}s … {what}", flush=True)
        time.sleep(0.3)


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
    prev, _mt = latest_product()
    old = set_rewrite_ini(mode)
    print(f"  [{tag}] opt.ini: {old} → {mode}", flush=True)

    dlg = simctl.blocking_dialog(pid)
    if dlg:
        rec["note"] = f"游戏有弹窗：{dlg[1]}"
        return rec

    # run() 是 F6 切换键，只有从「停止」状态出发才等于「运行」；
    # stop() 是 F4（幂等），多按无副作用。
    print(f"  [{tag}] stop(F4) 保底，把状态摁到「停止」", flush=True)
    simctl.stop()

    # 用 simctl 的 auto 交替（不传 key）：它按净消耗在 undo/redo 间交替，退出时
    # restore_stack 补平，栈守恒。这两个方向都会触发编译 —— 因为每次 opt.ini 都
    # 改过，游戏的编译缓存（按电路内容存）都失效了。
    # 发键后第一次常常没反应（刚抢到前台那一瞬）—— 重试几次。
    num, why = None, ""
    for attempt in range(1, 4):
        hook_before = len(hook_lines())
        since = time.time()
        note = "" if attempt == 1 else f"（第 {attempt} 次尝试）"
        print(f"  [{tag}] recompile() 触发重编译{note}", flush=True)
        r = simctl.recompile()      # auto 交替：由 simctl 按净消耗决定发 undo 还是 redo
        if not r.ok:
            why = f"重编译失败：{r.detail}"
            print(f"  [{tag}] ✗ {why}", flush=True)
            time.sleep(1.0)
            continue
        num, why = wait_new_product(prev, args.compile_timeout, hook_before, tag,
                                    since=since)
        if num:
            break
        print(f"  [{tag}] 这次没触发编译（{why}），重试", flush=True)
        time.sleep(1.0)
    if num is None:
        rec["note"] = why
        print(f"  [{tag}] ✗ 试了 3 次都没能触发编译：{why}", flush=True)
        print("     提醒：每次测试都需要一个『没编译过』的（电路内容 × opt.ini 模式）组合。"
              "auto 交替只在 A/B 两个内容间摆动，所以同一模式最多测 2 次；"
              "要接着测就得先在游戏里做一次编辑（产生新内容）。", flush=True)
        return rec
    rec["num"] = num
    print(f"  [{tag}] 新产物 #{num}", flush=True)

    # recompile 靠撤销电路触发，游戏自己会停；但状态别假设，再按一次 F4 保底。
    print(f"  [{tag}] stop(F4) 保底 → run(F6) 开跑", flush=True)
    simctl.stop()
    time.sleep(0.6)                     # 让游戏把 F4 处理完，免得 F6 撞上旧状态
    r = simctl.run()
    if not r.ok:
        rec["note"] = f"启动失败：{r.detail}"
        print(f"  [{tag}] ✗ {rec['note']}", flush=True)
        return rec
    try:
        rec["hz"] = sample_freq(h, base, args.seconds, args.warmup)
    finally:
        simctl.stop()
    if not rec["hz"]:
        rec["note"] = "没有有效采样（仿真没跑起来？F6 状态不对 / 编译失败）"
        print(f"  [{tag}] ✗ {rec['note']}", flush=True)
    else:
        med = statistics.median(rec["hz"])
        print(f"  [{tag}] 采样 {len(rec['hz'])} 点，中位数 {med / 1e6:.3f} MHz",
              flush=True)
    return rec


# ---------------------------------------------------------------- 主流程

def summarize(rows: list[dict]) -> tuple[list[str], float | None]:
    """把每个模式的多轮采样合成一行；相对基线的百分比按中位数算。"""
    base_hz = None
    if rows and rows[0]["hz"] and statistics.median(rows[0]["hz"]) >= 1e5:
        base_hz = statistics.median(rows[0]["hz"])   # 基线只认**第一个模式**（默认 off）
    # 它失败、或读到的是停机附近的噪声（<0.1MHz）时百分比留空，不拿别的顶上、
    # 也不拿接近 0 的值当分母。
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
    # **交错**顺序（轮次在外、模式在内）：主频会随时间漂移（实测同一产物两次跑出
    # 4.49 / 4.29 MHz，疑似 CPU 升温降频），块状重复会让漂移全落在后测的模式上。
    for i in range(args.repeat):
        for m in modes:
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
