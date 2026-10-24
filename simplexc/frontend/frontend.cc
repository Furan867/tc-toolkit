#include "frontend.hh"

#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>

#include "../sema/errors.hh"
#include "parser.hh"

namespace simplexc {

using sema::CompileError;
using sema::Responsibility;
using sema::Span;
using sema::Stage;

namespace {

bool is_py_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
std::string strip(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && is_py_space(s[b])) ++b;
  while (e > b && is_py_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

/* 原型 `declared_type_names` 里的 `re.finditer(r':\s*(@?[A-Za-z_]\w*)', d.params)`：
   `\s*` 允许跨行（params 里没有换行），逐个匹配、只取第一个捕获组（去掉前导 '@'）。 */
void collect_param_type_names(const std::string& params, std::set<std::string>& out) {
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (params[i] != ':') continue;
    std::size_t j = i + 1;
    while (j < params.size() && is_py_space(params[j])) ++j;
    std::size_t k = j;
    if (k < params.size() && params[k] == '@') ++k;
    if (k >= params.size()) continue;
    const char c = params[k];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) continue;
    const std::size_t start = k;
    ++k;
    while (k < params.size()) {
      const char d = params[k];
      const bool word = (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') ||
                        (d >= '0' && d <= '9') || d == '_';
      if (!word) break;
      ++k;
    }
    out.insert(params.substr(start, k - start));
    i = k - 1;
  }
}

std::string lstrip_at(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && s[i] == '@') ++i;
  return s.substr(i);
}

/* 顶层 `run_sim:` 那一类块：body 的收口 '}' 由调用方决定（见 parse_def）。 */
std::unique_ptr<Block> label_block(Parser& p, int lo, int hi) {
  auto blk = std::make_unique<Block>(lo);
  while (!p.at_eof()) {
    if (p.at("op", "}")) { /* 块自己的收尾 '}' */
      const Tok close = p.next();
      blk->close_line = close.line;
      return blk;
    }
    blk->stmts.push_back(p.parse_statement());
  }
  blk->close_line = hi;
  return blk;
}

}  // namespace

std::set<std::string> declared_type_names(const std::vector<Decl>& decls) {
  std::set<std::string> names;
  for (const Decl& d : decls) {
    if (d.kind == "type") {
      names.insert(d.name);
    } else if (d.kind == "def") {
      collect_param_type_names(d.params, names);
      const std::string ret = strip(d.ret);
      if (!ret.empty() && ret != "-" && ret != "?") names.insert(lstrip_at(ret));
    }
  }
  return names;
}

namespace {

/* `TypeRef` 不是 Node（见 frontend.hh 的说明），所以它自己一条递归。 */
void walk_type_ref(const TypeRef* t, const TypeRefVisitor& on_type) {
  if (t == nullptr) return;
  on_type(*t);
  walk_type_ref(t->elem.get(), on_type);
}

}  // namespace

