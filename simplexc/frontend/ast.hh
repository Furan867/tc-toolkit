// Simplex 的语法树（M4.5 冻结接口）。
//
// 这是 parser 的产出、lowering 的输入，两者按本文件对齐；改字段 = 改契约，先通知对面的写者。
// 节点与字段**逐条对应** M3 冻结原型 `frontend/expr.py` 的 dataclass（那是语义 oracle）：
// 原型怎么表达的，这里就怎么表达 —— 包括 `TypeRef` 只存**源里的拼写**这一点（类型解析在 lowering 侧）。
//
// 为什么是「继承 + kind 枚举」而不是 std::variant：lowering 里有大量按节点类别分派（原型是 isinstance），
// variant 会把每处分派写成 visit + 重载集，反而离原型更远。kind 由构造函数写死、`as<T>()` 校验，
// 不需要 dynamic_cast。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace simplexc {

// ---------------------------------------------------------------------------
// 错误（接口冻结版 §1 / §8）
// ---------------------------------------------------------------------------
struct Span {
  int line = 0;
  int col = 0;
};

enum class ErrKind {
  UnsupportedConstruct,
  InternalError,
  ParseError,
};

struct CompileError {
  ErrKind kind = ErrKind::InternalError;
  std::string message;
  Span span;
};

// ---------------------------------------------------------------------------
// 类型**拼写**（`U33` / `Ptr` / `@Size` / `Arr[U64]`）
//
// 注意：这不是接口冻结版 §1 的 `Type`（那是解析后的类型标识，属于 lowering 的 `Ty`）。
// 名字解析、`@Size` 绑定、`Arr[T]` 的元素类型都要等到 lowering 拿到作用域之后才知道。
// ---------------------------------------------------------------------------
struct TypeRef;
using TypeRefPtr = std::unique_ptr<TypeRef>;

struct TypeRef {
  int src_line = 0;
  std::string name;       // "U64" / "Ptr" / "@Size" / "Arr"（Arr 的元素类型放 elem）
  bool at = false;        // 拼写里带 '@'
  TypeRefPtr elem;        // `Arr[U64]` 的 `U64`
};

// ---------------------------------------------------------------------------
// 节点
// ---------------------------------------------------------------------------
enum class NK {
  // 表达式
  Literal, Ident, Cast, Unary, Binary, Cond, Call, Member, Index, ArrayLit, CompositeLit, Template,
  // 辅助（不是 Expr 也不是 Stmt）
  FieldInit, Block, Param,
  // 语句
  VarDecl, Assign, ExprStmt, If, PpIf, While, Switch, Return, Break, Def,
  // 顶层函数/标签块
  Function,
  // 容器节点也各自一个类别：与父语句共用类别会让 as<T>() 把 IfStmt 认成 IfCase
  IfCase, SwitchCase,
};

struct Node {
  const NK kind;
  const int src_line;
  Node(NK k, int line) : kind(k), src_line(line) {}
  virtual ~Node() = default;
};

using NodePtr = std::unique_ptr<Node>;

struct Expr : Node {
  Expr(NK k, int line) : Node(k, line) {}
};
struct Stmt : Node {
  Stmt(NK k, int line) : Node(k, line) {}
};

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// 每个具体节点带 `static constexpr NK k`；`as<T>()` 用它校验，写错类别会返回 nullptr 而不是崩。

struct LiteralExpr : Expr {
  static constexpr NK k = NK::Literal;
  explicit LiteralExpr(int line) : Expr(k, line) {}
  std::string lit_kind;    // "int" | "str" | "template"
  std::string text;        // 源码内容（int 保留 0x/0b 与前缀下划线）
  std::string quote;       // 字符串/模板的定界符：'"' / '\'' / '`'（U-L3 未定，必须留着）
  bool has_value = false;  // =0 与「没有值」要分得开（原型用 int|None）
  int64_t value = 0;
};

