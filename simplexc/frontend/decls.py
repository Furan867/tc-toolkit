#!python3
"""M3.0 前端第一步：词法掩码 → 顶层声明表 → 花括号配平 → 名字级可达性。

只回答四件事，别的都不做：
  1. 有哪些声明（`type` / `const` / `var` / `def` / 带标签的块 / 顶层 `#if`），各自占哪几行；
  2. 每个 `def` 的 `{}` 是否把体完整包住（只配平，**不解析体**）；
  3. 每个 `def` 从顶层入口（`run_sim` 标签块等）经调用图按名字是否可达；
  4. 顶层有什么东西不认识（`未支持` 表，不猜、不炸）。

输出表的列（TSV，首行是表头）：`kind / name / lo-hi / params / ret / body_parsed /
reachable / depth / enclosing / note`。两个容易误读的列：
  * `body_parsed` —— 本阶段函数体**只配平不解析**，所以非空体一律 `否`；只有空体（`{ }`）是 `是`
    （没东西可解析）。它不是「体是否闭合」，闭合与否由 `lo-hi` 与错误码回答。
  * `reachable` —— 两个口径：默认 `strict` 只从顶层语句与 `tc_main` 出发（参考实现的行为，
    见 `docs/notes/2026-10-18-m3-0-scope.md` §7.1）；`--roots lenient` 复现 dossier 里
    `reachability.py` 的口径（把 `def` 声明头也当成调用），两者差 9 个名字，见
    `docs/notes/2026-10-18-frontend-decls.md`。

为什么体只配平：`docs/status.md`「下一步」第 2 条 —— 参考实现按需分析函数体，
不可达 def 只跳过。所以体里那类毛病（`rv32im.si:154` 多一个 `)`）不该让本工具失败；
反过来，花括号不配平是**文件级结构**问题，必须报错并指出行号。

只读源码文本，不编译、不加载 DLL、不跑游戏。只依赖标准库。

用法：
    python simplexc/frontend/decls.py --src simplexc/corpus/rv32im/rv32im.si
    python simplexc/frontend/decls.py --src <file> --json out.json --quiet
    python simplexc/frontend/decls.py --src <file> --roots lenient      # 复现 dossier 口径

stdout = 声明表（TSV，一行一个声明）；stderr = 统计、警告、未支持项。
花括号不配平 → 退出码 1，stderr 指出行号；文件读不了 → 退出码 2。
"""
import argparse
import contextlib
import json
import os
import re
import sys
from dataclasses import dataclass, field

# Windows 下 stdout 走管道默认 GBK，含中文时不 reconfigure 会抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    _rec = getattr(_stream, "reconfigure", None)
    if _rec is not None:
        with contextlib.suppress(ValueError, OSError):
            _rec(encoding="utf-8", errors="replace")


# ---------------------------------------------------------------- 词法掩码

LITERAL_DELIMS = '"\'`'


def _scan_literal(text, i):
    """从 text[i]（字面量起始定界符）扫到闭定界符之后；遇换行即截断（未闭合字面量不跨行）。"""
    delim = text[i]
    j, n = i + 1, len(text)
    while j < n:
        if text[j] == '\\':
            j += 2
            continue
        if text[j] == delim:
            return j + 1
        if text[j] == '\n':
            return j
        j += 1
    return n


def _blank_keep_interp(seg):
    """模板字符串（反引号）：文字部分抹成空格，`{expr}` 里是代码，原样留下。"""
    out = ['\n' if ch == '\n' else ' ' for ch in seg]
    k = 0
    while k < len(seg):
        if seg[k] != '{':
            k += 1
            continue
        depth, m = 1, k + 1
        while m < len(seg) and depth:
            depth += (seg[m] == '{') - (seg[m] == '}')
            m += 1
        if depth:
            break
        out[k:m] = seg[k:m]
        k = m
    return ''.join(out)


def mask_source(text):
    """一次扫描产出两份等长文本（换行位置不变，行号一直对得上原文）。

    raw  = 注释 → 空格，字面量原样保留（用它判「这行是不是只有注释」）
    code = 注释 → 空格，字面量文字 → 空格（结构、名字、花括号一律只看它）
    """
    raw, code = [], []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            raw.append(' ' * (j - i))
            code.append(' ' * (j - i))
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            blank = ''.join('\n' if ch == '\n' else ' ' for ch in text[i:j])
            raw.append(blank)
            code.append(blank)
            i = j
        elif c in LITERAL_DELIMS:
            j = _scan_literal(text, i)
            seg = text[i:j]
            raw.append(seg)
            code.append(_blank_keep_interp(seg) if c == '`' else ' ' * (j - i))
            i = j
        else:
            raw.append(c)
            code.append(c)
            i += 1
    return ''.join(raw), ''.join(code)


