#include "parser.hh"

#include <array>
#include <cstdint>
#include <map>
#include <utility>

namespace simplexc {
namespace {

const std::array<const char*, 4>& unary_ops() {
  static const std::array<const char*, 4> v = {"-", "+", "~", "!"};
  return v;
}

/* 优先级只排语法（Nim 的次序：`&` 比 `==` 紧，实测语料靠它）；不做结合性以外的任何重排。 */
const std::map<std::string, int>& prec() {
  static const std::map<std::string, int> v = {
      {"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<", 4}, {"<=", 4},
      {">", 4},  {">=", 4}, {"|", 5},  {"^", 6},  {"&", 7}, {"<<", 8},
      {">>", 8}, {"+", 9},  {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}};
  return v;
}

const std::set<std::string>& stmt_words() {
  static const std::set<std::string> v = {
      "if",     "elif",   "else",    "while", "switch", "case", "default", "return",
      "break",  "let",    "var",     "const", "def",    "for",  "loop",    "type",
      "continue", "when", "try",     "discard"};
  return v;
}

const std::set<std::string>& unsupported_stmt_words() {
  static const std::set<std::string> v = {"for", "loop", "type", "continue", "when", "try",
                                          "discard"};
  return v;
}

const std::set<std::string>& type_builtin() {
  static const std::set<std::string> v = {"Int",  "Bool", "Str",  "None", "Time", "Ptr",
                                          "Seed", "Void", "Uint", "Sint", "Arr"};
  return v;
}

/* 原型 `TYPE_PAT = ^[US]\d+$`。 */
bool is_sized_type_spelling(const std::string& name) {
  if (name.size() < 2) return false;
  if (name[0] != 'U' && name[0] != 'S') return false;
  for (std::size_t i = 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

bool is_assign_op(const std::string& text) {
  static const std::set<std::string> v = {"=",   "+=", "-=", "*=", "/=",  "%=",
                                          "&=",  "|=", "^=", "<<=", ">>="};
  return v.count(text) != 0;
}

bool is_unary_op(const std::string& text) {
  for (const char* op : unary_ops()) {
    if (text == op) return true;
  }
  return false;
}

/* 原型 `int(t.text.replace('_',''), 0)`。语料里都是合法字面量；进制前缀之外按十进制。 */
int64_t parse_int_literal(const std::string& text) {
  std::string s;
  for (char c : text) {
    if (c != '_') s.push_back(c);
  }
  std::size_t i = 0;
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
    base = 2;
    i = 2;
  }
  uint64_t v = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    int d = 0;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    }
    if (d >= base) break;
    v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
  }
  return static_cast<int64_t>(v);
}

ParseError err_tok(const Tok& t, std::string msg) {
  return ParseError{t.line, t.col, std::move(msg)};
}
ParseError err_line(int line, std::string msg) { return ParseError{line, 0, std::move(msg)}; }

}  // namespace

std::string_view node_py_name(NK kind) {
  switch (kind) {
    case NK::Literal: return "LiteralExpr";
    case NK::Ident: return "IdentExpr";
    case NK::Cast: return "CastExpr";
    case NK::Unary: return "UnaryExpr";
    case NK::Binary: return "BinaryExpr";
    case NK::Cond: return "CondExpr";
    case NK::Call: return "CallExpr";
    case NK::Member: return "MemberExpr";
    case NK::Index: return "IndexExpr";
    case NK::ArrayLit: return "ArrayLitExpr";
    case NK::CompositeLit: return "CompositeLitExpr";
    case NK::Template: return "TemplateExpr";
    case NK::FieldInit: return "FieldInit";
    case NK::Block: return "Block";
    case NK::Param: return "Param";
    case NK::VarDecl: return "VarDeclStmt";
    case NK::Assign: return "AssignStmt";
    case NK::ExprStmt: return "ExprStmt";
    case NK::If: return "IfStmt";
    case NK::PpIf: return "PpIfStmt";
    case NK::While: return "WhileStmt";
    case NK::Switch: return "SwitchStmt";
    case NK::Return: return "ReturnStmt";
    case NK::Break: return "BreakStmt";
    case NK::Def: return "DefStmt";
    case NK::BlockStmt: return "BlockStmt";
    case NK::Function: return "FunctionDef";
    case NK::IfCase: return "IfCase";
    case NK::SwitchCase: return "SwitchCase";
  }
  return "Node";
}

