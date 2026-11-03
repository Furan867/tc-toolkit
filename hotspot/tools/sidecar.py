#!python3
"""sidecar.json（spec §6）的 schema、输入解析与不变式 —— make_sidecar / g2_check 共用。

坐标口径（spec §2，全文一处定义）：blob 偏移 = RIP − 代码起点 = blob 文件下标 − 8。
`<blob>.stubs` 的 offset 已经是这个口径（`simplexc/assemble/assemble.cc` 里 `p.addr`
相对 payload 起点算），`.debug_line` 的地址**不是** —— 它是对象 `.text` 节内偏移
（对象节 VMA=0，行程序首址处一条 ADDR64→.text 重定位，`objdump` 不重定位），所以必须
先经 `<blob>.offsets` 的 anchors 重映射一次。

不变式一律抛 SidecarError、由入口按非零退出：静默配对（拿错的区间凑出一个结果）
比缺数据难查得多（spec §2、§6）。
"""

from __future__ import annotations

import bisect
import hashlib
import json
import os
import re

SCHEMA_VERSION = 1
HEADER_LEN = 8
# 跳板长度：代码桩 12 B / `__imp_` 数据槽 8 B（simplexc/assemble/assemble.hh:80-83）
STUB_LEN = {"stub": 12, "slot": 8}
SECTION_CLASSES = ("text", "rdata", "data", "bss")
# 编译入口常量：spec §6 要求 opt_level/pass_pipeline 取编译入口的常量。
# 落码链固定 `default<O2>`（simplexc/codegen/llvm_ffi.cc:36），没有第二处定义可查。
OPT_LEVEL = "O2"
LLVM_PIPELINE = "default<O2>"        # 侧车字段名是 pass_pipeline，值取这条 LLVM pass 管线

EL_TSV_HEADER = "# el.tsv v1"
STUBS_HEADER = "offset\tkind\tsymbol"


class SidecarError(Exception):
    """输入不自洽 / 产物与基准不一致 —— 调用方按非零退出处理。"""


# ----------------------------------------------------------------- 产物文件