void walk_nodes(const Node* n, const NodeVisitor& on_node, const TypeRefVisitor& on_type) {
  if (n == nullptr) return;
  on_node(*n);
  auto walk_type = [&](const TypeRef* t) { walk_type_ref(t, on_type); };
  switch (n->kind) {
    case NK::Function: {
      const auto* f = static_cast<const FunctionDef*>(n);
      for (const std::unique_ptr<Param>& p : f->params) walk_nodes(p.get(), on_node, on_type);
      walk_type(f->ret.get());
      walk_nodes(f->body.get(), on_node, on_type);
      break;
    }
    case NK::Def: {
      const auto* f = static_cast<const DefStmt*>(n);
      for (const std::unique_ptr<Param>& p : f->params) walk_nodes(p.get(), on_node, on_type);
      walk_type(f->ret.get());
      walk_nodes(f->body.get(), on_node, on_type);
      break;
    }
    case NK::Param: {
      const auto* p = static_cast<const Param*>(n);
      walk_type(p->typ.get());
      break;
    }
    case NK::Literal:
    case NK::Ident: {
      break;
    }
    case NK::Return: {
      const auto* r = static_cast<const ReturnStmt*>(n);
      walk_nodes(r->value.get(), on_node, on_type);
      break;
    }
    case NK::Break:
      break;
    /* 裸块本身零条指令，但里面的语句必须递归进来 —— 这个 switch 的其它分支
       没有 default，漏掉它会让 covered_lines() 与可达闭包漏掉块内的一切。 */
    case NK::BlockStmt: {
      const auto* b = static_cast<const BlockStmt*>(n);
      walk_nodes(b->block.get(), on_node, on_type);
      break;
    }
    case NK::Cast: {
      const auto* c = static_cast<const CastExpr*>(n);
      walk_type(c->target.get());
      walk_nodes(c->expr.get(), on_node, on_type);
      break;
    }
    case NK::Unary: {
      const auto* u = static_cast<const UnaryExpr*>(n);
      walk_nodes(u->expr.get(), on_node, on_type);
      break;
    }
    case NK::Binary: {
      const auto* b = static_cast<const BinaryExpr*>(n);
      walk_nodes(b->left.get(), on_node, on_type);
      walk_nodes(b->right.get(), on_node, on_type);
      break;
    }
    case NK::Cond: {
      const auto* c = static_cast<const CondExpr*>(n);
      walk_nodes(c->cond.get(), on_node, on_type);
      walk_nodes(c->then_.get(), on_node, on_type);
      walk_nodes(c->else_.get(), on_node, on_type);
      break;
    }
    case NK::Call: {
      const auto* c = static_cast<const CallExpr*>(n);
      walk_nodes(c->callee.get(), on_node, on_type);
      for (const ExprPtr& a : c->args) walk_nodes(a.get(), on_node, on_type);
      for (const TypeRefPtr& t : c->type_args) walk_type(t.get());
      break;
    }
    case NK::Member: {
      const auto* m = static_cast<const MemberExpr*>(n);
      walk_nodes(m->obj.get(), on_node, on_type);
      break;
    }
    case NK::Index: {
      const auto* ix = static_cast<const IndexExpr*>(n);
      walk_nodes(ix->obj.get(), on_node, on_type);
      walk_nodes(ix->index.get(), on_node, on_type);
      break;
    }
    case NK::ArrayLit: {
      const auto* a = static_cast<const ArrayLitExpr*>(n);
      for (const ExprPtr& x : a->items) walk_nodes(x.get(), on_node, on_type);
      break;
    }
    case NK::FieldInit: {
      const auto* f = static_cast<const FieldInit*>(n);
      walk_nodes(f->value.get(), on_node, on_type);
      break;
    }
    case NK::CompositeLit: {
      const auto* c = static_cast<const CompositeLitExpr*>(n);
      walk_type(c->typ.get());
      for (const std::unique_ptr<FieldInit>& f : c->fields) walk_nodes(f.get(), on_node, on_type);
      break;
    }
    case NK::Template: {
      const auto* t = static_cast<const TemplateExpr*>(n);
      for (const ExprPtr& p : t->parts) walk_nodes(p.get(), on_node, on_type);
      break;
    }
    case NK::Block: {
      const auto* b = static_cast<const Block*>(n);
      for (const StmtPtr& s : b->stmts) walk_nodes(s.get(), on_node, on_type);
      break;
    }
    case NK::VarDecl: {
      const auto* v = static_cast<const VarDeclStmt*>(n);
      walk_nodes(v->init.get(), on_node, on_type);
      break;
    }
    case NK::Assign: {
      const auto* a = static_cast<const AssignStmt*>(n);
      walk_nodes(a->target.get(), on_node, on_type);
      walk_nodes(a->value.get(), on_node, on_type);
      break;
    }
    case NK::ExprStmt: {
      const auto* e = static_cast<const ExprStmt*>(n);
      walk_nodes(e->expr.get(), on_node, on_type);
      break;
    }
    case NK::If:
    case NK::PpIf: {
      const auto* st = static_cast<const IfStmt*>(n);
      for (const std::unique_ptr<IfCase>& c : st->cases) walk_nodes(c.get(), on_node, on_type);
      walk_nodes(st->else_block.get(), on_node, on_type);
      break;
    }
    case NK::IfCase: {
      const auto* c = static_cast<const IfCase*>(n);
      walk_nodes(c->cond.get(), on_node, on_type);
      walk_nodes(c->block.get(), on_node, on_type);
      break;
    }
    case NK::While: {
      const auto* w = static_cast<const WhileStmt*>(n);
      walk_nodes(w->cond.get(), on_node, on_type);
      walk_nodes(w->block.get(), on_node, on_type);
      break;
    }
    case NK::Switch: {
      const auto* s = static_cast<const SwitchStmt*>(n);
      walk_nodes(s->subject.get(), on_node, on_type);
      for (const std::unique_ptr<SwitchCase>& c : s->cases) walk_nodes(c.get(), on_node, on_type);
      break;
    }
    case NK::SwitchCase: {
      const auto* c = static_cast<const SwitchCase*>(n);
      walk_nodes(c->block.get(), on_node, on_type);
      walk_nodes(c->value.get(), on_node, on_type);
      break;
    }
  }
}