def split_lines(text):
    lines = text.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    return [ln.rstrip('\r') for ln in lines]


# ---------------------------------------------------------------- 声明识别

DEF_RE = re.compile(r'^([ \t]*)def[ \t]+(\w+)[ \t]*\(')
TYPE_RE = re.compile(r'^[ \t]*type[ \t]+(\w+)[ \t]+(Enum|Struct)\b')
CONST_RE = re.compile(r'^[ \t]*const[ \t]+(#\w+)[ \t]*=')
VAR_RE = re.compile(r'^[ \t]*var[ \t]+(\w+)[ \t]*=')
LABEL_RE = re.compile(r'^[ \t]*(\w+)[ \t]*:[ \t]*(?:while|for|loop)\b')
PPIF_RE = re.compile(r'^[ \t]*#(if|else|elif)\b')


@dataclass
class Decl:
    kind: str                     # type | const | var | def | label | pp_if
    name: str
    lo: int
    hi: int = 0
    depth: int = 0                # 声明起始处的花括号深度（0 = 文件顶层）
    enclosing: str = "-"          # 最近的外层声明名
    params: str = "-"
    ret: str = "-"
    body: str = "none"            # none | empty | braced
    note: str = "-"
    reachable: str = "-"
    parent: int = -1
    children: list = field(default_factory=list)


def _parse_def_tail(code, open_idx):
    """code[open_idx] == '('；返回 (params, ret, 体是否在本行开 '{')。头行没闭合 '(' 时 ret 记 '?'。"""
    depth, j = 0, open_idx
    while j < len(code):
        ch = code[j]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                break
        j += 1
    if depth != 0:
        return code[open_idx + 1:].strip() or "-", "?", False
    params = code[open_idx + 1:j].strip() or "-"
    rest = code[j + 1:]
    brace = rest.find('{')
    ret = (rest[:brace] if brace >= 0 else rest).strip() or "-"
    return params, ret, brace >= 0


def _paren_delta(text):
    return text.count('(') - text.count(')')


def _match_decl(code, raw, ln_no, depth, open_decls):
    """本行是否起一个声明。open_decls 只用于判「是不是声明层」与外层名。"""
    at_decl_level = all(d.kind == 'pp_if' for d in open_decls)
    enclosing = open_decls[-1].name if open_decls else "-"

    m = PPIF_RE.match(code)
    if m and at_decl_level:
        # 只有顶层（或顶层 #if 里）的 #if 算声明级块；函数体里的 #if 属于体，不记
        cond = raw.strip().rstrip('{').strip()
        return Decl('pp_if', cond, ln_no, depth=depth, enclosing=enclosing,
                    note="#" + m.group(1))

    m = DEF_RE.match(code)
    if m:
        params, ret, has_brace = _parse_def_tail(code, m.end() - 1)
        d = Decl('def', m.group(2), ln_no, depth=depth, enclosing=enclosing,
                 params=params, ret=ret)
        d.note = "nested" if enclosing != "-" else "-"
        d.body = "braced" if has_brace else "none"
        return d

    if not at_decl_level:
        return None

    m = TYPE_RE.match(code)
    if m:
        d = Decl('type', m.group(1), ln_no, depth=depth, enclosing=enclosing)
        d.note = m.group(2)
        return d

    m = CONST_RE.match(code)
    if m:
        d = Decl('const', m.group(1), ln_no, depth=depth, enclosing=enclosing)
        return d

    m = VAR_RE.match(code)
    if m:
        d = Decl('var', m.group(1), ln_no, depth=depth, enclosing=enclosing)
        return d

    m = LABEL_RE.match(code)
    if m:
        d = Decl('label', m.group(1), ln_no, depth=depth, enclosing=enclosing)
        return d

    return None


