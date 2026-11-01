#!python3
r"""stdlib 全量编译的尺子：现在能编到第几行、卡在哪类构造、离 1503 有多远。

为什么按行号区段分类、不看错误消息：消息文本是编译器实现的副产物（同一个构造换个措辞就归错类），
行号是语料的事实。`corpus/stdlib_prefix.si` 按构造切成首尾相接的区段，表就在下面 `REGIONS`；
改语料就得同步改它，否则 `OTHER` 的「构造数」会失真（行数变了 `EXPECTED_LINES` 会在 stderr 提醒）。

局限（必读）
------------
`build/simplexc_cli.exe` 是 **fail-fast** 的：sema 遇到第一个不支持的构造就抛错，一次只报一处
（`--list-unsupported` 已不能收清单，见 `cli/simplexc_cli.cc`）。所以：

- 「编译到的行」= 第一处错误的行号 − 1，本工具只能报这一处，后面的区段是否还有问题它不知道；
  错误不带行号时（`声明层错误` / `内部错误`）这一格是 null（JSON）或「行号未知」（表），不冒充 0；
- 「构造数」是**静态统计**（在该区段源码上跑 `COUNT_PATTERNS`），不是编过的数量。

用法
----
    python simplexc/tools/stdlib_check.py                   # 进度表（默认编 corpus/stdlib_prefix.si）
    python simplexc/tools/stdlib_check.py --json            # 同上，机器读（纯 ASCII，中文转义）
    python simplexc/tools/stdlib_check.py --json > work/tmp/stdlib_check/prev.json
    python simplexc/tools/stdlib_check.py --compare work/tmp/stdlib_check/prev.json   # 推进了几行

`--compare` 只读旧结果的 `reach_line`；没有旧结果就跳过，不算失败。
`--json` 的内容与进度表相同，但纯 ASCII（中文转义成 \uXXXX），这样 `json.load(open(path))`
在默认 GBK 的 locale 下也能读（UTF-8 中文会被 gbk 解错）；`--json` 下 stdout 只有 JSON，
对比结果走 stderr。

退出码：0 = 全量编过；1 = 有编译错误（已算出行号）；2 = 工具自身失败
（找不到 CLI / 读不了源 / 错误消息格式不认识 / 区间表行号越界）。

区间表来源（逐条可核对；在 `tc-toolkit/` 下跑，`f=simplexc/corpus/stdlib_prefix.si`）
------------------------------------------------------------------------------
    枚举              grep -n '^type .*Enum\[' f                     → 17
    类型别名          grep -n '^type ' f                           → 17,38,39,40,41,42,43（去掉枚举那行 = 6）
    @Type/@Any/@Size  grep -nE '@(Type|Any|Size)' f                → 45,483,491,580,883,887,899,954,968,974,997,1001,
                                                                      1007,1009,1084,1276,1299,1305,1497（含函数体内部的引用）
    asm               grep -n '^[[:space:]]*asm ' f                 → 50,101,145,151,188,202,567（50/101 在 /* */ 注释里）
    运算符重载        grep -nE '^(unary|binary) ' f                 → 168,172,176,180,974,997,1098,1104,1324,1330,1394,1400,1406,1412,1418,1424,1430,1436,1442
    kernel32          grep -n 'kernel32' f                         → 251,254,259,269,280,291,534,540,542,549,555（534/540 是注释）
    $ 形参            grep -nE '\$[A-Za-z_0-9]+ *:' f               → 172,180,451,483,491,1007,1009,1041,1084,
                                                                      1104,1412,1418,1481（数「带类型标注的 $ 形参」）
    索引语法          grep -nE '^(index_read|index_write) ' f        → 1312,1318

区段按**起始行**切：每个区段从自己的起始行到下一个区段的起始行−1，最后一段到文末。
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import json
import os
import re
import subprocess
import sys
import unicodedata
from dataclasses import dataclass
from typing import NoReturn

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_SRC = os.path.join("simplexc", "corpus", "stdlib_prefix.si")
DEFAULT_CLI = os.path.join("build", "simplexc_cli.exe")
EXPECTED_LINES = 1503                    # M5 的验收边界：整份 stdlib 前缀
CLI_TIMEOUT_S = 900
ERROR_CLIP = 56                          # 「首个错误」列的显示宽度上限

OTHER = "其他"


@dataclass(frozen=True)
class Region:
    """一个区段：`construct` 从 `start` 行起，到下一个区段起始行−1 为止。"""

    construct: str
    start: int
    evidence: str


REGIONS: tuple[Region, ...] = (
    Region(OTHER, 1, "const 常量：grep -n '^const ' → 2-10,12-15"),
    Region("枚举", 17, "grep -n '^type .*Enum\\[' → 17（Char Enum[...] 跨 20 行）"),
    Region("类型别名", 38, "grep -n '^type ' → 38-43（Time/Seed/File/Ptr/Str/Cstring）"),
    Region("@Type/@Any/@Size", 45, "grep -nE '@Any' → 45（print_using_stack(value: @Any)）；50-140 是注释"),
    Region("asm", 143, "grep -n '^[[:space:]]*asm ' → 145,151（breakpoint 内的 x86_64/aarch64 两段）"),
    Region("运算符重载", 168, "grep -nE '^(unary|binary) ' → 168,172,176,180（172/180 另带 $ 形参）"),
    Region(OTHER, 186, "def memory_copy/clear/copy_reverse；asm 在 188,202"),
    Region("kernel32", 243, "call_conv 243-249 + grep -n 'kernel32' → 251(extern),254,259,269,280,291"),
    Region(OTHER, 297, "arena/内存管理 def；451 的 __append 另带 $ 形参"),
    Region("$ 形参", 483, "grep -n 'dot .*(\\$' → 483,491（dot push/pop($array: Arr[@Type])）"),
    Region(OTHER, 515, "assert/output/sleep/get_time/dot write；536-560 调 kernel32"),
    Region("asm", 565, "grep -n '^[[:space:]]*asm ' → 567（get_cycle_count 内 rdtsc）"),
    Region("@Type/@Any/@Size", 580, "grep -n ': @Size' → 580（def hex(num: @Size)）"),
    Region(OTHER, 617, "big_int_* 一批 def（874-880 是空体桩）"),
    Region("@Type/@Any/@Size", 883, "grep -n '@Type' → 883（def random(type: @Type) @Type）"),
    Region(OTHER, 906, "def log10"),
    Region("@Type/@Any/@Size", 954, "grep -n '@Type' → 954,968（dot contains / dot in）"),
    Region("运算符重载", 974, "grep -nE '^binary ' → 974,997（Arr == / !=）"),
    Region("$ 形参", 1007, "grep -n 'def .*(\\$' → 1007,1009（quick_sort / partition）"),
    Region(OTHER, 1092, "dot len(input: Str)"),
    Region("运算符重载", 1098, "grep -nE '^binary ' → 1098,1104（Str + / +=；1104 另带 $ 形参）"),
    Region(OTHER, 1110, "str/int/hex/bin/print 一批 def"),
    Region("索引语法", 1312, "grep -nE '^(index_read|index_write) ' → 1312,1318"),
    Region("运算符重载", 1324, "grep -nE '^binary ' → 1324,1330（Str == / !=）"),
    Region(OTHER, 1336, "dot high + dot sec/ms/us/ns/min/hour/day（Time 换算）"),
    Region("运算符重载", 1394, "grep -nE '^binary ' → 1394-1442 共 9 条（Time 运算符；1412/1418 另带 $ 形参）"),
    Region(OTHER, 1448, "def exit() / dot write / xorshift"),
    Region("$ 形参", 1475, "grep -n 'dot .*(\\$' → 1481（dot next($s: Seed)）；1475 是顶层 var global_seed"),
    Region(OTHER, 1497, "def sample(array: Arr[@Any]) @Any"),
)

# 「该区段构造数」怎么数：$ 形参 数带类型标注的 `$名: 类型` 形参，kernel32 数调用点，其余按行首关键字。
# 其他 数的是顶层声明（`^[A-Za-z_]` 起首的行，`}` 之类不算）。
COUNT_PATTERNS: dict[str, re.Pattern[str]] = {
    OTHER: re.compile(r"^[A-Za-z_]"),
    "枚举": re.compile(r"^type\s+\w+\s+Enum\["),
    "类型别名": re.compile(r"^type\s+(?!\w+\s+Enum\[)"),
    "运算符重载": re.compile(r"^(?:unary|binary)\b"),
    "索引语法": re.compile(r"^(?:index_read|index_write)\b"),
    "$ 形参": re.compile(r"\$\w+\s*:"),
    "asm": re.compile(r"^\s*asm\b"),
    "kernel32": re.compile(r"kernel32\."),
    "@Type/@Any/@Size": re.compile(r"@(?:Type|Any|Size)\b"),
}

# 失败消息的形态（cli/simplexc_cli.cc 的 catch 分支）：带 stage/detail+行号的、unsupported、
# 语法/词法（行列两段）、带 stage/detail 但没行号的、以及裸消息（声明层错误、内部错误）。
RE_STAGED = re.compile(r"^编译失败：(?P<stage>[^/\s]+)/(?P<detail>.+?) 第 (?P<line>\d+) 行：(?P<msg>.*)$")
RE_UNSUPPORTED = re.compile(r"^UNSUPPORTED: (?P<msg>.*?) @ 第 (?P<line>\d+) 行$")
RE_SYNTAX = re.compile(r"^语法错误：第 (?P<line>\d+) 行 第 \d+ 列：(?P<msg>.*)$")
RE_LEX = re.compile(r"^词法错误：第 (?P<line>\d+) 行 第 \d+ 列：(?P<msg>.*)$")
RE_STAGED_NO_LINE = re.compile(r"^编译失败：(?P<stage>[^/\s]+)/(?P<detail>.+?)：(?P<msg>.*)$")
RE_NO_LINE = re.compile(r"^(?:编译失败|声明层错误|内部错误)：(?P<msg>.*)$")


def fail(message: str) -> NoReturn:
    """工具自身失败：消息走 stderr，退出码 2 —— 与 CLI 的 rc（0/1）区分开。"""
    print(message, file=sys.stderr)
    raise SystemExit(2)


def utf8_stdout() -> None:
    """控制台是 GBK（chcp 936），中文会花掉；判据输出要能直接贴。"""
    for stream in (sys.stdout, sys.stderr):
        rec = getattr(stream, "reconfigure", None)
        if rec:
            with contextlib.suppress(Exception):
                rec(encoding="utf-8", errors="replace")


def rel(path: str) -> str:
    """输出里一律用相对 tc-toolkit/ 的路径。"""
    try:
        r = os.path.relpath(path, ROOT)
    except ValueError:
        return path
    return path if r.startswith("..") else r


def resolve(path: str) -> str:
    """相对路径按 tc-toolkit/ 解释，不受当前目录影响。"""
    return path if os.path.isabs(path) else os.path.join(ROOT, path)


def parse_error(stderr_text: str) -> dict | None:
    """从 stderr 里认出第一处失败；认不出返回 None（调用方按工具自身失败处理）。"""
    for raw in stderr_text.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = RE_STAGED.match(line)
        if m:
            return {"stage": m["stage"], "detail": m["detail"],
                    "line": int(m["line"]), "message": m["msg"]}
        m = RE_UNSUPPORTED.match(line)
        if m:
            return {"stage": "lower", "detail": "UNSUPPORTED",
                    "line": int(m["line"]), "message": m["msg"]}
        m = RE_SYNTAX.match(line)
        if m:
            return {"stage": "parse", "detail": "语法错误",
                    "line": int(m["line"]), "message": m["msg"]}
        m = RE_LEX.match(line)
        if m:
            return {"stage": "lex", "detail": "词法错误",
                    "line": int(m["line"]), "message": m["msg"]}
        m = RE_STAGED_NO_LINE.match(line)
        if m:
            return {"stage": m["stage"], "detail": m["detail"],
                    "line": None, "message": m["msg"]}
        m = RE_NO_LINE.match(line)
        if m:
            return {"stage": "parse", "detail": "无行号", "line": None, "message": m["msg"]}
    return None


def run_cli(cli: str, src: str) -> tuple[int, str]:
    """编一次源；返回 (rc, stderr)。文件不存在/超时按工具失败抛 SystemExit(2)。"""
    try:
        p = subprocess.run([cli, "--src", src], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=CLI_TIMEOUT_S)
    except FileNotFoundError:
        fail(f"工具失败：找不到编译器 {cli}（先 python simplexc/tools/build.py cli）")
    except subprocess.TimeoutExpired:
        fail(f"工具失败：{rel(cli)} 超过 {CLI_TIMEOUT_S} s 没返回")
    return p.returncode, p.stderr


def read_source_lines(src: str) -> list[str]:
    try:
        with open(src, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError as exc:
        fail(f"工具失败：读不了 {src}：{exc}")


def spans(total: int) -> list[tuple[Region, int]]:
    """把「起始行」展开成 (区段, 结束行)；行号越界说明区间表与语料脱节。"""
    out = []
    for i, region in enumerate(REGIONS):
        end = REGIONS[i + 1].start - 1 if i + 1 < len(REGIONS) else total
        if region.start < 1 or region.start > total or end < region.start:
            fail(f"工具失败：区间表第 {i + 1} 段（{region.construct} @ {region.start}）"
                 f"越出源码范围 1..{total}；语料变了就同步改 REGIONS")
        out.append((region, end))
    return out


def count_construct(lines: list[str], construct: str, start: int, end: int) -> int:
    pattern = COUNT_PATTERNS[construct]
    return sum(1 for line in lines[start - 1:end] if pattern.search(line))


def region_rows(lines: list[str], first_error: dict | None, reach: int | None) -> list[dict]:
    rows = []
    err_line = None if first_error is None else first_error["line"]
    err_text = "" if first_error is None else (
        f"第 {first_error['line']} 行 {first_error['stage']}/{first_error['detail']}："
        f"{first_error['message']}")
    for region, end in spans(len(lines)):
        if reach is None:
            reached = "未知"
        else:
            reached = str(min(end, reach))
        hit = err_text if err_line is not None and region.start <= err_line <= end else ""
        rows.append({
            "construct": region.construct,
            "start": region.start,
            "end": end,
            "count": count_construct(lines, region.construct, region.start, end),
            "reach": None if reach is None else min(end, reach),
            "reach_text": reached,
            "first_error": hit,
            "evidence": region.evidence,
        })
    return rows


def blocking_row(rows: list[dict]) -> dict | None:
    for row in rows:
        if row["first_error"]:
            return row
    return None


def display_width(text: str) -> int:
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in text)


def clip(text: str, width: int) -> str:
    if display_width(text) <= width:
        return text
    out, used = [], 0
    for ch in text:
        w = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
        if used + w > width - 1:
            break
        out.append(ch)
        used += w
    return "".join(out) + "…"


def pad(text: str, width: int, right: bool = False) -> str:
    gap = " " * max(0, width - display_width(text))
    return gap + text if right else text + gap


def print_table(rows: list[dict]) -> None:
    header = ("区段", "起始行", "结束行", "构造数", "编译到的行", "首个错误")
    body = [(r["construct"], str(r["start"]), str(r["end"]), str(r["count"]),
             r["reach_text"], clip(r["first_error"] or "—", ERROR_CLIP)) for r in rows]
    widths = [max(display_width(header[i]), *(display_width(row[i]) for row in body))
              for i in range(len(header))]
    right_cols = {1, 2, 3, 4}
    print("  ".join(pad(c, widths[i], i in right_cols) for i, c in enumerate(header)))
    print("  ".join("-" * w for w in widths))
    for row in body:
        print("  ".join(pad(c, widths[i], i in right_cols) for i, c in enumerate(row)))


def summarize(payload: dict) -> None:
    total, reach = payload["total_lines"], payload["reach_line"]
    print(f"源：{payload['src']}（{total} 行）")
    print(f"CLI：{payload['cli']}（rc={payload['cli_rc']}）")
    if payload["ok"]:
        print(f"编译到：第 {total} 行 / {total}（100%）—— 全量编过")
        return
    err = payload["first_error"]
    where = "行号未知" if err is None or err["line"] is None else f"第 {err['line']} 行"
    if reach is None:
        print(f"编译到：行号未知 / {total}（第一处失败没带行号，量不出进度）")
    else:
        print(f"编译到：第 {reach} 行 / {total}（{100.0 * reach / total:.1f}%）"
              f"，第 {reach + 1} 行起还没过")
    if err is not None:
        print(f"第一处失败：{where} [{err['stage']}/{err['detail']}] {err['message']}")
    row = blocking_row(payload["regions"])
    if row is not None:
        print(f"卡在构造：{row['construct']}（区段 {row['start']}–{row['end']}，"
              f"该区段含 {row['count']} 个）")
    print("局限：编译器 fail-fast，只报第一处；「构造数」是静态统计，不是编过的数量。")


def print_compare(old_path: str, payload: dict, stream) -> None:
    """报「推进了几行」；没有旧结果就跳过（不算失败而是信息）。"""
    if not os.path.isfile(old_path):
        print(f"对比：跳过（没有旧结果 {rel(old_path)}）", file=stream)
        return
    try:
        with open(old_path, encoding="utf-8") as f:
            old = json.load(f)
        old_reach = old["reach_line"]
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"对比：跳过（读不懂旧结果 {rel(old_path)}：{exc}）", file=stream)
        return
    if not isinstance(old_reach, int) or isinstance(old_reach, bool):
        print(f"对比：跳过（旧结果没记到行号：reach_line={old_reach!r}）", file=stream)
        return
    new_reach = payload["reach_line"]
    if new_reach is None:
        print(f"对比：跳过（新结果的行号未知，量不出推进）；旧第 {old_reach} 行", file=stream)
        return
    delta = new_reach - old_reach
    arrow = "+" if delta > 0 else ""
    print(f"对比：{rel(old_path)} 旧第 {old_reach} 行 → 新第 {new_reach} 行，推进 {arrow}{delta} 行",
          file=stream)


def build_payload(lines: list[str], src: str, cli: str, rc: int, stderr_text: str) -> dict:
    total = len(lines)
    if total != EXPECTED_LINES:
        print(f"注意：{rel(src)} 有 {total} 行，与区间表假设的 {EXPECTED_LINES} 行不一致；"
              f"最后一区段与「构造数」可能失真", file=sys.stderr)

    ok = rc == 0
    error = None
    if ok:
        reach = total
    else:
        if rc == 2:
            fail(f"工具失败：CLI 退出码 2（参数或读文件问题）：{stderr_text.strip()[:400]}")
        error = parse_error(stderr_text)
        if error is None:
            fail(f"工具失败：CLI 退出码 {rc}，stderr 认不出错误格式："
                 f"{stderr_text.strip()[:400]!r}")
        line = error["line"]
        # 认不出行号（`声明层错误`/`内部错误`）时 reach 是 None：不许拿 0 冒充「一行都没过」。
        reach = None if line is None else max(0, line - 1)

    rows = region_rows(lines, error, reach)
    return {
        "src": rel(src),
        "cli": rel(cli),
        "cli_rc": rc,
        "total_lines": total,
        "expected_lines": EXPECTED_LINES,
        "ok": ok,
        "reach_line": reach,
        "reach_percent": None if reach is None else round(100.0 * reach / total, 1),
        "first_error": error,
        "blocking_construct": (blocking_row(rows) or {}).get("construct"),
        "regions": rows,
        "generated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "limitations": "编译器 fail-fast：一次只报第一处错误；构造数是静态统计，不是编过的数量。",
    }


def main(argv: list[str] | None = None) -> int:
    utf8_stdout()            # 必须在 parse_args 之前：--help 也在 parse_args 里打，docstring 有 −/→
    # --help 直接印完整 docstring：区间表的 grep 出处就在里面，下一个人用 --help 就能核对。
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", default=DEFAULT_SRC, help=f"默认 {DEFAULT_SRC}")
    parser.add_argument("--json", action="store_true", help="输出 JSON（内容与进度表相同）")
    parser.add_argument("--compare", metavar="OLD.json", help="与上次 --json 结果比，报推进了几行")
    args = parser.parse_args(argv)

    src, cli = resolve(args.src), resolve(DEFAULT_CLI)
    lines = read_source_lines(src)       # 先读源：源或路径写错时直接报「读不了」，不用惊动 CLI
    rc, stderr_text = run_cli(cli, src)
    payload = build_payload(lines, src, cli, rc, stderr_text)

    if args.json:
        # stdout 只放 JSON（否则 `--json > f` 落盘的文件带中文尾巴，json.load 会报编码错）；
        # ensure_ascii 默认开：输出纯 ASCII，`json.load(open(path))` 在 GBK 默认 locale 下也读得动。
        json.dump(payload, sys.stdout, indent=2)
        sys.stdout.write("\n")
        if args.compare:
            print_compare(args.compare, payload, sys.stderr)
    else:
        summarize(payload)
        print()
        print_table(payload["regions"])
        if args.compare:
            print()
            print_compare(args.compare, payload, sys.stdout)

    return 0 if payload["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
