/* lower.hh —— 类型化 AST → LLVM IR（M4.5 的 B 路产品实现）。
 *
 * 为什么按实测语义逐条实现，而不是「按 LLVM 的自然行为写」
 * ----------------------------------------------------
 * 下面这些行为是**实测过的原版编译器语义**（`docs/facts/semantics.md` 的 U-L1 / U-L7 / U-L8 与 `/`、`%` 那张表），
 * 其中三处与 LLVM 的直觉相反，猜错就是语义分叉：
 *   · `>>` 恒为 `lshr`（不是 `ashr`），移位量按 W 取模，`asr` 在 n ≥ W 时饱和；
 *   · 加宽分两路 —— **编译期常量**按源类型符号性扩，**运行期值**一律 `zext(trunc_W)`（同一个表达式里
 *     两种结果，见 U-L7）；所以窄整数一律用 i64 位模式 + 显式掩码承载，不用 LLVM 原生 `iN`
 *     （N∉{8,16,32,64} 时后端假设不可控）；
 *   · `/`、`%` 的 opcode 看**被除数**的声明类型，除数完全不参与；窄常量参与运行期除法时被当成
 *     「W 位模式零扩展成 64 位」（原版编译器的不自洽，不要「修正」）。
 * 每个此类取舍在实现里都标了对应的语义档案条目，改动前先读那几条。
 *
 * 两个类的分工
 * ---------------------------------------------------------------------
 *   `Lower`   —— **函数级** lowering：表达式/语句/调用/内建/按需生成被调函数，外加它自己要用的
 *                类型解析与常量工具（`ty_of_ref` / `const_value` / `ll_type` …）；
 *   `Program` —— **模块级**装配：声明表扫描、类型与全局变量、程序根 → `tc_main`、pass + 落码。
 */
#pragma once

#include "../frontend/ast.hh"
#include "../sema/errors.hh"
#include "../sema/types.hh"
#include "fn_ctx.hh"
#include "llvm_ffi.hh"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace simplexc {
namespace lower {

/* ---------------------------------------------------------------- 过渡态（②d 清理）
 * 类型系统与错误已搬到 `sema/`，用 using 把名字引入本命名空间 —— 让 `lower/` 里的调用点一行
 * 不用改。**这是刻意的过渡态**，不是长期形态。 */
using sema::CompileFail;
using sema::Diagnostic;
using sema::Ty;
using sema::TyRef;
using sema::TyKind;
using sema::Unsupported;
using sema::as_signed;
using sema::fold_arith;
using sema::fold_cmp;
using sema::fold_unary;
using sema::is_int;
using sema::kMask64;
using sema::make_array;
using sema::make_enum;
using sema::make_int;
using sema::make_struct;
using sema::mask_of;
using sema::storage_bytes;
using sema::ty_bool;
using sema::ty_i64;
using sema::ty_kind_name;
using sema::ty_ptr;
using sema::ty_seed;
using sema::ty_str;
using sema::ty_time;
using sema::ty_void;

/* `V` / `Binding` / `Scope` / `FnCtx` 在 `fn_ctx.hh`：它们都是**每函数一份**的东西，
 * 按需生成的递归靠压/弹 `FnCtx` 表达，不再靠逐字段快照存取。 */

/* ---------------------------------------------------------------- 前端 → lowering 的输入
 * 声明表（`Program` 从 `frontend/decls.hh` 拿的那张）。`ast.hh` 没有顶层 `type` 声明的
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

/* 一个嵌套 def 的闭包面：要传进按需生成的函数的捕获表，加上它体里按名调用、
 * 但**定义在它之外**的嵌套 def（这些名字也得在生成的函数里可解析）。 */
struct Closure {
    std::vector<Capture> caps;
    std::vector<const DefStmt*> defs;
};

/* ---------------------------------------------------------------- Lower */
class Lower {
public:
    Lower();
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

    /* ---- LLVM 类型与常量（`val_ll` / `ll_type` / `storage_ll` / `const_int` / `const_ptr`） */
    LLVMTypeRef val_ll(const Ty& ty);
    LLVMTypeRef ll_type(const Ty& ty);
    LLVMTypeRef storage_ll(const Ty& ty);
    LLVMValueRef const_int(uint64_t v) const;
    LLVMValueRef const_ptr(uint64_t addr) const;
    LLVMValueRef const_for_storage(const V& v);
    LLVMValueRef field_storage_value(const V& v, const TyRef& ty, int line);

    /* ---- 当前函数的上下文
     * `fn_ctxs_` 是**栈**：`back()` 就是当前函数（基座在 `Lower` 构造时压入，所以 `cur_fn()`
     * 在任何时刻都有效 —— `make_globals` 这类模块级步骤会求编译期常量，那时也要读 `ty_binds`）。
     * 递归生成被调函数时 `begin_function` 压一个新的，`finish_function` 弹掉；两者之外没有
     * 任何地方能改「当前函数」（见 `fn_ctx.hh` 头注释）。 */
    FnCtx& cur_fn() { return *fn_ctxs_.back(); }
    const FnCtx& cur_fn() const { return *fn_ctxs_.back(); }
    std::vector<std::unique_ptr<FnCtx>> fn_ctxs_;

