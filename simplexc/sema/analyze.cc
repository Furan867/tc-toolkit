/* sema/analyze.cc —— 已解析的模块 → HIR（骨架 + 类型解析 + 常量折叠 + 表达式 + 语句 + 布局）。
 *
 * 语义**全部**照 `lower.cc` 的现状搬（方案 §3 的「纯语义保持」）：`ty_of_name` /
 * `ty_of_ref` / `unify` / `arith_ty` / `convert` 的分派 / 常量求值 `cv` / `s_VarDeclStmt` 的
 * 「常量变量」规则 / `make_globals` 的顶层规则 / `s_IfStmt`+`in_scope_const`+`ppif_true` /
 * `s_SwitchStmt` / `array_literal` / `composite_literal` / `lvalue` 的四种形态 / `string_literal` /
 * `template_expr`。搬的时候只换**值的载体**：`lower` 的 `V{ll, ty, c}` 在这里是 `HNode` ——
 * 常量就是 `Const` 节点（`const_bits` 即原来的 `c`），运行期值就是它自己那个节点（没有指令句柄
 * 可用，也不需要：指令是 codegen 的事）。所以下面的表达式函数与 `lower.cc` 的同名函数逐句对应，
 * **改一处就要同时改另一处**（②c 删 `lower` 那一份）。
 *
 * 两处结构上的取舍（都是「值语义 / 可 diff」的要求，改之前先想清楚下游依赖）：
 *   1. `in_scope(…)` 在**这一层**判定并剪枝（`#if` 与裸 `if` 一视同仁，方案 §2b 补漏 3）：
 *      判假的块整块丢掉（连里面的名字都不解析），判真的块原样内联成一条 `Block` 语句
 *      （保留它那一层作用域，`lower_block` 就是这样的）。HIR 里**不出现** `in_scope`，也不折成
 *      `Const` —— 后者会让 codegen 分不清「真常量条件」（`if 1` 不许剪）。
 *   2. `if / elif / else` 链在 HIR 里是**嵌套**的（`else` 那一支是一个只装着里层 `If` 的 `Block`），
 *      因为 `If` 节点只有三个位（条件 / then / else）。作用域不变：`lower` 里每个 case 的块
 *      本来也是一层作用域。
 *
 * 三处容易踩空的地方（都在对应代码旁边写了）：
 *   1. 加宽只有 `zext`。`lower` 的运行期窄值一律是 `zext(trunc_W)` 的 64 位位模式，`convert`
 *      的加宽分支只补一层掩码 —— 按符号性扩的那一路只存在于**编译期常量**（`pat_const`），
 *      而常量在这里已经折成 `Const` 了 ⇒ `HCastKind::Sext` 在本步不会出现。
 *   2. `let` / `const`（非 `#`）的初值是常量时**没有节点**：原版编译器把它当常量本身，引用点
 *      直接是 `Const`（`lower.cc` 的 `s_VarDeclStmt`）。`const #X` 同理（零条指令）。
 *   3. 类型比较用 `same_val_ty`，不是指针相等 —— 类型不是 interned 的（`make_int` 每次都是
 *      新对象，只有 `ty_i64()` 那几个单例是同一个指针）。
 *
 * **本步仍有东西明确不做**：调用 / `def` / 嵌套 def / 闭包 / 泛型实例化（S2b-3）。遇到就走
 * `Unsupported`（带行号），不猜语义、不静默丢。
 *
 * 为什么 `While`/`Break` 在更早那一步就已经进来了：语料里程序根的**唯一**形态就是
 * `run_sim: while true { … }`（`decls.cc` 的 `match_label` 要求同一行跟 while/for/loop），
 * 不支持 `while` 就一个根块都建不出来。两者的结构逐字照 `lower.cc` 的 `s_WhileStmt` /
 * `s_BreakStmt`。
 */
#include "analyze.hh"

#include "errors.hh"
#include "layout.hh"

