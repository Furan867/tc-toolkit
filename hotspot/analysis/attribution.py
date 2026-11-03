"""samples.tsv × sidecar.json → 归因结果（无状态；换侧车重算即可，不必重采样）。

口径（沿用旧工具 `tools/analyze_sim_hotspot.py:190` 的 `in_jit` 分母）：
- 表 1–3 的分母 = bucket=0 且命中 `segments` 的样本数；
- 表 4 的分母 = 全样本。

bucket=0 未归属的二分按 `docs/specs/2026-11-01-hotspot-profiler-design.md` §7.1
（「有 stubs 切桩 / 剩余为公共根」）。§6 的查询顺序写的是 segments → lines → stubs，
但 `lines` 只提供行号明细、不参与这里的两分：若先判 `lines`，落进桩里又有行号信息
的样本会被算成公共根，表 4 的两个子桶就失真。
"""
from __future__ import annotations

import bisect
import json
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Generic, TypeVar

SAMPLE_HEADER = "# samples.tsv v1"
SAMPLE_COLUMNS = ("bucket", "blob_offset", "thread_id", "count")
BUCKETS = (0, 1, 2)


class AnalysisError(Exception):
    """输入文件的问题（缺文件 / 坏 schema / 坐标越界）：带文件与原因，由 CLI 转非零退出。"""

    def __init__(self, path: str, reason: str) -> None:
        super().__init__(f"{path}: {reason}")
        self.path = path
        self.reason = reason


# ---------------------------------------------------------------- samples.tsv


@dataclass(frozen=True)
class SampleRow:
    bucket: int
    offset: int
    thread_id: int
    count: int
    lineno: int


@dataclass(frozen=True)
class Samples:
    path: str
    rows: tuple[SampleRow, ...]
    bucket_totals: dict[int, int]

    @property
    def total(self) -> int:
        return sum(self.bucket_totals.values())


def load_samples(path: str) -> Samples:
    try:
        text = Path(path).read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise AnalysisError(path, f"读不了（{exc.strerror or exc}）") from exc
    lines = text.splitlines()
    if not lines:
        raise AnalysisError(path, "空文件")
    if lines[0].strip() != SAMPLE_HEADER:
        raise AnalysisError(path, f"首行必须是 {SAMPLE_HEADER!r}（实测 {lines[0]!r}）")
    if len(lines) < 2:
        raise AnalysisError(path, "缺少列头行")
    if tuple(lines[1].split("\t")) != SAMPLE_COLUMNS:
        raise AnalysisError(path, f"列头必须是 {'/'.join(SAMPLE_COLUMNS)}（实测 {lines[1]!r}）")

    rows: list[SampleRow] = []
    totals: dict[int, int] = dict.fromkeys(BUCKETS, 0)
    seen: set[tuple[int, int, int]] = set()
    for lineno, line in enumerate(lines[2:], start=3):
        if not line.strip():
            continue
        cells = line.split("\t")
        if len(cells) != len(SAMPLE_COLUMNS):
            raise AnalysisError(path, f"第 {lineno} 行有 {len(cells)} 列，应为 {len(SAMPLE_COLUMNS)} 列")
        values = []
        for name, cell in zip(SAMPLE_COLUMNS, cells, strict=True):
            try:
                values.append(int(cell))
            except ValueError as exc:
                raise AnalysisError(path, f"第 {lineno} 行 {name} {cell!r} 不是整数") from exc
        bucket, offset, thread_id, count = values
        if bucket not in BUCKETS:
            raise AnalysisError(path, f"第 {lineno} 行 bucket={bucket}，只允许 {'/'.join(map(str, BUCKETS))}")
        if count < 0:
            raise AnalysisError(path, f"第 {lineno} 行 count={count} 为负")
        if bucket == 0 and offset < 0:
            raise AnalysisError(path, f"第 {lineno} 行 blob_offset={offset} 为负")
        key = (bucket, offset, thread_id)
        if key in seen:
            raise AnalysisError(
                path, f"第 {lineno} 行 (bucket, blob_offset, thread_id)={key} 重复；"
                f"v1 契约要求同一键已合并计数")
        seen.add(key)
        rows.append(SampleRow(bucket, offset, thread_id, count, lineno))
        totals[bucket] += count
    return Samples(path=str(path), rows=tuple(rows), bucket_totals=totals)


# ------------------------------------------------------------------ 侧车


@dataclass(frozen=True)
class Element:
    id: int
    type: str
    width: int | None
    lines: tuple[tuple[int, int], ...]
    tail: int | None


Payload = TypeVar("Payload")


class IntervalTable(Generic[Payload]):
    """按 start 升序、同表内互不重叠的 half-open 区间表。"""

    def __init__(self, name: str, rows: list[tuple[int, int, Payload]]) -> None:
        self.name = name
        self.rows = tuple(rows)
        self.starts = tuple(start for start, _, _ in rows)

    def find(self, offset: int) -> Payload | None:
        index = bisect.bisect_right(self.starts, offset) - 1
        if index < 0:
            return None
        start, end, payload = self.rows[index]
        return payload if offset < end else None


