#!python3
"""自动测当前电路在各变体下的主频（`tools/bench_rewrite.py` 的实现）。

变体轴是 `compiler:rewrite`（§4）——旧调用 `-m on,short` 等价于「编译器不动、
只换 rewrite」，新调用 `--variant ref:off --variant ours:off` 一次对比两个实现。

前提
----
- 游戏在跑（`driver.preflight()` 会逐条查：游戏在跑 / PID 稳定 / 撤销栈非空 /
  能抢前台 / 无阻塞弹窗 / 偏移可读）；
- **撤销栈**：直接用 `driver.recompile()`（不传 key）——它按净消耗在 undo/redo 之间
  交替，退出时补平，栈守恒；但栈仍须**非空**（栈空后 Ctrl+Z 静默无效）；
- 测的过程中会**抢游戏窗口前台**（SendInput 有前台校验），期间别切走窗口。

每个变体一遍
------------
    写 opt.ini → stop() 保底 → recompile() → 等 NNN.opt.bin 出现
    → stop() 保底 → run() → 采样 → stop()

⚠ `driver.run()` 是 F6（运行/暂停**切换**），不是幂等的「确保在运行」：所以每次开跑
前先按 F4（stop，幂等）把状态摁到「停止」，再发 F6 才等于「运行」。连发两次 F6 等于没发。

主频取自游戏每秒重算的 `sim_speed`（float32 Hz），丢掉开头 warmup 秒的过渡值，
**记中位数**（决策 3：bench 不再用 mean）。

用法
----
    py tools/bench_rewrite.py                        # off,on,short,short_cse 各 8s
    py tools/bench_rewrite.py -m on,short            # 选模式与顺序
    py tools/bench_rewrite.py -s 12 -r 2             # 每个模式 12 个点、重复 2 轮
    py tools/bench_rewrite.py -n                     # 只打印计划，不动游戏与配置
    py tools/bench_rewrite.py --variant ref:off --variant ours:off --json out.json

    参数：-m/--modes  -s/--seconds  -d/--drop  -r/--repeat
          -t/--compile-timeout  -n/--dry-run
          --variant compiler:rewrite（可重复）  --json FILE

结果同时写到 `work/freq/bench_<时间戳>.tsv`（含每个采样点原始值）；`--json` 另出一份
`schema=1` 的机器读报告。opt.ini 会停在**最后一个变体**，原文件备份在
`work/tmp/opt.ini.bak`。
"""
from __future__ import annotations

import argparse
import atexit
import shutil
import sys
import time
from datetime import datetime
from pathlib import Path

from .. import tc_common, term, winmem
from . import driver, probe, products, report, sampler, variants
from .variants import Variant

EXE = tc_common.GAME_EXE_NAME
BENCH_DIR = report.FREQ_DIR
OPT_INI = variants.OPT_INI
BACKUP = report.ROOT / "work" / "tmp" / "opt.ini.bak"

_USAGE = """\
    py tools/bench_rewrite.py                        # off,on,short,short_cse 各 8s
    py tools/bench_rewrite.py -m on,short            # 选模式与顺序
    py tools/bench_rewrite.py -s 12 -r 2             # 每个模式 12 个点、重复 2 轮
    py tools/bench_rewrite.py -n                     # 只打印计划，不动游戏与配置
    py tools/bench_rewrite.py --variant ref:off --variant ours:off --json out.json

    参数：-m/--modes  -s/--seconds  -d/--drop  -r/--repeat
          -t/--compile-timeout  -n/--dry-run
          --variant compiler:rewrite（可重复）  --json FILE

结果同时写到 `work/freq/bench_<时间戳>.tsv`（含每个采样点原始值）；`--json` 另出一份
`schema=1` 的机器读报告。opt.ini 会停在**最后一个变体**，原文件备份在
`work/tmp/opt.ini.bak`。
"""


