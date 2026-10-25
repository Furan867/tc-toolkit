#!python3
"""M5 S3：98 份语料的并行差分驱动 —— 一份语料一行，回答「装得出来吗 / 终态等价吗」。

每份做四步（后三步只在装配成功后才做）：

  1. **装配**：`build/simplexc_cli.exe --src <si> --out <blob>`（产物旁会有 `.entry` / `.stubs`）。
     失败记 `cli_fail` + 原始错误首行；超过 `--cli-timeout` 记 `cli_fail(timeout)`，不重试。
  2. **参考侧跑**：`difftrace run --bin corpus/ref/<id>.bin --save <ref.seq>`。
  3. **我们侧跑**：`difftrace run --bin <blob> --save <ours.seq>`（entry 由 `<blob>.entry` 自动定）。
  4. **比终态**：`difftrace diff --state <ref.seq> <ours.seq>` —— 退出码 0=equal / 1=diff / 2=错误。

判据口径（2026-10-25 修订）：verdict 取**带 `--src`** 的那次，即只比 `[game_lo, game_hi)`。

原口径取不带 `--src` 的 `diff_bytes_raw`，理由是「`--src` 会退化成平凡的 equal」——那时
`36470ce` 还没修 `--src`，它按 region 命中过滤，而这一批语料的 `.si` 里没有 `memory_copy`、
`Spec::regions()` 只剩退化成单点 `(a,a)` 的标量，于是留下 0/0（实测 001 当时放弃 A 719797 条）。
修完之后 `--src` 按 `[game_lo, game_hi)` 过滤，001 是 kept 26550/26550，空过滤一律 rc=2。

不带 `--src` 会把**代码区字节**（每份产物自己的镜像）与**栈帧布局**也算进判据，这两样每份都
不同，于是「产物变一点就判 diff」。实测 98 份：50 份差异只在 stack/code、41 份只差在睿主
分配器页（`0x4009` / `0x400B` / `0x7FF9`），只有 5 份含真正游戏内存（`0x232…`）的差异。
`diff_bytes_raw` 仍记进 TSV 当参考列。

`first_diff_tick` 恒为 `n/a`：逐 tick 要 `--tick-mark`（每 tick 写一次的地址，98 份各不相同），
按上面的口径不传，difftrace 只报终态。差异的定位信息仍给：`first_diff_addr` + 按地址段分的字节数
（`diff_stack` / `diff_code` / `diff_game`；段界与 `game` 段的地址前缀统计都在
`difftrace diff --classify` 里，只那一处定义）。

判据、整份总数、按段分账都来自**同一次** `difftrace diff --classify`（一遍读盘）。三趟分开时
10000 tick 下光读两条序列就要 3 × 5 s（001 的参考序列 2.0 GB）。

并发：`--jobs` 走 `ProcessPoolExecutor`，每份一个子进程；子进程里顺序起三个 exe（`uc_engine` 每进程一个、
无共享状态）。main 进程只汇总与落盘，**每完成一份就重写 TSV**（中断可续跑，`--resume` 跳过已完成的）。
`.seq` 是逐条 store 的原始序列（100 tick 的语料约 20 MB/侧），`equal` 的行跑完就删，`diff` / `host_fail`
的行留着给 `difftrace where` 定位。

    python simplexc/tools/m5/batch.py --limit 3 --jobs 1     # 验脚本
    python simplexc/tools/m5/batch.py --jobs 12              # 全量 98 份
    python simplexc/tools/m5/batch.py --jobs 12 --resume     # 中断后接着跑
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from typing import NoReturn

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)                                 # simplexc/tools/
SIMPLEXC = os.path.dirname(TOOLS)                             # simplexc/
ROOT = os.path.dirname(SIMPLEXC)                              # reversing/

CLI = os.path.join(ROOT, "build", "simplexc_cli.exe")
DIFFTRACE = os.path.join(ROOT, "build", "difftrace.exe")
MANIFEST = os.path.join(SIMPLEXC, "corpus", "manifest.tsv")
REF_DIR = os.path.join(SIMPLEXC, "corpus", "ref")

DEFAULT_OUT_DIR = os.path.join(ROOT, "work", "tmp", "m5batch")
MAX_JOBS = 12                     # difftrace README 的实测：16 路只比 12 路快 7%，多占 1.75 GB

# M5 stdlib：拼在每份语料之前一起编译的固定前缀（从 compile_real.dll 原样提取，见
# recompile_corpus.py 的 extract_stdlib）。只有 `--with-prefix` 时才读。
PREFIX_PATH = os.path.join(SIMPLEXC, "corpus", "stdlib_prefix.si")

# `run` 一次要装配 291 MB guest 映射 + 灌 142 MB `RESET_DATA`，100 tick 的语料约 0.5 s；
# 跑到这个数就是卡住了（不是慢），按 host_fail 记，别把整轮拖死。
RUN_TIMEOUT = 900.0
DIFF_TIMEOUT = 900.0

STATUS_EQUAL = "equal"
STATUS_DIFF = "diff"
STATUS_CLI_FAIL = "cli_fail"
STATUS_HOST_FAIL = "host_fail"
# `--with-prefix` 时只有一种失败跟它有关：拼前缀本身写不出去（记 cli_fail + err_kind=prefix_join）。
STATUS_PREFIX_OK = "prefix_ok"

# 地址段的段界不在本脚本里定义 —— 它在 `difftrace/main.cc` 的 `kSegBounds`（栈 = driver.cc 的
# kStackBase/kStackSize，代码 = image.hh 的 kCodeBase 起 16 MB）。本脚本只拿 `--classify` 的
# 三个数；段界抄两份就会出现「同一份语料两个分类」。

TSV_COLUMNS = ["id", "status", "ticks", "seed", "cli_rc", "cli_ms", "blob_bytes", "entry",
               "stores_ref", "stores_ours", "diff_bytes_raw", "diff_stack", "diff_code",
               "diff_game", "diff_game_prefixes", "first_diff_addr", "first_diff_tick",
               "src_kept_a", "src_kept_b", "err_kind", "err_first_line", "seconds",
               "run_ms", "diff_ms", "diff_load_ms", "diff_fold_ms"]

INT_COLUMNS = ("cli_rc", "blob_bytes", "ticks", "stores_ref", "stores_ours",
               "diff_bytes_raw", "diff_stack", "diff_code", "diff_game", "src_kept_a", "src_kept_b")
# `cli_ms` 写的时候就是四舍五入到 0.1 ms 的浮点（容不下就丢信息，而 int 会把整张表读崩）。
FLOAT_COLUMNS = ("cli_ms", "seconds", "run_ms", "diff_ms", "diff_load_ms", "diff_fold_ms")

Row = dict[str, object]


def fail(msg, rc) -> NoReturn:
    print(msg, file=sys.stderr)
    raise SystemExit(rc)


def rel(path):
    try:
        return os.path.relpath(path, ROOT).replace("\\", "/")
    except ValueError:
        return path


def one_line(text, limit=200):
    """错误文本压成一行（TSV 的单元格里不能有 tab / 换行），超长截断。"""
    flat = " ".join((text or "").split())
    return flat[:limit] + ("…" if len(flat) > limit else "")


def first_error_line(stdout, stderr):
    """失败原因首行：先 stderr（错误都在那儿），没有再退回 stdout。"""
    for blob in (stderr, stdout):
        for line in (blob or "").splitlines():
            if line.strip():
                return one_line(line)
    return ""


def load_manifest():
    """`corpus/manifest.tsv` → 目标列表（98 份唯一语料，顺序照表）。"""
    try:
        with open(MANIFEST, encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f if line.strip()]
    except OSError as e:
        fail(f"读不了语料清单 {rel(MANIFEST)}：{e}", 2)
    head = rows[0]
    try:
        ci = {name: head.index(name) for name in ("id", "file", "entry")}
    except ValueError as e:
        fail(f"{rel(MANIFEST)} 的表头缺列（要 id / file / entry）：{e}", 2)
    return [{"id": row[ci["id"]],
             "src": os.path.join(SIMPLEXC, "corpus", row[ci["file"]]),
             "ref": os.path.join(REF_DIR, row[ci["id"]] + ".bin")}
            for row in rows[1:]]


def run_cli(src, blob, timeout):
    """装配一次。返回 (rc, ms, stdout, stderr, timed_out)。"""
    for suffix in ("", ".entry", ".stubs"):        # 别让上一次的 sidecar 冒充这一次
        try:
            os.remove(blob + suffix)
        except OSError as e:
            if not isinstance(e, FileNotFoundError):
                return (None, 0.0, "", f"删不了上一轮留下的产物 {rel(blob + suffix)}：{e}", False)
    t0 = time.perf_counter()
    try:
        proc = subprocess.run([CLI, "--src", src, "--out", blob], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return (None, round((time.perf_counter() - t0) * 1000, 1), "",
                f"CLI 超时（{timeout:.0f} s）", True)
    except OSError as e:
        return (None, round((time.perf_counter() - t0) * 1000, 1), "", f"起不了 CLI：{e}", False)
    return (proc.returncode, round((time.perf_counter() - t0) * 1000, 1), proc.stdout,
            proc.stderr, False)


def run_diff(bin_path, src, ticks, seed, save):
    """`difftrace run --save`。返回 (payload, error_line)。payload 是这份的 JSON 结果。"""
    cmd = [DIFFTRACE, "run", "--src", src, "--bin", bin_path, "--target", str(ticks),
           "--seed", seed, "--save", save, "--json"]
    payload, error = call_json(cmd, RUN_TIMEOUT)
    return payload, error


def format_prefixes(rows):
    """`[{"prefix":562,"count":88}, …]` → `0x232:88 0x4009:34`（TSV 里的样子，与旧版一致）。"""
    return " ".join(f"0x{int(p['prefix']):X}:{int(p['count'])}" for p in rows)


def diff_report(path_a, path_b, src):
    """`difftrace diff --state --classify`：一次调用、一遍读盘，拿全部三组数。

    三组：判据（带 `--src` 的 `equal` / `diff_bytes`）、整份总数（不过滤的 `raw_diff_bytes`）、
    按地址段分账（`raw_stack` / `raw_code` / `raw_game` + game 段的地址前缀）。以前是三趟：
    判据一趟、总数一趟、Python 里再把两条序列读一遍折一遍做分类 —— 10000 tick 下 001 的两条
    序列是 2.0 GB / 230 MB，每趟都要重新读盘，光读盘就约 5 s。
    """
    cmd = [DIFFTRACE, "diff", "--state", path_a, path_b, "--json", "--classify"]
    if src:
        cmd += ["--src", src]
    payload, error = call_json(cmd, DIFF_TIMEOUT)
    if payload is None:
        return (None, error)
    # diff 的退出码 1 = 「算完了，结论是不等」，不是错误 —— 只有 `ok:false` 才是（`call_json` 已判）。
    return (payload, error)


def call_json(cmd, timeout):
    """跑一个 `--json` 子命令。返回 (payload, error_line)。失败时 payload 为 None。

    difftrace 的错误有两种形态：异常路径给 `{"ok":false,"error":"…"}` + stderr 一行；
    `run` 没跑完 target 时给完整 JSON 但 `ok:false`（没有 error 键）—— 两种都要留下原始行。
    """
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return (None, f"difftrace 超时（{timeout:.0f} s）：{' '.join(cmd[1:3])}")
    except OSError as e:
        return (None, f"起不了 difftrace：{e}")
    payload = None
    for line in (proc.stdout or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                payload = None
    if payload is None:
        return (None, first_error_line(proc.stdout, proc.stderr) or f"difftrace rc={proc.returncode}")
    if not payload.get("ok"):
        return (payload, payload.get("error") or first_error_line(proc.stdout, proc.stderr)
                or f"difftrace rc={proc.returncode}")
    return (payload, None)


# ---- 终态差异的诊断（只分地址段，不改判据） ------------------------------------

# ---- 终态差异的诊断（只分地址段，不改判据） ------------------------------------
# 分账全在 `difftrace diff --classify` 里算（段界与游戏前缀的排序规则也只有那一处定义）：
# 一次调用、读一次盘就同时给判据、整份总数与三段字节数。

def classify_mismatch(row):
    """防线：按段分类的合计必须等于整份差异总数。

    两边现在都来自 difftrace 同一次折叠 —— 它不再能发现「两边折叠语义不一致」，但仍然能发现
    分类本身漏账（漏一段、前缀统计错）。不回退成 equals：不对就是不对。
    """
    parts = (row.get("diff_stack"), row.get("diff_code"), row.get("diff_game"))
    raw = row.get("diff_bytes_raw")
    if raw == "" or any(p == "" for p in parts):
        return None
    total = sum(int(p) for p in parts)
    if total == int(raw):
        return None
    return (f"按段分类合计 {total} B ≠ difftrace diff_bytes "
            f"{raw} B —— 三段之和必须等于整份差异")


# ---- 一份语料 ----------------------------------------------------------------

def blank_row():
    """一整行空值（列集合只由 `TSV_COLUMNS` 定义，行里的取值全是标量）。"""
    row: Row = {}
    row.update(dict.fromkeys(TSV_COLUMNS, ""))
    return row


def ms_since(t0):
    return round((time.perf_counter() - t0) * 1000, 1)


def run_one(task):
    """跑完一份语料，返回它那一行（字段全是标量，好进 TSV / 跨进程传）。"""
    t0 = time.perf_counter()
    cid = task["id"]
    src, ref, blob = task["src"], task["ref"], task["blob"]
    seq_ref = os.path.join(task["seq_dir"], cid + ".ref.seq")
    seq_ours = os.path.join(task["seq_dir"], cid + ".ours.seq")
    row = blank_row()
    # 分项计时：`seconds` 只回答「整份多慢」，回答不了「哪一段慢」。这几列跟着行一起写出去，
    # 每份都有了就不用为了定位瓶颈去单跑一份。
    row.update({"id": cid, "ticks": task["ticks"], "seed": task["seed"],
                "first_diff_tick": "n/a", "status": "",
                "run_ms": "", "diff_ms": "", "diff_load_ms": "", "diff_fold_ms": ""})

    def done(status, **kw):
        row.update(kw)
        row["status"] = status
        row["seconds"] = round(time.perf_counter() - t0, 2)
        return row

    # 1) 装配
    if task["with_prefix"]:
        # M5 stdlib：前缀拼在语料前一起编。`corpus/ref/` 里的参考产物**本来就是含前缀的**
        # （`compile()` 内部自动加前缀：语料用了 `memory_clear`/`get_time` 而自己没定义），
        # 所以这个模式照常跑差分，不是「只能报能不能编过」。拼接结果落地在 blob 旁边，好复现。
        joined = blob + ".joined.si"
        try:
            with open(PREFIX_PATH, encoding="utf-8") as f:
                prefix = f.read()
            with open(src, encoding="utf-8") as f:
                body = f.read()
            with open(joined, "w", encoding="utf-8", newline="\n") as f:
                f.write(prefix + "\n" + body)
        except OSError as e:
            return done(STATUS_CLI_FAIL, err_kind="prefix_join",
                        err_first_line=one_line(f"拼前缀失败：{e}"))
        src = joined
    rc, cli_ms, out, err, timed_out = run_cli(src, blob, task["cli_timeout"])
    row.update({"cli_rc": "" if rc is None else rc, "cli_ms": cli_ms})
    if timed_out:
        return done(STATUS_CLI_FAIL, err_kind="cli_timeout",
                    err_first_line=one_line(f"CLI 超时（{task['cli_timeout']:.0f} s）"))
    if rc != 0 or not os.path.exists(blob):
        return done(STATUS_CLI_FAIL, err_kind=f"rc={rc}" if rc else "no_blob",
                    err_first_line=first_error_line(out, err))
    try:
        row["blob_bytes"] = os.path.getsize(blob)
        with open(blob + ".entry", encoding="ascii") as f:
            row["entry"] = "0x" + f.read().strip().upper()
    except OSError as e:
        return done(STATUS_CLI_FAIL, err_kind="entry_missing",
                    err_first_line=one_line(f"读不了 {rel(blob)}.entry：{e}"))

    # 2/3) 两侧各跑一遍
    run_ms = 0.0
    for side, bin_path, seq in (("run_ref", ref, seq_ref), ("run_ours", blob, seq_ours)):
        if os.path.exists(seq):
            try:
                os.remove(seq)
            except OSError as e:
                return done(STATUS_HOST_FAIL, err_kind=side,
                            err_first_line=one_line(f"删不了上一轮的序列 {rel(seq)}：{e}"))
        if side == "run_ref" and not os.path.exists(bin_path):
            return done(STATUS_HOST_FAIL, err_kind="ref_missing",
                        err_first_line=one_line(f"参考产物不在：{rel(bin_path)}"))
        t_run = time.perf_counter()
        payload, error = run_diff(bin_path, src, task["ticks"], task["seed"], seq)
        run_ms += ms_since(t_run)
        if payload is None or error:
            return done(STATUS_HOST_FAIL, err_kind=side,
                        err_first_line=error or "run 没有 JSON 结果")
        row["stores_ref" if side == "run_ref" else "stores_ours"] = payload.get("store_count", "")

    # 4) 终态判据 + 诊断：**一次调用**。判据取带 `--src` 的那一份（只比 `[game_lo, game_hi)`，
    # 理由见模块 docstring）；`--classify` 顺带给出不带 `--src` 的整份总数与按地址段的分账。
    t_diff = time.perf_counter()
    payload, error = diff_report(seq_ref, seq_ours, src)
    row["diff_ms"] = ms_since(t_diff)
    row["run_ms"] = round(run_ms, 1)
    if error or payload is None:
        # 过滤器自己会拒绝荒谬的地址空间（与代码区重叠 / 跨度 > 4 GiB），那不是「相等」。
        row["err_kind"] = "src_filter"
        row["err_first_line"] = error or "--src 那次没有 JSON 结果"
        return done(STATUS_HOST_FAIL)
    row["src_kept_a"] = payload.get("kept_a", "")
    row["src_kept_b"] = payload.get("kept_b", "")
    row["diff_load_ms"] = payload.get("load_ms", "")
    row["diff_fold_ms"] = payload.get("classify_ms", "")
    equal = bool(payload.get("equal"))
    first = payload.get("raw_first_diff_addr")
    row.update({"diff_bytes_raw": payload.get("raw_diff_bytes", ""),
                "diff_stack": payload.get("raw_stack", ""),
                "diff_code": payload.get("raw_code", ""),
                "diff_game": payload.get("raw_game", ""),
                "diff_game_prefixes": format_prefixes(payload.get("raw_game_prefixes") or []),
                "first_diff_addr": "" if first in (None, "") else f"0x{int(first):X}"})
    mismatch = classify_mismatch(row)
    if mismatch:
        row["err_kind"] = "classify_mismatch"
        row["err_first_line"] = one_line(mismatch)

    if equal:
        for seq in (seq_ref, seq_ours):           # equal 的序列没有定位价值，别留着占 GB
            with contextlib.suppress(OSError):
                os.remove(seq)
        return done(STATUS_EQUAL)

    return done(STATUS_DIFF)


# ---- 落盘 --------------------------------------------------------------------

def write_tsv(rows, path, order):
    """逆序也按语料表顺序落，好让两次跑的 TSV 能直接 `diff`。"""
    index = {cid: i for i, cid in enumerate(order)}
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\t".join(TSV_COLUMNS) + "\n")
            for row in sorted(rows, key=lambda r: index.get(str(r["id"]), len(order))):
                f.write("\t".join(str(row.get(col, "")) for col in TSV_COLUMNS) + "\n")
    except OSError as e:
        fail(f"写不了 TSV {rel(path)}：{e}", 2)


def read_tsv(path):
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    if not lines:
        return []
    head = lines[0].split("\t")
    rows: list[Row] = []
    for line in lines[1:]:
        if not line.strip():
            continue
        cells = line.split("\t")
        row: Row = {col: (cells[i] if i < len(cells) else "") for i, col in enumerate(head)}
        for col in INT_COLUMNS + FLOAT_COLUMNS:
            cell = str(row.get(col, "")).strip()
            if not cell:
                row[col] = ""
                continue
            try:
                row[col] = float(cell) if col in FLOAT_COLUMNS else int(cell)
            except ValueError:
                # 不静默当「没有表」：`--resume` 会因此全量重跑，看上去像 resume 失效。
                fail(f"{rel(path)} 的 {col} 列不是数字（第 {len(rows) + 2} 行）：{cell!r}", 2)
        rows.append(row)
    return rows


def summarize(rows, total, wall_seconds, args):
    counts = dict.fromkeys((STATUS_EQUAL, STATUS_DIFF, STATUS_CLI_FAIL, STATUS_HOST_FAIL,
                            STATUS_PREFIX_OK), 0)
    for row in rows:
        status = str(row.get("status") or "")
        if status in counts:
            counts[status] += 1
    secs = sorted(float(r["seconds"]) for r in rows if r.get("seconds") != "")
    return {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "total": total,
        "done": len(rows),
        "counts": counts,
        "wall_seconds": round(wall_seconds, 1),
        "jobs": args.jobs,
        "ticks": args.ticks,
        "seed": args.seed,
        "seconds_median": secs[len(secs) // 2] if secs else None,
        "seconds_max": secs[-1] if secs else None,
        "seconds_min": secs[0] if secs else None,
    }


def md_cell(text, limit=150):
    flat = one_line(text, limit).replace("|", "\\|")
    return flat or "—"


def write_report(rows, summary, path, args, total, order):
    index = {cid: i for i, cid in enumerate(order)}
    rows = sorted(rows, key=lambda r: index.get(str(r["id"]), len(order)))
    c = summary["counts"]
    out = []
    w = out.append
    w("# M5 语料状态表（difftrace 版）\n")
    w(f"生成 {summary['generated']} · 已完成 {summary['done']}/{total} 份"
      f"{'' if summary['done'] == total else ' · **中途快照（还没跑完）**'}"
      f" · `--jobs {args.jobs}` · {args.ticks} tick · seed `{args.seed}`\n")
    w("判据口径：装配 `build/simplexc_cli.exe`；两侧各 `difftrace run --save` 跑同样 tick 数；"
      "`difftrace diff --state --src <si> --classify` 比折叠后的内存终态。"
      "**verdict 取带 `--src` 的那次**（只比 `[game_lo, game_hi)`，见模块 docstring 里 2026-10-25 的修订）；"
      "`diff_bytes_raw` 是不带 `--src` 的整份差异，只是参考列。"
      "`first_diff_tick` 一律 `n/a`（逐 tick 要 `--tick-mark`，这批不传）。\n")
    w("机器可读：`" + rel(args.tsv) + "` · 重跑：`python simplexc/tools/m5/batch.py --jobs 12`\n")

    w("## 汇总\n")
    w("| 指标 | 值 |")
    w("| --- | --- |")
    w(f"| 语料总数 | {total} |")
    w(f"| 本次完成 | {summary['done']} |")
    w(f"| equal（终态逐字节全等） | {c[STATUS_EQUAL]} |")
    w(f"| diff（终态有差异） | {c[STATUS_DIFF]} |")
    w(f"| cli_fail（装不出来） | {c[STATUS_CLI_FAIL]} |")
    w(f"| host_fail（difftrace 报错） | {c[STATUS_HOST_FAIL]} |")
    if c[STATUS_PREFIX_OK]:
        w(f"| prefix_ok（前缀拼上去了且装配通过） | {c[STATUS_PREFIX_OK]} |")
    w(f"| 总墙钟 | {summary['wall_seconds']} s（{args.jobs} 路） |")
    w(f"| 每份耗时 | 中位 {summary['seconds_median']} s / 最短 {summary['seconds_min']} s / "
      f"最长 {summary['seconds_max']} s |")
    w("")

    diffs = [r for r in rows if r.get("status") == STATUS_DIFF]
    w(f"## diff 明细（{len(diffs)} 份）\n")
    if diffs:
        w("差异字节按地址段分：`stack` = 栈区（8 MB）、"
          "`code` = 代码区（产物镜像，`kCodeBase` 起 16 MB）、`game` = 其余；段界只在"
          "`difftrace/main.cc` 的 `kSegBounds` 里定义一处。"
          "`src_kept_a/b` 是带 `--src` 那次两边留下的写条数（0/0 = 那个过滤器在这份上是空的）。\n")
        w("`game` 那一列不是「游戏状态」：它还包括参考侧分配器向宿主拿的页（`0x4009…` / `0x400b…`，"
          "旧口径叫 `extern_only`）与宿主帧（`0x7ff9…`）。真正的 `#SIMULATION_STATE` 段在 `0x2328…`，"
          "看最后一列的地址前缀区分。\n")
        w("| id | 第一个不同地址 | 差异字节 | stack | code | game | game 前缀 | src_kept_a/b |")
        w("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for r in diffs:
            w(f"| {r['id']} | {r.get('first_diff_addr') or '—'} | {r.get('diff_bytes_raw')} | "
              f"{r.get('diff_stack')} | {r.get('diff_code')} | {r.get('diff_game')} | "
              f"{r.get('diff_game_prefixes') or '—'} | "
              f"{r.get('src_kept_a')}/{r.get('src_kept_b')} |")
        src_filter_bad = [r for r in diffs if r.get("err_kind") == "src_filter"]
        if src_filter_bad:
            w("\n`--src` 那次自己报错的份（过滤器拒绝荒谬的地址空间，`src_kept` 留空）："
              + " · ".join(f"{r['id']}（{md_cell(str(r.get('err_first_line') or ''), 80)}）"
                           for r in src_filter_bad) + "\n")
    else:
        w("（没有）")
    w("")

    host_bad = [r for r in rows if r.get("status") == STATUS_HOST_FAIL]
    w(f"## host_fail 明细（{len(host_bad)} 份）\n")
    if host_bad:
        w("difftrace 的原始错误行原样保留，不跳过、不改写。\n")
        w("| id | 哪一步 | 原始错误行 |")
        w("| --- | --- | --- |")
        for r in host_bad:
            w(f"| {r['id']} | {r.get('err_kind') or '—'} | {md_cell(str(r.get('err_first_line') or ''))} |")
    else:
        w("（没有）")
    w("")

    cli_bad = [r for r in rows if r.get("status") == STATUS_CLI_FAIL]
    w(f"## cli_fail 明细（{len(cli_bad)} 份）\n")
    if cli_bad:
        w("| id | rc | 原始错误首行 |")
        w("| --- | --- | --- |")
        for r in cli_bad:
            w(f"| {r['id']} | {r.get('cli_rc') or '—'} | {md_cell(str(r.get('err_first_line') or ''))} |")
    else:
        w("（没有）")
    w("")

    w("## 耗时异常（> 5 s 或 < 0.2 s）\n")
    odd = [r for r in rows
           if r.get("seconds") != "" and (float(r["seconds"]) > 5.0 or float(r["seconds"]) < 0.2)]
    if odd:
        w("每行的耗时已拆成 `cli_ms`（装配）/ `run_ms`（两侧各起一次 `difftrace run --save`，含落盘）/"
          " `diff_ms`（一次 `diff --classify`，里面再分 `diff_load_ms` 读盘+过滤 与 `diff_fold_ms` 折叠），"
          "不用再去单跑一份定位。\n")
        w("| id | 秒 | CLI ms | run ms | diff ms（读盘/折叠） | stores ref/ours | 归类 | 说明 |")
        w("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for r in odd:
            status = str(r.get("status") or "")
            if float(r["seconds"]) < 0.2:
                why = ("装配就被拒（sema / 装配期），三个 exe 都没起" if status == STATUS_CLI_FAIL
                       else "小份：blob 几 KB、store 序列百条量级")
            else:
                why = ("看 run_ms 与 diff_ms 哪边大：10000 tick 下序列是 GB 级，"
                       "run 与折叠都会被它拖起来")
            w(f"| {r['id']} | {r['seconds']} | {r.get('cli_ms')} | {r.get('run_ms')} | "
              f"{r.get('diff_ms')}（{r.get('diff_load_ms')}/{r.get('diff_fold_ms')}） | "
              f"{r.get('stores_ref')}/{r.get('stores_ours')} | {status} | {why} |")
    else:
        w("（没有）")
    w("")
    w("## 复现一份\n")
    w("```\n"
      "build/simplexc_cli.exe --src simplexc/corpus/src/<id>.si --out work/tmp/m5batch/blobs/<id>.bin\n"
      "build/difftrace.exe run --src simplexc/corpus/src/<id>.si --bin simplexc/corpus/ref/<id>.bin "
      f"--target {args.ticks} --seed {args.seed} --save work/tmp/m5batch/seq/<id>.ref.seq --json\n"
      "build/difftrace.exe run --src simplexc/corpus/src/<id>.si --bin work/tmp/m5batch/blobs/<id>.bin "
      f"--target {args.ticks} --seed {args.seed} --save work/tmp/m5batch/seq/<id>.ours.seq --json\n"
      "build/difftrace.exe diff --state work/tmp/m5batch/seq/<id>.ref.seq "
      "work/tmp/m5batch/seq/<id>.ours.seq --src simplexc/corpus/src/<id>.si --classify --json\n"
      "```\n")
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out))
    except OSError as e:
        fail(f"写不了报告 {rel(path)}：{e}", 2)


def main(argv=None):
    ap = argparse.ArgumentParser(description="M5 语料状态表：装配 + difftrace 终态差分（可并行）")
    ap.add_argument("--ids", default=None, help="只跑这些 id（逗号分隔）")
    ap.add_argument("--limit", type=int, default=0, help="只跑前 N 份语料（0 = 全部）")
    ap.add_argument("--ticks", type=int, default=100)
    ap.add_argument("--seed", default="seq", choices=("seq", "none"),
                    help="`#RESET_DATA_*` 灌什么（`xorshift` 会让电路跑到垃圾地址上，不给这个选项）")
    ap.add_argument("--jobs", type=int, default=1, help=f"并发进程数（1..{MAX_JOBS}）")
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    ap.add_argument("--tsv", default=None, help="默认 <out-dir>/batch.tsv")
    ap.add_argument("--report", default=None, help="默认 <out-dir>/report.md")
    ap.add_argument("--cli-timeout", type=float, default=120.0, help="单份装配的超时（秒），超了记 cli_fail(timeout)")
    ap.add_argument("--resume", action="store_true", help="从已有 TSV 里跳过已完成的 id")
    ap.add_argument("--with-prefix", action="store_true",
                    help="把 corpus/stdlib_prefix.si 拼在每份语料前再编（M5 stdlib）。"
                         "corpus/ref/ 里的参考产物**本来就是含前缀的**（compile() 内部自动加前缀），"
                         "所以这个模式照常跑差分")
    args = ap.parse_args(argv)

    if args.jobs < 1 or args.jobs > MAX_JOBS:
        fail(f"--jobs 只在 1..{MAX_JOBS} 之间：difftrace 16 路实测只比 12 路快 7%，多占 1.75 GB 内存"
             "（见 simplexc/tools/difftrace/README.md）", 2)
    if args.tsv is None:
        args.tsv = os.path.join(args.out_dir, "batch.tsv")
    if args.report is None:
        args.report = os.path.join(args.out_dir, "report.md")
    need = (CLI, DIFFTRACE)
    for exe in need:
        if not os.path.exists(exe):
            hint = "python simplexc/tools/build.py simplexc_cli" if exe == CLI else \
                   "python simplexc/tools/build.py difftrace"
            fail(f"缺 {rel(exe)}：先跑 {hint}", 2)

    blob_dir = os.path.join(args.out_dir, "blobs")
    seq_dir = os.path.join(args.out_dir, "seq")
    for d in (args.out_dir, blob_dir, seq_dir, os.path.dirname(os.path.abspath(args.tsv)),
              os.path.dirname(os.path.abspath(args.report))):
        try:
            os.makedirs(d, exist_ok=True)
        except OSError as e:
            fail(f"建不了输出目录 {d}：{e}", 2)

    targets = load_manifest()
    order = [t["id"] for t in targets]
    if args.ids:
        want = {s.strip() for s in args.ids.split(",") if s.strip()}
        unknown = sorted(want - set(order))
        if unknown:
            fail(f"--ids 里有语料表里没有的 id：{unknown}", 2)
        targets = [t for t in targets if t["id"] in want]
    if args.limit:
        targets = targets[:args.limit]

    rows = read_tsv(args.tsv) if args.resume else []
    done_ids = {str(r.get("id")) for r in rows if r.get("status")}
    todo = [t for t in targets if t["id"] not in done_ids]
    print(f"M5 语料状态表：{len(targets)} 份目标，本次跑 {len(todo)} 份（{args.jobs} 路 · "
          f"{args.ticks} tick · seed {args.seed}）"
          + (f"，--resume 跳过已完成 {len(done_ids)} 份" if done_ids else ""), flush=True)

    tasks = [{"id": t["id"], "src": t["src"], "ref": t["ref"],
              "blob": os.path.join(blob_dir, t["id"] + ".bin"), "seq_dir": seq_dir,
              "ticks": args.ticks, "seed": args.seed, "cli_timeout": args.cli_timeout,
              "with_prefix": args.with_prefix}
             for t in todo]

    wall0 = time.perf_counter()
    if not tasks:
        pass
    elif args.jobs == 1:
        for i, task in enumerate(tasks, 1):
            row = run_one(task)
            rows.append(row)
            write_tsv(rows, args.tsv, order)
            print(progress_line(i, len(tasks), row), flush=True)
    else:
        with ProcessPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(run_one, task): task["id"] for task in tasks}
            for i, fut in enumerate(as_completed(futures), 1):
                cid = futures[fut]
                try:
                    row = fut.result()
                except Exception as e:                    # 子进程炸了也要留一行，不能丢份
                    row = blank_row()
                    row.update({"id": cid, "status": STATUS_HOST_FAIL, "err_kind": "worker",
                                "err_first_line": one_line(f"{type(e).__name__}: {e}"),
                                "first_diff_tick": "n/a"})
                rows.append(row)
                write_tsv(rows, args.tsv, order)          # 每完成一份就落盘：中断可续跑
                print(progress_line(i, len(tasks), row), flush=True)
    wall = time.perf_counter() - wall0

    summary = summarize(rows, len(targets), wall, args)
    write_tsv(rows, args.tsv, order)
    write_report(rows, summary, args.report, args, len(targets), order)
    print(f"汇总：完成 {summary['done']}/{len(targets)} · "
          + " · ".join(f"{s} {n}" for s, n in summary["counts"].items())
          + f" · 总墙钟 {summary['wall_seconds']} s（{args.jobs} 路）", flush=True)
    print(f"TSV {rel(args.tsv)} · 报告 {rel(args.report)}", flush=True)
    return 0


def progress_line(i, n, row):
    status = row.get("status")
    extra = ""
    if status == STATUS_DIFF:
        extra = (f" diff={row.get('diff_bytes_raw')}B "
                 f"(stack {row.get('diff_stack')}/code {row.get('diff_code')}/game {row.get('diff_game')})"
                 f" 首址 {row.get('first_diff_addr')}")
    elif status in (STATUS_CLI_FAIL, STATUS_HOST_FAIL):
        extra = f" {row.get('err_kind')}: {one_line(str(row.get('err_first_line') or ''), 120)}"
    return (f"[{i}/{n}] {row.get('id')} {status} {row.get('seconds')}s cli={row.get('cli_ms')}ms "
            f"blob={row.get('blob_bytes')}B stores {row.get('stores_ref')}/{row.get('stores_ours')}"
            + extra)


if __name__ == "__main__":
    reconfigure = getattr(sys.stdout, "reconfigure", None)
    if reconfigure is not None:                       # 控制台常是 GBK，中文会花
        reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