#include "../frontend/ast.hh"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace simplexc {
namespace sema {
namespace {

/* ---------------------------------------------------------------- 与 lower 同表的小件 */

/* 报错文本按原型 `type(node).__name__` 拼；与 `lower.cc` 的 `kind_name` 同一张表（②c 之后
 * 两边只剩一处）。 */
const char* ast_kind_name(NK k) {
    switch (k) {
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
        case NK::BlockStmt: return "BlockStmt";
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
        case NK::Function: return "FunctionDef";
        case NK::IfCase: return "IfCase";
        case NK::SwitchCase: return "SwitchCase";
    }
    return "?";
}

bool is_arith(const std::string& op) {
    static const std::set<std::string> kOps{"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"};
    return kOps.count(op) != 0;
}

bool is_cmp(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

HBinOp cmp_bin_op(const std::string& op) {
    if (op == "==") return HBinOp::Eq;
    if (op == "!=") return HBinOp::Ne;
    if (op == "<") return HBinOp::Lt;
    if (op == "<=") return HBinOp::Le;
    if (op == ">") return HBinOp::Gt;
    return HBinOp::Ge;
}

HBinOp arith_bin_op(const std::string& op) {
    if (op == "+") return HBinOp::Add;
    if (op == "-") return HBinOp::Sub;
    if (op == "*") return HBinOp::Mul;
    if (op == "/") return HBinOp::Div;
    if (op == "%") return HBinOp::Rem;
    if (op == "&") return HBinOp::BitAnd;
    if (op == "|") return HBinOp::BitOr;
    if (op == "^") return HBinOp::BitXor;
    if (op == "<<") return HBinOp::Shl;
    return HBinOp::Shr;
}

/* 复合赋值运算符 → 对应的二元运算符（与 `lower.cc` 的 `assign_binop` 同一张表）。 */
const std::map<std::string, std::string>& assign_binop() {
    static const std::map<std::string, std::string> m{
        {"+=", "+"}, {"-=", "-"}, {"*=", "*"}, {"/=", "/"}, {"%=", "%"},
        {"&=", "&"}, {"|=", "|"}, {"^=", "^"}, {"<<=", "<<"}, {">>=", ">>"}};
    return m;
}

/* 前缀（stdlib）里定义、生成源码当作「已存在」用的全局：`lower.cc` 的 `prefix_globals()` 只有
 * 这一个（`var global_seed = Seed 1`），而且是**按需建全局**的 —— 第一次被引用才进
 * `Hir.globals`，所以它的下标跟在顶层全局后面。`init` 就是前缀那一行给的初值。 */
struct PrefixGlobal {
    TyRef ty;
    uint64_t init = 0;
};

const std::map<std::string, PrefixGlobal>& prefix_globals() {
    static const std::map<std::string, PrefixGlobal> m{{"global_seed", {ty_seed(), 1}}};
    return m;
}

/* 前缀里的函数名：只给 `in_scope(…)` 的 ③ 档用（调用本身是 S2b-3 的事）。 */
bool is_prefix_func(const std::string& n) { return n == "get_time" || n == "sleep" || n == "memory_clear" ||
                                                  n == "memory_copy" || n == "thread_exit"; }

/* 源码里直接用、文件里没声明的编译期常量（与 `lower.cc` 的 `hash_consts` 同一份表）。 */
struct HashConst {
    uint64_t value = 0;
    TyRef ty;
};

const std::map<std::string, HashConst>& hash_consts() {
    static const std::map<std::string, HashConst> m{{"#U64_HIGH", {kMask64, make_int(64, false)}}};
    return m;
}

/* ---------------------------------------------------------------- 节点小件 */

Span span_at(int line) {
    Span s;
    s.line = line;
    return s;
}

HNode expr_node(HKind kind, TyRef ty, int line) {
    HNode n;
    n.kind = kind;
    n.ty = std::move(ty);
    n.span = span_at(line);
    return n;
}

HNode stmt_node(HKind kind, int line) {
    HNode n;
    n.kind = kind;
    n.span = span_at(line);
    return n;
}

/* `Const` 的位模式一律按 `ty` 截断（`hir.hh` 的约定）。调用点本来就都掩过，这里再掩一次，
 * 免得某条新路径漏掉 —— 它不会改变任何已经正确的值。 */
HNode const_node(const TyRef& ty, uint64_t bits, int line) {
    HNode n = expr_node(HKind::Const, ty, line);
    n.const_bits = is_int(*ty) ? (bits & mask_of(ty->bits)) : (bits & kMask64);
    return n;
}

HNode local_node(const std::string& name, TyRef ty, int line) {
    HNode n = expr_node(HKind::Local, std::move(ty), line);
    n.name = name;
    return n;
}

HNode global_node(int id, TyRef ty, int line) {
    HNode n = expr_node(HKind::Global, std::move(ty), line);
    n.global_id = id;
    return n;
}

HNode cast_node(HCastKind kind, HNode v, const TyRef& target, int line) {
    HNode n = expr_node(HKind::Cast, target, line);
    n.cast_kind = kind;
    n.kids.push_back(std::move(v));
    return n;
}

/* 常量形态下按**源类型符号性**扩展后的 64 位内容（`lower.cc` 的 `pat_const`；U-L7 的第一路）。
 * 非 `Const` 节点返回空 —— 「加宽按符号性扩」只对常量成立。 */
std::optional<uint64_t> pat_const(const HNode& n) {
    if (n.kind != HKind::Const) return std::nullopt;
    if (!is_int(*n.ty)) return n.const_bits & kMask64;
    uint64_t v = n.const_bits & mask_of(n.ty->bits);
    if (n.ty->is_signed && n.ty->bits < 64 && ((v >> (n.ty->bits - 1)) & 1) != 0) {
        v |= ~mask_of(n.ty->bits) & kMask64;
    }
    return v & kMask64;
}

/* 「位模式不用动、类型标签也不用换」—— 只有这时 `convert` 才什么都不插。
 * 判据只看位宽/符号性/`is_bool`（Int）与种类 + 名字（Ptr/Str/Enum/Struct；struct 的字段由
 * 名字唯一决定）。`Arr[...]` 不走它：两个 `Arr[U8]` 是不同对象（类型没有 interned），而且数组
 * 值的相等在参考里本来就没有定义（`unify` 对 `Arr` 直接报错）—— 所以它总是插一个 `Raw`。 */
bool same_val_ty(const TyRef& a, const TyRef& b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    if (a->kind == TyKind::Int) {
        return a->bits == b->bits && a->is_signed == b->is_signed && a->is_bool == b->is_bool;
    }
    if (a->kind == TyKind::Array) return false;
    return a->name == b->name;
}

/* `cv` 里「必须是常量」的断言：与 `lower.cc` 的 `need_const` 同名同义（内部失败，不是用户错）。 */
uint64_t need_const(const HNode& n, int line) {
    if (n.kind != HKind::Const) throw Unsupported("（内部）常量求值拿到非常量", line);
    return n.const_bits;
}

/* ---------------------------------------------------------------- 新代码的报错（方案 §2d）
 *
 * 两个薄封装：调用点写出来就是「哪一轴轴」而不是一个裸构造。三个责任档的判定见 `errors.hh`：
 * 认不出的源码构造 = `Unimplemented`，语法/类型错 = `UserSource`，内部不自洽 = `OurBug`。
 * 只在**本文件新写的那几个函数**里用；旧函数继续抛 `Unsupported`（一个函数只用一种）。 */
[[noreturn]] void unimplemented(const std::string& detail, int line) {
    throw CompileError(Responsibility::Unimplemented, Stage::Sema, span_at(line), detail);
}

[[noreturn]] void user_error(const std::string& detail, int line) {
    throw CompileError(Responsibility::UserSource, Stage::Sema, span_at(line), detail);
}

[[noreturn]] void our_bug(const std::string& detail, int line) {
    throw CompileError(Responsibility::OurBug, Stage::Sema, span_at(line), detail);
}

/* 程序根语句树里的声明（`lower.cc` 的 `collect_root_decls`）。两个后果都靠它：
 *   · `const #X` 写在根语句树里也是**整单元**可见的（引用点可以排在声明之前、可以在别的块里）；
 *   · 同处的嵌套 `def` 名算 `in_scope` 的 ① 档（U-L14 的 T4/T7/T8/T9）。
 * 范围照它逐句对应：`While`/`If`/`#if`/`Switch` 递归下去，**裸块（`BlockStmt`）不钻**、
 * `def` 体不钻。 */
void collect_root_decls(const Block& block, std::vector<const DefStmt*>& defs,
                        std::vector<const VarDeclStmt*>& consts) {
    for (const StmtPtr& st : block.stmts) {
        if (const auto* d = as<DefStmt>(st.get())) {
            defs.push_back(d);
        } else if (const auto* v = as<VarDeclStmt>(st.get())) {
            if (v->kw == "const" && !v->name.empty() && v->name[0] == '#') consts.push_back(v);
        } else if (const auto* w = as<WhileStmt>(st.get())) {
            collect_root_decls(*w->block, defs, consts);
        } else if (const auto* i = as<IfStmt>(st.get())) {
            for (const auto& c : i->cases) collect_root_decls(*c->block, defs, consts);
            if (i->else_block != nullptr) collect_root_decls(*i->else_block, defs, consts);
        } else if (const auto* p = as<PpIfStmt>(st.get())) {
            for (const auto& c : p->cases) collect_root_decls(*c->block, defs, consts);
            if (p->else_block != nullptr) collect_root_decls(*p->else_block, defs, consts);
        } else if (const auto* s = as<SwitchStmt>(st.get())) {
            for (const auto& c : s->cases) collect_root_decls(*c->block, defs, consts);
        }
    }
}

/* ---------------------------------------------------------------- 分析器 */

class Analyzer {
public:
    explicit Analyzer(Hir& out) : hir_(out) {}
    void run(const ModuleSource& src);

private:
    /* 名字绑定：本步只有「局部」与「编译期常量」两种（捕获 / 嵌套 def 是后续步骤的事）。 */
    struct Binding {
        enum class Kind { Local, Const };
        Kind kind = Kind::Local;
        TyRef ty;
        uint64_t bits = 0; /* Const：已按 ty 截断的位模式 */
    };

    struct GlobalRef {
        int id = -1;
        TyRef ty;
    };

    /* ---- 模块级 */
    void make_types(const ModuleSource& src);
    void scan_scope_names(const ModuleSource& src);
    void make_top_level(const std::vector<const VarDeclStmt*>& top_vars);
    void scan_root_consts(const Block& body);
    HNode root_const_value(const VarDeclStmt& decl);
    GlobalRef prefix_global(const std::string& name, int line);
    int intern_string(const std::string& bytes);

    /* ---- 类型 */
    TyRef ty_of_name(const std::string& name, int line);
    TyRef ty_of_ref(const TypeRef& ref, int line);
    TyRef unify(const TyRef& a, const TyRef& b, int line);
    TyRef arith_ty(const HNode& a, const HNode& b, const std::string& op, int line);

    /* ---- 表达式 */
    HNode e(const Expr& node);
    HNode e_literal(const LiteralExpr& node);
    HNode e_ident(const IdentExpr& node);
    HNode e_cast(const CastExpr& node);
    HNode e_unary(const UnaryExpr& node);
    HNode e_binary(const BinaryExpr& node);
    HNode e_cond(const CondExpr& node);
    HNode string_literal(const std::string& text, int line);
    HNode array_lit_node(const ArrayLitExpr& node);
    HNode composite_lit_node(const CompositeLitExpr& node);
    HNode template_expr(const TemplateExpr& node);
    HNode convert(HNode v, const TyRef& target, int line);
    HNode binop(const std::string& op, const HNode& a, const HNode& b, int line);
    HNode compare(const std::string& op, const HNode& a, const HNode& b, int line);
    HNode logical(const std::string& op, const HNode& a, const HNode& b, int line);
    HNode as_cond(const HNode& v, int line);

    /* 常量求值（`lower.cc` 的 `const_value` / `cv`）：只认常量表达式，其它一律 nullopt / throw。 */
    std::optional<HNode> const_value(const Node& node);
    HNode cv(const Node& node);

    /* ---- 语句 */
    HNode block(const Block& b);
    void statement(HNode& parent, const Stmt& st);
    void var_decl_stmt(HNode& parent, const VarDeclStmt& st);
    void assign_stmt(HNode& parent, const AssignStmt& st);
    void if_stmt(HNode& parent, const IfStmt& st);
    HNode if_node(const std::vector<const IfCase*>& cases, std::size_t at, const Block* else_block);
    void ppif_stmt(HNode& parent, const PpIfStmt& st);
    void switch_stmt(HNode& parent, const SwitchStmt& st);
    void while_stmt(HNode& parent, const WhileStmt& st);
    void break_stmt(HNode& parent, const BreakStmt& st);
    HNode return_stmt(const ReturnStmt& st);
    HNode lvalue(const Expr& node);
    HNode lvalue_ident(const IdentExpr& id);
    HNode index_place(const IndexExpr& ix);
    HNode member_place(const MemberExpr& mem);
    HNode str_compare(const std::string& op, const HNode& a, const HNode& b, int line);

    /* `in_scope(…)` 的判定：nullopt = 不是这个形态（按运行期处理）。`lower.cc` 的 `in_scope_const`
     * 与 `ppif_true` 合成一处 —— `#if` 只是「必须是这个形态」。 */
    std::optional<bool> in_scope_const(const Expr& cond);

    /* ---- 作用域 */
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }
    void bind_local(const std::string& name, TyRef ty);
    void bind_const(const std::string& name, TyRef ty, uint64_t bits);
    const Binding* lookup(const std::string& name) const;

    Hir& hir_;
    TyRef ret_; /* 当前函数的返回类型：本步只有程序根（`None`） */
    std::vector<std::map<std::string, Binding>> scopes_;
    /* `break` 的标签栈（与 `lower` 的 `breaks_` 同构）：空标签 = 最近的一层，带标签则向内找
     * 第一个同名的外层循环；找不到就 `Unsupported`（不许落到外层循环上去）。
     * `switch` 不进这个栈：`lower.cc` 的 `s_SwitchStmt` 把每个 case 拆成独立的 if，`break`
     * 一直是穿到外层循环的。 */
    std::vector<std::string> breaks_;
    std::map<std::string, GlobalRef> globals_;
    std::map<std::string, HirConst> consts_;                  /* 顶层 `const #` */
    std::map<std::string, const VarDeclStmt*> root_consts_;   /* 程序根体里的 `const #` */
    std::map<std::string, TyRef> enums_;                      /* 枚举类型名 → 类型 */
    std::map<std::string, std::pair<std::string, int>> enum_member_; /* 成员 → (枚举, 序号) */
    std::map<std::string, StructLayout> structs_;             /* struct 名 → 布局 */
    std::map<std::pair<std::string, bool>, int> string_ids_;  /* (字节, cstring) → 串池下标 */
    /* `in_scope` 的 ① 档：整单元预扫的名字表（`lower.cc` 的 `in_scope_names_`）。必须在任何剪枝
     * 之前建好，而且只能看**未经裁剪的** AST（U-L14 的 F1/F2）。 */
    std::set<std::string> in_scope_names_;
};

/* ---------------------------------------------------------------- 模块级 */

void Analyzer::run(const ModuleSource& src) {
    hir_.main.key = "main";   /* 程序根的**逻辑名**；产物符号名 `tc_main` 由 codegen 派生 */
    hir_.main.name = "main";
    hir_.main.ret = ty_void();
    ret_ = ty_void();

    /* 顶层 def 需要实例闭包 + 捕获面，是 S2b-3 的事。这里显式报错，不静默丢。
     * 这一句用旧分型 `Unsupported`（S2b-1 就是这个形状）：本步「明确不做」的那一类（调用 /
     * def / 闭包 / 泛型）统一走它，新写的 sema 语义错误才用 `CompileError`。 */
    if (!src.defs.empty()) {
        throw Unsupported("顶层 def " + src.defs.front()->name + "（调用/def 是 S2b-3）",
                          src.defs.front()->src_line);
    }
    make_types(src);
    scan_scope_names(src);
    make_top_level(src.top_vars);
    if (src.label == nullptr) throw CompileFail("没有顶层语句块（程序根）");
    scan_root_consts(*src.label->body);
    hir_.main.body = block(*src.label->body);
}

/* `type` 声明（`lower.cc` 的 `Program::scan_decls`）：枚举先建（struct 的字段可以引用枚举），
 * struct 的布局与位宽由 `layout_struct` 一处算。
 * 与 `lower` 的差别只有一处：`lower` 在 collect 模式下会把单个 struct 的失败记成诊断继续跑，
 * 这里直接抛 —— 判据要的是「能编过 / 编不过」，不是「编过一半」。 */
void Analyzer::make_types(const ModuleSource& src) {
    for (const EnumSource& d : src.enums) {
        enums_[d.name] = layout_enum(d.name, d.members.size());
        for (std::size_t i = 0; i < d.members.size(); ++i) {
            const auto ins = enum_member_.insert(
                std::make_pair(d.members[i], std::make_pair(d.name, static_cast<int>(i))));
            if (!ins.second) {
                user_error("枚举成员 " + d.members[i] + " 重名（枚举 " + d.name + "）", 0);
            }
        }
    }
    for (const StructSource& d : src.structs) {
        std::vector<std::pair<std::string, TyRef>> fields;
        fields.reserve(d.fields.size());
        for (const auto& f : d.fields) fields.emplace_back(f.first, ty_of_ref(*f.second, 0));
        StructLayout lay = layout_struct(d.name, fields);
        hir_.structs.push_back(HirStruct{d.name, lay.ty->bits, lay.fields});
        structs_[d.name] = std::move(lay);
    }
}

/* `in_scope` ① 档的整单元预扫（`lower.cc` 的 `scan_scope_names`）。名字表取**未经 `in_scope`
 * 裁剪的原 AST**（U-L14 的 F1/F2：写在由自己守卫的块里、或写在被丢掉的块里的 def 都算），
 * 所以它只需一遍、不必到不动点。
 * 逐档对应 `docs/facts/semantics.md` / `docs/notes/2026-10-18-ul14-in-scope.md` §0：这一档收
 * `def` / `type`（含 enum 类型名与成员）/ 顶层 `const #X` / 程序根整棵语句树里的嵌套 def 与
 * `const #`。`var` 不算（它的可见性跟声明顺序走）。 */
void Analyzer::scan_scope_names(const ModuleSource& src) {
    for (const FunctionDef* d : src.defs) in_scope_names_.insert(d->name);
    for (const EnumSource& d : src.enums) {
        in_scope_names_.insert(d.name);
        for (const std::string& member : d.members) in_scope_names_.insert(member);
    }
    for (const StructSource& d : src.structs) in_scope_names_.insert(d.name);
    for (const VarDeclStmt* st : src.top_vars) {
        if (st->kw == "const" && !st->name.empty() && st->name[0] == '#') {
            in_scope_names_.insert(st->name);
        }
    }
    if (src.label == nullptr) return;
    std::vector<const DefStmt*> defs;
    std::vector<const VarDeclStmt*> consts;
    collect_root_decls(*src.label->body, defs, consts);
    for (const DefStmt* d : defs) in_scope_names_.insert(d->name);
    for (const VarDeclStmt* v : consts) in_scope_names_.insert(v->name);
}

/* 顶层声明（`lower.cc` 的 `make_globals`）：`const #X` 进 `consts`（零条指令），别的都要有
 * 编译期常量初值 → 一个带 `init_bits` 的全局。**按源码顺序**求值：引用只能看到排在前面
 * 的常量（原版就是这么逐条建的，顺序不能改）。 */
void Analyzer::make_top_level(const std::vector<const VarDeclStmt*>& top_vars) {
    for (const VarDeclStmt* st : top_vars) {
        if (st->kw == "const" && !st->name.empty() && st->name[0] == '#') {
            auto v = const_value(*st->init);
            if (!v) throw Unsupported("编译期常量 " + st->name + " 的初始化式不是常量", st->src_line);
            if (v->kind != HKind::Const) {
                throw Unsupported("编译期常量 " + st->name + " 不是整数常量（字符串常量未接）",
                                  st->src_line);
            }
            HirConst c;
            c.name = st->name;
            c.ty = v->ty;
            c.bits = v->const_bits;
            hir_.consts.push_back(c);
            consts_[c.name] = std::move(c);
            continue;
        }
        auto v = const_value(*st->init);
        if (!v) {
            throw Unsupported("顶层 var " + st->name + " 的初始化式不是编译期常量（运行期初始化未做）",
                              st->src_line);
        }
        HirGlobal g;
        g.name = st->name;
        g.ty = v->ty;
        g.init_bits = v->const_bits;
        globals_[g.name] = GlobalRef{static_cast<int>(hir_.globals.size()), g.ty};
        hir_.globals.push_back(std::move(g));
    }
}

/* 程序根体里的 `const #` 在 `lower` 里是**整单元**可见的（`collect_root_decls`，引用点可以排在
 * 声明之前）。范围必须与那个函数逐句一致：`while`/`if`/`#if`/`switch` 的块递归下去，裸块与
 * `def` 体不钻。（S2b-1 只收直接语句的口径当时写的是「本步没有 if/while 可钻」，但 `while`
 * 那时已有 —— 那一版会漏掉根循环体里的 `const #`。） */
void Analyzer::scan_root_consts(const Block& body) {
    std::vector<const DefStmt*> defs;
    std::vector<const VarDeclStmt*> consts;
    collect_root_decls(body, defs, consts);
    for (const VarDeclStmt* v : consts) root_consts_[v->name] = v;   /* 同名：后声明的赢（`lower` 同） */
}

/* 前缀全局**按需建**（`lower.cc` 的 `prefix_global`）：第一次被引用才进 `Hir.globals`，所以它的
 * 下标跟在顶层全局后面 —— 与今天「`make_globals` 先建完、前缀用到时才加」的顺序一致。 */
Analyzer::GlobalRef Analyzer::prefix_global(const std::string& name, int line) {
    auto hit = globals_.find(name);
    if (hit != globals_.end()) return hit->second;
    auto it = prefix_globals().find(name);
    if (it == prefix_globals().end()) unimplemented("前缀全局 " + name, line);
    HirGlobal g;
    g.name = name;
    g.ty = it->second.ty;
    g.init_bits = it->second.init;
    GlobalRef ref{static_cast<int>(hir_.globals.size()), g.ty};
    globals_[name] = ref;
    hir_.globals.push_back(std::move(g));
    return ref;
}

/* 串池（`lower.cc` 的 `str_pool_`）：同一串字面量只占一个槽（`Hir.strings` 下标），
 * `cstring` 也算键的一部分（两条字节相同但补零形态不同的串不是同一个）。 */
int Analyzer::intern_string(const std::string& bytes) {
    const std::pair<std::string, bool> key{bytes, false};
    auto hit = string_ids_.find(key);
    if (hit != string_ids_.end()) return hit->second;
    const int id = static_cast<int>(hir_.strings.size());
    hir_.strings.push_back(HirString{bytes, false});
    string_ids_[key] = id;
    return id;
}

HNode Analyzer::root_const_value(const VarDeclStmt& decl) {
    auto v = const_value(*decl.init);
    if (!v) throw Unsupported("编译期常量 " + decl.name + " 的初始化式不是常量", decl.src_line);
    return *v;
}

/* ---------------------------------------------------------------- 类型 */

/* `lower.cc` 的 `ty_of_name`。`enums_` / `structs_` 的查表位置与它一致（在 `U*`/`S*` 数字档
 * 之前）—— 枚举/struct 名与位宽拼写不能混。 */
TyRef Analyzer::ty_of_name(const std::string& name, int line) {
    if (!name.empty() && name[0] == '@') throw Unsupported("编译期类型参数 " + name, line);
    if (name == "Int") return ty_i64();
    if (name == "Uint" || name == "Sint") return ty_i64(); /* 重载族约束（接口 §1） */
    if (name == "Bool") return ty_bool();
    if (name == "Ptr") return ty_ptr();
    if (name == "Str") return ty_str();
    if (name == "Time") return ty_time();
    if (name == "Seed") return ty_seed();
    if (name == "None" || name == "Void") return ty_void();
    if (enums_.count(name) != 0) return enums_.at(name);
    if (structs_.count(name) != 0) return structs_.at(name).ty;
    if ((name[0] == 'U' || name[0] == 'S') && name.size() > 1) {
        bool digits = true;
        for (std::size_t i = 1; i < name.size(); ++i) {
            digits = digits && name[i] >= '0' && name[i] <= '9';
        }
        if (digits) {
            const int bits = std::stoi(name.substr(1));
            if (bits < 1 || bits > 64) throw Unsupported("位宽 " + name, line);
            return make_int(bits, name[0] == 'S');
        }
    }
    throw Unsupported("类型 " + name, line);
}

TyRef Analyzer::ty_of_ref(const TypeRef& ref, int line) {
    if (ref.at) throw Unsupported("编译期类型参数 " + ref.name, line);
    if (ref.elem != nullptr) {
        if (ref.name != "Arr") throw Unsupported("泛型类型 " + ref.name + "[…]", line);
        return make_array(ty_of_ref(*ref.elem, line));
    }
    return ty_of_name(ref.name, line);
}

/* `lower.cc` 的 `unify` 逐行照搬（含每一条 `Unsupported` 的文本）。 */
TyRef Analyzer::unify(const TyRef& a, const TyRef& b, int line) {
    if (a->kind == TyKind::Ptr || b->kind == TyKind::Ptr) {
        if ((a->kind == TyKind::Ptr || a->kind == TyKind::Int) &&
            (b->kind == TyKind::Ptr || b->kind == TyKind::Int)) {
            return ty_ptr();
        }
        throw Unsupported("指针与 " +
                              std::string(ty_kind_name(a->kind == TyKind::Ptr ? b->kind : a->kind)) +
                              " 混用",
                          line);
    }
    if (a->kind == TyKind::Struct || b->kind == TyKind::Struct) {
        throw Unsupported("struct 参与运算", line);
    }
    if (a->kind == TyKind::Array || b->kind == TyKind::Array) {
        throw Unsupported("Arr 参与运算", line);
    }
    if (a->kind == TyKind::Str || b->kind == TyKind::Str) {
        if (a->kind == TyKind::Str && b->kind == TyKind::Str) return ty_str();
        throw Unsupported("Str 与整数混用", line);
    }
    if (a->kind == TyKind::Enum && b->kind == TyKind::Enum) {
        if (a->name != b->name) {
            throw Unsupported("枚举 " + a->name + " 与 " + b->name + " 混用", line);
        }
        return a;
    }
    if (a->kind == TyKind::Enum) return b;
    if (b->kind == TyKind::Enum) return a;
    if (is_int(*a) && is_int(*b)) {
        if (a->bits == b->bits) return a; /* 同宽不同符号性：以左操作数为准 */
        return a->bits > b->bits ? a : b;
    }
    throw Unsupported("类型 " + std::string(ty_kind_name(a->kind)) + " 与 " +
                          ty_kind_name(b->kind) + " 混用",
                      line);
}

/* `lower.cc` 的 `arith_ty`：整型**字面量**取窄的那一侧的位宽（字面量多态），只铺给 `+`。 */
TyRef Analyzer::arith_ty(const HNode& a, const HNode& b, const std::string& op, int line) {
    if (op == "+" && is_int(*a.ty) && is_int(*b.ty) && a.ty->bits != b.ty->bits) {
        const bool narrow_is_a = a.ty->bits < b.ty->bits;
        const TyRef& narrow = narrow_is_a ? a.ty : b.ty;
        const HNode& literal = narrow_is_a ? b : a;
        if (narrow->bits < 64 && pat_const(literal).has_value()) return narrow;
    }
    return unify(a.ty, b.ty, line);
}

/* ---------------------------------------------------------------- 表达式 */

HNode Analyzer::e(const Expr& node) {
    switch (node.kind) {
        case NK::Literal: return e_literal(static_cast<const LiteralExpr&>(node));
        case NK::Ident: return e_ident(static_cast<const IdentExpr&>(node));
        case NK::Cast: return e_cast(static_cast<const CastExpr&>(node));
        case NK::Unary: return e_unary(static_cast<const UnaryExpr&>(node));
        case NK::Binary: return e_binary(static_cast<const BinaryExpr&>(node));
        case NK::Cond: return e_cond(static_cast<const CondExpr&>(node));
        /* Member / Index 的两条路（读一份值、拿一份位置）在 HIR 里是**同一个节点**：
         * 读与写的区别由外层节点决定（`Member`/`Index` 就是位置本身，`ExprStmt`/`Assign`/
         * `VarDecl` 才决定要不要 load）。所以这里与 `lvalue` 走同一条路（`lower.cc` 的
         * `e_MemberExpr` / `e_IndexExpr` 也是 `lvalue(…)` + `load_addr`）。 */
        case NK::Member:
        case NK::Index: return lvalue(node);
        case NK::ArrayLit: return array_lit_node(static_cast<const ArrayLitExpr&>(node));
        case NK::CompositeLit:
            return composite_lit_node(static_cast<const CompositeLitExpr&>(node));
        case NK::Template: return template_expr(static_cast<const TemplateExpr&>(node));
        case NK::Call:
            throw Unsupported("调用（S2b-3 才做）", node.src_line);
        default:
            throw Unsupported("表达式形态 " + std::string(ast_kind_name(node.kind)), node.src_line);
    }
}

HNode Analyzer::e_literal(const LiteralExpr& node) {
    if (node.lit_kind == "int") {
        return const_node(ty_i64(), static_cast<uint64_t>(node.value) & kMask64, node.src_line);
    }
    /* `str` / 无插值的 `template`：两条在 `lower` 里走同一个 `string_literal`。 */
    if (node.lit_kind == "str" || node.lit_kind == "template") {
        return string_literal(node.text, node.src_line);
    }
    throw Unsupported("字面量 " + node.lit_kind, node.src_line);
}

/* 字符串字面量 → 一个 `StrLit`（字节进串池，`Hir.strings`）。
 *   · 含反斜杠的字面量**不支持**：转义表里 `\t` → 0x08、`\b`/`\x42` 不认、`\\` 解析失败
 *     （U-L10），没有一条能直接当「标准 C 转义」用，所以照 `lower` 报错而不是猜。
 *   · 末尾补 0 与 `len << 16` 头是**串表布局**（`lower` 对所有字面量都补），属 codegen 细节，
 *     所以 `HirString.cstring` 在本步恒为 false（语料里 0 处 `c'…'` 形态）。 */
HNode Analyzer::string_literal(const std::string& text, int line) {
    if (text.find('\\') != std::string::npos) {
        unimplemented("含转义的字符串字面量（U-L10 未定）", line);
    }
    HNode n = expr_node(HKind::StrLit, ty_str(), line);
    n.string_id = intern_string(text);
    return n;
}

HNode Analyzer::e_ident(const IdentExpr& node) {
    const std::string& name = node.name;
    if (name == "true" || name == "false") {
        return const_node(ty_bool(), name == "true" ? 1u : 0u, node.src_line);
    }
    if (const Binding* b = lookup(name)) {
        if (b->kind == Binding::Kind::Local) return local_node(name, b->ty, node.src_line);
        return const_node(b->ty, b->bits, node.src_line);
    }
    auto g = globals_.find(name);
    if (g != globals_.end()) return global_node(g->second.id, g->second.ty, node.src_line);
    if (prefix_globals().count(name) != 0) {
        const GlobalRef pg = prefix_global(name, node.src_line);
        return global_node(pg.id, pg.ty, node.src_line);
    }
    /* 枚举成员是整数常量（序号从 0 起），类型是枚举自己 —— 与 `lower.cc` 的 `e_IdentExpr`
     * 同一个位置（在 `consts_` 之前）。 */
    auto em = enum_member_.find(name);
    if (em != enum_member_.end()) {
        return const_node(enums_.at(em->second.first), static_cast<uint64_t>(em->second.second),
                          node.src_line);
    }
    auto c = consts_.find(name);
    if (c != consts_.end()) {
        return const_node(c->second.ty, c->second.bits, node.src_line);
    }
    auto hc = hash_consts().find(name);
    if (hc != hash_consts().end()) {
        return const_node(hc->second.ty, hc->second.value, node.src_line);
    }
    auto rc = root_consts_.find(name);
    if (rc != root_consts_.end()) return root_const_value(*rc->second);
    throw Unsupported("未定义的名字 " + name, node.src_line);
}

HNode Analyzer::e_cast(const CastExpr& node) {
    const TyRef target = ty_of_ref(*node.target, node.src_line);
    /* `Arr[T] []`：空数组字面量自己不带元素类型（`ArrayLitExpr` 只存 items），类型只在外面这层
     * 转换上，所以只能在这里接住（`lower.cc` 的 `e_CastExpr` 同形）。HIR 里就是一个没有 kid 的
     * `ArrayLit` —— 静态的 8 字节零头 + 空载荷由 codegen 落表。 */
    if (target->kind == TyKind::Array) {
        const auto* lit = as<ArrayLitExpr>(node.expr.get());
        if (lit != nullptr && lit->items.empty()) {
            return expr_node(HKind::ArrayLit, target, node.src_line);
        }
    }
    return convert(e(*node.expr), target, node.src_line);
}

HNode Analyzer::e_unary(const UnaryExpr& node) {
    /* `+x` 是恒等，sema 里就没了（`hir.hh` 的 `HUnOp` 因此只有三种）。 */
    if (node.op == "+") return e(*node.expr);
    if (node.op == "!") {
        const HNode v = e(*node.expr);
        auto c = pat_const(v);
        if (c) return const_node(ty_bool(), *c == 0 ? 1u : 0u, node.src_line);
        HNode n = expr_node(HKind::Unary, ty_bool(), node.src_line);
        n.un_op = HUnOp::Not;
        n.kids.push_back(v);
        return n;
    }
    const HNode v = e(*node.expr);
    if (!is_int(*v.ty)) {
        throw Unsupported("一元 " + node.op + " 用在 " + ty_kind_name(v.ty->kind), node.src_line);
    }
    auto c = pat_const(v);
    if (c) return const_node(v.ty, fold_unary(node.op, *c, *v.ty), node.src_line);
    HNode n = expr_node(HKind::Unary, v.ty, node.src_line);
    if (node.op == "-") {
        n.un_op = HUnOp::Neg;
    } else if (node.op == "~") {
        n.un_op = HUnOp::BitNot;
    } else {
        throw Unsupported("一元运算符 " + node.op, node.src_line);
    }
    n.kids.push_back(v);
    return n;
}

HNode Analyzer::e_binary(const BinaryExpr& node) {
    const HNode a = e(*node.left);
    const HNode b = e(*node.right);
    if (node.op == "&&" || node.op == "||") return logical(node.op, a, b, node.src_line);
    if (is_cmp(node.op)) return compare(node.op, a, b, node.src_line);
    return binop(node.op, a, b, node.src_line);
}

/* `lower.cc` 的 `logical`：`&&`/`||` 短路，两侧按「非零为真」看（`as_cond`），**不做** `convert`
 * —— 所以两侧类型可以不相等（不变量 ④ 对这两种 op 只查结果类型是 Bool）。 */
HNode Analyzer::logical(const std::string& op, const HNode& a, const HNode& b, int line) {
    as_cond(a, line);
    as_cond(b, line);
    HNode n = expr_node(HKind::Binary, ty_bool(), line);
    n.bin_op = op == "&&" ? HBinOp::LogAnd : HBinOp::LogOr;
    n.kids.push_back(a);
    n.kids.push_back(b);
    return n;
}

/* `lower.cc` 的 `compare`：`unify` → 两侧 `convert` → 常量则 `fold_cmp`。 */
HNode Analyzer::compare(const std::string& op, const HNode& a, const HNode& b, int line) {
    const TyRef ty = unify(a.ty, b.ty, line);
    if (ty->kind == TyKind::Str) return str_compare(op, a, b, line);
    const HNode av = convert(a, ty, line);
    const HNode bv = convert(b, ty, line);
    auto ca = pat_const(av);
    auto cb = pat_const(bv);
    if (ca && cb) return const_node(ty_bool(), fold_cmp(op, *ca, *cb, *ty), line);
    HNode n = expr_node(HKind::Binary, ty_bool(), line);
    n.bin_op = cmp_bin_op(op);
    n.kids.push_back(av);
    n.kids.push_back(bv);
    return n;
}

/* `lower.cc` 的 `str_compare`。`Str` 的比较**不走** `convert`/`pat_const`（`lower` 也是在
 * `compare` 开头就分流）：两条 `Str` 是不同长度、不同地址的串，比 `eq` 位模式毫无意义 ——
 * 参考走 `__simplexc_streq`（先比长度、再逐字节），所以这里只留「按内容比」这个事实
 * （`StrEq`/`StrNe`），不折叠、也折不出常量（`lower` 同）。`<`/`>` 参考自己就报错。 */
HNode Analyzer::str_compare(const std::string& op, const HNode& a, const HNode& b, int line) {
    if (op != "==" && op != "!=") unimplemented("Str 比大小（" + op + "）", line);
    HNode n = expr_node(HKind::Binary, ty_bool(), line);
    n.bin_op = op == "==" ? HBinOp::StrEq : HBinOp::StrNe;
    n.kids.push_back(a);
    n.kids.push_back(b);
    return n;
}

/* `lower.cc` 的 `binop`：`arith_ty` → 两侧 `convert` → 常量则 `fold_arith`（截断在运算结果上）。 */
HNode Analyzer::binop(const std::string& op, const HNode& a, const HNode& b, int line) {
    if (!is_arith(op)) throw Unsupported("运算符 " + op, line);
    const TyRef ty = arith_ty(a, b, op, line);
    const HNode av = convert(a, ty, line);
    const HNode bv = convert(b, ty, line);
    if (ty->kind == TyKind::Str) {
        if (op != "+") throw Unsupported("Str 的 " + op, line);
        throw Unsupported("Str 拼接", line); /* `str_concat` 要串池与运行期辅助函数 */
    }
    if (ty->kind == TyKind::Ptr) {
        if (op != "+" && op != "-") throw Unsupported("指针 " + op, line);
        auto ca = pat_const(av);
        auto cb = pat_const(bv);
        if (ca && cb) {
            const uint64_t r = op == "+" ? *ca + *cb : *ca - *cb;
            return const_node(ty_ptr(), r & kMask64, line);
        }
        HNode n = expr_node(HKind::Binary, ty, line);
        n.bin_op = arith_bin_op(op);
        n.kids.push_back(av);
        n.kids.push_back(bv);
        return n;
    }
    auto ca = pat_const(av);
    auto cb = pat_const(bv);
    if (ca && cb) return const_node(ty, fold_arith(op, *ca, *cb, *ty), line);
    HNode n = expr_node(HKind::Binary, ty, line);
    n.bin_op = arith_bin_op(op);
    n.kids.push_back(av);
    n.kids.push_back(bv);
    return n;
}

/* `lower.cc` 的 `e_CondExpr`：条件是 `as_cond`（不 convert），两臂 `unify` 后各自 `convert`。 */
HNode Analyzer::e_cond(const CondExpr& node) {
    const HNode c = e(*node.cond);
    as_cond(c, node.src_line);
    const HNode t = e(*node.then_);
    const HNode f = e(*node.else_);
    if (t.ty->kind == TyKind::Void || f.ty->kind == TyKind::Void) {
        throw Unsupported("三目的一支没有值", node.src_line);
    }
    const TyRef ty = unify(t.ty, f.ty, node.src_line);
    HNode n = expr_node(HKind::Cond, ty, node.src_line);
    n.kids.push_back(c);
    n.kids.push_back(convert(t, ty, node.src_line));
    n.kids.push_back(convert(f, ty, node.src_line));
    return n;
}

HNode Analyzer::as_cond(const HNode& v, int line) {
    if (v.ty->kind == TyKind::Void) throw Unsupported("None 值当条件", line);
    return v;
}

/* ---------------------------------------------------------------- 数组 / 复合字面量 / 模板 */

/* `lower.cc` 的 `array_literal`：
 *   · 元素走 `cv`（常量求值）—— 数组字面量在参考那边是一个**静态片**，元素不是运行期值；
 *   · 元素类型取**第一个**元素的类型，后面各元素**不** `convert`（写入时按自己的宽度截断，
 *     `const_for_storage`）；
 *   · 空字面量 `[ ]` 报错，但 `Arr[U8] []` 是另一条路（见 `e_cast`）。
 * 数组的 8 字节 Nim 头（`p[-8] = len << 16`）不在 HIR 节点上：它是数组值的表示，属于 codegen。
 *
 * 本函数只抛 `CompileError`（新写代码的唯一错误模型）：`const_value` 已经把常量求值的失败
 * 折成 nullopt，这里补上人读的原因。 */
HNode Analyzer::array_lit_node(const ArrayLitExpr& node) {
    std::vector<HNode> items;
    items.reserve(node.items.size());
    for (const ExprPtr& item : node.items) {
        std::optional<HNode> v = const_value(*item);
        if (!v.has_value()) {
            unimplemented("数组字面量的元素不是编译期常量", item->src_line);
        }
        items.push_back(std::move(*v));
    }
    if (items.empty()) unimplemented("空数组字面量（`Arr[T] []` 要走显式转换）", node.src_line);
    HNode n = expr_node(HKind::ArrayLit, make_array(items[0].ty), node.src_line);
    n.kids = std::move(items);
    return n;
}

/* `lower.cc` 的 `composite_literal`。两件事必须分开看（U-L4 已验）：
 *   · **求值顺序 = 源码书写顺序**（字段值有副作用时看得见）—— 所以下面的循环按
 *     `node.fields` 的顺序求值；
 *   · HIR 里的 kid 顺序 = **声明序**（方案 §2b 冻结），字面量里没写的字段是它的零值 `Const`。
 *     于是 `kids[i]` 恒对应 `ty->fields[i]`，codegen 不必再处理「哪些字段没写」。
 * 字段按**名字**匹配（与书写顺序无关），未列字段默认 0。 */
HNode Analyzer::composite_lit_node(const CompositeLitExpr& node) {
    const TyRef ty = ty_of_ref(*node.typ, node.src_line);
    if (ty->kind != TyKind::Struct) {
        unimplemented("复合字面量类型 " + std::string(ty_kind_name(ty->kind)), node.src_line);
    }
    std::vector<HNode> kids;
    kids.reserve(ty->fields.size());
    for (const auto& f : ty->fields) kids.push_back(const_node(f.second, 0, node.src_line));
    for (const auto& init : node.fields) {
        std::size_t idx = ty->fields.size();
        for (std::size_t i = 0; i < ty->fields.size(); ++i) {
            if (ty->fields[i].first == init->name) {
                idx = i;
                break;
            }
        }
        if (idx == ty->fields.size()) {
            unimplemented("struct " + ty->name + " 没有字段 " + init->name, init->src_line);
        }
        HNode v = e(*init->value);
        kids[idx] = convert(std::move(v), ty->fields[idx].second, init->src_line);
    }
    HNode n = expr_node(HKind::CompositeLit, ty, node.src_line);
    n.kids = std::move(kids);
    return n;
}

/* `lower.cc` 的 `template_expr` 的**结构部分**（渲染规则在 `docs/notes/2026-10-18-str-interp.md`）：
 * 文字段与插值交替、以文字段开头、以文字段结尾 —— 所以 kid 个数是 2×插值数 + 1。
 * HIR 不发指令、也不决定缓冲大小：每段渲染成多少字节是 codegen 的事（十进制/`true`/`false`/
 * 原样字节），这里只把「哪些段是字面字节、哪些是表达式」记下来。
 * 插值段的类型必须可渲染（`Str` / `Ptr` / `Int`（含 `Bool`）），其余报错 —— 与
 * `lower.cc` 的 `part_capacity` 一致（枚举插值参考自己 fatal）。 */
HNode Analyzer::template_expr(const TemplateExpr& node) {
    if (node.texts.size() != node.parts.size() + 1) {
        our_bug("模板的文字段个数应是插值数 + 1（" + std::to_string(node.texts.size()) + " vs " +
                    std::to_string(node.parts.size()) + "）",
                node.src_line);
    }
    HNode n = expr_node(HKind::Template, ty_str(), node.src_line);
    for (std::size_t i = 0; i < node.texts.size(); ++i) {
        n.kids.push_back(string_literal(node.texts[i], node.src_line));
        if (i >= node.parts.size()) break;
        HNode v = e(*node.parts[i]);
        const TyKind k = v.ty->kind;
        if (k != TyKind::Str && k != TyKind::Ptr && k != TyKind::Int) {
            unimplemented("模板插值遇到 " + std::string(ty_kind_name(k)), node.parts[i]->src_line);
        }
        n.kids.push_back(std::move(v));
    }
    return n;
}

/* `lower.cc` 的 `convert` —— **隐式转换唯一的落点**，分派规则一条不改，只是把
 * `V::instr(...)` 换成 `Cast` 节点、`V::konst(...)` 换成 `Const` 节点：
 *   · 目标 Int：真指针 → `PtrToInt`；常量 → 折成 `Const`；否则按位宽决定 `Trunc` / `Zext` / `Raw`。
 *   · 目标 Str/Arr：源也是真指针 → `Raw`（只换标签）；常量 → `Const`；否则 `IntToPtr`。
 *   · 别的（`Ptr` / 枚举）：常量 → `Const`，运行期 → `Raw`（只换类型标签，位模式不动）。
 * 加宽只出 `Zext`：运行期窄值本来就是 `zext(trunc_W)` 的 64 位位模式，`convert` 在这条路上只
 * 补一层掩码；按符号性扩的那一路只属于编译期常量（`pat_const`），而常量在上面已经折完了。 */
HNode Analyzer::convert(HNode v, const TyRef& target, int line) {
    if (target->kind == TyKind::Void) throw Unsupported("转换成 None", line);
    if (v.ty->kind == TyKind::Struct || target->kind == TyKind::Struct) {
        if (v.ty->kind == TyKind::Struct && target->kind == TyKind::Struct &&
            v.ty->name == target->name) {
            return v;
        }
        throw Unsupported("转换 " + std::string(ty_kind_name(v.ty->kind)) + " → " +
                              ty_kind_name(target->kind),
                          line);
    }
    auto c = pat_const(v);
    if (target->kind == TyKind::Int) {
        if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) { /* 真指针 → 整数 */
            return cast_node(HCastKind::PtrToInt, std::move(v), target, line);
        }
        if (c) return const_node(target, *c & mask_of(target->bits), line);
        if (same_val_ty(v.ty, target)) return v;
        if (target->bits < v.ty->bits) return cast_node(HCastKind::Trunc, std::move(v), target, line);
        if (target->bits > v.ty->bits) return cast_node(HCastKind::Zext, std::move(v), target, line);
        return cast_node(HCastKind::Raw, std::move(v), target, line); /* 同宽、只换符号性/名义位 */
    }
    if (target->kind == TyKind::Str || target->kind == TyKind::Array) { /* 整数地址 → 真指针 */
        if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) {
            return cast_node(HCastKind::Raw, std::move(v), target, line);
        }
        if (c) return const_node(target, *c & kMask64, line);
        return cast_node(HCastKind::IntToPtr, std::move(v), target, line);
    }
    /* `Ptr` / 枚举：i64 之间位重解释，只换类型标签。 */
    if (c) return const_node(target, *c & kMask64, line);
    if (same_val_ty(v.ty, target)) return v;
    return cast_node(HCastKind::Raw, std::move(v), target, line);
}