bool is_expr_kind(NK kind) {
  switch (kind) {
    case NK::Literal:
    case NK::Ident:
    case NK::Cast:
    case NK::Unary:
    case NK::Binary:
    case NK::Cond:
    case NK::Call:
    case NK::Member:
    case NK::Index:
    case NK::ArrayLit:
    case NK::CompositeLit:
    case NK::Template:
      return true;
    default:
      return false;
  }
}

bool is_stmt_kind(NK kind) {
  switch (kind) {
    case NK::VarDecl:
    case NK::Assign:
    case NK::ExprStmt:
    case NK::If:
    case NK::PpIf:
    case NK::While:
    case NK::Switch:
    case NK::Return:
    case NK::Break:
    case NK::Def:
    case NK::BlockStmt:
      return true;
    default:
      return false;
  }
}

std::string_view Parser::kind_name(const Expr& e) { return node_py_name(e.kind); }

// ---------------------------------------------------------------- token 操作

Parser::Parser(std::vector<Tok> toks, std::set<std::string> type_names)
    : toks_(std::move(toks)), type_names_(std::move(type_names)) {}

Tok Parser::peek(int k) const {
  const int j = i_ + k;
  if (j >= 0 && j < static_cast<int>(toks_.size())) return toks_[static_cast<std::size_t>(j)];
  return toks_.back();
}

Tok Parser::next() {
  const Tok t = peek();
  if (i_ < static_cast<int>(toks_.size())) ++i_;
  return t;
}

bool Parser::at(const std::string& kind, const std::string& text, int k) const {
  const Tok t = peek(k);
  return t.kind == kind && (text.empty() || t.text == text);
}

bool Parser::at_eof() const { return peek().kind == "eof"; }

Tok Parser::expect(const std::string& kind, const std::string& text, const std::string& what) {
  const Tok t = peek();
  if (!at(kind, text)) throw err_tok(t, "这里要 " + what + "，看到 " + describe(t));
  return next();
}

/* 收尾定界符：报错时把「哪一个 '(' 没配对」也带上，只报行号不够用。 */
Tok Parser::close_tok(const std::string& text, const Tok& open_tok) {
  if (at("op", text)) return next();
  const Tok t = peek();
  throw err_tok(t, "这里要 '" + text + "'（配对第 " + std::to_string(open_tok.line) + " 行的 '" +
                       open_tok.text + "'），看到 " + describe(t));
}

bool Parser::is_type_name(const std::string& name) const {
  return type_names_.count(name) != 0 || type_builtin().count(name) != 0 ||
         is_sized_type_spelling(name);
}

// ---------------------------------------------------------------- 类型

TypeRefPtr Parser::parse_type_ref() {
  const Tok t = peek();
  if (t.kind != "ident" && t.kind != "at") {
    throw err_tok(t, "这里要类型名，看到 " + describe(t));
  }
  next();
  return type_ref_after(t);
}

TypeRefPtr Parser::type_ref_after(const Tok& t) {
  auto tr = std::make_unique<TypeRef>();
  tr->src_line = t.line;
  tr->name = t.text;
  tr->at = (t.kind == "at");
  if (at("op", "[")) {
    next();
    tr->elem = parse_type_ref();
    expect("op", "]", "']'");
  }
  return tr;
}

// ---------------------------------------------------------------- def 头 / 参数

void Parser::parse_def_head(std::string& name, std::vector<std::unique_ptr<Param>>& params,
                            TypeRefPtr& ret) {
  name = expect("ident", "", "函数名").text;
  params = parse_params();
  ret = nullptr;
  if (!at("op", "{")) ret = parse_type_ref();
}

