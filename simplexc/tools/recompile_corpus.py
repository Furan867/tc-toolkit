#!python3
"""M0 语料固化：把 work/hook/all 的 180 份捕获去重成 98 份唯一源码，离线重编译出含 entry 的 manifest。

为什么要离线重编译：转发层每次加载都会清空 work/hook/，180 组捕获只剩「源码, 产物」两两配对，
manifest（现存那份只有一次会话的 9 行）与 `NNN.entry.txt` 都已丢，entry 一个都没留下。
离线编译同一 boot 内确定（同进程重复调用逐字节相同），所以可以用它把 entry 补回来。

`ref_match` 为 `no` 是**期望内**的：参考产物里烘焙着捕获得那次编译进程的 compile_real.dll
加载基址，基址随 boot 变（boot 内跨进程恒定），跨 boot 不可能逐字节相等。所以另有两列
`ref_match_rebased` / `rebased_delta`：把基址差值归一化后是否逐字节相同，判据见
`simplexc/tools/classify_abs_addrs.py`。

用法（解释器用 PATH 上的 Python 3.13；子进程一律 sys.executable，不写解释器路径）：
    python simplexc/tools/recompile_corpus.py            # 全流程：去重落盘 → 拷贝 campaign → 抽 stdlib → 逐份编译
    python simplexc/tools/recompile_corpus.py --check     # 只校验 manifest 与 corpus/ 的一致性，不编译
    python simplexc/tools/recompile_corpus.py --expect OTHER.si   # 换 stdlib 比对副本

⚠ compile.dll 遇编译错误会 abort 整个进程（Nim sysFatal），所以：
  - 每编译完一份就 append + flush + fsync 一行 manifest，重跑时按 src_sha256 跳过已有行；
  - 进每一份之前先打印「正在编译谁、前一份是谁」，abort 后从输出最后一行就能定位现场。

⚠ 配对规则：hook_src_NNN.si ↔ hook_out_NNN.bin（不是同名）。
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import ctypes
import glob
import hashlib
import importlib.util
import io
import os
import struct
import sys
import time

import classify_abs_addrs as C  # 同目录：movabs/PE/差异分析的唯一定义处

HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)
REPO = os.path.dirname(SIMPLEXC)                      # tc-toolkit/


def _load_by_path(path, name):
    """按文件路径加载模块（tools/ 不是包，静态 import 解析不到）。"""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


T = _load_by_path(os.path.join(REPO, "tools", "try_compile_dll.py"), "try_compile_dll")

if REPO not in sys.path:
    sys.path.insert(0, REPO)
from libs.tc_common import GAME_REAL_DLL_NAME, game_dir  # noqa: E402

CAPTURE_DIR = os.path.join(REPO, "work", "hook", "all")
CORPUS = os.path.join(SIMPLEXC, "corpus")
SRC_DIR = os.path.join(CORPUS, "src")
REF_DIR = os.path.join(CORPUS, "ref")
CAMPAIGN_DIR = os.path.join(CORPUS, "campaign")
MANIFEST = os.path.join(CORPUS, "manifest.tsv")

GAME = str(game_dir())
GAME_CAMPAIGN = os.path.join(GAME, "campaign")
GAME_DLL_REAL = os.path.join(GAME, GAME_REAL_DLL_NAME)

# stdlib 固定前缀（口径见 docs/specs/2026-10-15-simplex-llvm-compiler-design.md §2.6）：
# DLL 里那段是 27711 字节、含 55 处 CRLF；工作副本做 CRLF→LF 归一化后是 27656 字节。
STDLIB_OFFSET = 0x119F48
STDLIB_LEN = 27711
STDLIB_CRLF = 55
STDLIB_LF_LEN = 27656
STDLIB_EXPECT = r"C:\Users\Furan\AppData\Local\Temp\pi\tc-si\stdlib_prefix.si"

COLUMNS = ["id", "file", "src_len", "src_sha256", "dups", "seen",
           "flags", "entry", "out_len", "out_sha256", "ref_match",
           "ref_match_rebased", "rebased_delta"]
FLAGS = 0
IDENTICAL = "逐字节相同"


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def read(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


# ---------------------------------------------------------------- 捕获 → 唯一源码

def unique_sources():
    """按源码字节去重 CAPTURE_DIR 的捕获（等价于按源码 md5 去重，见 notes 里的说明）。

    返回 {id: {"src": bytes, "sha256": str, "seen": [捕获编号...]}}，id = 该源码的最小捕获编号。
    """
    groups = collections.defaultdict(list)
    for name in sorted(os.listdir(CAPTURE_DIR)):
        if name.startswith("hook_src_") and name.endswith(".si"):
            num = name[len("hook_src_"):-len(".si")]
            groups[read(os.path.join(CAPTURE_DIR, name))].append(num)
    out = {}
    for nums in groups.values():
        nums = sorted(nums)
        src = read(os.path.join(CAPTURE_DIR, f"hook_src_{nums[0]}.si"))
        out[nums[0]] = {"src": src, "sha256": sha256(src), "seen": nums}
    return collections.OrderedDict(sorted(out.items()))


def write_sources(uniq):
    """按捕获原字节落盘 corpus/src/<id>.si；返回目录里多出来的（不属于本次去重结果）文件名。"""
    os.makedirs(SRC_DIR, exist_ok=True)
    stale = set(os.listdir(SRC_DIR))
    for cid, entry in uniq.items():
        path = os.path.join(SRC_DIR, f"{cid}.si")
        if os.path.exists(path):
            if read(path) != entry["src"]:
                raise SystemExit(f"{path} 已存在但内容与捕获不一致，停下人工确认")
        else:
            with open(path, "wb") as f:
                f.write(entry["src"])
        stale.discard(f"{cid}.si")
    return sorted(stale)


# ---------------------------------------------------------------- 编译一份

def compile_one(dll, src: bytes):
    """调一次 compile()，返回 (产物字节, entry, out_len)；失败（err_len != 0）抛 RuntimeError。"""
    with contextlib.redirect_stdout(io.StringIO()):      # 屏蔽 try_compile 的过程提示
        raw = T.try_compile(dll, src)
    out_len, out_ptr, entry, err_len, err_ptr = struct.unpack_from("<5Q", raw, 0)
    if err_len:
        msg = ctypes.string_at(err_ptr, min(err_len, 4096)).decode("utf-8", "replace")
        raise RuntimeError(f"compile 报错（err_len={err_len}）：{msg}")
    if not out_len or not out_ptr:
        raise RuntimeError(f"compile 无产物且无错误：out_len={out_len} out_ptr={out_ptr}")
    return ctypes.string_at(out_ptr, out_len), entry, out_len


def verdict(res, note):
    """把 classify_abs_addrs.pair_analysis 的结果翻成 manifest 的三列。

    `ref_match` 是原始字节比较；`ref_match_rebased` / `rebased_delta` 只认「差异全部落在
    image 空间 movabs 立即数 + 差值恒定 + 归一化后逐字节相同」这一条判据，任一条不成立就是 no。
    """
    if note == IDENTICAL:
        return "yes", "yes", "-"
    ok = (not res["unclassified"]) and res["delta"] is not None and res["rebased_equal"]
    return "no", ("yes" if ok else "no"), (hex(res["delta"]) if res["delta"] is not None else "-")


# ---------------------------------------------------------------- manifest

def read_manifest():
    """读已有 manifest，返回 {id: row_dict}。表头不匹配就停下（可能被人改过）。"""
    rows = {}
    if not os.path.exists(MANIFEST):
        return rows
    with open(MANIFEST, encoding="utf-8", newline="") as f:
        lines = f.read().splitlines()
    if not lines:
        return rows
    if lines[0].split("\t") != COLUMNS:
        raise SystemExit(f"manifest 表头不是预期列，停下人工确认：{lines[0]!r}")
    for ln in lines[1:]:
        if not ln.strip():
            continue
        parts = ln.split("\t")
        if len(parts) != len(COLUMNS):
            raise SystemExit(f"manifest 行列数不对：{ln!r}")
        rows[parts[0]] = dict(zip(COLUMNS, parts, strict=True))
    return rows


def make_row(cid, ctx, entry, out_len, out_sha, ref_match, rebased, delta):
    """按 COLUMNS 的顺序拼一行 manifest（制表符分隔）。"""
    return "\t".join([
        cid,
        f"src/{cid}.si",
        str(ctx["src_len"]),
        ctx["src_sha256"],
        str(ctx["dups"]),
        ctx["seen"],
        hex(FLAGS),
        hex(entry),
        str(out_len),
        out_sha,
        ref_match,
        rebased,
        delta,
    ])


# ---------------------------------------------------------------- 三个阶段

def copy_campaign():
    levels = sorted(os.path.basename(os.path.dirname(p))
                    for p in glob.glob(os.path.join(GAME_CAMPAIGN, "*", "test.si")))
    os.makedirs(CAMPAIGN_DIR, exist_ok=True)
    total = 0
    for level in levels:
        data = read(os.path.join(GAME_CAMPAIGN, level, "test.si"))
        dst = os.path.join(CAMPAIGN_DIR, level, "test.si")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst) and read(dst) != data:
            raise SystemExit(f"{dst} 已存在但内容与游戏目录不一致，停下人工确认")
        with open(dst, "wb") as f:
            f.write(data)
        total += len(data)
    return len(levels), total


def extract_stdlib(expect_path):
    """从 compile_real.dll 抽固定前缀并做 CRLF→LF 归一化，与既有副本逐字节比对。"""
    raw = read(GAME_DLL_REAL)[STDLIB_OFFSET:STDLIB_OFFSET + STDLIB_LEN]
    if len(raw) != STDLIB_LEN:
        raise SystemExit(f"从 {GAME_DLL_REAL} 偏移 {STDLIB_OFFSET:#x} 只取到 {len(raw)} 字节")
    crlf = raw.count(b"\r\n")
    lf = raw.replace(b"\r\n", b"\n")
    if crlf != STDLIB_CRLF or len(lf) != STDLIB_LF_LEN:
        raise SystemExit(f"stdlib 口径不符：CRLF={crlf}（期望 {STDLIB_CRLF}）、"
                         f"归一化后 {len(lf)} B（期望 {STDLIB_LF_LEN}）")
    with open(os.path.join(CORPUS, "stdlib_prefix.si"), "wb") as f:
        f.write(lf)
    if not os.path.exists(expect_path):
        raise SystemExit(f"找不到比对副本 {expect_path}（--expect 换一个）")
    if read(expect_path) != lf:
        raise SystemExit(f"归一化后的前缀与 {expect_path} 不一致")
    return len(raw), crlf, len(lf), sha256(lf)


def cmd_run(args):
    t0 = time.time()
    uniq = unique_sources()
    print(f"捕获目录 {CAPTURE_DIR}：{sum(len(e['seen']) for e in uniq.values())} 份 → "
          f"{len(uniq)} 份唯一源码")
    stale = write_sources(uniq)
    if stale:
        print(f"  ⚠ corpus/src/ 里有 {len(stale)} 个不属于本次去重结果的文件（未删）：{stale[:5]}")
    print(f"  源码落盘 → {SRC_DIR}")

    n, nbytes = copy_campaign()
    print(f"campaign 拷贝：{n} 份 / {nbytes} B → {CAMPAIGN_DIR}（本轮不编译）")

    length, crlf, lf_len, std_sha = extract_stdlib(args.expect)
    print(f"stdlib 前缀：DLL {STDLIB_OFFSET:#x} 起 {length} B（{crlf} 处 CRLF）→ 归一化 {lf_len} B"
          f"  sha256={std_sha[:16]}  与 {os.path.basename(args.expect)} 逐字节相同"
          f" → {os.path.join(CORPUS, 'stdlib_prefix.si')}")

    os.makedirs(REF_DIR, exist_ok=True)
    rows = read_manifest()
    if rows:
        print(f"manifest 已有 {len(rows)} 行，按 src_sha256 校验并跳过")

    dll = C.load_compiler()
    pe = C.read_pe(GAME_DLL_REAL)
    ctx = {cid: {"src_len": len(e["src"]), "src_sha256": e["sha256"],
                 "dups": len(e["seen"]), "seen": ",".join(e["seen"])}
           for cid, e in uniq.items()}

    yes = no = rebased_no = skipped = sites_total = unclassified_total = 0
    with open(MANIFEST, "a", encoding="utf-8", newline="") as mf:
        if not rows:
            mf.write("\t".join(COLUMNS) + "\n")
            mf.flush()
        prev = "-"
        for cid, source in uniq.items():
            if cid in rows:
                if rows[cid]["src_sha256"] != source["sha256"]:
                    raise SystemExit(f"{cid} 的源码与 manifest 里记的不是同一份，停下人工确认")
                skipped += 1
                prev = cid
                continue
            print(f"编译 {cid}（前一份 {prev}，src_len={len(source['src'])}）...", flush=True)
            out, entry, out_len = compile_one(dll, source["src"])
            ours_path = os.path.join(REF_DIR, f"{cid}.bin")
            with open(ours_path, "wb") as f:
                f.write(out)
            ref_path = os.path.join(CAPTURE_DIR, f"hook_out_{cid}.bin")
            res, note = C.pair_analysis(ours_path, ref_path, pe)
            if res is None:
                raise SystemExit(f"{cid}: 参考产物对不上（{note}），停下")
            ref_match, rebased, delta = verdict(res, note)
            mf.write(make_row(cid, ctx[cid], entry, out_len, sha256(out),
                              ref_match, rebased, delta) + "\n")
            mf.flush()
            os.fsync(mf.fileno())
            yes += ref_match == "yes"
            no += ref_match == "no"
            rebased_no += rebased == "no"
            sites_total += len(res["sites"])
            unclassified_total += len(res["unclassified"])
            print(f"  out_len={out_len} entry={hex(entry)} ref_match={ref_match}"
                  f" ref_match_rebased={rebased} delta={delta} 站点={len(res['sites'])}"
                  f" 未分类={len(res['unclassified'])}", flush=True)
            if rebased == "no":
                raise SystemExit(f"{cid}: 归一化后仍与参考产物不同（未分类差异 "
                                 f"{res['unclassified'][:3]}），停下报主程")
            prev = cid

    print(f"\n编译完成：ref_match yes={yes} no={no}，ref_match_rebased=no 共 {rebased_no}"
          f"（跳过已有 {skipped} 行），耗时 {time.time() - t0:.1f}s")
    print(f"差异站点合计 {sites_total} 处，未分类差异合计 {unclassified_total} 处")
    top = sorted(uniq.items(), key=lambda kv: -len(kv[1]["seen"]))[:3]
    print("重复最多的源码：" + "、".join(f"{cid}({len(e['seen'])} 次)" for cid, e in top))
    return 0


def cmd_check(args):
    """不重编译，只校验 manifest ↔ corpus/src ↔ corpus/ref ↔ 捕获 的一致性。

    其中「同源检查」是本次改动的核心三条断言：差异必须全部落在 image 空间的
    `movabs r64,imm64` 立即数内（未分类 = 0）、差值必须恒定、归一化后必须逐字节相同。
    """
    bad = []
    if not os.path.exists(MANIFEST):
        print("manifest 不存在", file=sys.stderr)
        return 1
    with open(MANIFEST, encoding="utf-8", newline="") as f:
        lines = f.read().splitlines()
    if lines[0].split("\t") != COLUMNS:
        bad.append(f"表头不对：{lines[0]!r}")
    rows = [dict(zip(COLUMNS, ln.split("\t"), strict=True)) for ln in lines[1:] if ln.strip()]
    uniq = unique_sources()
    if len(rows) != len(uniq):
        bad.append(f"数据行 {len(rows)} != 唯一源码 {len(uniq)}")
    if [r["id"] for r in rows] != sorted(r["id"] for r in rows):
        bad.append("id 不是升序")
    if len({r["id"] for r in rows}) != len(rows):
        bad.append("id 有重复")
    pe = C.read_pe(GAME_DLL_REAL)
    sites_total = unclassified_total = rebased_yes = 0
    deltas = set()
    for r in rows:
        cid = r["id"]
        entry = uniq.get(cid)
        if entry is None:
            bad.append(f"{cid}: 不在捕获去重结果里")
            continue
        if r["file"] != f"src/{cid}.si":
            bad.append(f"{cid}: file 列 {r['file']!r} 不是 src/{cid}.si")
        src_path = os.path.join(SRC_DIR, f"{cid}.si")
        if not os.path.exists(src_path):
            bad.append(f"{cid}: 缺 {src_path}")
        elif read(src_path) != entry["src"]:
            bad.append(f"{cid}: 源码与捕获不一致")
        if int(r["src_len"]) != len(entry["src"]) or r["src_sha256"] != entry["sha256"]:
            bad.append(f"{cid}: src_len/src_sha256 与捕获不符")
        if int(r["dups"]) != len(entry["seen"]) or r["seen"] != ",".join(entry["seen"]):
            bad.append(f"{cid}: dups/seen 与捕获不符")
        if r["flags"] != hex(FLAGS):
            bad.append(f"{cid}: flags={r['flags']!r}")
        try:
            ent, out_len = int(r["entry"], 16), int(r["out_len"])
        except ValueError:
            bad.append(f"{cid}: entry/out_len 不是数字：{r['entry']!r} {r['out_len']!r}")
            continue
        if not 0 < ent < out_len:
            bad.append(f"{cid}: entry={ent:#x} 不在 (0, out_len={out_len}) 内")
        ref_path = os.path.join(REF_DIR, f"{cid}.bin")
        if not os.path.exists(ref_path):
            bad.append(f"{cid}: 缺 {ref_path}")
            continue
        prod = read(ref_path)
        if len(prod) != out_len or sha256(prod) != r["out_sha256"]:
            bad.append(f"{cid}: ref/{cid}.bin 与 out_len/out_sha256 不符（{len(prod)} B）")
        captured = os.path.join(CAPTURE_DIR, f"hook_out_{cid}.bin")
        if not os.path.exists(captured):
            bad.append(f"{cid}: 缺参考产物 {captured}")
            continue
        res, note = C.pair_analysis(ref_path, captured, pe)
        if res is None:
            bad.append(f"{cid}: 参考产物对不上（{note}）")
            continue
        ref_match, rebased, delta = verdict(res, note)
        sites_total += len(res["sites"])
        unclassified_total += len(res["unclassified"])
        rebased_yes += rebased == "yes"
        for d in res["deltas"]:
            deltas.add(d)
        if (ref_match, rebased, delta) != (r["ref_match"], r["ref_match_rebased"], r["rebased_delta"]):
            bad.append(f"{cid}: manifest 记的是 ({r['ref_match']}, {r['ref_match_rebased']}, "
                       f"{r['rebased_delta']})，实测是 ({ref_match}, {rebased}, {delta})")
        if note != IDENTICAL:
            if res["unclassified"]:
                bad.append(f"{cid}: 有 {len(res['unclassified'])} 处差异不在 image 空间 "
                           f"movabs 立即数内：{res['unclassified'][:3]}")
            if len(res["deltas"]) != 1:
                bad.append(f"{cid}: 差值不恒定：{[hex(d) for d in res['deltas']]}")
            if not res["rebased_equal"]:
                bad.append(f"{cid}: 归一化后仍与参考产物不同")
    print(f"manifest 数据行={len(rows)} 列={COLUMNS}")
    print(f"同源检查：差异站点 {sites_total} 处，未分类差异 {unclassified_total} 处，"
          f"恒定差值 {'、'.join(hex(d) for d in sorted(deltas)) or '-'}，"
          f"归一化后逐字节相同 {rebased_yes}/{len(rows)} 份")
    if bad:
        print(f"发现 {len(bad)} 处问题：")
        for b in bad[:30]:
            print("  " + b)
        return 1
    print("校验通过：manifest、corpus/src、corpus/ref 与捕获三者一致，同源检查三条断言全过")
    return 0


def main():
    ap = argparse.ArgumentParser(description="M0 语料固化：去重 + 离线重编译补 entry")
    ap.add_argument("--check", action="store_true", help="只校验，不编译")
    ap.add_argument("--expect", default=STDLIB_EXPECT,
                    help="stdlib 前缀比对副本（默认用户临时目录那份）")
    args = ap.parse_args()
    return cmd_check(args) if args.check else cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())
