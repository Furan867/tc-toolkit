#!python3
"""M5 第一步：98 份语料的「能否装配 + 逐 tick 是否全等」覆盖表。

一份语料做两件事，第二件只在第一件成功后才做：

  1. **装配**：`build/simplexc_cli.exe --src <si> --out <blob>`（+ 同目录的 `.entry` / `.stubs`）。
     记 rc / 墙上耗时 / blob 大小 / entry；失败记错误类别与**原始错误首行**。
  2. **逐 tick 差分**：参考产物（`simplexc/corpus/ref/<id>.bin`）与我们自己的产物各跑一遍宿主，
     10 tick，两边都灌 `--seed-reset-data seq`，逐 tick 比段哈希 / 页 / extern / sim_cycle。

宿主与比对逻辑**不复写**：直接用 `simplexc/tools/host/selftest.py` 的 `run_host` / `read_snaps` /
`compare`（与 `check.py` 的 `lower_equiv` 同一条路）。参考产物的 rebase
（`corpus/manifest.tsv` 的 `rebased_delta = 0x380000`）由 `host.py` 的 `_rebase` 在进程内归一化，
这里不需要额外动作。`selftest.compare` 的段名是**运行期绝对地址**，所以额外记一列
`region_keys_same`：两边段名集合不同就说明宿主这次给的内存布局不一样，那种「差异」不能算到产物头上。

`status` 除 `equal` / `diff` / `cli_fail`… 外还有一档 **`extern_only`**：装配成功、**每个 tick 的段哈希 /
页 / `sim_cycle` 全等、tick 条数相同**，唯一的不同是 extern 序列 —— 内存状态逐 tick 等价，差异来自
**参考侧**产物自己的分配器调了 `VirtualAlloc`（我们不调）。所以它不算「值不对」：单列一档、
不计入差分回退率，但差异本身照旧记在 `extern_detail` 列里（审计用）。

产物：
  · `--tsv`（默认 `work/tmp/m5/coverage.tsv`，不进 git）—— 机器可读，每份一行；
  · `--summary-json`（默认 `work/tmp/m5/coverage.summary.json`）—— 汇总数字与失败分类；
  · `--report`（默认 `docs/notes/2026-10-18-m5-coverage.md`）—— 人读：覆盖表 + 汇总 + 失败最小复现。

checkpoint：每 `--batch` 份就重写一次上面三个文件（不是跑完才写），所以中断也留下已完成的部分；
`--resume` 从已有 TSV 里跳过已完成的 id 接着跑。

    python simplexc/tools/m5/coverage.py                  # 全量（98 + rv32im 基准行）
    python simplexc/tools/m5/coverage.py --limit 10       # 只跑前 10 份（验脚本用）
    python simplexc/tools/m5/coverage.py --resume         # 接着上次跑

**目录模式**（`--dir`）：扫一个目录、只做第 ①步，不差分。给的是 `corpus/campaign/` 这类
**没有 manifest、也没有参考产物**的语料 —— 没有参考产物可比，所以到「装得出来」为止：

    python simplexc/tools/m5/coverage.py --dir simplexc/corpus/campaign --no-diff \
        --report docs/notes/2026-10-18-campaign-survey.md

目录模式的形状 = 每个一级子目录里的 `test.si` 是一份语料，id = 子目录名。三个产物
（`--tsv` / `--summary-json` / `--report`）的默认值在这条路上全部落在 `--out-dir` 下
（`survey.tsv` / `survey.summary.json` / `survey.md`）—— 默认值若还指向仓库里那 98 份的报告，
跑一次摸底就把它们覆盖了。报告里带一节「与 98 份失败分类的对照」（口径见 `write_survey_report`）。
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from typing import NoReturn

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)                                 # simplexc/tools/
SIMPLEXC = os.path.dirname(TOOLS)                             # simplexc/
ROOT = os.path.dirname(SIMPLEXC)                              # reversing/

SELFTEST = os.path.join(TOOLS, "host", "selftest.py")
MANIFEST = os.path.join(SIMPLEXC, "corpus", "manifest.tsv")
REF_DIR = os.path.join(SIMPLEXC, "corpus", "ref")
CLI = os.path.join(ROOT, "build", "simplexc_cli.exe")

RV32IM_SRC = os.path.join("simplexc", "corpus", "rv32im", "rv32im.si")
RV32IM_BIN = os.path.join("simplexc", "corpus", "rv32im", "rv32im.bin")
RV32IM_CAPTURE = os.path.join(SIMPLEXC, "corpus", "rv32im", "manifest-capture.tsv")

DEFAULT_OUT_DIR = os.path.join(ROOT, "work", "tmp", "m5")
DEFAULT_TSV = os.path.join(DEFAULT_OUT_DIR, "coverage.tsv")
DEFAULT_SUMMARY = os.path.join(DEFAULT_OUT_DIR, "coverage.summary.json")
DEFAULT_REPORT = os.path.join(ROOT, "docs", "notes", "2026-10-18-m5-coverage.md")

# 目录模式的产物名（落在 `--out-dir` 下，不进 git）与 98 份那轮的汇总的位置。
SURVEY_TSV = "survey.tsv"
SURVEY_SUMMARY = "survey.summary.json"
SURVEY_REPORT = "survey.md"
COVERAGE_SUMMARY = DEFAULT_SUMMARY          # 目录模式的报告拿它做「与 98 份对照」

TSV_COLUMNS = ["scope", "id", "status", "cli_rc", "cli_ms", "blob_bytes", "entry", "ref_bytes",
               "ticks_ref", "ticks_ours", "region_keys_same", "diff_lines", "diff_ticks",
               "err_kind", "err_detail", "err_first_line", "first_diff", "extern_detail",
               "host_retries", "seconds"]

# 一行结果就是 TSV 的这些列（对外全是字符串，写盘时再字符串化）。
Row = dict[str, object]
INT_COLUMNS = ("cli_rc", "cli_ms", "blob_bytes", "ref_bytes", "ticks_ref", "ticks_ours",
               "diff_lines", "diff_ticks", "host_retries")
FLOAT_COLUMNS = ("seconds",)

# `status` 的取值 —— 报告里的「失败分类」按前两级展开。
STATUS_EQUAL = "equal"                 # 装配成功且 10 tick 逐 tick 全等
STATUS_DIFF = "diff"                   # 装配成功但有差异
STATUS_EXTERN_ONLY = "extern_only"     # 装配成功、每个 tick 的段哈希/页/sim_cycle 全等，唯一差异是 extern 序列
                                       #   （内存逐 tick 等价：参考侧分配器调 `VirtualAlloc`，我们不调）
STATUS_CLI_FAIL = "cli_fail"           # 装配失败（rc≠0 或没有产物）
STATUS_CLI_TIMEOUT = "cli_timeout"
STATUS_REF_MISSING = "ref_missing"     # 装配成功，但参考产物不在（ref/ 不进 git，可能是没重新生成）
STATUS_HOST_FAIL = "host_fail"
STATUS_HOST_TIMEOUT = "host_timeout"
STATUS_ASSEMBLED = "assembled"         # 目录模式（--no-diff）：装出来了，没有参考产物可比，到此为止

# 失败/不等分的状态。`extern_only` 刻意**不在**这里：它内存逐 tick 等价，不算「值不对」，
# 单列一档（汇总里单独一列，见 `aggregate` 的 `extern_only` / `extern_only_details`）。
FAIL_STATUSES = [STATUS_CLI_FAIL, STATUS_CLI_TIMEOUT, STATUS_REF_MISSING,
                 STATUS_HOST_FAIL, STATUS_HOST_TIMEOUT, STATUS_DIFF]

# 「装出来了」的状态（差分那步的结果不算 —— 它要求已经有产物）。
ASSEMBLED_STATUSES = (STATUS_EQUAL, STATUS_DIFF, STATUS_EXTERN_ONLY, STATUS_REF_MISSING,
                      STATUS_HOST_FAIL, STATUS_HOST_TIMEOUT, STATUS_ASSEMBLED)


def fail(msg, rc) -> NoReturn:
    """失败出口。本脚本原本一个都没有（错误只能靠调用方看 rc）。

    消息照 `hook_offline.BuildError` 的风格写：「做什么 + 路径 + 原因」。
    rc 口径同 `lower/ir_build.py`：环境/路径失败 2，编译/比对失败 1。
    """
    print(msg, file=sys.stderr)
    raise SystemExit(rc)


def load_selftest():
    """按路径加载 `host/selftest.py`（它不是包，但 import 它会顺带把 host 的依赖装好）。"""
    spec = importlib.util.spec_from_file_location("m5_selftest", SELFTEST)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {SELFTEST}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


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
    """CLI 的原始错误首行：先 stderr（失败信息都在 stderr），没有再看 stdout。

    只取**第一行**：CLI 一次只报一个错，多行是它的上下文/建议。"""
    for blob in (stderr, stdout):
        for line in (blob or "").splitlines():
            if line.strip():
                return one_line(line)
    return ""


def last_error_line(stderr):
    """宿主的错误行：取**最后一行** —— 宿主平时往 stderr 打的都是过程日志，
    真正的原因（崩溃 / 熔断）总是最后那一条。"""
    lines = [ln for ln in (stderr or "").splitlines() if ln.strip()]
    return one_line(lines[-1]) if lines else ""


# 错误类别：`cli/simplexc_cli.cc` 的 catch 分支前缀一一对应（前缀是稳定的，细节不稳定）。
# `err_detail` 把「第 N 行 第 M 列」与数字抹掉，好让同一类错误在多份语料上合并计数。
ERROR_KINDS = [
    ("语法错误", "ParseError"),
    ("词法错误", "LexError"),
    ("UNSUPPORTED", "Unsupported"),
    ("编译失败", "CompileFail"),
    ("装配失败", "AssembleFail"),
    ("内部错误", "InternalError"),
    ("读不了", "IoRead"),
    ("写不了", "IoWrite"),
]
_NO_NUMBERS = re.compile(r"0[xX][0-9a-fA-F]+|\d+")
# 错误信息里的路径（含 `\` 或 `/` 的整串）也归一化 —— 否则同一类错误在每份语料上都是新的一类。
_PATH_TOKEN = re.compile(r"\S*[\\/]\S*")


def normalize_detail(detail):
    return one_line(_PATH_TOKEN.sub("<path>", _NO_NUMBERS.sub("#", detail)), 120)


def classify_cli_error(rc, first_line):
    """→ (err_kind, err_detail)。`err_detail` 是首行去掉行号/列号/数字后的形态。"""
    if not first_line:
        return ("RcOnly", f"rc={rc}")
    for prefix, kind in ERROR_KINDS:
        if first_line.startswith(prefix):
            detail = first_line.split("：", 1)[1] if "：" in first_line else first_line
            return (kind, normalize_detail(detail))
    return ("Other", normalize_detail(first_line))


def load_manifest_targets():
    """`corpus/manifest.tsv` → 目标列表（98 份唯一语料，顺序照表）。"""
    try:
        with open(MANIFEST, encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f if line.strip()]
    except OSError as e:
        fail(f"读不了语料清单 {rel(MANIFEST)}：{e}", 2)
    head = rows[0]
    ci = {name: head.index(name) for name in ("id", "file", "entry")}
    targets = []
    for row in rows[1:]:
        cid = row[ci["id"]]
        targets.append({"scope": "corpus", "id": cid,
                        "src": rel(os.path.join(SIMPLEXC, "corpus", row[ci["file"]])),
                        "ref": rel(os.path.join(REF_DIR, cid + ".bin")),
                        "ref_entry": int(row[ci["entry"]], 0)})
    return targets


def load_dir_targets(root):
    """扫一个目录 → 目标列表。id = 子目录名，`src` = 子目录里的 `test.si`。

    目录语料（`corpus/campaign/` 那种）没有 manifest、没有参考产物，所以 `ref` / `ref_entry` 留空
    —— `--no-diff` 那条路上用不到它们。空目录不算「成功跑完 0 份」，宁可在这里停。"""
    try:
        names = sorted(e.name for e in os.scandir(root)
                       if e.is_dir() and os.path.isfile(os.path.join(e.path, "test.si")))
    except OSError as e:
        fail(f"读不了语料目录 {root}：{e}", 2)
    if not names:
        fail(f"{root} 下没有「子目录/test.si」—— 目录语料的形状是一层子目录加一份 test.si"
             "（比如 simplexc/corpus/campaign/<关卡名>/test.si）", 2)
    return [{"scope": "survey", "id": name,
             "src": rel(os.path.join(root, name, "test.si")), "ref": "", "ref_entry": 0}
            for name in names]


def rv32im_ref_entry():
    """rv32im 的参考入口**不硬编码**：从捕获 manifest 里找 `out_sha256` 等于 rv32im.bin 的那一行，
    取它的 `r2`（捕获时记下的入口寄存器）。找不到就报错 —— 不静默用一个可能是旧的常量。"""
    bin_sha = hashlib.sha256(read_bytes(os.path.join(ROOT, RV32IM_BIN))).hexdigest()
    try:
        with open(RV32IM_CAPTURE, encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f if line.strip()]
    except OSError as e:
        fail(f"读不了 rv32im 的捕获 manifest {rel(RV32IM_CAPTURE)}：{e}", 2)
    ci = {name: rows[0].index(name) for name in ("out_sha256", "r2")}
    for row in rows[1:]:
        if row[ci["out_sha256"]] == bin_sha:
            return int(row[ci["r2"]], 0)
    raise SystemExit(f"{RV32IM_BIN} 的 sha256 在 {RV32IM_CAPTURE} 里找不到 —— 捕获 manifest 与产物对不上")


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        fail(f"读不了 {rel(path)}：{e}", 2)


def rv32im_target():
    return {"scope": "baseline", "id": "rv32im", "src": RV32IM_SRC, "ref": RV32IM_BIN,
            "ref_entry": rv32im_ref_entry()}


def run_cli(src, blob, timeout):
    """跑一次产品路径 CLI。返回 (rc, seconds, stdout, stderr, timed_out)。"""
    for suffix in ("", ".entry", ".stubs"):                   # 先删干净：别让上一次的 sidecar 冒充这一次的
        try:
            os.remove(blob + suffix)
        except FileNotFoundError:
            pass                                       # 第一轮跑本来就没它，不是失败
        except OSError as e:
            fail(f"删不了上一轮留下的产物 {rel(blob + suffix)}：{e}", 2)
    cmd = [CLI, "--src", src, "--out", blob]
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return (None, round(time.perf_counter() - t0, 3), "", f"CLI 超时（{timeout:.0f} s）", True)
    return (proc.returncode, round(time.perf_counter() - t0, 3), proc.stdout, proc.stderr, False)


def read_entry(blob):
    """`.entry` 的内容是十六进制（无 `0x` 前缀，见 `cli/simplexc_cli.cc`）。"""
    try:
        with open(blob + ".entry", encoding="ascii") as f:
            return "0x" + f.read().strip().upper()
    except OSError as e:
        # 不在这里退出：这份语料的失败要记成它自己的 EntryMissing 行（run_target 的 except OSError）
        raise OSError(f"读不了装配器写出的 {rel(blob)}.entry：{e}") from e


def host_once(selftest, src, binpath, entry, ticks, seed, out, budget, timeout):
    """一遍宿主。返回 (rc, seconds, snaps, timed_out, err_line)。宿主的 stderr 只留首行 —— 里面有噪声。"""
    extra = ["--entry", entry]
    if seed:
        extra += ["--seed-reset-data", seed]
    t0 = time.perf_counter()
    try:
        proc = selftest.run_host(src, binpath, ticks, out, budget, extra)
    except subprocess.TimeoutExpired:
        return (None, round(time.perf_counter() - t0, 3), [], True, f"宿主超时（>{timeout} s）")
    return (proc.returncode, round(time.perf_counter() - t0, 3),
            selftest.read_snaps(out), False, last_error_line(proc.stderr))


def diff_ticks_of(diff_lines):
    """差异涉及多少个 tick（`compare` 的每条差异都是 `tick N: …` 开头）。"""
    ticks = set()
    for line in diff_lines:
        m = re.match(r"tick (\d+):", line)
        if m:
            try:
                ticks.add(int(m.group(1)))
            except ValueError:
                # `\d+` 保证是数字 —— 不可达；留着是为了形状变了别抛裸 ValueError
                fail(f"tick 号不是十进制整数：{m.group(0)!r}", 2)
    return len(ticks)


# `selftest.compare` 每条差异都是 `tick N: …` 开头，第二个成分决定差异的种类 ——
# 拿它当 TickDiff 的子类，比笼统一类「有差异」有用。顺序重要（页/段的行里也有别的词）。
DIFF_KINDS = [("段 ", "段哈希"), ("页不同", "页"), ("sim_cycle", "sim_cycle"),
              ("extern", "extern"), ("没有这一 tick", "tick 条数")]


def diff_kind(line):
    for needle, kind in DIFF_KINDS:
        if needle in line:
            return kind
    return one_line(line, 60)


def region_keys_same(ref_snaps, ours_snaps):
    """两边每个 tick 的段名集合是否相同。段名是运行期绝对地址 —— 不同就说明两遍宿主的内存布局
    不一样，那种差异是宿主噪声，不能算产物的错。"""
    for i in range(min(len(ref_snaps), len(ours_snaps))):
        a, b = ref_snaps[i].get("regions"), ours_snaps[i].get("regions")
        if set(a or {}) != set(b or {}):
            return False
    return True


def extern_names(snaps):
    """一份快照序列里 extern 的 `(总条数, {符号名: 条数})`；名字缺失记 `?`。"""
    total = 0
    names: dict[str, int] = {}
    for snap in snaps:
        for ex in snap.get("extern") or []:
            total += 1
            name = ex.get("name") if isinstance(ex, dict) else None
            key = str(name) if name else "?"
            names[key] = names.get(key, 0) + 1
    return total, names


def names_text(names):
    """`{符号: 条数}` → `VirtualAlloc×1、…`（空集写「无」）。"""
    return "、".join(f"{k}×{v}" for k, v in sorted(names.items())) or "无"


def extern_only_split(ref_snaps, ours_snaps):
    """→ `(是否只差 extern, 审计内容)`。

    判据（全部满足才算）：tick 条数相同、**每个 tick 的段哈希 / 页 / `sim_cycle` 全等**、
    且 extern 序列至少有一处不同。

    刻意**不拿 `selftest.compare` 的输出当判据**：它到 8 条差异就收口，只看它会把第 9 条往后
    的内存差异当成「只差 extern」—— 这里逐 tick 全看一遍。返回的第二项把 extern 差异摊开写
    （两边条数与符号名、哪些 tick 不同）：单列一档 ≠ 没差异，内容得留在表里。
    """
    if not ref_snaps or len(ref_snaps) != len(ours_snaps):
        return False, ""
    tick_differs = []
    for i, (a, b) in enumerate(zip(ref_snaps, ours_snaps, strict=False)):
        if a.get("regions") != b.get("regions") or a["pages"] != b["pages"] \
                or a["sim_cycle"] != b["sim_cycle"]:
            return False, ""
        if a["extern"] != b["extern"]:
            tick_differs.append(i + 1)
    if not tick_differs:
        return False, ""
    ref_total, ref_names = extern_names(ref_snaps)
    ours_total, ours_names = extern_names(ours_snaps)
    return True, (f"extern ref {ref_total} 条（{names_text(ref_names)}）/ "
                  f"ours {ours_total} 条（{names_text(ours_names)}）· 差异 tick {tick_differs}")


def run_target(target, selftest, args):
    """一个目标 → 一行结果（TSV 的各列 + 报告要的原始文本）。"""
    row: Row = dict.fromkeys(TSV_COLUMNS, "")
    row["scope"] = target["scope"]
    row["id"] = target["id"]
    src = os.path.join(ROOT, target["src"])
    blob = os.path.join(args.out_dir, "out", target["id"] + ".bin")
    try:
        os.makedirs(os.path.dirname(blob), exist_ok=True)
    except OSError as e:
        fail(f"建不了产物目录 {rel(os.path.dirname(blob))}：{e}", 2)
    ref = os.path.join(ROOT, target["ref"]) if target["ref"] else None
    row["ref_bytes"] = os.path.getsize(ref) if ref and os.path.exists(ref) else 0

    t0 = time.perf_counter()
    rc, cli_s, out_s, err_s, timed_out = run_cli(src, blob, args.cli_timeout)
    row["cli_rc"] = "" if rc is None else rc
    try:
        row["cli_ms"] = int(cli_s * 1000)
    except (ValueError, OverflowError) as e:
        # 正常不可达（cli_s 来自 perf_counter）；留着是为了它变形时别抛裸异常
        fail(f"cli_ms 字段算不出来：cli_s={cli_s!r}（{e}）", 2)
    blob_ok = os.path.exists(blob)
    row["blob_bytes"] = os.path.getsize(blob) if blob_ok else 0
    err_line = first_error_line(out_s, err_s)

    if timed_out:
        row["status"] = STATUS_CLI_TIMEOUT
        row["err_kind"], row["err_detail"] = "CliTimeout", f">{args.cli_timeout:.0f} s"
        row["err_first_line"] = err_line
        row["seconds"] = round(time.perf_counter() - t0, 3)
        return row
    if rc != 0 or not blob_ok:
        row["status"] = STATUS_CLI_FAIL
        row["err_kind"], row["err_detail"] = classify_cli_error(rc, err_line)
        row["err_first_line"] = err_line
        row["seconds"] = round(time.perf_counter() - t0, 3)
        return row

    try:
        row["entry"] = read_entry(blob)
    except OSError as e:
        row["status"] = STATUS_CLI_FAIL
        row["err_kind"], row["err_detail"] = "EntryMissing", one_line(str(e), 120)
        row["err_first_line"] = row["err_detail"]
        row["seconds"] = round(time.perf_counter() - t0, 3)
        return row

    if args.no_diff:
        # 目录语料没有参考产物 —— 判到「装得出来」为止（差分要两边都有产物才能比）。
        row["status"] = STATUS_ASSEMBLED
        row["seconds"] = round(time.perf_counter() - t0, 3)
        return row

    if not (ref and os.path.exists(ref)):
        row["status"] = STATUS_REF_MISSING
        row["err_kind"], row["err_detail"] = "RefMissing", rel(ref or "")
        row["err_first_line"] = f"没有参考产物 {rel(ref or '')}（corpus/ref/ 不进 git）"
        row["seconds"] = round(time.perf_counter() - t0, 3)
        return row

    logs = os.path.join(args.out_dir, "logs")
    try:
        os.makedirs(logs, exist_ok=True)
    except OSError as e:
        fail(f"建不了宿主日志目录 {rel(logs)}：{e}", 2)
    snaps = {}
    for tag, binpath, entry in (("ref", ref, hex(target["ref_entry"])),
                               ("ours", blob, row["entry"])):
        out = os.path.join(logs, f"{target['id']}.{tag}.jsonl")
        try:
            os.remove(out)
        except FileNotFoundError:
            pass                                       # 第一轮跑本来就没它，不是失败
        except OSError as e:
            fail(f"删不了上一轮留下的宿主日志 {rel(out)}：{e}", 2)
        timeout = max(150.0, args.ticks * (args.budget + 5))
        # 宿主的瞬时失败多数是**固定地址撞车**：它要把游戏的段映射到捕获时的绝对地址
        # （014/012 实测 `VirtualAlloc(0x232a9f50000, …) 失败 err=487`），换一个进程地址空间就好了 ——
        # 那是测量噪声，不是产物的差异。所以宿主失败（非超时）重试，超时不重试（超时重试只会把时间翻倍）。
        attempts = 0
        rc_h, sn, to, hline = None, [], False, ""
        while attempts < args.host_retries + 1:
            attempts += 1
            rc_h, _secs, sn, to, hline = host_once(selftest, src, binpath, entry, args.ticks,
                                                 args.seed, out, args.budget, timeout)
            if to or (rc_h == 0 and sn):
                break
        row["host_retries"] = attempts - 1
        if to:
            row["status"] = STATUS_HOST_TIMEOUT
            row["err_kind"], row["err_detail"] = "HostTimeout", f"{tag} > {timeout:.0f} s"
            row["err_first_line"] = hline
            row["ticks_" + tag] = len(sn)
            row["seconds"] = round(time.perf_counter() - t0, 3)
            return row
        if rc_h != 0 or not sn:
            row["status"] = STATUS_HOST_FAIL
            row["err_kind"] = "HostRc"
            row["err_detail"] = f"{tag} rc={rc_h} 快照 {len(sn)} 条（试 {attempts} 次）"
            row["err_first_line"] = f"{tag}：{hline}"
            row["ticks_" + tag] = len(sn)
            row["seconds"] = round(time.perf_counter() - t0, 3)
            return row
        snaps[tag] = sn
    row["ticks_ref"] = len(snaps["ref"])
    row["ticks_ours"] = len(snaps["ours"])
    row["region_keys_same"] = "yes" if region_keys_same(snaps["ref"], snaps["ours"]) else "no"

    diff = selftest.compare(snaps["ref"], snaps["ours"])
    row["diff_lines"] = len(diff)
    row["diff_ticks"] = diff_ticks_of(diff)
    if diff:
        row["first_diff"] = one_line(diff[0], 300)
        row["err_kind"], row["err_detail"] = "TickDiff", diff_kind(diff[0])
        row["err_first_line"] = row["first_diff"]
        only_extern, extern_note = extern_only_split(snaps["ref"], snaps["ours"])
        if only_extern:
            row["status"] = STATUS_EXTERN_ONLY
            row["extern_detail"] = extern_note
            row["err_kind"], row["err_detail"] = "ExternOnly", "只差 extern（内存逐 tick 等价）"
            row["err_first_line"] = extern_note
        else:
            row["status"] = STATUS_DIFF
    else:
        row["status"] = STATUS_EQUAL
    row["seconds"] = round(time.perf_counter() - t0, 3)
    return row


def aggregate(rows, no_diff=False):
    """汇总数字。语料口径 = 除 rv32im 基准行以外的所有行（`corpus` 与目录模式的 `survey` 都算）。"""
    corpus = [r for r in rows if r["scope"] != "baseline"]
    baseline = [r for r in rows if r["scope"] == "baseline"]
    total = len(corpus)
    by_status = {}
    for r in corpus:
        by_status[r["status"]] = by_status.get(r["status"], 0) + 1
    assembled = sum(by_status.get(s, 0) for s in ASSEMBLED_STATUSES)
    equal = by_status.get(STATUS_EQUAL, 0)
    extern_only = by_status.get(STATUS_EXTERN_ONLY, 0)
    classes = {}
    for r in corpus:
        # `extern_only` 内存逐 tick 等价，不进失败分类表（那些份的差异另有 `extern_only_details`）
        if r["status"] in (STATUS_EQUAL, STATUS_EXTERN_ONLY):
            continue
        key = (r["err_kind"] or r["status"], r["err_detail"] or r["status"])
        hit = classes.setdefault(key, {"kind": key[0], "detail": key[1], "count": 0, "ids": []})
        hit["count"] += 1
        hit["ids"].append(r["id"])
    cli_ms = sorted(r["cli_ms"] for r in corpus)
    summary = {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "total": total,
        "no_diff": no_diff,
        "assembled": assembled,
        "equal": equal,
        "by_status": by_status,
        "extern_only": extern_only,
        "extern_only_ids": [r["id"] for r in corpus if r["status"] == STATUS_EXTERN_ONLY],
        "extern_only_details": [{"id": r["id"], "extern": r["extern_detail"]}
                                for r in corpus if r["status"] == STATUS_EXTERN_ONLY],
        "fallback_rate_assemble": round((total - assembled) / total, 4) if total else None,
        # 「差分回退率」不含 `extern_only`：那些内存逐 tick 等价，不算「值不对」。
        # 含它们的原始比率另记 `rate_not_tick_equal`，方便与旧口径对账。
        "fallback_rate_diff": round((total - equal - extern_only) / total, 4) if total else None,
        "rate_not_tick_equal": round((total - equal) / total, 4) if total else None,
        "fail_classes": sorted(classes.values(), key=lambda c: (-c["count"], c["kind"], c["detail"])),
        "cli_ms_median": cli_ms[len(cli_ms) // 2] if cli_ms else None,
        "cli_ms_max": cli_ms[-1] if cli_ms else None,
        "corpus_wall_seconds": round(sum(r["seconds"] for r in corpus), 1),
        "baseline": [{"id": r["id"], "status": r["status"], "blob_bytes": r["blob_bytes"],
                      "entry": r["entry"], "diff_lines": r["diff_lines"],
                      "diff_ticks": r["diff_ticks"], "ticks_ref": r["ticks_ref"],
                      "region_keys_same": r["region_keys_same"]} for r in baseline],
    }
    return summary


def summary_line(summary):
    parts = [f"总 {summary['total']}", f"可装配 {summary['assembled']}"]
    if not summary["no_diff"]:
        parts.append(f"逐 tick 全等 {summary['equal']}")
        if summary["extern_only"]:
            parts.append(f"只差 extern {summary['extern_only']}")
    for s in FAIL_STATUSES:
        n = summary["by_status"].get(s, 0)
        if n:
            parts.append(f"{s} {n}")
    parts.append(f"装配回退率 {summary['fallback_rate_assemble']:.1%}")
    if not summary["no_diff"]:            # 没跑差分就别报差分回退率 —— 那会读成「全都不等」
        parts.append(f"差分回退率 {summary['fallback_rate_diff']:.1%}（不含只差 extern）")
    return " · ".join(parts)


def write_tsv(rows, path):
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\t".join(TSV_COLUMNS) + "\n")
            for row in rows:
                f.write("\t".join(str(row.get(col, "")) for col in TSV_COLUMNS) + "\n")
    except OSError as e:
        fail(f"写不了覆盖表 {rel(path)}：{e}", 2)


def read_tsv(path):
    rows: list[Row] = []
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return rows
    if not lines:
        return rows
    head = lines[0].split("\t")
    for line in lines[1:]:
        if not line.strip():
            continue
        cells = line.split("\t")
        row: Row = {col: (cells[i] if i < len(cells) else "") for i, col in enumerate(head)}
        for col in INT_COLUMNS:                              # TSV 里全是字符串 → 数值列还原
            cell = str(row[col]).strip()
            try:
                row[col] = int(cell) if cell else ""
            except ValueError:
                fail(f"{rel(path)} 的 {col} 列不是整数：{cell!r}", 2)
        for col in FLOAT_COLUMNS:
            cell = str(row[col]).strip()
            try:
                row[col] = float(cell) if cell else 0.0
            except ValueError:
                fail(f"{rel(path)} 的 {col} 列不是浮点数：{cell!r}", 2)
        rows.append({col: row.get(col, "") for col in TSV_COLUMNS})
    return rows


def md_cell(text, limit=110):
    flat = one_line(text, limit).replace("|", "\\|")
    return flat or "—"


def repro_commands(row, args):
    """失败最小复现：装配失败给 CLI 一条；差分失败给两条宿主 + 一条比对（日志留在 --out-dir/logs/）。"""
    lines = []
    if row["status"] in (STATUS_CLI_FAIL, STATUS_CLI_TIMEOUT):
        lines.append(f"build/simplexc_cli.exe --src {row['src']} --out work/tmp/m5/out/{row['id']}.bin")
    elif row["status"] in (STATUS_DIFF, STATUS_EXTERN_ONLY, STATUS_HOST_FAIL, STATUS_HOST_TIMEOUT):
        src = row["src"]
        lines.append(f"python simplexc/tools/host/host.py --src {src} --bin {row['ref_path']} "
                     f"--entry {row['ref_entry_s']} --ticks {args.ticks} --seed-reset-data {args.seed} "
                     f"--out work/tmp/m5/logs/{row['id']}.ref.jsonl --quiet")
        lines.append(f"python simplexc/tools/host/host.py --src {src} --bin work/tmp/m5/out/{row['id']}.bin "
                     f"--entry {row['entry']} --ticks {args.ticks} --seed-reset-data {args.seed} "
                     f"--out work/tmp/m5/logs/{row['id']}.ours.jsonl --quiet")
        lines.append(f'python -c "import importlib.util as u,json;s=u.spec_from_file_location(\'s\','
                     f'\'simplexc/tools/host/selftest.py\');m=u.module_from_spec(s);s.loader.exec_module(m);'
                     f'L=lambda p:[json.loads(x) for x in open(p,encoding=\'utf-8\')];'
                     f'print(*m.compare(L(\'work/tmp/m5/logs/{row["id"]}.ref.jsonl\'),'
                     f'L(\'work/tmp/m5/logs/{row["id"]}.ours.jsonl\')),sep=\'\\n\')"')
    return lines


def write_report(rows, summary, path, args, targets):
    corpus = [r for r in rows if r["scope"] == "corpus"]
    src_of = {t["id"]: t for t in targets}
    corpus_total = len([t for t in targets if t["scope"] == "corpus"])
    work_done = len(corpus)
    out = []
    w = out.append
    w("# M5 覆盖表：98 份语料的装配与逐 tick 等价\n")
    w(f"生成 {summary['generated']} · 已完成 {work_done}/{corpus_total} 份 · "
      f"{'**中途快照（还没跑完）**' if work_done < corpus_total else '跑完'}\n")
    w("判据口径：每份语料 ①`build/simplexc_cli.exe --src <si> --out <blob>` 装配；"
      f"②装配成功后，参考产物（`corpus/ref/<id>.bin`）与我们的产物各跑一遍宿主，"
      f"**{args.ticks} tick**，两边都灌 `--seed-reset-data {args.seed}`，逐 tick 比段哈希/页/extern"
      "（宿主与比对逻辑复用 `simplexc/tools/host/selftest.py`）。\n")
    w("重跑：`python simplexc/tools/m5/coverage.py`（覆盖本文件）· "
      "机器可读：`work/tmp/m5/coverage.tsv`（不进 git）\n")

    w("## 汇总（口径 = 98 份语料）\n")
    w("| 指标 | 值 |")
    w("| --- | --- |")
    w(f"| 语料总数 | {corpus_total}（本次已跑 {summary['total']}） |")
    w(f"| 可装配 | {summary['assembled']}（{1 - summary['fallback_rate_assemble']:.1%}） |")
    w(f"| 逐 tick 全等 | {summary['equal']}（{summary['equal'] / summary['total']:.1%}） |")
    w(f"| 只差 extern（内存逐 tick 等价） | {summary['extern_only']} |")
    w(f"| 装配回退率 | **{summary['fallback_rate_assemble']:.1%}**（{summary['total'] - summary['assembled']} 份装不出来） |")
    w(f"| 差分回退率（不含只差 extern） | **{summary['fallback_rate_diff']:.1%}**"
      f"（{summary['total'] - summary['equal'] - summary['extern_only']} 份不能逐 tick 全等、且不是只差 extern） |")
    w(f"| 不能逐 tick 全等（含只差 extern） | {summary['total'] - summary['equal']}"
      f"（{summary['rate_not_tick_equal']:.1%}） |")
    for s in FAIL_STATUSES:
        n = summary["by_status"].get(s, 0)
        if n:
            w(f"| 其中 {s} | {n} |")
    w(f"| CLI 耗时 | 中位 {summary['cli_ms_median']} ms / 最大 {summary['cli_ms_max']} ms |")
    w(f"| 总墙上时间（语料部分） | {summary['corpus_wall_seconds']} s |")
    w("")
    diff_rows = [r for r in corpus if r["status"] in (STATUS_EQUAL, STATUS_DIFF, STATUS_EXTERN_ONLY)]
    keys_no = [r["id"] for r in diff_rows if r["region_keys_same"] != "yes"]
    host_bad = [r["id"] for r in corpus if r["status"] in (STATUS_HOST_FAIL, STATUS_HOST_TIMEOUT)]
    timeouts = [r["id"] for r in corpus if r["status"] in (STATUS_CLI_TIMEOUT, STATUS_HOST_TIMEOUT)]
    w("### 口径自检\n")
    w(f"- 段名集合（运行期绝对地址）两边一致：{len(diff_rows) - len(keys_no)}/{len(diff_rows)} 份"
      + ("—— 全部一致，差异不是宿主内存布局的噪声" if not keys_no else f"；不一致 {keys_no}，那几行得重跑"))
    w(f"- 参考产物在宿主里自己跑不起来（判定不了，不算我们的差异）：{host_bad or '无'}")
    retried = [r["id"] for r in corpus if str(r.get("host_retries") or 0) not in ("", "0")]
    retried_ok = [i for i in retried if i not in host_bad]
    w(f"- 宿主首轮失败（固定地址撞车这类噪声）：{retried or '无'}；其中重试后拿到结果的 {retried_ok or '无'}，"
      f"重试仍失败的 {[i for i in retried if i in host_bad] or '无'}")
    if host_bad:
        sample = next(r for r in corpus if r["id"] == host_bad[0])
        w(f"- **这几份判定不稳定**：{host_bad} 的**参考产物**自己在宿主里崩（`{sample['err_first_line']}`）"
          "—— 崩不崩跟进程地址空间有关（同一命令重跑时好时坏）→ 不能据它判我们的产物。")
    w(f"- 超时：{timeouts or '无'}")
    w("")
    for b in summary["baseline"]:
        w(f"基准行 **{b['id']}**（不在 98 内）：装配 {b['status']} · blob {b['blob_bytes']} B · "
          f"entry {b['entry'] or '—'} · {args.ticks} tick 差异 {b['diff_lines']} 条"
          f"（{b['diff_ticks']}/{args.ticks} 个 tick）· 段名集合一致 {b['region_keys_same'] or '—'}")
        w("")

    w("## 失败分类\n")
    if not summary["fail_classes"]:
        w("无失败。\n")
    else:
        w("| 类别 | 细节 | 数量 | 语料 |")
        w("| --- | --- | --- | --- |")
        for c in summary["fail_classes"]:
            ids = "、".join(c["ids"])
            w(f"| {md_cell(c['kind'], 40)} | {md_cell(c['detail'])} | {c['count']} | {ids} |")
        w("")
        w("### 失败最小复现\n")
        seen = set()
        for c in summary["fail_classes"]:
            key = (c["kind"], c["detail"])
            if key in seen:
                continue
            seen.add(key)
            rep = next(r for r in corpus if r["id"] in c["ids"])
            tgt = src_of[rep["id"]]
            row = dict(rep, src=tgt["src"], ref_path=tgt["ref"] or "",
                       ref_entry_s=hex(tgt["ref_entry"]))
            w(f"**{c['kind']}：{md_cell(c['detail'], 80)}**（{c['count']} 份，例 {rep['id']}）\n")
            w("```")
            w(f"# 原始错误首行：{row['err_first_line'] or '—'}")
            for line in repro_commands(row, args):
                w(line)
            w("```\n")

    w("## 只差 extern（`extern_only`）\n")
    extern_details = summary["extern_only_details"]
    if not extern_details:
        w("无。\n")
    else:
        w(f"**含义**：这 {len(extern_details)} 份**每个 tick 的段哈希 / 页 / `sim_cycle` 全等**、tick 条数相同，"
          "唯一的不同是 extern 序列 —— 内存状态逐 tick 等价，差异来自**参考侧**产物自己的分配器调了"
          " `VirtualAlloc`（我们产物不调）。所以单列一档：**不算「值不对」，不计入差分回退率**；"
          "extern 差异本身照旧记下来供审计。\n")
        w("| id | extern 差异（两边条数与符号名、哪些 tick） |")
        w("| --- | --- |")
        for e in extern_details:
            w(f"| {e['id']} | {md_cell(e['extern'])} |")
        w("")

    w("## 覆盖表（每份一行）\n")
    w("`差异 tick` = `selftest.compare` 报出的差异涉及多少个 tick；它自己会在 ~8 条处收口，"
      "所以差异明细看 `work/tmp/m5/logs/<id>.{ref,ours}.jsonl`。\n")
    w("| id | 装配 | blob B | entry | 差异 tick | 状态 | 失败类别 | 说明 |")
    w("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in corpus:
        asm = "ok" if r["status"] in ASSEMBLED_STATUSES else "fail"
        diff = (f"{r['diff_ticks']}/{r['ticks_ref']}"
                if r["status"] in (STATUS_EQUAL, STATUS_DIFF, STATUS_EXTERN_ONLY) else "—")
        note = r["err_first_line"] or r["first_diff"]
        w(f"| {r['id']} | {asm} | {r['blob_bytes'] or '—'} | {r['entry'] or '—'} | {diff} | "
          f"{r['status']} | {md_cell(r['err_kind'] or '—', 40)} | {md_cell(note)} |")
    w("")
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out))
    except OSError as e:
        fail(f"写不了报告 {rel(path)}：{e}", 2)
    return path


def read_json(path):
    """读一个 JSON；读不到返回 None —— 调用方自己决定「没有」算不算错。"""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except OSError:
        return None
    except ValueError as e:
        fail(f"读不了 {rel(path)}：{e}", 2)


def kind_totals(classes):
    """失败分类 → 按 `err_kind` 计数（对照先看这一层，再看逐类）。"""
    out = {}
    for c in classes:
        out[c["kind"]] = out.get(c["kind"], 0) + c["count"]
    return out


_ID_TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def family_key(kind, detail):
    """摸底口径的分类键：`err_detail` 里的**标识符也抹成 `<id>`**。

    `normalize_detail` 只抹数字与路径，所以同一条消息换个变量名就成了新类
    （`顶层 var registers …` / `顶层 var ram …`）—— 摸底问的是「卡在哪一类」，不是「哪个变量名」：
    不抹标识符就会把 14 份同一形态的失败拆成 8 个 1 份的类。"""
    return (kind or "?", _ID_TOKEN.sub("<id>", detail or "?"))


def families(entries):
    """[(kind, detail, count, ids)] → 按 `family_key` 合并（总数不变）。

    展示用的 `kind` / `detail` 取**首见**那一份的原文 —— 归并键只用来配对，不去当列名；
    一个族里有多种原文时额外记 `variants`，免得「14 份写成 `顶层 var input`」被读成只关于 `input`。"""
    groups = {}
    for kind, detail, count, ids in entries:
        key = family_key(kind, detail)
        g = groups.setdefault(key, {"kind": kind or "?", "detail": detail or "?",
                                    "key": key, "count": 0, "ids": [], "variants": set()})
        g["count"] += count
        g["ids"].extend(ids)
        g["variants"].add((kind, detail))
    for g in groups.values():
        g["variant_count"] = len(g["variants"])
    return sorted(groups.values(), key=lambda c: (-c["count"], c["kind"], c["detail"]))


def survey_families(rows):
    """本次摸底的行 → 失败分类（同族归并）。"""
    return families([(r["err_kind"] or r["status"], r["err_detail"] or r["status"], 1, [r["id"]])
                     for r in rows if r["status"] != STATUS_ASSEMBLED])


def base_round(summary_json):
    """98 份那轮：`(原始汇总, 同族归并的失败分类)`；读不到就是 `(None, None)`。"""
    data = read_json(summary_json)
    if not data:
        return None, None
    return data, families([(c["kind"], c["detail"], c["count"], c["ids"])
                           for c in data["fail_classes"]])


def survey_comparison(classes, base, base_classes):
    """本次目录摸底 vs 98 份（hook 抓的生成代码）的失败分类对照 —— 这是摸底的主要产出。

    口径：两边都取**同族归并**后的类（`family_key`），所以在两轮里形状相同的消息就是同一类；
    98 份那轮的数据来自 `COVERAGE_SUMMARY`。配不上的就是这一轮新的类。"""
    out = []
    w = out.append
    if base is None:
        w(f"- 98 份那轮的汇总不在 `{rel(COVERAGE_SUMMARY)}`（先跑一次 "
          "`python simplexc/tools/m5/coverage.py`）—— 本节留空，其余结论不受影响。\n")
        return out
    base_by_key = {c["key"]: c for c in base_classes}
    mine_keys = {c["key"] for c in classes}
    w(f"98 份那轮（同一口径）：总 {base['total']} · 可装配 {base['assembled']} · "
      f"逐 tick 全等 {base['equal']}（`{rel(COVERAGE_SUMMARY)}`）。\n")

    w("### 按 err_kind\n")
    w("| err_kind | 本次目录 | 98 份 |")
    w("| --- | --- | --- |")
    mine_kind, base_kind = kind_totals(classes), kind_totals(base_classes)
    for kind in sorted(set(mine_kind) | set(base_kind)):
        w(f"| {md_cell(kind, 40)} | {mine_kind.get(kind, 0)} | {base_kind.get(kind, 0)} |")
    w("")

    w("### 逐类配对（标识符抹成 `<id>`）\n")
    w("| 缺口（err_kind / err_detail） | 本次目录 | 98 份 | 两边都有？ |")
    w("| --- | --- | --- | --- |")
    overlap = []
    for c in classes:
        hit = base_by_key.get(c["key"])
        if hit:
            overlap.append((c, hit))
        w(f"| {md_cell(c['kind'], 40)} / {md_cell(c['detail'])} | {c['count']} | "
          f"{hit['count'] if hit else '—'} | {'**是**' if hit else '**否（本轮新增）**'} |")
    w("")
    new_here = [c for c in classes if c["key"] not in base_by_key]
    only_base = [c for c in base_classes if c["key"] not in mine_keys]
    w("- 本轮有、98 份没有的类：" + ("无" if not new_here else "；".join(
        f"{c['kind']}/{md_cell(c['detail'], 60)}（{c['count']} 份）" for c in new_here)))
    w("- 98 份有、本轮没有的类：" + ("无" if not only_base else "；".join(
        f"{c['kind']}/{md_cell(c['detail'], 60)}（{c['count']} 份）" for c in only_base)))
    if not classes:
        w("- 本轮没有失败类。")
    elif not overlap:
        w(f"- **结论：两轮没有一类重合**（本轮 {len(classes)} 类 vs 98 份 {len(base_classes)} 类）"
          " —— 两类语料卡住的不是同一类缺口。")
    else:
        w(f"- **结论：重合 {len(overlap)} 类**：" + "；".join(
            f"{c['kind']}/{md_cell(c['detail'], 60)}（本轮 {c['count']} 份 · 98 份 {b['count']} 份）"
            for c, b in overlap))
    w("")
    return out


def write_survey_report(rows, summary, path, args, targets):
    """目录摸底的人读报告（`--dir` 那条路）：汇总 + 失败分类（带原文首行）+ 与 98 份的对照 + 每份一行。

    **没有差分那一段**：目录语料没有参考产物，判定到「装得出来」为止。"""
    corpus = [r for r in rows if r["scope"] != "baseline"]
    src_of = {t["id"]: t for t in targets}
    total = len([t for t in targets if t["scope"] != "baseline"])
    failed = summary["total"] - summary["assembled"]
    out = []
    w = out.append
    root = args.dir or "（--dir）"
    w(f"# 目录摸底：`{rel(root)}` 的 {total} 份能不能装出来（只装配、不差分）\n")
    w(f"生成 {summary['generated']} · 已完成 {len(corpus)}/{total} 份 · "
      f"{'**中途快照（还没跑完）**' if len(corpus) < total else '跑完'}\n")
    w("判据口径：每份语料只做第 ①步 `build/simplexc_cli.exe --src <子目录>/test.si --out <blob>`；"
      "这个目录没有 manifest、也没有参考产物，所以**不做差分、不生成参考产物**。"
      "错误类别按 CLI 的 catch 分支前缀归类，细节里行号/列号/数字已抹掉（同一类缺口合并计数）。"
      f"注意：没参考产物就分不清「我们缺功能」与「文件本身不是完整程序」—— {total} 份里的失败，"
      "有一部分可能属于后者，要它自己编一遍才知道。\n")
    w(f"重跑：`python simplexc/tools/m5/coverage.py --dir {rel(root)} --no-diff "
      f"--out-dir {rel(args.out_dir)} --report {rel(path)}` · 机器可读：`{rel(args.tsv)}`\n")

    w("## 汇总\n")
    w("| 指标 | 值 |")
    w("| --- | --- |")
    w(f"| 语料总数 | {total}（本次已跑 {summary['total']}） |")
    if summary["total"]:
        w(f"| 可装配 | {summary['assembled']}（{summary['assembled'] / summary['total']:.1%}） |")
        w(f"| 装不出来 | {failed}（{failed / summary['total']:.1%}） |")
    else:
        w("| 可装配 | 0 |")
        w("| 装不出来 | 0 |")
    for s in FAIL_STATUSES:
        n = summary["by_status"].get(s, 0)
        if n:
            w(f"| 其中 {s} | {n} |")
    w(f"| CLI 耗时 | 中位 {summary['cli_ms_median']} ms / 最大 {summary['cli_ms_max']} ms |")
    w(f"| 总墙上时间 | {summary['corpus_wall_seconds']} s |")
    timeouts = [r["id"] for r in corpus if r["status"] == STATUS_CLI_TIMEOUT]
    w(f"| 超时 | {timeouts or '无'} |")
    w("")

    w("## 失败分类\n")
    classes = survey_families(corpus)
    base, base_classes = base_round(COVERAGE_SUMMARY)
    if not classes:
        w("无失败 —— 全部装出来了。\n")
    else:
        w("按 `err_kind`：" + " · ".join(
            f"{k} {v}" for k, v in sorted(kind_totals(classes).items(), key=lambda kv: (-kv[1], kv[0]))) + "\n")
        w("逐类（同一个键 = 同一类；细节里的标识符抹成 `<id>`，见 `family_key`）：\n")
        w("| 类别 | 细节 | 数量 | 语料 |")
        w("| --- | --- | --- | --- |")
        for c in classes:
            variants = f"（{c['variant_count']} 种原文，细节取首见）" if c["variant_count"] > 1 else ""
            w(f"| {md_cell(c['kind'], 40)} | {md_cell(c['detail'])}{variants} | {c['count']} | "
              f"{'、'.join(c['ids'])} |")
        w("")
        w("### 每一类的原文错误首行\n")
        for c in classes:
            rep = next(r for r in corpus if r["id"] in c["ids"])
            blob = rel(os.path.join(args.out_dir, "out", rep["id"] + ".bin"))
            w(f"**{c['kind']}：{md_cell(c['detail'], 80)}**（{c['count']} 份，例 {rep['id']}）\n")
            w("```")
            w(f"# 原始错误首行：{rep['err_first_line'] or '—'}")
            w(f"build/simplexc_cli.exe --src {src_of[rep['id']]['src']} --out {blob}")
            w("```\n")

    w("## 与 98 份（hook 抓的生成代码）失败分类的对照\n")
    w("这一节是本次摸底的主要产出：两轮跑的是**两类不同的语料**（campaign = 关卡自带的手写测试程序，"
      "98 份 = hook 抓的游戏生成代码），比的是它们「卡在哪一类」是否同一批。\n")
    for line in survey_comparison(classes, base, base_classes):
        w(line)

    w("## 覆盖表（每份一行）\n")
    w("| id | 装配 | blob B | entry | cli ms | 状态 | 失败类别 | 原始错误首行 |")
    w("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in corpus:
        asm = "ok" if r["status"] in ASSEMBLED_STATUSES else "fail"
        w(f"| {r['id']} | {asm} | {r['blob_bytes'] or '—'} | {r['entry'] or '—'} | {r['cli_ms']} | "
          f"{r['status']} | {md_cell(r['err_kind'] or '—', 40)} | {md_cell(r['err_first_line'])} |")
    w("")
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out))
    except OSError as e:
        fail(f"写不了报告 {rel(path)}：{e}", 2)
    return path


def main(argv=None):
    # 宿主是 Python：它的 stderr 按**控制台编码**（GBK）输出，而 `selftest.run_host` 按 utf-8 解码
    # —— 中文会变成乱码。让子进程按 utf-8 输出，与调用方的解码口径对齐（CLI 是 C++，不受影响）。
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    ap = argparse.ArgumentParser(description="M5 覆盖表：装配 + 逐 tick 差分；--dir = 目录摸底（只装配）")
    ap.add_argument("--dir", default=None,
                    help="扫一个目录：每个一级子目录里的 test.si 是一份语料（无 manifest、不差分）")
    ap.add_argument("--no-diff", action="store_true",
                    help="只装配不差分；只有跟 --dir 一起用才有意义（目录语料没有参考产物）")
    ap.add_argument("--ids", default=None, help="只跑这些 id（逗号分隔）")
    ap.add_argument("--limit", type=int, default=0, help="只跑前 N 份语料（0 = 全部）")
    ap.add_argument("--ticks", type=int, default=10)
    ap.add_argument("--seed", default="seq", choices=("seq", "xorshift"), help="两遍宿主灌的 #RESET_DATA 图案")
    ap.add_argument("--budget", type=float, default=30.0, help="宿主每 tick 预算（秒），透传 host.py")
    ap.add_argument("--host-retries", type=int, default=4,
                    help="宿主失败（非超时）的重试次数：固定地址撞车、参考产物间歇崩溃都靠它拿稳定结果")
    ap.add_argument("--cli-timeout", type=float, default=300.0)
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    ap.add_argument("--tsv", default=None,
                    help=f"默认 manifest 模式 {rel(DEFAULT_TSV)} · 目录模式 <out-dir>/{SURVEY_TSV}")
    ap.add_argument("--summary-json", default=None,
                    help=f"默认 manifest 模式 {rel(DEFAULT_SUMMARY)} · 目录模式 <out-dir>/{SURVEY_SUMMARY}")
    ap.add_argument("--report", default=None,
                    help=f"默认 manifest 模式 {rel(DEFAULT_REPORT)} · 目录模式 <out-dir>/{SURVEY_REPORT}"
                         "（默认值就落在 work/tmp 下 —— 不显式给就不会盖掉仓库里那 98 份的报告）")
    ap.add_argument("--batch", type=int, default=10, help="每跑完这么多份就落盘一次")
    ap.add_argument("--resume", action="store_true", help="从已有 TSV 里跳过已完成的 id")
    ap.add_argument("--no-baseline", action="store_true", help="不跑 rv32im 基准行")
    args = ap.parse_args(argv)

    if args.no_diff and not args.dir:
        fail("--no-diff 只有跟 --dir 一起才有意义：manifest 里那 98 份有参考产物，差分正是那条路的目的", 2)
    if args.dir:
        args.no_diff = True          # 目录语料没有参考产物 —— 这条路上差分拿不到结果，不必等它现形
    # 目录模式的三个产物默认落在 out-dir 下：仓库里那 98 份的报告不能被一次摸底盖掉。
    if args.tsv is None:
        args.tsv = os.path.join(args.out_dir, SURVEY_TSV) if args.dir else DEFAULT_TSV
    if args.summary_json is None:
        args.summary_json = os.path.join(args.out_dir, SURVEY_SUMMARY) if args.dir else DEFAULT_SUMMARY
    if args.report is None:
        args.report = os.path.join(args.out_dir, SURVEY_REPORT) if args.dir else DEFAULT_REPORT

    if not os.path.exists(CLI):
        raise SystemExit(f"缺 {rel(CLI)}：先跑 python simplexc/tools/build.py simplexc_cli")
    for d in (args.out_dir, dirname_of(args.tsv), dirname_of(args.summary_json), dirname_of(args.report)):
        try:
            os.makedirs(d, exist_ok=True)
        except OSError as e:
            fail(f"建不了输出目录 {d}：{e}", 2)
    selftest = load_selftest()

    if args.dir:
        targets = load_dir_targets(args.dir)
    else:
        targets = load_manifest_targets()
    if args.ids:
        want = {s.strip() for s in args.ids.split(",") if s.strip()}
        targets = [t for t in targets if t["id"] in want]
        if not args.dir and not args.no_baseline and "rv32im" in want:
            targets.append(rv32im_target())
    else:
        if args.limit:
            targets = targets[:args.limit]
        if not args.dir and not args.no_baseline:
            targets = targets + [rv32im_target()]
    write = write_survey_report if args.dir else write_report

    rows = read_tsv(args.tsv) if args.resume else []
    done_ids = {r["id"] for r in rows}
    todo = [t for t in targets if t["id"] not in done_ids]
    what = f"目录摸底（{rel(args.dir)}）" if args.dir else "M5 覆盖表"
    print(f"{what}：{len(targets)} 个目标，本次跑 {len(todo)} 个"
          f"{'（--resume 跳过已完成 ' + str(len(done_ids)) + ' 个）' if done_ids else ''}", flush=True)

    for i, tgt in enumerate(todo, 1):
        row = run_target(tgt, selftest, args)
        rows.append(row)
        print(f"[{i}/{len(todo)}] {row['scope']}/{row['id']} {row['status']} "
              f"rc={row['cli_rc'] or '—'} {row['cli_ms']}ms blob={row['blob_bytes']}B "
              f"entry={row['entry'] or '—'} diff={row['diff_lines'] if row['status'] in (STATUS_EQUAL, STATUS_DIFF, STATUS_EXTERN_ONLY) else '—'}"
              + (f" · {row['err_kind']}: {row['err_first_line']}" if row["status"] != STATUS_EQUAL else ""),
              flush=True)
        if i % args.batch == 0 or i == len(todo):
            summary = aggregate(rows, args.no_diff)
            write_tsv(rows, args.tsv)
            try:
                with open(args.summary_json, "w", encoding="utf-8", newline="\n") as f:
                    json.dump(summary, f, ensure_ascii=False, indent=2)
                    f.write("\n")
            except OSError as e:
                fail(f"写不了汇总 JSON {rel(args.summary_json)}：{e}", 2)
            write(rows, summary, args.report, args, targets)
            print(f"  ── checkpoint：{summary_line(summary)}", flush=True)

    summary = aggregate(rows, args.no_diff)
    write_tsv(rows, args.tsv)
    try:
        with open(args.summary_json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(summary, f, ensure_ascii=False, indent=2)
            f.write("\n")
    except OSError as e:
        fail(f"写不了汇总 JSON {rel(args.summary_json)}：{e}", 2)
    write(rows, summary, args.report, args, targets)
    print("汇总：" + summary_line(summary))
    for c in summary["fail_classes"]:
        print(f"  失败分类 {c['count']:>2} × {c['kind']}/{c['detail']}  例 {c['ids'][0]}")
    print(f"机器可读 {rel(args.tsv)} · 报告 {rel(args.report)}")
    return 0


def dirname_of(path):
    return os.path.dirname(os.path.abspath(path))


if __name__ == "__main__":
    reconfigure = getattr(sys.stdout, "reconfigure", None)
    if reconfigure is not None:                              # 控制台常是 GBK，中文会花
        reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