std::vector<std::unique_ptr<Param>> Parser::parse_params() {
  expect("op", "(", "'('");
  std::vector<std::unique_ptr<Param>> params;
  if (at("op", ")")) {
    next();
    return params;
  }
  while (true) {
    const Tok n = expect("ident", "", "参数名");
    if (at("op", "=")) throw err_tok(peek(), "参数默认值未支持");
    if (!at("op", ":")) throw err_tok(peek(), "参数 '" + n.text + "' 缺类型标注");
    next();
    auto p = std::make_unique<Param>(n.line);
    p->name = n.text;
    p->typ = parse_type_ref();
    params.push_back(std::move(p));
    if (at("op", ",")) {
      next();
      continue;
    }
    expect("op", ")", "')'");
    return params;
  }
}

// ---------------------------------------------------------------- 语句

std::unique_ptr<Block> Parser::parse_block() {
  const Tok t = peek();
  if (!at("op", "{")) {
    const std::string hint = at("op", "=") ? "——'=' 是赋值，比较要写 '=='" : "";
    throw err_tok(t, "这里要 '{'（块开始），看到 " + describe(t) + hint);
  }
  const Tok open_tok = next();
  auto blk = std::make_unique<Block>(open_tok.line);
  while (!at("op", "}")) {
    if (peek().kind == "eof") {
      throw err_tok(peek(), "还没闭合就结束了：第 " + std::to_string(open_tok.line) +
                                " 行的 '{' 没有配对的 '}'");
    }
    blk->stmts.push_back(parse_statement());
  }
  const Tok close = next();
  blk->close_line = close.line;
  return blk;
}

std::unique_ptr<Stmt> Parser::parse_statement() {
  const Tok t = peek();
  if (t.kind == "hash" && t.text == "if") return parse_ppif();
  if (t.kind == "op" && t.text == "{") {
    auto bs = std::make_unique<BlockStmt>(t.line);
    bs->block = parse_block();
    return bs;
  }
  if (t.kind == "ident") {
    const std::string& w = t.text;
    if (w == "var" || w == "let" || w == "const") return parse_var(t);
    if (w == "if") return parse_if(t);
    if (w == "while") return parse_while(t, "");
    if (w == "switch") return parse_switch(t);
    if (w == "return") return parse_return(t);
    if (w == "break") return parse_break(t);
    if (w == "def") {
      next();
      std::string name;
      std::vector<std::unique_ptr<Param>> params;
      TypeRefPtr ret;
      parse_def_head(name, params, ret);
      auto d = std::make_unique<DefStmt>(t.line);
      d->name = name;
      d->params = std::move(params);
      d->ret = std::move(ret);
      d->body = parse_block();
      return d;
    }
    if (w == "elif" || w == "else") throw err_tok(t, "'" + w + "' 前面没有 'if'");
    if (w == "case" || w == "default") throw err_tok(t, "'" + w + "' 前面没有 'switch'");
    if (unsupported_stmt_words().count(w)) throw err_tok(t, "语句 '" + w + "' 未支持");
    const Tok n1 = peek(1), n2 = peek(2);
    if (n1.kind == "op" && n1.text == ":" && n1.line == t.line && n2.kind == "ident" &&
        n2.text == "while") {
      next();
      next();
      return parse_while(peek(), w);
    }
  }
  return parse_assign_or_expr();
}

std::unique_ptr<Stmt> Parser::parse_var(const Tok& t) {
  next();
  std::string name;
  if (t.text == "const") {
    const Tok n = expect("hash", "", "编译期常量名 '#NAME'");
    name = "#" + n.text;
  } else {
    name = expect("ident", "", "变量名").text;
  }
  expect("op", "=", "'='");
  auto v = std::make_unique<VarDeclStmt>(t.line);
  v->kw = t.text;
  v->name = name;
  v->init = parse_expr();
  return v;
}

std::unique_ptr<Stmt> Parser::parse_assign_or_expr() {
  ExprPtr e = parse_expr();
  const Tok t = peek();
  if (t.kind == "op" && is_assign_op(t.text)) {
    next();
    if (e->kind != NK::Ident && e->kind != NK::Index && e->kind != NK::Member) {
      throw err_tok(t, "赋值左边不是变量 / 下标 / 字段");
    }
    auto a = std::make_unique<AssignStmt>(e->src_line);
    a->op = t.text;
    a->value = parse_expr();
    a->target = std::move(e);
    return a;
  }
  if (e->kind != NK::Call) {
    throw err_line(e->src_line, "语句位置出现没有副作用的表达式（" +
                                    std::string(kind_name(*e)) +
                                    "）——是不是把 '=' 写成了 '=='？");
  }
  auto s = std::make_unique<ExprStmt>(e->src_line);
  s->expr = std::move(e);
  return s;
}

