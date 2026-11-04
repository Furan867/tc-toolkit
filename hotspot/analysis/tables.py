"""热度表的数据结构（数字口径只在这里算，渲染层只管排版）。

分母：表 1–3 = bucket0 已归属样本；表 4 = 全样本；表 5 = bucket0 全部样本，
只在侧车带 `functions` 时才有（`docs/specs/2026-11-01-hotspot-profiler-design.md` §7.2）。
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
    # 暂停开销（`[window]` 的三个 `*_pause_us`）；旧 run.ini 没这三个键 → None，显示为未知。
    pause_avg_us: int | None
    pause_max_us: int | None
    pause_total_us: int | None
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
    file: str | None = None     # 行号归属的文件；旧 sidecar 没有 → None（按原行号显示）


@dataclass(frozen=True)
class InstRow:
    type: str
    id: int
    samples: int
    share: float
    src_lines: tuple[tuple[int, int], ...]
    line_rows: tuple[LineRow, ...]
    src_file: str | None = None    # 元件的行区间属于哪个文件（el.tsv 只记源文件）


@dataclass(frozen=True)
class BucketRow:
    key: str
    label: str
    samples: int
    share: float
    detail: tuple[tuple[str, int, float], ...] = ()


@dataclass(frozen=True)
class FuncRow:
    """一个函数的样本热度（同一个函数的内联实例按 name/file/line 合并）。

    口径是 bucket0 全部样本：元件段的机器码也挂在某个函数下，和元件表并行算，互不扣除。
    """
    name: str
    file: str | None
    line: int | None
    samples: int
    share: float
    origin: str        # 声明文件的角色：stdlib / source（同表 4 的划法）


@dataclass(frozen=True)
class Tables:
    meta: RunMeta
    total_samples: int
    attributed_samples: int
    bucket0_samples: int          # 表 5 分母
    types: tuple[TypeRow, ...]
    instances: tuple[InstRow, ...]
    buckets: tuple[BucketRow, ...]
    functions: tuple[FuncRow, ...] = field(default_factory=tuple)
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
            LineRow(file=ref.file, line=ref.line, samples=n, share=_share(n, samples))
            for ref, n in sorted(line_counter.items(),
                                 key=lambda kv: (-kv[1], kv[0].file or "", kv[0].line)))
        inst_rows.append(InstRow(
            type=key[0], id=key[1], samples=samples, share=_share(samples, attributed),
            src_lines=sidecar.elements[key[1]].lines, line_rows=line_rows,
            src_file=sidecar.source_file))

    total = attribution.total
    bucket0 = attribution.bucket_totals.get(0, 0)
    stub_detail = tuple(
        (name, n, _share(n, attribution.stub_samples))
        for name, n in sorted(attribution.stub_totals.items(), key=lambda kv: (-kv[1], kv[0])))
    if sidecar.functions is None:
        root_detail = _root_by_lines(attribution)
        function_rows: tuple[FuncRow, ...] = ()
    else:
        root_detail = _root_by_function(attribution, sidecar)
        function_rows = _function_rows(attribution, sidecar, bucket0, top_n)
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
        BucketRow(key="bucket2", label="bucket2 IMAGE 区（exe/dll，游戏引擎/调度/输入）",
                  samples=attribution.bucket_totals.get(2, 0),
                  share=_share(attribution.bucket_totals.get(2, 0), total)),
    )

    return Tables(
        meta=meta, total_samples=total, attributed_samples=attributed, bucket0_samples=bucket0,
        types=type_rows, instances=tuple(inst_rows), buckets=buckets,
        functions=function_rows, notes=_notes(meta, attribution), watermark=watermark)


def _root_by_lines(attribution: Attribution) -> tuple[tuple[str, int, float], ...]:
    """旧 sidecar（无 functions）的公共根拆法：按有没有行号信息、有的话属哪份文件。"""
    root = attribution.root_samples
    detail = [("有行号信息", attribution.root_with_line,
               _share(attribution.root_with_line, root))]
    # 带行号信息但没有元件的机器段（公共/根代码）按**文件**拆开：stdlib 前缀本来就没有元件注释，
    # 它的行落在这一桁是预期内的；混在一起看不出到底是哪份代码在跑。
    detail += [
        (f"有行号信息 · {name}", n, _share(n, root))
        for name, n in sorted(attribution.root_line_files.items(), key=lambda kv: (-kv[1], kv[0]))]
    detail.append(
        ("无行号信息", root - attribution.root_with_line,
         _share(root - attribution.root_with_line, root)))
    return tuple(detail)


def _root_by_function(attribution: Attribution, sidecar: Sidecar) -> tuple[tuple[str, int, float], ...]:
    """公共根 → stdlib 函数 / 源文件函数 / 无函数信息（表 4；元件样本不在公共根里）。

    角色看函数的**声明文件**（`meta.sources` 划出的 stdlib 与源文件），不看行号：行号属哪段
    是行表的结论，而函数本身可能跨段；先按行号划会把跨段函数的样本标错。
    """
    by_role: Counter = Counter()
    for func_id, samples in attribution.root_function_samples.items():
        info = sidecar.function_info[func_id]
        by_role[_origin(sidecar.stdlib_file, info.file)] += samples

    root = attribution.root_samples
    return tuple(
        (label, samples, _share(samples, root))
        for label, samples in (("stdlib 函数", by_role["stdlib"]),
                               ("source 函数", by_role["source"]),
                               ("无函数信息", attribution.root_no_function)))


def _function_rows(attribution: Attribution, sidecar: Sidecar, bucket0: int,
                   top_n: int) -> tuple[FuncRow, ...]:
    """表 5：bucket0 全部样本按最内层函数排名（元件段与公共/根的样本各算各的）。"""
    by_function: Counter = Counter()
    for func_id, samples in attribution.function_samples.items():
        info = sidecar.function_info[func_id]
        by_function[(info.name, info.file, info.line)] += samples
    ranked = sorted(by_function.items(),
                    key=lambda kv: (-kv[1], kv[0][0] or "", kv[0][1] or "", kv[0][2] or 0))
    return tuple(
        FuncRow(name=name or "（无名）", file=file, line=line, samples=n,
                share=_share(n, bucket0), origin=_origin(sidecar.stdlib_file, file))
        for (name, file, line), n in ranked[:max(1, top_n)])


def _origin(stdlib_file: str | None, file: str | None) -> str:
    """函数角色看声明文件；侧车没带 stdlib 台账（无 `--stdlib`）时全算 source。"""
    return "stdlib" if stdlib_file is not None and file == stdlib_file else "source"


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
        return "频率读数缺失（run.ini 没有 freq.start_hz / freq.end_hz），tick 估算不可用"
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
            f"run.ini 的 window.sample_count={meta.sample_count} 与 samples.tsv 合计 "
            f"{attribution.total} 不一致（采样中途截断或回写前参数变了？）")
    if meta.bases:
        notes.append(f"内存中整段校验通过的产物副本 {len(meta.bases)} 处（多副本已折算到同一坐标）")
    return tuple(notes)
