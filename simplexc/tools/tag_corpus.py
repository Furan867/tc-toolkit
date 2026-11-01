#!python3
"""M0 构造覆盖率矩阵：给 98 份唯一源码按「用到的语言构造」打标签 → 构造 × 文件矩阵。

为什么必须先去注释与字面量：语料里有大段中文注释（直接 grep `load(` 会命中注释），
字面量里也有 `table[2][1]` / `[Z][Z]` / `#E5787D` 这类看起来像代码的内容（`_asm` 这种后缀命名会命中 `asm`）。
所以先做一次掩码扫描（`//` `/* */`、`"…"`、`'…'`、`` `…` `` → 空格），
按**字节长度与换行位置不变**的方式做，行号因此一直对得上原文，证据片段从原文行取。

`raw` = 去注释、留字面量；`code` = 去注释且去字面量。名字/关键字一律用 `code`，
「这里是不是字面量」用 `raw`。三种字面量形态：
  `"…"` 双引号字符串（前端主要形态）
  `'…'`  单引号（`label == 'Program'`，语义未定，见 notes 判不准）
  `` `…` `` 反引号模板字符串，`{expr}` 内是**代码**（插值）——所以 `code` 里只抹字面量文字，
          保留插值括号与表达式（否则模板里的 `get_binary_repr(...)` 会被当成不存在）。

只读 corpus/src/**、manifest.tsv、stdlib_prefix.si；不编译、不加载 DLL、不跑游戏。

用法（用 PATH 上的 Python 3.13）：
    python simplexc/tools/tag_corpus.py             # 重新生成 corpus/coverage.tsv
    python simplexc/tools/tag_corpus.py --check     # 与现有 coverage.tsv 逐格比对，退出码 0/1
    python simplexc/tools/tag_corpus.py --evidence            # 每个构造打印 ≤3 条证据
    python simplexc/tools/tag_corpus.py --evidence --only 位运算内建
    python simplexc/tools/tag_corpus.py --prefix    # 看每个构造在 stdlib 前缀里是否出现
"""
import argparse
import bisect
import contextlib
import os
import re
import sys