    /* ---- 函数的开始 / 收尾 / 块与 alloca */
    void mark_no_builtin(LLVMValueRef fnref);
    /* 压一个新函数上下文并返回它：builder / entry 块 / 空作用域表在这里建好；
     * 身份字段（`name` / `def` / `ty_binds`）由调用点在返回的引用上写 —— 它们的沿用规则
     * 三种调用点各不相同（见 `ensure_*_fn` 与 `emit_main`）。 */
    FnCtx& begin_function(LLVMValueRef fnref, const TyRef& ret_ty);
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

    /* ---- 值工具（`llv` / `pat_const` / `pattern_of` / `mask_ll` / `convert`） */
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

    /* ---- 泛型实例化（`@Type` / `@Size` / `@Any`）
     * 语义（实测：docs/notes/2026-10-18-routea-semantics.md §1）：按**实参的具体类型**实例化，
     * `size()` 折成编译期常数（单位 = 位）。接口 §1 冻结的名字规则是
     * `实例化名 = "<函数名>$<实参规范化串>"`、同一 (函数, 实参) 只实例化一次（缓存键就是那个串）。
     * 类型实参在调用点有**两种写法**（都实测过）：`g(<U8>)`（不占运行期槽）与 `g(load(<U8>,p))`
     * （值本身就是实参、类型由它推出）。所以运行期形参表只能是**每个实例一份**。 */
    struct Inst {
        std::vector<TyRef> bind_order;               /* 类型参数的具体类型（按声明序） */
        std::map<std::string, TyRef> binds;          /* 类型参数名 → 具体类型 */
        std::vector<TyRef> rt_types;                 /* 运行期形参类型（按声明序，跳过不占槽的类型参数） */
        std::vector<const Param*> rt_params;         /* 与 rt_types 一一对应 */
        std::vector<size_t> rt_arg;                  /* 运行期形参 i ← 调用点 args[rt_arg[i]] */
        std::string sig;                             /* 规范化串；无类型参数时为空（= 产物不变） */
    };
    static bool is_type_param(const TypeRef& ref);
    static bool has_type_params(const std::vector<std::unique_ptr<Param>>& params);
    Inst resolve_inst(const std::vector<std::unique_ptr<Param>>& params,
                      const CallExpr& node, int line);
    TyRef inst_ret_type(const TypeRef* ret, const Inst& inst, int line);
    /* 类型值表达式（只出现在类型实参位置）：`load(type_of(x), p)` 的第一个实参。 */
    TyRef type_value_of(const Expr& node, int line);
    /* 编译期要的「这个操作数的类型」：本实例的类型参数先查，其余走 `static_type`。 */
    TyRef operand_type(const Expr& node, int line);
    /* `size(x)` 的编译期常数（单位 = 位）；只对类型参数成立，别的形态仍报 Unsupported。 */
    V builtin_size(const CallExpr& node);

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

    Closure captures_of(const DefStmt& st);
    /* 顶层 `def` 的闭包面（按**调用点**算，见定义处）。 */
    Closure def_captures(const FunctionDef& d);
    void collect_captures(const Block& body, const std::vector<std::string>& params,
                          bool into_own_defs, Closure& out,
                          std::set<const DefStmt*>& visiting);

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
    /* `__simplexc_streq(ptr a, ptr b, u64 n) -> 1/0`：Str 内容相等（`switch` 与 `==` 共用）。 */
    LLVMValueRef ensure_streq_fn();
    /* `llvm.ctlz.i64` 的声明（`clz` 用，见 `builtin_clz`）。 */
    LLVMValueRef ensure_clz_fn();

    /* ---- 运算（类型统一 + 常量折叠 + 逐 op 的参考行为） */
    TyRef unify(const TyRef& a, const TyRef& b, int line);
    /* 算术的结果类型：整型字面量与窄整型混用时取**窄**的那一侧（不是 `unify` 的「宽者胜」）。 */
    TyRef arith_ty(const V& a, const V& b, const std::string& op, int line);
    LLVMValueRef as_cond(const V& v);
    V compare(const std::string& op, const V& a, const V& b, int line);
    /* `Str == Str` / `Str != Str`：按**内容**（长度 + 逐字节），不是指针 —— 语料用 `switch id`
     * 拿**运行期插值出来**的名字派发 UI 元素，指针比永远不会命中。 */
    V str_compare(const std::string& op, const V& a, const V& b, int line);
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
    V builtin_clz(const CallExpr& node);
    V builtin_ror(const CallExpr& node);
    V builtin_minmax(const std::string& name, const CallExpr& node);
    V builtin_array(const CallExpr& node);
    V builtin_random(const CallExpr& node);
    /* `random` 的种子：前缀全局 `global_seed`（源码自己也会给它赋值，所以只能有一份）。 */
    std::pair<LLVMValueRef, TyRef> seed_global(int line);
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
    /* `i = 0` + 循环头；返回 i 的地址槽，循环头块与 `n` 从出参出。 */
    LLVMValueRef byte_loop_prologue(unsigned n_param, LLVMBasicBlockRef* head_out,
                                    LLVMValueRef* n_out);
    LLVMValueRef ensure_def_fn(const std::string& name, int64_t lo, const FunctionDef& d,
                               const TyRef& ret_ty, const Inst& inst, const Closure& closure);
    LLVMValueRef ensure_nested_fn(const DefStmt& st, const TyRef& ret_ty,
                                  const std::vector<TyRef>& param_tys, const Closure& closure);

