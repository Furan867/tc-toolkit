/* lower.hh —— 类型化 AST → LLVM IR（M4.5 的 B 路产品实现；语义 oracle = `lower/ir_build.py`，只读）。
 *
 * 为什么逐条复刻 oracle 而不是「按 LLVM 的自然行为写」
 * ----------------------------------------------------
 * oracle 的行为是**实测过的参考实现语义**（`docs/facts.md` 的 U-L1 / U-L7 / U-L8 与 `/`、`%` 那张表），
 * 其中三处与 LLVM 的直觉相反，猜错就是语义分叉：
 *   · `>>` 恒为 `lshr`（不是 `ashr`），移位量按 W 取模，`asr` 在 n ≥ W 时饱和；
 *   · 加宽分两路 —— **编译期常量**按源类型符号性扩，**运行期值**一律 `zext(trunc_W)`（同一个表达式里
 *     两种结果，见 U-L7）；所以窄整数一律用 i64 位模式 + 显式掩码承载，不用 LLVM 原生 `iN`
 *     （N∉{8,16,32,64} 时后端假设不可控）；
 *   · `/`、`%` 的 opcode 看**被除数**的声明类型，除数完全不参与；窄常量参与运行期除法时被当成
 *     「W 位模式零扩展成 64 位」（参考实现的不自洽，不要「修正」）。
 * 每个此类取舍在实现里都标了 oracle 的行号，改动前先读那几行。
 *
 * 两个类的分工（与 oracle 的 `class Lower` / `class Program(Lower)` 一致）
 * ---------------------------------------------------------------------
 *   `Lower`   —— **函数级** lowering：表达式/语句/调用/内建/按需生成被调函数，外加它自己要用的
 *                类型解析与常量工具（`ty_of_ref` / `const_value` / `ll_type` …）；
 *   `Program` —— **模块级**装配：声明表扫描、类型与全局变量、程序根 → `tc_main`、pass + 落码。
 */
#pragma once

#include "../frontend/ast.hh"
#include "llvm_ffi.hh"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace simplexc {
namespace lower {

/* ---------------------------------------------------------------- 错误
 * 两种错误分开（oracle 同名）：`Unsupported` = 没实现 / 没定的构造（带源码行号，接口 §8 要求错误能
 * 定位到 span），`CompileFail` = 内部失败或环境问题。壳按 `ErrKind` 分类回退，别在这里合并。 */
class Unsupported : public std::runtime_error {
public:
    Unsupported(const std::string& msg, int line)
        : std::runtime_error(msg), line_(line) {}
    int line() const { return line_; }

private:
    int line_;
};

class CompileFail : public std::runtime_error {
public:
    explicit CompileFail(const std::string& msg) : std::runtime_error(msg) {}
};

struct Diagnostic {
    int line = 0;
    std::string msg;
};

/* ---------------------------------------------------------------- 类型
 * oracle `ir_build.py:227-278`。位宽是**真语义**（U-L7），`bits` 可以是 1..64 的任意值。 */
enum class TyKind { Void, Int, Ptr, Enum, Struct, Array, Str };

struct Ty;
using TyRef = std::shared_ptr<const Ty>;

struct Ty {
    TyKind kind = TyKind::Void;
    int bits = 64;
    bool is_signed = false;
    /* `Bool` 与 `U1` 在算术语义上等价（接口 §1），但**插值渲染不同**：`Bool` → `true`/`false`、
     * `U1` → `1`/`0`（实测见 docs/notes/2026-10-18-str-interp.md）—— 所以这个名义标记必须留着。 */
    bool is_bool = false;
    std::string name;                                   /* Enum / Struct 的名字 */
    std::vector<std::pair<std::string, TyRef>> fields;  /* Struct 字段（按声明序） */
    TyRef elem;                                         /* Array / Str 的元素类型 */
};

/* oracle 的模块级单例（`I64` / `BOOL` / `PTR` / `STR` / `SEED` / `TIME`）。 */
const TyRef& ty_void();
const TyRef& ty_i64();
const TyRef& ty_bool();
const TyRef& ty_ptr();
const TyRef& ty_str();
const TyRef& ty_seed();
const TyRef& ty_time();

TyRef make_int(int bits, bool is_signed);
TyRef make_enum(const std::string& name);
TyRef make_struct(const std::string& name, int bits, std::vector<std::pair<std::string, TyRef>> fields);
TyRef make_array(TyRef elem);

inline constexpr uint64_t kMask64 = ~uint64_t{0};
uint64_t mask_of(int bits);
int storage_bytes(const Ty& ty);   /* 内存字节宽度；Void / 未知抛 Unsupported */
const char* ty_kind_name(TyKind k);
inline bool is_int(const Ty& ty) { return ty.kind == TyKind::Int; }

/* 表达式的 lowering 结果：i64 位模式值（或 struct 聚合）+ 类型 + 可选的编译期常量。 */
struct V {
    LLVMValueRef ll = nullptr;
    TyRef ty;
    std::optional<uint64_t> c;

