#!python3
"""生成 sidecar.json（spec §6 `ours` Provider）—— 构建后离线跑，只读装配器产物。

输入：LLVM 对象（`.debug_line` 行表，W1 产）+ `el.tsv`（元件表，W1 产）+
`<blob>.offsets`（偏移基准，W1 产）+ 产物本身 + 现成的 `<blob>.stubs`。
输出：`segments`（blob 偏移区间 → 元件）、`lines`（全部有行号信息的机器段，含未归属段；
行号已按 offsets 的 `sources` 绑回 (file, 文件内行)）、`stubs`、`elements`（按 (type,id)
合并的台账）、`meta`（可选 `--timing-log` + 原样抄过来的 `sources`）。

为什么行号要绑定来源文件：`--stdlib` 把前缀拼在源码前，`.debug_line` 里源码的行全部
`+ 前缀行数`；而 `el.tsv` 记的是源文件内行号 —— 不拆回 (file, 行)，两边的行号就会
差一个前缀（源码行被标成 stdlib、stdlib 行去查源文件的元件表）。

为什么不能只靠对象文件：`.debug_line` 的地址是对象 `.text` 节内偏移（对象节 VMA=0），
装配器又把 `.debug*` 全丢了（`simplexc/assemble/assemble.cc:163-172`），所以「对象地址
→ blob 偏移」只能靠 W1 另吐的 anchors 重映射。这条链路断了就报错，不猜。

用法：
    python hotspot/tools/make_sidecar.py --blob work/hook/x.bin --obj build/module.obj \\
        --elements work/hook/x.el.tsv --offsets work/hook/x.bin.offsets --out sidecar.json
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

import sidecar as sc


def run_objdump_decodedline(objdump, obj_path):
    """调 objdump 拿行表文本。非零退出/空输出直接报错 —— 空行表会静默产出空 sidecar。"""
    try:
        proc = subprocess.run([objdump, "--dwarf=decodedline", obj_path],
                              capture_output=True, text=True, check=False)
    except OSError as exc:
        raise sc.SidecarError(f"跑不了 objdump（{objdump}）：{exc}") from exc
    if proc.returncode != 0:
        raise sc.SidecarError(f"objdump --dwarf=decodedline 失败（{proc.returncode}）："
                              f"{proc.stderr.strip()[:400]}")
    if "Line number" not in proc.stdout:
        raise sc.SidecarError(f"{obj_path}：objdump --dwarf=decodedline 没有行表"
                              "（对象没带 DWARF 行信息？）")
    return proc.stdout


def build_sidecar(args):
    blob = sc.read_blob(args.blob)
    offsets = sc.load_offsets(args.offsets, blob)
    entry = sc.select_debug_lines(offsets, args.obj)
    anchors = sc.build_anchors(offsets, entry)

    line_offset, stdlib_file, source_file, sources = load_sources(offsets)
    elements = sc.build_elements(sc.parse_elements_tsv(args.elements))
    cus = sc.parse_decodedline(run_objdump_decodedline(args.objdump, args.obj))
    lines = bind_files(sc.build_lines(cus, anchors, offsets["body_end"]),
                       line_offset, stdlib_file, source_file)
    # elements 的行区间是**源文件内**行号，所以只有源文件的行能 join 出 segments：stdlib 前缀的
    # 行没有元件注释，拿它去查源文件的元件表会命中值域相同的行（两边的行号都从 1 数起）。
    own = [span for span in lines if source_file is None or span["file"] == source_file]
    segments = sc.join_segments(own, elements)

    stubs_path = args.stubs or (args.blob + ".stubs")
    try:
        stubs = sc.parse_stubs(stubs_path)
    except FileNotFoundError:
        stubs = []
    sc.check_stub_region(stubs, offsets)

    meta = None
    if args.timing_log:
        with open(args.timing_log, encoding="utf-8", errors="replace") as f:
            meta = sc.parse_timing_log(f.read())
    if sources is not None:
        meta = dict(meta or {})
        meta["sources"] = sources
        for element in elements:
            element["source"] = source_file

    sidecar = {
        "version": sc.SCHEMA_VERSION,
        "compiler": "ours",
        "artifact": sc.blob_fingerprint(blob),
        "meta": meta,
        "elements": elements,
        "segments": segments,
        "lines": lines,
        "stubs": stubs,
    }
    sc.check_invariants(sidecar, offsets["code_size"])
    return sidecar


def load_sources(offsets):
    """offsets 的 `line_offset` / `sources` → (line_offset, stdlib 文件名, 源文件名, sources)。

    旧 offsets（W1 修之前）没有这两个字段：返回 line_offset=0、文件名为 None、sources=None，
    行号就按拼接后原样用、也不写 `file` —— 旧产物与旧验收脚本照旧能跑。
    """
    line_offset = offsets.get("line_offset")
    sources = offsets.get("sources")
    if line_offset is None and sources is None:
        return 0, None, None, None
    if not isinstance(line_offset, int) or isinstance(line_offset, bool) or line_offset < 0:
        raise sc.SidecarError(f"offsets line_offset={line_offset!r} 不是非负整数")
    if not isinstance(sources, list) or not sources:
        raise sc.SidecarError("offsets 有 line_offset 但没有 sources")
    by_role = {}
    for item in sources:
        if not isinstance(item, dict):
            raise sc.SidecarError(f"offsets sources 条目不是对象：{item!r}")
        role, name, lines = item.get("role"), item.get("file"), item.get("lines")
        if not isinstance(name, str) or not name:
            raise sc.SidecarError(f"offsets sources 条目缺 file：{item!r}")
        if not isinstance(lines, int) or isinstance(lines, bool) or lines < 0:
            raise sc.SidecarError(f"offsets sources 条目 {name} 的 lines={lines!r} 不是非负整数")
        if role in by_role:
            raise sc.SidecarError(f"offsets sources 里 role={role!r} 出现两次")
        by_role[role] = item
    stdlib, source = by_role.get("stdlib"), by_role.get("source")
    if source is None:
        raise sc.SidecarError('offsets sources 里没有 role="source" 的条目')
    if stdlib is None and line_offset != 0:
        raise sc.SidecarError(f"offsets line_offset={line_offset} 但没有 stdlib 条目")
    if stdlib is not None and stdlib["lines"] != line_offset:
        raise sc.SidecarError(f"offsets 的 stdlib lines={stdlib['lines']} 与 "
                              f"line_offset={line_offset} 不符（前缀行数口径不一致）")
    return line_offset, (stdlib["file"] if stdlib else None), source["file"], sources


def bind_files(lines, line_offset, stdlib_file, source_file):
    """拼接后行号 → (file, 文件内行)：`line <= line_offset` 属 stdlib 前缀，行号不变。"""
    if source_file is None:
        return lines
    for span in lines:
        if span["line"] <= line_offset:
            span["file"] = stdlib_file
        else:
            span["file"] = source_file
            span["line"] -= line_offset
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description="生成 sidecar.json（blob 偏移 → 元件/行/跳板）")
    ap.add_argument("--blob", required=True, help="产物文件（含 8 字节头）")
    ap.add_argument("--obj", required=True, help="LLVM 对象文件（带 .debug_line）")
    ap.add_argument("--elements", required=True, help="元件表 el.tsv（W1 --elements-out）")
    ap.add_argument("--offsets", required=True, help="偏移基准 <blob>.offsets（W1 --offsets-out）")
    ap.add_argument("--out", required=True, help="输出 sidecar.json")
    ap.add_argument("--stubs", help="跳板表（默认 <blob>.stubs；不存在且 offset 无跳板区时为空）")
    ap.add_argument("--timing-log", help="simplexc_cli --timing 的 stderr 落盘（→ meta）")
    ap.add_argument("--objdump", default="objdump", help="objdump 可执行文件（默认 PATH 里的）")
    args = ap.parse_args(argv)

    try:
        sidecar = build_sidecar(args)
    except sc.SidecarError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(sidecar, f, ensure_ascii=False, indent=2)
        f.write("\n")

    covered = sum(s["end"] - s["start"] for s in sidecar["segments"])
    print(f"sidecar → {args.out}：{len(sidecar['elements'])} 元件 / "
          f"{len(sidecar['segments'])} 段（覆盖 {covered} B）/ "
          f"{len(sidecar['lines'])} 行区间 / {len(sidecar['stubs'])} 跳板")
    return 0


if __name__ == "__main__":
    sys.exit(main())