namespace {

int close_line_of(const Node& n) {
  switch (n.kind) {
    case NK::ArrayLit: return static_cast<const ArrayLitExpr&>(n).close_line;
    case NK::CompositeLit: return static_cast<const CompositeLitExpr&>(n).close_line;
    case NK::Block: return static_cast<const Block&>(n).close_line;
    case NK::BlockStmt: return static_cast<const BlockStmt&>(n).block->close_line;
    default: return 0;
  }
}

}  // namespace

std::set<int> covered_lines(const FunctionDef& fd) {
  std::set<int> out;
  walk_nodes(
      &fd,
      [&](const Node& n) {
        out.insert(n.src_line);
        const int cl = close_line_of(n);
        if (cl != 0) out.insert(cl);
      },
      [&](const TypeRef& t) { out.insert(t.src_line); });
  return out;
}

namespace {

/* 掩码 + 切行 + 声明扫描（**不分词**）—— 读文件与读内存源码共用的那一段。 */
ScannedSource scan_source_text(const std::string& text) {
  const Masked masked = mask_source(text);
  ScannedSource out;
  out.code_lines = split_lines(masked.code);
  out.raw_lines = split_lines(masked.raw);
  out.masked_raw = masked.raw;
  out.scan = scan(out.code_lines, out.raw_lines);
  return out;
}

/* 拿到 ScannedSource 之后的共同收尾（分词、名字表、可达闭包）。
   写成自由函数而不是私有成员：`Frontend` 的字段本来就都是公开的，而类声明里只该多出
   第二种构造 —— 公开接口（接口冻结版 §1）越窄越好。 */
void fill_frontend(Frontend& fe, ScannedSource scanned) {
  fe.code_lines = std::move(scanned.code_lines);
  fe.raw_lines = std::move(scanned.raw_lines);
  fe.decls = std::move(scanned.scan.decls);
  fe.decl_errors = std::move(scanned.scan.errors);
  fe.top_unsupported = std::move(scanned.scan.unsupported);

  fe.toks = tokenize(scanned.masked_raw);
  fe.type_names = declared_type_names(fe.decls);

  const Closure cl =
      closure(fe.decls, fe.code_lines, strict_root_lines(fe.decls, LineSet()), false);
  fe.by_name = cl.by_name;
  fe.live = cl.live;
}

}  // namespace

ScannedSource load_and_scan(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ReadError{"读不了 " + path + "：No such file or directory"};
  std::ostringstream buf;
  buf << in.rdbuf();
  return scan_source_text(buf.str());
}

Frontend::Frontend(const std::string& path) : src_path(path) {
  fill_frontend(*this, load_and_scan(path));
}

Frontend::Frontend(SrcText src) : src_path(kMemorySrcPath) {
  fill_frontend(*this, scan_source_text(src.text));
}

std::vector<int> Frontend::candidates(const std::string& name) const {
  std::vector<int> out;
  for (std::size_t i = 0; i < decls.size(); ++i) {
    if (decls[i].name == name && (decls[i].kind == "def" || decls[i].kind == "label")) {
      out.push_back(static_cast<int>(i));
    }
  }
  return out;
}

void Frontend::code_line_numbers(const Decl& d, std::vector<int>& lines,
                                 std::set<int>& nested) const {
  lines.clear();
  nested.clear();
  for (int ln = d.lo; ln <= d.hi; ++ln) {
    if (ln >= 1 && static_cast<std::size_t>(ln) <= code_lines.size() &&
        !strip(code_lines[ln - 1]).empty()) {
      lines.push_back(ln);
    }
  }
  for (int ci : d.children) {
    const Decl& c = decls[static_cast<std::size_t>(ci)];
    for (int ln = c.lo; ln <= c.hi; ++ln) {
      if (ln >= 1 && static_cast<std::size_t>(ln) <= code_lines.size() &&
          !strip(code_lines[ln - 1]).empty()) {
        nested.insert(ln);
      }
    }
  }
}