# Windows 下 stdout 走管道时默认 GBK，输出里的 ✅/❌ 会直接抛 UnicodeEncodeError（本工具就跑挂过）
for _stream in (sys.stdout, sys.stderr):
    _rec = getattr(_stream, "reconfigure", None)
    if _rec is not None:
        with contextlib.suppress(ValueError, OSError):
            _rec(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.normpath(os.path.join(HERE, '..', 'corpus'))
MANIFEST = os.path.join(CORPUS, 'manifest.tsv')
SRC_DIR = os.path.join(CORPUS, 'src')
PREFIX = os.path.join(CORPUS, 'stdlib_prefix.si')
COVERAGE = os.path.join(CORPUS, 'coverage.tsv')

HEADER_COL = '构造'
COUNT_COL = '文件数'
EVIDENCE_MAX = 3


# ---------------------------------------------------------------- 文本预处理

LITERAL_DELIMS = '"\'`'


def scan_literal(text, i):
    """从 text[i]（字面量起始定界符）扫到闭定界符之后；遇换行即截断。"""
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


def blank_keep_interp(seg):
    """模板字符串：文字部分抹成空格，{expr} 原样留下（连括号）。"""
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


def strip_all(text):
    """一次扫描产出两份掩码文本，长度与换行不变。

    raw  = 注释 → 空格，字面量原样保留（判「这是字面量」用它）
    code = 注释 → 空格，字面量文字 → 空格（模板字符串的插值表达式保留）
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
            j = scan_literal(text, i)
            seg = text[i:j]
            raw.append(seg)
            code.append(blank_keep_interp(seg) if c == '`' else ' ' * (j - i))
            i = j
        else:
            raw.append(c)
            code.append(c)
            i += 1
    return ''.join(raw), ''.join(code)


DECL_LINE = re.compile(r'^(var|let|const|type|def|extern|import|asm|#)\b')
CLOSERS = '{)}],;'
DEF_LINE = re.compile(r'^\s*def\s+(\w+)', re.M)
STRUCT_DECL = re.compile(r'\btype\s+(\w+)\s+Struct\b')
ENUM_DECL = re.compile(r'\btype\s+\w+\s+Enum\s*\[([^\]]*)\]')
PARAM_STR = re.compile(r'\b(\w+)\s*:\s*Str\b')
PARAM_ARR = re.compile(r'\b(\w+)\s*:\s*Arr\s*\[')
INIT_STR = re.compile(r'\b(?:var|let|const)\s+(#?\w+)\s*=\s*"')
INIT_ARR = re.compile(r'\b(?:var|let|const)\s+(#?\w+)\s*=\s*'
                      r'(?:raw_cast\s*\(\s*<\s*Arr\b|array\s*\(|\[)')
INIT_PTR = re.compile(r'\b(?:var|let|const)\s+(#?\w+)\s*=\s*'
                      r'(?:Ptr\b|raw_cast\s*\(\s*<\s*(?:Ptr|Arr)\b)')


class Src:
    """一份源码 + 预计算的掩码文本与名字集合。"""

    def __init__(self, name, text):
        self.name = name                     # 用于证据显示的短名（文件 id 或 'stdlib_prefix'）
        self.text = text
        self.lines = text.splitlines()
        self.raw, self.code = strip_all(text)
        self.starts = [0]
        for k, ch in enumerate(self.raw):
            if ch == '\n':
                self.starts.append(k + 1)
        self.audit = self.check_masks()
        self.def_names = {}
        for m in DEF_LINE.finditer(self.code):
            self.def_names.setdefault(m.group(1), []).append(self.line(m.start(), self.code))
        self.structs = set(STRUCT_DECL.findall(self.code))
        members, spans = [], []
        for m in ENUM_DECL.finditer(self.code):
            spans.append(m.span(1))
            members += [p.strip() for p in m.group(1).split(',') if p.strip()]
        self.enum_members = sorted(set(members))
        self.enum_masked = self._mask_spans(self.code, spans)
        self.no_struct_decl = STRUCT_DECL.sub('type x Struct', self.code)

    @staticmethod
    def _mask_spans(text, spans):
        buf = list(text)
        for a, b in spans:
            buf[a:b] = ' ' * (b - a)
        return ''.join(buf)

    def check_masks(self):
        """掩码不变式：长度不变、换行数不变、`code` 里不该再有定界符（有就是未闭合）。"""
        bad = []
        if not (len(self.raw) == len(self.code) == len(self.text)):
            bad.append('掩码长度与原文本不一致')
        if self.raw.count('\n') != self.text.count('\n'):
            bad.append('换行数不一致')
        left = ''.join(sorted({ch for ch in self.code if ch in LITERAL_DELIMS}))
        if left:
            bad.append(f'code 里残留定界符 {left}（未闭合的字面量）')
        return bad

    def line(self, pos, text):
        return bisect.bisect_right(self.starts, pos)

    def snippet(self, ln):
        if 1 <= ln <= len(self.lines):
            return self.lines[ln - 1].strip()[:80]
        return ''

    def top_level_stmts(self):
        """顶层语句（全局 var 初始化之外的那些）的行号。深度 0、首 token 不是声明关键字。"""
        out, depth = [], 0
        for ln, line in enumerate(self.code.split('\n'), 1):
            stripped = line.strip()
            if depth == 0 and stripped and stripped[0] not in CLOSERS \
                    and not DECL_LINE.match(stripped.split(None, 1)[0]):
                out.append(ln)
            depth += line.count('{') - line.count('}')
        return out

    def nested_defs(self):
        out, depth = [], 0
        for ln, line in enumerate(self.code.split('\n'), 1):
            if depth > 0 and DEF_LINE.match(line):
                out.append(ln)
            depth += line.count('{') - line.count('}')
        return out


# ---------------------------------------------------------------- 命中收集

def hits(src, pattern, where='code', flags=re.M, text=None):
    if text is None:
        text = src.code if where == 'code' else src.raw
    seen, out = set(), []
    for m in re.finditer(pattern, text, flags):
        ln = src.line(m.start(), text)
        if ln in seen:
            continue
        seen.add(ln)
        out.append((ln, src.snippet(ln)))
    return out


def union(*groups):
    seen, out = set(), []
    for group in groups:
        for ln, sn in group:
            if ln not in seen:
                seen.add(ln)
                out.append((ln, sn))
    return sorted(out)


def names_alt(names):
    return '|'.join(re.escape(n) for n in sorted(names, key=len, reverse=True))


def index_hits(src, names, kind):
    """names 里的名字被下标访问。kind='write' 只取 `name[...] =`（排除 ==）。"""
    if not names:
        return []
    pat = r'\b(?:' + names_alt(names) + r')\s*\['
    if kind == 'write':
        pat += r'[^\]\n]*\]\s*(?![=])=(?!=)'
    return hits(src, pat)


# ---------------------------------------------------------------- 构造定义

def build_constructs():
    C = []

    def add(name, fn, why=''):
        C.append((name, fn, why))

    add('顶层语句块', lambda s: [(ln, s.snippet(ln)) for ln in s.top_level_stmts()],
        '程序根 = 顶层语句块（含全局 var 初始化）；这里只算非声明语句')
    add('顶层 var 初始化', lambda s: hits(s, r'(?m)^var\s+#?\w+\s*=(?!=)'),
        '深度 0 的 var 带初值 = 全局数据初始化点')
    add('def 定义', lambda s: hits(s, r'(?m)^\s*def\s+\w+'))
    add('def 重载（同名多定义）',
        lambda s: [(ln, s.snippet(ln)) for n, lns in sorted(s.def_names.items()) if len(lns) > 1
                   for ln in lns[1:]],
        '同一文件内同名 def ≥2 个')
    add('type alias', lambda s: hits(s, r'(?m)^\s*type\s+\w+\s+'
                                    r'(?!Enum\s*\[|Struct\s*\{)[A-Za-z_]\w*(?:\[[^\]\n]*\])?\s*$'),
        '语法是 `type Time U64`（没有 `=`）；语料 0 命中，前缀里有 6 行')
    add('type Enum[...]', lambda s: hits(s, r'(?m)^\s*type\s+\w+\s+Enum\s*\['))
    add('type Struct{...}', lambda s: hits(s, r'(?m)^\s*type\s+\w+\s+Struct\s*\{'))
    add('Struct 复合字面量', struct_lit_hits,
        '先按本文件声明的 Struct 名匹配；声明头与 def 返回类型都已抹平')
    add('泛型参数 @Type', lambda s: hits(s, r'@Type\b'))
    add('泛型参数 @Size', lambda s: hits(s, r'@Size\b'))
    add('泛型参数 @Any', lambda s: hits(s, r'@Any\b'))
    add('泛型调用点（f(<T>, …)）', lambda s: hits(s, r'\b\w+\s*\(\s*<'),
        '实测只有 load / raw_cast / array 三种')
    add('$ 参数', lambda s: hits(s, r'\$[A-Za-z_]\w*'),
        '语料里 0 命中（只出现在 // 注释的 $c_alloc 示例里）')
    add('switch/case', lambda s: union(hits(s, r'\bswitch\b'), hits(s, r'\bcase\b')))
    add('while', lambda s: hits(s, r'\bwhile\b'))
    add('带标签循环', lambda s: hits(s, r'(?m)^\s*\w+\s*:\s*(?:while|for|loop)\b'))
    add('break/continue', lambda s: union(hits(s, r'\bbreak\b'), hits(s, r'\bcontinue\b')))
    add('if/else', lambda s: hits(s, r'\belse\b') if hits(s, r'\bif\b') else [])
    add('&&/||/!', lambda s: union(hits(s, r'&&'), hits(s, r'\|\|'),
                                   hits(s, r'(?<![=!<>])!(?!=)')))
    add('Bool/true/false', lambda s: hits(s, r'\b(?:Bool|true|false)\b'))
    add('sized int 显式转换（U8 (x) 等）', lambda s: hits(s, r'\b(?:U\d+|I\d+)\s*\('))
    add('下划线数字字面量', lambda s: hits(s, r'\b\d[\d_]*_\d'))
    add('大整数常量（> 2^32）', big_int_hits, '十进制或 0x 字面量 > 2^32')
    add('指针算术（Ptr ± Int）', ptr_arith_hits, '只认本文件声明的 Ptr 名字 ± 数字/表达式')
    add('load(<T>, addr)', lambda s: hits(s, r'\bload\s*\(\s*<'))
    add('store(addr, v)', lambda s: hits(s, r'\bstore\s*\('))
    add('raw_cast(<T>, v)', lambda s: hits(s, r'\braw_cast\s*\(\s*<'))
    add('type_of(x)', lambda s: hits(s, r'\btype_of\s*\('))
    add('size(x)', lambda s: hits(s, r'(?<![.\w])size\s*\('))
    add('array(<T>, n)', lambda s: hits(s, r'\barray\s*\(\s*<'))
    add('min/max', minmax_hits, '调用点，排除 `def min` / `def max` 定义行')
    add('位运算内建（asr/lsr/lsl/ror/rol/clz/ctz/bswap）',
        lambda s: hits(s, r'\b(?:asr|lsr|lsl|ror|rol|clz|ctz|bswap)\s*\('))
    add('字符串字面量', lambda s: hits(s, r'"', where='raw'), '只算双引号 `"…"`')
    add('字符串拼接 +', lambda s: hits(s, r'["\'`][^"\'`\n]*["\'`]\s*\+|\+\s*["\'`]', where='raw'),
        '双引号/单引号/反引号字面量都可以作为 `+` 的操作数')
    add('字符串索引/切片', str_index_hits, 'Str 类型名字下标 / `[a..b]` 切片')
    add('方法糖 .len()', lambda s: hits(s, r'\.\s*len\s*\('))
    add('方法糖（时间单位 .ms() 等）', lambda s: hits(s, r'\.\s*(?:ms|ns|us|sec|min|hour|day)\s*\('),
        '语料里只出现 .ms()')
    add('数组索引读写', arr_index_hits, 'Arr 名字下标 + 下标写入')
    add('数组字面量', lambda s: hits(s, r'(?:=|\(|,|\breturn)\s*\[[^\]\n]*\]'),
        '赋值/实参/return 位置的 `[...]`')
    add('enum 成员作为值', enum_value_hits, '把 `type X Enum[...]` 的成员名当表达式用')
    add('#if in_scope("…")', lambda s: hits(s, r'#if\s+in_scope\s*\('))
    add('const #NAME', lambda s: hits(s, r'\bconst\s+#\w+'))
    add('asm 块', lambda s: hits(s, r'(?m)^\s*asm\b'), '语料里 0 命中（前缀里有）')
    add('extern 声明', lambda s: hits(s, r'(?m)^\s*extern\b'), '语料里 0 命中（前缀里有）')
    add('big_int_*', lambda s: hits(s, r'\bbig_int_\w*'), '语料里 0 命中（前缀里有）')

    # 「其它」桶：下面这 8 行的并集（清单里没有、但前端绕不开的）
    extras = [
        ('内嵌 def（闭包）', lambda s: [(ln, s.snippet(ln)) for ln in s.nested_defs()],
         '`def` 出现在另一个 def/语句块内部'),
        ('let 局部绑定', lambda s: hits(s, r'\blet\s+\w+'), '不可变局部绑定，区别于 var'),
        ('hex/bin 内建', lambda s: hits(s, r'\b(?:hex|bin)\s*\('), '整数 → 字符串的字面量转换'),
        ('空函数体 {}', lambda s: hits(s, r'\bdef\b[^{}\n]*\{\s*\}'),
         '生成代码的占位函数（`def get_memory(...) Int { }`）'),
        ('其它：Int/Bool 显式转换', lambda s: hits(s, r'\b(?:Int|Bool)\s*\('),
         '与 sized int 转换同一套语法，名字不是 U1..U64'),
        ('其它：单引号字面量（\'…\'）', lambda s: hits(s, r"'[^'\n]*'", where='raw'),
         '`label == \'Program\'`；是字符串还是 char 类别未定'),
        ('其它：反引号模板字符串（`…`）', lambda s: hits(s, r'`', where='raw'),
         '文字 + `{expr}` 插值；内容不当代码看'),
        ('其它：模板插值 {expr}', lambda s: hits(s, r'`[^`\n]*\{', where='raw'),
         '反引号里 `{expr}`，expr 是表达式（tag 时要当代码看）'),
    ]
    add('其它（列出你发现的）', lambda s: union(*[fn(s) for _, fn, _ in extras]),
        '= 下面 8 行的并集')
    for name, fn, why in extras:
        add(name, fn, why)
    return C


def struct_lit_hits(s):
    """复合字面量：`Input {}`。要先排除 `def f(...) Input {` 的返回类型 + 函数体。"""
    if not s.structs:
        return []
    alt = names_alt(s.structs)
    text = re.sub(r'\bdef\b[^{]*?\b(?:' + alt + r')\s*\{',
                  lambda m: m.group(0)[:-1] + ' ', s.no_struct_decl)
    return hits(s, r'\b(?:' + alt + r')\s*\{', text=text)


def big_int_hits(s):
    out = []
    for pat in (r'\b\d[\d_]*\b', r'\b0[xX][0-9A-Fa-f][0-9A-Fa-f_]*\b'):
        for m in re.finditer(pat, s.code):
            lit = m.group(0).replace('_', '')
            try:
                val = int(lit, 16) if lit[:2].lower() == '0x' else int(lit)
            except ValueError:
                continue
            if val > 2 ** 32:
                ln = s.line(m.start(), s.code)
                out.append((ln, s.snippet(ln)))
    return union(out)


def ptr_arith_hits(s):
    names = set(INIT_PTR.findall(s.code))
    if not names:
        return []
    alt = names_alt(names)
    pat = (r'\b(?:' + alt + r')\b\s*(?![=])(?:[-+](?![=+]))\s*\S'
           r'|\S\s*(?:[-+](?![=+]))\s*\b(?:' + alt + r')\b')
    return hits(s, pat)


def minmax_hits(s):
    out = []
    for m in re.finditer(r'\b(?:min|max)\s*\(', s.code):
        ln = s.line(m.start(), s.code)
        before = s.lines[ln - 1][:max(0, m.start() - s.starts[ln - 1])] if ln - 1 < len(s.lines) else ''
        if re.search(r'\bdef\s+\w*$', before):
            continue
        out.append((ln, s.snippet(ln)))
    return union(out)


def str_index_hits(s):
    names = set(PARAM_STR.findall(s.raw)) | set(INIT_STR.findall(s.raw))
    return union(index_hits(s, names, 'read'), hits(s, r'\[[^\]\n]*\.\.[^\]\n]*\]'))


def arr_index_hits(s):
    names = set(PARAM_ARR.findall(s.code)) | set(INIT_ARR.findall(s.code))
    return union(index_hits(s, names, 'read'), index_hits(s, names, 'write'))


def enum_value_hits(s):
    if not s.enum_members:
        return []
    text = s.enum_masked
    pat = r'\b(?:' + names_alt(s.enum_members) + r')\b'
    return [(s.line(m.start(), text), s.snippet(s.line(m.start(), text)))
            for m in re.finditer(pat, text)]


# ---------------------------------------------------------------- 主流程

def read_manifest():
    ids = []
    try:
        with open(MANIFEST, encoding='utf-8') as fh:
            head = fh.readline().rstrip('\n').split('\t')
            if head[0] != 'id':
                raise SystemExit(f'manifest 首列不是 id：{head[0]!r}')
            for line in fh:
                if line.strip():
                    ids.append(line.split('\t')[0])
    except OSError as exc:
        raise SystemExit(f'读不到 {MANIFEST}：{exc}') from exc
    return ids


def load_src(name, path):
    try:
        with open(path, encoding='utf-8', errors='surrogateescape', newline='') as fh:
            return Src(name, fh.read())
    except OSError as exc:
        raise SystemExit(f'读不到 {path}：{exc}') from exc


def load_corpus():
    ids = read_manifest()
    srcs = [load_src(i, os.path.join(SRC_DIR, i + '.si')) for i in ids]
    return ids, srcs


def evaluate(ids, srcs, constructs):
    """→ {构造名: (bits[98], hits[98] = [(行号, 片段),...], 文件数)}"""
    table = {}
    for name, fn, _why in constructs:
        bits, allhits = [], []
        for src in srcs:
            hs = fn(src)
            bits.append(1 if hs else 0)
            allhits.append(hs)
        table[name] = (bits, allhits, sum(bits))
    return table


def render(ids, constructs, table):
    out = [HEADER_COL] + list(ids) + [COUNT_COL]
    rows = ['\t'.join(out)]
    for name, _fn, _why in constructs:
        bits = table[name][0]
        rows.append('\t'.join([name] + [str(b) for b in bits] + [str(sum(bits))]))
    return '\n'.join(rows) + '\n'


def check(ids, constructs, table):
    problems = []
    if not os.path.exists(COVERAGE):
        return [f'{COVERAGE} 不存在']
    try:
        with open(COVERAGE, encoding='utf-8', newline='') as fh:
            lines = fh.read().split('\n')
    except OSError as exc:
        return [f'读不到 {COVERAGE}：{exc}']
    lines = [line for line in lines if line != '']
    expect_header = '\t'.join([HEADER_COL] + list(ids) + [COUNT_COL])
    if lines[0] != expect_header:
        problems.append('表头不一致')
    if len(lines) != len(constructs) + 1:
        problems.append(f'行数 {len(lines)} ≠ 构造数+1 = {len(constructs) + 1}')
        return problems
    want = len(ids) + 2
    for row, (name, _fn, _why) in zip(lines[1:], constructs, strict=True):
        f = row.split('\t')
        if len(f) != want:
            problems.append(f'{name}: 列数 {len(f)} ≠ {want}')
            continue
        if f[0] != name:
            problems.append(f'构造名顺序不一致：文件 {f[0]!r} / 重算 {name!r}')
            continue
        bits = table[name][0]
        if [int(x) for x in f[1:1 + len(ids)]] != bits:
            problems.append(f'{name}: 命中格与重算不一致')
        if int(f[-1]) != sum(bits):
            problems.append(f'{name}: 文件数列 {f[-1]} ≠ {sum(bits)}')
    return problems


def evidence(ids, constructs, table, only):
    for name, _fn, why in constructs:
        if only and only not in name:
            continue
        bits, allhits, _ = table[name]
        note = f'  — {why}' if why else ''
        print(f'== {name}  ({sum(bits)}/98){note}')
        shown = 0
        for cid, hs in zip(ids, allhits, strict=True):
            for ln, sn in hs:
                if shown >= EVIDENCE_MAX:
                    break
                print(f'   {cid}:{ln}  {sn}')
                shown += 1
            if shown >= EVIDENCE_MAX:
                break
        if shown == 0:
            print('   （无命中）')


def main(argv):
    doc = (__doc__ or '').splitlines()
    ap = argparse.ArgumentParser(description=doc[0] if doc else '构造覆盖率矩阵')
    ap.add_argument('--check', action='store_true', help='与现有 coverage.tsv 比对，不写文件')
    ap.add_argument('--evidence', action='store_true', help='打印每个构造的样例证据')
    ap.add_argument('--only', default='', help='配合 --evidence：只打印名字含该子串的构造')
    ap.add_argument('--prefix', action='store_true', help='额外报告每个构造是否出现在 stdlib 前缀里')
    args = ap.parse_args(argv)

    constructs = build_constructs()
    ids, srcs = load_corpus()
    broken = [s.name for s in srcs if s.audit]
    if broken:
        print(f'❌ 掩码不变式违例（{len(broken)} 份）：{" ".join(broken[:8])} —— 标签不可信')
        return 1
    table = evaluate(ids, srcs, constructs)

    if args.check:
        problems = check(ids, constructs, table)
        for p in problems:
            print('❌ ' + p)
        if problems:
            return 1
        print(f'✅ coverage.tsv 与重算一致：{len(constructs)} 构造 × {len(ids)} 文件')
        return 0

    if args.evidence:
        evidence(ids, constructs, table, args.only)
        return 0

    text = render(ids, constructs, table)
    try:
        with open(COVERAGE, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
    except OSError as exc:
        raise SystemExit(f'写不出 {COVERAGE}：{exc}') from exc

    full = [n for n, _f, _w in constructs if table[n][2] == len(ids)]
    zero = [n for n, _f, _w in constructs if table[n][2] == 0]
    print(f'写出 {COVERAGE}：{len(constructs)} 构造 × {len(ids)} 文件（掩码不变式全通过）')
    print(f'98/98（{len(full)}）：{"、".join(full)}')
    print(f'0/98（{len(zero)}）：{"、".join(zero)}')
    if args.prefix:
        psrc = load_src('stdlib_prefix', PREFIX)
        print('前缀命中：')
        for name, fn, _why in constructs:
            n = len(fn(psrc))
            if n:
                print(f'   {name:<40} {n} 处')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