std::unique_ptr<Stmt> Parser::parse_if(const Tok& t) {
  next();
  auto st = std::make_unique<IfStmt>(t.line);
  auto c0 = std::make_unique<IfCase>(t.line);
  c0->cond = parse_expr();
  c0->block = parse_block();
  st->cases.push_back(std::move(c0));
  while (at("ident", "elif")) {
    const Tok et = next();
    auto c = std::make_unique<IfCase>(et.line);
    c->cond = parse_expr();
    c->block = parse_block();
    st->cases.push_back(std::move(c));
  }
  if (at("ident", "else")) {
    next();
    if (!at("op", "{")) {
      throw err_tok(peek(), "'else' 后面要 '{'（`else if` 请写 'elif'）");
    }
    st->else_block = parse_block();
  }
  return st;
}

/* `#if <const-expr> { … }`：`else`/`elif` 可能写成裸字（语料两种都有）。
   绑定规则只有一条：`else`/`elif` 必须**紧跟在** `#if` 那块的 `}` 之后（下一个 token），
   所以 `if a { #if in_scope("b") {…} } else {…}` 里的 `else` 不会被这里抢走。 */
std::unique_ptr<Stmt> Parser::parse_ppif() {
  const Tok t = next();
  auto st = std::make_unique<PpIfStmt>(t.line);
  auto c0 = std::make_unique<IfCase>(t.line);
  c0->cond = parse_expr();
  c0->block = parse_block();
  st->cases.push_back(std::move(c0));
  while (at("hash", "elif") || at("ident", "elif")) {
    const Tok et = next();
    auto c = std::make_unique<IfCase>(et.line);
    c->cond = parse_expr();
    c->block = parse_block();
    st->cases.push_back(std::move(c));
  }
  if (at("hash", "else") || at("ident", "else")) {
    next();
    st->else_block = parse_block();
  }
  return st;
}

std::unique_ptr<Stmt> Parser::parse_while(const Tok& t, const std::string& label) {
  next();
  auto w = std::make_unique<WhileStmt>(t.line);
  w->cond = parse_expr();
  w->block = parse_block();
  w->label = label;
  return w;
}

std::unique_ptr<Stmt> Parser::parse_switch(const Tok& t) {
  next();
  auto sw = std::make_unique<SwitchStmt>(t.line);
  sw->subject = parse_expr();
  while (true) {
    const Tok c = peek();
    if (c.kind == "ident" && c.text == "case") {
      next();
      auto sc = std::make_unique<SwitchCase>(c.line);
      sc->value = parse_expr();
      sc->block = parse_block();
      sw->cases.push_back(std::move(sc));
    } else if (c.kind == "ident" && c.text == "default") {
      next();
      auto sc = std::make_unique<SwitchCase>(c.line);
      sc->block = parse_block();
      sw->cases.push_back(std::move(sc));
    } else {
      break;
    }
  }
  return sw;
}

std::unique_ptr<Stmt> Parser::parse_return(const Tok& t) {
  next();
  const Tok nxt = peek();
  if (nxt.kind == "eof" || at("op", "}") || nxt.line > t.line) {
    return std::make_unique<ReturnStmt>(t.line);
  }
  auto r = std::make_unique<ReturnStmt>(t.line);
  r->value = parse_expr();
  /* 参考编译器把 `return <expr>)` 里多出的那一个 `)` 静默吞掉（产物与删掉它逐字节相同），
     但同一形状写在语句位置它是 fatal（front_end.nim(2688,28) Missing open paren）——
     所以只在 `return` 这一处放宽，语句路径照旧由 parse_primary 报错。 */
  if (at("op", ")")) next();
  return r;
}