def _bench_one(variant: Variant, args, pid: int, base: int, h: int,
               tag: str) -> report.BenchRow:
    """走一遍：换配置 → 重编译 → 运行 → 采样 → 停止。"""
    row = report.BenchRow(mode=variant.rewrite, num=None, hz=[],
                          compiler=variant.compiler)
    prev = products.latest_product()
    variants.set_variant(variant)

    dlg = driver.blocking_dialog(pid)
    if dlg:
        row.note = f"游戏有弹窗：{dlg[1]}"
        return row

    driver.stop()                       # F4 幂等，把状态摁到「停止」

    # 发键后第一次常常没反应（刚抢到前台那一瞬）—— 重试几次。
    num, why = None, ""
    for attempt in range(1, 4):
        hook_before = len(products.hook_log_lines())
        since = time.time()
        if attempt > 1:
            print(f"  [{tag}] 重试第 {attempt} 次", flush=True)
        r = driver.recompile()          # auto 交替：由 simctl 按净消耗决定 undo / redo
        if not r.ok:
            why = f"重编译失败：{r.detail}"
            print(f"  [{tag}] ✗ {why}", flush=True)
            time.sleep(1.0)
            continue
        num, why = products.wait_new_product(
            prev.num if prev else None, args.compile_timeout, hook_before, tag, since=since)
        if num:
            break
        time.sleep(1.0)
    if num is None:
        row.note = why
        print(f"  [{tag}] ✗ 试了 3 次都没能触发编译：{why}", flush=True)
        print("     提醒：撤销/重做只要有变化，游戏就会重新编译 —— 拿不到新产物通常"
              "是按键没送到游戏（别让游戏最小化、别把前台抢走），或撤销栈已空。",
              flush=True)
        return row
    row.num = num

    # recompile 靠撤销电路触发，游戏自己会停；但状态别假设，再按一次 F4 保底。
    driver.stop()
    time.sleep(0.6)                     # 让游戏把 F4 处理完，免得 F6 撞上旧状态
    r = driver.run()
    if not r.ok:
        row.note = f"启动失败：{r.detail}"
        print(f"  [{tag}] ✗ {row.note}", flush=True)
        return row
    try:
        row.hz = [s.hz for s in sampler.collect(h, base, args.seconds, drop=args.drop)]
    finally:
        driver.stop()
    own = products.ownership(num)
    row.impl = own.impl
    row.fp = products.product_fp(num)     # 现算一次，表格与 TSV 共用同一份
    if not row.hz:
        row.note = "没有有效采样（仿真没跑起来？F6 状态不对 / 编译失败）"
        print(f"  [{tag}] ✗ {row.note}", flush=True)
    elif row.median == 0:
        # 停机哨兵（facts.md §采样判据 ①）—— F6 状态不对时全采到 0，别当成「跑得慢」。
        row.note = "采样全是 0（停机哨兵）—— F6 没把仿真跑起来"
        print(f"  [{tag}] ✗ {row.note}", flush=True)
    else:
        med = row.median
        print(f"  {report.pad(f'[{tag}]', 12)}"
              f"{term.paint(report.pad(f'#{num}', 6), 'bold', 'magenta')}"
              f"{report.pad(f'{len(row.hz)}x', 4)}"
              f"{term.paint(f'{med / 1e6:.3f}', *report.hz_style(med))}", flush=True)
    return row