/* ---------------------------------------------------------------- 常量求值 */

/* `lower.cc` 的 `const_value`：常量求值失败 = 「这个初始化式不是常量」，不是错误本身。
 * `CompileError` 也收（新写的子函数抛它，比如带反斜杠的串字面量）；`OurBug` 不吞 —— 内部不
 * 自洽不能伪装成「不是常量」。 */
std::optional<HNode> Analyzer::const_value(const Node& node) {
    try {
        return cv(node);
    } catch (const Unsupported&) {
        return std::nullopt;
    } catch (const CompileError& e) {
        if (e.resp == Responsibility::OurBug) throw;
        return std::nullopt;
    }
}

/* `lower.cc` 的 `cv`。与 `e()` 的**差别照抄**（`lower` 那边也是这样）：走 `unify` 而不是
 * `arith_ty`（没有窄侧胜）、不认 `&&`/`||`、只认 `raw_cast` 一类能折的调用。 */
HNode Analyzer::cv(const Node& node) {
    switch (node.kind) {
        case NK::Literal: {
            const auto& n = static_cast<const LiteralExpr&>(node);
            if (n.lit_kind == "int") {
                return const_node(ty_i64(), static_cast<uint64_t>(n.value) & kMask64, n.src_line);
            }
            /* 串字面量在常量环境里也是常量（`lower.cc` 的 `cv` 同）：`switch` 的 Str 主体靠
             * `case "…"` 这种常量值派发。 */
            if (n.lit_kind == "str" || n.lit_kind == "template") {
                return string_literal(n.text, n.src_line);
            }
            throw Unsupported("字面量 " + n.lit_kind, n.src_line);
        }
        case NK::Ident: {
            const auto& n = static_cast<const IdentExpr&>(node);
            if (n.name == "true" || n.name == "false") {
                return const_node(ty_bool(), n.name == "true" ? 1u : 0u, n.src_line);
            }
            /* 局部 `const` 只活在作用域里（`e_ident` 同样先查 lookup）—— 不查这里，
               函数内的 `const #N = #A | #B` 会被判成「初始化式不是常量」。 */
            if (const Binding* b = lookup(n.name)) {
                if (b->kind == Binding::Kind::Const) return const_node(b->ty, b->bits, n.src_line);
            }
            auto c = consts_.find(n.name);
            if (c != consts_.end()) return const_node(c->second.ty, c->second.bits, n.src_line);
            auto em = enum_member_.find(n.name);
            if (em != enum_member_.end()) {
                return const_node(enums_.at(em->second.first),
                                  static_cast<uint64_t>(em->second.second), n.src_line);
            }
            auto hc = hash_consts().find(n.name);
            if (hc != hash_consts().end()) {
                return const_node(hc->second.ty, hc->second.value, n.src_line);
            }
            auto rc = root_consts_.find(n.name);
            if (rc != root_consts_.end()) return root_const_value(*rc->second);
            throw Unsupported("名字 " + n.name, n.src_line);
        }
        case NK::Cast: {
            const auto& n = static_cast<const CastExpr&>(node);
            const TyRef target = ty_of_ref(*n.target, n.src_line);
            if (target->kind == TyKind::Array) {
                const auto* lit = as<ArrayLitExpr>(n.expr.get());
                if (lit != nullptr && lit->items.empty()) {
                    return expr_node(HKind::ArrayLit, target, n.src_line);
                }
            }
            return convert(cv(*n.expr), target, n.src_line);
        }
        case NK::Unary: {
            const auto& n = static_cast<const UnaryExpr&>(node);
            const HNode inner = cv(*n.expr);
            if (n.op == "+") return inner;
            if (!is_int(*inner.ty)) {
                throw Unsupported(std::string("一元 ") + n.op + " 用在 " + ty_kind_name(inner.ty->kind),
                                  n.src_line);
            }
            return const_node(inner.ty, fold_unary(n.op, need_const(inner, n.src_line), *inner.ty),
                              n.src_line);
        }
        case NK::Binary: {
            const auto& n = static_cast<const BinaryExpr&>(node);
            if (n.op == "&&" || n.op == "||") throw Unsupported("常量逻辑运算", n.src_line);
            const HNode l = cv(*n.left);
            const HNode r = cv(*n.right);
            const TyRef ty = unify(l.ty, r.ty, n.src_line);
            const HNode a = convert(l, ty, n.src_line);
            const HNode b = convert(r, ty, n.src_line);
            if (is_cmp(n.op)) {
                return const_node(ty_bool(),
                                  fold_cmp(n.op, need_const(a, n.src_line), need_const(b, n.src_line),
                                           *ty),
                                  n.src_line);
            }
            if (!is_arith(n.op)) throw Unsupported("运算符 " + n.op, n.src_line);
            if (ty->kind == TyKind::Ptr) {
                if (n.op != "+" && n.op != "-") throw Unsupported("指针 " + n.op, n.src_line);
                auto ca = pat_const(a);
                auto cb = pat_const(b);
                if (!ca || !cb) throw Unsupported("指针算术的操作数不是常量", n.src_line);
                const uint64_t sum = n.op == "+" ? *ca + *cb : *ca - *cb;
                return const_node(ty_ptr(), sum & kMask64, n.src_line);
            }
            return const_node(ty, fold_arith(n.op, need_const(a, n.src_line),
                                             need_const(b, n.src_line), *ty),
                              n.src_line);
        }
        default:
            throw Unsupported("常量求值遇到 " + std::string(ast_kind_name(node.kind)), node.src_line);
    }
}