std::unique_ptr<Stmt> Parser::parse_break(const Tok& t) {
  next();
  std::string label;
  const Tok nxt = peek();
  if (nxt.kind == "ident" && nxt.line == t.line && !stmt_words().count(nxt.text)) {
    label = next().text;
  }
  auto b = std::make_unique<BreakStmt>(t.line);
  b->label = label;
  return b;
}

// ---------------------------------------------------------------- 表达式

std::unique_ptr<Expr> Parser::parse_expr() {
  ExprPtr e = parse_binary(1);
  if (at("op", "?")) {
    next();
    ExprPtr then_e = parse_expr();
    expect("op", ":", "':'（三目运算符）");
    auto c = std::make_unique<CondExpr>(e->src_line);
    c->then_ = std::move(then_e);
    c->else_ = parse_expr();
    c->cond = std::move(e);
    return c;
  }
  return e;
}

std::unique_ptr<Expr> Parser::parse_binary(int min_prec) {
  ExprPtr left = parse_unary();
  while (true) {
    const Tok t = peek();
    if (t.kind != "op") return left;
    auto it = prec().find(t.text);
    if (it == prec().end() || it->second < min_prec) return left;
    next();
    ExprPtr right = parse_binary(it->second + 1);
    auto b = std::make_unique<BinaryExpr>(left->src_line);
    b->op = t.text;
    b->right = std::move(right);
    b->left = std::move(left);
    left = std::move(b);
  }
}

std::unique_ptr<Expr> Parser::parse_unary() {
  const Tok t = peek();
  if (t.kind == "op" && is_unary_op(t.text)) {
    next();
    auto u = std::make_unique<UnaryExpr>(t.line);
    u->op = t.text;
    u->expr = parse_unary();
    return u;
  }
  if (cast_ahead()) {
    const Tok name_tok = next();
    auto c = std::make_unique<CastExpr>(name_tok.line);
    c->target = type_ref_after(name_tok);
    c->expr = parse_unary();
    return c;
  }
  return parse_postfix();
}

/* `T (expr)` / `T ident` / `T 0` 是转换，`f(x)` 是调用。三条判据：
   1. **并置**（`TestResult max(...)`）只有转换一种读法 → 转换；
   2. `name (` 靠「有没有空白」+ 类型名表消歧：紧邻的是调用（Nim 语法 `f (x)` 不是调用），
      有空隙且是类型名的是转换；
   3. `T ~x` / `T !x` 只有转换一种读法；`T -x` / `T +x` 靠类型名表（实测：语料里没有
      「类型名 + 二元加减」的写法，而 `S8 -8` 这种负字面量必须读成转换）。
   跨行的并置是两条语句，不算。 */
bool Parser::cast_ahead() {
  const Tok t = peek();
  if (t.kind != "ident" && t.kind != "at") return false;
  const Tok nxt = peek(1);
  if (nxt.line != t.line) return false;
  const bool is_type = is_type_name(t.text);
  if (nxt.kind == "op" && nxt.text == "(") {
    if (is_type) {
      if (!nxt.gap) {
        notes.push_back("第 " + std::to_string(t.line) + " 行：`" + t.text +
                        "(…)` 紧邻无空白，按类型转换解析（若其实是函数调用请改名）");
      }
      return true;
    }
    return false;
  }
  if (nxt.kind == "ident" || nxt.kind == "number" || nxt.kind == "string" ||
      nxt.kind == "template" || nxt.kind == "hash" || nxt.kind == "at") {
    return !(nxt.kind == "ident" && stmt_words().count(nxt.text));
  }
  if (nxt.kind == "op" && (nxt.text == "~" || nxt.text == "!")) return true;
  if (nxt.kind == "op" && (nxt.text == "+" || nxt.text == "-")) return is_type;
  if (nxt.kind == "op" && nxt.text == "[") return is_type; /* `Arr[U8] []`；`x [0]` 仍是下标 */
  return false;
}

