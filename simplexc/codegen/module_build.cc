/* module_build.cc —— 声明表 + 已解析 def → `ModuleAst`（实现见头文件的「为什么这一层存在」）。
 *
 * 三段都照抄冻结原型的对应实现（语义依据见 `docs/facts/semantics.md`）：
 *   `scan_type`     ← `_scan_type`（`type X Enum [...]` / `type X Struct { ... }`）
 *   `parse_top_var` ← `parse_decl_stmt`（切出该声明的 token，交给 parser 当一条语句解析）
 *   `build_module`  ← `Program.__init__` 里那段按声明表拼 `ModuleAst` 的循环
 */
#include "module_build.hh"

#include <cctype>
#include <sstream>
#include <string>
#include <utility>

#include "../frontend/decls.hh"
#include "../frontend/parser.hh"

namespace simplexc {
namespace lower {
namespace {

/* 声明可能跨行，所以先拼成一个串再切。拼完行号就没了，而字段的类型名要在**它自己那一行**上报错，
 * 所以同时记下每段物理行在 `text` 里的起点。（`span.hh`：`line >= 1` 才算有位置，第 0 行不算。） */
struct JoinedDecl {
  std::string text;
  std::vector<size_t> seg_begin; /* 第 i 段 = 物理行 `lo + i` 在 `text` 里的起始偏移 */
  int lo = 1;