    /* ---- 编译期常量求值（`const_value` 是「不是常量就 None」的包装） */
    std::optional<V> const_value(const Node& node);
    V cv(const Node& node);
    V string_literal(const LiteralExpr& node);
    V array_literal(const ArrayLitExpr& node);
    /* `Arr[T] []`：空数组字面量（类型只在外面的转换上，见定义处）。 */
    V empty_array(const TyRef& arr_ty, int line);
    V composite_literal(const CompositeLitExpr& node);
    V root_const_value(const VarDeclStmt& decl);   /* 程序根体的 `const #`：求值一次 */

    /* ---- 状态（Program 填；Lower 读） */
    std::map<std::string, TyRef> structs_;
    std::map<std::string, std::vector<std::string>> enums_;
    std::map<std::string, std::pair<std::string, int>> enum_member_;   /* 成员 → (枚举, 序号) */
    std::map<std::string, std::pair<LLVMValueRef, TyRef>> globals_;
    std::map<std::string, V> consts_;
    std::map<std::string, std::vector<const FunctionDef*>> defs_;
    std::vector<const VarDeclStmt*> top_vars_;
    /* `in_scope("名字")` ① 档的名字表：整单元预扫的 `def` / `type` / `const #`，与引用点位置无关
     * （U-L14 §0）。`var` 故意不在里面 —— 它的可见性跟声明顺序走，属于 ② 档。 */
    std::set<std::string> in_scope_names_;
    /* 程序根语句树里的嵌套 def：整单元可见（任何函数里都能按名调用），与顶层 `defs_` 同档 ——
     * U-L14 §2 的 T8/U1：引用点在别的 `def` 体里、声明在 `run_sim` 体里的名字判真。 */
    std::map<std::string, const DefStmt*> root_defs_;
    /* 程序根语句树里的 `const #`（`collect_root_decls` 收的）：与 `root_defs_` 同档 ——
     * 它的**值**也要能在任何函数里按名取到。引用点常常在别的函数里（008 的 `check_output`
     * 被顶层 def `mode_run` 调用，那时这个 const 的词法作用域并不在栈上，`lookup` 查不到）。
     * 按**声明节点指针**备忘：同一个声明只求值一次、只出一个全局常量 —— 否则
     * 程序根自己那句声明与外部引用点会各求一次值（两个内容相同、名字不同的私有全局）。 */
    std::map<std::string, const VarDeclStmt*> root_const_decls_;
    std::map<const VarDeclStmt*, V> root_const_values_;

    struct FieldRef {
        int off = 0;
        TyRef ty;
    };
    std::map<std::string, std::map<std::string, FieldRef>> field_offsets_;
    std::map<std::string, LLVMTypeRef> struct_ll_;
    std::map<std::string, V> str_pool_;
    /* `array(<T>, n)` 每个调用点一枚私有全局：同名不同点会撞车，序号保证唯一。 */
    int array_serial_ = 0;
    std::map<std::string, LLVMValueRef> helpers_;   /* 模板插值用的内联辅助函数（按需生成） */

    struct Fn {
        LLVMValueRef fnref = nullptr;
        TyRef ret;
        std::vector<TyRef> params;
        std::vector<Capture> captures;
    };
    /* 键 = (类别, 名字, 源码行, 附加名字)：与原型的三元组 / 二元组一一对应。
     * 类别 1（顶层 def）的第 4 位是**捕获签名**：空串 = 没有捕获，键与闭包化之前完全相同。 */
    std::map<std::tuple<int, std::string, int64_t, std::string>, Fn> fns_;
    /* 每个顶层 def 已发过几个捕获实例（生成 `$c0`/`$c1` 这样的名字后缀，并按上限兜住
     * 「互调 + 捕获」的发散，见 `ensure_def_fn`）。 */
    std::map<std::pair<std::string, int64_t>, int> cap_instances_;

    std::unique_ptr<Context> ctx_;
    std::unique_ptr<Module> mod_;
    LLVMTypeRef i64_ = nullptr;
    LLVMTypeRef ptr_ = nullptr;
    LLVMTypeRef void_ty_ = nullptr;
    std::map<unsigned, LLVMTypeRef> int_types_;

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
    void scan_scope_names();
    void make_context();
    void make_types();
    void make_globals();
    void emit_main(const FunctionDef& label);

    ModuleAst ast_;
};

}  // namespace lower
}  // namespace simplexc
