"""人读表 + JSON 报告 —— 排版、区间表、对照表的**唯一一处**。

两层输出（设计 §4「输出两层」）：

    人读    终端表格：① **区间表**（watch 的总结：一次编译 → 一次最大值）与
            ② **对照表**（bench 的 A/B：每个变体一行）。同一条口径 = **median**。
    机器读  `Report`（`schema = 1`）：`cli_bench --json` 写的那个文件，判据入口只消费它。

排版三件套的约束：**对齐用显示宽度、着色一律放在对齐之后** —— ANSI 转义不占显示
宽度，但 `len()` 会把它算进去，先着色再 `pad` 表格就歪了（`term.paint` 的说明）。
"""
from __future__ import annotations

import json
import re
import statistics
import unicodedata
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from libs import term
from . import products, sampler
from .sampler import Region

ROOT = Path(__file__).resolve().parents[2]
FREQ_DIR = ROOT / "work" / "freq"      # 采样数据，跨会话保留（不会被人清掉）
LOG_PATH = FREQ_DIR / "freq_log.tsv"
SUMMARY_PATH = FREQ_DIR / "freq_summary.txt"

BAR_UNIT_HZ = 0.7e6      # 柱状图每格代表的频率（满 14 格 ≈ 9.8 MHz）
BAR_WIDTH = 14

SCHEMA = 1               # JSON 报告的 schema 版本（`Report.schema`）

#: 归属占位符 —— 与 `products.UNKNOWN` 同一个意思，这里给表格用的短占位。
DASH = "—"


# ---------------------------------------------------------------- 排版

def disp_width(text: str) -> int:
    """终端显示宽度 —— 中日韩全角字符占 2 列，len() 会算少，表格就歪了。"""
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in text)


def pad(text: str, width: int) -> str:
    """按**显示宽度**左对齐补齐（对齐必须在着色之前完成）。"""
    return text + " " * max(0, width - disp_width(text))


def pad_left(text: str, width: int) -> str:
    """按**显示宽度**右对齐补齐 —— str.rjust 按字符数算，中文（占 2 列）会多补。"""
    return " " * max(0, width - disp_width(text)) + text


def fmt_hz(hz: float) -> str:
    """终端用的频率串，固定 13 显示列宽。"""
    if hz == 0:
        return pad("停机", 13)
    return f"{hz / 1e6:9.3f} MHz"


def bar(hz: float) -> str:
    """固定 BAR_WIDTH 显示列的柱状条（未着色）。

    非零值**至少给一格** —— 电路自带的速度上限可能只有几 kHz，按比例算柱子
    会全空，那就等于没有信息（实测电路 B 限 10 kHz 时就是这情况）。
    """
    if hz == 0:
        return " " * BAR_WIDTH
    n = max(1, min(BAR_WIDTH, int(hz / BAR_UNIT_HZ)))
    return "▇" * n + " " * (BAR_WIDTH - n)


def hz_style(hz: float) -> tuple[str, ...]:
    """频率与柱子的配色：低保真频率（多半是电路自带限速）用黄，正常用绿。"""
    return ("bold", "yellow") if sampler.is_low(hz) else ("bold", "green")


# ---------------------------------------------------------------- 区间表（watch 总结）

def _rewrite_cell(own: products.Ownership) -> str:
    """改写列：改写器实际做的（`opt` 行）→ 转发层配的（`cfg` 行）→ 这一趟压根没起改写器。

    第三档的必要性：`impl=ours` 的产物不过改写器（`hook.log` 里它的终态就是 `impl` 行，
    见 `products.done_rounds`），既没有 `opt` 也没有 `cfg` 行；只写 `?` 会让人以为
    「改写器失败了」，而事实是这一趟本就不该有改写 —— 归因取自 `impl` 行，仍然是
    `hook.log` 说的。
    """
    if own.opt_mode != products.UNKNOWN:
        return own.opt_mode
    if own.cfg_rewrite != products.UNKNOWN:
        return own.cfg_rewrite
    if own.impl != products.UNKNOWN:
        return f"—（impl={own.impl}）"
    return products.UNKNOWN


