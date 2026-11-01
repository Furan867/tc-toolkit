#!python3
"""常驻采样器（`tools/watch_freq.py` 的实现）—— 被动记录实测主频并标出编译点。

与旧 `watch_freq.py` **输出逐字符一致**（终端、`work/freq/freq_log.tsv`、
`work/freq/freq_summary.txt`）：这里只是把采样循环、分段、日志挪进了
`tools/freq/{sampler,report,products}.py`，口径不变。

读哪里
------
**只看退出时那份总结**（终端上就是，另外还落一份无色的 `work/freq/freq_summary.txt`）。
逐事件的 `work/freq/freq_log.tsv` 只给脚本用，别手动读它 —— 监听几分钟就能摞几百行。

用法
----
    py tools/watch_freq.py                  # Ctrl-C 停
    py tools/watch_freq.py --seconds 60     # 记 60 秒后自动收尾
    py tools/watch_freq.py -v               # 总结里附上逐段明细
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from datetime import datetime

from .. import tc_common, term, winmem
from . import products, report, sampler

EXE = tc_common.GAME_EXE_NAME


def _say_line(on_heartbeat: list[bool]):
    """把一行输出让出来（若上一条是原地刷新的心跳行）。"""
    if on_heartbeat[0]:
        sys.stdout.write("\n")
        on_heartbeat[0] = False


def _compile_line(num: str | None, starter: bool, sink: sampler.Sink, say) -> None:
    """打一条编译点（启动时补报的那条标为「起始」）并落日志。

    信号取自**改写后产物** NNN.opt.bin 出现，所以该有的都有：指纹 = 实际写回游戏那份
    机器码的 sha256 前 8 位；改写处数 / rewrite = `hook.log` 里实际执行的值。若转发层
    记的配置值与实际执行值不同，直接把差异标出来（那就是要查的信号）。
    """
    tag = "起始编译" if starter else "编译点"
    when = datetime.now().strftime("[%H:%M:%S]")
    who = f"#{num}" if num else "?"
    fp = products.product_fp(num)
    _logfp, rewritten, actual = products.opt_info(num)
    cfg = products.mode_from_hook_log(num)
    if actual == "?":
        rewrite = cfg if cfg != "?" else "?"
    elif cfg != "?" and cfg != actual:
        rewrite = f"{actual}（hook.log 记 {cfg}）"
    else:
        rewrite = actual
    say(f"{term.paint(when, 'blue')}  {term.paint('▣ ' + tag, 'bold', 'magenta')} "
        f"{term.paint(who, 'bold')}  {term.paint('指纹 ' + fp, 'blue')}"
        + (f"  {term.paint(rewritten, 'green')}" if rewritten else "")
        + f"  {term.paint('rewrite=' + rewrite, 'cyan')}")
    sink.write("compile", note=f"{tag} {who} 指纹={fp} {rewritten} rewrite={rewrite}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="被动采样游戏实测主频")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="两次样本的最小间隔秒（默认 1.0，与游戏重算节奏一致；再密不产生新信息）")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="记录指定秒数后自动收尾（默认 0 = 一直记到 Ctrl-C）")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="总结里附上每个区间的逐段明细")
    args = ap.parse_args(argv)

    sink = sampler.Sink(report.LOG_PATH)
    pid, base = _wait_for_game()
    h = winmem.open_process(pid)
    sink.write("session", note=f"开始记录 PID={pid} 基址=0x{base:X}")
    print(f"{term.paint('● 已连接', 'bold', 'green')}  PID {term.paint(str(pid), 'bold')}  "
          f"基址 {term.paint(f'0x{base:X}', 'blue')}  "
          f"{term.paint('总结会写进 work/freq/freq_summary.txt  ·  Ctrl-C 结束', 'cyan')}\n")

    regions: list[sampler.Region] = []
    on_heartbeat = [False]

    def say(text: str) -> None:
        _say_line(on_heartbeat)
        print(text, flush=True)

    def new_region(label: str, num: str | None = None) -> None:
        """开一个新区间，只记编号 —— 指纹/模式等摘要时再查。"""
        regions.append(sampler.Region(label, num))

    def report_compile(num: str | None, starter: bool = False) -> None:
        _compile_line(num, starter, sink, say)

    def on_segment(segment: sampler.Segment) -> None:
        """段收尾时不打印，只记账：落一行日志 + 存进当前区间（退出时按区间归并）。"""
        region = regions[-1]
        region.add(segment)
        sink.write("segment", hz=None if segment.median == 0 else segment.median,
                   note=f"{region.label} n={segment.count} 时长={segment.duration:.0f}s "
                        + ("停机" if segment.median == 0
                           else f"中位数={segment.median:.0f}Hz 波动={segment.spread:.1f}%")
                        + (" 过渡" if segment.transient else ""))

    seg = sampler.Segmenter(on_segment)
    clock_now = sampler.Sampler(h, base, interval=args.interval)

    seen_hook: set[str] = set()       # hook_out_*.bin —— 只用来发现「编译发生了」
    seen_opt: set[str] = set()        # NNN.opt.bin   —— 改写走完，用它报编译点
    pending: dict[str, float] = {}    # 见过 hook_out、还没等到 opt 的编号
    products.scan_captures(seen_hook)
    products.scan_products(seen_opt)  # 首次只登记现状，不重复报成「新编译」
    starter = products.latest_product()
    if starter:
        report_compile(starter.num, starter=True)
        new_region(f"#{starter.num}", starter.num)
    else:
        new_region("起始")

    deadline = time.time() + args.seconds if args.seconds > 0 else None

    try:
        while True:
            if deadline and time.time() >= deadline:
                break
            time.sleep(min(sampler.POLL, args.interval))
            clock = datetime.now().strftime("[%H:%M:%S]")

            # 两条腿：hook_out 出现 = 编译发生了；opt.bin 出现 = 改写走完了。
            # 编译点以 opt 为准（那时指纹与日志都就绪），hook_out 只用来记待办，
            # 免得改写器压根没产出时这里一声不响。
            for name in products.scan_captures(seen_hook):
                m = re.search(r"(\d+)", name)
                if m:
                    pending.setdefault(m.group(1), time.time())
            for num in products.scan_products(seen_opt):
                pending.pop(num, None)
                seg.close()           # 先让旧段落进旧区间
                report_compile(num)
                new_region(f"#{num}", num)
                clock_now.rearm()
            for num, t0 in list(pending.items()):
                if time.time() - t0 > sampler.OPT_WAIT:
                    pending.pop(num)
                    seg.close()
                    say(f"{term.paint(clock, 'blue')}  "
                        f"{term.paint(f'❌ 编译点 #{num}：改写后产物一直没出现', 'red')}"
                        f"  {term.paint('（看 work/opt/opt_stderr.log）', 'yellow')}")
                    sink.write("compile",
                               note=f"#{num} 改写后产物缺失（超时 {sampler.OPT_WAIT:.0f}s）")
                    new_region(f"#{num}", num)
                    clock_now.rearm()

            if not winmem.find_pid(EXE):
                sink.write("game", note="游戏已退出，等待重新启动")
                say(f"{term.paint(clock, 'blue')}  "
                    f"{term.paint('○ 游戏退出，等待重启 …', 'yellow')}")
                pid, base = _wait_for_game()
                h = winmem.open_process(pid)
                clock_now = sampler.Sampler(h, base, interval=args.interval)
                seen_hook.clear()
                seen_opt.clear()
                pending.clear()
                products.scan_captures(seen_hook)
                products.scan_products(seen_opt)
                sink.write("game", note=f"重连 PID={pid} 基址=0x{base:X}")
                continue

            sample = clock_now.poll()
            if sample is None:
                continue
            hz = sample.hz

            started = seg.add(sample.t, hz)
            if hz == 0:
                # 停机心跳与采样同频，但**原地刷新同一行**，所以不会刷屏。
                sys.stdout.write("\r" + term.paint(
                    f"{clock}  · 仍在停机（已 {seg.count()}s）", "yellow"))
                sys.stdout.flush()
                on_heartbeat[0] = True
                continue
            mark = term.paint("▲", "green") if started else " "
            style = report.hz_style(hz)
            say(f"{term.paint(clock, 'blue')} {mark} {term.paint(report.fmt_hz(hz), *style)}"
                f" {term.paint(report.bar(hz), *style)}")
    except KeyboardInterrupt:
        print()
    finally:
        _say_line(on_heartbeat)
        seg.close()
        report.print_summary(regions, sink, args.verbose)
        sink.write("session", note="结束记录")
        sink.close()
        print(term.paint("■ 已停止", "bold", "green"))
    return 0


def _wait_for_game() -> tuple[int, int]:
    """等游戏出现（重连用）。文案与旧版一致。"""
    hinted = False
    while True:
        pid = winmem.find_pid(EXE)
        if pid:
            base, _path = winmem.find_module(pid, EXE)
            if base:
                return pid, base
        if not hinted:
            print(term.paint(f"等 {EXE} 启动 …（只读内存，不影响游戏）", "yellow"),
                  flush=True)
            hinted = True
        time.sleep(2.0)


if __name__ == "__main__":
    sys.exit(main())