struct IdentExpr : Expr {
  static constexpr NK k = NK::Ident;
  explicit IdentExpr(int line) : Expr(k, line) {}
  std::string name;  // 编译期常量带 '#' 前缀（`#SIMULATION_STATE`）
};

/* 带 `{…}` 插值的模板字面量。`texts` 是文字段（个数 = 插值个数 + 1，允许空串），
   `parts` 是插值里的表达式（按出现顺序）。
   无插值的模板**不走这里**：它仍是 `LiteralExpr{lit_kind="template"}` —— 那条路径的产物已验证；
   分成两个节点，是因为「有插值 / 没插值」在语法上确实是两种形状（前者要在 lowering 里拼字符串）。 */
struct TemplateExpr : Expr {
  static constexpr NK k = NK::Template;
  explicit TemplateExpr(int line) : Expr(k, line) {}
  std::vector<std::string> texts;
  std::vector<ExprPtr> parts;
};

// `U32 (expr)` / `U64 expr` / `S32 0` —— 显式类型转换，操作数只到 unary 级。
struct CastExpr : Expr {
  static constexpr NK k = NK::Cast;
  explicit CastExpr(int line) : Expr(k, line) {}
  TypeRefPtr target;
  ExprPtr expr;
};

struct UnaryExpr : Expr {
  static constexpr NK k = NK::Unary;
  explicit UnaryExpr(int line) : Expr(k, line) {}
  std::string op;
  ExprPtr expr;
};

struct BinaryExpr : Expr {
  static constexpr NK k = NK::Binary;
  explicit BinaryExpr(int line) : Expr(k, line) {}
  std::string op;
  ExprPtr left;
  ExprPtr right;
};

struct CondExpr : Expr {
  static constexpr NK k = NK::Cond;
  explicit CondExpr(int line) : Expr(k, line) {}
  ExprPtr cond;
  ExprPtr then_;
  ExprPtr else_;
};

struct CallExpr : Expr {
  static constexpr NK k = NK::Call;
  explicit CallExpr(int line) : Expr(k, line) {}
  ExprPtr callee;
  std::vector<ExprPtr> args;
  std::vector<TypeRefPtr> type_args;  // `load(<U64>, p)` 里的 `<U64>`
};

struct MemberExpr : Expr {
  static constexpr NK k = NK::Member;
  explicit MemberExpr(int line) : Expr(k, line) {}
  ExprPtr obj;
  std::string name;
};

struct IndexExpr : Expr {
  static constexpr NK k = NK::Index;
  explicit IndexExpr(int line) : Expr(k, line) {}
  ExprPtr obj;
  ExprPtr index;
};

struct ArrayLitExpr : Expr {
  static constexpr NK k = NK::ArrayLit;
  explicit ArrayLitExpr(int line) : Expr(k, line) {}
  std::vector<ExprPtr> items;
  int close_line = 0;
};

struct FieldInit : Node {
  static constexpr NK k = NK::FieldInit;
  explicit FieldInit(int line) : Node(k, line) {}
  std::string name;
  ExprPtr value;
};

struct CompositeLitExpr : Expr {
  static constexpr NK k = NK::CompositeLit;
  explicit CompositeLitExpr(int line) : Expr(k, line) {}
  TypeRefPtr typ;
  std::vector<std::unique_ptr<FieldInit>> fields;
  int close_line = 0;
};

struct Block : Node {
  static constexpr NK k = NK::Block;
  explicit Block(int line) : Node(k, line) {}
  std::vector<StmtPtr> stmts;
  int close_line = 0;
};

struct Param : Node {
  static constexpr NK k = NK::Param;
  explicit Param(int line) : Node(k, line) {}
  std::string name;
  TypeRefPtr typ;  // 可为空（无类型参数的简写）
};

// ---- 语句 ----
struct VarDeclStmt : Stmt {
  static constexpr NK k = NK::VarDecl;
  explicit VarDeclStmt(int line) : Stmt(k, line) {}
  std::string kw;    // var | let | const
  std::string name;  // const 带 '#' 前缀
  ExprPtr init;
};