def summarize(regions: list[Region], verbose: bool = False) -> tuple[str, list[str], str]:
    """按「一次编译 → 一次运行」的区间归并，**一个电路一行**。

    两次编译之间必然是同一次运行，所以每个区间只取**最大的非停机段中位数**当代表。
    不设样本数下限：过渡值（窗口横跨切换点）总是偏低，取最大就天然把它排除了；反之，
    短时间运行（稳定段只有 1~2 个样本）也能被如实报出来 —— 行内的样本数就是读者判断
    可靠性的依据。一次都没跑起来的区间省略。
    返回 (标题, **无色**文本行, 一行摘要) —— 终端与 freq_summary.txt 共用同一份文字。
    """
    lines: list[str] = []
    digest: list[str] = []
    nth = 0
    total = 0
    for region in regions:
        total += sum(seg.count for seg in region.segments)
        runs = [seg for seg in region.segments if seg.median > 0]
        if runs:
            nth += 1
            seg = max(runs, key=lambda s: s.median)
            med, vals = seg.median, seg.values
            # 区间只记编号，摘要时重查 —— 读数只依赖产物与日志，跟当时终端打印了什么无关。
            own = products.ownership(region.num)
            fp = products.product_fp(region.num)
            n_rw = own.rewritten
            if fp == "-" and own.sha_log != "-":
                fp = own.sha_log      # 产物已被清（游戏重启过）：退回日志里记的 sha
            rewrite = _rewrite_cell(own)
            row = (f"{nth:>2d}) {region.label:<5s}{med / 1e6:6.3f}"
                   f" {len(vals):>3d}  {(max(vals) - min(vals)) / med * 100:5.2f}"
                   f" {fp:<8s} {n_rw or '-':>5s}  {rewrite}")
            lines.append(row)
            digest.append(f"{region.label}={med / 1e6:.3f}MHz")
        elif verbose:
            lines.append(f"   --) {region.label:<8s} 未运行")
        if verbose:
            for seg in region.segments:
                what = "停机" if seg.median == 0 else f"{seg.median / 1e6:8.3f} MHz"
                lines.append(f"      · {region.label:<8s} {what}"
                             f"  ×{seg.count:<3d} {'过渡' if seg.transient else ''}")
    title = f"{nth} 个编译区间 · 共 {total} 个样本 · 约 {total}s"
    if lines:
        lines.insert(0, "   #      主频 样本   波动 指纹      改写  rewrite")
    return title, lines, " ".join(digest) or "无运行区间"


def summary_text(title: str, lines: list[str], when: datetime | None = None) -> str:
    """`freq_summary.txt` 的全文（每次运行**覆盖写**，只保留最近一次）。

    以文本模式写出——Windows 上换行是 `\\r\\n`，与历史文件一致。
    """
    body = lines or ["   （本次没有跑起来的电路）"]
    when = when or datetime.now()
    return (f"=== {when:%Y-%m-%d %H:%M:%S}  {title} ===\n"
            + "".join(f"{ln}\n" for ln in body))


def print_summary(regions: list[Region], sink, verbose: bool = False,
                  summary_path: Path | None = None,
                  when: datetime | None = None) -> tuple[str, list[str], str]:
    """终端打印总结，并把同一份无色文字**覆盖写**进 freq_summary.txt。

    想回溯历史不必另存归档：freq_log.tsv 的 `summary` 行每次都带一行紧凑摘要
    （`#007=6.974MHz #008=6.961MHz …`），`grep summary` 一下就是全部历史。
    """
    title, lines, digest = summarize(regions, verbose)
    print()
    print(term.paint(f"── 总结 ──  {title}", "bold", "magenta"))
    for line in lines:
        print(line)
    if not lines:
        print(term.paint("   （本次没有跑起来的电路）", "yellow"))
    (summary_path or SUMMARY_PATH).write_text(
        summary_text(title, lines, when), encoding="utf-8")
    sink.write("summary", note=f"{title} | {digest}")
    return title, lines, digest


# ---------------------------------------------------------------- 对照表（bench A/B）

@dataclass
class BenchRow:
    """一次「换配置 → 重编译 → 运行 → 采样」的结果。

    `fp` 给成 None 时才去盘上现算（历史数据显式给——那些产物早被下一次游戏启动清掉了）。
    `compiler` / `impl` 取不到时是 `?`（不知道），写进 TSV 时规整成 `-`。
    """

    mode: str                       # rewrite（旧列名保留）
    num: str | None
    hz: list[float]
    compiler: str = ""             # 空 = 历史数据没有这一列（写 TSV 时变 `-`）
    impl: str = ""
    fp: str | None = None
    note: str = ""

    @property
    def median(self) -> float:
        return statistics.median(self.hz) if self.hz else 0.0

    def fingerprint(self) -> str:
        if self.fp is not None:
            return self.fp
        return products.product_fp(self.num) if self.num else ""


