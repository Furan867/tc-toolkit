#!python3
"""W3 验收：自造一份 DWARF4 COFF 样本，跑 make_sidecar / g2_check，并验三个负例。

样本全部**程序生成**（不手抄望值）：
  1. 按一张元件表生成 C 源（每个函数一行源码，行号即元件行区间）；
  2. `simplexc/.deps/llvm/bin/clang.exe -target x86_64-pc-windows-msvc -gdwarf-4` 现编；
  3. 解析 COFF 节表，按 `simplexc/assemble/assemble.cc:211-237` 的布局规则（类内顺序 =
     对象顺序、节前按节对齐、body_end = 末节尾）复刻 blob / offsets.json / .stubs；
  4. 跑 make_sidecar → 验 schema 与不变式；跑 g2_check --limit 20 → 三方对齐；
  5. 三个负例（id 重复 / 行区间重叠 / 锚点缺失）各必须非零退出。

W1 真产物到位后不用这个脚本，直接跑两个工具（样例见 W3 验收记录）：
    make_sidecar.py --blob <blob> --obj <module.obj> --elements <el.tsv> --offsets <offsets.json>
    g2_check.py --sidecar <sidecar.json> --obj <module.obj> --offsets <offsets.json> --blob <blob>
本脚本是 W1 还没产物时（或想复现判据时）的自造样本。

用法：
    python hotspot/tools/acceptance_sidecar.py
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from hotspot.tools import sidecar as sc  # noqa: E402

CLANG = ROOT / "simplexc/.deps/llvm/bin/clang.exe"
MAKE_SIDECAR = TOOLS / "make_sidecar.py"
G2_CHECK = TOOLS / "g2_check.py"

COFF_HEADER_LEN = 20
COFF_SECTION_LEN = 40
LAYOUT_CLASSES = ("text", "rdata", "data", "bss")
STUB_CODE = bytes([0x48, 0xB8]) + bytes(8) + bytes([0xFF, 0xE0])    # movabs rax,imm64; jmp rax

# (id, type, width, tail, 函数个数) —— 行区间 [行, 行+1) 由源码生成时逐行记录
ELEMENT_SPECS = (
    (7, "com_register_word", 8, 4294967296, 6),
    (9, "com_register_bit", 1, 4294967297, 6),
    (12, "com_log_gate", 1, None, 6),
    (15, "com_constant_byte", 8, 4294967298, 6),
)
HELPER_COUNT = 3        # 不属于任何元件的函数 → 未归属段（只有 lines，没有 segments）
TIMING_LOG = ("[timing] 前端=11 ms  lowering=22 ms  LLVM(O2+代码生成)=33 ms  "
              "装配=44 ms  合计=110 ms\n")


def section_align(chars):
    """IMAGE_SCN_ALIGN_* 在 chars 的 bits 20..23（assemble.cc:67-72）。"""
    code = (chars >> 20) & 0xF
    if code < 1 or code > 14:
        return 1
    return 1 << (code - 1)


def section_class(name):
    if "$zzz" in name or name.startswith((".comment", ".note")):
        return None
    for cls in LAYOUT_CLASSES:
        if name.startswith("." + cls):
            return cls
    return None


def parse_coff(path):
    """COFF 对象 → 保留节（与 assemble.cc 的 classify 同规则），含节内容与对齐。"""
    data = path.read_bytes()
    nsec = struct.unpack_from("<H", data, 2)[0]
    sym_ptr, nsym = struct.unpack_from("<II", data, 8)
    strtab = sym_ptr + nsym * 18
    sections = []
    for i in range(nsec):
        base = COFF_HEADER_LEN + i * COFF_SECTION_LEN
        raw_name = data[base:base + 8]
        if raw_name[:1] == b"/":
            offset = int(raw_name[1:].split(b"\0")[0], 10)
            name = data[strtab + offset:].split(b"\0", 1)[0].decode()
        else:
            name = raw_name.split(b"\0", 1)[0].decode()
        vsize, = struct.unpack_from("<I", data, base + 8)
        raw_size, raw_ptr = struct.unpack_from("<II", data, base + 16)
        chars, = struct.unpack_from("<I", data, base + 36)
        cls = section_class(name)
        if cls is None:
            continue
        body = data[raw_ptr:raw_ptr + raw_size] if raw_size and raw_ptr else b""
        sections.append({"sec": i + 1, "name": name, "class": cls,
                         "size": max(raw_size, vsize), "align": section_align(chars),
                         "body": body})
    return sections


def layout(sections):
    """复刻装配器的节布局 → 每节的 blob_off；返回 body_end。"""
    offset = 0
    for cls in LAYOUT_CLASSES:
        for sec in sections:
            if sec["class"] != cls:
                continue
            align = sec["align"] or 1
            offset = (offset + align - 1) // align * align
            sec["blob_off"] = offset
            offset += sec["size"]
    return offset


def function_plan():
    """排函数顺序：元件的函数**轮转交错**，否则相邻同元件的函数会合并成一大段。"""
    plan = []
    longest = max(spec[4] for spec in ELEMENT_SPECS)
    for k in range(longest):
        for eid, _etype, _width, _tail, count in ELEMENT_SPECS:
            if k < count:
                plan.append((eid, f"e{eid}_{k}", k))
        if k < HELPER_COUNT:
            plan.append((None, f"helper_{k}", k))
    return plan


def write_source(path):
    """生成 C 源：每个函数独占一行，行号记进元件行表（元件 → [行, 行+1)）。"""
    lines = ["/* W3 自造样本（由 tools/acceptance_sidecar.py 生成） */", ""]
    rows = []
    for eid, name, k in function_plan():
        lines.append(f"__declspec(noinline) int {name}(volatile int *p) "
                     f"{{ int s = 0; for (int i = 0; i < 8; i++) s += p[i] * {len(lines) + k}; "
                     f"return s ^ {len(lines) * 7 + k}; }}")
        if eid is None:
            continue
        etype, width, tail = next((s[1], s[2], s[3]) for s in ELEMENT_SPECS if s[0] == eid)
        rows.append({"id": eid, "type": etype, "width": width, "tail": tail,
                     "line_lo": len(lines), "line_hi": len(lines) + 1})
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return rows


def write_elements_tsv(path, rows):
    def cell(value):
        return "" if value is None else str(value)

    out = [sc.EL_TSV_HEADER]
    for row in rows:
        out.append("\t".join([str(row["id"]), row["type"], str(row["width"]),
                              cell(row["tail"]), str(row["line_lo"]), str(row["line_hi"])]))
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def build_sample(workdir):
    """造一份自洽样本 → (blob, obj, el.tsv, offsets, stubs, 生成时的元件行表)。"""
    src = workdir / "sample.c"
    rows = write_source(src)
    obj = workdir / "module.obj"
    subprocess.run([str(CLANG), "-target", "x86_64-pc-windows-msvc", "-g", "-gdwarf-4", "-O2",
                    "-c", str(src), "-o", str(obj)], check=True)

    sections = parse_coff(obj)
    body_end = layout(sections)
    stub_start = body_end                                        # 16 对齐 → 首个桩无填充
    slots_base = stub_start + sc.STUB_LEN["stub"]
    slot_start = slots_base                                      # 8 对齐 → 首个槽无填充
    code_size = slot_start + sc.STUB_LEN["slot"]

    payload = bytearray(code_size)
    for sec in sections:
        payload[sec["blob_off"]:sec["blob_off"] + len(sec["body"])] = sec["body"]
    payload[stub_start:stub_start + len(STUB_CODE)] = STUB_CODE
    total = sc.HEADER_LEN + code_size
    header = (total | (1 << 62)).to_bytes(sc.HEADER_LEN, "little")
    blob = workdir / "sample.bin"
    blob.write_bytes(header + bytes(payload))

    stubs = workdir / "sample.bin.stubs"
    stubs.write_text("\n".join([sc.STUBS_HEADER,
                                f"{stub_start:x}\tstub\tkernel32.Sleep",
                                f"{slot_start:x}\tslot\t__imp_kernel32_GetTickCount"]) + "\n",
                     encoding="utf-8")

    fingerprint = sc.blob_fingerprint(header + bytes(payload))
    text_sec = next(s for s in sections if s["class"] == "text")
    offsets = workdir / "sample.bin.offsets"
    offsets.write_text(json.dumps({
        "version": 1, "artifact": fingerprint, "header_len": sc.HEADER_LEN,
        "code_size": code_size, "entry": 0, "body_end": body_end,
        "stubs_base": stub_start, "slots_base": slots_base,
        "sections": [{"obj": 0, "obj_path": obj.name, "sec": s["sec"], "name": s["name"],
                      "class": s["class"], "blob_off": s["blob_off"], "size": s["size"],
                      "align": s["align"]} for s in sections],
        "debug_lines": [{"obj": 0, "obj_path": obj.name,
                         "anchors": [{"text_sec": text_sec["sec"],
                                      "blob_off": text_sec["blob_off"]}]}],
    }, indent=2) + "\n", encoding="utf-8")

    elements = workdir / "sample.el.tsv"
    write_elements_tsv(elements, rows)
    timing = workdir / "sample.timing.log"
    timing.write_text(TIMING_LOG, encoding="utf-8")
    return {"blob": blob, "obj": obj, "elements": elements, "offsets": offsets,
            "stubs": stubs, "timing": timing, "rows": rows, "body_end": body_end,
            "slots_base": slots_base, "fingerprint": fingerprint}


def run_tool(script, *args):
    return subprocess.run([sys.executable, str(script), *map(str, args)],
                          capture_output=True, text=True, check=False)


def expect_failure(script, args, keyword, what):
    proc = run_tool(script, *args)
    assert proc.returncode != 0, f"{what}：应非零退出，实得 {proc.returncode}\n{proc.stdout}"
    combined = proc.stdout + proc.stderr
    assert keyword in combined, f"{what}：报错里没有 {keyword!r}\n{combined}"
    print(f"  负例 {what}：rc={proc.returncode}，报错含 {keyword!r} ✓")
    return proc


def check_positive(sample, workdir):
    out = workdir / "sidecar.json"
    proc = run_tool(MAKE_SIDECAR, "--blob", sample["blob"], "--obj", sample["obj"],
                    "--elements", sample["elements"], "--offsets", sample["offsets"],
                    "--out", out, "--timing-log", sample["timing"])
    assert proc.returncode == 0, f"make_sidecar 失败：\n{proc.stdout}\n{proc.stderr}"
    print("  make_sidecar：" + proc.stdout.strip().splitlines()[-1])

    side = json.loads(out.read_text(encoding="utf-8"))
    assert side["version"] == sc.SCHEMA_VERSION and side["compiler"] == "ours"
    assert side["artifact"] == sample["fingerprint"]
    offsets = json.loads(sample["offsets"].read_text(encoding="utf-8"))
    sc.check_invariants(side, offsets["code_size"])          # 二次验，独立于生成侧

    ids = {el["id"] for el in side["elements"]}
    assert ids == {spec[0] for spec in ELEMENT_SPECS}, f"元件 id 集合不对：{sorted(ids)}"
    for el in side["elements"]:
        spec = next(s for s in ELEMENT_SPECS if s[0] == el["id"])
        assert el["type"] == spec[1] and el["width"] == spec[2] and el["tail"] == spec[3]
        want = [[r["line_lo"], r["line_hi"]] for r in sample["rows"] if r["id"] == el["id"]]
        assert el["lines"] == want, f"元件 {el['id']} 行区间 {el['lines']} != {want}"

    assert side["segments"], "segments 是空的"
    assert all(seg["id"] in ids for seg in side["segments"])
    covered = sum(seg["end"] - seg["start"] for seg in side["segments"])
    assert covered < offsets["body_end"], "segments 覆盖了整个 body —— 未归属段没被排除"
    seg_starts = {seg["start"] for seg in side["segments"]}
    unowned = [ln for ln in side["lines"]
               if not any(seg["start"] <= ln["start"] < seg["end"] for seg in side["segments"])]
    assert unowned, "没有未归属段（helper 函数应当只出现在 lines 里）"
    assert not any(ln["start"] in seg_starts and ln["line"] == 0 for ln in side["lines"])

    assert [(s["kind"], s["end"] - s["start"]) for s in side["stubs"]] == \
        [("stub", sc.STUB_LEN["stub"]), ("slot", sc.STUB_LEN["slot"])]
    assert side["stubs"][0]["start"] == sample["body_end"]
    assert side["stubs"][1]["start"] == sample["slots_base"]

    assert side["meta"]["stages"] == {"frontend": 11, "sema": None, "lowering": 22,
                                      "llvm": 33, "assemble": 44}, side["meta"]
    assert side["meta"]["opt_level"] == sc.OPT_LEVEL
    assert side["meta"]["pass_pipeline"] == sc.LLVM_PIPELINE
    print(f"  schema/不变式：{len(side['elements'])} 元件 / {len(side['segments'])} 段 / "
          f"{len(side['lines'])} 行区间 / {len(unowned)} 段未归属 / "
          f"{len(side['stubs'])} 跳板 / meta.stages ✓")

    want_n = min(20, len(side["segments"]))
    assert want_n == 20, f"样本段数不够 G2 抽 20 段：{len(side['segments'])}"
    proc = run_tool(G2_CHECK, "--sidecar", out, "--obj", sample["obj"],
                    "--offsets", sample["offsets"], "--blob", sample["blob"], "--limit", 20)
    assert proc.returncode == 0, f"G2 红：\n{proc.stdout}\n{proc.stderr}"
    tail = [ln for ln in proc.stdout.splitlines() if ln.startswith("G2：")]
    assert tail, f"G2 输出缺汇总行：\n{proc.stdout}"
    assert tail[-1].startswith(f"G2：{want_n}/{want_n} 段三方一致"), tail[-1]
    print("  " + tail[-1])
    return side, out


def check_negatives(sample, workdir):
    base_args = ["--blob", sample["blob"], "--obj", sample["obj"],
                 "--offsets", sample["offsets"], "--out", workdir / "neg.json"]
    rows = sample["rows"]

    dup = workdir / "dup.el.tsv"
    write_elements_tsv(dup, rows + [dict(rows[0], type="com_log_gate")])
    expect_failure(MAKE_SIDECAR, base_args + ["--elements", dup],
                   "出现了两次", "id 重复")

    overlap = workdir / "overlap.el.tsv"
    bad_row = dict(rows[0], id=99, line_lo=rows[0]["line_lo"], line_hi=rows[1]["line_hi"] + 5)
    write_elements_tsv(overlap, rows + [bad_row])
    expect_failure(MAKE_SIDECAR, base_args + ["--elements", overlap],
                   "行区间重叠", "行区间重叠")

    offsets = json.loads(sample["offsets"].read_text(encoding="utf-8"))
    offsets["debug_lines"][0]["anchors"] = []
    no_anchor = workdir / "noanchor.offsets.json"
    no_anchor.write_text(json.dumps(offsets, indent=2) + "\n", encoding="utf-8")
    expect_failure(MAKE_SIDECAR, ["--blob", sample["blob"], "--obj", sample["obj"],
                                  "--elements", sample["elements"], "--offsets", no_anchor,
                                  "--out", workdir / "neg.json"],
                   "没有 anchors", "锚点缺失")


def main(argv=None):
    ap = argparse.ArgumentParser(description="W3 验收（自造样本 + G2 + 三个负例）")
    ap.add_argument("--dir", help="样本目录（默认新建临时目录）")
    args = ap.parse_args(argv)

    if not CLANG.is_file():
        print(f"错误：找不到 {CLANG}（先跑 python simplexc/tools/build.py objs？）",
              file=sys.stderr)
        return 2
    workdir = Path(args.dir) if args.dir else Path(tempfile.mkdtemp(prefix="w3-sidecar-"))
    workdir.mkdir(parents=True, exist_ok=True)
    print(f"样本目录 {workdir}")

    sample = build_sample(workdir)
    print(f"样本：blob {sample['blob'].stat().st_size} B / body_end {sample['body_end']} / "
          f"{len(sample['rows'])} 行元件记录")

    side, out = check_positive(sample, workdir)
    print(f"  正例：{out.name} 已产出（{out.stat().st_size} B）")
    check_negatives(sample, workdir)

    digest = hashlib.sha256(out.read_bytes()).hexdigest()[:16]
    print(f"验收通过：sidecar {digest}… / {len(side['segments'])} 段 / G2 绿 / 三个负例红")
    return 0


if __name__ == "__main__":
    sys.exit(main())
