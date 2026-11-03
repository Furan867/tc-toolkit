#!python3
"""`.debug_info` / `.debug_ranges` → 函数区间表（对象 `.text` 节内偏移，映射前）。

为什么必须读 abbrev 才判 `DW_AT_high_pc`：它的值含义由 **abbrev 里的 form** 决定 ——
`DW_FORM_addr` 是绝对结束地址、`DW_FORM_data*` 是长度。按「值比 low_pc 小就是长度」这类
启发式判，起点为 0 的函数（正常编译时第一个函数就是）会被判成长度，009 实测覆盖从
99.97% 掉到 74%。

为什么不去压成单区间：`DW_AT_ranges` 给的就是多段（内联/块搬动后的不连续代码），压成
`[min,max)` 会把中间别的东西算进这个函数。

为什么要切一遍最内层：DIE 是嵌套的（内联实例落在宿主函数里），直接拿原始段当查询表会
重叠。这里按 depth 最大者切出**全局互不重叠**的区间表，sidecar 与分析层都只认这张表。
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass, replace

import sidecar as sc

DEBUG_RANGES = ".debug_ranges"
# .debug_ranges 里的基址选择项：begin 取全 1 时，end 是新的基址（DWARF 4 §2.17.3）
BASE_ADDRESS_SELECTION = 0xFFFFFFFFFFFFFFFF
FUNC_TAGS = {"DW_TAG_subprogram": "subprogram", "DW_TAG_inlined_subroutine": "inline"}
# 长度口径的 high_pc form（DW_FORM_addr 是绝对地址，单独判）
LENGTH_FORMS = frozenset({"DW_FORM_data1", "DW_FORM_data2", "DW_FORM_data4",
                          "DW_FORM_data8", "DW_FORM_udata"})

ABBREV_DIE = re.compile(r"^\s+(\d+)\s+(DW_TAG_\w+)\s+\[(?:has|no) children\]\s*$")
ABBREV_ATTR = re.compile(r"^\s+(DW_AT_\w+)\s+(DW_FORM_\w+)\s*$")
DIE = re.compile(r"^ <(\d+)><([0-9a-f]+)>: Abbrev Number: (\d+)(?: \((DW_TAG_\w+)\))?")
DIE_ATTR = re.compile(r"^\s+<([0-9a-f]+)>\s+(DW_AT_\w+)\s*: (.*)$")
HEX_ROW = re.compile(r"^\s*([0-9a-f]{4,16})\s+([0-9a-f\s]+?)\s{2,}")
INDIRECT = re.compile(r"^\([^)]*\):\s*(.*)$")
REF = re.compile(r"<0x([0-9a-f]+)>")
NUMBER = re.compile(r"^(0x[0-9a-f]+|\d+)")


@dataclass(frozen=True)
class Function:
    """一个函数 DIE 的结论：身份（name/decl_line/origin）+ 它自己那段机器码。

    `ranges` 是**最内层切分后**归它的段（可能多段，也可能为空：抽象实例没有函数体，
    或者整段都被更内层的内联实例盖住了）。
    """

    func_id: int
    tag: str                      # "subprogram" | "inline"
    depth: int
    name: str | None
    decl_line: int | None
    origin: int | None            # 抽象实例的 func_id；没有 abstract_origin 则为 None
    ranges: tuple[tuple[int, int], ...]


# ------------------------------------------------------------------ 解析


def parse_abbrev(text):
    """`objdump --dwarf=abbrev` → {abbrev 号: {"tag": …, "attrs": {属性名: form}}}。"""
    table = {}
    current = None
    for line in text.splitlines():
        match = ABBREV_DIE.match(line)
        if match:
            current = {"tag": match.group(2), "attrs": {}}
            table[int(match.group(1))] = current
            continue
        match = ABBREV_ATTR.match(line)
        if match and current is not None:
            current["attrs"][match.group(1)] = match.group(2)
    if not table:
        raise sc.SidecarError("objdump --dwarf=abbrev 里没有 abbrev 表（对象没带 .debug_abbrev？）")
    return table


@dataclass
class Die:
    """一条 DIE：偏移/深度/abbrev 号 + 属性原文（解析中是先建后填，不冻结）。"""

    offset: int
    depth: int
    abbrev: int
    tag: str | None
    attrs: dict[str, str]        # 属性名 → objdump 打印的原始值文本
    cu_low_pc: int               # 所属 CU 的 low_pc（DW_AT_ranges 的相对基址）

    def attr_text(self, name):
        return self.attrs.get(name)


def parse_info(text):
    """`objdump --dwarf=info` → DIE 列表（按输出顺序 = 偏移升序）。"""
    dies = []
    current = None
    cu_low_pc = 0
    for line in text.splitlines():
        match = DIE.match(line)
        if match:
            current = Die(offset=int(match.group(2), 16), depth=int(match.group(1)),
                          abbrev=int(match.group(3)), tag=match.group(4),
                          attrs={}, cu_low_pc=cu_low_pc)
            dies.append(current)
            continue
        match = DIE_ATTR.match(line)
        if match and current is not None:
            current.attrs.setdefault(match.group(2), match.group(3).strip())
            if current.tag == "DW_TAG_compile_unit" and match.group(2) == "DW_AT_low_pc":
                cu_low_pc = _number(current.attrs["DW_AT_low_pc"])
                current.cu_low_pc = cu_low_pc
    if not dies:
        raise sc.SidecarError("objdump --dwarf=info 没有 DIE（对象没带 .debug_info？）")
    return dies


def parse_section_dump(text):
    """`objdump -s -j <节>` 的十六进制转储 → 节内容字节。

    objdump 每行 16 字节、按 4 字节一组打印（组内是内存顺序），行尾是 ASCII 列 ——
    解析到 16 字节就停，免得把恰好是十六进制字符的 ASCII 列吃进来。
    """
    data = bytearray()
    for line in text.splitlines():
        match = HEX_ROW.match(line)
        if not match:
            continue
        offset = int(match.group(1), 16)
        chunk = bytearray()
        for token in match.group(2).split():
            if len(token) % 2 or len(chunk) >= 16:
                break
            chunk += bytes.fromhex(token)
        if offset != len(data):
            raise sc.SidecarError(f"节转储不连续：行首偏移 {offset:#x}，已收 {len(data)} 字节")
        data += chunk
    if not data:
        raise sc.SidecarError("节转储里没有数据行（objdump -s 输出格式变了？）")
    return bytes(data)


def read_ranges(data, offset, base, die_offset):
    """`.debug_ranges` 的一段 → [(绝对起始, 绝对结束)]。

    `base` 初值是该 DIE 所在 CU 的 low_pc：表里的地址是相对基址的偏移，遇到基址选择项
    （begin 全 1）就换基址。多段原样保留，不压成单区间。
    """
    spans = []
    pos = offset
    while True:
        if pos + 16 > len(data):
            raise sc.SidecarError(f"DIE 0x{die_offset:x}：.debug_ranges@{offset:#x} 没有终止项 (0,0)")
        begin = int.from_bytes(data[pos:pos + 8], "little")
        end = int.from_bytes(data[pos + 8:pos + 16], "little")
        pos += 16
        if begin == BASE_ADDRESS_SELECTION:
            base = end
            continue
        if begin == 0 and end == 0:
            return spans
        if end <= begin:
            raise sc.SidecarError(f"DIE 0x{die_offset:x}：.debug_ranges 段 "
                                  f"[{begin:#x},{end:#x}) 非正长度")
        spans.append((base + begin, base + end))


# ------------------------------------------------------------------ 区间


def innermost_spans(functions):
    """嵌套 DIE 的段 → 互不重叠的 [(start, end, func_id)]，同一地址取 depth 最大的 DIE。"""
    events = []
    for function in functions:
        for start, end in function.ranges:
            events.append((start, 1, function.depth, function.func_id))
            events.append((end, -1, function.depth, function.func_id))
    events.sort(key=lambda item: (item[0], item[1]))

    active = {}                  # func_id → depth
    spans = []
    cursor = None
    for coord, delta, depth, func_id in events:
        if cursor is None:
            cursor = coord
        elif coord > cursor:
            owner = max(active, key=lambda fid: (active[fid], -fid)) if active else None
            if spans and spans[-1][2] == owner and spans[-1][1] == cursor:
                spans[-1][1] = coord
            elif owner is not None:
                spans.append([cursor, coord, owner])
            cursor = coord
        if delta > 0:
            active[func_id] = depth
        else:
            active.pop(func_id, None)
    return [(start, end, func_id) for start, end, func_id in spans]


def _number(text):
    match = NUMBER.match(text.strip())
    if match is None:
        raise sc.SidecarError(f"DWARF 属性值 {text!r} 不是数字")
    return int(match.group(1), 0)


def _name(die):
    for key in ("DW_AT_name", "DW_AT_linkage_name"):
        raw = die.attr_text(key)
        if raw is None:
            continue
        match = INDIRECT.match(raw)
        value = match.group(1) if match else raw
        if value:
            return value
    return None


def _reference(die):
    """`DW_AT_abstract_origin` / `DW_AT_specification` 指向的 DIE 偏移。"""
    for key in ("DW_AT_abstract_origin", "DW_AT_specification"):
        raw = die.attr_text(key)
        if raw is None:
            continue
        match = REF.search(raw)
        if match:
            return int(match.group(1), 16)
    return None


def _resolve(die, by_offset):
    """内联实例常不带 name/decl_line：沿 abstract_origin 链找回源的声明。

    返回 (name, decl_line, 第一跳的 origin 偏移)。
    """
    name = _name(die)
    decl_line = die.attr_text("DW_AT_decl_line")
    line = _number(decl_line) if decl_line is not None else None
    origin = _reference(die)
    current = by_offset.get(origin) if origin is not None else None
    seen = set()
    while current is not None and current.offset not in seen:
        seen.add(current.offset)
        name = name or _name(current)
        raw_line = current.attr_text("DW_AT_decl_line")
        if line is None and raw_line is not None:
            line = _number(raw_line)
        following = _reference(current)
        current = by_offset.get(following) if following is not None else None
    return name, line, origin


def _spans(die, abbrevs, ranges):
    """一个函数 DIE 自己的机器码段（对象坐标）。抽象实例没有函数体 → 空。"""
    low_raw = die.attr_text("DW_AT_low_pc")
    if low_raw is not None:
        high_raw = die.attr_text("DW_AT_high_pc")
        if high_raw is None:
            raise sc.SidecarError(f"DIE 0x{die.offset:x}：有 DW_AT_low_pc 但没有 DW_AT_high_pc")
        form = abbrevs.get(die.abbrev, {}).get("attrs", {}).get("DW_AT_high_pc")
        if form is None:
            raise sc.SidecarError(f"DIE 0x{die.offset:x}：abbrev {die.abbrev} 里查不到 "
                                  "DW_AT_high_pc 的 form —— 判不了它是长度还是绝对地址")
        low = _number(low_raw)
        high = _number(high_raw)
        if form == "DW_FORM_addr":
            end = high
        elif form in LENGTH_FORMS:
            end = low + high
        else:
            raise sc.SidecarError(f"DIE 0x{die.offset:x}：DW_AT_high_pc 的 form {form} "
                                  "既不是 DW_FORM_addr 也不在长度 form 里")
        if end <= low:
            raise sc.SidecarError(f"DIE 0x{die.offset:x}：区间 [{low:#x},{end:#x}) 非正长度")
        return ((low, end),)

    offset_raw = die.attr_text("DW_AT_ranges")
    if offset_raw is not None:
        return tuple(read_ranges(ranges, _number(offset_raw), die.cu_low_pc, die.offset))
    return ()                    # 抽象实例（DW_AT_inline 且无 low_pc）不生成函数体


# ------------------------------------------------------------------ 入口


def function_table(dies, abbrevs, ranges):
    """DIE 列表 → 函数表（按 DIE 偏移升序编号，ranges 已切成互不重叠）。"""
    by_offset = {die.offset: die for die in dies}
    ordered = []
    for die in dies:
        kind = FUNC_TAGS.get(die.tag)
        if kind is None:
            continue
        name, decl_line, origin = _resolve(die, by_offset)
        ordered.append((die, kind, name, decl_line, origin))
    if not ordered:
        raise sc.SidecarError("调试信息里没有函数 DIE（DW_TAG_subprogram / "
                              "DW_TAG_inlined_subroutine）—— 对象是不是没用 --debug-info 编？")
    func_of_die = {die.offset: index for index, (die, *_rest) in enumerate(ordered)}

    records = [
        Function(func_id=index, tag=kind, depth=die.depth, name=name, decl_line=decl_line,
                 origin=func_of_die.get(origin), ranges=_spans(die, abbrevs, ranges))
        for index, (die, kind, name, decl_line, origin) in enumerate(ordered)]
    owned = {}
    for start, end, func_id in innermost_spans(records):
        owned.setdefault(func_id, []).append((start, end))
    return [replace(record, ranges=tuple(owned.get(record.func_id, ()))) for record in records]


def run_objdump(objdump, arguments, obj_path):
    try:
        proc = subprocess.run([objdump, *arguments, str(obj_path)],
                              capture_output=True, text=True, check=False)
    except OSError as exc:
        raise sc.SidecarError(f"跑不了 objdump（{objdump}）：{exc}") from exc
    if proc.returncode != 0:
        raise sc.SidecarError(f"objdump {' '.join(arguments)} 失败（{proc.returncode}）："
                              f"{proc.stderr.strip()[:400]}")
    return proc.stdout


def build_functions(objdump, obj_path):
    """读对象的 DWARF 调试信息 → 函数表（对象 `.text` 节内偏移）。"""
    dies = parse_info(run_objdump(objdump, ["--dwarf=info"], obj_path))
    abbrevs = parse_abbrev(run_objdump(objdump, ["--dwarf=abbrev"], obj_path))
    ranges = b""
    if any(die.attr_text("DW_AT_ranges") is not None for die in dies):
        ranges = parse_section_dump(run_objdump(objdump, ["-s", "-j", DEBUG_RANGES], obj_path))
    return function_table(dies, abbrevs, ranges)