def scan(code_lines, raw_lines):
    """行级结构扫描：识别声明、配平花括号、记下未支持项。返回 (decls, errors, unsupported)。"""
    decls, errors, unsupported = [], [], []
    depth = 0
    open_braces = []        # 未闭合 '{' 所在行号
    open_decls = []         # 已开体、还没收口的声明

    for ln_no, code in enumerate(code_lines, 1):
        d0 = depth
        raw = raw_lines[ln_no - 1]
        started = _match_decl(code, raw, ln_no, d0, open_decls)

        # 花括号配平（字面量与注释已被掩码，所以只剩结构性的 {}）
        for ch in code:
            if ch == '{':
                open_braces.append(ln_no)
                depth += 1
            elif ch == '}':
                if not open_braces:
                    errors.append(f"第 {ln_no} 行：多余的 '}}'（此处没有未闭合的 '{{'）")
                else:
                    open_braces.pop()
                    depth -= 1

        if started is not None:
            decls.append(started)
            if len(decls) > 1 and started.enclosing != "-":
                for idx in range(len(decls) - 2, -1, -1):
                    if decls[idx].name == started.enclosing and decls[idx].depth < d0:
                        started.parent = idx
                        decls[idx].children.append(len(decls) - 1)
                        break
            if depth > d0:
                open_decls.append(started)
            else:
                started.hi = ln_no

        # 收口：本行结束后回到（或越过）声明起始深度的，说明体已闭合
        while open_decls and depth <= open_decls[-1].depth:
            open_decls.pop().hi = ln_no

        # 未支持：声明层上既不是声明、也不是单独的闭合括号/空行/注释
        if (not open_decls and started is None and depth == 0
                and code.strip() and not re.fullmatch(r'[}\])]+[ \t]*;?', code.strip())):
            unsupported.append((ln_no, raw.strip()))

    if open_braces:
        if len(open_braces) == 1:
            where = f"第 {open_braces[0]} 行"
        else:
            head = ", ".join(str(x) for x in open_braces[:5])
            more = "" if len(open_braces) <= 5 else " 等"
            where = f"起始行 {head}{more}（第一个在第 {open_braces[0]} 行，最近一个在第 {open_braces[-1]} 行）"
        errors.append(f"花括号未配平：文件结束时还有 {len(open_braces)} 个 '{{' 未闭合，{where}")

    for d in decls:
        d.hi = d.hi or d.lo

    return decls, errors, unsupported


# ---------------------------------------------------------------- 体判空与括号体检

def _body_is_empty(code_lines, d):
    """体里除空白外没有别的东西 → 空体（`{ }` / `{\\n}`）。只掩码后看字符。"""
    if d.kind != 'def' or d.body != 'braced':
        return False
    text = "".join(code_lines[d.lo - 1:d.hi])
    open_at = text.find('{')
    close_at = text.rfind('}')
    if open_at < 0 or close_at < open_at:
        return False
    return not text[open_at + 1:close_at].strip()


# ---------------------------------------------------------------- 可达性

CALL_RE = re.compile(r"(?<![\w.])([A-Za-z_]\w*)[ \t]*\(")
NOT_CALL = {"if", "elif", "else", "while", "for", "switch", "case", "return", "def",
            "let", "var", "const", "type", "and", "or", "not", "in"}
# 名字级匹配，不做重载解析：漏判调用 = 把活的当死的（危险），多判 = 把死的当活的（安全）。


def span_lines(d, decls, include_nested=False):
    """声明的行；默认去掉嵌套声明的行（嵌套 def 的体不算外层调出来的调用）。"""
    lines = set(range(d.lo, d.hi + 1))
    if include_nested:
        return lines
    for ci in d.children:
        lines -= set(range(decls[ci].lo, decls[ci].hi + 1))
    return lines


def own_lines(d, decls):
    return span_lines(d, decls)


def calls_in(lines, code_lines, skip_def_header=True):
    names = set()
    for ln in sorted(lines):
        line = code_lines[ln - 1]
        if not line:
            continue
        for m in CALL_RE.finditer(line):
            if skip_def_header and line[:m.start()].rstrip().endswith('def'):
                continue                      # 声明头 `def name(` 不是调用
            n = m.group(1)
            if n not in NOT_CALL:
                names.add(n)
    return names


def strict_root_lines(decls, extra_root_lines):
    """真入口：顶层语句（标签块 / 顶层 var 初始化）+ tc_main。体只在被调用时才看。"""
    lines = set(extra_root_lines)
    for d in decls:
        if d.kind in ('label', 'var', 'const', 'pp_if'):
            lines |= own_lines(d, decls)
    for d in decls:
        if d.kind == 'def' and d.name == 'tc_main':
            lines |= own_lines(d, decls)
    return lines


def lenient_root_lines(decls, n_lines):
    """复现 dossier `reachability.py` 的口径：任何「不在 def 体里」的行都算顶层。

    它的两个副作用 `--roots strict` 不复现：单行 def 的声明头 `def f(` 被当成一次调用；
    嵌套 def 的声明头被算成外层 def 的一次调用（两者都靠 skip_def_header=False 复现）。
    """
    lines = set(range(1, n_lines + 1))
    for d in decls:
        if d.kind == 'def':
            lines -= set(range(d.lo, d.hi))
    return lines