  int line_of(size_t pos) const {
    int line = lo;
    for (size_t i = 0; i < seg_begin.size() && seg_begin[i] <= pos; ++i) {
      line = lo + static_cast<int>(i);
    }
    return line;
  }
};

JoinedDecl join_lines(const std::vector<std::string>& lines, int lo, int hi) {
  JoinedDecl out;
  out.lo = lo;
  for (int i = lo - 1; i < hi && i < static_cast<int>(lines.size()); ++i) {
    if (i >= 0) {
      if (!out.text.empty()) out.text += ' ';
      out.seg_begin.push_back(out.text.size());
      out.text += lines[static_cast<size_t>(i)];
    }
  }
  return out;
}

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

/* 跳过空白（type 声明可能被摆成多行）。 */
size_t skip_ws(const std::string& s, size_t i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
  return i;
}

std::string read_ident(const std::string& s, size_t& i) {
  size_t b = i;
  if (i < s.size() && is_ident_start(s[i])) {
    ++i;
    while (i < s.size() && is_ident_char(s[i])) ++i;
  }
  return s.substr(b, i - b);
}

/* 字段/成员的类型拼写：`U32` / `@Size`（接口 §1 的 `TypeRef` 只存拼写）。 */
std::unique_ptr<TypeRef> make_type_ref(const std::string& word, int line) {
  auto t = std::make_unique<TypeRef>();
  t->src_line = line;
  if (!word.empty() && word[0] == '@') {
    t->at = true;
    t->name = word.substr(1);
  } else {
    t->name = word;
  }
  return t;
}

/* 别名右手的类型拼写：`U64` / `@Size` / `Arr[Char]` / `Arr[Arr[U8]]`。
 * 与 `make_type_ref` 的区别：那个吃一个词，这个按 `Arr[…]` 递归到闭环。 */
std::unique_ptr<TypeRef> parse_type_spelling(const std::string& s, size_t& i, int line) {
  i = skip_ws(s, i);
  auto t = std::make_unique<TypeRef>();
  t->src_line = line;
  if (i < s.size() && s[i] == '@') {
    t->at = true;
    ++i;
  }
  t->name = read_ident(s, i);
  if (t->name.empty()) {
    throw CompileError{Responsibility::UserSource, Stage::Parse, Span{line}, "类型别名缺右手的类型：" + s};
  }
  i = skip_ws(s, i);
  if (i < s.size() && s[i] == '[') {
    ++i;
    t->elem = parse_type_spelling(s, i, line);
    i = skip_ws(s, i);
    if (i >= s.size() || s[i] != ']') {
      throw CompileError{Responsibility::UserSource, Stage::Parse, Span{line}, "类型别名的 '[' 没闭合：" + s};
    }
    ++i;
  }
  return t;
}

/* `type X Enum [A, B, C]` / `type X Struct { f: T, g: U }` / `type X <类型拼写>`。 */
void scan_type(const std::vector<std::string>& code_lines, const Decl& d, OwnedModule& out) {
  const JoinedDecl joined = join_lines(code_lines, d.lo, d.hi);
  const std::string& text = joined.text;
  if (d.note == "alias") {
    size_t i = skip_ws(text, 0);
    read_ident(text, i); /* `type` */
    i = skip_ws(text, i);
    read_ident(text, i); /* 别名自己 */
    AliasDecl a;
    a.name = d.name;
    a.target = out.keep_type(parse_type_spelling(text, i, d.lo));
    i = skip_ws(text, i);
    if (i < text.size()) {
      throw CompileError{Responsibility::UserSource, Stage::Parse, Span{d.lo},
                         "类型别名 " + d.name + " 的右手后面还有东西：" + text.substr(i)};
    }
    out.ast.aliases.push_back(a);
    return;
  }
  if (d.note == "Enum") {
    const size_t lb = text.find("Enum");
    const size_t l = lb == std::string::npos ? std::string::npos : text.find('[', lb);
    const size_t r = text.rfind(']');
    if (l == std::string::npos || r == std::string::npos || r < l) {
      throw CompileError{Responsibility::UserSource, Stage::Parse, Span{d.lo}, "枚举声明看不懂：" + text};
    }
    EnumDecl e;
    e.name = d.name;
    std::stringstream ss(text.substr(l + 1, r - l - 1));
    std::string item;
    while (std::getline(ss, item, ',')) {
      size_t i = skip_ws(item, 0);
      std::string mem = read_ident(item, i);
      if (!mem.empty()) e.members.push_back(mem);
    }
    out.ast.enums.push_back(std::move(e));
    return;
  }
  if (d.note == "Struct") {
    const size_t l = text.find('{');
    const size_t r = text.rfind('}');
    if (l == std::string::npos || r == std::string::npos || r < l) {
      throw CompileError{Responsibility::UserSource, Stage::Parse, Span{d.lo}, "结构声明看不懂：" + text};
    }
    StructDecl s;
    s.name = d.name;
    const std::string body = text.substr(l + 1, r - l - 1);
    size_t pos = 0;
    while (pos < body.size()) {
      size_t comma = body.find(',', pos);
      const std::string part =
          body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      size_t i = skip_ws(part, 0);
      std::string fname = read_ident(part, i);
      /* 空片段（尾随逗号、纯空白）照旧跳过；**有字段名却切不出类型**的不许再静默丢掉 ——
       * 静默丢字段等于悄悄改布局，后面按少一个字段的偏移读写字节（P4）。 */
      if (!fname.empty()) {
        const size_t after_name = skip_ws(part, i);
        if (after_name >= part.size() || part[after_name] != ':') {
          throw CompileError{Responsibility::UserSource, Stage::Parse,
                             Span{joined.line_of(l + 1 + pos + i)},
                             "结构 " + d.name + " 的字段 '" + fname + "' 缺类型（写成 '名字: 类型'）：" + part};
        }
        const bool at = after_name + 1 < part.size() && part[after_name + 1] == '@';
        i = skip_ws(part, after_name + 1);
        size_t j = i;
        if (j < part.size() && part[j] == '@') ++j;
        std::string tname = read_ident(part, j);
        if (tname.empty()) {
          throw CompileError{Responsibility::UserSource, Stage::Parse,
                             Span{joined.line_of(l + 1 + pos + after_name)},
                             "结构 " + d.name + " 的字段 '" + fname + "' 的类型名是空的：" + part};
        }
        s.fields.emplace_back(
            fname, out.keep_type(make_type_ref(at ? "@" + tname : tname,
                                              joined.line_of(l + 1 + pos + j))));
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    out.ast.structs.push_back(std::move(s));
    return;
  }
  throw CompileError{Responsibility::Unimplemented, Stage::Parse, Span{d.lo}, "type 声明形态（" + d.note + "）"};
}

/* 顶层 `var`/`const`：切出这个声明占的行上的 token，交给 parser 当一条语句解析
   （原型 `parse_decl_stmt` 就是这么做的 —— 不另写一份声明解析）。 */
std::unique_ptr<VarDeclStmt> parse_top_var(const Frontend& fe, const Decl& d) {
  std::vector<Tok> toks;
  for (const Tok& t : fe.toks) {
    if (t.line >= d.lo && t.line <= d.hi) toks.push_back(t);
  }
  Tok eof;
  eof.kind = "eof";
  eof.line = d.hi;
  eof.col = 1;
  eof.gap = true;
  toks.push_back(eof);
  Parser p(std::move(toks), fe.type_names);
  std::unique_ptr<Stmt> st = p.parse_statement();
  auto* vd = as<VarDeclStmt>(st.get());
  if (vd == nullptr) {
    throw CompileError{Responsibility::UserSource, Stage::Parse, Span{d.lo}, "顶层 " + d.kind + " 不是 var/const 声明"};
  }
  return std::unique_ptr<VarDeclStmt>(static_cast<VarDeclStmt*>(st.release()));
}

/* 把 `ParsedDef::notes` 搬进 `OwnedModule::notes`，带上「哪一段源码」的坐标。
 * 为什么两个分支（def / 程序根 label）都要调：生产者 `Parser::notes` 两条路径都会写，
 * 只在 def 那条收就会把 `U8(3)` 这类藏在程序根里的 note 又静默丢掉。 */
void take_notes(OwnedModule& out, const Decl& d, const ParsedDef& pd) {
  for (const std::string& note : pd.notes) {
    out.notes.push_back(d.kind + " '" + d.name + "'（第 " + std::to_string(d.lo) + " 行）：" + note);
  }
}

/* `dot <名字>($接收者: T, …) R { … }` 的**头行**。
 * 为什么要在这里认：`decls.cc` 的 `match_decl` 只认 `def` / `type` / `const` / `var` / 标签 /
 * `call_conv` / `extern` / `#if`，而 `dot` 头同时不是其中任何一种 —— 它又开了一个 `{`，于是整条
 * 声明（连体）按「花括号体」被跳过，既不进声明表、也不报未支持（`scan` 只在 `depth == 0` 时记
 * `unsupported`，而这一行结束时 `depth` 已经是 1）。方法单元只能在这里补。
 * 只认顶层（`depth == 0`）：体里的同名行是另一回事，不是声明。 */
bool match_dot_head(const std::string& code, std::string& name) {
  size_t i = 0;
  while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) ++i;
  if (code.compare(i, 3, "dot") != 0) return false;
  i += 3;
  if (i < code.size() && (is_ident_char(code[i]) || code[i] == '$')) return false;
  const size_t after_kw = i;
  i = skip_ws(code, i);
  if (i == after_kw) return false; /* `dot` 后面必须有空白 */
  name = read_ident(code, i);
  if (name.empty()) return false;
  i = skip_ws(code, i);
  return i < code.size() && code[i] == '(';
}

/* 一个顶层 `dot` 单元在源码里的位置（头行 + 收口行 + 方法名）。 */
struct DotHead {
  int lo = 0;
  int hi = 0;
  std::string name;
};

/* 顶层 `dot` 头行，按源码顺序。`hi` 靠花括号配平（头行开的那一个 `{` 回到深度 0 的行）。 */
std::vector<DotHead> dot_heads(const std::vector<std::string>& code_lines) {
  std::vector<DotHead> out;
  int depth = 0;
  for (size_t i = 0; i < code_lines.size(); ++i) {
    const std::string& code = code_lines[i];
    std::string name;
    if (depth == 0 && match_dot_head(code, name)) {
      /* 从头行本身开始配平：它是唯一把深度从 0 推到 1 的那一行。 */
      int inner = 0;
      int hi = static_cast<int>(i) + 1;
      for (size_t j = i; j < code_lines.size(); ++j) {
        for (char c : code_lines[j]) {
          if (c == '{') {
            ++inner;
          } else if (c == '}' && inner > 0) {
            --inner;
          }
        }
        if (inner == 0) {
          hi = static_cast<int>(j) + 1;
          break;
        }
      }
      out.push_back(DotHead{static_cast<int>(i) + 1, hi, name});
    }
    for (char c : code) {
      if (c == '{') {
        ++depth;
      } else if (c == '}' && depth > 0) {
        --depth;
      }
    }
  }
  return out;
}

/* 把一个 `dot` 单元按 `def` 的形状解析（`dot` 关键字吃掉之后，`名字(形参) 返回类型 { 体 }`
 * 与 `def` 逐字相同，`parse_def_head` + `parse_block` 直接能接）。
 * 不回填声明表的 `Decl`：那种记录属于前端，而这里只需要一棵能交给 sema 的 AST。 */
FunctionDef* parse_dot_unit(const Frontend& fe, int lo, OwnedModule& out) {
  std::vector<Tok> toks;
  int last = lo;
  for (const Tok& t : fe.toks) {
    if (t.line >= lo) {
      toks.push_back(t);
      last = t.line;
    }
  }
  Tok eof;
  eof.kind = "eof";
  eof.line = last;
  eof.col = 1;
  eof.gap = true;
  toks.push_back(eof);
  Parser p(std::move(toks), fe.type_names);
  p.expect("ident", "dot", "'dot'");
  auto fd = std::make_unique<FunctionDef>(lo);
  std::string name;
  std::vector<std::unique_ptr<Param>> params;
  TypeRefPtr ret;
  p.parse_def_head(name, params, ret);
  fd->name = name;
  fd->form = "dot";
  fd->params = std::move(params);
  fd->ret = std::move(ret);
  fd->body = p.parse_block();
  fd->lo = lo;
  fd->hi = fd->body->close_line;
  for (const std::string& note : p.notes) {
    out.notes.push_back("dot '" + name + "'（第 " + std::to_string(lo) + " 行）：" + note);
  }
  return out.keep(std::move(fd));
}

}  // namespace

FunctionDef* OwnedModule::keep(std::unique_ptr<FunctionDef> fn) {
  fns.push_back(std::move(fn));
  return fns.back().get();
}

const TypeRef* OwnedModule::keep_type(std::unique_ptr<TypeRef> t) {
  refs.push_back(std::move(t));
  return refs.back().get();
}

OwnedModule build_module(const Frontend& fe, std::string source_name) {
  OwnedModule out;
  /* 顶层未知行（`decls.cc:479-484` 记进 `top_unsupported`）以前只写不读：那行既不进声明表、也不进
   * 解析器，于是「这不是 simplex 能认的一行」没有任何痕迹，文件照出 blob（P5）。98 份语料里
   * **0 份**走到这里（`work/tmp/s3/notes-p145.md`），所以改成显式失败：责任 = 未实现（分不清是
   * 用户拼错还是我们没接的顶层构造）、阶段 = 语法（声明扫描属前端）、span = 行号 + 原文。 */
  if (!fe.top_unsupported.empty()) {
    const std::pair<int, std::string>& first = fe.top_unsupported.front();
    throw CompileError{Responsibility::Unimplemented, Stage::Parse, Span{first.first},
                       "顶层这一行既不是声明也不是语句（未支持）：" + first.second};
  }
  /* `dot` 单元：声明扫描看不见它们（见 `match_dot_head` 的注释），所以在这里按顶层头行逐个
   * 解析。解析是**渴望**的：单元体里的语法错当场报，不等到调用点 —— 否则一个从未被调用的单元
   * 会静默沉掉，而它需要的语义（比如 `$` 形参）就没人知道还差着。 */
  const std::vector<DotHead> units = dot_heads(fe.code_lines);
  /* 可达性必须把单元体当**根**重算：`Frontend::live` 只从顶层语句与 `tc_main` 出发，而单元体不
   * 在声明表里 —— 否则 `dot next` 调的 `xorshift` 会被当不可达而整条丢掉（真语料里
   * `xorshift` 只有这一个调用点），到调用点就变成「未定义的函数」。 */
  LineSet roots = strict_root_lines(fe.decls, LineSet());
  for (const DotHead& h : units) {
    for (int ln = h.lo; ln <= h.hi; ++ln) roots.insert(ln);
  }
  const Closure live = closure(fe.decls, fe.code_lines, roots, false);
  for (size_t i = 0; i < fe.decls.size(); ++i) {
    const Decl& d = fe.decls[i];
    const int idx = static_cast<int>(i);
    if (d.kind == "type" && d.depth == 0) {
      scan_type(fe.code_lines, d, out);
    } else if (d.kind == "def" && d.depth == 0) {
      if (live.live.count(d.name) == 0) {
        /* 不可达：不解析、不进模块 —— 于是它体内的语法错不会报。这是**保真行为**（参考实现同样
         * 不报，`docs/notes/2026-10-18-m3-0-scope.md:298-305`），改成报错会与逐 tick 差分判据分叉。
         * 但「悄悄跳过」不行：名字与行号必须留痕，否则事后无从判断某个错是没报还是不存在。 */
        out.notes.push_back("不可达的 def '" + d.name + "'（第 " + std::to_string(d.lo) +
                            " 行）已跳过：体内语法/语义错按参考实现不报");
        continue;
      }
      ParsedDef pd = fe.parse_def(idx);
      /* 解析期 notes（`Parser::notes`：体收口行对不上、`U8(…)` 这种紧邻无空白的转换按类型转换
       * 解析…）以前随 `pd` 析构丢掉，唯一读者是开发工具 `ast_dump`。这里收进 `out.notes`，
       * 由壳带到 `hook.log`；不升级成错误（§4b D2：只有「我们跳过了某个语义」才算降级）。 */
      take_notes(out, d, pd);
      out.ast.defs.push_back(out.keep(std::move(pd.fn)));
    } else if (d.kind == "label") {
      if (out.ast.label != nullptr) {
        throw CompileError{Responsibility::UserSource, Stage::Parse, Span{}, "一个文件里有两个顶层语句块（程序根只能有一个）"};
      }
      ParsedDef pd = fe.parse_def(idx);
      take_notes(out, d, pd);
      out.ast.label = out.keep(std::move(pd.fn));
    } else if ((d.kind == "var" || d.kind == "const") && d.depth == 0) {
      out.vars.push_back(parse_top_var(fe, d));
      out.ast.top_vars.push_back(out.vars.back().get());
    } else if (d.kind == "callconv" || d.kind == "extern") {
      /* 元数据声明：`call_conv x86_64 windows_x64 (…)` 与 `extern windows_x64 kernel32`。
       * `extern` 现在**有**下游（登记库名 → sema 的 `kernel32.'Name'(…)` 认得它是库而不是
       * 未定义标识符），`call_conv` 仍无人读：我们的后端只用平台默认的 Win64 约定，而前缀声明
       * 的那一组（rcx/rdx/r8/r9 + 40 字节影子空间）与它就是同一套。 */
      if (d.kind == "extern") {
        out.ast.extern_libs.push_back(d.name);
      } else {
        out.notes.push_back(d.kind + " '" + d.name + "'（第 " + std::to_string(d.lo) +
                            " 行）已认形状但未产出代码：ABI 这一层还没接（后端用平台默认的 Win64）");
      }
    }
  }
  /* `dot` 单元：声明扫描看不见它们（见 `match_dot_head` 的注释），所以在这里按顶层头行逐个
   * 解析。解析是**渴望**的：单元体里的语法错当场报，不等到调用点 —— 否则一个从未被调用的单元
   * 会静默沉掉，而它需要的语义（比如 `$` 形参）就没人知道还差着。 */
  for (const DotHead& head : units) {
    /* 单元体里有我们**前端还不认**的语法时（前缀 `dot cstring` 的 `return <Cstring> result`
     * 就是：尖括号转换还没接）不能因此让整个文件编不过 —— 那种单元只要没人调就与产物无关。
     * 但也不能静默吞掉：记一条 note（壳会写进 hook.log），调用点仍然照实报「方法 .x() 未实现」。 */
    try {
      out.ast.dot_units.push_back(parse_dot_unit(fe, head.lo, out));
    } catch (const CompileError& e) {
      out.notes.push_back("dot '" + head.name + "'（第 " + std::to_string(head.lo) +
                          " 行）解析失败已跳过：" + e.detail);
    }
  }
  out.ast.source_name = std::move(source_name);
  return out;
}

}  // namespace lower
}  // namespace simplexc
