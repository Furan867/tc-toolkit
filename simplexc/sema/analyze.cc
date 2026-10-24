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

#include "builtins.hh"
#include "errors.hh"
#include "layout.hh"

#include "../frontend/ast.hh"

#include <cstddef>
#include <cstdint>
#include <functional>
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

/* ---------------------------------------------------------------- 泛型与捕获的小件 */

/* 类型参数写在形参类型那一格上（`x: @Type`）。只有**整格**是类型参数才算 —— `Arr[@Any]` 这种
 * 嵌套拼写在 `ty_of_ref` 就报错了，与 `lower.cc` 的 `is_type_param` 同一条口径。 */
bool is_type_param(const TypeRef& ref) {
    return ref.at && (ref.name == "Type" || ref.name == "Size" || ref.name == "Any");
}

bool has_type_params(const std::vector<std::unique_ptr<Param>>& params) {
    for (const auto& p : params) {
        if (p->typ != nullptr && is_type_param(*p->typ)) return true;
    }
    return false;
}

/* 先序遍历一个节点的**子节点**（列表字段也下钻），字段顺序照 `ast.hh` 的声明序。
 * 只有算捕获表要用它：原型按「遍历所有 dataclass 字段」求自由变量，而那个顺序决定捕获在闭包
 * 环境里的槽位序号（`HCapture.index` / `$c<n>`）—— 换顺序就是换产物，不是换个遍历方法。 */
template <class F>
void for_each_child(const Node& n, F&& f) {
    switch (n.kind) {
        case NK::Cast: f(*static_cast<const CastExpr&>(n).expr); break;
        case NK::Unary: f(*static_cast<const UnaryExpr&>(n).expr); break;
        case NK::Binary: {
            const auto& x = static_cast<const BinaryExpr&>(n);
            f(*x.left);
            f(*x.right);
            break;
        }
        case NK::Cond: {
            const auto& x = static_cast<const CondExpr&>(n);
            f(*x.cond);
            f(*x.then_);
            f(*x.else_);
            break;
        }
        case NK::Call: {
            const auto& x = static_cast<const CallExpr&>(n);
            f(*x.callee);
            for (const auto& a : x.args) f(*a);
            break;
        }
        case NK::Member: f(*static_cast<const MemberExpr&>(n).obj); break;
        case NK::Index: {
            const auto& x = static_cast<const IndexExpr&>(n);
            f(*x.obj);
            f(*x.index);
            break;
        }
        case NK::ArrayLit:
            for (const auto& it : static_cast<const ArrayLitExpr&>(n).items) f(*it);
            break;
        case NK::CompositeLit:
            for (const auto& fi : static_cast<const CompositeLitExpr&>(n).fields) f(*fi);
            break;
        case NK::Template:
            for (const auto& p : static_cast<const TemplateExpr&>(n).parts) f(*p);
            break;
        case NK::FieldInit: f(*static_cast<const FieldInit&>(n).value); break;
        case NK::Block:
            for (const auto& s : static_cast<const Block&>(n).stmts) f(*s);
            break;
        /* 裸块的体是 `BlockStmt` 自己那个字段，不走 `NK::Block` 那条 —— 漏掉它会让捕获表
         * 看不见块内的自由变量。 */
        case NK::BlockStmt: f(*static_cast<const BlockStmt&>(n).block); break;
        case NK::VarDecl: f(*static_cast<const VarDeclStmt&>(n).init); break;
        case NK::Assign: {
            const auto& x = static_cast<const AssignStmt&>(n);
            f(*x.target);
            f(*x.value);
            break;
        }
        case NK::ExprStmt: f(*static_cast<const ExprStmt&>(n).expr); break;
        case NK::IfCase: {
            const auto& x = static_cast<const IfCase&>(n);
            f(*x.cond);
            f(*x.block);
            break;
        }
        case NK::If: {
            const auto& x = static_cast<const IfStmt&>(n);
            for (const auto& c : x.cases) f(*c);
            if (x.else_block != nullptr) f(*x.else_block);
            break;
        }
        case NK::PpIf: {
            const auto& x = static_cast<const PpIfStmt&>(n);
            for (const auto& c : x.cases) f(*c);
            if (x.else_block != nullptr) f(*x.else_block);
            break;
        }
        case NK::While: {
            const auto& x = static_cast<const WhileStmt&>(n);
            f(*x.cond);
            f(*x.block);
            break;
        }
        case NK::SwitchCase: {
            const auto& x = static_cast<const SwitchCase&>(n);
            f(*x.block);
            if (x.value != nullptr) f(*x.value);
            break;
        }
        case NK::Switch: {
            const auto& x = static_cast<const SwitchStmt&>(n);
            f(*x.subject);
            for (const auto& c : x.cases) f(*c);
            break;
        }
        case NK::Return:
            if (static_cast<const ReturnStmt&>(n).value != nullptr) {
                f(*static_cast<const ReturnStmt&>(n).value);
            }
            break;
        case NK::Def: {
            const auto& x = static_cast<const DefStmt&>(n);
            for (const auto& p : x.params) f(*p);
            f(*x.body);
            break;
        }
        case NK::Function: {
            const auto& x = static_cast<const FunctionDef&>(n);
            for (const auto& p : x.params) f(*p);
            f(*x.body);
            break;
        }
        default: break; /* 叶子节点与 Param（只有类型拼写） */
    }
}

/* 捕获签名（`lower.cc` 的 `ensure_def_fn` 里那段）：`名字:类型键` 逗号分隔。
 * **不许排序、不许去重后再排** —— 它既是实例键里区分闭包面的那一段，也是 `$c<n>` 的计数依据。 */
std::string capture_sig(const std::vector<HCapture>& caps) {
    std::string out;
    for (std::size_t j = 0; j < caps.size(); ++j) {
        if (j != 0) out += ',';
        out += caps[j].name;
        out += ':';
        out += ty_key(*caps[j].ty);
    }
    return out;
}

