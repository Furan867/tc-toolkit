// 语句 + 表达式 → AST（M4.5 的 C++17 产品实现）。
//
// 语义 oracle = 冻结原型 `frontend/expr.py` 的 `Parser`（只读）：方法一条对一条，判定顺序与
// 错误文本照抄（`ParseError` 的行号/列号进判据）。产出节点就是 `ast.hh` 冻结的那套类型。
//
// 四条保真纪律（与原型一致，不是风格）：
//   1. 每个节点带 `src_line`（1-based）。
//   2. `U32 (expr)` / `U64 expr` / `S32 0` 都是 `CastExpr`，**不消失**。
//   3. 解析期**不折叠任何运算**（`S32 0 - S32 7` 保持源里的形状）。
//   4. 运算符不重排、不化简。
//
// 故意不做：类型、作用域、重载、常量求值、`in_scope` 真假、泛型单态化、模板插值、
// 不可达 def（`decls.hh` 的纪律：只配平跳过）。遇到不认识的一律抛错，不猜语义。
#pragma once

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ast.hh"
#include "lexer.hh"

namespace simplexc {

/* 位置可以是 token，也可以只有行号（报「这条语句没有副作用」时就只有行号）。 */
struct ParseError {
  int line = 0;
  int col = 0;
  std::string msg;
};

/* 原型用 `type(e).__name__` 拼错误文本，所以「节点名」是报错的一部分，
   和 `ast_dump` 里的 JSON `kind` 字段是同一张表。 */
std::string_view node_py_name(NK kind);

/* `isinstance(n, Expr)` / `isinstance(n, Stmt)` 的等价物（统计用）。 */
bool is_expr_kind(NK kind);
bool is_stmt_kind(NK kind);

class Parser {
 public:
  Parser(std::vector<Tok> toks, std::set<std::string> type_names);

  /* 解析过程中的提醒（原型是 `Parser.notes`，最终进 stats.warnings）。 */
  std::vector<std::string> notes;

  // ---- token 操作（原型里 Frontend 也要用 at/expect/parse_statement，故是公开的）
  Tok peek(int k = 0) const;
  Tok next();
  bool at_eof() const;
  bool at(const std::string& kind, const std::string& text = std::string(), int k = 0) const;
  Tok expect(const std::string& kind, const std::string& text, const std::string& what);
  Tok close_tok(const std::string& text, const Tok& open_tok);

  // ---- 顶层（原型里由 Frontend 驱动的三条：label 块用 at/parse_statement，
  //      def 用 expect('def')+parse_def_head+parse_block）
  std::unique_ptr<Block> parse_block();
  std::unique_ptr<Stmt> parse_statement();
  void parse_def_head(std::string& name, std::vector<std::unique_ptr<Param>>& params,
                      TypeRefPtr& ret);

 private:
  std::vector<Tok> toks_;
  int i_ = 0;
  std::set<std::string> type_names_;

  bool is_type_name(const std::string& name) const;

  // ---- 类型
  TypeRefPtr parse_type_ref();
  TypeRefPtr type_ref_after(const Tok& t);

  // ---- 参数
  std::vector<std::unique_ptr<Param>> parse_params();

  // ---- 语句
  std::unique_ptr<Stmt> parse_var(const Tok& t);
  std::unique_ptr<Stmt> parse_assign_or_expr();
  std::unique_ptr<Stmt> parse_if(const Tok& t);
  std::unique_ptr<Stmt> parse_ppif();
  std::unique_ptr<Stmt> parse_while(const Tok& t, const std::string& label);
  std::unique_ptr<Stmt> parse_switch(const Tok& t);
  std::unique_ptr<Stmt> parse_return(const Tok& t);
  std::unique_ptr<Stmt> parse_break(const Tok& t);

  // ---- 表达式
  std::unique_ptr<Expr> parse_expr();
  std::unique_ptr<Expr> parse_binary(int min_prec);
  std::unique_ptr<Expr> parse_unary();
  bool cast_ahead();
  std::unique_ptr<Expr> parse_postfix();
  void parse_args(const Tok& open_tok, std::vector<TypeRefPtr>& type_args,
                  std::vector<ExprPtr>& args);
  std::vector<TypeRefPtr> parse_type_arg_list();
  std::unique_ptr<Expr> parse_primary();
  std::unique_ptr<Expr> parse_template(const Tok& t);
  std::unique_ptr<Expr> parse_template_part(const Tok& t, std::size_t begin, std::size_t end);
  std::unique_ptr<Expr> parse_array_literal();
  std::unique_ptr<Expr> parse_composite(const Tok& t);

  /* 表达式节点的 kind 名（报错文本里用） */
  static std::string_view kind_name(const Expr& e);
};

}  // namespace simplexc