def closure(decls, code_lines, root_lines, lenient=False):
    """lenient=True 时整套口径都按 dossier 的做法（声明头算调用、嵌套 def 的体算外层）。"""
    by_name = {}
    for idx, d in enumerate(decls):
        if d.kind == 'def':
            by_name.setdefault(d.name, []).append(idx)

    roots = {n for n in calls_in(root_lines, code_lines, not lenient) if n in by_name}
    live, work = set(), list(roots)
    while work:
        n = work.pop()
        if n in live:
            continue
        live.add(n)
        for idx in by_name[n]:
            body = span_lines(decls[idx], decls, include_nested=lenient)
            for c in calls_in(body, code_lines, not lenient):
                if c in by_name and c not in live:
                    work.append(c)
    return by_name, live, roots


# ---------------------------------------------------------------- 汇总

def line_class(code_line, raw_line):
    if code_line.strip():
        return 'code'
    if raw_line.strip():
        return 'comment'
    return 'blank'


def summarize(decls, code_lines, raw_lines, by_name, live, roots, lenient_live,
              unsupported, warnings):
    total = len(code_lines)
    covered = set()
    for d in decls:
        covered |= set(range(d.lo, d.hi + 1))

    counts = {}
    for ln, (c, r) in enumerate(zip(code_lines, raw_lines, strict=True), 1):
        if ln in covered:
            continue
        cls = line_class(c, r)
        counts.setdefault(cls, []).append(ln)

    code_lines_total = sum(1 for c in code_lines if c.strip())
    code_covered = sum(1 for ln in covered if code_lines[ln - 1].strip())

    def_span = [d for d in decls if d.kind == 'def']
    live_idx = [i for name in live for i in by_name[name]]
    dead_idx = [i for i, d in enumerate(decls) if d.kind == 'def' and d.name not in live]

    def code_count(idx_list):
        if not idx_list:
            return 0
        lines = set()
        for i in idx_list:
            lines |= own_lines(decls[i], decls)
        return sum(1 for ln in lines if code_lines[ln - 1].strip())

    reach_code = code_count(live_idx)
    dead_code = code_count(dead_idx)

    kinds = {}
    for d in decls:
        kinds[d.kind] = kinds.get(d.kind, 0) + 1

    return {
        'lines_total': total,
        'decls_total': len(decls),
        'kinds': kinds,
        'covered': len(covered),
        'covered_ratio': round(len(covered) / total, 4) if total else 0.0,
        'code_lines_total': code_lines_total,
        'code_lines_covered': code_covered,
        'uncovered_blank': counts.get('blank', []),
        'uncovered_comment': counts.get('comment', []),
        'uncovered_other': counts.get('code', []),
        'def_decls': len(def_span),
        'def_names': len(by_name),
        'reachable_names': sorted(live),
        'unreachable_names': sorted(set(by_name) - live),
        'reachable_decls': len(live_idx),
        'unreachable_decls': len(dead_idx),
        'reachable_code_lines': reach_code,
        'unreachable_code_lines': dead_code,
        'roots': sorted(roots),
        'reachable_names_lenient': sorted(lenient_live),
        'unreachable_names_lenient': sorted(set(by_name) - lenient_live),
        'unsupported': unsupported,
        'warnings': warnings,
    }


def body_parsed_value(d):
    """TSV/JSON 的 `body_parsed` 列：本阶段只配平不解析，所以只有空体算「是」。"""
    if d.kind != 'def':
        return "-"
    return "是" if d.body == 'empty' else "否"


TSV_COLS = ("kind", "name", "lo", "hi", "params", "ret", "body_parsed", "reachable",
            "depth", "enclosing", "note")


def render_tsv(decls):
    out = ["\t".join(TSV_COLS)]
    for d in decls:
        span = f"{d.lo}-{d.hi}" if d.hi != d.lo else str(d.lo)
        out.append("\t".join((
            d.kind, d.name, span, d.params, d.ret, body_parsed_value(d), d.reachable,
            str(d.depth), d.enclosing, d.note)))
    return "\n".join(out)


def to_json(decls, src, summary):
    return {
        "source": src,
        "summary": summary,
        "decls": [
            {
                "kind": d.kind, "name": d.name, "lo": d.lo, "hi": d.hi,
                "params": d.params, "return_type": d.ret,
                "body_parsed": body_parsed_value(d),
                "reachable": d.reachable, "depth": d.depth, "enclosing": d.enclosing,
                "note": d.note,
            }
            for d in decls
        ],
    }