/* ---------------------------------------------------------------- 语句 */

HNode Analyzer::block(const Block& b) {
    HNode out = stmt_node(HKind::Block, b.src_line);
    push_scope();
    for (const StmtPtr& st : b.stmts) statement(out, *st);
    pop_scope();
    return out;
}

void Analyzer::statement(HNode& parent, const Stmt& st) {
    switch (st.kind) {
        case NK::VarDecl:
            var_decl_stmt(parent, static_cast<const VarDeclStmt&>(st));
            return;
        case NK::Assign:
            assign_stmt(parent, static_cast<const AssignStmt&>(st));
            return;
        case NK::ExprStmt: {
            HNode n = stmt_node(HKind::ExprStmt, st.src_line);
            n.kids.push_back(e(*static_cast<const ExprStmt&>(st).expr));
            parent.kids.push_back(std::move(n));
            return;
        }
        case NK::Return:
            parent.kids.push_back(return_stmt(static_cast<const ReturnStmt&>(st)));
            return;
        case NK::BlockStmt:
            /* 裸块 = 一层作用域（`lower` 的 `lower_stmt` 直接转 `lower_block`）。 */
            parent.kids.push_back(block(*static_cast<const BlockStmt&>(st).block));
            return;
        case NK::If:
            if_stmt(parent, static_cast<const IfStmt&>(st));
            return;
        case NK::PpIf:
            ppif_stmt(parent, static_cast<const PpIfStmt&>(st));
            return;
        case NK::Switch:
            switch_stmt(parent, static_cast<const SwitchStmt&>(st));
            return;
        case NK::While:
            while_stmt(parent, static_cast<const WhileStmt&>(st));
            return;
        case NK::Break:
            break_stmt(parent, static_cast<const BreakStmt&>(st));
            return;
        default:
            throw Unsupported("语句形态 " + std::string(ast_kind_name(st.kind)), st.src_line);
    }
}

