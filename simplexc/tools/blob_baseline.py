#!python3
"""产物基线回归：重构（lower/ → sema/ + codegen/）前后，每一份语料的装配产物必须等价。

比什么
------
不是比 `.obj` —— COFF 头带编译时间戳，同一份源码两次编译天然不同。比的是 CLI 写出的
**装配后 blob** `<id>.bin` 及 sidecar `<id>.bin.stubs` / `<id>.bin.entry`。每份语料一行清单：

    id  status  blob_len  entry  stub_count  norm_sha256  stubs_sha256  raw_sha256

判等只用前 6 列（`CMP_FIELDS`）；`raw_sha256` 只记不比，它含 kernel32 绝对地址，跨 boot 就变。
`stubs_sha256` 单独比是因为「桩块整体前移」这类错误只让偏移变、归一化后产物仍相等，
直接指出来比看字节差好查。比较规则见 `simplexc/tools/baseline/README.md`。

两个子命令
----------
    # 落基线（清单进 git，产物不进）
    python simplexc/tools/blob_baseline.py --record simplexc/tools/baseline

    # 判据（check.py 的 blob_baseline 步骤就是这条）
    python simplexc/tools/blob_baseline.py --verify simplexc/tools/baseline

`--record` 把清单写进 `<DIR>/corpus-baseline_<YYYY-MM-DD>.tsv`，blob 与 sidecar 落 `--blobs`
（默认 `work/tmp/baseline-blobs/`，体积大不进 git）；`--verify` 用**同形**的清单与产物目录重跑
98 份、逐份比 6 个字段，差异报「文件 / 字段 / 期望 vs 实际 / 首个差异字节区间」。

退出码：0 = 98 份全一致；1 = 有差异（或基线外新增 / 本轮缺失）；2 = 参数或环境（缺 CLI、
缺清单、缺产物目录、清单有多份）。
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import glob
import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
ROOT = os.path.dirname(SIMPLEXC)                        # tc-toolkit/
CORPUS = os.path.join(SIMPLEXC, "corpus")
MANIFEST = os.path.join(CORPUS, "manifest.tsv")           # 名字会撞上面的 --manifest 参数，故全大写
CLI = os.path.join(SIMPLEXC, "build", "simplexc_cli.exe")

BASELINE_GLOB = "corpus-baseline_*.tsv"
DEFAULT_BLOBS = os.path.join(ROOT, "work", "tmp", "baseline-blobs")
DEFAULT_WORKDIR = os.path.join(ROOT, "work", "tmp", "blob_baseline", "run")

HEADER_LEN = 8          # blob[0:8] 是头 word（len | 1<<62），机器码从 +8 起
STUB_IMM_OFF = 2        # 代码桩 `48 B8 <imm64> FF E0`：变的是从桩内 +2 起的 imm64
STUB_IMM_LEN = 8
SLOT_LEN = 8            # `__imp_X` 数据槽：整 8 字节就是 X 的绝对地址

CMP_FIELDS = ("status", "blob_len", "entry", "stub_count", "norm_sha256", "stubs_sha256")
RECORD_FIELDS = CMP_FIELDS + ("raw_sha256",)


class BaselineError(RuntimeError):
    """参数 / 环境 / 读写失败 —— 与「产物不一致」区分开：前者 rc=2，后者 rc=1。"""


def utf8_stdout():
    """控制台是 GBK（chcp 936），中文/→/≠ 不转 UTF-8 会花掉（与 check.py 同一处理）。"""
    for stream in (sys.stdout, sys.stderr):
        rec = getattr(stream, "reconfigure", None)
        if rec:
            with contextlib.suppress(Exception):
                rec(encoding="utf-8", errors="replace")


def rel(path):
    """输出里一律用相对 tc-toolkit/ 的路径（换机器/换盘符也读得顺）。"""
    try:
        return os.path.relpath(path, ROOT).replace("\\", "/")
    except ValueError:
        return path


# ---- 读写（每条都带「路径 + 原因」，失败也是非零退出）----------------------
def read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        raise BaselineError(f"读不了 {rel(path)}：{e}") from e


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise BaselineError(f"读不了 {rel(path)}：{e}") from e


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    except OSError as e:
        raise BaselineError(f"读不了 {rel(path)}：{e}") from e
    return h.hexdigest()


def remove_stale(path):
    """删上一轮的产物；删不掉要报（否则下一行读到的是旧文件，判等会白过）。"""
    if not os.path.exists(path):
        return
    try:
        os.remove(path)
    except OSError as e:
        raise BaselineError(f"删不了上一轮的产物 {rel(path)}：{e}") from e


def ensure_dir(path):
    try:
        os.makedirs(path, exist_ok=True)
    except OSError as e:
        raise BaselineError(f"建不了目录 {rel(path)}：{e}") from e


# ---- 归一化规则（唯一定义处，README 只描述）--------------------------------
def masked_ranges(stubs_path):
    """`<blob>.stubs` → [(blob 内起点, 长度)]：只有这几段字节由**进程实例**决定。

    sidecar 的 offset 坐标系是机器码起点（= blob + 8，`assemble.hh` §StubRef），所以落盘偏移要 +8。
    代码桩只掩 imm64 —— `48 B8 <imm64> FF E0` 的前后两字节是确定的；数据槽整 8 字节都掩。
    掩多了等于放弃判据，掩少了跨 boot 就误报。
    """
    lines = read_text(stubs_path).splitlines()
    if not lines or not lines[0].startswith("offset\t"):
        raise BaselineError(f"{rel(stubs_path)} 不是 .stubs sidecar（首行应为 offset/kind/symbol）")
    ranges = []
    for line in lines[1:]:
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            raise BaselineError(f"{rel(stubs_path)} 有一行缺 offset/kind 列：{line!r}")
        try:
            off = int(parts[0], 16)
        except ValueError as e:
            raise BaselineError(f"{rel(stubs_path)} 的偏移 {parts[0]!r} 不是十六进制：{e}") from e
        kind = parts[1]
        if kind == "slot":
            ranges.append((HEADER_LEN + off, SLOT_LEN))
        elif kind == "stub":
            ranges.append((HEADER_LEN + off + STUB_IMM_OFF, STUB_IMM_LEN))
        else:
            raise BaselineError(f"{rel(stubs_path)} 有未知 kind {kind!r}")
    return ranges


def normalize(blob, ranges, where):
    """把绝对地址的字节清零，其余一个字节都不动。返回归一化后的字节。"""
    buf = bytearray(blob)
    for start, count in ranges:
        if start + count > len(buf):
            raise BaselineError(f"{where} 的落点 {start:#x}..{start + count - 1:#x} "
                                f"超出产物长度 {len(buf)}")
        buf[start:start + count] = bytes(count)
    return bytes(buf)


def byte_spans(offsets):
    """偏移列表 → `["0x40..0x40", …]`（相邻合并），报告里指得出第几个字节。"""
    out = []
    for i in sorted(offsets):
        if out and i == out[-1][1]:
            out[-1][1] = i + 1
        else:
            out.append([i, i + 1])
    return [f"{start:#x}..{end - 1:#x}" for start, end in out]


def byte_diff(base_blob, now_blob):
    """两份产物的**归一化**字节差。跨 boot 时 raw 不同，归一化后才可比。

    返回 (差异处数, 区间列表)；长度不同时返回 (None, []) —— 那时「第几个字节」无意义，
    字段里的 `blob_len` 已经报了。
    """
    base = normalize(read_bytes(base_blob), masked_ranges(base_blob + ".stubs"), rel(base_blob))
    now = normalize(read_bytes(now_blob), masked_ranges(now_blob + ".stubs"), rel(now_blob))
    if len(base) != len(now):
        return None, []
    offsets = [i for i in range(len(base)) if base[i] != now[i]]
    return len(offsets), byte_spans(offsets)


# ---- 跑语料 -------------------------------------------------------------
def read_corpus_manifest():
    """`simplexc/corpus/manifest.tsv` → [(id, file)]。"""
    lines = read_text(MANIFEST).splitlines()
    if not lines:
        raise BaselineError(f"语料清单 {rel(MANIFEST)} 是空的")
    header = lines[0].split("\t")
    for col in ("id", "file"):
        if col not in header:
            raise BaselineError(f"语料清单 {rel(MANIFEST)} 缺 {col} 列：{lines[0]!r}")
    i_id, i_file = header.index("id"), header.index("file")
    rows = []
    for line in lines[1:]:
        if not line.strip():
            continue
        parts = line.split("\t")
        rows.append((parts[i_id], parts[i_file]))
    return rows


def run_cli(src, out_path):
    """跑一次 CLI（一个进程 = 游戏里一次编译请求同形）。

    `cwd=ROOT`：CLI 默认 `--objs-dir simplexc/build/objs` 是相对路径，换了 cwd 就找不到入口垫片。
    """
    if not os.path.exists(CLI):
        raise BaselineError(f"缺 CLI {rel(CLI)}：先跑 python simplexc/tools/build.py")
    try:
        return subprocess.run([CLI, "--src", src, "--out", out_path], cwd=ROOT,
                              capture_output=True, encoding="utf-8", errors="replace")
    except OSError as e:
        raise BaselineError(f"跑不了 CLI {rel(CLI)}：{e}") from e


def first_line(text, limit=160):
    lines = (text or "").strip().splitlines()
    return lines[0].strip()[:limit] if lines else ""


def blob_fields(blob_path):
    """一份成功产物的字段（`status` 由调用方按 rc 给）。值一律是**字符串** —— 清单是文本，
    记录与比较用同一种拼写，来回读一遍不会因为 int/str 差一个空格判成不等。
    """
    blob = read_bytes(blob_path)
    ranges = masked_ranges(blob_path + ".stubs")
    norm = normalize(blob, ranges, rel(blob_path))
    return {
        "blob_len": str(len(blob)),
        "entry": read_text(blob_path + ".entry").strip(),
        "stub_count": str(len(ranges)),
        "norm_sha256": sha256_bytes(norm),
        "stubs_sha256": sha256_file(blob_path + ".stubs"),
        "raw_sha256": sha256_bytes(blob),
    }


def compile_corpus(out_dir):
    """逐份编译全部语料，产物落 out_dir。返回 (记录列表, 用时秒)。

    `status` 是 `ok` / `fail:rc=<n>` / `fail:no-blob` —— 失败也是基线的一部分：重构让
    「报错的那些」变成出产物，是**有意变更**，判据要报出来让人判，不是静默放过。
    """
    ensure_dir(out_dir)
    records = []
    started = time.perf_counter()
    for cid, rel_file in read_corpus_manifest():
        src = os.path.join(CORPUS, rel_file.replace("/", os.sep))
        out_path = os.path.join(out_dir, f"{cid}.bin")
        for suffix in ("", ".stubs", ".entry"):
            remove_stale(out_path + suffix)
        proc = run_cli(src, out_path)
        rec = {"id": cid, "file": rel_file, "blob": None, "stderr_first": "",
               "status": "", "blob_len": "0", "entry": "", "stub_count": "0",
               "norm_sha256": "", "stubs_sha256": "", "raw_sha256": ""}
        if proc.returncode == 0 and os.path.exists(out_path):
            rec["status"] = "ok"
            rec["blob"] = out_path
            rec.update(blob_fields(out_path))
        elif proc.returncode == 0:
            rec["status"] = "fail:no-blob"      # rc=0 却没产物：CLI 自己没写出来
        else:
            rec["status"] = f"fail:rc={proc.returncode}"
            rec["stderr_first"] = first_line(proc.stderr)
        records.append(rec)
    return records, round(time.perf_counter() - started, 2)


# ---- 清单 ---------------------------------------------------------------
def write_baseline(path, records):
    lines = [
        "# 产物基线回归：每份语料的装配产物指纹。判据入口 simplexc/tools/blob_baseline.py --verify",
        "# 判等比较 6 列：status blob_len entry stub_count norm_sha256 stubs_sha256",
        "# raw_sha256 只记不比 —— 它含 kernel32 绝对地址，跨 boot 就变；比较规则见"
        " simplexc/tools/baseline/README.md",
        "\t".join(("id",) + RECORD_FIELDS),
    ]
    for rec in records:
        lines.append("\t".join([rec["id"]] + [str(rec[k]) for k in RECORD_FIELDS]))
    ensure_dir(os.path.dirname(os.path.abspath(path)))
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    except OSError as e:
        raise BaselineError(f"写不了基线清单 {rel(path)}：{e}") from e


def read_baseline(path):
    """基线清单 → {id: {字段: 值}}（跳过 `#` 注释行）。"""
    rows = {}
    cols = []
    for line in read_text(path).splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if parts[0] == "id":
            cols = parts
            continue
        if not cols:
            raise BaselineError(f"基线清单 {rel(path)} 缺表头（第一条数据前应有 id 开头的行）")
        rows[parts[0]] = dict(zip(cols, parts, strict=False))   # 少列的那几行照旧只取有的值
    if not rows:
        raise BaselineError(f"基线清单 {rel(path)} 里一行数据都没有")
    return rows


def find_baseline(dirpath, explicit):
    """`--manifest` 优先；否则取目录里唯一那份 `corpus-baseline_*.tsv`。"""
    if explicit:
        return explicit
    found = sorted(glob.glob(os.path.join(dirpath, BASELINE_GLOB)))
    if not found:
        raise BaselineError(f"{rel(dirpath)} 里没有基线清单（{BASELINE_GLOB}）—— "
                            f"先跑 python simplexc/tools/blob_baseline.py --record {rel(dirpath)}")
    if len(found) > 1:
        raise BaselineError(f"{rel(dirpath)} 里有 {len(found)} 份基线清单："
                            + "、".join(rel(p) for p in found) + " —— 用 --manifest 指定一份")
    return found[0]


# ---- 判据 ---------------------------------------------------------------
def baseline_row(tsv_row, base_blob):
    """这一份的「期望值」：清单为准，产物目录在时**以产物为准**。

    只看清单会让「基线产物被改了一个字节」看不见（反向验证就白过），也发现不了「产物目录与
    清单不是同一次 record」。两者不一致时一并报出来：那是基线自己坏了，得重新 record。
    返回 (期望值, [(字段, 清单值, 产物值)])。
    """
    if not os.path.exists(base_blob):
        return tsv_row, []                   # 只有清单：照样能判，只是报不出字节区间
    derived = blob_fields(base_blob)
    # 只看 `CMP_FIELDS`：`raw_sha256` 天生跨 boot 就变，拿它判「产物目录与清单不符」会每次都报
    mismatch = [(k, str(tsv_row.get(k, "-")), derived[k]) for k in CMP_FIELDS
                if k in derived and str(tsv_row.get(k, "")) != derived[k]]
    want = dict(tsv_row)
    want.update(derived)
    return want, mismatch


def diff_one(rec, want, bad_fields, base_blob, mismatch):
    """一份差异的全部现场：哪个文件、哪几个字段、期望 vs 实际、首个字节区间。"""
    item = {"id": rec["id"], "file": rec["file"], "stderr": rec["stderr_first"],
            "fields": [(k, str(want.get(k, "-")), str(rec[k])) for k in bad_fields],
            "mismatch": mismatch, "bytes": None}
    if rec["status"] != "ok" or not rec["blob"]:
        return item
    if not os.path.exists(base_blob):
        return item                          # 基线没存这份产物：只能报字段差
    if {"norm_sha256", "blob_len"} & set(bad_fields) or mismatch:
        item["bytes"] = byte_diff(base_blob, rec["blob"])
    return item


def verify(baseline_path, blobs_dir, workdir):
    """重编全部语料并与基线（清单 + 产物目录）逐份比。返回报告 dict。"""
    base = read_baseline(baseline_path)
    records, secs = compile_corpus(workdir)
    diffs, new, same = [], [], 0
    for rec in records:
        tsv_row = base.get(rec["id"])
        if tsv_row is None:
            new.append(rec["id"])
            continue
        base_blob = os.path.join(blobs_dir, f"{rec['id']}.bin")
        want, mismatch = baseline_row(tsv_row, base_blob)
        bad = [k for k in CMP_FIELDS if str(rec[k]) != str(want.get(k, ""))]
        if not bad and not mismatch:
            same += 1
            continue
        diffs.append(diff_one(rec, want, bad, base_blob, mismatch))
    seen = {rec["id"] for rec in records}
    missing = [cid for cid in base if cid not in seen]
    return {"baseline": baseline_path, "blobs": blobs_dir, "count": len(records),
            "same": same, "diffs": diffs, "new": new, "missing": missing, "seconds": secs}


def print_diff(item):
    if item["fields"]:
        print(f"[{item['id']} {item['file']}] 字段不同："
              + "、".join(field for field, _want, _got in item["fields"]))
        for field, want, got in item["fields"]:
            print(f"  {field:<13} 基线 {want}   本轮 {got}")
    if item["mismatch"]:
        print(f"[{item['id']} {item['file']}] 基线产物与清单不符"
              "（产物目录过期或被改过，需重新 record）：")
        for field, tsv_value, blob_value in item["mismatch"]:
            print(f"  {field:<13} 清单 {tsv_value}   产物 {blob_value}")
    if item["bytes"] and item["bytes"][1]:
        count, spans = item["bytes"]
        shown = "、".join(spans[:4]) + ("…" if len(spans) > 4 else "")
        print(f"  字节差区间     首个 {spans[0]}；归一化后共 {count} 处（{shown}）")
    elif item["bytes"] is not None:
        print("  字节差区间     长度不同，无从报偏移（看上面的 blob_len）")
    if item["stderr"]:
        print(f"  本轮 stderr    {item['stderr']}")


def main(argv=None):
    utf8_stdout()
    ap = argparse.ArgumentParser(
        description="产物基线回归：98 份语料的装配产物在重构前后必须等价")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--record", metavar="DIR",
                       help="跑全部语料，清单写进 DIR（corpus-baseline_<日期>.tsv），产物落 --blobs")
    group.add_argument("--verify", metavar="DIR", help="重跑并与 DIR 里的基线清单逐份比")
    ap.add_argument("--blobs", default=None,
                    help="产物目录（默认 work/tmp/baseline-blobs/，体积大不进 git）")
    ap.add_argument("--manifest", default=None,
                    help="基线清单文件（默认 <DIR>/corpus-baseline_<日期>.tsv；verify 取 DIR 里唯一那份）")
    ap.add_argument("--workdir", default=DEFAULT_WORKDIR,
                    help="--verify 重编产物的落地目录（默认 work/tmp/blob_baseline/run/）")
    args = ap.parse_args(argv)
    blobs = args.blobs or DEFAULT_BLOBS

    try:
        if args.record:
            path = args.manifest or os.path.join(
                args.record, f"corpus-baseline_{datetime.date.today().isoformat()}.tsv")
            records, secs = compile_corpus(blobs)
            write_baseline(path, records)
            produced = sum(1 for rec in records if rec["status"] == "ok")
            print(f"产物基线已落盘：清单 {rel(path)}（{len(records)} 行）· "
                  f"产物 {rel(blobs)}/（{produced} 份 blob + sidecar）")
            print(f"出产物 {produced} 份 · 报错 {len(records) - produced} 份 · 用时 {secs}s")
            return 0

        baseline = find_baseline(args.verify, args.manifest)
        if not os.path.isdir(blobs):
            raise BaselineError(f"缺产物目录 {rel(blobs)}/ —— 它与清单是一对，"
                                "跑 --record 时一起生成（只有清单也能判等，但没有字节区间）")
        report = verify(baseline, blobs, args.workdir)
    except BaselineError as e:
        print(f"FAIL {e}", file=sys.stderr)
        return 2

    print(f"产物基线回归 · 基线 {rel(report['baseline'])} · 产物 {rel(report['blobs'])}/")
    for item in report["diffs"]:
        print_diff(item)
    if report["new"]:
        print(f"基线外新增（清单里没有）：{'、'.join(report['new'])}")
    if report["missing"]:
        print(f"本轮缺失（清单里有、这次没编）：{'、'.join(report['missing'])}")
    bad = report["diffs"] or report["new"] or report["missing"]
    print(f"一致 {report['same']} 份 · 差异 {len(report['diffs'])} 份 · "
          f"基线外新增 {len(report['new'])} 份 · 本轮缺失 {len(report['missing'])} 份 · "
          f"重编 {report['count']} 份用时 {report['seconds']}s")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
