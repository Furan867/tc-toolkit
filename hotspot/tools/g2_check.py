#!python3
"""G2 抽查（spec §8）：sidecar `segments` 均匀取 N 段，逐段做三方对齐。

三方 = ①`objdump --dwarf=decodedline`（DWARF 行程序的原始地址→行）
②sidecar 的 `lines`/`segments`（我们 join 出来的结论，坐标已重映射成 blob 偏移）
③`objdump -d -l` 的反汇编行注（objdump 自己的 DWARF 消费者 + 反汇编器，独立于 ①②）
三者对**该段首地址**给出的行号必须一致 —— 不一致就说明重映射或 join 错了。
sidecar 的行按来源文件绑定（file + 文件内行），比较前按 offsets 的 `line_offset` 把源文件的
行加回拼接后行号（stdlib 行本来就没动过）。

坐标：sidecar 是 blob 偏移；objdump 是对象 `.text` 节内偏移，按 `--offsets` 的 anchors
换算（blob 偏移 − anchor），与 make_sidecar 用的是同一份基准。

用法：
    python hotspot/tools/g2_check.py --sidecar sidecar.json --obj build/module.obj \\
        --offsets build/offsets.json --blob work/hook/x.bin [--limit 20]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from hotspot.tools import sidecar as sc  # noqa: E402

INSN_ROW = re.compile(r"^\s*(?P<addr>[0-9a-f]+):\t")
LINE_MARKER = re.compile(r"^(?P<file>.+?):(?P<line>\d+)(?:\s+\(discriminator\s+\d+\))?\s*$")


def parse_disasm_lines(text):
    """`objdump -d -l` → {指令地址: 行号}（没被行注覆盖的指令不记）。

    行注是流式的（只在行号变化处打一条 `/path/file.c:42`），所以「当前行」跟着走；
    新函数标签处**不重置** —— 内联代码块的开头常常没有新行注，重置会把正常的归属丢掉。
    """
    out = {}
    cur_line = None
    for raw_line in text.splitlines():
        line = raw_line.rstrip("\r")
        match = INSN_ROW.match(line)
        if match:
            if cur_line is not None:
                out[int(match.group("addr"), 16)] = cur_line
            continue
        match = LINE_MARKER.match(line)
        if match:
            cur_line = int(match.group("line"))
    return out


def run_objdump(argv):
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, check=False)
    except OSError as exc:
        raise sc.SidecarError(f"跑不了 {argv[0]}：{exc}") from exc
    if proc.returncode != 0:
        raise sc.SidecarError(f"{' '.join(argv)} 失败（{proc.returncode}）："
                              f"{proc.stderr.strip()[:400]}")
    return proc.stdout


def sidecar_line_map(offsets, sidecar):
    """sidecar `lines` 的行是 (file, 文件内行)，decodedline 与 objdump -l 给的是**拼接后**行号。

    源文件的行要加回前缀行数才能与 objdump 对得上；stdlib 行本来就没动过。旧 sidecar 没有
    `file`：行号原样用。
    """
    line_offset = offsets.get("line_offset")
    if not isinstance(line_offset, int) or isinstance(line_offset, bool):
        line_offset = 0
    source_file = next((s.get("file") for s in (offsets.get("sources") or [])
                        if isinstance(s, dict) and s.get("role") == "source"), None)
    out = {}
    for span in sidecar["lines"]:
        name = span.get("file")
        spliced = span["line"] + line_offset if name is not None and name == source_file \
            else span["line"]
        out[span["start"]] = (name, span["line"], spliced)
    return out


class Sources:
    """三方数据源，按需缓存 —— 一次 G2 只调两三次 objdump。"""

    def __init__(self, objdump, obj_path, sections_by_key, anchor_obj):
        self.objdump = objdump
        self.obj_path = obj_path
        self.sections_by_key = sections_by_key
        self.anchor_obj = anchor_obj
        self.line_ranges = {}
        self.disasm = {}

    def line_range_starts(self):
        if not self.line_ranges:
            text = run_objdump([self.objdump, "--dwarf=decodedline", self.obj_path])
            for rows in sc.parse_decodedline(text):
                for start, end, line in sc.line_ranges(rows):
                    self.line_ranges[start] = (end, line)
        return self.line_ranges

    def disasm_lines(self, text_sec):
        if text_sec not in self.disasm:
            sec = self.sections_by_key[(self.anchor_obj, text_sec)]
            text = run_objdump([self.objdump, "-d", "-l",
                                f"--section={sec['name']}", self.obj_path])
            self.disasm[text_sec] = parse_disasm_lines(text)
        return self.disasm[text_sec]


def anchor_of(anchors, blob_offset):
    hits = [a for a in anchors if a["blob_off"] <= blob_offset < a["blob_off"] + a["size"]]
    if len(hits) != 1:
        raise sc.SidecarError(f"{blob_offset} 落在 {len(hits)} 个 .text 锚点内，无法换算")
    return hits[0]


def sample_uniform(items, count):
    """均匀取 count 段（确定性：`i*len//count`），不足 count 时全取。"""
    if count <= 0:
        raise sc.SidecarError(f"--limit={count} 必须为正")
    if len(items) <= count:
        return list(items)
    return [items[i * len(items) // count] for i in range(count)]


def main(argv=None):
    ap = argparse.ArgumentParser(description="G2：sidecar segments 三方对齐抽查")
    ap.add_argument("--sidecar", required=True, help="make_sidecar.py 产出的 sidecar.json")
    ap.add_argument("--obj", required=True, help="同一份 LLVM 对象文件")
    ap.add_argument("--offsets", required=True, help="<blob>.offsets（anchors 来源）")
    ap.add_argument("--blob", required=True, help="产物文件（验 fingerprint 与不变式上界）")
    ap.add_argument("--limit", type=int, default=20, help="抽样段数（默认 20）")
    ap.add_argument("--objdump", default="objdump", help="objdump 可执行文件")
    args = ap.parse_args(argv)

    try:
        with open(args.sidecar, encoding="utf-8") as f:
            sidecar = json.load(f)
        blob = sc.read_blob(args.blob)
        offsets = sc.load_offsets(args.offsets, blob)
        if sidecar.get("artifact") != sc.blob_fingerprint(blob):
            raise sc.SidecarError("sidecar 的 artifact 指纹与 --blob 不符（不是同一份产物）")
        sc.check_invariants(sidecar, offsets["code_size"])
        entry = sc.select_debug_lines(offsets, args.obj)
        anchors = sc.build_anchors(offsets, entry)
    except sc.SidecarError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1

    if not sidecar["segments"]:
        print("错误：sidecar 没有 segments，无可抽查（行表或元件表是空的？）", file=sys.stderr)
        return 1

    line_at = sidecar_line_map(offsets, sidecar)
    sections_by_key = {(s["obj"], s["sec"]): s for s in offsets["sections"]}
    sources = Sources(args.objdump, args.obj, sections_by_key, entry["obj"])
    dl_starts = sources.line_range_starts()

    picked = sample_uniform(sidecar["segments"], args.limit)
    bad = []
    for i, seg in enumerate(picked, start=1):
        try:
            anchor = anchor_of(anchors, seg["start"])
        except sc.SidecarError as exc:
            bad.append((seg, None, None, None, str(exc)))
            print(f"[{i:>2}/{len(picked)}] blob 0x{seg['start']:04x} 元件 {seg['id']}  ✗ {exc}")
            continue
        obj_addr = seg["start"] - anchor["blob_off"]
        dl_line = dl_starts.get(obj_addr, (None, None))[1]
        side_file, side_shown, side_line = line_at.get(seg["start"], (None, None, None))
        dis_line = sources.disasm_lines(anchor["sec"]).get(obj_addr)
        shown = f"{side_file}:{side_shown}" if side_file else side_shown
        head = (f"[{i:>2}/{len(picked)}] blob 0x{seg['start']:04x}-0x{seg['end']:04x} "
                f"元件 {seg['id']}  行 decodedline={dl_line} sidecar={shown} "
                f"objdump-dl={dis_line}")
        if None in (dl_line, side_line, dis_line):
            bad.append((seg, dl_line, side_line, dis_line, "有一方给不出该地址的行号"))
            print(f"{head}  ✗ 缺数据")
        elif dl_line != side_line or side_line != dis_line:
            bad.append((seg, dl_line, side_line, dis_line, "三方不一致"))
            print(f"{head}  ✗ 不一致")
        else:
            print(f"{head}  ok")

    print(f"G2：{len(picked) - len(bad)}/{len(picked)} 段三方一致"
          f"（抽样总体 {len(sidecar['segments'])} 段，均匀取样）")
    if bad:
        print("不合格段：", file=sys.stderr)
        for seg, dl_line, side_line, dis_line, why in bad:
            print(f"  blob 0x{seg['start']:04x}-0x{seg['end']:04x} 元件 {seg['id']}：{why}"
                  f"（decodedline={dl_line} sidecar={side_line} objdump-dl={dis_line}）",
                  file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