/* `lower.cc` 的 `s_VarDeclStmt`。三种 kw 的去向完全不同，抄清楚：
 *   `const #X`            → 编译期常量，**零条指令**（只进作用域 / 顶层进 `consts`）
 *   `let` / `const` + 常量 → 原版把它当常量本身，引用点直接是 `Const`，同样零条指令
 *   其余（含 `var`）       → 一个 `VarDecl`（codegen 版 alloca + store） */
void Analyzer::var_decl_stmt(HNode& parent, const VarDeclStmt& st) {
    if (st.kw == "const" && !st.name.empty() && st.name[0] == '#') {
        auto v = const_value(*st.init);
        if (!v) throw Unsupported("编译期常量 " + st.name + " 的初始化式不是常量", st.src_line);
        /* `Binding::Const` 只存**位模式**，字符串常量无处放 —— 报错，不静默丢串池 id。 */
        if (v->kind != HKind::Const) {
            throw Unsupported("编译期常量 " + st.name + " 不是整数常量（字符串常量未接）",
                              st.src_line);
        }
        bind_const(st.name, v->ty, v->const_bits);
        return;
    }
    HNode init = e(*st.init);
    if (init.ty->kind == TyKind::Void) {
        throw Unsupported(st.name + " 的初始化式没有值", st.src_line);
    }
    if ((st.kw == "let" || st.kw == "const") && init.kind == HKind::Const) {
        bind_const(st.name, init.ty, init.const_bits);
        return;
    }
    HNode n = stmt_node(HKind::VarDecl, st.src_line);
    n.ty = init.ty;
    n.name = st.name;
    n.kids.push_back(std::move(init));
    const TyRef ty = n.ty; /* 声明之后才可见（`var x = x` 取外层那个 x） */
    parent.kids.push_back(std::move(n));
    bind_local(st.name, ty);
}