    static V instr(LLVMValueRef v, TyRef t) { return V{v, std::move(t), std::nullopt}; }
    static V konst(TyRef t, uint64_t c) { return V{nullptr, std::move(t), c}; }
};

/* ---------------------------------------------------------------- 常量折叠
 * 结果是**该类型的位模式**（`0 <= v < 2^bits`）。三条参考行为照抄 oracle `312-378`：截断发生在运算
 * 结果上（U-L7）、`>>` 是 lshr 语义、除法的符号性看被除数且除数为 0 时商 0 余数为被除数。 */
int64_t as_signed(uint64_t x, int bits);
uint64_t fold_arith(const std::string& op, uint64_t a, uint64_t b, const Ty& ty);
uint64_t fold_unary(const std::string& op, uint64_t a, const Ty& ty);
uint64_t fold_cmp(const std::string& op, uint64_t a, uint64_t b, const Ty& ty);   /* 0 / 1 */

/* ---------------------------------------------------------------- 前端 → lowering 的输入
 * oracle `Program.__init__` 从 `frontend/decls.py` 拿的那张声明表。`ast.hh` 没有顶层 `type` 声明的
 * 节点（它只管语句与表达式），所以这里给两个最小载体，字段类型仍用 `TypeRef` 的**拼写**表示。
 *
 * **AST 节点不归本模块所有**：`Program` 只持裸指针，调用方必须保证它们在 `Program` 活着期间有效
 * （前端把整棵树一次解析完，天然满足这点）。 */
struct StructDecl {
    std::string name;
    std::vector<std::pair<std::string, const TypeRef*>> fields;
};

struct EnumDecl {
    std::string name;
    std::vector<std::string> members;   /* 按声明序 = 序号（lowering 把它们当整数常量） */
};

struct ModuleAst {
    const FunctionDef* label = nullptr;          /* 程序根（`form == "label"`）；空 → CompileFail */
    std::vector<const FunctionDef*> defs;        /* 顶层 def */
    std::vector<const VarDeclStmt*> top_vars;    /* 顶层 var / const / let */
    std::vector<EnumDecl> enums;
    std::vector<StructDecl> structs;
    std::string source_name;                     /* 只进报错文本 */
};

/* 节点类别名（报错文本用，与原型 `type(node).__name__` 一致）。 */
const char* kind_name(NK k);

struct Capture {
    std::string name;
    LLVMValueRef ptr = nullptr;
    TyRef ty;
};

/* ---------------------------------------------------------------- Lower */
class Lower {
public:
    virtual ~Lower();