ParsedDef Frontend::parse_def(int decl_idx) const {
  const Decl& d = decls[static_cast<std::size_t>(decl_idx)];
  std::vector<Tok> toks_for_def;
  for (const Tok& t : toks) {
    if (d.lo <= t.line && t.line <= d.hi) toks_for_def.push_back(t);
  }
  if (toks_for_def.empty()) {
    Tok eof;
    eof.kind = "eof";
    eof.line = d.lo;
    eof.col = 1;
    eof.gap = true;
    throw CompileError{Responsibility::UserSource, Stage::Parse, Span{d.lo, 0},
                       "L" + std::to_string(d.lo) + "-" + std::to_string(d.hi) + " 里没有 token"};
  }
  {
    Tok eof;
    eof.kind = "eof";
    eof.line = d.hi;
    eof.col = 1;
    eof.gap = true;
    toks_for_def.push_back(eof);
  }

  Parser p(std::move(toks_for_def), type_names);
  ParsedDef out;
  if (d.kind == "label") {
    auto fd = std::make_unique<FunctionDef>(d.lo);
    fd->name = d.name;
    fd->form = "label";
    fd->body = label_block(p, d.lo, d.hi);
    fd->lo = d.lo;
    fd->hi = d.hi;
    out.fn = std::move(fd);
  } else {
    p.expect("ident", "def", "'def'");
    auto fd = std::make_unique<FunctionDef>(d.lo);
    std::string name;
    std::vector<std::unique_ptr<Param>> params;
    TypeRefPtr ret;
    p.parse_def_head(name, params, ret);
    fd->name = name;
    fd->form = "def";
    fd->params = std::move(params);
    fd->ret = std::move(ret);
    fd->body = p.parse_block();
    fd->lo = d.lo;
    fd->hi = d.hi;
    out.fn = std::move(fd);
  }
  const Tok tail = p.peek();
  if (tail.kind != "eof") {
    throw CompileError{Responsibility::UserSource, Stage::Parse, Span{tail.line, tail.col},
                       "体已经闭合，这里还剩 " + describe(tail)};
  }
  if (out.fn->body->close_line != d.hi) {
    p.notes.push_back("体收口在第 " + std::to_string(out.fn->body->close_line) +
                      " 行，声明表的 hi = " + std::to_string(d.hi));
  }
  out.notes = p.notes;
  return out;
}

Stats stats_for(const Frontend& fe, const Decl& d, const FunctionDef& fd) {
  Stats st;
  std::vector<int> code;
  std::set<int> nested;
  fe.code_line_numbers(d, code, nested);

  std::vector<int> own;
  for (int ln : code) {
    if (!nested.count(ln)) own.push_back(ln);
  }
  const std::set<int> cov = covered_lines(fd);

  st.def = d.name;
  st.lo = d.lo;
  st.hi = d.hi;

  int stmts = 0, exprs = 0, nested_defs = 0;
  auto count_kind = [&](const std::string& name) {
    for (auto& kv : st.node_kinds) {
      if (kv.first == name) {
        ++kv.second;
        return;
      }
    }
    st.node_kinds.emplace_back(name, 1);
  };
  walk_nodes(
      &fd,
      [&](const Node& n) {
        if (is_stmt_kind(n.kind)) ++stmts;
        if (is_expr_kind(n.kind)) ++exprs;
        if (n.kind == NK::Def) ++nested_defs;
        count_kind(std::string(node_py_name(n.kind)));
      },
      [&](const TypeRef&) { count_kind("TypeRef"); });
  st.stmts = stmts;
  st.exprs = exprs;
  st.nested_defs = nested_defs;

  st.code_lines_own = static_cast<int>(own.size());
  st.code_lines_nested = static_cast<int>(nested.size());
  st.code_lines_span = static_cast<int>(code.size());

  for (int ln : own) {
    if (!cov.count(ln)) st.uncovered_own.push_back(ln);
  }
  for (int ln : code) {
    if (!cov.count(ln)) st.uncovered_span.push_back(ln);
  }
  st.covered_own = static_cast<int>(own.size()) - static_cast<int>(st.uncovered_own.size());
  st.covered_ratio = 1.0;
  if (!own.empty()) {
    const double r = static_cast<double>(st.covered_own) / static_cast<double>(own.size());
    st.covered_ratio = std::nearbyint(r * 10000.0) / 10000.0;
  }
  return st;
}

}  // namespace simplexc