@dataclass(frozen=True)
class Sidecar:
    path: str
    compiler: str
    artifact_len: int | None
    artifact_sha256: str | None
    elements: dict[int, Element]
    segments: IntervalTable[int]
    lines: IntervalTable[LineRef]
    stubs: IntervalTable[tuple[str, str]]
    source_file: str | None = None      # meta.sources 里 role=="source" 的文件（旧 sidecar → None）


@dataclass(frozen=True)
class LineRef:
    """一行源码的归属：`file` 是旧 sidecar 没有的字段，没有就按原行号显示。"""
    file: str | None
    line: int

    def __str__(self) -> str:
        return f"{self.file}:{self.line}" if self.file else f"{self.line}"


def load_sidecar(path: str, code_len: int | None = None) -> Sidecar:
    try:
        text = Path(path).read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise AnalysisError(path, f"读不了（{exc.strerror or exc}）") from exc
    try:
        doc = json.loads(text)
    except json.JSONDecodeError as exc:
        raise AnalysisError(path, f"不是合法 JSON（第 {exc.lineno} 行第 {exc.colno} 列：{exc.msg}）") from exc
    if not isinstance(doc, dict):
        raise AnalysisError(path, "顶层必须是对象")

    version = doc.get("version")
    if version != 1:
        raise AnalysisError(path, f"version 必须是 1（实测 {version!r}）")
    compiler = doc.get("compiler")
    if not isinstance(compiler, str) or not compiler:
        raise AnalysisError(path, f"compiler 必须是非空字符串（实测 {compiler!r}）")

    artifact = doc.get("artifact", {})
    if not isinstance(artifact, dict):
        raise AnalysisError(path, "artifact 必须是对象")
    art_len = artifact.get("len")
    if art_len is not None and not isinstance(art_len, int):
        raise AnalysisError(path, f"artifact.len 必须是整数（实测 {art_len!r}）")
    art_sha = artifact.get("sha256")
    if art_sha is not None and not isinstance(art_sha, str):
        raise AnalysisError(path, f"artifact.sha256 必须是字符串（实测 {art_sha!r}）")

    elements = _load_elements(path, doc.get("elements"))
    ids = set(elements)

    def segment_payload(item: dict, where: str):
        eid = _int_field(path, item, "id", where)
        if eid not in ids:
            raise AnalysisError(path, f"{where}: id {eid} 不在 elements 台账里（表不变式：segments.id ⊆ elements.id）")
        return eid

    segments = _load_table(path, code_len, "segments", doc.get("segments"), segment_payload)
    lines = _load_table(path, code_len, "lines", doc.get("lines"), _line_payload(path))
    stubs = _load_table(path, code_len, "stubs", doc.get("stubs"), _stub_payload(path))

    return Sidecar(
        path=str(path), compiler=compiler, artifact_len=art_len, artifact_sha256=art_sha,
        elements=elements, segments=segments, lines=lines, stubs=stubs,
        source_file=_source_file(path, doc.get("meta")))


def _load_elements(path: str, raw) -> dict[int, Element]:
    if not isinstance(raw, list):
        raise AnalysisError(path, f"elements 必须是数组（实测 {type(raw).__name__}）")
    elements: dict[int, Element] = {}
    for index, item in enumerate(raw):
        where = f"elements[{index}]"
        if not isinstance(item, dict):
            raise AnalysisError(path, f"{where} 必须是对象")
        eid = _int_field(path, item, "id", where)
        etype = item.get("type")
        if not isinstance(etype, str) or not etype:
            raise AnalysisError(path, f"{where}: type 必须是非空字符串（实测 {etype!r}）")
        width = item.get("width")
        if width is not None and not isinstance(width, int):
            raise AnalysisError(path, f"{where}: width 必须是整数或 null（实测 {width!r}）")
        tail = item.get("tail")
        if tail is not None and not isinstance(tail, int):
            raise AnalysisError(path, f"{where}: tail 必须是整数或 null（实测 {tail!r}）")
        ranges = item.get("lines", [])
        if not isinstance(ranges, list):
            raise AnalysisError(path, f"{where}: lines 必须是数组")
        pairs = []
        for j, pair in enumerate(ranges):
            if not (isinstance(pair, list) and len(pair) == 2
                    and all(isinstance(v, int) for v in pair)):
                raise AnalysisError(path, f"{where}.lines[{j}] 必须是 [lo, hi] 两个整数（实测 {pair!r}）")
            pairs.append((pair[0], pair[1]))
        if eid in elements:
            raise AnalysisError(
                path, f"{where}: id {eid} 重复（同一 sidecar 内 id 必须唯一，否则实例主键撞车）")
        elements[eid] = Element(id=eid, type=etype, width=width, lines=tuple(pairs), tail=tail)
    return elements