/* `lower.cc` 的 `s_AssignStmt`：先取左值、再求右值，右侧 `convert` 到左值的类型；
 * 复合赋值 `x op= v` = `binop(op, 读一次 x, v)` 再 `convert`。 */
void Analyzer::assign_stmt(HNode& parent, const AssignStmt& st) {
    HNode target = lvalue(*st.target);
    HNode val;
    if (st.op == "=") {
        val = convert(e(*st.value), target.ty, st.src_line);
    } else {
        auto it = assign_binop().find(st.op);
        if (it == assign_binop().end()) throw Unsupported("复合赋值运算符 " + st.op, st.src_line);
        const HNode cur = target; /* HIR 里 `Local`/`Global` 节点就是「读一次」 */
        val = convert(binop(it->second, cur, e(*st.value), st.src_line), target.ty, st.src_line);
    }
    HNode n = stmt_node(HKind::Assign, st.src_line);
    n.kids.push_back(std::move(target));
    n.kids.push_back(std::move(val));
    parent.kids.push_back(std::move(n));
}

/* ================================================================ 语句：if / #if / switch
 *
 * 三条共用一个前提：`in_scope("名字")` 是**编译期谓词**，不只在 `#if` 里（方案 §2b 补漏 3，
 * U-L14 实测）。判假的块**整块丢掉**（连里面的名字都不解析 —— 参考就是这样的，U-L14 的 V7/H2
 * 两个「未定义名」控制片段证明了这一点）；判真的块原样内联成一条 `Block` 语句，保留它那一层
 * 作用域（`lower_block` 就是这样）。HIR 里**不出现** `in_scope`，也**不折成 `Const`** —— 后者会
 * 让 codegen 分不清「真常量条件」（`if 1` 这种不许剪，它跟 `in_scope` 不是一回事）。
 */