def _wanted_variants(args) -> list[Variant]:
    """`--variant` 优先；否则退到旧 `-m`：编译器保持不变、只换 rewrite。"""
    if args.variant:
        return [Variant.parse(v) for v in args.variant]
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    known = set(variants.REWRITES)
    bad = [m for m in modes if m not in known]
    if bad:
        raise SystemExit(f"不认识的模式：{', '.join(bad)}（可选 {', '.join(variants.REWRITES)}）")
    cur = variants.read_variant()       # 保持用户 opt.ini 里的 compiler 不变
    return [Variant(cur.compiler, m) for m in modes]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="自动测当前电路在各变体（compiler × rewrite）下的主频",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_USAGE)
    ap.add_argument("-m", "--modes", default=",".join(variants.REWRITES),
                    help=f"要测的 rewrite 模式与顺序，逗号分隔（默认 {','.join(variants.REWRITES)}）")
    ap.add_argument("-s", "--seconds", type=float, default=8.0,
                    help="每个变体采几个点、也就是几秒（默认 8；游戏每秒重算一次）")
    ap.add_argument("-d", "--drop", type=int, default=1,
                    help="丢弃开头的几个采样点（默认 1；第一个点落在启动切换窗口里）")
    ap.add_argument("-r", "--repeat", type=int, default=1,
                    help="每个变体重复几轮（默认 1）")
    ap.add_argument("-t", "--compile-timeout", type=float, default=60.0,
                    help="等一次重编译完成的超时秒数（默认 60）")
    ap.add_argument("-n", "--dry-run", action="store_true",
                    help="只打印计划，不动游戏/配置")
    ap.add_argument("--variant", action="append", metavar="COMPILER:REWRITE", default=None,
                    help="要测的变体，如 `ours:off`；可重复给多个（优先于 -m）")
    ap.add_argument("--json", metavar="FILE", default=None,
                    help="把 schema=1 的机器读报告写到这个文件")
    args = ap.parse_args(argv)

    wanted = _wanted_variants(args)
    plan = [str(v) for v in wanted]
    print(f"计划：{' → '.join(plan)}，每个 {int(args.seconds)} 个点"
          f"（丢开头 {args.drop} 个），重复 {args.repeat} 轮")
    if args.dry_run:
        for i in range(args.repeat):          # 与实际运行的次序保持一致（轮次在外）
            for v in wanted:
                print(f"  [第{i + 1}轮] {v} → 重编译 → 运行 "
                      f"{int(args.seconds)} 个点 → 停止")
        print("（--dry-run，未执行）")
        return 0

    checks = driver.preflight()
    for c in checks:
        print(f"  {c}")
    blocked = [c for c in checks if c.verifiable and not c.ok]
    if blocked:
        print(f"\n前置断言不过（{blocked[0].name}）—— 先按上面的 → 处理，再重跑。",
              file=sys.stderr)
        return 1

    found = probe.find_game()
    if found is None:
        print(f"{EXE} 没在运行", file=sys.stderr)
        return 1
    pid, base = found
    h = winmem.open_process(pid)
    print(f"游戏 PID {pid}   模块基址 0x{base:X}")

    OPT_INI.parent.mkdir(parents=True, exist_ok=True)
    BACKUP.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(OPT_INI, BACKUP)
    # 退出时（正常 / 异常 / Ctrl-C）都把 opt.ini 恢复成开始时的样子
    atexit.register(lambda: shutil.copyfile(BACKUP, OPT_INI))
    print(f"opt.ini 已备份到 {BACKUP}")

    rows: list[report.BenchRow] = []
    t_start = time.time()
    # **交错**顺序（轮次在外、变体在内）：主频会随时间漂移（实测同一产物两次跑出
    # 4.49 / 4.29 MHz，疑似 CPU 升温降频），块状重复会让漂移全落在后测的变体上。
    for i in range(args.repeat):
        for v in wanted:
            tag = f"{v} {i + 1}/{args.repeat}" if args.repeat > 1 else str(v)
            if winmem.find_pid(EXE) != pid:
                print("⚠ 游戏进程变了（重启过？）—— 内存偏移全部失效，中止",
                      file=sys.stderr)
                rows.append(report.BenchRow(mode=v.rewrite, num=None, hz=[],
                                            compiler=v.compiler,
                                            note="游戏进程变了，中止"))
                break
            try:
                rows.append(_bench_one(v, args, pid, base, h, tag))
            except KeyboardInterrupt:
                print("\n中断，输出已完成的部分：", file=sys.stderr)
                break
    print(f"\n耗时 {time.time() - t_start:.0f}s\n")

    lines, _base_hz = report.bench_table(rows)
    print("\n".join(lines))

    BENCH_DIR.mkdir(parents=True, exist_ok=True)
    out = BENCH_DIR / f"bench_{datetime.now():%Y%m%d_%H%M%S}.tsv"
    try:
        report.write_bench_tsv(out, rows)
    except OSError as e:
        print(f"写结果失败：{e}", file=sys.stderr)
        return 1
    print(f"\n结果已写入 {out}")

    if args.json:
        machine = report.build_report(rows)
        try:
            machine.write_json(Path(args.json))
        except OSError as e:
            print(f"写 JSON 失败：{e}", file=sys.stderr)
            return 1
        print(f"JSON 报告（schema={machine.schema}）已写入 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