def read_blob(path):
    """读产物文件并校验 8 字节头。头字段值 = **文件总长**（含头），与 bit62 相或。"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER_LEN:
        raise SidecarError(f"{path}：不足 {HEADER_LEN} 字节，不像产物文件")
    total = int.from_bytes(data[:HEADER_LEN], "little") & ~(1 << 62)
    if total != len(data):
        raise SidecarError(f"{path}：头里的长度 {total} 与文件长度 {len(data)} 不符")
    return data


def blob_fingerprint(data):
    return {"len": len(data), "sha256": hashlib.sha256(data).hexdigest()}


# ----------------------------------------------------------------- 偏移基准

def load_offsets(path, blob):
    """读 `<blob>.offsets`（spec §6 schema）并逐条验不变式。返回原字典（已验过）。"""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except OSError as exc:
        raise SidecarError(f"读不了 offsets 文件 {path}：{exc}") from exc
    except json.JSONDecodeError as exc:
        raise SidecarError(f"offsets 不是合法 JSON（{path}）：{exc}") from exc

    if data.get("version") != SCHEMA_VERSION:
        raise SidecarError(f"offsets version={data.get('version')!r}，只认 {SCHEMA_VERSION}")
    if data.get("header_len") != HEADER_LEN:
        raise SidecarError(f"offsets header_len={data.get('header_len')!r}，应为 {HEADER_LEN}")
    code_size = len(blob) - HEADER_LEN
    if data.get("code_size") != code_size:
        raise SidecarError(f"offsets code_size={data.get('code_size')!r} 与产物 "
                           f"len-{HEADER_LEN}={code_size} 不符")

    art = data.get("artifact") or {}
    want = blob_fingerprint(blob)
    if art.get("len") is not None and art["len"] != want["len"]:
        raise SidecarError(f"offsets artifact.len={art['len']} 与产物 {want['len']} 不符")
    if art.get("sha256") and str(art["sha256"]).lower() != want["sha256"]:
        raise SidecarError("offsets artifact.sha256 与产物不符（offsets 不是这份产物的）")

    body_end = _require_int(data, "body_end")
    stubs_base = _require_int(data, "stubs_base")
    slots_base = _require_int(data, "slots_base")
    _require_int(data, "entry")
    if not 0 <= body_end <= code_size:
        raise SidecarError(f"offsets body_end={body_end} 越出 [0,{code_size}]")
    if stubs_base != body_end:
        raise SidecarError(f"offsets stubs_base={stubs_base} 应等于 body_end={body_end}")
    if not body_end <= slots_base <= code_size:
        raise SidecarError(f"offsets slots_base={slots_base} 不在 [{body_end},{code_size}]")

    sections = data.get("sections")
    if not isinstance(sections, list) or not sections:
        raise SidecarError("offsets 没有 sections")
    for s in sections:
        for key in ("obj", "sec", "blob_off", "size"):
            _require_int(s, key)
        if s.get("class") not in SECTION_CLASSES:
            raise SidecarError(f"offsets 节 {s.get('name')!r} 的 class={s.get('class')!r} 不认识")
        if s["blob_off"] < 0 or s["size"] < 0:
            raise SidecarError(f"offsets 节 {s.get('name')!r} 的偏移/大小为负")
        if s["blob_off"] + s["size"] > body_end:
            raise SidecarError(f"offsets 节 {s.get('name')!r} 越过 body_end {body_end}")
    # 空节（size=0）是正常的：clang 对空 `.data`/`.bss` 就是这种形态，跳过不算重叠
    _check_sorted_non_overlapping(
        [(s["blob_off"], s["blob_off"] + s["size"]) for s in sections],
        "offsets sections", code_size, allow_empty=True)

    if not isinstance(data.get("debug_lines"), list) or not data["debug_lines"]:
        raise SidecarError("offsets 没有 debug_lines 条目（W1 没吐行表基准）")
    for entry in data["debug_lines"]:
        _require_int(entry, "obj")
        if not isinstance(entry.get("anchors"), list):
            raise SidecarError("offsets debug_lines 条目的 anchors 不是数组")
    return data


def _require_int(mapping, key):
    value = mapping.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        raise SidecarError(f"offsets 字段 {key}={value!r} 不是整数")
    return value


def select_debug_lines(offsets, obj_path):
    """挑出 `--obj` 对应的 debug_lines 条目（按 obj_path 的文件名比对）。

    首版单 `.text` 对象只有一条；多条且都认不出来时报错 —— 猜一个对象会让整份 sidecar
    的地址基准静默错位。
    """
    entries = offsets["debug_lines"]
    want = os.path.basename(obj_path).lower()
    hits = [e for e in entries if os.path.basename(str(e.get("obj_path", ""))).lower() == want]
    if len(hits) == 1:
        return hits[0]
    if not hits and len(entries) == 1:
        return entries[0]
    raise SidecarError(f"{obj_path}：offsets 的 debug_lines 里"
                       f"{'没有唯一匹配的条目' if hits else '对不上任何条目'}"
                       f"（共 {len(entries)} 条）")


def build_anchors(offsets, entry):
    """debug_lines 条目 → [{'blob_off','size','sec'}]（对象节内偏移的换算表）。"""
    by_key = {(s["obj"], s["sec"]): s for s in offsets["sections"]}
    anchors = []
    for a in entry.get("anchors") or []:
        text_sec = a.get("text_sec")
        blob_off = a.get("blob_off")
        if not isinstance(text_sec, int) or not isinstance(blob_off, int):
            raise SidecarError(f"offsets anchors 条目字段不是整数：{a!r}")
        sec = by_key.get((entry["obj"], text_sec))
        if sec is None:
            raise SidecarError(f"offsets 锚点指向不存在的节：obj={entry['obj']} sec={text_sec}")
        if sec["class"] != "text":
            raise SidecarError(f"offsets 锚点指向非 text 节 {sec.get('name')!r}")
        if sec["blob_off"] != blob_off:
            raise SidecarError(f"offsets 锚点 blob_off={blob_off} 与节 "
                               f"{sec.get('name')!r} 的 blob_off={sec['blob_off']} 不符")
        anchors.append({"blob_off": blob_off, "size": sec["size"], "sec": text_sec})
    if not anchors:
        raise SidecarError(f"offsets 的 debug_lines 条目（obj={entry['obj']}）没有 anchors"
                           " —— 行表地址没有重映射基准")
    return anchors


def obj_to_blob(anchors, addr, allow_end=False):
    """对象 `.text` 节内偏移 → blob 偏移。

    `allow_end=True` 时允许 `addr == size`（行程序的 end_sequence 常落在节尾）。
    多处命中一律报错：猜一个锚点会让整段行表静默错位。
    """
    hits = [a for a in anchors
            if 0 <= addr < a["size"] or (allow_end and addr == a["size"])]
    if not hits:
        raise SidecarError(f"对象内地址 0x{addr:x} 落在任何 .text 锚点之外")
    if len(hits) > 1:
        raise SidecarError(f"对象内地址 0x{addr:x} 同时落在 {len(hits)} 个 .text 锚点内，无法判定")
    return hits[0]["blob_off"] + addr


# ----------------------------------------------------------------- 行表

DECODEDLINE_ROW = re.compile(
    r"^(?P<name>.*?)\s+(?P<line>\d+|-)\s+(?P<addr>0[xX][0-9a-fA-F]+|0)"
    r"(?:\s+(?P<view>\d+))?(?:\s+(?P<stmt>x))?\s*$")
DECODEDLINE_HEADER = ("Line number", "Starting address")


def parse_decodedline(text):
    """`objdump --dwarf=decodedline` 输出 → 每个 CU 一个 [(起始地址, 行号|None)] 行表。

    只认表头之后的「行号 + 起始地址」行：`File name  Line number  Starting address
    View  Stmt`。行号 `-` 是 end_sequence（序列到此为止，自己不产区间）；行号 0 真实
    存在，表示「这里没有源码行」——它不产区间，但给上一条划上界。
    """
    cus, cur = [], None
    in_table = False
    for raw_line in text.splitlines():
        line = raw_line.rstrip("\r")
        stripped = line.strip()
        if stripped.startswith("CU:"):
            if cur:
                cus.append(cur)
            cur = None
            in_table = False
            continue
        if not in_table:
            if all(k in stripped for k in DECODEDLINE_HEADER):
                in_table = True
            continue
        if not stripped:
            continue
        match = DECODEDLINE_ROW.match(line)
        if not match:
            in_table = False        # 表尾（或者别的输出）：本 CU 的行表到此结束
            continue
        addr = int(match.group("addr"), 16)
        line_s = match.group("line")
        if cur is None:
            cur = []
        cur.append((addr, None if line_s == "-" else int(line_s)))
    if cur:
        cus.append(cur)
    return [rows for rows in cus if rows]


def line_ranges(cu_rows):
    """一个 CU 的行表 → [(起始地址, 结束地址, 行号)]（对象坐标，half-open）。

    `[addr_i, addr_{i+1})` 属于 `line_i`。丢弃：end_sequence、行号 0、零长度区间
    （同一地址上的多条记录，来自 View 列）。
    """
    if not cu_rows:
        return []
    if cu_rows[-1][1] is not None:
        raise SidecarError(f"行表末尾没有 end_sequence（末行地址 0x{cu_rows[-1][0]:x}）"
                           " —— 末区间没有上界，不能猜")
    out = []
    for i in range(len(cu_rows) - 1):
        start, line = cu_rows[i]
        end = cu_rows[i + 1][0]
        if line is None or line == 0 or end <= start:
            continue
        out.append((start, end, line))
    return out


def build_lines(cus, anchors, body_end):
    """行表 → sidecar `lines`（blob 坐标）：相邻同行的区间合并，全表有序。"""
    spans = []
    for rows in cus:
        for start, end, line in line_ranges(rows):
            blob_start = obj_to_blob(anchors, start)
            blob_end = obj_to_blob(anchors, end, allow_end=True)
            if blob_end - blob_start != end - start:
                raise SidecarError(f"行区间 0x{start:x}-0x{end:x} 跳过了 .text 节边界")
            spans.append((blob_start, blob_end, line))
    spans.sort()
    merged = []
    for start, end, line in spans:
        if merged and merged[-1]["line"] == line and merged[-1]["end"] == start:
            merged[-1]["end"] = end
        else:
            merged.append({"start": start, "end": end, "line": line})
    for span in merged:
        if span["end"] > body_end:
            raise SidecarError(f"行区间 [{span['start']},{span['end']}) 越过 body_end {body_end}")
    _check_sorted_non_overlapping([(s["start"], s["end"]) for s in merged], "lines", body_end)
    return merged


# ----------------------------------------------------------------- 元件表

def parse_elements_tsv(path):
    """读 `el.tsv`（spec §5.3）：首行 `# el.tsv v1`，数据行 6 个 TAB 字段。"""
    with open(path, encoding="utf-8") as f:
        raw_lines = f.read().splitlines()
    if not raw_lines or raw_lines[0].strip() != EL_TSV_HEADER:
        raise SidecarError(f"{path}：首行不是 {EL_TSV_HEADER!r}")
    rows = []
    for lineno, raw in enumerate(raw_lines[1:], start=2):
        if not raw.strip():
            continue
        parts = raw.split("\t")
        if len(parts) != 6:
            raise SidecarError(f"{path}:{lineno}：应有 6 个 TAB 字段，实得 {len(parts)}")
        id_s, type_s, width_s, tail_s, lo_s, hi_s = parts
        where = f"{path}:{lineno}"
        try:
            row = {"id": int(id_s), "type": type_s.strip(), "width": int(width_s),
                   "tail": int(tail_s) if tail_s.strip() else None,
                   "line_lo": int(lo_s), "line_hi": int(hi_s), "at": where}
        except ValueError as exc:
            raise SidecarError(f"{where}：字段不是整数（{exc}）") from exc
        if row["line_hi"] < row["line_lo"]:
            raise SidecarError(f"{where}：行区间 [{row['line_lo']},{row['line_hi']}) 上界小于下界")
        rows.append(row)
    if not rows:
        raise SidecarError(f"{path}：没有数据行")
    return rows


def build_elements(rows):
    """el.tsv 行 → 元件台账：同一 (type,id) 的多段合并，行区间数组化（spec §2）。"""
    by_key = {}
    for row in rows:
        key = (row["type"], row["id"])
        el = by_key.get(key)
        if el is None:
            by_key[key] = el = {"id": row["id"], "type": row["type"], "width": row["width"],
                                "lines": [], "tail": row["tail"]}
        else:
            if el["width"] != row["width"]:
                raise SidecarError(f"{row['at']}：元件 ({row['type']},{row['id']}) 的位宽 "
                                   f"{row['width']} 与先前的 {el['width']} 不一致")
            if row["tail"] is not None:
                if el["tail"] is not None and el["tail"] != row["tail"]:
                    raise SidecarError(f"{row['at']}：元件 ({row['type']},{row['id']}) 的 tail "
                                       f"{row['tail']} 与先前的 {el['tail']} 不一致")
                el["tail"] = row["tail"]
        el["lines"].append([row["line_lo"], row["line_hi"]])

    elements = list(by_key.values())
    owner = {}
    for el in elements:
        el["lines"].sort()
        if el["id"] in owner:
            raise SidecarError(f"id {el['id']} 在同一次编译里出现了两次（type {owner[el['id']]}"
                               f" 与 {el['type']}）—— 侧车要求 id 唯一，不静默配对")
        owner[el["id"]] = el["type"]
    elements.sort(key=lambda e: (e["id"], e["type"]))
    check_element_line_intervals(elements)
    return elements


def check_element_line_intervals(elements):
    """元件行区间两两不重叠 —— 重叠就意味着同一行能落到两个元件，join 会静默选错。

    空区间（`lo == hi`，元件注释后面紧跟下一个注释、中间没有源码行）可以存在，
    不参与重叠判定也不参与 join。
    """
    spans = []
    for el in elements:
        for lo, hi in el["lines"]:
            if hi < lo:
                raise SidecarError(f"元件 {el['id']} 的行区间 [{lo},{hi}) 上界小于下界")
            if hi > lo:
                spans.append((lo, hi, el["id"]))
    spans.sort()
    prev = None
    for lo, hi, eid in spans:
        if prev is not None and lo < prev[1]:
            raise SidecarError(f"行区间重叠：[{lo},{hi})（元件 {eid}）与 "
                               f"[{prev[0]},{prev[1]})（元件 {prev[2]}）")
        prev = (lo, hi, eid)


class ElementIndex:
    """「源码行 → 元件 id」的二分索引（行区间已验过互不重叠，空区间已排除）。"""

    def __init__(self, elements):
        self._spans = sorted((lo, hi, el["id"]) for el in elements
                             for lo, hi in el["lines"] if hi > lo)
        self._starts = [lo for lo, _, _ in self._spans]

    def find(self, line):
        i = bisect.bisect_right(self._starts, line) - 1
        if i < 0:
            return None
        lo, hi, eid = self._spans[i]
        return eid if lo <= line < hi else None


def join_segments(lines, elements):
    """`lines` × 元件行区间 → `segments`（blob 偏移区间 → 元件 id）。

    未被任何元件覆盖的机器段只留在 `lines` 里（spec §6）；相邻且同一元件的区间合并。
    """
    index = ElementIndex(elements)
    segments = []
    for span in lines:
        eid = index.find(span["line"])
        if eid is None:
            continue
        if segments and segments[-1]["id"] == eid and segments[-1]["end"] == span["start"]:
            segments[-1]["end"] = span["end"]
        else:
            segments.append({"start": span["start"], "end": span["end"], "id": eid})
    return segments


# ----------------------------------------------------------------- 跳板

def parse_stubs(path):
    """读 `<blob>.stubs`（`simplexc/cli/simplexc_cli.cc:246` 现成产物）→ sidecar `stubs`。

    表里只有 offset/kind/symbol 三列，`end` 是本文件的换算（stub +12 / slot +8）。
    """
    with open(path, encoding="utf-8") as f:
        raw_lines = f.read().splitlines()
    if not raw_lines or raw_lines[0].strip() != STUBS_HEADER:
        raise SidecarError(f"{path}：表头不是 {STUBS_HEADER!r}")
    stubs = []
    for lineno, raw in enumerate(raw_lines[1:], start=2):
        if not raw.strip():
            continue
        parts = raw.split("\t")
        if len(parts) != 3:
            raise SidecarError(f"{path}:{lineno}：应有 3 个 TAB 字段，实得 {len(parts)}")
        off_s, kind, name = parts
        if kind not in STUB_LEN:
            raise SidecarError(f"{path}:{lineno}：跳板类型 {kind!r} 不认识")
        try:
            start = int(off_s, 16)
        except ValueError as exc:
            raise SidecarError(f"{path}:{lineno}：offset {off_s!r} 不是十六进制") from exc
        stubs.append({"start": start, "end": start + STUB_LEN[kind], "kind": kind, "name": name})
    stubs.sort(key=lambda s: s["start"])
    return stubs


def check_stub_region(stubs, offsets):
    """跳板必须整段落在 offsets 给出的跳板区（`[stubs_base, slots_base)` / 槽区）。"""
    code_size = offsets["code_size"]
    for stub in stubs:
        if stub["end"] > code_size:
            raise SidecarError(f"跳板 {stub['name']} [{stub['start']},{stub['end']}) "
                               f"越过 code_size {code_size}")
        lo = offsets["stubs_base"] if stub["kind"] == "stub" else offsets["slots_base"]
        if stub["start"] < lo:
            raise SidecarError(f"跳板 {stub['name']} 起点 {stub['start']} 落在跳板区 {lo} 之前")
    if offsets["slots_base"] > offsets["stubs_base"] and not stubs:
        raise SidecarError("offsets 显示有跳板区（slots_base > stubs_base），但没有跳板条目"
                           " —— 缺 <blob>.stubs")


# ----------------------------------------------------------------- 时延段

TIMING_PAIR = re.compile(r"(\S+?)=(\d+)\s*ms")
# simplexc_cli --timing 的中文标签 → schema 键；sema 段目前不存在，恒 null（spec §6）
TIMING_LABELS = (("前端", "frontend"), ("lowering", "lowering"),
                 ("LLVM", "llvm"), ("装配", "assemble"))


def parse_timing_log(text):
    """`simplexc_cli --timing` 的 stderr → sidecar `meta`（spec §6）。"""
    stages: dict[str, int | None] = {"frontend": None, "sema": None, "lowering": None,
                                     "llvm": None, "assemble": None}
    seen = False
    for line in text.splitlines():
        if "[timing]" not in line:
            continue
        for label, value in TIMING_PAIR.findall(line):
            for prefix, key in TIMING_LABELS:
                if label.startswith(prefix):
                    stages[key] = int(value)
                    seen = True
    if not seen:
        raise SidecarError("timing 日志里没有可解析的 [timing] 段")
    return {"opt_level": OPT_LEVEL, "pass_pipeline": LLVM_PIPELINE, "stages": stages}


# ----------------------------------------------------------------- 不变式

def _check_sorted_non_overlapping(spans, what, limit, allow_empty=False):
    prev_end = 0
    for start, end in spans:
        if end == start and allow_empty:
            continue
        if end <= start:
            raise SidecarError(f"{what}：区间 [{start},{end}) 非正长度")
        if start < prev_end:
            raise SidecarError(f"{what}：区间 [{start},{end}) 与前一段结束 {prev_end} "
                               "重叠或未升序")
        if end > limit:
            raise SidecarError(f"{what}：区间 [{start},{end}) 越过上界 {limit}")
        prev_end = end


def check_invariants(sidecar, code_size):
    """整份 sidecar 的不变式（spec §6）—— 违反即抛，不修不猜。"""
    if sidecar.get("version") != SCHEMA_VERSION:
        raise SidecarError(f"sidecar version={sidecar.get('version')!r}，只认 {SCHEMA_VERSION}")
    if sidecar.get("compiler") not in ("ours", "ref"):
        raise SidecarError(f"sidecar compiler={sidecar.get('compiler')!r} 不认识")
    art = sidecar.get("artifact") or {}
    if not isinstance(art.get("len"), int) or not isinstance(art.get("sha256"), str):
        raise SidecarError("sidecar artifact 缺 len/sha256")
    if len(art["sha256"]) != 64:
        raise SidecarError(f"sidecar artifact.sha256 长度 {len(art['sha256'])}，应为 64")

    elements = sidecar["elements"]
    ids = {}
    for el in elements:
        if el["id"] in ids:
            raise SidecarError(f"elements 里 id {el['id']} 重复")
        ids[el["id"]] = el["type"]
    check_element_line_intervals(elements)

    for name in ("segments", "lines", "stubs"):
        arr = sidecar[name]
        _check_sorted_non_overlapping([(it["start"], it["end"]) for it in arr], name, code_size)

    index = ElementIndex(elements)
    for seg in sidecar["segments"]:
        if seg["id"] not in ids:
            raise SidecarError(f"segment [{seg['start']},{seg['end']}) 的元件 id {seg['id']} "
                               "不在 elements 里")

    seg_starts = [s["start"] for s in sidecar["segments"]]
    for span in sidecar["lines"]:
        i = bisect.bisect_right(seg_starts, span["start"]) - 1
        owner = None
        if i >= 0:
            seg = sidecar["segments"][i]
            if seg["start"] <= span["start"] and span["end"] <= seg["end"]:
                owner = seg["id"]
        want = index.find(span["line"])
        if owner != want:
            raise SidecarError(f"行区间 [{span['start']},{span['end']}) 第 {span['line']} 行："
                               f"segments 说 {owner}，行区间表说 {want} —— join 不自洽")
