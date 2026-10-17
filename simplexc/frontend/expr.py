#!python3
"""M3.0 前端第二刀：语句 + 表达式 → AST（不做类型检查、不生成 IR、不化简）。

输入是 `decls.py` 给的 `lo`/`hi`，按行切出某个 def 的 token 流，解析成语法树再 dump。
只到**语法层**：类型只保留源里的拼写（`TypeRef.name`），作用域/重载/常量求值/`in_scope` 真假
一个都不做——那是 `docs/specs/2026-10-15-simplexc-interfaces.md` §1 冻结版 C++ 侧 `TypedProgram`
的事，不要在这里先写一版。

四条保真纪律（不是风格，是后续 lowering 的输入）：

  1. 每个节点带 `src_line`（1-based）。
  2. `U32 (expr)` / `U64 (expr)` 是 `CastExpr{target}` 节点，**不消失**。
  3. 字面量的类型标注（`S32 0`、`U8 250`）同样是 `CastExpr`，且解析期**不折叠任何运算**：
     `S32 0 - S32 7` 就是 `Binary(Cast(S32,0), '-', Cast(S32,7))`。理由见
     `dossier/div_sign.md` 指示 3：参考实现的除法对「两个窄类型字面量折叠出的常量」有特殊行为
     （`(S32 0 - S32 7) / 2` = 2147483644 而不是 −3），要复刻就必须在 AST 里看见**源里的形状**；
     折叠掉就再也分不出它和 `S32 (0 - 7)`。
  4. 二元/一元运算符不折叠、不化简、不重排（`1 + 1` 就是 `Binary(1,'+',1)`）。

**故意不做**（不是漏了）：类型、作用域、重载、常量求值、`in_scope` 真假、泛型单态化、模板插值、
不可达 def（`decls.py` 的纪律：只配平跳过）。遇到不认识的构造一律报错退出（不猜语义），
`--all` 会把每个 def 的结果列成表，未支持项带行号。

用法：
    python simplexc/frontend/expr.py --src <file.si> --def mode_run            # AST 到 stdout
    python simplexc/frontend/expr.py --src <file.si> --def mode_run --stats    # 加详细统计
    python simplexc/frontend/expr.py --src <file.si> --def mode_run --json out.json
    python simplexc/frontend/expr.py --src <file.si> --all                     # 全文件体检表

`--def` 也接受**带标签的顶层块**（`run_sim`）——它是冻结接口 §1 里的程序根。
stdout = AST dump（或 `--all` 的 TSV 表）；stderr = 统计、警告、错误定位。
解析失败 / 找不到 def → 退出码 1；文件读不了 → 2。
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import sys
from dataclasses import dataclass, fields as dc_fields

# Windows 下 stdout 走管道默认 GBK，含中文时不 reconfigure 会抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    _rec = getattr(_stream, "reconfigure", None)
    if _rec is not None:
        with contextlib.suppress(ValueError, OSError):
            _rec(encoding="utf-8", errors="replace")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decls import (closure, mask_source, scan,  # noqa: E402
                   split_lines, strict_root_lines)

# ---------------------------------------------------------------- 词法

IDENT_RE = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')

OPS3 = ('<<=', '>>=')
OPS2 = ('==', '!=', '<=', '>=', '&&', '||', '+=', '-=', '*=', '/=', '%=',
        '&=', '|=', '^=', '<<', '>>')
OPS1 = '+-*/%&|^~!<>()[]{},:;=?.'
STRING_DELIMS = '"\''


@dataclass
class Tok:
    kind: str      # ident | hash | at | number | string | template | op | eof
    text: str      # ident/number/op 是原文；string/template 是定界符之间的内容
    line: int      # 1-based
    col: int
    gap: bool      # 与上一个 token 之间是否有空白（`f(x)` 是调用，`T (x)` 是转换）
    quote: str = ''      # string/template 的定界符
    interp: bool = False  # 模板里是否有 `{…}` 插值


class LexError(Exception):
    def __init__(self, line, col, msg):
        super().__init__(msg)
        self.line, self.col, self.msg = line, col, msg


def _scan_quoted(text, i, j_max):
    """从 text[i]（定界符）扫到闭合定界符之后；遇换行即截断。返回结束下标（未闭合 = -1）。"""
    delim = text[i]
    j = i + 1
    while j < j_max:
        c = text[j]
        if c == '\\':
            j += 2
            continue
        if c == delim:
            return j + 1
        if c == '\n':
            return -1
        j += 1
    return -1


def tokenize(text):
    """把源码（注释已掩码的那一份）切成 token 流，末尾补一个 EOF。

    token 里带行号与「前面有没有空白」——后者是消歧 `f(x)`（调用）与 `T (x)`（转换）的
    唯一依据，Nim 语法里 `f (x)` 不是调用（实测：语料里「有空白 + (」的写法全是类型名）。
    """
    toks = []
    i, n, line, col = 0, len(text), 1, 1
    gap = True
    while i < n:
        c = text[i]
        if c == '\n':
            i, line, col, gap = i + 1, line + 1, 1, True
            continue
        if c in ' \t\r':
            i, col, gap = i + 1, col + 1, True
            continue
        start_line, start_col = line, col
        m = IDENT_RE.match(text, i)
        if m:
            toks.append(Tok('ident', m.group(0), start_line, start_col, gap))
            col += m.end() - i
            i = m.end()
            gap = False
            continue
        if c == '#' or c == '@':
            m = IDENT_RE.match(text, i + 1)
            if not m:
                raise LexError(start_line, start_col, f"第 {start_line} 行：'{c}' 后面不是名字")
            kind = 'hash' if c == '#' else 'at'
            toks.append(Tok(kind, m.group(0), start_line, start_col, gap))
            col += m.end() - i
            i = m.end()
            gap = False
            continue
        if c.isdigit():
            j = i
            if text.startswith(('0x', '0X'), i):
                j = i + 2
                while j < n and (text[j] in '0123456789abcdefABCDEF_' or text[j] == '_'):
                    j += 1
            elif text.startswith(('0b', '0B'), i):
                j = i + 2
                while j < n and text[j] in '01_':
                    j += 1
            else:
                while j < n and (text[j].isdigit() or text[j] == '_'):
                    j += 1
            if j < n and text[j] == '.' and j + 1 < n and text[j + 1].isdigit():
                raise LexError(start_line, start_col, f"第 {start_line} 行：浮点字面量未支持")
            toks.append(Tok('number', text[i:j], start_line, start_col, gap))
            col += j - i
            i = j
            gap = False
            continue
        if c in STRING_DELIMS or c == '`':
            end = _scan_quoted(text, i, n)
            if end < 0:
                raise LexError(start_line, start_col,
                               f"第 {start_line} 行：{c} 字面量没有闭合")
            inner = text[i + 1:end - 1]
            kind = 'template' if c == '`' else 'string'
            interp = kind == 'template' and '{' in inner
            toks.append(Tok(kind, inner, start_line, start_col, gap, quote=c, interp=interp))
            col += end - i
            i = end
            gap = False
            continue
        for ops, size in ((OPS3, 3), (OPS2, 2)):
            if text.startswith(ops, i):
                toks.append(Tok('op', text[i:i + size], start_line, start_col, gap))
                i += size
                col += size
                gap = False
                break
        else:
            if c not in OPS1:
                raise LexError(start_line, start_col,
                               f"第 {start_line} 行：不认识的字符 {c!r}")
            toks.append(Tok('op', c, start_line, start_col, gap))
            i, col, gap = i + 1, col + 1, False
    toks.append(Tok('eof', '', line, col, gap))
    return toks


def describe(t):
    if t.kind == 'eof':
        return "这里就到头了"
    if t.kind in ('ident', 'hash', 'at'):
        return f"'{t.text}'"
    if t.kind == 'number':
        return f"数字 {t.text}"
    if t.kind == 'string':
        return "字符串字面量"
    if t.kind == 'template':
        return "模板字面量"
    return f"'{t.text}'"


# ---------------------------------------------------------------- AST

@dataclass
class Node:
    src_line: int


@dataclass
class Expr(Node):
    pass


@dataclass
class Stmt(Node):
    pass


@dataclass
class TypeRef(Node):
    """源里的类型**拼写**（`U33` / `Ptr` / `@Size` / `Arr[U64]`），不是冻结接口 §1 的 `Type`。"""
    name: str
    at: bool = False
    elem: TypeRef | None = None


@dataclass
class LiteralExpr(Expr):
    kind: str                # int | str | template
    text: str                # 源码内容（int 保留 0x/0b 与下划线）
    quote: str = ''          # 字符串/模板的定界符：'"' / "'" / '`'（U-L3 未定，必须留着）
    value: int | None = None


@dataclass
class IdentExpr(Expr):
    name: str                # 编译期常量带 '#' 前缀（`#SIMULATION_STATE`）


@dataclass
class CastExpr(Expr):
    """`U32 (expr)` / `U64 expr` / `S32 0` —— 显式类型转换，操作数只到 unary 级。"""
    target: TypeRef
    expr: Expr


@dataclass
class UnaryExpr(Expr):
    op: str
    expr: Expr


@dataclass
class BinaryExpr(Expr):
    op: str
    left: Expr
    right: Expr


@dataclass
class CondExpr(Expr):
    cond: Expr
    then: Expr
    else_: Expr


@dataclass
class CallExpr(Expr):
    callee: Expr
    args: list[Expr]
    type_args: list[TypeRef]      # `load(<U64>, p)` 里的 `<U64>`


@dataclass
class MemberExpr(Expr):
    obj: Expr
    name: str


@dataclass
class IndexExpr(Expr):
    obj: Expr
    index: Expr


@dataclass
class ArrayLitExpr(Expr):
    items: list[Expr]
    close_line: int = 0


@dataclass
class FieldInit(Node):
    name: str
    value: Expr


@dataclass
class CompositeLitExpr(Expr):
    typ: TypeRef
    fields: list[FieldInit]
    close_line: int = 0


@dataclass
class Block(Node):
    stmts: list[Stmt]
    close_line: int = 0


@dataclass
class Param(Node):
    name: str
    typ: TypeRef | None


@dataclass
class VarDeclStmt(Stmt):
    kw: str                  # var | let | const
    name: str                # const 带 '#' 前缀
    init: Expr


@dataclass
class AssignStmt(Stmt):
    target: Expr
    op: str                  # '=' / '+=' / '-=' / '&=' / …
    value: Expr


@dataclass
class ExprStmt(Stmt):
    expr: Expr


@dataclass
class IfCase(Node):
    cond: Expr
    block: Block


@dataclass
class IfStmt(Stmt):
    cases: list[IfCase]
    else_block: Block | None = None


@dataclass
class PpIfStmt(Stmt):
    """`#if <const-expr> { … }`（语料里唯一形态是 `#if in_scope("name")`）——真假判定是 U-L14。"""
    cases: list[IfCase]
    else_block: Block | None = None


@dataclass
class WhileStmt(Stmt):
    cond: Expr
    block: Block
    label: str | None = None


@dataclass
class SwitchCase(Node):
    block: Block
    value: Expr | None = None      # None = default


@dataclass
class SwitchStmt(Stmt):
    subject: Expr
    cases: list[SwitchCase]


@dataclass
class ReturnStmt(Stmt):
    value: Expr | None = None


@dataclass
class BreakStmt(Stmt):
    label: str | None = None


@dataclass
class DefStmt(Stmt):
    name: str
    params: list[Param]
    ret: TypeRef | None
    body: Block


@dataclass
class FunctionDef(Node):
    name: str
    form: str                # def | label（`run_sim:` 顶层块）
    params: list[Param]
    ret: TypeRef | None
    body: Block
    lo: int = 0
    hi: int = 0


def walk(node):
    """先序遍历所有 Node（列表字段也下钻）。"""
    if isinstance(node, list):
        for x in node:
            yield from walk(x)
        return
    if not isinstance(node, Node):
        return
    yield node
    for f in dc_fields(node):
        v = getattr(node, f.name)
        if isinstance(v, (Node, list)):
            yield from walk(v)


def node_to_dict(n):
    if isinstance(n, list):
        return [node_to_dict(x) for x in n]
    if isinstance(n, Node):
        d = {'kind': type(n).__name__, 'src_line': n.src_line}
        for f in dc_fields(n):
            if f.name == 'src_line':
                continue
            d[f.name] = node_to_dict(getattr(n, f.name))
        return d
    return n


def render_tree(n, indent=0):
    """人读的 dump：节点行给 kind/行号/标量字段，子节点缩进。"""
    pad = '  ' * indent
    if isinstance(n, list):
        return "\n".join(render_tree(x, indent) for x in n)
    if not isinstance(n, Node):
        return f"{pad}{n}"
    scal, kids = [], []
    for f in dc_fields(n):
        if f.name == 'src_line':
            continue
        v = getattr(n, f.name)
        if isinstance(v, Node) or (isinstance(v, list) and v and isinstance(v[0], Node)):
            kids.append((f.name, v))
        else:
            scal.append(f"{f.name}={v!r}")
    head = f"{pad}{type(n).__name__} @{n.src_line}"
    if scal:
        head += "  " + " ".join(scal)
    out = [head]
    for name, v in kids:
        if isinstance(v, list):
            out.append(f"{pad}  {name}[{len(v)}]")
            for x in v:
                out.append(render_tree(x, indent + 2))
        else:
            out.append(f"{pad}  {name}:")
            out.append(render_tree(v, indent + 2))
    return "\n".join(out)


# ---------------------------------------------------------------- 语法

ASSIGN_OPS = ('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=')
UNARY_OPS = ('-', '+', '~', '!')
# 优先级只排语法（Nim 的次序：`&` 比 `==` 紧，实测语料靠它）；不做结合性以外的任何重排
PREC = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '<=': 4, '>': 4, '>=': 4,
        '|': 5, '^': 6, '&': 7, '<<': 8, '>>': 8, '+': 9, '-': 9,
        '*': 10, '/': 10, '%': 10}
STMT_WORDS = {'if', 'elif', 'else', 'while', 'switch', 'case', 'default', 'return',
              'break', 'let', 'var', 'const', 'def', 'for', 'loop', 'type',
              'continue', 'when', 'try', 'discard'}
UNSUPPORTED_STMT_WORDS = ('for', 'loop', 'type', 'continue', 'when', 'try', 'discard')
TYPE_BUILTIN = {'Int', 'Bool', 'Str', 'None', 'Time', 'Ptr', 'Seed', 'Void', 'Uint',
                'Sint', 'Arr'}
TYPE_PAT = re.compile(r'^[US]\d+$')


class ParseError(Exception):
    """位置可以是 token，也可以只有行号（报「这条语句没有副作用」时就只有行号）。"""

    def __init__(self, where, msg):
        super().__init__(msg)
        self.line = where if isinstance(where, int) else where.line
        self.col = 0 if isinstance(where, int) else where.col
        self.msg = msg


class Parser:
    def __init__(self, toks, type_names, func_names):
        self.toks = list(toks)
        self.i = 0
        self.type_names = type_names
        self.func_names = func_names
        self.notes = []

    # ---- token 操作
    def peek(self, k=0):
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else self.toks[-1]

    def next(self):
        t = self.peek()
        self.i += 1
        return t

    def at(self, kind, text=None, k=0):
        t = self.peek(k)
        return t.kind == kind and (text is None or t.text == text)

    def expect(self, kind, text, what):
        t = self.peek()
        if not self.at(kind, text):
            raise ParseError(t, f"这里要 {what}，看到 {describe(t)}")
        return self.next()

    def close(self, text, open_tok):
        """收尾定界符：报错时把「哪一个 '(' 没配对」也带上，只报行号不够用。"""
        if self.at('op', text):
            return self.next()
        t = self.peek()
        raise ParseError(t, f"这里要 '{text}'（配对第 {open_tok.line} 行的 '{open_tok.text}'），"
                            f"看到 {describe(t)}")

    def _is_type_name(self, name):
        return (name in self.type_names or name in TYPE_BUILTIN
                or bool(TYPE_PAT.match(name)))

    # ---- 类型
    def parse_type_ref(self):
        t = self.peek()
        if t.kind not in ('ident', 'at'):
            raise ParseError(t, f"这里要类型名，看到 {describe(t)}")
        self.next()
        return self._type_ref_after(t)

    def _type_ref_after(self, t):
        elem = None
        if self.at('op', '['):
            self.next()
            elem = self.parse_type_ref()
            self.expect('op', ']', "']'")
        return TypeRef(src_line=t.line, name=t.text, at=(t.kind == 'at'), elem=elem)

    # ---- def 头 / 参数
    def parse_def_head(self):
        """'def' 已在调用点消费：NAME(params) [RET]。"""
        name = self.expect('ident', None, "函数名").text
        params = self.parse_params()
        ret = None
        if not self.at('op', '{'):
            ret = self.parse_type_ref()
        return name, params, ret

    def parse_params(self):
        self.expect('op', '(', "'('")
        params = []
        if self.at('op', ')'):
            self.next()
            return params
        while True:
            n = self.expect('ident', None, "参数名")
            if self.at('op', '='):
                raise ParseError(self.peek(), "参数默认值未支持")
            if not self.at('op', ':'):
                raise ParseError(self.peek(), f"参数 '{n.text}' 缺类型标注")
            self.next()
            params.append(Param(src_line=n.line, name=n.text, typ=self.parse_type_ref()))
            if self.at('op', ','):
                self.next()
                continue
            self.expect('op', ')', "')'")
            return params

    # ---- 语句
    def parse_block(self):
        t = self.peek()
        if not self.at('op', '{'):
            hint = "——'=' 是赋值，比较要写 '=='" if self.at('op', '=') else ""
            raise ParseError(t, f"这里要 '{{'（块开始），看到 {describe(t)}{hint}")
        open_tok = self.next()
        stmts = []
        while not self.at('op', '}'):
            if self.peek().kind == 'eof':
                raise ParseError(self.peek(),
                                 f"还没闭合就结束了：第 {open_tok.line} 行的 '{{' 没有配对的 '}}'")
            stmts.append(self.parse_statement())
        close = self.next()
        return Block(src_line=open_tok.line, close_line=close.line, stmts=stmts)

    def parse_statement(self):
        t = self.peek()
        if t.kind == 'hash' and t.text == 'if':
            return self.parse_ppif()
        if t.kind == 'ident':
            w = t.text
            if w in ('var', 'let', 'const'):
                return self.parse_var(t)
            if w == 'if':
                return self.parse_if(t)
            if w == 'while':
                return self.parse_while(t, None)
            if w == 'switch':
                return self.parse_switch(t)
            if w == 'return':
                return self.parse_return(t)
            if w == 'break':
                return self.parse_break(t)
            if w == 'def':
                self.next()
                name, params, ret = self.parse_def_head()
                return DefStmt(src_line=t.line, name=name, params=params, ret=ret,
                               body=self.parse_block())
            if w in ('elif', 'else'):
                raise ParseError(t, f"'{w}' 前面没有 'if'")
            if w in ('case', 'default'):
                raise ParseError(t, f"'{w}' 前面没有 'switch'")
            if w in UNSUPPORTED_STMT_WORDS:
                raise ParseError(t, f"语句 '{w}' 未支持")
            n1, n2 = self.peek(1), self.peek(2)
            if (n1.kind == 'op' and n1.text == ':' and n1.line == t.line
                    and n2.kind == 'ident' and n2.text == 'while'):
                self.next()
                self.next()
                return self.parse_while(self.peek(), w)
        return self.parse_assign_or_expr()

    def parse_var(self, t):
        self.next()
        if t.text == 'const':
            n = self.expect('hash', None, "编译期常量名 '#NAME'")
            name = '#' + n.text
        else:
            name = self.expect('ident', None, "变量名").text
        self.expect('op', '=', "'='")
        return VarDeclStmt(src_line=t.line, kw=t.text, name=name, init=self.parse_expr())

    def parse_assign_or_expr(self):
        e = self.parse_expr()
        t = self.peek()
        if t.kind == 'op' and t.text in ASSIGN_OPS:
            self.next()
            if not isinstance(e, (IdentExpr, IndexExpr, MemberExpr)):
                raise ParseError(t, "赋值左边不是变量 / 下标 / 字段")
            return AssignStmt(src_line=e.src_line, target=e, op=t.text,
                              value=self.parse_expr())
        if not isinstance(e, CallExpr):
            raise ParseError(e.src_line,
                             f"语句位置出现没有副作用的表达式（{type(e).__name__}）"
                             "——是不是把 '=' 写成了 '=='？")
        return ExprStmt(src_line=e.src_line, expr=e)

    def parse_if(self, t):
        self.next()
        cases = [IfCase(src_line=t.line, cond=self.parse_expr(), block=self.parse_block())]
        while self.at('ident', 'elif'):
            et = self.next()
            cases.append(IfCase(src_line=et.line, cond=self.parse_expr(),
                                block=self.parse_block()))
        else_block = None
        if self.at('ident', 'else'):
            self.next()
            if not self.at('op', '{'):
                raise ParseError(self.peek(), "'else' 后面要 '{'（`else if` 请写 'elif'）")
            else_block = self.parse_block()
        return IfStmt(src_line=t.line, cases=cases, else_block=else_block)

    def parse_ppif(self):
        """`#if <const-expr> { … }`：`else`/`elif` 可能写成裸字（语料两种都有）。

        绑定规则只有一条：`else`/`elif` 必须**紧跟在** `#if` 那块的 `}` 之后（下一个 token），
        所以 `if a { #if in_scope("b") {…} } else {…}` 里的 `else` 不会被这里抢走。
        """
        t = self.next()                      # #if
        cases = [IfCase(src_line=t.line, cond=self.parse_expr(), block=self.parse_block())]
        while self.at('hash', 'elif') or self.at('ident', 'elif'):
            et = self.next()
            cases.append(IfCase(src_line=et.line, cond=self.parse_expr(),
                                block=self.parse_block()))
        else_block = None
        if self.at('hash', 'else') or self.at('ident', 'else'):
            self.next()
            else_block = self.parse_block()
        return PpIfStmt(src_line=t.line, cases=cases, else_block=else_block)

    def parse_while(self, t, label):
        self.next()
        cond = self.parse_expr()
        return WhileStmt(src_line=t.line, cond=cond, block=self.parse_block(), label=label)

    def parse_switch(self, t):
        self.next()
        subject = self.parse_expr()
        cases = []
        while True:
            c = self.peek()
            if c.kind == 'ident' and c.text == 'case':
                self.next()
                cases.append(SwitchCase(src_line=c.line, value=self.parse_expr(),
                                        block=self.parse_block()))
            elif c.kind == 'ident' and c.text == 'default':
                self.next()
                cases.append(SwitchCase(src_line=c.line, block=self.parse_block()))
            else:
                break
        return SwitchStmt(src_line=t.line, subject=subject, cases=cases)

    def parse_return(self, t):
        self.next()
        nxt = self.peek()
        if nxt.kind == 'eof' or self.at('op', '}') or nxt.line > t.line:
            return ReturnStmt(src_line=t.line)
        return ReturnStmt(src_line=t.line, value=self.parse_expr())

    def parse_break(self, t):
        self.next()
        label = None
        nxt = self.peek()
        if nxt.kind == 'ident' and nxt.line == t.line and nxt.text not in STMT_WORDS:
            label = self.next().text
        return BreakStmt(src_line=t.line, label=label)

    # ---- 表达式
    def parse_expr(self):
        e = self.parse_binary(1)
        if self.at('op', '?'):
            self.next()
            then = self.parse_expr()
            self.expect('op', ':', "':'（三目运算符）")
            return CondExpr(src_line=e.src_line, cond=e, then=then,
                            else_=self.parse_expr())
        return e

    def parse_binary(self, min_prec):
        left = self.parse_unary()
        while True:
            t = self.peek()
            if t.kind != 'op' or t.text not in PREC or PREC[t.text] < min_prec:
                return left
            self.next()
            right = self.parse_binary(PREC[t.text] + 1)
            left = BinaryExpr(src_line=left.src_line, op=t.text, left=left, right=right)

    def parse_unary(self):
        t = self.peek()
        if t.kind == 'op' and t.text in UNARY_OPS:
            self.next()
            return UnaryExpr(src_line=t.line, op=t.text, expr=self.parse_unary())
        if self._cast_ahead():
            name_tok = self.next()
            return CastExpr(src_line=name_tok.line, target=self._type_ref_after(name_tok),
                            expr=self.parse_unary())
        return self.parse_postfix()

    def _cast_ahead(self):
        """`T (expr)` / `T ident` / `T 0` 是转换，`f(x)` 是调用。三条判据：

        1. **并置**（`TestResult max(...)`）只有转换一种读法 → 转换；
        2. `name (` 靠「有没有空白」+ 类型名表消歧：紧邻的是调用（Nim 语法 `f (x)` 不是调用），
           有空隙且是类型名的是转换；
        3. `T ~x` / `T !x` 只有转换一种读法；`T -x` / `T +x` 靠类型名表（实测：语料里
           没有「类型名 + 二元加减」的写法，而 `S8 -8` 这种负字面量必须读成转换）。

        跨行的并置是两条语句，不算。
        """
        t = self.peek()
        if t.kind not in ('ident', 'at'):
            return False
        nxt = self.peek(1)
        if nxt.line != t.line:
            return False
        is_type = self._is_type_name(t.text)
        if nxt.kind == 'op' and nxt.text == '(':
            if is_type:
                if not nxt.gap:
                    self.notes.append(
                        f"第 {t.line} 行：`{t.text}(…)` 紧邻无空白，按类型转换解析"
                        f"（若其实是函数调用请改名）")
                return True
            return False
        if nxt.kind in ('ident', 'number', 'string', 'template', 'hash', 'at'):
            return not (nxt.kind == 'ident' and nxt.text in STMT_WORDS)
        if nxt.kind == 'op' and nxt.text in ('~', '!'):
            return True
        if nxt.kind == 'op' and nxt.text in ('+', '-'):
            return is_type
        if nxt.kind == 'op' and nxt.text == '[':
            return is_type                      # `Arr[U8] []`；`x [0]` 仍是下标
        return False

    def parse_postfix(self):
        e = self.parse_primary()
        while True:
            t = self.peek()
            if t.kind == 'op' and t.text == '(':
                self.next()
                type_args, args = self.parse_args(t)
                e = CallExpr(src_line=e.src_line, callee=e, args=args, type_args=type_args)
            elif t.kind == 'op' and t.text == '[':
                self.next()
                idx = self.parse_expr()
                self.expect('op', ']', "']'")
                e = IndexExpr(src_line=e.src_line, obj=e, index=idx)
            elif t.kind == 'op' and t.text == '.':
                self.next()
                n = self.expect('ident', None, "成员名")
                e = MemberExpr(src_line=e.src_line, obj=e, name=n.text)
            else:
                return e

    def parse_args(self, open_tok):
        """`(` 已在调用点消费。返回 (type_args, args)。

        `<T>` 类型实参可以出现在参数表里**任何位置**（`load(<U8>, p)`、
        `get_ram_value("Program", Int counter, <U8>)` 两种都在语料里）；
        尾随逗号也收。
        """
        args, type_args = [], []
        while True:
            if self.at('op', '<'):
                save = self.i
                cand = []
                try:
                    cand = self.parse_type_arg_list()
                except ParseError:
                    cand = []
                # 类型实参表后面必须紧跟 ',' 或 ')'，否则那不是 `<T>` 而是比较运算
                if cand and (self.at('op', ',') or self.at('op', ')')):
                    type_args.extend(cand)
                    if self.at('op', ','):
                        self.next()
                        continue
                    self.next()
                    return type_args, args
                self.i = save
            if self.at('op', ')'):           # 空表或尾随逗号
                self.next()
                return type_args, args
            args.append(self.parse_expr())
            if self.at('op', ','):
                self.next()
                continue
            if self.at('op', ')'):
                self.next()
                return type_args, args
            t = self.peek()
            raise ParseError(t, f"这里要 ',' 或 ')'（配对第 {open_tok.line} 行的 '('），"
                                f"看到 {describe(t)}")

    def parse_type_arg_list(self):
        self.expect('op', '<', "'<'")
        types = [self.parse_type_ref()]
        while self.at('op', ','):
            self.next()
            types.append(self.parse_type_ref())
        self.expect('op', '>', "'>'")
        return types

    def parse_primary(self):
        t = self.peek()
        if t.kind == 'number':
            self.next()
            return LiteralExpr(src_line=t.line, kind='int', text=t.text,
                               value=int(t.text.replace('_', ''), 0))
        if t.kind == 'string':
            self.next()
            return LiteralExpr(src_line=t.line, kind='str', text=t.text, quote=t.quote)
        if t.kind == 'template':
            self.next()
            if t.interp:
                raise ParseError(t, "模板字面量的 `{…}` 插值语义未定（U-L3 未决）→ 未支持")
            return LiteralExpr(src_line=t.line, kind='template', text=t.text, quote=t.quote)
        if t.kind == 'hash':
            self.next()
            return IdentExpr(src_line=t.line, name='#' + t.text)
        if t.kind == 'at':
            raise ParseError(t, f"'@{t.text}' 只能出现在类型位置")
        if t.kind == 'op' and t.text == '(':
            self.next()
            e = self.parse_expr()
            self.close(')', t)
            return e
        if t.kind == 'op' and t.text == '[':
            return self.parse_array_literal()
        if t.kind == 'ident':
            nxt = self.peek(1)
            if (nxt.kind == 'op' and nxt.text == '{' and nxt.line == t.line
                    and self._is_type_name(t.text)):
                return self.parse_composite(t)
            self.next()
            return IdentExpr(src_line=t.line, name=t.text)
        raise ParseError(t, f"表达式里出现意外的 {describe(t)}")

    def parse_array_literal(self):
        t = self.next()                       # [
        items = []
        while True:
            if self.at('op', ']'):            # 空表或尾随逗号
                return ArrayLitExpr(src_line=t.line, items=items,
                                    close_line=self.next().line)
            items.append(self.parse_expr())
            if self.at('op', ','):
                self.next()
                continue
            close = self.expect('op', ']', "']'")
            return ArrayLitExpr(src_line=t.line, items=items, close_line=close.line)

    def parse_composite(self, t):
        typ = self._type_ref_after(self.next())
        self.expect('op', '{', "'{'")
        fields = []
        while True:
            if self.at('op', '}'):            # 空字面量或尾随逗号
                return CompositeLitExpr(src_line=t.line, typ=typ, fields=fields,
                                        close_line=self.next().line)
            n = self.expect('ident', None, "字段名")
            self.expect('op', ':', "':'")
            fields.append(FieldInit(src_line=n.line, name=n.text, value=self.parse_expr()))
            if self.at('op', ','):
                self.next()
                continue
            close = self.expect('op', '}', "'}'")
            return CompositeLitExpr(src_line=t.line, typ=typ, fields=fields,
                                    close_line=close.line)


# ---------------------------------------------------------------- 前处理与统计

def declared_type_names(decls):
    """源里出现过的类型名：`type` 声明 + 每个 def 头的参数/返回类型字面。"""
    names = set()
    for d in decls:
        if d.kind == 'type':
            names.add(d.name)
        elif d.kind == 'def':
            for m in re.finditer(r':\s*(@?[A-Za-z_]\w*)', d.params or ''):
                names.add(m.group(1).lstrip('@'))
            ret = (d.ret or '').strip()
            if ret and ret not in ('-', '?'):
                names.add(ret.lstrip('@'))
    return names


class Frontend:
    def __init__(self, src_path):
        with open(src_path, 'rb') as f:
            text = f.read().decode('utf-8')
        raw_text, code_text = mask_source(text)
        self.src_path = src_path
        self.code_lines = split_lines(code_text)
        self.raw_lines = split_lines(raw_text)
        self.decls, self.decl_errors, self.top_unsupported = scan(self.code_lines,
                                                                 self.raw_lines)
        self.toks = tokenize(raw_text)
        self.type_names = declared_type_names(self.decls)
        self.func_names = {d.name for d in self.decls if d.kind == 'def'}
        self.live = set()
        by_name, live, _ = closure(self.decls, self.code_lines,
                                   strict_root_lines(self.decls, set()))
        self.by_name, self.live = by_name, live

    def candidates(self, name):
        return [d for d in self.decls if d.name == name and d.kind in ('def', 'label')]

    def parse_def(self, d):
        """解析一个 def（或 `run_sim:` 顶层块）的体；返回 (FunctionDef, Parser)。"""
        toks = [t for t in self.toks if d.lo <= t.line <= d.hi]
        if not toks:
            raise ParseError(Tok('eof', '', d.lo, 1, True), f"L{d.lo}-{d.hi} 里没有 token")
        toks.append(Tok('eof', '', d.hi, 1, True))
        p = Parser(toks, self.type_names, self.func_names)
        if d.kind == 'label':
            stmts = []
            while not p.at('eof'):
                if p.at('op', '}'):           # 块自己的收尾 '}'
                    close = p.next()
                    body = Block(src_line=d.lo, close_line=close.line, stmts=stmts)
                    break
                stmts.append(p.parse_statement())
            else:
                body = Block(src_line=d.lo, close_line=d.hi, stmts=stmts)
            fd = FunctionDef(src_line=d.lo, name=d.name, form='label', params=[], ret=None,
                             body=body, lo=d.lo, hi=d.hi)
        else:
            p.expect('ident', 'def', "'def'")
            name, params, ret = p.parse_def_head()
            body = p.parse_block()
            fd = FunctionDef(src_line=d.lo, name=name, form='def', params=params, ret=ret,
                             body=body, lo=d.lo, hi=d.hi)
        tail = p.peek()
        if tail.kind != 'eof':
            raise ParseError(tail, f"体已经闭合，这里还剩 {describe(tail)}")
        if fd.body.close_line != d.hi:
            p.notes.append(f"体收口在第 {fd.body.close_line} 行，声明表的 hi = {d.hi}")
        return fd, p

    def code_line_numbers(self, d):
        """span 内的代码行（去掉空行/注释行），并标出嵌套 def 占的行。"""
        lines = [ln for ln in range(d.lo, d.hi + 1) if self.code_lines[ln - 1].strip()]
        nested = set()
        for ci in d.children:
            c = self.decls[ci]
            nested |= {ln for ln in range(c.lo, c.hi + 1)
                       if self.code_lines[ln - 1].strip()}
        return lines, nested


def covered_lines(fd):
    """产生过节点的源码行 + 每个块的收尾 '}' 行 —— 覆盖率的分母口径在这里。"""
    out = set()
    for n in walk(fd):
        out.add(n.src_line)
        cl = getattr(n, 'close_line', 0)
        if cl:
            out.add(cl)
    return out


def stats_for(fe, d, fd):
    code, nested = fe.code_line_numbers(d)
    own = [ln for ln in code if ln not in nested]
    cov = covered_lines(fd)
    nodes = list(walk(fd))
    exprs = [n for n in nodes if isinstance(n, Expr)]
    stmts = [n for n in nodes if isinstance(n, Stmt)]
    kinds = {}
    for n in nodes:
        kinds[type(n).__name__] = kinds.get(type(n).__name__, 0) + 1
    miss_own = [ln for ln in own if ln not in cov]
    miss_all = [ln for ln in code if ln not in cov]
    return {
        'def': d.name,
        'lo': d.lo, 'hi': d.hi,
        'stmts': len(stmts),
        'exprs': len(exprs),
        'nested_defs': sum(1 for n in nodes if isinstance(n, DefStmt)),
        'code_lines_own': len(own),
        'code_lines_nested': len(nested),
        'code_lines_span': len(code),
        'covered_own': len(own) - len(miss_own),
        'covered_ratio': round((len(own) - len(miss_own)) / len(own), 4) if own else 1.0,
        'uncovered_own': miss_own,
        'uncovered_span': miss_all,
        'node_kinds': kinds,
        'unsupported': [],
        'warnings': [],
    }


def print_stats(st, fe, detail):
    def p(msg=''):
        print(msg, file=sys.stderr)

    p(f"{fe.src_path} {st['def']}  L{st['lo']}-{st['hi']}")
    p(f"  语句 {st['stmts']} 条（含嵌套 def {st['nested_defs']} 个）；表达式节点 {st['exprs']} 个")
    p(f"  代码行 {st['code_lines_own']}（嵌套 def 另 {st['code_lines_nested']}，"
      f"span 合计 {st['code_lines_span']}）；覆盖 {st['covered_own']}/{st['code_lines_own']}"
      f" = {100.0 * st['covered_ratio']:.1f}%")
    if st['unsupported']:
        p(f"  未支持构造 {len(st['unsupported'])} 条：")
        for ln, text in st['unsupported']:
            p(f"    第 {ln} 行：{text[:100]}")
    else:
        p("  未支持构造：无（不认识的构造一律报错退出，不静默跳过；整文件的清单看 --all）")
    for w in st.get('warnings', []):
        p("  警告：" + w)
    if detail:
        if st['uncovered_own']:
            p(f"  未覆盖行（{len(st['uncovered_own'])}）：")
            for ln in st['uncovered_own'][:40]:
                p(f"    第 {ln} 行：{fe.raw_lines[ln - 1].strip()[:100]}")
        kinds = " ".join(f"{k}×{v}" for k, v in sorted(st['node_kinds'].items(),
                                                       key=lambda kv: -kv[1]))
        p("  节点：" + kinds)


# ---------------------------------------------------------------- CLI

def _where(e):
    """错误头：有列就给列，没有就只给行。"""
    return f"第 {e.line} 行" + (f" 第 {e.col} 列" if e.col else "")


def run_one(fe, args):
    cands = fe.candidates(args.name)
    if not cands:
        names = sorted({d.name for d in fe.decls if d.kind in ('def', 'label')})
        print(f"找不到 {args.name}；本文件里有：{', '.join(names)}", file=sys.stderr)
        return 1
    pick = next((k for k, c in enumerate(cands)
                 if c.body == 'braced' or c.kind == 'label'), 0)
    d = cands[pick]
    if len(cands) > 1:
        print(f"注意：{args.name} 有 {len(cands)} 处声明，用第 {pick + 1} 处"
              f"（L{d.lo}-{d.hi}）", file=sys.stderr)
    if d.kind == 'def' and d.body != 'braced':
        print(f"{args.name}（L{d.lo}）没有函数体，没什么可解析的", file=sys.stderr)
        return 1
    try:
        fd, p = fe.parse_def(d)
    except ParseError as e:
        line = fe.raw_lines[e.line - 1] if 0 < e.line <= len(fe.raw_lines) else ''
        print("错误：" + _where(e) + "：" + e.msg, file=sys.stderr)
        print(f"      {e.line} | {line}", file=sys.stderr)
        return 1
    except LexError as e:
        print("错误：" + _where(e) + "：" + e.msg, file=sys.stderr)
        return 1

    st = stats_for(fe, d, fd)
    st['warnings'] = p.notes
    if not args.quiet:
        print_stats(st, fe, args.stats)
    print(render_tree(fd))
    if args.json:
        out = {'source': args.src, 'def': d.name, 'form': fd.form, 'lo': d.lo, 'hi': d.hi,
               'stats': st, 'ast': node_to_dict(fd)}
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(out, f, ensure_ascii=False, indent=1)
            f.write("\n")
    return 0


def run_all(fe, args):
    print("\t".join(("name", "form", "lo", "hi", "reachable", "stmts", "exprs",
                     "code", "covered", "结果")))
    n_fail = n_live_fail = 0
    for d in fe.decls:
        if d.kind not in ('def', 'label') or (d.kind == 'def' and d.body != 'braced'):
            continue
        reach = '是' if (d.kind == 'label' or d.name in fe.live) else '否'
        try:
            fd, p = fe.parse_def(d)
        except (ParseError, LexError) as e:
            n_fail += 1
            if reach == '是':
                n_live_fail += 1
            print("\t".join((d.name, d.kind, str(d.lo), str(d.hi), reach, "-", "-", "-",
                             "-", f"失败 第 {e.line} 行：{e.msg[:80]}")))
            continue
        st = stats_for(fe, d, fd)
        print("\t".join((d.name, d.kind, str(d.lo), str(d.hi), reach, str(st['stmts']),
                         str(st['exprs']), str(st['code_lines_own']),
                         f"{st['covered_own']}/{st['code_lines_own']}",
                         "OK" if st['covered_ratio'] == 1.0 else "有未覆盖行")))
    if not args.quiet:
        print(f"共 {n_fail} 个 def 解析失败（其中可达 {n_live_fail} 个）", file=sys.stderr)
    return 1 if n_live_fail else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="语句 + 表达式 → AST（不生成 IR、不做类型检查）")
    ap.add_argument('--src', required=True, help=".si 源文件")
    ap.add_argument('--def', dest='name', help="要 dump 的 def 名（也接受 `run_sim` 那样的标签块）")
    ap.add_argument('--all', action='store_true',
                    help="体检全文件：每个有体的 def 一行（可达 def 失败 → 退出 1）")
    ap.add_argument('--json', help="把 AST + 统计写成 JSON（路径）")
    ap.add_argument('--stats', action='store_true', help="stderr 上多打详细统计")
    ap.add_argument('--quiet', action='store_true', help="不打印 stderr 统计")
    args = ap.parse_args(argv)
    if not args.all and not args.name:
        ap.error("要么给 --def <名字>，要么给 --all")
    try:
        fe = Frontend(args.src)
    except OSError as e:
        print(f"读不了 {args.src}：{e}", file=sys.stderr)
        return 2
    except LexError as e:
        print("错误：" + _where(e) + "：" + e.msg, file=sys.stderr)
        return 1
    return run_all(fe, args) if args.all else run_one(fe, args)


if __name__ == '__main__':
    sys.exit(main())