def bench_table(rows: list[BenchRow]) -> tuple[list[str], float | None]:
    """A/B 对照表；相对基线的百分比按**中位数**算（决策 3：bench 不再用 mean）。

    基线只认**第一个变体**（默认 `off`）。它失败、或读到的是停机附近的噪声
    （< 0.1 MHz）时百分比留空，不拿别的顶上、也不拿接近 0 的值当分母。
    """
    base_hz = None
    if rows and rows[0].hz and statistics.median(rows[0].hz) >= 1e5:
        base_hz = statistics.median(rows[0].hz)

    # 列宽按**显示宽度**算（中文占 2 列）；数值列不带单位 —— 单位是 MHz，与区间表
    # 保持同一口径，不在每列重复。列头写「中位数」而不是旧版的「平均」：决策 3 之后
    # 聚合口径就是 median，标签跟着改（让标签与实际口径一致）。
    W10, W7, W8, W6, W7B, W11, W7C = 10, 7, 8, 6, 7, 11, 7
    head = (pad("模式", W10) + pad("产物", W7) + pad("中位数", W8)
            + pad("样本", W6) + pad("波动", W7B)
            + pad("指纹", W11) + pad("改写", W7C) + pad_left("相对基线", 8))
    out: list[str] = [head]
    for r in rows:
        fp, n_rw = DASH, "-"
        if r.num:
            fp = r.fingerprint()
            _f, rewritten, _rw = products.opt_info(r.num)
            m = re.search(r"(\d+)", rewritten)      # `改写 1590 处` → 1590（同区间表）
            n_rw = m.group(1) if m else "-"
        prod = f"#{r.num}" if r.num else DASH
        if not r.hz:
            out.append(pad(r.mode, W10) + pad(prod, W7) + pad(DASH, W8)
                       + pad("0", W6) + pad(DASH, W7B)
                       + pad(fp, W11) + pad(DASH, W7C) + (r.note or "无数据"))
            continue
        med = r.median
        spread = (max(r.hz) - min(r.hz)) / med * 100
        rel = "" if base_hz in (None, 0) else f"{(med / base_hz - 1) * 100:+.1f}%"
        color = ("bold", "green") if rel.startswith("+") else ("bold", "red")
        out.append(pad(r.mode, W10)
                   + term.paint(pad(prod, W7), "bold", "magenta")
                   + term.paint(pad(f"{med / 1e6:.3f}", W8), *hz_style(med))
                   + pad(str(len(r.hz)), W6)
                   + pad(f"{spread:.2f}", W7B)
                   + pad(fp, W11)
                   + term.paint(pad(n_rw, W7C), "bold", "cyan")
                   + (term.paint(pad_left(rel, 8), *color) if rel else ""))
        if r.note:
            out.append(pad("", W10) + r.note)
    return out, base_hz


# ---------------------------------------------------------------- TSV 与 JSON

#: bench TSV 的列契约。`mean_hz → median_hz`（决策 3），末尾追加 `compiler` / `impl`
#: （决策 5，向后兼容：旧列的位置与含义不变，新增列在最后）。
BENCH_HEADER = "# bench_rewrite.py —— 每个模式一次编译 → 一次采样\n"
BENCH_COLUMNS = ("mode\tnum\tfp\tmedian_hz\tsample_n\tnote\tsamples\t"
                 "compiler\timpl\n")


def bench_tsv(rows: list[BenchRow]) -> str:
    """bench 结果 TSV 的全文（含每个采样点**原始值**）。

    旧数据没有 `compiler` / `impl` 时写 `-`（不是空串）——「缺失」与「空值」要分得开。
    """
    lines = [BENCH_HEADER, BENCH_COLUMNS]
    for r in rows:
        med = r.median
        samples = ";".join(f"{v:.0f}" for v in r.hz)
        lines.append(f"{r.mode}\t{r.num or ''}\t{r.fingerprint()}\t{med:.0f}\t"
                     f"{len(r.hz)}\t{r.note}\t{samples}\t"
                     f"{r.compiler or '-'}\t{r.impl or '-'}\n")
    return "".join(lines)