std::unique_ptr<Expr> Parser::parse_postfix() {
  ExprPtr e = parse_primary();
  while (true) {
    const Tok t = peek();
    if (t.kind == "op" && t.text == "(") {
      next();
      std::vector<TypeRefPtr> type_args;
      std::vector<ExprPtr> args;
      parse_args(t, type_args, args);
      auto c = std::make_unique<CallExpr>(e->src_line);
      c->args = std::move(args);
      c->type_args = std::move(type_args);
      c->callee = std::move(e);
      e = std::move(c);
    } else if (t.kind == "op" && t.text == "[") {
      next();
      ExprPtr idx = parse_expr();
      expect("op", "]", "']'");
      auto ix = std::make_unique<IndexExpr>(e->src_line);
      ix->index = std::move(idx);
      ix->obj = std::move(e);
      e = std::move(ix);
    } else if (t.kind == "op" && t.text == ".") {
      next();
      const Tok n = expect("ident", "", "成员名");
      auto m = std::make_unique<MemberExpr>(e->src_line);
      m->name = n.text;
      m->obj = std::move(e);
      e = std::move(m);
    } else {
      return e;
    }
  }
}

/* `(` 已在调用点消费。`<T>` 类型实参可以出现在参数表里**任何位置**
   （`load(<U8>, p)`、`get_ram_value("Program", Int counter, <U8>)` 两种都在语料里）；尾随逗号也收。 */
void Parser::parse_args(const Tok& open_tok, std::vector<TypeRefPtr>& type_args,
                        std::vector<ExprPtr>& args) {
  while (true) {
    if (at("op", "<")) {
      const int save = i_;
      std::vector<TypeRefPtr> cand;
      try {
        cand = parse_type_arg_list();
      } catch (const ParseError&) {
        cand.clear(); /* 不是类型实参表 → 当比较运算，回退（notes 与原型一样不回滚） */
      }
      /* 类型实参表后面必须紧跟 ',' 或 ')'，否则那不是 `<T>` 而是比较运算 */
      if (!cand.empty() && (at("op", ",") || at("op", ")"))) {
        for (TypeRefPtr& t : cand) type_args.push_back(std::move(t));
        if (at("op", ",")) {
          next();
          continue;
        }
        next();
        return;
      }
      i_ = save;
    }
    if (at("op", ")")) {
      next();
      return;
    }
    args.push_back(parse_expr());
    if (at("op", ",")) {
      next();
      continue;
    }
    if (at("op", ")")) {
      next();
      return;
    }
    const Tok t = peek();
    throw err_tok(t, "这里要 ',' 或 ')'（配对第 " + std::to_string(open_tok.line) + " 行的 '('），看到 " +
                        describe(t));
  }
}

std::vector<TypeRefPtr> Parser::parse_type_arg_list() {
  expect("op", "<", "'<'");
  std::vector<TypeRefPtr> types;
  types.push_back(parse_type_ref());
  while (at("op", ",")) {
    next();
    types.push_back(parse_type_ref());
  }
  expect("op", ">", "'>'");
  return types;
}

std::unique_ptr<Expr> Parser::parse_primary() {
  const Tok t = peek();
  if (t.kind == "number") {
    next();
    auto l = std::make_unique<LiteralExpr>(t.line);
    l->lit_kind = "int";
    l->text = t.text;
    l->has_value = true;
    l->value = parse_int_literal(t.text);
    return l;
  }
  if (t.kind == "string") {
    next();
    auto l = std::make_unique<LiteralExpr>(t.line);
    l->lit_kind = "str";
    l->text = t.text;
    l->quote = t.quote;
    return l;
  }
  if (t.kind == "template") {
    next();
    if (t.interp) return parse_template(t);
    auto l = std::make_unique<LiteralExpr>(t.line);
    l->lit_kind = "template";
    l->text = t.text;
    l->quote = t.quote;
    return l;
  }
  if (t.kind == "hash") {
    next();
    auto id = std::make_unique<IdentExpr>(t.line);
    id->name = "#" + t.text;
    return id;
  }
  if (t.kind == "at") {
    throw err_tok(t, "'@" + t.text + "' 只能出现在类型位置");
  }
  if (t.kind == "op" && t.text == "(") {
    next();
    ExprPtr e = parse_expr();
    close_tok(")", t);
    return e;
  }
  if (t.kind == "op" && t.text == "[") return parse_array_literal();
  if (t.kind == "ident") {
    const Tok nxt = peek(1);
    if (nxt.kind == "op" && nxt.text == "{" && nxt.line == t.line && is_type_name(t.text)) {
      return parse_composite(t);
    }
    next();
    auto id = std::make_unique<IdentExpr>(t.line);
    id->name = t.text;
    return id;
  }
  throw err_tok(t, "表达式里出现意外的 " + describe(t));
}