def _load_table(
    path: str, code_len: int | None, name: str, raw,
    payload: Callable[[dict, str], Payload]) -> IntervalTable[Payload]:
    if not isinstance(raw, list):
        raise AnalysisError(path, f"{name} 必须是数组（实测 {type(raw).__name__}）")
    rows: list[tuple[int, int, Payload]] = []
    for index, item in enumerate(raw):
        where = f"{name}[{index}]"
        if not isinstance(item, dict):
            raise AnalysisError(path, f"{where} 必须是对象")
        start = _int_field(path, item, "start", where)
        end = _int_field(path, item, "end", where)
        if start < 0:
            raise AnalysisError(path, f"{where}: start={start} 为负")
        if end <= start:
            raise AnalysisError(path, f"{where}: end={end} 必须大于 start={start}（区间 half-open）")
        if code_len is not None and end > code_len:
            raise AnalysisError(
                path, f"{where}: 结束偏移 {end} 超出产物代码长度 {code_len}（坐标越界）")
        rows.append((start, end, payload(item, where)))
    for prev, cur in zip(rows, rows[1:], strict=False):
        if cur[0] < prev[1]:
            raise AnalysisError(
                path, f"{name}: 必须按 start 升序且同表内互不重叠"
                      f"（前一区间止于 {prev[1]}，后一区间起于 {cur[0]}）")
    return IntervalTable(name, rows)


def _line_payload(path: str):
    def extract(item: dict, where: str) -> LineRef:
        line = _int_field(path, item, "line", where)
        name = item.get("file")
        if name is not None and (not isinstance(name, str) or not name):
            raise AnalysisError(path, f"{where}: file 必须是非空字符串或 null（实测 {name!r}）")
        return LineRef(file=name, line=line)
    return extract


def _source_file(path: str, meta) -> str | None:
    """`meta.sources` 里 role=="source" 的文件名；旧 sidecar 没有这份台账 → None。"""
    if meta is None:
        return None
    if not isinstance(meta, dict):
        raise AnalysisError(path, f"meta 必须是对象或 null（实测 {type(meta).__name__}）")
    for item in meta.get("sources") or []:
        if isinstance(item, dict) and item.get("role") == "source":
            name = item.get("file")
            return name if isinstance(name, str) and name else None
    return None


def _stub_payload(path: str):
    def extract(item: dict, where: str) -> tuple[str, str]:
        name = item.get("name")
        if not isinstance(name, str) or not name:
            raise AnalysisError(path, f"{where}: name 必须是非空字符串（实测 {name!r}）")
        kind = item.get("kind", "stub")
        if not isinstance(kind, str) or not kind:
            raise AnalysisError(path, f"{where}: kind 必须是非空字符串（实测 {kind!r}）")
        return (kind, name)
    return extract


def _int_field(path: str, item: dict, key: str, where: str) -> int:
    value = item.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        raise AnalysisError(path, f"{where}: {key} 必须是整数（实测 {value!r}）")
    return value


# ------------------------------------------------------------------ 归因


@dataclass
class Attribution:
    total: int                                        # 全样本（表 4 分母）
    bucket_totals: dict[int, int]
    attributed: int                                   # bucket0 命中 segments（表 1–3 分母）
    per_instance: Counter = field(default_factory=Counter)          # (type, id) -> 样本数
    per_type: Counter = field(default_factory=Counter)              # type -> 样本数
    instance_lines: dict = field(default_factory=dict)              # (type, id) -> Counter[LineRef]
    stub_totals: Counter = field(default_factory=Counter)           # "name" -> 样本数
    stub_samples: int = 0
    root_samples: int = 0
    root_with_line: int = 0                                         # 公共根里带行号信息的样本
    root_line_files: Counter = field(default_factory=Counter)       # 公共根里带行号的样本按文件分


def attribute(samples: Samples, sidecar: Sidecar, code_len: int | None = None) -> Attribution:
    result = Attribution(
        total=samples.total, bucket_totals=dict(samples.bucket_totals), attributed=0)
    for row in samples.rows:
        if row.bucket != 0:
            continue
        if code_len is not None and row.offset >= code_len:
            raise AnalysisError(
                samples.path,
                f"第 {row.lineno} 行 bucket=0 的 blob_offset={row.offset} 超出代码长度 {code_len}"
                f"（坐标越界：采样与侧车不是同一份产物？）")
        eid = sidecar.segments.find(row.offset)
        if eid is not None:
            element = sidecar.elements[eid]
            key = (element.type, element.id)
            result.per_instance[key] += row.count
            result.per_type[element.type] += row.count
            result.attributed += row.count
            line = sidecar.lines.find(row.offset)
            if line is not None:
                result.instance_lines.setdefault(key, Counter())[line] += row.count
            continue
        stub = sidecar.stubs.find(row.offset)
        if stub is not None:
            kind, name = stub
            result.stub_totals[name if kind == "stub" else f"{name}（{kind}）"] += row.count
            result.stub_samples += row.count
            continue
        result.root_samples += row.count
        ref = sidecar.lines.find(row.offset)
        if ref is not None:
            result.root_with_line += row.count
            if ref.file:
                result.root_line_files[ref.file] += row.count
    return result