struct AssignStmt : Stmt {
  static constexpr NK k = NK::Assign;
  explicit AssignStmt(int line) : Stmt(k, line) {}
  ExprPtr target;
  std::string op;  // '=' / '+=' / '-=' / '&=' / …
  ExprPtr value;
};

struct ExprStmt : Stmt {
  static constexpr NK k = NK::ExprStmt;
  explicit ExprStmt(int line) : Stmt(k, line) {}
  ExprPtr expr;
};

struct IfCase : Node {
  static constexpr NK k = NK::IfCase;
  explicit IfCase(int line) : Node(k, line) {}
  ExprPtr cond;
  std::unique_ptr<Block> block;
};

struct IfStmt : Stmt {
  static constexpr NK k = NK::If;
  explicit IfStmt(int line) : Stmt(k, line) {}
  std::vector<std::unique_ptr<IfCase>> cases;
  std::unique_ptr<Block> else_block;  // 可为空
};

// `#if <const-expr> { … }`（语料里唯一形态是 `#if in_scope("name")`）—— 真假判定是 U-L14。
struct PpIfStmt : Stmt {
  static constexpr NK k = NK::PpIf;
  explicit PpIfStmt(int line) : Stmt(k, line) {}
  std::vector<std::unique_ptr<IfCase>> cases;
  std::unique_ptr<Block> else_block;
};

struct WhileStmt : Stmt {
  static constexpr NK k = NK::While;
  explicit WhileStmt(int line) : Stmt(k, line) {}
  ExprPtr cond;
  std::unique_ptr<Block> block;
  std::string label;  // 空 = 无标签
};

struct SwitchCase : Node {
  static constexpr NK k = NK::SwitchCase;
  explicit SwitchCase(int line) : Node(k, line) {}
  std::unique_ptr<Block> block;
  ExprPtr value;  // 空 = default
};

struct SwitchStmt : Stmt {
  static constexpr NK k = NK::Switch;
  explicit SwitchStmt(int line) : Stmt(k, line) {}
  ExprPtr subject;
  std::vector<std::unique_ptr<SwitchCase>> cases;
};

struct ReturnStmt : Stmt {
  static constexpr NK k = NK::Return;
  explicit ReturnStmt(int line) : Stmt(k, line) {}
  ExprPtr value;  // 可为空
};

struct BreakStmt : Stmt {
  static constexpr NK k = NK::Break;
  explicit BreakStmt(int line) : Stmt(k, line) {}
  std::string label;  // 空 = 无标签
};

struct DefStmt : Stmt {  // 函数体里的嵌套 def
  static constexpr NK k = NK::Def;
  explicit DefStmt(int line) : Stmt(k, line) {}
  std::string name;
  std::vector<std::unique_ptr<Param>> params;
  TypeRefPtr ret;
  std::unique_ptr<Block> body;
};

struct FunctionDef : Node {
  static constexpr NK k = NK::Function;
  explicit FunctionDef(int line) : Node(k, line) {}
  std::string name;
  std::string form;  // "def" | "label"（`run_sim:` 顶层块）
  std::vector<std::unique_ptr<Param>> params;
  TypeRefPtr ret;
  std::unique_ptr<Block> body;
  int lo = 0;  // 源码行范围（原型用 lo/hi 做可达性与记账）
  int hi = 0;
};

// 类别安全的向下取用：kind 不符返回 nullptr（原型里 isinstance 为假的分支）。
template <class T>
T* as(Node* n) {
  if (n != nullptr && n->kind == T::k) return static_cast<T*>(n);
  return nullptr;
}
template <class T>
const T* as(const Node* n) {
  if (n != nullptr && n->kind == T::k) return static_cast<const T*>(n);
  return nullptr;
}

}  // namespace simplexc