/* `lower.cc` 的 `in_scope_const`。判定三档（U-L14 §0）：
 *   ① 整单元预扫的名字表（`in_scope_names_`，与引用点位置无关、也与自己裁剪掉的块无关）
 *   ② 词法作用域（参数 / 局部 / 同体嵌套 def）—— 本步只有 `lookup`，没有 def
 *   ③ 前缀（stdlib）的名字 —— `def` 与全局都算；**内建不算**（P4/P5）
 * 编译期常量的查询串带不带 `#` 都算（V2/V2b），所以两个方向都查一遍。 */
std::optional<bool> Analyzer::in_scope_const(const Expr& cond) {
    const auto* call = as<CallExpr>(&cond);
    if (call == nullptr) return std::nullopt;
    const auto* id = as<IdentExpr>(call->callee.get());
    if (id == nullptr || id->name != "in_scope" || call->args.size() != 1) return std::nullopt;
    const auto* arg = as<LiteralExpr>(call->args[0].get());
    if (arg == nullptr || (arg->lit_kind != "str" && arg->lit_kind != "template")) {
        user_error("in_scope() 的参数不是字符串字面量", cond.src_line);
    }
    const std::string& t = arg->text;
    if (in_scope_names_.count(t) != 0) return true;
    if (t.empty() || t[0] != '#') {
        if (in_scope_names_.count('#' + t) != 0) return true;
    } else if (in_scope_names_.count(t.substr(1)) != 0) {
        return true;
    }
    if (lookup(t) != nullptr) return true;
    return is_prefix_func(t) || prefix_globals().count(t) != 0;
}

/* `lower.cc` 的 `s_IfStmt`：先把 `in_scope` 的 case 判掉（判真的立刻发块、**后面的 case 与
 * else 全不要**；判假的直接跳过），剩下的才是运行期的 if/elif/else。 */
void Analyzer::if_stmt(HNode& parent, const IfStmt& st) {
    std::vector<const IfCase*> cases;
    for (const auto& case_ : st.cases) {
        const std::optional<bool> decided = in_scope_const(*case_->cond);
        if (decided.has_value()) {
            if (*decided) {
                parent.kids.push_back(block(*case_->block));
                return;
            }
            continue;
        }
        cases.push_back(case_.get());
    }
    if (cases.empty()) {
        if (st.else_block != nullptr) parent.kids.push_back(block(*st.else_block));
        return;
    }
    parent.kids.push_back(if_node(cases, 0, st.else_block.get()));
}

/* if/elif/else 链 → 嵌套的 `If`。`If` 只有三个位（条件 / then / else），所以 elif 那一支必须
 * 包在一条 `Block` 里 —— 这条 `Block` 是**合成的**（只装里层 `If`，不声明任何名字），作用域与
 * `lower` 里「下一个 case 自己一层块」一致。 */
HNode Analyzer::if_node(const std::vector<const IfCase*>& cases, std::size_t at,
                        const Block* else_block) {
    const IfCase& case_ = *cases[at];
    HNode n = stmt_node(HKind::If, case_.src_line);
    const HNode cond = e(*case_.cond);
    as_cond(cond, case_.src_line);
    n.kids.push_back(cond);
    n.kids.push_back(block(*case_.block));
    if (at + 1 < cases.size()) {
        HNode rest = stmt_node(HKind::Block, cases[at + 1]->src_line);
        rest.kids.push_back(if_node(cases, at + 1, else_block));
        n.kids.push_back(std::move(rest));
    } else if (else_block != nullptr) {
        n.kids.push_back(block(*else_block));
    }
    return n;
}

