/* module_build.cc —— 声明表 + 已解析 def → `ModuleAst`（实现见头文件的「为什么这一层存在」）。
 *
 * 三段都照抄原型 `ir_build.py` 的对应实现（只读 oracle）：
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

std::string join_lines(const std::vector<std::string>& lines, int lo, int hi) {
  std::string out;
  for (int i = lo - 1; i < hi && i < static_cast<int>(lines.size()); ++i) {
    if (i >= 0) {
      if (!out.empty()) out += ' ';
      out += lines[static_cast<size_t>(i)];
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
std::unique_ptr<TypeRef> make_type_ref(const std::string& word) {
  auto t = std::make_unique<TypeRef>();
  if (!word.empty() && word[0] == '@') {
    t->at = true;
    t->name = word.substr(1);
  } else {
    t->name = word;
  }
  return t;
}

/* `type X Enum [A, B, C]` 与 `type X Struct { f: T, g: U }` —— 生成代码子集里只有这两种。 */
void scan_type(const std::vector<std::string>& code_lines, const Decl& d, OwnedModule& out) {
  const std::string text = join_lines(code_lines, d.lo, d.hi);
  if (d.note == "Enum") {
    const size_t lb = text.find("Enum");
    const size_t l = lb == std::string::npos ? std::string::npos : text.find('[', lb);
    const size_t r = text.rfind(']');
    if (l == std::string::npos || r == std::string::npos || r < l) {
      throw CompileFail("第 " + std::to_string(d.lo) + " 行：枚举声明看不懂：" + text);
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
      throw CompileFail("第 " + std::to_string(d.lo) + " 行：结构声明看不懂：" + text);
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
      if (!fname.empty()) {
        i = skip_ws(part, i);
        if (i < part.size() && part[i] == ':') {
          const bool at = i + 1 < part.size() && part[i + 1] == '@';
          i = skip_ws(part, i + 1);
          size_t j = i;
          if (j < part.size() && part[j] == '@') ++j;
          std::string tname = read_ident(part, j);
          if (!tname.empty()) {
            s.fields.emplace_back(fname, out.keep_type(make_type_ref(at ? "@" + tname : tname)));
          }
        }
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    out.ast.structs.push_back(std::move(s));
    return;
  }
  throw Unsupported("type 声明形态（" + d.note + "）", d.lo);
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
    throw CompileFail("第 " + std::to_string(d.lo) + " 行：顶层 " + d.kind + " 不是 var/const 声明");
  }
  return std::unique_ptr<VarDeclStmt>(static_cast<VarDeclStmt*>(st.release()));
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
  for (size_t i = 0; i < fe.decls.size(); ++i) {
    const Decl& d = fe.decls[i];
    const int idx = static_cast<int>(i);
    if (d.kind == "type" && d.depth == 0) {
      scan_type(fe.code_lines, d, out);
    } else if (d.kind == "def" && d.depth == 0) {
      if (fe.live.count(d.name) == 0) continue; /* 不可达：不解析、不进模块 */
      ParsedDef pd = fe.parse_def(idx);
      out.ast.defs.push_back(out.keep(std::move(pd.fn)));
    } else if (d.kind == "label") {
      if (out.ast.label != nullptr) {
        throw CompileFail("一个文件里有两个顶层语句块（程序根只能有一个）");
      }
      ParsedDef pd = fe.parse_def(idx);
      out.ast.label = out.keep(std::move(pd.fn));
    } else if ((d.kind == "var" || d.kind == "const") && d.depth == 0) {
      out.vars.push_back(parse_top_var(fe, d));
      out.ast.top_vars.push_back(out.vars.back().get());
    }
  }
  out.ast.source_name = std::move(source_name);
  return out;
}

}  // namespace lower
}  // namespace simplexc
