"""四张热度表的数据结构（数字口径只在这里算，渲染层只管排版）。

分母：表 1–3 = bucket0 已归属样本；表 4 = 全样本（`docs/specs/2026-11-01-hotspot-profiler-design.md` §7.2）。
表 1 按侧车台账列**全部**类型（含零值），旧工具只列采到的类型，导致零采样类型整行消失
（`tools/analyze_sim_hotspot.py:205` 的教训）。
"""
from __future__ import annotations

import math
from collections import Counter
from dataclasses import dataclass, field

from attribution import Attribution, Sidecar


@dataclass(frozen=True)
class RunMeta:
    run_name: str
    run_path: str
    created_at: str
    artifact_path: str
    artifact_sha256: str
    artifact_file_len: int | None
    artifact_code_len: int | None
    artifact_num: str
    window_sec: float | None
    interval_ms: float | None
    sample_count: int
    bases: tuple[str, ...]
    freq_start_hz: float | None
    freq_end_hz: float | None


@dataclass(frozen=True)
class TypeRow:
    type: str
    instances: int
    samples: int
    share: float


@dataclass(frozen=True)
class LineRow:
    line: int
    samples: int
    share: float


@dataclass(frozen=True)
class InstRow:
    type: str
    id: int
    samples: int
    share: float
    src_lines: tuple[tuple[int, int], ...]
    line_rows: tuple[LineRow, ...]


@dataclass(frozen=True)
class BucketRow:
    key: str
    label: str
    samples: int
    share: float
    detail: tuple[tuple[str, int, float], ...] = ()


@dataclass(frozen=True)
class Tables:
    meta: RunMeta
    total_samples: int
    attributed_samples: int
    types: tuple[TypeRow, ...]
    instances: tuple[InstRow, ...]
    buckets: tuple[BucketRow, ...]
    notes: tuple[str, ...] = field(default_factory=tuple)
    watermark: str | None = None


def _share(part: int, whole: int) -> float:
    return part / whole if whole else 0.0


def build_tables(meta: RunMeta, attribution: Attribution, sidecar: Sidecar,
                 top_n: int, watermark: str | None = None) -> Tables:
    attributed = attribution.attributed
    per_type_instances: Counter = Counter(el.type for el in sidecar.elements.values())

    type_rows = tuple(
        TypeRow(type=name, instances=per_type_instances[name],
                samples=attribution.per_type.get(name, 0),
                share=_share(attribution.per_type.get(name, 0), attributed))
        for name in sorted(
            per_type_instances,
            key=lambda t: (-attribution.per_type.get(t, 0), t)))

    # 实例主键 (type, id)：id 合并键，不按段聚合（同一元件两段机器码 = 一个实例）
    all_keys = [(el.type, el.id) for el in sidecar.elements.values()]
    all_keys.sort(key=lambda k: (-attribution.per_instance.get(k, 0), k[0], k[1]))
    inst_rows = []
    for key in all_keys[:max(1, top_n)]:
        samples = attribution.per_instance.get(key, 0)
        line_counter = attribution.instance_lines.get(key, Counter())
        line_rows = tuple(
            LineRow(line=line, samples=n, share=_share(n, samples))
            for line, n in sorted(line_counter.items(), key=lambda kv: (-kv[1], kv[0])))
        inst_rows.append(InstRow(
            type=key[0], id=key[1], samples=samples, share=_share(samples, attributed),
            src_lines=sidecar.elements[key[1]].lines, line_rows=line_rows))

    total = attribution.total
    stub_detail = tuple(
        (name, n, _share(n, attribution.stub_samples))
        for name, n in sorted(attribution.stub_totals.items(), key=lambda kv: (-kv[1], kv[0])))
    root_detail = (
        ("有行号信息", attribution.root_with_line,
         _share(attribution.root_with_line, attribution.root_samples)),
        ("无行号信息", attribution.root_samples - attribution.root_with_line,
         _share(attribution.root_samples - attribution.root_with_line, attribution.root_samples)),
    )
    buckets = (
        BucketRow(key="bucket0_attributed", label="bucket0 已归属（落在元件机器段里）",
                  samples=attributed, share=_share(attributed, total)),
        BucketRow(key="bucket0_stub", label="bucket0 未归属 · 外部调用桩（由 stubs 切出）",
                  samples=attribution.stub_samples, share=_share(attribution.stub_samples, total),
                  detail=stub_detail),
        BucketRow(key="bucket0_root", label="bucket0 未归属 · 公共/根代码",
                  samples=attribution.root_samples, share=_share(attribution.root_samples, total),
                  detail=root_detail),
        BucketRow(key="bucket1", label="bucket1 其他非 IMAGE 可执行区（旧电路 JIT 残留等）",
                  samples=attribution.bucket_totals.get(1, 0),
                  share=_share(attribution.bucket_totals.get(1, 0), total)),
        BucketRow(key="bucket2", label="bucket2 IMAGE 区（exe/dll，含游戏自身仿真驱动主循环）",
                  samples=attribution.bucket_totals.get(2, 0),
                  share=_share(attribution.bucket_totals.get(2, 0), total)),
    )

    return Tables(
        meta=meta, total_samples=total, attributed_samples=attributed,
        types=type_rows, instances=tuple(inst_rows), buckets=buckets,
        notes=_notes(meta, attribution), watermark=watermark)


def confidence_note(samples: int) -> str:
    """样本量置信提示：假定样本近似独立，单类型占比的 1σ 统计误差 ≈ 100/√N 百分点。"""
    if samples <= 0:
        return "无已归属样本，任何占比都没有意义（电路没在跑 / 采样没落进产物）"
    margin = 100.0 / math.sqrt(samples)
    if samples < 1000:
        return f"样本量偏低（已归属 {samples}）：占比的 1σ 统计误差约 ±{margin:.1f} 百分点，" \
               f"表内小数点后一位不可当真"
    return f"样本量 {samples}：占比的 1σ 统计误差约 ±{margin:.1f} 百分点"


def freq_note(meta: RunMeta) -> str:
    """近似口径（spec §9）：频率是游戏 UI 主频读数，被平滑、1 s 更新。"""
    start, end = meta.freq_start_hz, meta.freq_end_hz
    if not start and not end:
        return "频率读数缺失（run.toml 没有 freq.start_hz / freq.end_hz），tick 估算不可用"
    head = "近似口径（游戏 UI 主频显示值，被平滑、1 s 更新；非精确 tick）："
    parts = []
    if start:
        parts.append(f"窗口首 {start / 1e6:.3f} MHz")
    if end:
        parts.append(f"窗口尾 {end / 1e6:.3f} MHz")
    text = f"{head}{' / '.join(parts)}"
    if start and end and meta.window_sec:
        mean_hz = (start + end) / 2
        ticks = mean_hz * meta.window_sec
        text += (f"；按均值 {mean_hz / 1e6:.3f} MHz × {meta.window_sec:g} s ≈ {ticks:.3g} tick"
                 f"（粗估，精度「近似」）")
    return text


def _notes(meta: RunMeta, attribution: Attribution) -> tuple[str, ...]:
    notes = [confidence_note(attribution.attributed), freq_note(meta)]
    if meta.sample_count and meta.sample_count != attribution.total:
        notes.append(
            f"run.toml 的 window.sample_count={meta.sample_count} 与 samples.tsv 合计 "
            f"{attribution.total} 不一致（采样中途截断或回写前参数变了？）")
    if meta.bases:
        notes.append(f"内存中整段校验通过的产物副本 {len(meta.bases)} 处（多副本已折算到同一坐标）")
    return tuple(notes)
