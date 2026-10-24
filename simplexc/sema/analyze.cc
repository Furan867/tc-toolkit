/* sema/analyze.cc —— 已解析的模块 → HIR（②b-1：骨架 + 类型解析 + 常量折叠 + 表达式）。
 *
 * 语义**全部**照 `lower.cc` 的现状搬（方案 §3 的「纯语义保持」）：`ty_of_name` /
 * `ty_of_ref` / `unify` / `arith_ty` / `convert` 的分派 / 常量求值 `cv` / `s_VarDeclStmt` 的
 * 「常量变量」规则 / `make_globals` 的顶层规则。搬的时候只换**值的载体**：`lower` 的
 * `V{ll, ty, c}` 在这里是 `HNode` —— 常量就是 `Const` 节点（`const_bits` 即原来的 `c`），
 * 运行期值就是它自己那个节点（没有指令句柄可用，也不需要：指令是 codegen 的事）。
 * 所以下面的表达式函数与 `lower.cc` 的同名函数逐句对应，**改一处就要同时改另一处**（②c 删
 * `lower` 那一份）。
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
 * 本步**不接**的形态一律抛 `Unsupported`（带行号），不猜语义：`if`/`switch`/`#if`/嵌套 def/
 * 顶层 def/调用（⇒ `Str` 插值与内建也不可达）/`Str` 字面量/数组/结构/枚举/泛型/闭包。
 * 顶层 `def` 抛而不是静默丢掉：丢一个 def 就是静默改产物。
 *
 * 为什么 `While`/`Break` 在本步：语料里程序根的**唯一**形态就是 `run_sim: while true { … }`
 * （`decls.cc` 的 `match_label` 要求同一行跟 while/for/loop），不支持 `while` 就一个根块都建
 * 不出来。两者的结构逐字照 `lower.cc` 的 `s_WhileStmt` / `s_BreakStmt`。
 */
#include "analyze.hh"

#include "errors.hh"

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

/* 前缀（stdlib）里定义、生成源码当作「已存在」用的全局：`lower` 的 `prefix_globals()` 只有
 * 这一个，且是**按需建全局**的。本步只认名字并把「没实现」说清楚（建全局要定它在
 * `Hir.globals` 里的位置，那属于接前缀表的那一步）。 */
bool is_prefix_global(const std::string& n) { return n == "global_seed"; }

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
 * 名字唯一决定）。`Arr[...]` 不走它：两个 `Arr[U8]` 是不同对象，而本步也没有数组值。 */
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
    void make_top_level(const std::vector<const VarDeclStmt*>& top_vars);
    void scan_root_consts(const Block& body);
    HNode root_const_value(const VarDeclStmt& decl);

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
    void while_stmt(HNode& parent, const WhileStmt& st);
    void break_stmt(HNode& parent, const BreakStmt& st);
    HNode return_stmt(const ReturnStmt& st);
    HNode lvalue(const Expr& node);

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
     * 第一个同名的外层循环；找不到就 `Unsupported`（不许落到外层循环上去）。 */
    std::vector<std::string> breaks_;
    std::map<std::string, GlobalRef> globals_;
    std::map<std::string, HirConst> consts_;                  /* 顶层 `const #` */
    std::map<std::string, const VarDeclStmt*> root_consts_;   /* 程序根体里的 `const #` */
};

/* ---------------------------------------------------------------- 模块级 */

void Analyzer::run(const ModuleSource& src) {
    hir_.main.key = "main";   /* 程序根的**逻辑名**；产物符号名 `tc_main` 由 codegen 派生 */
    hir_.main.name = "main";
    hir_.main.ret = ty_void();
    ret_ = ty_void();

    /* 顶层 def 需要实例闭包 + 捕获面，是 ②b 后续两步的事。这里显式报错，不静默丢。 */
    if (!src.defs.empty()) {
        throw Unsupported("顶层 def " + src.defs.front()->name, src.defs.front()->src_line);
    }
    make_top_level(src.top_vars);
    if (src.label == nullptr) throw CompileFail("没有顶层语句块（程序根）");
    scan_root_consts(*src.label->body);
    hir_.main.body = block(*src.label->body);
}

