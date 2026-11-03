#!python3
"""生成 sidecar.json（spec §6 `ours` Provider）—— 构建后离线跑，只读装配器产物。

输入：LLVM 对象（`.debug_line` 行表，W1 产）+ `el.tsv`（元件表，W1 产）+
`<blob>.offsets`（偏移基准，W1 产）+ 产物本身 + 现成的 `<blob>.stubs`。
输出：`segments`（blob 偏移区间 → 元件）、`lines`（全部有行号信息的机器段，含未归属段）、
`stubs`、`elements`（按 (type,id) 合并的台账）、`meta`（可选 `--timing-log`）。

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

    elements = sc.build_elements(sc.parse_elements_tsv(args.elements))
    cus = sc.parse_decodedline(run_objdump_decodedline(args.objdump, args.obj))
    lines = sc.build_lines(cus, anchors, offsets["body_end"])
    segments = sc.join_segments(lines, elements)

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