def print_summary(summary, quiet):
    if quiet:
        return
    s = summary

    def p(msg=""):
        print(msg, file=sys.stderr)

    kinds = " / ".join(f"{k} {v}" for k, v in sorted(s['kinds'].items()))
    p(f"总行 {s['lines_total']}；声明 {s['decls_total']} 条（{kinds}）")
    cov = 100.0 * s['code_lines_covered'] / s['code_lines_total'] if s['code_lines_total'] else 0.0
    p(f"覆盖：行 {s['covered']}/{s['lines_total']} = {100.0 * s['covered_ratio']:.1f}%；"
      f"代码行 {s['code_lines_covered']}/{s['code_lines_total']} = {cov:.1f}%")
    p(f"未覆盖：空行 {len(s['uncovered_blank'])}、注释行 {len(s['uncovered_comment'])}、"
      f"其它 {len(s['uncovered_other'])}")
    if s['uncovered_other']:
        rest = ", ".join(str(x) for x in s['uncovered_other'][:40])
        p(f"  其它未覆盖行：{rest}")
    p(f"def：{s['def_decls']} 处声明 / {s['def_names']} 个名字；"
      f"可达 {len(s['reachable_names'])} 个名字（{s['reachable_decls']} 处，代码行 {s['reachable_code_lines']}）"
      f" / 不可达 {len(s['unreachable_names'])} 个名字（{s['unreachable_decls']} 处，"
      f"代码行 {s['unreachable_code_lines']}）")
    p("入口（顶层块里的调用）：" + ", ".join(s['roots']))
    p(f"可达（{s['roots_mode']} 口径）：" + ", ".join(s['reachable_names']))
    p(f"可达（另一口径：{'strict' if s['roots_mode'] == 'lenient' else 'lenient'}）"
      f" {len(s['reachable_names_lenient'])} 个名字")
    p("不可达：" + ", ".join(s['unreachable_names']))
    for w in s['warnings']:
        p("警告：" + w)
    if s['unsupported']:
        p(f"未支持的顶层构造 {len(s['unsupported'])} 条：")
        for ln, text in s['unsupported'][:20]:
            p(f"  第 {ln} 行：{text[:100]}")
    else:
        p("未支持的顶层构造：无")


def main(argv=None):
    ap = argparse.ArgumentParser(description="顶层声明表 + 花括号配平 + 名字级可达性（不解析体）")
    ap.add_argument('--src', required=True, help=".si 源文件")
    ap.add_argument('--json', help="把完整结果写成 JSON（路径）")
    ap.add_argument('--roots', choices=('strict', 'lenient'), default='strict',
                    help="strict=只从顶层语句与 tc_main 出发（默认）；"
                         "lenient=复现 dossier reachability.py 的口径（把 def 声明头也当调用）")
    ap.add_argument('--quiet', action='store_true', help="不打印 stderr 统计")
    args = ap.parse_args(argv)

    try:
        with open(args.src, 'rb') as f:
            text = f.read().decode('utf-8')
    except OSError as e:
        print(f"读不了 {args.src}：{e}", file=sys.stderr)
        return 2

    raw_text, code_text = mask_source(text)
    code_lines = split_lines(code_text)
    raw_lines = split_lines(raw_text)

    decls, errors, unsupported = scan(code_lines, raw_lines)

    warnings = []
    for d in decls:
        if d.kind != 'def':
            continue
        if _body_is_empty(code_lines, d):
            d.body = 'empty'
        span = "".join(code_lines[d.lo - 1:d.hi])
        delta = _paren_delta(span)
        if delta:
            d.note = (f"{d.note}; " if d.note != "-" else "") + f"paren{delta:+d}"
            warnings.append(f"{d.name}（L{d.lo}-{d.hi}）圆括号不配平（{delta:+d} 个）"
                            "—— 按纪律只配平花括号，不报错")

    extra_root_lines = set()
    for ln, _ in unsupported:
        extra_root_lines.add(ln)
    by_name, live, roots = closure(decls, code_lines, strict_root_lines(decls, extra_root_lines))
    _, lenient_live, _ = closure(decls, code_lines, lenient_root_lines(decls, len(code_lines)),
                                 lenient=True)
    live = lenient_live if args.roots == 'lenient' else live
    for d in decls:
        if d.kind == 'def':
            d.reachable = "是" if d.name in live else "否"
    summary = summarize(decls, code_lines, raw_lines, by_name, live, roots, lenient_live,
                        unsupported, warnings)
    summary['roots_mode'] = args.roots

    print(render_tsv(decls))
    print_summary(summary, args.quiet)

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(to_json(decls, args.src, summary), f, ensure_ascii=False, indent=1)
            f.write("\n")

    if errors:
        for e in errors:
            print("错误：" + e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