    const std::vector<Diagnostic>& diagnostics() const { return unsupported_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    int function_count() const { return fn_seen_ + 1; }   /* 含程序根 */

protected:
    /* ---- 模块骨架（Program 建；Lower 只借用句柄与类型缓存） */
    void open_module(const char* name);
    LLVMContextRef ctx() const { return ctx_ ? ctx_->get() : nullptr; }
    LLVMModuleRef module() const { return mod_ ? mod_->get() : nullptr; }

    void record(const std::string& msg, int line);

    /* ---- LLVM 类型与常量（oracle `Program.val_ll/ll_type/storage_ll/const_int/const_ptr`） */
    LLVMTypeRef val_ll(const Ty& ty);
    LLVMTypeRef ll_type(const Ty& ty);
    LLVMTypeRef storage_ll(const Ty& ty);
    LLVMValueRef const_int(uint64_t v) const;
    LLVMValueRef const_ptr(uint64_t addr) const;
    LLVMValueRef const_for_storage(const V& v);

    /* ---- 函数的状态保存 / 恢复（「按需生成」要边发一个函数边发它调用的函数） */
    struct Binding {
        enum class Kind { Local, Capture, Const, Def } kind = Kind::Local;
        LLVMValueRef ptr = nullptr;      /* Local / Capture：地址 */
        TyRef ty;                        /* Local / Capture */
        std::optional<V> value;          /* Const */
        const DefStmt* def = nullptr;    /* Def */

        static Binding local(LLVMValueRef p, TyRef t);
        static Binding capture(LLVMValueRef p, TyRef t);
        static Binding constant(V v);
        static Binding nested(const DefStmt* d);
    };
    using Scope = std::map<std::string, Binding>;

    struct FnState {
        LLVMBuilderRef bd = nullptr;
        LLVMValueRef fnref = nullptr;
        TyRef ret_ty;
        LLVMBasicBlockRef cur_block = nullptr;
        LLVMBasicBlockRef entry = nullptr;
        std::vector<Scope> scopes;
        std::vector<std::pair<std::string, LLVMBasicBlockRef>> breaks;
        std::string cur_name;
        const FunctionDef* cur_def = nullptr;
    };
    FnState save_state() const;
    void restore_state(const FnState& s);

    /* ---- 函数的开始 / 收尾 / 块与 alloca */
    void mark_no_builtin(LLVMValueRef fnref);
    void begin_function(LLVMValueRef fnref, const TyRef& ret_ty);
    void finish_function();
    LLVMBasicBlockRef new_block(const char* name);
    LLVMBasicBlockRef br_new_block(const char* name);
    /* entry 块最前面的 alloca（**静态** alloca）；`alloca_slot` 是它的类型化包装。 */
    LLVMValueRef alloca_in_entry(LLVMTypeRef ty, const std::string& name);
    LLVMValueRef alloca_slot(const TyRef& ty, const std::string& name);

    /* ---- 作用域 */
    void bind(const std::string& name, Binding b);
    void bind_root_scope(const std::string& name, Binding b);
    const Binding* lookup(const std::string& name) const;

    /* ---- 值工具（oracle `llv` / `pat_const` / `pattern_of` / `mask_ll` / `convert`） */
    LLVMValueRef llv(const V& v);
    std::optional<uint64_t> pat_const(const V& v);
    LLVMValueRef pattern_of(const V& v);
    uint64_t need_const(const V& v, int line);
    LLVMValueRef mask_ll(LLVMValueRef ll, int bits);
    V convert(const V& v, const TyRef& target, int line);
    LLVMValueRef to_ptr(const V& v);
    V load_addr(LLVMValueRef p, const TyRef& ty);
    void store_addr(LLVMValueRef p, const V& v, const TyRef& ty, int line);
    std::pair<LLVMValueRef, TyRef> lvalue(const Node& node);
    std::pair<LLVMValueRef, TyRef> prefix_global(const std::string& name, int line);

    /* ---- 类型解析（`TypeRef` 只存拼写，解析在 lowering 侧 —— ast.hh 的约定） */
    TyRef ty_of_name(const std::string& name, int line);
    TyRef ty_of_ref(const TypeRef& ref, int line);

    /* ---- 语句 */
    void lower_block(const Block& block);
    void lower_stmt(const Stmt& st);
    void lower_if_runtime(const std::vector<const IfCase*>& cases, const Block* else_block);
    std::optional<bool> in_scope_const(const Expr& cond);
    bool ppif_true(const Expr& cond);

    void s_VarDeclStmt(const VarDeclStmt& st);
    void s_AssignStmt(const AssignStmt& st);
    void s_ExprStmt(const ExprStmt& st);
    void s_IfStmt(const IfStmt& st);
    void s_PpIfStmt(const PpIfStmt& st);
    void s_WhileStmt(const WhileStmt& st);
    void s_BreakStmt(const BreakStmt& st);
    void s_ReturnStmt(const ReturnStmt& st);
    void s_SwitchStmt(const SwitchStmt& st);
    void s_DefStmt(const DefStmt& st);

    std::vector<Capture> captures_of(const DefStmt& st);

    /* ---- 表达式 */
    V e(const Expr& node);
    V e_LiteralExpr(const LiteralExpr& node);
    V e_IdentExpr(const IdentExpr& node);
    V e_CastExpr(const CastExpr& node);
    V e_UnaryExpr(const UnaryExpr& node);
    V e_BinaryExpr(const BinaryExpr& node);
    V e_CondExpr(const CondExpr& node);
    V e_CallExpr(const CallExpr& node);
    V e_MemberExpr(const MemberExpr& node);
    V e_IndexExpr(const IndexExpr& node);
    V e_ArrayLitExpr(const ArrayLitExpr& node);
    V e_CompositeLitExpr(const CompositeLitExpr& node);
    V e_TemplateExpr(const TemplateExpr& node);

    /* ---- 模板插值与 Str 拼接（语义实测：docs/notes/2026-10-18-str-interp.md） */
    V template_expr(const TemplateExpr& node);
    V str_concat(const V& a, const V& b, int line);
    LLVMValueRef render_part(LLVMValueRef cur, const V& v, int line);
    uint64_t part_capacity(const V& v, int line);
    LLVMValueRef write_literal(LLVMValueRef cur, const std::string& text, int line);
    LLVMValueRef len_of(const V& obj, int line);
    /* 产物内联的两个辅助函数（internal linkage，**不引外部符号**）：
       `__simplexc_dec(ptr p, u64 幅度, u64 负号) -> 写完的游标`、
       `__simplexc_copy(ptr dst, ptr src, u64 n) -> dst + n`。 */
    LLVMValueRef ensure_dec_fn();
    LLVMValueRef ensure_copy_fn();

    /* ---- 运算（类型统一 + 常量折叠 + 逐 op 的参考行为） */
    TyRef unify(const TyRef& a, const TyRef& b, int line);
    LLVMValueRef as_cond(const V& v);
    V compare(const std::string& op, const V& a, const V& b, int line);
    LLVMValueRef sext64(LLVMValueRef ll, int bits);
    V logical(const BinaryExpr& node);
    V binop(const std::string& op, const V& a, const V& b, int line);
    LLVMValueRef div(const std::string& op, LLVMValueRef x, LLVMValueRef y, const Ty& ty);

    /* ---- 调用与按需生成 */
    TyRef static_type(const Node& node);
    bool compatible(const TyRef& arg, const TyRef& param);
    const FunctionDef* pick_def(const std::string& name, const CallExpr& node);
    TyRef defstmt_ret(const DefStmt& st);
    std::vector<TyRef> defstmt_params(const DefStmt& st);
    std::optional<std::vector<TyRef>> decl_param_types(const FunctionDef& d);
    V call_named(const std::string& name, const CallExpr& node);
    LLVMValueRef emit_call(LLVMValueRef fnref, const std::vector<LLVMValueRef>& args);
    V call_user(const FunctionDef& d, const CallExpr& node);
    V call_nested(const Binding& binding, const CallExpr& node);
    V builtin_load(const CallExpr& node);
    V builtin_store(const CallExpr& node);
    V builtin_raw_cast(const CallExpr& node);
    V builtin_asr(const CallExpr& node);
    V builtin_minmax(const std::string& name, const CallExpr& node);
    V call_method(const MemberExpr& callee, const CallExpr& node);
    V call_prefix(const std::string& name, const CallExpr& node);
    LLVMValueRef get_extern(const std::string& name, const TyRef& ret_ty,
                            const std::vector<TyRef>& param_tys);
    LLVMValueRef ensure_prefix_fn(const std::string& name, const TyRef& ret_ty,
                                  const std::vector<TyRef>& param_tys);
    void body_get_time();
    void body_sleep();
    void body_memory_clear();
    void body_memory_copy();
    /* `i = 0` + 循环头；返回 i 的地址槽，循环头块与 `n` 从出参出（oracle 同名函数）。 */
    LLVMValueRef byte_loop_prologue(unsigned n_param, LLVMBasicBlockRef* head_out,
                                    LLVMValueRef* n_out);
    LLVMValueRef ensure_def_fn(const std::string& name, int64_t lo, const FunctionDef& d,
                               const TyRef& ret_ty, const std::vector<TyRef>& param_tys);
    LLVMValueRef ensure_nested_fn(const DefStmt& st, const TyRef& ret_ty,
                                  const std::vector<TyRef>& param_tys,
                                  const std::vector<Capture>& caps);

    /* ---- 编译期常量求值（`const_value` 是「不是常量就 None」的包装） */
    std::optional<V> const_value(const Node& node);
    V cv(const Node& node);
    V string_literal(const LiteralExpr& node);
    V array_literal(const ArrayLitExpr& node);
    V composite_literal(const CompositeLitExpr& node);

    /* ---- 状态（Program 填；Lower 读） */
    std::map<std::string, TyRef> structs_;
    std::map<std::string, std::vector<std::string>> enums_;
    std::map<std::string, std::pair<std::string, int>> enum_member_;   /* 成员 → (枚举, 序号) */
    std::map<std::string, std::pair<LLVMValueRef, TyRef>> globals_;
    std::map<std::string, V> consts_;
    std::map<std::string, std::vector<const FunctionDef*>> defs_;
    std::vector<const VarDeclStmt*> top_vars_;

    struct FieldRef {
        int off = 0;
        TyRef ty;
    };
    std::map<std::string, std::map<std::string, FieldRef>> field_offsets_;
    std::map<std::string, LLVMTypeRef> struct_ll_;
    std::map<std::string, V> str_pool_;
    std::map<std::string, LLVMValueRef> helpers_;   /* 模板插值用的内联辅助函数（按需生成） */

    struct Fn {
        LLVMValueRef fnref = nullptr;
        TyRef ret;
        std::vector<TyRef> params;
        std::vector<Capture> captures;
    };
    /* 键 = (类别, 名字, 源码行, 附加名字)：与原型的三元组 / 二元组一一对应。 */
    std::map<std::tuple<int, std::string, int64_t, std::string>, Fn> fns_;

    std::unique_ptr<Context> ctx_;
    std::unique_ptr<Module> mod_;
    LLVMTypeRef i64_ = nullptr;
    LLVMTypeRef ptr_ = nullptr;
    LLVMTypeRef void_ty_ = nullptr;
    std::map<unsigned, LLVMTypeRef> int_types_;

    /* 当前函数 */
    LLVMBuilderRef bd_ = nullptr;
    LLVMValueRef fnref_ = nullptr;
    TyRef ret_ty_;
    LLVMBasicBlockRef cur_block_ = nullptr;
    LLVMBasicBlockRef entry_ = nullptr;
    std::vector<Scope> scopes_;
    std::vector<std::pair<std::string, LLVMBasicBlockRef>> breaks_;   /* label（空 = 无标签）→ 出口块 */
    std::string cur_name_;
    const FunctionDef* cur_def_ = nullptr;

    int fn_seen_ = 0;
    bool collect_ = false;
    std::vector<Diagnostic> unsupported_;
    std::vector<std::string> warnings_;
};

/* ---------------------------------------------------------------- Program */
class Program : public Lower {
public:
    explicit Program(const ModuleAst& ast, bool collect = false);

    /* 生成整个模块：程序根 → `tc_main`，其余 def 在 lowering 中按需生成。 */
    void build();
    std::string ir_text() const;
    std::vector<uint8_t> emit_object();
    const ModuleAst& ast() const { return ast_; }

private:
    void scan_decls();
    void make_context();
    void make_types();
    void make_globals();
    void emit_main(const FunctionDef& label);

    ModuleAst ast_;
};

}  // namespace lower
}  // namespace simplexc