/* `lower.cc` 的 `s_PpIfStmt` + `ppif_true`：每个 case 的条件**必须**是 `in_scope(…)`（否则
 * 报错，不是回退到运行期）—— 一个都没中就走 `else`。命中的块同样保留自己那层作用域。 */
void Analyzer::ppif_stmt(HNode& parent, const PpIfStmt& st) {
    for (const auto& case_ : st.cases) {
        const std::optional<bool> decided = in_scope_const(*case_->cond);
        if (!decided.has_value()) {
            unimplemented("#if 的条件不是 in_scope(\"名字\")", case_->cond->src_line);
        }
        if (*decided) {
            parent.kids.push_back(block(*case_->block));
            return;
        }
    }
    if (st.else_block != nullptr) parent.kids.push_back(block(*st.else_block));
}

/* `lower.cc` 的 `s_SwitchStmt`，逐句对应（**没有** C 语言的 fallthrough）：
 *   · 主体求值一次；每个 `case 值:` 是**独立的**条件，命中就执行自己那个块、不穿到下一个；
 *   · `default` 只在所有 case 都不命中时跑；参考实现把它放到所有 case 之后（`lower` 里是先
 *     把 plain / defaults 分开，只跑 `defaults[0]`），HIR 的 kid 顺序就按这个写；
 *   · `case` 值必须是常量 —— `Str` 主体时是 Str 常量（按内容比），否则按 `pat_const` 取位模式；
 *   · `break` 不进 switch 的标签栈（见 `breaks_` 的注释）。 */
void Analyzer::switch_stmt(HNode& parent, const SwitchStmt& st) {
    HNode n = stmt_node(HKind::Switch, st.src_line);
    const HNode subj = e(*st.subject);
    const bool str_subj = subj.ty->kind == TyKind::Str;
    n.kids.push_back(subj);
    std::vector<const SwitchCase*> plain;
    std::vector<const SwitchCase*> defaults;
    for (const auto& c : st.cases) {
        (c->value != nullptr ? plain : defaults).push_back(c.get());
    }
    for (const SwitchCase* case_ : plain) {
        const std::optional<HNode> cvv = const_value(*case_->value);
        if (str_subj) {
            if (!cvv.has_value() || cvv->ty->kind != TyKind::Str) {
                unimplemented("Str 主体 switch 的 case 值不是 Str 常量", case_->src_line);
            }
            n.kids.push_back(*cvv);
        } else {
            if (!cvv.has_value()) unimplemented("switch case 的值不是常量", case_->src_line);
            n.kids.push_back(const_node(cvv->ty, pat_const(*cvv).value_or(0), case_->src_line));
        }
        n.kids.push_back(block(*case_->block));
    }
    if (!defaults.empty()) n.kids.push_back(block(*defaults[0]->block));
    parent.kids.push_back(std::move(n));
}

/* `lower.cc` 的 `s_WhileStmt`：条件按 `as_cond`（非零为真、不 convert），循环体是一层作用域。
 * `label` 只是 `break <标签>` 要匹配的名字，但它是 `While` 自己的名字，所以落在节点上。 */
void Analyzer::while_stmt(HNode& parent, const WhileStmt& st) {
    const HNode cond = e(*st.cond);
    as_cond(cond, st.src_line);
    HNode n = stmt_node(HKind::While, st.src_line);
    n.label = st.label;
    breaks_.push_back(st.label);
    HNode body = block(*st.block);
    breaks_.pop_back();
    n.kids.push_back(cond);
    n.kids.push_back(std::move(body));
    parent.kids.push_back(std::move(n));
}

/* `lower.cc` 的 `s_BreakStmt`：向内找第一个标签匹配的循环（空标签 = 最近的一层）。
 * 找不到就是 `Unsupported` —— 不能落到外层循环上（那是静默改控制流）。 */
void Analyzer::break_stmt(HNode& parent, const BreakStmt& st) {
    bool found = false;
    for (auto it = breaks_.rbegin(); it != breaks_.rend() && !found; ++it) {
        found = st.label.empty() || st.label == *it;
    }
    if (!found) throw Unsupported("break " + st.label, st.src_line);
    HNode n = stmt_node(HKind::Break, st.src_line);
    n.label = st.label;
    parent.kids.push_back(std::move(n));
}

HNode Analyzer::return_stmt(const ReturnStmt& st) {
    HNode n = stmt_node(HKind::Return, st.src_line);
    if (st.value == nullptr) {
        if (ret_->kind != TyKind::Void) throw Unsupported("非 None 函数里裸 return", st.src_line);
        return n;
    }
    if (ret_->kind == TyKind::Void) throw Unsupported("None 函数里 return 值", st.src_line);
    n.kids.push_back(convert(e(*st.value), ret_, st.src_line));
    return n;
}

/* 左值 = 「位置表达式」，`lower.cc` 的 `lvalue` 的四种形态：标识符 / 下标 / 字段（本步）。
 * 后两种在 `e()` 里也被当作「取一份值」用（读与写在 HIR 里是**同一个节点**，见 `e` 的注释）。
 * 每个形态一个函数：错误模型不混 —— 标识符那支是旧路径（`Unsupported`），下标/字段是本步新写的
 * （只抛 `CompileError`）。 */
HNode Analyzer::lvalue(const Expr& node) {
    if (const auto* id = as<IdentExpr>(&node)) return lvalue_ident(*id);
    if (const auto* ix = as<IndexExpr>(&node)) return index_place(*ix);
    if (const auto* mem = as<MemberExpr>(&node)) return member_place(*mem);
    throw Unsupported("左值形态 " + std::string(ast_kind_name(node.kind)), node.src_line);
}

HNode Analyzer::lvalue_ident(const IdentExpr& id) {
    const std::string& name = id.name;
    if (const Binding* b = lookup(name)) {
        if (b->kind == Binding::Kind::Const) throw Unsupported("给常量 " + name + " 赋值", id.src_line);
        return local_node(name, b->ty, id.src_line);
    }
    auto g = globals_.find(name);
    if (g != globals_.end()) return global_node(g->second.id, g->second.ty, id.src_line);
    if (prefix_globals().count(name) != 0) {
        const GlobalRef pg = prefix_global(name, id.src_line);
        return global_node(pg.id, pg.ty, id.src_line);
    }
    throw Unsupported("未定义的名字 " + name, id.src_line);
}

/* `lower.cc` 的 `lvalue(Index)`：基址必须是 `Arr`/`Str`，下标 `convert` 到 `I64`（无符号零扩展），
 * 步长 = 元素字节宽度（U-L1：地址是字节地址）。步长**不进** HIR：`Index.ty` 已经是元素类型，
 * codegen 自己乘 `storage_bytes(elem)`。 */
HNode Analyzer::index_place(const IndexExpr& ix) {
    const HNode base = e(*ix.obj);
    if (base.ty->kind != TyKind::Array && base.ty->kind != TyKind::Str) {
        unimplemented("下标用在 " + std::string(ty_kind_name(base.ty->kind)) + " 上", ix.src_line);
    }
    HNode n = expr_node(HKind::Index, base.ty->elem, ix.src_line);
    n.kids.push_back(base);
    n.kids.push_back(convert(e(*ix.index), ty_i64(), ix.src_line));
    return n;
}

/* `lower.cc` 的 `lvalue(Member)`：基址必须是 struct，字段按**名字**查布局表（序号 + 字节偏移
 * 都在 `Hir.structs` 里，codegen 不再算）。偏移 0 的字段同样出 `Member` —— HIR 不靠「偏移 0
 * 就省一层」这种优化，那是 codegen 的事。 */
HNode Analyzer::member_place(const MemberExpr& mem) {
    const HNode base = lvalue(*mem.obj);
    if (base.ty->kind != TyKind::Struct) {
        unimplemented("字段访问用在 " + std::string(ty_kind_name(base.ty->kind)) + " 上",
                      mem.src_line);
    }
    auto st = structs_.find(base.ty->name);
    if (st == structs_.end()) our_bug("struct " + base.ty->name + " 没有布局", mem.src_line);
    for (std::size_t i = 0; i < st->second.fields.size(); ++i) {
        const HirField& f = st->second.fields[i];
        if (f.name != mem.name) continue;
        HNode n = expr_node(HKind::Member, f.ty, mem.src_line);
        n.field_index = static_cast<int>(i);
        n.offset = f.offset;
        n.kids.push_back(base);
        return n;
    }
    unimplemented("struct " + base.ty->name + " 没有字段 " + mem.name, mem.src_line);
}

/* ---------------------------------------------------------------- 作用域 */

void Analyzer::bind_local(const std::string& name, TyRef ty) {
    Binding b;
    b.kind = Binding::Kind::Local;
    b.ty = std::move(ty);
    scopes_.back()[name] = std::move(b);
}

void Analyzer::bind_const(const std::string& name, TyRef ty, uint64_t bits) {
    Binding b;
    b.kind = Binding::Kind::Const;
    b.ty = std::move(ty);
    b.bits = bits;
    scopes_.back()[name] = std::move(b);
}

const Analyzer::Binding* Analyzer::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

}  // namespace

Hir analyze(const ModuleSource& src) {
    Hir hir;
    Analyzer a(hir);
    a.run(src);
    return hir;
}

}  // namespace sema
}  // namespace simplexc