def write_bench_tsv(path: Path, rows: list[BenchRow]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(bench_tsv(rows), encoding="utf-8")


@dataclass
class VariantReport:
    """一个变体（`compiler:rewrite`）的一轮 A/B 结果。"""

    variant: str
    compiler: str
    rewrite: str
    num: str | None
    product_sha: str
    impl: str
    impl_config: str
    samples: list[float]
    median_hz: float
    spread_pct: float
    relative_pct: float | None
    fallback_rate: float | None
    fallback_reasons: dict[str, int] = field(default_factory=dict)
    note: str = ""

    def to_dict(self) -> dict:
        return {
            "variant": self.variant,
            "compiler": self.compiler,
            "rewrite": self.rewrite,
            "num": self.num,
            "product_sha": self.product_sha,
            "impl": self.impl,                 # 实际走了哪个实现（hook.log impl 行）
            "impl_config": self.impl_config,   # 配的 compiler（impl 行的 mode）
            "samples": [round(v) for v in self.samples],
            "median_hz": round(self.median_hz, 3),
            "spread_pct": round(self.spread_pct, 3),
            "relative_pct": (None if self.relative_pct is None
                             else round(self.relative_pct, 3)),
            "fallback_rate": self.fallback_rate,
            "fallback_reasons": self.fallback_reasons,
            "note": self.note,
        }


@dataclass
class Report:
    """机器读的 A/B 报告（判据入口只消费它）。`schema` 是版本号，加字段时 +1。"""

    when: str = ""
    variants: list[VariantReport] = field(default_factory=list)
    baseline: str | None = None
    schema: int = SCHEMA

    def to_dict(self) -> dict:
        return {"schema": self.schema, "when": self.when, "baseline": self.baseline,
                "variants": [v.to_dict() for v in self.variants]}

    def write_json(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")


#: 回退判定：配的是 `ours*` 而这次实际**不是** `ours` —— 词表见
#: `docs/specs/2026-10-15-simplexc-interfaces.md` §5。
_FALLBACK_IMPL = "fallback"


def _is_fallback(compiler: str, impl: str) -> bool:
    return compiler.startswith("ours") and impl != "ours"


def _relative(med: float, base_hz: float | None) -> float | None:
    if base_hz in (None, 0) or not med:
        return None
    return (med / base_hz - 1) * 100


def build_report(rows: list[BenchRow], *, when: str | None = None) -> Report:
    """把 bench 的每轮结果汇成 `Report`。

    回退率按 per-call `impl` 行算：分母 = 该变体所有已知 `impl` 的轮次，分子 =
    `impl` 与配的 `compiler` 不符（`ours*` 却跑成 `ref`/`fallback`/…）的轮次。
    `fallback_reasons` 按实际 `impl` 归类。相对基线只认**第一个变体**。

    `impl` 由 `_bench_one` 从 `hook.log` 的 `impl` 行填好（本函数只看 `BenchRow`，
    离线也能喂构造数据）。
    """
    base_hz = statistics.median(rows[0].hz) if (rows and rows[0].hz) else None
    compiles: dict[str, int] = {}
    falls: dict[str, int] = {}
    reasons: dict[str, dict[str, int]] = {}
    for r in rows:
        if not r.num:
            continue
        key = f"{r.compiler}:{r.mode}" if r.compiler else r.mode
        compiles[key] = compiles.get(key, 0) + 1
        if _is_fallback(r.compiler, r.impl):
            falls[key] = falls.get(key, 0) + 1
            bucket = reasons.setdefault(key, {})
            bucket[r.impl] = bucket.get(r.impl, 0) + 1

    out: list[VariantReport] = []
    for r in rows:
        med = r.median
        vals = r.hz
        spread = 0.0 if (not vals or med == 0) else (max(vals) - min(vals)) / med * 100
        name = f"{r.compiler}:{r.mode}" if r.compiler else r.mode
        n_calls = compiles.get(name, 0)
        rate = (falls.get(name, 0) / n_calls) if n_calls else None
        out.append(VariantReport(
            variant=name,
            compiler=r.compiler or "?",
            rewrite=r.mode,
            num=r.num,
            product_sha=r.fingerprint() or "-",
            impl=r.impl or "?",
            impl_config=r.compiler or "?",
            samples=vals,
            median_hz=med,
            spread_pct=spread,
            relative_pct=_relative(med, base_hz),
            fallback_rate=rate,
            fallback_reasons=dict(reasons.get(name, {})),
            note=r.note,
        ))
    return Report(when=when or f"{datetime.now():%Y-%m-%d %H:%M:%S}",
                  baseline=(out[0].variant if out else None), variants=out)