/* 重载粗判（`lower.cc` 的 `compatible`）：种类对得上、Int 之间一律算匹配、Enum/Struct 看名字。 */
bool compatible(const TyRef& arg, const TyRef& param) {
    if (!arg || arg->kind != param->kind) return false;
    if (arg->kind == TyKind::Int) return true;
    if (arg->kind == TyKind::Enum || arg->kind == TyKind::Struct) {
        return arg->name == param->name;
    }
    return true;
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

/* 常量**引用点**：整数常量带位模式，非整数常量（串/数组/聚合）带 `const_id`（`Hir.consts` 下标）。
 * span 记**引用点**的行，所以同一个常量在不同引用点的 dump 行不同 —— 但 `const_id` 相同，
 * 一眼能看出它们在引用同一个条目。 */
HNode const_ref(const TyRef& ty, uint64_t bits, int const_id, int line) {
    HNode n = const_node(ty, bits, line);
    n.const_id = const_id;
    return n;
}

HNode local_node(const std::string& name, TyRef ty, int line) {
    HNode n = expr_node(HKind::Local, std::move(ty), line);
    n.name = name;
    return n;
}

/* 捕获的读/写都是**这一个节点**：`capture_index` 指向 `HirFunc.caps` 的槽位序号（= 现状闭包
 * 参数表里的位置）。名字不留在节点上（§2b 的载荷表只有序号），codegen 靠 `caps[index]` 查。 */
HNode capture_node(TyRef ty, int index, int line) {
    HNode n = expr_node(HKind::Capture, std::move(ty), line);
    n.capture_index = index;
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
 * 非 `Const` 节点返回空 —— 「加宽按符号性扩」只对常量成立。带 `const_id` 的 `Const`（串/数组/
 * 聚合常量）也返回空：它的值不是位模式，当成 0 折叠就是静默错值。 */
std::optional<uint64_t> pat_const(const HNode& n) {
    if (n.kind != HKind::Const) return std::nullopt;
    if (n.const_id >= 0) return std::nullopt;
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
    if (n.kind != HKind::Const || n.const_id >= 0) {
        throw Unsupported("（内部）常量求值拿到非常量", line);
    }
    return n.const_bits;
}

/* 顶层全局初值的位模式（`HirGlobal.init_bits` 只有一个 u64 —— 方案 §2b 的冻结形状）。
 * 语料里非整型/非指针的顶层初值只有 `Input {}` / `Output {}` 这一种：每个字段都是零值，所以
 * 位模式就是 0（codegen 按 `ty` 的完整宽度写 `zeroinitializer`）。别的聚合常量没有表示位，
 * 显式报未实现 —— 不许挑一个字段代替整个聚合（那是静默错值）。 */
uint64_t global_init_bits(const HNode& v, int line) {
    /* 带 `const_id` 的 `Const` 是串/数组/聚合常量：它没有 64 位位模式，拿 `const_bits`（0）当
     * 初值就是静默错值。 */
    if (v.kind == HKind::Const && v.const_id < 0) return v.const_bits;
    if (v.kind == HKind::CompositeLit) {
        for (const HNode& field : v.kids) {
            if (field.kind != HKind::Const || field.const_bits != 0) {
                throw Unsupported("顶层 var 的复合字面量初值不是全零", line);
            }
        }
        return 0;
    }
    /* 串/数组初始化式折出来的是 `StrLit` / `ArrayLit` 节点（串池 id / 载荷），而 `init_bits` 只能放
     * 位模式 —— 所以这里显式报，不拿 0 充数（那就是静默的空指针/空数组）。语料里没有这种顶层
     * 初始化式（98/98 份只有整型、指针、`raw_cast` 与空聚合）。 */
    throw Unsupported("顶层 var 的初值不是位模式（字符串/数组初值未接）", line);
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

struct GlobalRef {
    int id = -1;
    TyRef ty;
};

/* 一个 def 的实例化决定（`lower.cc` 的 `Inst`）：泛型绑定 + 运行期形参 + 规范化签名。
 * `rt_*` 四个数组按下标对齐：`rt_params[i]` 是第 i 个**运行期**形参，`rt_types[i]` 是它的类型，
 * `rt_arg[i]` 是它在调用点实参表里的下标 —— 类型实参不占运行期槽（`g(a, <U8>)`），所以两者可以
 * 错位。（类型形绑定的那个形式参确实没有运行期槽 —— 体内只能拿它当 `size()`/`type_of()` 的类型
 * 来源，读它的值是无定义的。对齐这件事是必须的：按声明位次取 `rt_params` 时，类型参数排在值参数
 * **前面**的写法会把名字和类型错配成「把 k 绑成 U64 却叫 n」。）
 * `sig` 就是实例键里 `$` 后面那一段（非泛型 def 是空串，⇒ 键与函数名都不带 `$`）。 */
struct Inst {
    std::vector<const Param*> rt_params;
    std::vector<TyRef> rt_types;
    std::vector<std::size_t> rt_arg;
    std::map<std::string, TyRef> binds;
    std::vector<TyRef> bind_order;
    std::string sig;
};

/* 一个 def 的捕获面（`lower.cc` 的 `Closure`）：`caps` 按**首次遇到**的顺序排 —— 那个顺序同时
 * 是闭包环境里的槽位序号（`HCapture.index`）与 `$c<n>` 里的 n，所以不能排序、不能去重后再排。
 * `defs` 是被摊平进来的兄弟 def：它们的捕获要经调用者带进来（`collect_captures` 的注释）。 */
struct Closure {
    std::vector<HCapture> caps;
    /* 被摊平进来的兄弟 def + 它们的**词法**归属（`owner|名字@行` 那个串）。归属必须在这里带
     * 下来：兄弟 def 声明在**外层**的体里，而它是在内层的实例里被绑进作用域的 —— 拿当前实例
     * 的词法键当它的归属，互调的链路就会一圈长一段（实测 `ping`/`pong` 直接不收敛）。 */
    std::vector<std::pair<const DefStmt*, std::string>> defs;
};

/* 一个 def 的词法键：`归属 + "|" + 名字 + "@" + 声明行`。只由**声明位置**决定。 */
std::string lex_of(const std::string& owner, const DefStmt& st) {
    return owner + "|" + st.name + "@" + std::to_string(st.src_line);
}

/* 模块级共享上下文：一次分析只有一份，所有函数实例共读共写。
 * 为什么把 `Analyzer` 拆成「模块」+「每函数」两层：实例闭包是**递归**的（def 体里发现新实例就
 * 得当场把那个体也算完，见 `instantiate_top`），递归下去时新建一个 `Analyzer` 当子上下文，
 * 不去保存-恢复一堆成员（方案 §3.3 的硬要求）。 */
struct Module {
    Module(Hir& h, const ModuleSource& s) : hir(h), src(s) {}

    Hir& hir;
    const ModuleSource& src;
    /* 顶层 def：同名多个 = 重载，按声明序（`pick_def` 的候选表）。 */
    std::map<std::string, std::vector<const FunctionDef*>> defs;
    /* 程序根体里的嵌套 def：整单元可见、且前向可见（`lower.cc` 的 `root_defs_`）。 */
    std::map<std::string, const DefStmt*> root_defs;
    std::map<std::string, GlobalRef> globals;
    /* 顶层 `const #`：名字 → **引用节点**（整数带位模式，非整数带 `const_id`）。 */
    std::map<std::string, HNode> consts;
    std::map<std::string, const VarDeclStmt*> root_consts;   /* 程序根体里的 `const #` */
    /* `const #` 声明 → 已折好的引用节点（`lower.cc` 的 `root_const_values_` 同一件事）：同一个
     * 声明只折一次、非整数常量只占一个 `Hir.consts` 条目。按**声明节点指针**备。 */
    std::map<const VarDeclStmt*, HNode> const_by_decl;
    std::map<std::string, TyRef> enums;                      /* 枚举类型名 → 类型 */
    std::map<std::string, std::pair<std::string, int>> enum_member; /* 成员 → (枚举, 序号) */
    std::map<std::string, StructLayout> structs;             /* struct 名 → 布局 */
    std::map<std::pair<std::string, bool>, int> string_ids;  /* (字节, cstring) → 串池下标 */
    /* `in_scope` 的 ① 档：整单元预扫的名字表（`lower.cc` 的 `mod_->in_scope_names`）。必须在任何剪枝
     * 之前建好，而且只能看**未经裁剪的** AST（U-L14 的 F1/F2）。 */
    std::set<std::string> in_scope_names;
    /* 实例闭包的两张表：`instance` 是「规范化实例键 → `hir.funcs` 下标」的**唯一**一张表；
     * `cap_instances` 复现 `lower` 的每 (def, lo) 捕获实例计数（`$c<n>` 的 n 与 64 上限）。 */
    std::map<std::string, int> instance;
    std::map<std::pair<std::string, int>, int> cap_instances;
};

class Analyzer {
public:
    explicit Analyzer(Module& m) : mod_(&m) {}
    /* 程序根：模块级建表 + 根体。
     * 下面两个 `run_*_def` 是**子上下文入口**（递归分析一个函数体），不开新作用域链之外的东西。 */
    void run(const ModuleSource& src);

private:
    struct Binding {
        enum class Kind { Local, Capture, Const, Def };
        Kind kind = Kind::Local;
        TyRef ty;                     /* Local / Capture */
        uint64_t bits = 0;            /* Const：已按 ty 截断的位模式（整数常量） */
        int const_id = -1;            /* Const：非整数常量的身份（`Hir.consts` 下标） */
        int index = -1;               /* Capture：闭包环境里的槽位序号 */
        const DefStmt* def = nullptr; /* Def */
        std::string owner_lex;        /* Def：它声明在那一层的词法键（键里的归属段） */
    };

    /* ---- 模块级 */
    void make_types(const ModuleSource& src);
    void scan_scope_names(const ModuleSource& src);
    void make_top_level(const std::vector<const VarDeclStmt*>& top_vars);
    void scan_root_consts(const Block& body);
    HNode const_decl_value(const VarDeclStmt& decl);
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
    HNode composite_lit_node(const CompositeLitExpr& node, bool const_ctx = false);
    HNode template_expr(const TemplateExpr& node);
    HNode convert(HNode v, const TyRef& target, int line);
    HNode binop(const std::string& op, const HNode& a, const HNode& b, int line);
    HNode compare(const std::string& op, const HNode& a, const HNode& b, int line);
    HNode logical(const std::string& op, const HNode& a, const HNode& b, int line);
    HNode as_cond(const HNode& v, int line);

    /* 常量求值（`lower.cc` 的 `const_value` / `cv`）：只认常量表达式，其它一律 nullopt / throw。 */
    std::optional<HNode> const_value(const Node& node);
    HNode cv(const Node& node);

    /* ---- 调用与实例闭包
     *
     * 实例闭包是**深度优先、边算边登记**的：`instantiate_*` 先把实例键记进 `mod_->instance` 与
     * `hir.funcs`，再递归分析它的体 —— 自引用与互调靠这条收敛（回头再调同一个键就复用下标）。
     *
     * 枚举顺序**不能随手改**：`$c<n>` 的 n 与 `hir.funcs` 的下标都取自「第一次遇到」的顺序，
     * 而这个顺序就是下层语料产物的名字序列。现状（`lower.cc`）的顺序是「先 ensure 被调者
     * （连它的体一起算完），再算实参」，所以 `call_user` / `call_nested` 也必须先
     * `instantiate_*` 再求实参；语句/表达式按源码顺序遍历。改成先算实参、或改成广度优先，
     * 产物里的 `$c0`/`$c1` 就会对调。 */
    HNode e_call(const CallExpr& node);
    HNode call_named(const std::string& name, const CallExpr& node);
    HNode call_user(const FunctionDef& d, const CallExpr& node);
    HNode call_nested(const DefStmt& st, const CallExpr& node, const std::string& owner_lex);    HNode call_load(const CallExpr& node);
    HNode call_store(const CallExpr& node);
    HNode call_raw_cast(const CallExpr& node);
    HNode call_asr(const CallExpr& node);
    HNode call_clz(const CallExpr& node);
    HNode call_ror(const CallExpr& node);
    HNode call_minmax(HBuiltin id, const CallExpr& node);
    HNode call_array(const CallExpr& node);
    HNode call_random(const CallExpr& node);
    HNode call_method(const MemberExpr& callee, const CallExpr& node);
    HNode call_size(const CallExpr& node);
    HNode call_prefix(const std::string& name, const CallExpr& node);
    HNode builtin_node(HBuiltin id, TyRef ty, std::vector<HNode> kids, int line);
    void def_stmt(HNode& parent, const DefStmt& st);

    const FunctionDef* pick_def(const std::string& name, const CallExpr& node);
    Inst resolve_inst(const std::vector<std::unique_ptr<Param>>& params, const CallExpr& node, int line);
    std::optional<std::vector<TyRef>> decl_param_types(const FunctionDef& d);
    std::string inst_sig(const FunctionDef& d, const Inst& inst);
    TyRef inst_ret_type(const TypeRef* ret, const Inst& inst, int line);
    TyRef defstmt_ret(const DefStmt& st);
    std::vector<TyRef> defstmt_params(const DefStmt& st);
    TyRef static_type(const Node& node);
    TyRef type_value_of(const Expr& node, int line);
    TyRef operand_type(const Expr& node, int line);

    /* 登记一个实例并返回它在 `hir.funcs` 里的下标；体由子上下文（`run_*_def`）算。 */
    int instantiate_top(const FunctionDef& d, const Inst& inst, const Closure& cl);
    int instantiate_nested(const DefStmt& st, const std::vector<TyRef>& param_tys, const Closure& cl,
                           const std::string& owner_lex);
    void run_top_def(const FunctionDef& d, const Inst& inst, const std::vector<HCapture>& caps, int index);
    void run_nested_def(const DefStmt& st, const std::vector<TyRef>& param_tys, const Closure& cl,
                        const std::string& lex, int index);
    Closure captures_of(const DefStmt& st, const std::string& owner_lex);
    Closure def_captures(const FunctionDef& d);
    void collect_captures(const Block& body, const std::vector<std::string>& params,
                          bool into_own_defs, Closure& out, std::set<const DefStmt*>& visiting,
                          const std::string& owner_lex);

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
    void bind_const(const std::string& name, HNode value);
    void bind_capture(const std::string& name, TyRef ty, int index);
    void bind_def(const std::string& name, const DefStmt* def, const std::string& owner_lex);
    const Binding* lookup(const std::string& name) const;

    Module* mod_;
    TyRef ret_; /* 当前函数的返回类型（程序根是 `None`） */
    std::vector<std::map<std::string, Binding>> scopes_;
    /* `break` 的标签栈（与 `lower` 的 `breaks_` 同构）：空标签 = 最近的一层，带标签则向内找
     * 第一个同名的外层循环；找不到就 `Unsupported`（不许落到外层循环上去）。
     * `switch` 不进这个栈：`lower.cc` 的 `s_SwitchStmt` 把每个 case 拆成独立的 if，`break`
     * 一直是穿到外层循环的。 */
    std::vector<std::string> breaks_;
    /* 当前实例的泛型绑定（`lower.cc` 的 `ty_binds_`）：类型实参不占运行期槽，所以「名字 → 具体
     * 类型」只能从这里来（`operand_type`）。 */
    std::map<std::string, TyRef> ty_binds_;
    /* 当前实例的**词法**键：`main` / 顶层 def 名 / 嵌套 def 的 `owner|名字@行` 链。
     * 它只由**声明位置**决定，不含实例序号与捕获面 —— 递归的嵌套 def 靠这一点收敛
     * （拿实例键当 owner 时，自递归每绕一圈键就长一段，实例无界增长，实测栈溢出）。 */
    std::string cur_key_;
};

/* ---------------------------------------------------------------- 模块级 */

void Analyzer::run(const ModuleSource& src) {
    mod_->hir.main.key = "main";   /* 程序根的**逻辑名**；产物符号名 `tc_main` 由 codegen 派生 */
    mod_->hir.main.name = "main";
    mod_->hir.main.ret = ty_void();
    ret_ = ty_void();
    cur_key_ = "main"; /* 根体里的嵌套 def 拿它当 owner（现状同样用 `cur_name_ = "main"`） */

    make_types(src);
    scan_scope_names(src); /* 里面把顶层 def（重载表）与根体嵌套 def 登记进 `Module` */
    make_top_level(src.top_vars);
    if (src.label == nullptr) throw CompileFail("没有顶层语句块（程序根）");
    scan_root_consts(*src.label->body);
    HNode body = block(*src.label->body);
    mod_->hir.main.body = std::move(body);
}


/* `type` 声明（`lower.cc` 的 `Program::scan_decls`）：枚举先建（struct 的字段可以引用枚举），
 * struct 的布局与位宽由 `layout_struct` 一处算。
 * 与 `lower` 的差别只有一处：`lower` 在 collect 模式下会把单个 struct 的失败记成诊断继续跑，
 * 这里直接抛 —— 判据要的是「能编过 / 编不过」，不是「编过一半」。 */
void Analyzer::make_types(const ModuleSource& src) {
    for (const EnumSource& d : src.enums) {
        mod_->enums[d.name] = layout_enum(d.name, d.members.size());
        for (std::size_t i = 0; i < d.members.size(); ++i) {
            const auto ins = mod_->enum_member.insert(
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
        mod_->hir.structs.push_back(HirStruct{d.name, lay.ty->bits, lay.fields});
        mod_->structs[d.name] = std::move(lay);
    }
}

/* `in_scope` ① 档的整单元预扫（`lower.cc` 的 `scan_scope_names`）。名字表取**未经 `in_scope`
 * 裁剪的原 AST**（U-L14 的 F1/F2：写在由自己守卫的块里、或写在被丢掉的块里的 def 都算），
 * 所以它只需一遍、不必到不动点。
 * 逐档对应 `docs/facts/semantics.md` / `docs/notes/2026-10-18-ul14-in-scope.md` §0：这一档收
 * `def` / `type`（含 enum 类型名与成员）/ 顶层 `const #X` / 程序根整棵语句树里的嵌套 def 与
 * `const #`。`var` 不算（它的可见性跟声明顺序走）。 */
void Analyzer::scan_scope_names(const ModuleSource& src) {
    for (const FunctionDef* d : src.defs) {
        mod_->in_scope_names.insert(d->name);
        /* 同名多个 = 重载，按声明序（`pick_def` 的候选表）。 */
        mod_->defs[d->name].push_back(d);
    }
    for (const EnumSource& d : src.enums) {
        mod_->in_scope_names.insert(d.name);
        for (const std::string& member : d.members) mod_->in_scope_names.insert(member);
    }
    for (const StructSource& d : src.structs) mod_->in_scope_names.insert(d.name);
    for (const VarDeclStmt* st : src.top_vars) {
        if (st->kw == "const" && !st->name.empty() && st->name[0] == '#') {
            mod_->in_scope_names.insert(st->name);
        }
    }
    if (src.label == nullptr) return;
    std::vector<const DefStmt*> defs;
    std::vector<const VarDeclStmt*> consts;
    collect_root_decls(*src.label->body, defs, consts);
    for (const DefStmt* d : defs) {
        mod_->in_scope_names.insert(d->name);
        /* 根体里的嵌套 def：整单元可见 + 前向可见（`call_named` / `collect_captures` 两张表都查它）。
         * 同名时后声明的赢（与 `lower.cc` 的 `root_defs_[d->name] = d` 同）。 */
        mod_->root_defs[d->name] = d;
    }
    for (const VarDeclStmt* v : consts) mod_->in_scope_names.insert(v->name);
}

/* 顶层声明（`lower.cc` 的 `make_globals`）：`const #X` 进 `consts`（零条指令），别的都要有
 * 编译期常量初值 → 一个带 `init_bits` 的全局。**按源码顺序**求值：引用只能看到排在前面
 * 的常量（原版就是这么逐条建的，顺序不能改）。 */
void Analyzer::make_top_level(const std::vector<const VarDeclStmt*>& top_vars) {
    for (const VarDeclStmt* st : top_vars) {
        if (st->kw == "const" && !st->name.empty() && st->name[0] == '#') {
            const HNode ref = const_decl_value(*st);
            if (ref.const_id < 0) {
                /* 整数常量：一个具名条目（dump/诊断用），值在 `bits`。 */
                HirConst c;
                c.name = st->name;
                c.ty = ref.ty;
                c.bits = ref.const_bits;
                mod_->hir.consts.push_back(c);
            }
            /* 非整数常量的条目已由 `const_decl_value` 建好（值在池里、引用点是 `const_id`）。 */
            mod_->consts[st->name] = ref;
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
        g.init_bits = global_init_bits(*v, st->src_line);
        mod_->globals[g.name] = GlobalRef{static_cast<int>(mod_->hir.globals.size()), g.ty};
        mod_->hir.globals.push_back(std::move(g));
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
    for (const VarDeclStmt* v : consts) mod_->root_consts[v->name] = v;   /* 同名：后声明的赢（`lower` 同） */
}

/* 前缀全局**按需建**（`lower.cc` 的 `prefix_global`）：第一次被引用才进 `Hir.globals`，所以它的
 * 下标跟在顶层全局后面 —— 与今天「`make_globals` 先建完、前缀用到时才加」的顺序一致。 */
GlobalRef Analyzer::prefix_global(const std::string& name, int line) {
    auto hit = mod_->globals.find(name);
    if (hit != mod_->globals.end()) return hit->second;
    auto it = prefix_globals().find(name);
    if (it == prefix_globals().end()) unimplemented("前缀全局 " + name, line);
    HirGlobal g;
    g.name = name;
    g.ty = it->second.ty;
    g.init_bits = it->second.init;
    GlobalRef ref{static_cast<int>(mod_->hir.globals.size()), g.ty};
    mod_->globals[name] = ref;
    mod_->hir.globals.push_back(std::move(g));
    return ref;
}

/* 串池（`lower.cc` 的 `str_pool_`）：同一串字面量只占一个槽（`Hir.strings` 下标），
 * `cstring` 也算键的一部分（两条字节相同但补零形态不同的串不是同一个）。 */
int Analyzer::intern_string(const std::string& bytes) {
    const std::pair<std::string, bool> key{bytes, false};
    auto hit = mod_->string_ids.find(key);
    if (hit != mod_->string_ids.end()) return hit->second;
    const int id = static_cast<int>(mod_->hir.strings.size());
    mod_->hir.strings.push_back(HirString{bytes, false});
    mod_->string_ids[key] = id;
    return id;
}

HNode Analyzer::const_decl_value(const VarDeclStmt& decl) {
    auto hit = mod_->const_by_decl.find(&decl);
    if (hit != mod_->const_by_decl.end()) return hit->second;
    auto v = const_value(*decl.init);
    if (!v) throw Unsupported("编译期常量 " + decl.name + " 的初始化式不是常量", decl.src_line);
    HNode ref = *v;
    if (v->kind != HKind::Const) {
        HirConst c;
        c.name = decl.name;
        c.ty = v->ty;
        c.value = *v;
        mod_->hir.consts.push_back(std::move(c));
        ref = const_node(v->ty, 0, decl.src_line);
        ref.const_id = static_cast<int>(mod_->hir.consts.size()) - 1;
    }
    mod_->const_by_decl[&decl] = ref;
    return ref;
}

/* ---------------------------------------------------------------- 类型 */

/* `lower.cc` 的 `ty_of_name`。`mod_->enums` / `mod_->structs` 的查表位置与它一致（在 `U*`/`S*` 数字档
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
    if (mod_->enums.count(name) != 0) return mod_->enums.at(name);
    if (mod_->structs.count(name) != 0) return mod_->structs.at(name).ty;
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
        case NK::Call: return e_call(static_cast<const CallExpr&>(node));
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

/* U-L10 的转义表（`docs/facts/semantics.md` 实测）：`\0 \a \f \v \e \n \r` 是标准含义，
 * 但 **`\t` → 0x08**（不是 0x09，参考自己的表如此），`\b` / `\x42` / `\"` **不认**（原样保留
 * 反斜杠），`\\` 参考自己解析失败。解码只在 sema 做一次：串池里放**解码后的字节**。 */
std::string unescape(const std::string& text, int line) {
    if (text.find('\\') == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            out.push_back(text[i]);   /* 末尾单独的 '\' 只可能是源里的最后一位，原样留 */
            continue;
        }
        switch (text[i + 1]) {
            case '0': out.push_back('\0'); break;
            case 'a': out.push_back('\a'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case 'e': out.push_back('\x1b'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\x08'); break;
            case '\\': unimplemented("`\\\\` 转义（参考自己解析失败，U-L10）", line);
            default:
                out.push_back('\\');
                out.push_back(text[i + 1]);
                break;
        }
        ++i;
    }
    return out;
}

/* 字符串字面量 → 一个 `StrLit`（字节进串池，`Hir.strings`）。
 *   · 转义按 U-L10 的表解（`unescape`）；`\\` 那个形态报掉（参考也解析不过）。
 *   · 末尾补 0 与 `len << 16` 头是**串表布局**（`lower` 对所有字面量都补），属 codegen 细节，
 *     所以 `HirString.cstring` 在本步恒为 false（语料里 0 处 `c'…'` 形态）。
 *   · 语料里两条字节相同的字面量只占一个槽（`lower` 的 `str_pool_` 就是按字节去重的）。参考那边
 *     两个同样的字面量指针不同（U-L10 的旁证，未定论），但我们只保证产物等价，不去凑那个细节。 */
HNode Analyzer::string_literal(const std::string& text, int line) {
    HNode n = expr_node(HKind::StrLit, ty_str(), line);
    n.string_id = intern_string(unescape(text, line));
    return n;
}

HNode Analyzer::e_ident(const IdentExpr& node) {
    const std::string& name = node.name;
    if (name == "true" || name == "false") {
        return const_node(ty_bool(), name == "true" ? 1u : 0u, node.src_line);
    }
    if (const Binding* b = lookup(name)) {
        if (b->kind == Binding::Kind::Local) return local_node(name, b->ty, node.src_line);
        if (b->kind == Binding::Kind::Capture) {
            return capture_node(b->ty, b->index, node.src_line);
        }
        if (b->kind == Binding::Kind::Const) return const_ref(b->ty, b->bits, b->const_id, node.src_line);
        throw Unsupported("def 名 " + name + " 当值用", node.src_line);
    }
    auto g = mod_->globals.find(name);
    if (g != mod_->globals.end()) return global_node(g->second.id, g->second.ty, node.src_line);
    if (prefix_globals().count(name) != 0) {
        const GlobalRef pg = prefix_global(name, node.src_line);
        return global_node(pg.id, pg.ty, node.src_line);
    }
    /* 枚举成员是整数常量（序号从 0 起），类型是枚举自己 —— 与 `lower.cc` 的 `e_IdentExpr`
     * 同一个位置（在 `mod_->consts` 之前）。 */
    auto em = mod_->enum_member.find(name);
    if (em != mod_->enum_member.end()) {
        return const_node(mod_->enums.at(em->second.first), static_cast<uint64_t>(em->second.second),
                          node.src_line);
    }
    auto c = mod_->consts.find(name);
    if (c != mod_->consts.end()) {
        return const_ref(c->second.ty, c->second.const_bits, c->second.const_id, node.src_line);
    }
    auto hc = hash_consts().find(name);
    if (hc != hash_consts().end()) {
        return const_node(hc->second.ty, hc->second.value, node.src_line);
    }
    auto rc = mod_->root_consts.find(name);
    if (rc != mod_->root_consts.end()) return const_decl_value(*rc->second);
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
    HNode inner = e(*node.expr);
    /* 旧的 `convert` 对**显式** cast 无条件补掩码 —— 目标类型与源相同时也不例外
     * （`repro_mask5.si`：旧 3 道 `and`、不补时 2 道，blob 长度跟着变）。`same_val_ty` 早退
     * 只属于隐式转换那一侧，所以这里自己发 `Trunc`（同宽 → 只出一道掩码）。
     * 操作数是**常量**时不出 —— 旧 `convert` 先查常量、再查 `same_val_ty`，常量那一路本来
     * 就零条指令（`let v = (U1 ~(U1 0))` 这种就不该变成一个槽）。 */
    if (is_int(*target) && same_val_ty(inner.ty, target) && !pat_const(inner)) {
        return cast_node(HCastKind::Trunc, std::move(inner), target, node.src_line);
    }
    return convert(std::move(inner), target, node.src_line);
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
        /* `Str + Str` = `str_concat`（开栈缓冲 + `__simplexc_copy`，全在 codegen）：HIR 里就是
         * 一条 `Binary{Add, ty=Str}`，两侧已经 `convert` 过（两边都是 Str ⇒ `Raw`，只换标签）。
         * 不折常量：串的常量形态带着串池 id，而拼接结果的长度是运行期才知道的。 */
        HNode n = expr_node(HKind::Binary, ty, line);
        n.bin_op = HBinOp::Add;
        n.kids.push_back(av);
        n.kids.push_back(bv);
        return n;
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
 * 字段按**名字**匹配（与书写顺序无关），未列字段默认 0。
 *
 * `const_ctx` = 从 `cv` 进来（顶层 `var level_input = Input {}` 那种）：字段值只能常量求值。
 * 与 `lower.cc` 的 `in_function` 分支同一个分叉：常量上下文里走 `e()` 会把运行期表达式带进一个
 * 「没有地方放指令」的位置。
 *
 * 书写序在 codegen 那侧是按 kid 的 `span.line` 稳定排序还原的（`col` 前端从不填，见
 * `span.hh`），所以**同一行里的几个字段**还原不出来 —— 那一行内部退回声明序。退回本身没
 * 问题、**只有它和书写序不同时**才有问题：同一行里出现声明序倒置（写了 `b` 再写 `a`）时，
 * codegen 会按 `a, b` 求值，与旧路径静默地不同（字段值带副作用时产物不同）。语料里有 26 行
 * 是「同一行按声明序写」（`Input {a: …, b: …}`），那类退回的结果就是书写序，照旧放行。 */
HNode Analyzer::composite_lit_node(const CompositeLitExpr& node, bool const_ctx) {
    const TyRef ty = ty_of_ref(*node.typ, node.src_line);
    if (ty->kind != TyKind::Struct) {
        unimplemented("复合字面量类型 " + std::string(ty_kind_name(ty->kind)), node.src_line);
    }
    std::vector<std::pair<int, std::size_t>> written; /* (行号, 声明序) 按书写序 */
    written.reserve(node.fields.size());
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
        for (const auto& prev : written) {
            if (prev.first != init->src_line || prev.second < idx) continue;
            unimplemented("复合字面量的字段 " + init->name +
                              " 与同行更早写的字段声明序倒置：同一行内的求值序无法从行号恢复",
                          init->src_line);
        }
        written.emplace_back(init->src_line, idx);
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
        if (const_ctx) {
            std::optional<HNode> raw = const_value(*init->value);
            if (!raw.has_value()) {
                unimplemented("复合字面量的字段 " + init->name + " 不是编译期常量", init->src_line);
            }
            kids[idx] = convert(std::move(*raw), ty->fields[idx].second, init->src_line);
            continue;
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
        /* 同类型也照样发 `Raw`（= 一道掩码）：旧 `convert` 对整数目标**无条件**掩码，codegen 侧
         * 那道 `convert` 已经删掉，这里再早退就会比旧路径少一条 `and`。 */
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

/* ---------------------------------------------------------------- 调用与实例闭包 */

HNode Analyzer::e_call(const CallExpr& node) {
    if (const auto* callee = as<IdentExpr>(node.callee.get())) return call_named(callee->name, node);
    if (const auto* mem = as<MemberExpr>(node.callee.get())) return call_method(*mem, node);
    throw Unsupported("被调者形态 " + std::string(ast_kind_name(node.callee->kind)), node.src_line);
}

/* `lower.cc` 的 `call_named`，**顺序一样**（它决定「同名时变量优先还是 def 优先」）：
 * 顶层 def（重载）→ 词法作用域里的嵌套 def → 程序根体里的嵌套 def（整单元可见、前向可见）→
 * 内建 → 前缀函数 → 报错。 */
HNode Analyzer::call_named(const std::string& name, const CallExpr& node) {
    if (const FunctionDef* d = pick_def(name, node)) return call_user(*d, node);
    if (const Binding* b = lookup(name)) {
        if (b->kind == Binding::Kind::Def) return call_nested(*b->def, node, b->owner_lex);
    }
    auto rd = mod_->root_defs.find(name);
    if (rd != mod_->root_defs.end()) return call_nested(*rd->second, node, "main");
    if (name == "load") return call_load(node);
    if (name == "store") return call_store(node);
    if (name == "raw_cast") return call_raw_cast(node);
    if (name == "asr") return call_asr(node);
    if (name == "clz") return call_clz(node);
    if (name == "ror") return call_ror(node);
    if (name == "min") return call_minmax(HBuiltin::Min, node);
    if (name == "max") return call_minmax(HBuiltin::Max, node);
    if (name == "array") return call_array(node);
    if (name == "random") return call_random(node);
    if (name == "size") return call_size(node);
    if (name == "in_scope" || name == "type_of") {
        throw Unsupported(name + "() 只支持编译期形态", node.src_line);
    }
    if (is_prefix_func(name)) return call_prefix(name, node);
    if (mod_->defs.count(name) != 0) {
        throw Unsupported(name + "() 的重载解析拿不到参数类型", node.src_line);
    }
    throw Unsupported("未定义的函数 " + name, node.src_line);
}

/* `lower.cc` 的 `call_user`。**三步的顺序不能换**：定实例（含泛型绑定）→ 在**调用点**算捕获表 →
 * 递归算被调者的体 → 最后才求实参。顺序一换，`$c<n>` 的编号与 `funcs[]` 的顺序就跟着换。 */
HNode Analyzer::call_user(const FunctionDef& d, const CallExpr& node) {
    const Inst inst = resolve_inst(d.params, node, d.lo);
    const TyRef ret_ty = inst_ret_type(d.ret.get(), inst, d.lo);
    /* 捕获**在调用点**算（原版编译器也是在调用点解析自由名），声明参数只是它的一部分。 */
    const Closure cl = def_captures(d);
    const int index = instantiate_top(d, inst, cl);
    HNode n = expr_node(HKind::Call, ret_ty, node.src_line);
    n.callee.kind = HCalleeKind::Func;
    n.callee.index = index;
    for (std::size_t i = 0; i < inst.rt_types.size(); ++i) {
        n.kids.push_back(convert(e(*node.args[inst.rt_arg[i]]), inst.rt_types[i], node.src_line));
    }
    return n;
}

/* `lower.cc` 的 `call_nested`（顺序同上）。`owner_lex` = 被调 def 的**词法**归属：它声明在哪一层
 * 的体里就写哪一层的词法键 —— 自递归（经 `root_defs` 拿到自己）算 `main`，声在别的函数体里
 * （经作用域里的 `Def` 绑定拿到）算那一层的键。 */
HNode Analyzer::call_nested(const DefStmt& st, const CallExpr& node, const std::string& owner_lex) {
    const Closure cl = captures_of(st, owner_lex);
    const TyRef ret_ty = defstmt_ret(st);
    const std::vector<TyRef> param_tys = defstmt_params(st);
    if (param_tys.size() != node.args.size()) {
        throw Unsupported(st.name + "() 的参数个数对不上", node.src_line);
    }
    const int index = instantiate_nested(st, param_tys, cl, owner_lex);
    HNode n = expr_node(HKind::Call, ret_ty, node.src_line);
    n.callee.kind = HCalleeKind::Func;
    n.callee.index = index;
    for (std::size_t i = 0; i < node.args.size(); ++i) {
        n.kids.push_back(convert(e(*node.args[i]), param_tys[i], node.src_line));
    }
    return n;
}

HNode Analyzer::builtin_node(HBuiltin id, TyRef ty, std::vector<HNode> kids, int line) {
    HNode n = expr_node(HKind::Call, std::move(ty), line);
    n.callee.kind = HCalleeKind::Builtin;
    n.callee.index = static_cast<int>(id);
    n.kids = std::move(kids);
    return n;
}

/* `lower.cc` 的 `builtin_load`：两种形态 —— `load(<T>, p)`（一个实参，类型实参在 `type_args`）
 * 与 `load(type_of(x), p)`（两个实参，类型实参写成类型值表达式）。 */
HNode Analyzer::call_load(const CallExpr& node) {
    if (!node.type_args.empty() && node.args.size() == 1) {
        const TyRef ty = ty_of_ref(*node.type_args[0], node.src_line);
        return builtin_node(HBuiltin::Load, ty, {e(*node.args[0])}, node.src_line);
    }
    if (node.type_args.empty() && node.args.size() == 2) {
        const TyRef ty = type_value_of(*node.args[0], node.src_line);
        return builtin_node(HBuiltin::Load, ty, {e(*node.args[1])}, node.src_line);
    }
    throw Unsupported("load(<T>, p) 的形态", node.src_line);
}

/* `lower.cc` 的 `builtin_store`：结果类型是 `None`（当作 `ExprStmt` 用）。 */
HNode Analyzer::call_store(const CallExpr& node) {
    if (node.args.size() != 2) throw Unsupported("store(p, v) 的形态", node.src_line);
    std::vector<HNode> kids;
    kids.push_back(e(*node.args[0]));
    HNode val = e(*node.args[1]);
    if (val.ty->kind == TyKind::Void) throw Unsupported("store 的值是 None", node.src_line);
    kids.push_back(std::move(val));
    return builtin_node(HBuiltin::Store, ty_void(), std::move(kids), node.src_line);
}

/* `lower.cc` 的 `builtin_raw_cast`：`V::instr(llv(v), ty)` 就是**只换类型标签**，所以 HIR 里
 * 它就是 `Cast{Raw}`（不是内建调用：没有指令要发）。常量直接折成 `Const`。 */
HNode Analyzer::call_raw_cast(const CallExpr& node) {
    if (node.type_args.empty() || node.args.size() != 1) {
        throw Unsupported("raw_cast(<T>, v) 的形态", node.src_line);
    }
    const TyRef ty = ty_of_ref(*node.type_args[0], node.src_line);
    HNode v = e(*node.args[0]);
    auto c = pat_const(v);
    if (c) return const_node(ty, *c & kMask64, node.src_line);
    return cast_node(HCastKind::Raw, std::move(v), ty, node.src_line);
}

/* ---------------------------------------------------------------- 内建（方案 §2f 的 3..9）
 *
 * 七条都逐句照 `lower/lower.cc` 的同名 `builtin_*`：能折的常量在 sema 折好（决策 4：折叠全归
 * sema），折不了的发 `Call{Builtin}`；操作数的形状与那几条 `builtin_*` 取的一致（见
 * `builtins.hh` 的头注释）。语义的细节（`min/max` 的窄侧规则、`clz` 数的是声明位宽、`ror` 两条
 * 路口径不同、`array` 每调用点一枚私有全局、`random` 的顺序递推）一律照现状，不重新推导。
 */

/* `lower.cc` 的 `builtin_asr`：按 W 位符号位算术右移，与 `>>` 不同 —— n ≥ W 时**饱和**到 W-1
 * （`>>` 是按 W 取模）。折叠用 `pat_const`（已按源类型符号性扩过的 64 位内容）。 */
HNode Analyzer::call_asr(const CallExpr& node) {
    if (node.args.size() != 2) unimplemented("asr(x, n) 的形态", node.src_line);
    const HNode x = e(*node.args[0]);
    const HNode n = e(*node.args[1]);
    if (!is_int(*x.ty)) {
        unimplemented(std::string("asr 用在 ") + ty_kind_name(x.ty->kind), node.src_line);
    }
    const int bits = x.ty->bits;
    const auto cx = pat_const(x);
    const auto cn = pat_const(n);
    if (cx && cn) {
        const int64_t amt =
            bits == 64 ? static_cast<int64_t>(*cn & 63)
                       : static_cast<int64_t>(std::min<uint64_t>(*cn, uint64_t(bits - 1)));
        const uint64_t r = static_cast<uint64_t>(as_signed(*cx, bits) >> amt) & mask_of(bits);
        return const_node(x.ty, r, node.src_line);
    }
    /* 移位量先 `convert` 到操作数的类型（`builtin_asr` 同）；掩码与 select 饱和是 codegen 的事。 */
    return builtin_node(HBuiltin::Asr, x.ty, {x, convert(n, x.ty, node.src_line)}, node.src_line);
}

/* `lower.cc` 的 `builtin_clz`：数的是**操作数声明位宽**里的前导零（`clz(U8)` 数是 8 位里的），
 * 而且现状**不折常量**（`ctlz` 交给 codegen）—— 这里也不折，折了就把发指令变成发数字。 */
HNode Analyzer::call_clz(const CallExpr& node) {
    if (node.args.size() != 1) unimplemented("clz(x) 的形态", node.src_line);
    const HNode x = e(*node.args[0]);
    if (!is_int(*x.ty)) {
        unimplemented(std::string("clz 用在 ") + ty_kind_name(x.ty->kind), node.src_line);
    }
    return builtin_node(HBuiltin::Clz, x.ty, {x}, node.src_line);
}

/* `lower.cc` 的 `builtin_ror`：参考的两条路**口径不同**（实测，见那里的注释）——
 *   · 两个操作数都是常量：折的是「按源类型符号性扩成 64 位的值与旋转量按 64 取模」的 64 位旋转；
 *   · 有一个是运行期值：按**操作数位宽**算（旋转量按 W 取模）。
 * 只写一条就会与参考分叉，所以两条各按实测：常量走上面的折叠，运行期发 `Call{Builtin::Ror}`
 * （旋转量**不** `convert`，那一路只看低位）。 */
HNode Analyzer::call_ror(const CallExpr& node) {
    if (node.args.size() != 2) unimplemented("ror(x, n) 的形态", node.src_line);
    const HNode x = e(*node.args[0]);
    const HNode n = e(*node.args[1]);
    if (!is_int(*x.ty)) {
        unimplemented(std::string("ror 用在 ") + ty_kind_name(x.ty->kind), node.src_line);
    }
    if (!is_int(*n.ty)) {
        unimplemented(std::string("ror 的旋转量是 ") + ty_kind_name(n.ty->kind), node.src_line);
    }
    const int bits = x.ty->bits;
    const auto cx = pat_const(x);
    const auto cn = pat_const(n);
    if (cx && cn) {
        const uint64_t amt = *cn & 63;
        const uint64_t r = (*cx >> amt) | (*cx << ((64 - amt) & 63));
        return const_node(x.ty, r & mask_of(bits), node.src_line);
    }
    return builtin_node(HBuiltin::Ror, x.ty, {x, n}, node.src_line);
}

/* `lower.cc` 的 `builtin_minmax`：结果类型由**静态类型** `unify` 出来（拿不到就按 I64），两侧
 * `convert` 过去；常量比按目标类型的符号性，运行期交给 codegen 的 `select`。 */
HNode Analyzer::call_minmax(HBuiltin id, const CallExpr& node) {
    const bool is_min = id == HBuiltin::Min;
    const std::string name = hbuiltin_name(id);
    if (node.args.size() != 2) unimplemented(name + "(a, b) 的形态", node.src_line);
    const TyRef ta = static_type(*node.args[0]);
    const TyRef tb = static_type(*node.args[1]);
    const TyRef ty = unify(ta ? ta : ty_i64(), tb ? tb : ty_i64(), node.src_line);
    const HNode a = convert(e(*node.args[0]), ty, node.src_line);
    const HNode b = convert(e(*node.args[1]), ty, node.src_line);
    const auto ca = pat_const(a);
    const auto cb = pat_const(b);
    if (ca && cb) {
        const int64_t x = ty->is_signed ? as_signed(*ca, ty->bits) : static_cast<int64_t>(*ca);
        const int64_t y = ty->is_signed ? as_signed(*cb, ty->bits) : static_cast<int64_t>(*cb);
        const int64_t pick = is_min ? std::min(x, y) : std::max(x, y);
        return const_node(ty, static_cast<uint64_t>(pick) & mask_of(ty->bits), node.src_line);
    }
    return builtin_node(id, ty, {a, b}, node.src_line);
}

/* `lower.cc` 的 `builtin_array`：`array(<T>, n)` 的值是「指向载荷的指针」，载荷前 8 字节是
 * `n << 16` 的头、载荷零初始化 —— 这些都归 codegen（每调用点一枚私有全局）。sema 只给
 * 「元素类型 + 长度（已折成常量）」：`ty` = `Arr[T]`，kids = `[Const n]`。
 * 长度必须是编译期常量（`builtin_array` 走 `need_const(cv(…))` 同一条口径）。 */
HNode Analyzer::call_array(const CallExpr& node) {
    if (node.type_args.size() != 1 || node.args.size() != 1) {
        unimplemented("array(<T>, n) 的形态", node.src_line);
    }
    const TyRef elem = ty_of_ref(*node.type_args[0], node.src_line);
    const std::optional<HNode> n = const_value(*node.args[0]);
    if (!n.has_value() || n->kind != HKind::Const) {
        unimplemented("array(<T>, n) 的长度不是编译期常量", node.src_line);
    }
    if (n->const_bits > (uint64_t{1} << 32)) {
        unimplemented("array 的长度 " + std::to_string(n->const_bits) + " 太大", node.src_line);
    }
    return builtin_node(HBuiltin::Array, make_array(elem),
                        {const_node(ty_i64(), n->const_bits, node.src_line)}, node.src_line);
}

/* `lower.cc` 的 `builtin_random`：状态就是前缀的 `var global_seed = Seed 1`（语料自己会写
 * `global_seed = Seed (…)`，所以两边必须是**同一份**全局），递推是四步顺序赋值，全在 codegen。
 * 种子的全局**在这里按需建**（`prefix_global`，与现状「第一次引用才进 `globals`」同），并作为
 * 第一枚操作数带下去 —— HIR 的引用一律是已解析的编号。 */
HNode Analyzer::call_random(const CallExpr& node) {
    if (node.args.size() > 1) unimplemented("random 的参数个数", node.src_line);
    const GlobalRef seed = prefix_global("global_seed", node.src_line);
    std::vector<HNode> kids;
    kids.push_back(global_node(seed.id, seed.ty, node.src_line));
    if (!node.args.empty()) {
        /* `random(n) = U64(state) % U64(n)`：取模口径照 `div`，全在 codegen。 */
        kids.push_back(convert(e(*node.args[0]), seed.ty, node.src_line));
    }
    return builtin_node(HBuiltin::Random, seed.ty, std::move(kids), node.src_line);
}

/* `lower.cc` 的 `call_method`：方法糖在 sema 消解，**HIR 里不留方法名**。现状只认 `.len()` 与
 * `.ms()`（98 份语料里 212 / 294 处；其余方法名一处也没有，`call_method` 本来也报错）——
 * 所以其余名字继续显式报未实现。
 *
 * 两个都落到内建 10/11，不拆成已有节点：拆 `.len()` 会把 `gep -8` 换成 inttoptr/ptrtoint
 * 算术，IR 形状就变了；「产物逐字节相同」是主判据，不能靠 O2 折叠去赌（方案 §2f 追加段）。
 * 接收者先求值、再按名字分派 —— 与 `call_method` 同（实参它不看，这里也不看）。 */
HNode Analyzer::call_method(const MemberExpr& callee, const CallExpr& node) {
    const HNode obj = e(*callee.obj);
    if (callee.name == "len") {
        if (obj.ty->kind != TyKind::Str && obj.ty->kind != TyKind::Array) {
            unimplemented(".len() 用在 " + std::string(ty_kind_name(obj.ty->kind)), node.src_line);
        }
        return builtin_node(HBuiltin::Len, ty_i64(), {obj}, node.src_line);
    }
    if (callee.name == "ms") {
        if (!is_int(*obj.ty)) {
            unimplemented(".ms() 用在 " + std::string(ty_kind_name(obj.ty->kind)), node.src_line);
        }
        /* 折不折常量是 codegen 的事（`lower` 在常量时折的是 `pat_const` 那个按源类型符号性
         * 扩过的 64 位值，折法只该有一处）。 */
        return builtin_node(HBuiltin::Ms, ty_time(), {obj}, node.src_line);
    }
    unimplemented("方法 ." + callee.name + "()", node.src_line);
}

/* 前缀函数的固定签名（方案 §2f；逐个照 `lower.cc` 的 `call_prefix` + `ensure_prefix_fn`）。 * 参数名取 `stdlib_prefix.si` 里那几个 `def` 自己写的名字 —— HIR 的形参必须带名字，而".
 * 编不编译 body 是 codegen 看的 `pre_body` 决定的，名字只为了 dump 能读。
 * 为什么不动 body：现状这五个的体是手写的 IR（`body_get_time` …），顺序与外部符号（
 * `GetSystemTimePreciseAsFileTime`）都是实测出来的；在 sema 里重新展开一次只会分叉。 */
struct PrefixFunc {
    HPreBody tag = HPreBody::None;
    TyRef ret;
    std::vector<std::pair<std::string, TyRef>> params;
};

const std::map<std::string, PrefixFunc>& prefix_funcs() {
    static const std::map<std::string, PrefixFunc> m{
        {"get_time", {HPreBody::GetTime, ty_time(), {}}},
        {"sleep", {HPreBody::Sleep, ty_void(), {{"duration", ty_time()}}}},
        {"memory_clear",
         {HPreBody::MemoryClear, ty_void(), {{"source", ty_ptr()}, {"length", ty_i64()}}}},
        {"memory_copy",
         {HPreBody::MemoryCopy,
          ty_void(),
          {{"source", ty_ptr()}, {"destination", ty_ptr()}, {"length", ty_i64()}}}},
        {"thread_exit", {HPreBody::ThreadExit, ty_void(), {}}},
    };
    return m;
}

/* `lower.cc` 的 `call_prefix`：五个名字走固定的签名，实参在这里就 `convert` 到声明的形参类型。
 * 与内建的区别就在 HIR 里：它是一条普通的 `Call{FnId}`，被调那个实例带 `pre_body`、body 是空
 * `Block`（codegen 见到非 `None` 就不编译 body）。
 * 实例按名字缓存（`lower.cc` 的 `fns_` 也是按名字缓存）：同一份前缀函数只发一个实例。 */
HNode Analyzer::call_prefix(const std::string& name, const CallExpr& node) {
    const auto sig = prefix_funcs().find(name);
    if (sig == prefix_funcs().end()) unimplemented("前缀函数 " + name, node.src_line);
    const PrefixFunc& fn_sig = sig->second;
    if (node.args.size() != fn_sig.params.size()) {
        unimplemented(name + "(…) 的参数个数", node.src_line);
    }
    /* 实参按源码顺序求值 + convert（与 `call_prefix` 的 emit 顺序一致）。 */
    std::vector<HNode> args;
    for (std::size_t i = 0; i < fn_sig.params.size(); ++i) {
        args.push_back(convert(e(*node.args[i]), fn_sig.params[i].second, node.src_line));
    }
    int index = -1;
    const auto hit = mod_->instance.find(name);
    if (hit != mod_->instance.end()) {
        index = hit->second;
    } else {
        HirFunc fn;
        fn.key = name;
        fn.name = name;
        fn.pre_body = fn_sig.tag;
        fn.ret = fn_sig.ret;
        for (const auto& p : fn_sig.params) fn.params.push_back(HParam{p.first, p.second});
        /* 空块：带 span（不变量 ⑤），但没有语句 —— codegen 根本不会走到它。 */
        fn.body = stmt_node(HKind::Block, node.src_line);
        index = static_cast<int>(mod_->hir.funcs.size());
        mod_->hir.funcs.push_back(std::move(fn));
        mod_->instance[name] = index;
    }
    HNode n = expr_node(HKind::Call, fn_sig.ret, node.src_line);
    n.callee.kind = HCalleeKind::Func;
    n.callee.index = index;
    n.kids = std::move(args);
    return n;
}

/* `lower.cc` 的 `builtin_size`：`size(x)` = 操作数**声明类型**的位宽，编译期常数（单位位）。 */
HNode Analyzer::call_size(const CallExpr& node) {
    if (node.args.size() != 1) throw Unsupported("size(x) 的形态", node.src_line);
    const TyRef ty = operand_type(*node.args[0], node.src_line);
    return const_node(ty_i64(), static_cast<uint64_t>(size_bits(*ty)), node.src_line);
}

/* 体内的嵌套 `def` 只**登记**，不生成：真被调用时才发（`lower.cc` 的 `s_DefStmt`）。
 * HIR 里没有节点（§2b：`def` 声明属于语法糖，已投影掉）。 */
void Analyzer::def_stmt(HNode& /*parent*/, const DefStmt& st) {
    bind_def(st.name, &st, cur_key_);
}

/* `lower.cc` 的 `pick_def`：同名重载按「参数类型对得上」挑本文件 def，按声明序取第一个。 */
const FunctionDef* Analyzer::pick_def(const std::string& name, const CallExpr& node) {
    auto it = mod_->defs.find(name);
    if (it == mod_->defs.end()) return nullptr;
    for (const FunctionDef* d : it->second) {
        if (has_type_params(d->params)) {
            /* 泛型候选：先定实例化（类型实参 / 推型），再只比**运行期**形参。定不下来就跳过
             * 这个候选 —— 不能把异常泄出去，否则同一名字的其它重载都再也选不到。 */
            Inst inst;
            try {
                inst = resolve_inst(d->params, node, d->lo);
            } catch (const Unsupported&) {
                continue;
            } catch (const CompileError& e) {
                if (e.resp == Responsibility::OurBug) throw;
                continue;
            }
            bool ok = true;
            for (std::size_t i = 0; i < inst.rt_types.size(); ++i) {
                if (!compatible(static_type(*node.args[inst.rt_arg[i]]), inst.rt_types[i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return d;
            continue;
        }
        auto types = decl_param_types(*d);
        if (!types || types->size() != node.args.size()) continue;
        bool ok = true;
        for (std::size_t i = 0; i < types->size(); ++i) {
            if (!compatible(static_type(*node.args[i]), (*types)[i])) {
                ok = false;
                break;
            }
        }
        if (ok) return d;
    }
    return nullptr;
}

/* `lower.cc` 的 `resolve_inst`，逐句照搬（含「`<U8>` 不占运行期槽」与 `#v` 后缀）。
 * 区别只有一处：签名那段要读 `params[i]->typ`，缺类型时先报错而不是解引用空指针。 */
Inst Analyzer::resolve_inst(const std::vector<std::unique_ptr<Param>>& params, const CallExpr& node,
                            int line) {
    Inst out;
    std::vector<std::size_t> tp; /* 类型参数的声明位置（按声明序） */
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i]->typ != nullptr && is_type_param(*params[i]->typ)) tp.push_back(i);
    }
    if (tp.empty()) {
        if (node.args.size() != params.size() || !node.type_args.empty()) {
            throw Unsupported("实参个数对不上", line);
        }
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (params[i]->typ == nullptr) {
                throw Unsupported("参数 " + params[i]->name + " 缺类型", line);
            }
            out.rt_params.push_back(params[i].get());
            out.rt_types.push_back(ty_of_ref(*params[i]->typ, line));
            out.rt_arg.push_back(i);
        }
        return out;
    }

    /* 写法一（类型实参，不占运行期槽）：`g(a, <U8>)` —— 实参个数 = 声明参数 - 类型参数个数。
     * 写法二（值实参，推型）：`g(load(<U8>, p))` —— 实参个数 = 声明参数个数。 */
    const bool by_value = node.args.size() == params.size();
    if (!by_value && node.args.size() + tp.size() != params.size()) {
        throw Unsupported("泛型实参个数对不上", line);
    }
    if (node.type_args.size() > tp.size()) throw Unsupported("类型实参个数超过类型参数", line);
    if (tp.size() > 1 && !node.type_args.empty() && node.type_args.size() != tp.size()) {
        throw Unsupported("类型实参个数少于类型参数", line);
    }

    std::size_t ti = 0; /* 下一个待定类型的类型参数（声明序） */
    for (std::size_t i = 0; i < params.size(); ++i) {
        const bool is_tp = params[i]->typ != nullptr && is_type_param(*params[i]->typ);
        if (!is_tp) {
            if (params[i]->typ == nullptr) {
                throw Unsupported("参数 " + params[i]->name + " 缺类型", line);
            }
            out.rt_params.push_back(params[i].get());
            out.rt_types.push_back(ty_of_ref(*params[i]->typ, line));
            out.rt_arg.push_back(by_value ? i : i - (ti > tp.size() ? tp.size() : ti));
            continue;
        }
        TyRef bound = nullptr;
        if (ti < node.type_args.size()) {
            bound = ty_of_ref(*node.type_args[ti], line);
        } else if (by_value) {
            bound = static_type(*node.args[i]);
            if (!bound) throw Unsupported("推不出类型 " + params[i]->name, line);
        } else {
            throw Unsupported("类型实参没给 " + params[i]->name, line);
        }
        out.bind_order.push_back(bound);
        out.binds[params[i]->name] = bound;
        ++ti;
        if (by_value) {
            out.rt_params.push_back(params[i].get());
            out.rt_types.push_back(bound);
            out.rt_arg.push_back(i);
        }
    }
    /* 同一 (函数, 实参) 只实例化一次的判据就是这个串（接口 §1）：缓存键与函数名的 `$` 后缀共用。 */
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i != 0) out.sig += ",";
        if (params[i]->typ == nullptr) {
            throw Unsupported("参数 " + params[i]->name + " 缺类型", line);
        }
        if (is_type_param(*params[i]->typ)) {
            out.sig += "@" + ty_key(*out.binds.at(params[i]->name));
        } else {
            out.sig += ty_key(*ty_of_ref(*params[i]->typ, line));
        }
    }
    if (by_value) out.sig += "#v";
    return out;
}

std::optional<std::vector<TyRef>> Analyzer::decl_param_types(const FunctionDef& d) {
    std::vector<TyRef> out;
    for (const auto& p : d.params) {
        if (p->typ == nullptr) return std::nullopt;         /* 重载解析里这会跳过这个候选 */
        if (is_type_param(*p->typ)) return std::nullopt;    /* 泛型走 `resolve_inst` */
        out.push_back(ty_of_ref(*p->typ, d.lo));
    }
    return out;
}

/* 返回类型写成类型参数（`@Size` / `@Type`）时取**第一个**类型参数的绑定（`lower.cc` 同）。 */
TyRef Analyzer::inst_ret_type(const TypeRef* ret, const Inst& inst, int line) {
    if (ret == nullptr) return ty_void();
    if (!is_type_param(*ret)) return ty_of_ref(*ret, line);
    if (inst.bind_order.empty()) throw Unsupported("返回类型 " + ret->name + " 没有绑定", line);
    return inst.bind_order.front();
}

TyRef Analyzer::defstmt_ret(const DefStmt& st) {
    return st.ret != nullptr ? ty_of_ref(*st.ret, st.src_line) : ty_void();
}

std::vector<TyRef> Analyzer::defstmt_params(const DefStmt& st) {
    std::vector<TyRef> out;
    for (const auto& p : st.params) {
        if (p->typ == nullptr) throw Unsupported("参数 " + p->name + " 缺类型", p->src_line);
        out.push_back(ty_of_ref(*p->typ, p->src_line));
    }
    return out;
}

/* `lower.cc` 的 `static_type`：只用于**重载粗判**与推型，拿不到就返回 `nullptr`（不猜）。
 * `CompileError` 也当「拿不到」（`OurBug` 不吞：内部不自洽不能伪装成「推不出类型」）。 */
TyRef Analyzer::static_type(const Node& node) {
    try {
        switch (node.kind) {
            case NK::Literal: {
                const auto& n = static_cast<const LiteralExpr&>(node);
                if (n.lit_kind == "int") return ty_i64();
                if (n.lit_kind == "str" || n.lit_kind == "template") return ty_str();
                return nullptr;
            }
            case NK::Ident: {
                const auto& n = static_cast<const IdentExpr&>(node);
                /* `true`/`false` 不是绑定（只在 `e_ident` 里特判）：少了这两行，
                 * `ui_set_hidden("x", false)` 这种实参的重载粗判拿不到 Bool → 无候选。 */
                if (n.name == "true" || n.name == "false") return ty_bool();
                if (const Binding* b = lookup(n.name)) {
                    if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
                        return b->ty;
                    }
                    if (b->kind == Binding::Kind::Const) return b->ty;
                    return nullptr;
                }
                auto g = mod_->globals.find(n.name);
                if (g != mod_->globals.end()) return g->second.ty;
                auto pg = prefix_globals().find(n.name);
                if (pg != prefix_globals().end()) return pg->second.ty;
                auto em = mod_->enum_member.find(n.name);
                if (em != mod_->enum_member.end()) return mod_->enums.at(em->second.first);
                auto cst = mod_->consts.find(n.name);
                if (cst != mod_->consts.end()) return cst->second.ty;
                auto hc = hash_consts().find(n.name);
                if (hc != hash_consts().end()) return hc->second.ty;
                return nullptr;
            }
            case NK::Cast: {
                const auto& n = static_cast<const CastExpr&>(node);
                return ty_of_ref(*n.target, n.src_line);
            }
            case NK::Unary: {
                const auto& n = static_cast<const UnaryExpr&>(node);
                return n.op == "!" ? ty_bool() : static_type(*n.expr);
            }
            case NK::Binary: {
                const auto& n = static_cast<const BinaryExpr&>(node);
                if (is_cmp(n.op) || n.op == "&&" || n.op == "||") return ty_bool();
                const TyRef a = static_type(*n.left);
                const TyRef b = static_type(*n.right);
                if (a && b) return unify(a, b, n.src_line);
                return nullptr;
            }
            case NK::Call: {
                const auto& n = static_cast<const CallExpr&>(node);
                if (const auto* mem = as<MemberExpr>(n.callee.get())) {
                    if (mem->name == "len") return ty_i64();
                    if (mem->name == "ms") return ty_time();
                    return nullptr;
                }
                const auto* id = as<IdentExpr>(n.callee.get());
                if (id == nullptr) return nullptr;
                const std::string& name = id->name;
                if ((name == "load" || name == "raw_cast") && !n.type_args.empty()) {
                    return ty_of_ref(*n.type_args[0], n.src_line);
                }
                /* `load(type_of(x), p)`：类型实参写成类型值表达式时 `type_args` 是空的。 */
                if (name == "load" && n.type_args.empty() && n.args.size() == 2) {
                    try {
                        return type_value_of(*n.args[0], n.src_line);
                    } catch (const Unsupported&) {
                        return nullptr;
                    }
                }
                /* `array(<T>, n)`：`var a = array(<U8>, 16)` 的静态类型决定 `a[i]` 的步长。 */
                if (name == "array" && !n.type_args.empty()) {
                    return make_array(ty_of_ref(*n.type_args[0], n.src_line));
                }
                if ((name == "min" || name == "max" || name == "asr" || name == "clz") &&
                    !n.args.empty()) {
                    return static_type(*n.args[0]);
                }
                if (name == "sleep" || name == "memory_clear" || name == "memory_copy" ||
                    name == "thread_exit") {
                    return ty_void();
                }
                if (name == "get_time") return ty_time();
                if (name == "size") return ty_i64();
                if (const FunctionDef* d = pick_def(name, n)) {
                    const Inst inst = resolve_inst(d->params, n, d->lo);
                    return inst_ret_type(d->ret.get(), inst, d->lo);
                }
                if (const Binding* b = lookup(name)) {
                    if (b->kind == Binding::Kind::Def) return defstmt_ret(*b->def);
                }
                auto rd = mod_->root_defs.find(name);
                if (rd != mod_->root_defs.end()) return defstmt_ret(*rd->second);
                return nullptr;
            }
            case NK::Member: {
                const auto& n = static_cast<const MemberExpr&>(node);
                const TyRef obj = static_type(*n.obj);
                if (obj && obj->kind == TyKind::Struct) {
                    auto st = mod_->structs.find(obj->name);
                    if (st != mod_->structs.end()) {
                        for (const HirField& f : st->second.fields) {
                            if (f.name == n.name) return f.ty;
                        }
                    }
                }
                return nullptr;
            }
            case NK::Index: {
                const auto& n = static_cast<const IndexExpr&>(node);
                const TyRef obj = static_type(*n.obj);
                if (obj && (obj->kind == TyKind::Array || obj->kind == TyKind::Str)) return obj->elem;
                return nullptr;
            }
            case NK::CompositeLit: {
                const auto& n = static_cast<const CompositeLitExpr&>(node);
                return ty_of_ref(*n.typ, n.src_line);
            }
            case NK::Template: return ty_str();
            default: return nullptr;
        }
    } catch (const Unsupported&) {
        return nullptr;
    } catch (const CompileError& e) {
        if (e.resp == Responsibility::OurBug) throw;
        return nullptr;
    }
}

/* 类型值只出现在类型实参位置，语料里唯一形态是 `load(type_of(x), p)`。 */
TyRef Analyzer::type_value_of(const Expr& node, int line) {
    const auto* call = as<CallExpr>(&node);
    if (call == nullptr) throw Unsupported("类型值形态", line);
    const auto* id = as<IdentExpr>(call->callee.get());
    if (id == nullptr || id->name != "type_of" || call->args.size() != 1) {
        throw Unsupported("类型值形态", line);
    }
    return operand_type(*call->args[0], line);
}

/* 类型参数不占运行期槽（`g(a, <U8>)` 那种写法），所以它在作用域里**没有**变量绑定 ——
 * 名字 → 具体类型只能从本实例的类型环境（`ty_binds_`）拿。 */
TyRef Analyzer::operand_type(const Expr& node, int line) {
    const auto* id = as<IdentExpr>(&node);
    if (id != nullptr) {
        auto it = ty_binds_.find(id->name);
        if (it != ty_binds_.end()) return it->second;
    }
    const TyRef ty = static_type(node);
    if (!ty) throw Unsupported("拿不到类型", line);
    return ty;
}

/* 实例键里 `$` 后面那一段。泛型 def 用 `resolve_inst` 算好的签名；**非泛型 def 的 `inst.sig`
 * 恒为空串**（`resolve_inst` 在无类型参数时提前返回），只按名字做键会把同名重载折成一个实例
 * —— 语料里 `get_component_count()` 与 `get_component_count(t)` 就是这种名字。所以重载集里补上
 * 声明参数类型：键的格式不变（仍是 `<名字>$<实参串>`），挑中的那一个重载的形参类型就是实参类型。
 * 声明参数类型完全相同的两个同名 def 仍会折成一个 —— 静态解析也只会挑到第一个（`pick_def`）。 */
std::string Analyzer::inst_sig(const FunctionDef& d, const Inst& inst) {
    if (!inst.sig.empty()) return inst.sig;
    const auto set = mod_->defs.find(d.name);
    if (set == mod_->defs.end() || set->second.size() < 2) return std::string();
    std::string out;
    for (std::size_t i = 0; i < d.params.size(); ++i) {
        if (i != 0) out += ",";
        if (d.params[i]->typ == nullptr) {
            throw Unsupported("参数 " + d.params[i]->name + " 缺类型", d.lo);
        }
        out += ty_key(*ty_of_ref(*d.params[i]->typ, d.lo));
    }
    return out;
}

int Analyzer::instantiate_top(const FunctionDef& d, const Inst& inst, const Closure& cl) {
    const std::string cap_sig = capture_sig(cl.caps);
    const std::string sig = inst_sig(d, inst);
    std::string key = d.name;
    if (!sig.empty()) key += "$" + sig;
    if (!cap_sig.empty()) key += "|" + cap_sig;
    auto hit = mod_->instance.find(key);
    if (hit != mod_->instance.end()) return hit->second;

    int cap_index = -1;
    if (!cap_sig.empty()) {
        int& seen = mod_->cap_instances[{d.name, d.lo}];
        /* 上限照搬现状（`lower.cc` 的 64）：互调时按捕获签名各发一份实例，理论上有限、代价可能
         * 很长。报错回退到参考产物，比在这里无界发函数安全。
         * 注：它拦的不只是真发散 —— 捕获签名按 (名字, 类型) 区分，所以同一个 def 在 65 个不同
         * 类型的同名局部上被调用也会到这里。 */
        if (seen >= 64) {
            throw CompileError(Responsibility::Unimplemented, Stage::Sema, span_at(d.lo),
                               d.name + " 的闭包实例数不收敛（" + cap_sig + "）");
        }
        cap_index = seen++;
    }

    HirFunc fn;
    fn.key = key;
    /* `name` 只给纯 def 名：`$<sig>` / `$c<n>` / `#L<行>` / `tc_main` 都是 codegen 派生的
     * 产物符号名（方案 §2b 补漏 1），sema 只交出拼它们要用的三个事实
     * （`inst_sig` / `cap_index` / `decl_line` / `line_suffix`）。 */
    fn.name = d.name;
    fn.inst_sig = inst.sig;
    fn.cap_index = cap_index;
    fn.decl_line = d.lo;
    /* 旧 `lower.cc`：同名顶层 def 多于一个时产物名挂 `#L<行>`，否则就是裸名字。 */
    const auto same = mod_->defs.find(d.name);
    fn.line_suffix = same != mod_->defs.end() && same->second.size() > 1;
    fn.ret = inst_ret_type(d.ret.get(), inst, d.lo);
    for (std::size_t i = 0; i < inst.rt_types.size(); ++i) {
        fn.params.push_back(HParam{inst.rt_params[i]->name, inst.rt_types[i]});
    }
    fn.caps = cl.caps;
    const int index = static_cast<int>(mod_->hir.funcs.size());
    mod_->hir.funcs.push_back(std::move(fn));
    /* **登记之后**才分析它的体：自引用/互调回头再调这个键时直接复用下标（不收敛的只有
     * 捕获签名一直在变的链路，那条由上面 64 上限收住）。 */
    mod_->instance[key] = index;
    Analyzer child(*mod_);
    child.run_top_def(d, inst, cl.caps, index);
    return index;
}

int Analyzer::instantiate_nested(const DefStmt& st, const std::vector<TyRef>& param_tys,
                                 const Closure& cl, const std::string& owner_lex) {
    /* 嵌套 def 的键 = **词法**归属 + def 名 + 源码行 + 捕获签名。前三样只由声明位置决定：
     * 换成「调用它的那个实例的键」时，自递归每绕一圈键就长一段、实例无界增长（实测栈溢出），
     * 而词法键让递归与互调都收敛到同一个实例。
     * 抓获签名跟着键走：同一个嵌套 def 在两个捕获面不同的调用点发两份实例（现状的键里没有捕获面，
     * 会复用第一个实例 —— 第二次调用传进去的指针个数对不上，那是待爆的形态，不复现）。 */
    const std::string lex = lex_of(owner_lex, st);
    const std::string cap_sig = capture_sig(cl.caps);
    std::string key = lex;
    if (!cap_sig.empty()) key += "|" + cap_sig;
    auto hit = mod_->instance.find(key);
    if (hit != mod_->instance.end()) return hit->second;
    /* 与顶层实例同一道上限：词法键已经让正当的递归收敛，能撞上这条的只有捕获签名一直在变的
     * 链路 —— 报错（回退到参考产物）比在这里无界发函数安全。 */
    if (!cap_sig.empty() && mod_->cap_instances[{st.name, st.src_line}] >= 64) {
        throw CompileError(Responsibility::Unimplemented, Stage::Sema, span_at(st.src_line),
                           st.name + " 的闭包实例数不收敛（" + cap_sig + "）");
    }

    HirFunc fn;
    fn.key = key;
    /* `name` 只给 def 自己的名字：`owner + "$" + name + "#L<line>"` 是 codegen 按 §2b 补漏 1
     * 派的符号名（`owner` 还取决于调用点），这里再拼一遍就把 owner 前缀叠两次了。 */
    fn.name = st.name;
    fn.decl_line = st.src_line;
    fn.ret = defstmt_ret(st);
    for (std::size_t i = 0; i < st.params.size() && i < param_tys.size(); ++i) {
        fn.params.push_back(HParam{st.params[i]->name, param_tys[i]});
    }
    fn.caps = cl.caps;
    const int index = static_cast<int>(mod_->hir.funcs.size());
    mod_->hir.funcs.push_back(std::move(fn));
    mod_->instance[key] = index;
    if (!cap_sig.empty()) ++mod_->cap_instances[{st.name, st.src_line}];
    Analyzer child(*mod_);
    child.run_nested_def(st, param_tys, cl, lex, index);
    return index;
}

/* 子上下文：一个顶层 def 实例的体。作用域 = 形参 → 捕获（形参同名的不再捕），类型环境 = 本实例
 * 的泛型绑定。`lower.cc` 的 `ensure_def_fn` 逐句对应。 */
void Analyzer::run_top_def(const FunctionDef& d, const Inst& inst,
                           const std::vector<HCapture>& caps, int index) {
    ret_ = inst_ret_type(d.ret.get(), inst, d.lo);
    /* 词法键只取 def 名（不含重载区分、也不含实例序号）：体里嵌套 def 的键靠它 + 名字@行 定位，
     * 同一个 def 的不同实例必须给出同一个前缀。 */
    cur_key_ = d.name;
    ty_binds_ = inst.binds;
    push_scope();
    for (std::size_t i = 0; i < inst.rt_types.size(); ++i) {
        bind_local(inst.rt_params[i]->name, inst.rt_types[i]);
    }
    for (const HCapture& c : caps) {
        bool is_param = false;
        for (const auto& p : d.params) is_param = is_param || p->name == c.name;
        if (is_param) continue;
        bind_capture(c.name, c.ty, c.index);
    }
    HNode body = block(*d.body);
    pop_scope();
    mod_->hir.funcs[static_cast<std::size_t>(index)].body = std::move(body);
}

/* 子上下文：一个嵌套 def 实例的体。先绑被摊平进来的兄弟 def，再绑形参与捕获 —— 同名时变量优先
 * （调用仍能按名查到 def，`call_named` 会往 `root_defs` 再找一遍）。 */
void Analyzer::run_nested_def(const DefStmt& st, const std::vector<TyRef>& param_tys,
                              const Closure& cl, const std::string& lex, int index) {
    ret_ = defstmt_ret(st);
    cur_key_ = lex;
    push_scope();
    for (const auto& d : cl.defs) bind_def(d.first->name, d.first, d.second);
    for (std::size_t i = 0; i < st.params.size() && i < param_tys.size(); ++i) {
        bind_local(st.params[i]->name, param_tys[i]);
    }
    for (const HCapture& c : cl.caps) {
        bool is_param = false;
        for (const auto& p : st.params) is_param = is_param || p->name == c.name;
        if (is_param) continue;
        bind_capture(c.name, c.ty, c.index);
    }
    HNode body = block(*st.body);
    pop_scope();
    mod_->hir.funcs[static_cast<std::size_t>(index)].body = std::move(body);
}

Closure Analyzer::captures_of(const DefStmt& st, const std::string& owner_lex) {
    Closure out;
    std::set<const DefStmt*> visiting;
    visiting.insert(&st); /* 兄弟 def 互调会绕回来，先把自己放进环里 */
    std::vector<std::string> params;
    for (const auto& p : st.params) params.push_back(p->name);
    collect_captures(*st.body, params, false, out, visiting, lex_of(owner_lex, st));
    return out;
}

/* 顶层 def 的捕获面：算的是「它（连同它体里按名调用的兄弟 def）真正用到、而调用点绑在栈槽/捕获
 * 上的名字」。一个 def 的自由名全落在模块级（全局 / const / 枚举 / 前缀 / 其它 def）时结果是空的
 * ⇒ 签名与应用面都不变。`into_own_defs`：体里**自己声明**的嵌套 def 也要往下摊平（110：
 * root → `mode_run` → `handle_test_result` → `get_input_switched` 的 `seen`）。 */
Closure Analyzer::def_captures(const FunctionDef& d) {
    Closure out;
    std::set<const DefStmt*> visiting;
    std::vector<std::string> params;
    for (const auto& p : d.params) params.push_back(p->name);
    /* 归属段用 def 名，`run_top_def` 的 `cur_key_` 必须是同一个串。 */
    collect_captures(*d.body, params, true, out, visiting, d.name);
    return out;
}

/* `lower.cc` 的 `collect_captures` 逐句照搬（连两遍遍历的形状一起）。
 *
 * 嵌套 def 体里引用了哪些**外层局部**（按引用捕获）：自己的参数、`var`/`let`/内层 `def` 都算
 * 「自己的」，同名遮蔽外层 —— 不往内层 `DefStmt` 的体里钻：内层 def 真被调用时再算它自己的捕获，
 * 那时本函数的参数/局部/捕获都已经在作用域里。
 * 但**兄弟 def**（本函数体里按名调用、定义在外面的嵌套 def）必须在这里就摊平：原版编译器按
 * **最外层调用点**解析 free variable，而我们把它生成成一个真函数，它的捕获只能由**调用它的
 * 那一层**带进来（006：`handle_test_result` 调 `halt`，`halt` 用 `run_sim` 体的
 * `burst_target_cycle` —— 后者得先被 `handle_test_result` 捕获、再原样传给 `halt`）。
 *
 * 为什么用的是**本 analyzer 的** `lookup`：`lookup` 看到的是调用点的作用域 —— 这正是原版
 * 「自由名在调用点解析」的那条实测（`docs/facts/semantics.md` 的「嵌套 def 的自由变量按调用点
 * 解析」）。所以在调用点的 analyzer 上调它，不要挪到被调者的上下文里。 */
void Analyzer::collect_captures(const Block& body, const std::vector<std::string>& params,
                                bool into_own_defs, Closure& out,
                                std::set<const DefStmt*>& visiting, const std::string& owner_lex) {
    std::set<std::string> own(params.begin(), params.end());
    std::map<std::string, const DefStmt*> own_defs;

    std::function<void(const Node&)> collect = [&](const Node& n) {
        if (n.kind == NK::VarDecl) own.insert(static_cast<const VarDeclStmt&>(n).name);
        if (n.kind == NK::Def) {
            const auto& d = static_cast<const DefStmt&>(n);
            own.insert(d.name);
            if (into_own_defs) own_defs.emplace(d.name, &d);
        }
        for_each_child(n, collect);
    };
    collect(body);

    std::function<void(const Node&)> rec = [&](const Node& n) {
        if (n.kind == NK::Def) return; /* 内层 def 有自己的作用域 */
        if (n.kind == NK::Ident) {
            const auto& id = static_cast<const IdentExpr&>(n);
            const DefStmt* target = nullptr;
            std::string target_owner;
            if (own.count(id.name) == 0) {
                if (const Binding* b = lookup(id.name)) {
                    if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
                        bool known = false;
                        for (const auto& c : out.caps) known = known || c.name == id.name;
                        if (!known) {
                            out.caps.push_back(HCapture{id.name, b->ty,
                                                        static_cast<int>(out.caps.size())});
                        }
                        return;
                    }
                    if (b->kind == Binding::Kind::Def) {
                        target = b->def;
                        target_owner = b->owner_lex;
                    }
                } else {
                    /* 程序根语句树里的嵌套 def 不在词法作用域里（`call_named` 走 `root_defs`
                     * 那条路），这里用同一张表认名字，否则它捕获的外层变量传不进去。 */
                    auto rd = mod_->root_defs.find(id.name);
                    if (rd != mod_->root_defs.end()) {
                        target = rd->second;
                        target_owner = "main";
                    }
                }
            }
            if (target == nullptr && into_own_defs) {
                /* 本函数体里自己声明的嵌套 def：归属就是本层。 */
                auto od = own_defs.find(id.name);
                if (od != own_defs.end()) {
                    target = od->second;
                    target_owner = owner_lex;
                }
            }
            if (target != nullptr && visiting.insert(target).second) {
                bool known = false;
                for (const auto& d : out.defs) known = known || d.first == target;
                if (!known) out.defs.emplace_back(target, target_owner);
                std::vector<std::string> inner_params;
                for (const auto& p : target->params) inner_params.push_back(p->name);
                collect_captures(*target->body, inner_params, into_own_defs, out, visiting,
                                 lex_of(target_owner, *target));
            }
        }
        for_each_child(n, rec);
    };
    rec(body);
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
                if (b->kind == Binding::Kind::Const) {
                    return const_ref(b->ty, b->bits, b->const_id, n.src_line);
                }
            }
            auto c = mod_->consts.find(n.name);
            if (c != mod_->consts.end()) {
                return const_ref(c->second.ty, c->second.const_bits, c->second.const_id, n.src_line);
            }
            auto em = mod_->enum_member.find(n.name);
            if (em != mod_->enum_member.end()) {
                return const_node(mod_->enums.at(em->second.first),
                                  static_cast<uint64_t>(em->second.second), n.src_line);
            }
            auto hc = hash_consts().find(n.name);
            if (hc != hash_consts().end()) {
                return const_node(hc->second.ty, hc->second.value, n.src_line);
            }
            auto rc = mod_->root_consts.find(n.name);
            if (rc != mod_->root_consts.end()) return const_decl_value(*rc->second);
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
        case NK::ArrayLit: return array_lit_node(static_cast<const ArrayLitExpr&>(node));
        case NK::CompositeLit:
            /* 顶层 `var level_input = Input {}`：聚合的**常量**形态（`lower.cc` 的 `cv` 也直接
             * 转 `composite_literal`，那里靠 `in_function` 分叉）。 */
            return composite_lit_node(static_cast<const CompositeLitExpr&>(node), true);
        case NK::Call: {
            /* 顶层 var 的初值在语料里就是 `raw_cast(<Arr[U64]>, <常量地址>)`：它是**编译期常量**
             * （方案 §2f），所以常量求值要认这一个调用形态，与 `lower.cc` 的 `cv` 同一处、同一句。
             * 别把它读成「运行期初始化」：那会改 `HirGlobal` 的语义，波及 codegen 的 `make_globals`。 */
            const auto& n = static_cast<const CallExpr&>(node);
            const auto* id = as<IdentExpr>(n.callee.get());
            if (id != nullptr && id->name == "raw_cast" && !n.type_args.empty() &&
                n.args.size() == 1) {
                const TyRef ty = ty_of_ref(*n.type_args[0], n.src_line);
                return const_node(ty, pat_const(cv(*n.args[0])).value_or(0) & kMask64, n.src_line);
            }
            throw Unsupported("常量求值遇到函数调用", n.src_line);
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
        case NK::Def:
            def_stmt(parent, static_cast<const DefStmt&>(st));
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
        /* 值只折一次（`const_decl_value` 按声明节点备忘）；非整数常量进 `Hir.consts` 池，
         * 引用点拿到的 `Const` 带 `const_id` —— 引用 N 次仍是同一个常量。 */
        const HNode ref = const_decl_value(st);
        bind_const(st.name, ref);
        /* 非整数常量在 codegen 那边是**一枚私有全局**，它的发射时机是产物的一部分：旧路径在
         * 声明语句处就地折值，全局就排在这里（而不是第一个引用它的语句处）。HIR 里 `const #`
         * 本来没有节点，补一条**零指令**的占位语句把它钉回这个位置 —— `s_ExprStmt` 只是 `e()`
         * 一次，`Const` 不发指令，副作用就只有「把私有全局发出去」。整数常量没有全局，不补。 */
        if (ref.const_id >= 0) {
            HNode n = stmt_node(HKind::ExprStmt, st.src_line);
            n.kids.push_back(ref);
            parent.kids.push_back(std::move(n));
        }
        return;
    }
    HNode init = e(*st.init);
    if (init.ty->kind == TyKind::Void) {
        throw Unsupported(st.name + " 的初始化式没有值", st.src_line);
    }
    if ((st.kw == "let" || st.kw == "const") && init.kind == HKind::Const) {
        bind_const(st.name, init);
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

/* `lower.cc` 的 `s_AssignStmt`：先取左值、再求右值。**隐式转换不在这里发** —— 旧路径把它留给
 * codegen 的 store 链（`s_Assign` 的 `convert` + `store_addr` 的 `convert`，两道掩码），
 * 这里再插一个 `Cast` 就成了三道（157 的 4 道就是它）。源码里写了类型前缀的
 * （`x = U8 (y)`）那道 `Cast` 是 `e_cast` 发的，与隐式转换不是同一道，所以在 HIR 里仍可区分。 */
void Analyzer::assign_stmt(HNode& parent, const AssignStmt& st) {
    HNode target = lvalue(*st.target);
    HNode val;
    if (st.op == "=") {
        val = e(*st.value);
    } else {
        auto it = assign_binop().find(st.op);
        if (it == assign_binop().end()) throw Unsupported("复合赋值运算符 " + st.op, st.src_line);
        const HNode cur = target; /* HIR 里 `Local`/`Global` 节点就是「读一次」 */
        val = binop(it->second, cur, e(*st.value), st.src_line);
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
 *   ① 整单元预扫的名字表（`mod_->in_scope_names`，与引用点位置无关、也与自己裁剪掉的块无关）
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
    if (mod_->in_scope_names.count(t) != 0) return true;
    if (t.empty() || t[0] != '#') {
        if (mod_->in_scope_names.count('#' + t) != 0) return true;
    } else if (mod_->in_scope_names.count(t.substr(1)) != 0) {
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
        if (b->kind == Binding::Kind::Def) throw Unsupported("给 def 名 " + name + " 赋值", id.src_line);
        if (b->kind == Binding::Kind::Capture) return capture_node(b->ty, b->index, id.src_line);
        return local_node(name, b->ty, id.src_line);
    }
    auto g = mod_->globals.find(name);
    if (g != mod_->globals.end()) return global_node(g->second.id, g->second.ty, id.src_line);
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
    auto st = mod_->structs.find(base.ty->name);
    if (st == mod_->structs.end()) our_bug("struct " + base.ty->name + " 没有布局", mem.src_line);
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

void Analyzer::bind_const(const std::string& name, HNode value) {
    Binding b;
    b.kind = Binding::Kind::Const;
    b.ty = value.ty;
    b.bits = value.const_bits;
    b.const_id = value.const_id;
    scopes_.back()[name] = std::move(b);
}

void Analyzer::bind_capture(const std::string& name, TyRef ty, int index) {
    Binding b;
    b.kind = Binding::Kind::Capture;
    b.ty = std::move(ty);
    b.index = index;
    scopes_.back()[name] = std::move(b);
}

void Analyzer::bind_def(const std::string& name, const DefStmt* def, const std::string& owner_lex) {
    Binding b;
    b.kind = Binding::Kind::Def;
    b.def = def;
    b.owner_lex = owner_lex;
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
    Module mod(hir, src);
    Analyzer a(mod);
    a.run(src);
    return hir;
}

}  // namespace sema
}  // namespace simplexc