std::unique_ptr<Expr> Parser::parse_template(const Tok& t) {
  /* 模板的 token 文本 = 反引号之间的**原文**（词法器不拆 `{…}`），所以这里自己拆：
     文字段进 `texts`，`{…}` 里的代码交给一个**子 Parser**（词法器原样复用）。
     模板不可能跨行（`scan_quoted` 遇换行即未闭合），所以子 token 只需把列号平移：
     源列 = 模板起始列 + 1（反引号本身）+ 片段起点之前的码点数。 */
  auto te = std::make_unique<TemplateExpr>(t.line);
  const std::string& text = t.text;
  std::size_t i = 0;
  while (true) {
    const std::size_t open = text.find('{', i);
    if (open == std::string::npos) {
      te->texts.push_back(text.substr(i));
      return te;
    }
    te->texts.push_back(text.substr(i, open - i));
    /* 找配对的 `}`：嵌套的 `{` 计数，引号里的花括号不算（`{'}'}`）。 */
    std::size_t j = open + 1;
    int depth = 1;
    while (j < text.size() && depth > 0) {
      const char c = text[j];
      if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
      } else if (c == '\'' || c == '"') {
        const std::size_t end = scan_quoted_at(text, j);
        if (end == std::string::npos) break;
        j = end;
        continue;
      }
      ++j;
    }
    if (depth > 0) {
      throw ParseError{t.line, t.col + 1 + code_points_between(text, 0, open),
                       "模板字面量的 '{' 没有配对的 '}'"};
    }
    const std::size_t close = j - 1; /* 指向 '}' */
    te->parts.push_back(parse_template_part(t, open + 1, close));
    i = close + 1;
  }
}

std::unique_ptr<Expr> Parser::parse_template_part(const Tok& t, std::size_t begin,
                                                  std::size_t end) {
  std::vector<Tok> sub = tokenize(std::string_view(t.text).substr(begin, end - begin));
  const int shift = t.col + code_points_between(t.text, 0, begin);
  for (Tok& s : sub) {
    s.line = t.line;
    s.col += shift;
  }
  Parser p(std::move(sub), type_names_);
  std::unique_ptr<Expr> e = p.parse_expr();
  if (!p.at_eof()) {
    const Tok extra = p.peek();
    throw ParseError{extra.line, extra.col,
                     "模板插值里多余的 " + describe(extra)};
  }
  return e;
}

std::unique_ptr<Expr> Parser::parse_array_literal() {
  const Tok t = next(); /* [ */
  auto lit = std::make_unique<ArrayLitExpr>(t.line);
  while (true) {
    if (at("op", "]")) { /* 空表或尾随逗号 */
      lit->close_line = next().line;
      return lit;
    }
    lit->items.push_back(parse_expr());
    if (at("op", ",")) {
      next();
      continue;
    }
    const Tok close = expect("op", "]", "']'");
    lit->close_line = close.line;
    return lit;
  }
}

std::unique_ptr<Expr> Parser::parse_composite(const Tok& t) {
  auto lit = std::make_unique<CompositeLitExpr>(t.line);
  lit->typ = type_ref_after(next());
  expect("op", "{", "'{'");
  while (true) {
    if (at("op", "}")) { /* 空字面量或尾随逗号 */
      lit->close_line = next().line;
      return lit;
    }
    const Tok n = expect("ident", "", "字段名");
    expect("op", ":", "':'");
    auto f = std::make_unique<FieldInit>(n.line);
    f->name = n.text;
    f->value = parse_expr();
    lit->fields.push_back(std::move(f));
    if (at("op", ",")) {
      next();
      continue;
    }
    const Tok close = expect("op", "}", "'}'");
    lit->close_line = close.line;
    return lit;
  }
}

}  // namespace simplexc