/* 顶层声明（`lower.cc` 的 `make_globals`）：`const #X` 进 `consts`（零条指令），别的都要有
 * 编译期常量初值 → 一个带 `init_bits` 的全局。**按源码顺序**求值：引用只能看到排在前面
 * 的常量（原版就是这么逐条建的，顺序不能改）。 */
void Analyzer::make_top_level(const std::vector<const VarDeclStmt*>& top_vars) {
    for (const VarDeclStmt* st : top_vars) {
        if (st->kw == "const" && !st->name.empty() && st->name[0] == '#') {
            auto v = const_value(*st->init);
            if (!v) throw Unsupported("编译期常量 " + st->name + " 的初始化式不是常量", st->src_line);
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
 * 声明之前）。它只收**直接语句** —— 不钻裸块、也不钻 def 体，这里照抄同一个口径（本步没有
 * `if`/`while`/`switch` 可钻）。 */
void Analyzer::scan_root_consts(const Block& body) {
    for (const StmtPtr& st : body.stmts) {
        const auto* v = as<VarDeclStmt>(st.get());
        if (v != nullptr && v->kw == "const" && !v->name.empty() && v->name[0] == '#') {
            root_consts_.emplace(v->name, v);
        }
    }
}

HNode Analyzer::root_const_value(const VarDeclStmt& decl) {
    auto v = const_value(*decl.init);
    if (!v) throw Unsupported("编译期常量 " + decl.name + " 的初始化式不是常量", decl.src_line);
    return *v;
}

/* ---------------------------------------------------------------- 类型 */

/* `lower.cc` 的 `ty_of_name`。两处**刻意缺失**：`enums_` / `structs_` 的查表 —— 本步的输入
 * 里没有 `type` 声明，所以枚举/struct 名字落进最后那个 `Unsupported`（而不是被猜成整数）。 */
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
        default:
            throw Unsupported("表达式形态 " + std::string(ast_kind_name(node.kind)), node.src_line);
    }
}

HNode Analyzer::e_literal(const LiteralExpr& node) {
    if (node.lit_kind == "int") {
        return const_node(ty_i64(), static_cast<uint64_t>(node.value) & kMask64, node.src_line);
    }
    /* `str` / `template`：字符串池与模板渲染是后续步骤（`Hir.strings` 那一支）。 */
    throw Unsupported("字面量 " + node.lit_kind, node.src_line);
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
    if (is_prefix_global(name)) throw Unsupported("前缀全局 " + name, node.src_line);
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
    if (ty->kind == TyKind::Str) throw Unsupported("Str 比较（" + op + "）", line);
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

/* `lower.cc` 的 `const_value`：常量求值失败 = 「这个初始化式不是常量」，不是错误本身。 */
std::optional<HNode> Analyzer::const_value(const Node& node) {
    try {
        return cv(node);
    } catch (const Unsupported&) {
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

/* 左值形态（`lower.cc` 的 `lvalue` 里本步能到的那一支）：本步只有标识符 —— 下标/字段要
 * 数组与 struct 的布局。 */
HNode Analyzer::lvalue(const Expr& node) {
    const auto* id = as<IdentExpr>(&node);
    if (id == nullptr) {
        throw Unsupported("左值形态 " + std::string(ast_kind_name(node.kind)), node.src_line);
    }
    const std::string& name = id->name;
    if (const Binding* b = lookup(name)) {
        if (b->kind == Binding::Kind::Const) {
            throw Unsupported("给常量 " + name + " 赋值", node.src_line);
        }
        return local_node(name, b->ty, node.src_line);
    }
    auto g = globals_.find(name);
    if (g != globals_.end()) return global_node(g->second.id, g->second.ty, node.src_line);
    if (is_prefix_global(name)) throw Unsupported("前缀全局 " + name, node.src_line);
    throw Unsupported("未定义的名字 " + name, node.src_line);
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
