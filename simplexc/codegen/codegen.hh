/* codegen/codegen.hh —— **类型化 HIR → LLVM IR**（codegen 的终态入口）。
 *
 * 输入只有 `sema::Hir`：不 include `frontend/`，不查源码，不做类型推导、不决定实例化
 * （方案 §1 的终态表 / §2 决策 2、4）。三件事因此从本模块消失：
 *   · 类型怎么会合（`unify` / `arith_ty` / `static_type` / `operand_type` / `type_value_of`）——
 *     HIR 每个表达式节点自带 `ty`，隐式转换已经是显式的 `Cast` 节点；
 *   · 常量折叠（`const_value` / `cv`）—— HIR 的 `Const` 已折好、已按类型截断；
 *   · 实例化与重载解析（`resolve_inst` / `pick_def` / `compatible`）—— `Call` 的被调者已经是
 *     `Hir.funcs` 的下标。
 * 留下的全是「值 → LLVM」的映射本身：块结构、alloca、位模式约定（窄整型一律 i64 位模式 + 显式
 * 掩码，U-L7）、调用约定，以及 98 份语料实测出来的逐 op 行为（见 `docs/facts/semantics.md`）。
 *
 * 符号名为什么由这里派生而不是 sema 给好：`tc_main` / `name[#L<lo>][$sig][$c<n>]` /
 * `owner$name#L<line>` 是**产物**的细节（方案 §2b 补漏 1），选法依赖「这个 def 在文件里是否重载」
 * 与「当前正在发的是不是顶层实例」——都是发射期的知识。sema 只给逻辑名 + `cap_index`。
 *
 * 函数体**按需发射**：`Call` 第一次指向某个 `Hir.funcs[i]` 时才建那个 LLVM 函数并当场发它的体。
 * 顺序因此与改前逐字节相同（旧路径就是「发函数体时又发现新实例」的按需递归），也是块与指令
 * 命名能对上的前提。
 */
#pragma once

#include "../sema/errors.hh"
#include "../sema/hir.hh"
#include "../sema/types.hh"
#include "llvm_ffi.hh"
#include "codegen_ctx.hh"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace simplexc {
namespace codegen {

using sema::CompileError;
using sema::Responsibility;
using sema::Span;
using sema::Stage;
using sema::Ty;
using sema::TyRef;
using sema::TyKind;
using sema::is_int;
using sema::kMask64;
using sema::make_array;
using sema::make_int;
using sema::mask_of;
using sema::shift_amount_mask;
using sema::storage_bytes;
using sema::ty_bool;
using sema::ty_i64;
using sema::ty_kind_name;
using sema::ty_ptr;
using sema::ty_str;
using sema::ty_time;
using sema::ty_void;

/* ---------------------------------------------------------------- Codegen */
class Codegen {
public:
    explicit Codegen(const sema::Hir& hir);
    virtual ~Codegen();

    const std::vector<std::string>& warnings() const { return warnings_; }
    int function_count() const { return fn_seen_ + 1; } /* 含程序根 */

protected:
    /* ---- 模块骨架 */
    void open_module(const char* name);
    LLVMContextRef ctx() const { return ctx_ ? ctx_->get() : nullptr; }
    LLVMModuleRef module() const { return mod_ ? mod_->get() : nullptr; }

    /* ---- LLVM 类型与常量 */
    LLVMTypeRef val_ll(const Ty& ty);
    LLVMTypeRef ll_type(const Ty& ty);
    LLVMTypeRef storage_ll(const Ty& ty);
    LLVMValueRef const_int(uint64_t v) const;
    LLVMValueRef const_ptr(uint64_t addr) const;
    /* 位模式 → 存储形态的常量（全局初值、聚合字段都走它）。 */
    LLVMValueRef const_for_storage(const V& v, Span span);
    LLVMValueRef const_for_bits(const TyRef& ty, uint64_t bits);
    LLVMValueRef field_storage_value(const V& v, const TyRef& ty, int line);

    /* ---- 当前函数的上下文（栈：`back()` 就是当前函数） */
    FnCtx& cur_fn() { return *fn_ctxs_.back(); }
    const FnCtx& cur_fn() const { return *fn_ctxs_.back(); }
    std::vector<std::unique_ptr<FnCtx>> fn_ctxs_;

    void mark_no_builtin(LLVMValueRef fnref);
    FnCtx& begin_function(LLVMValueRef fnref, const TyRef& ret_ty);
    void finish_function();
    LLVMBasicBlockRef new_block(const char* name);
    LLVMBasicBlockRef br_new_block(const char* name);
    LLVMValueRef alloca_in_entry(LLVMTypeRef ty, const std::string& name);
    LLVMValueRef alloca_slot(const TyRef& ty, const std::string& name);

    /* ---- 作用域 */
    void bind(const std::string& name, Binding b);
    void bind_root_scope(const std::string& name, Binding b);
    const Binding* lookup(const std::string& name) const;

    /* ---- 值工具（`llv` / `pattern_of` / `mask_ll` / `convert`） */
    LLVMValueRef llv(const V& v);
    std::optional<uint64_t> pat_const(const V& v);
    LLVMValueRef pattern_of(const V& v);
    LLVMValueRef mask_ll(LLVMValueRef ll, int bits);
    /* 值 → 目标类型的位模式（旧 `convert` 的掩码）。**每个**旧路径调 `convert` 的地方都在这里
     * 调它，包括「值已经是同一类型」的情形 —— 旧路径那时也照发一层 `and`，少发一层 IR 就
     * 逐指令不同了（产物字节不变，但双跑的 IR 判据会红）。 */
    V convert(const V& v, const TyRef& target, int line);
    LLVMValueRef to_ptr(const V& v);
    V load_addr(LLVMValueRef p, const TyRef& ty);
    void store_addr(LLVMValueRef p, const V& v, const TyRef& ty, int line);
    std::pair<LLVMValueRef, TyRef> lvalue(const sema::HNode& node);
    std::pair<LLVMValueRef, TyRef> global_slot(int global_id, int line);
    /* 建一个全局（初值 = `HirGlobal.init_bits`，按 `ty` 的整宽写）。顶层声明的全局在
     * `Program::make_globals` 里预建，前缀全局第一次被引用时才建（建序照旧路径）。 */
    std::pair<LLVMValueRef, TyRef> add_global(const sema::HirGlobal& g);

    /* ---- 语句（HIR 的 9 种语句节点） */
    void lower_block(const sema::HNode& block);
    void lower_stmt(const sema::HNode& st);
    /* `if` / `elif` / `else` 链：所有分支汇到同一个 `ifend` 块（旧 `s_IfStmt` 的形状）。 */
    void lower_if_runtime(const std::vector<const sema::HNode*>& conds,
                          const std::vector<const sema::HNode*>& blocks,
                          const sema::HNode* else_block);
    void s_VarDecl(const sema::HNode& st);
    void s_Assign(const sema::HNode& st);
    void s_ExprStmt(const sema::HNode& st);
    void s_If(const sema::HNode& st);
    void s_While(const sema::HNode& st);
    void s_Break(const sema::HNode& st);
    void s_Return(const sema::HNode& st);
    void s_Switch(const sema::HNode& st);

    /* ---- 表达式（HIR 的 15 种表达式节点） */
    V e(const sema::HNode& node);
    V e_const(const sema::HNode& node);
    V e_strlit(const sema::HNode& node);
    V e_local(const sema::HNode& node);
    V e_global(const sema::HNode& node);
    V e_capture(const sema::HNode& node);
    V e_unary(const sema::HNode& node);
    V e_binary(const sema::HNode& node);
    V e_cond(const sema::HNode& node);
    V e_call(const sema::HNode& node);
    V e_member(const sema::HNode& node);
    V e_index(const sema::HNode& node);
    V e_cast(const sema::HNode& node);
    /* `Cast` 节点单独走：`e_cast` = `apply_cast(node, e(kid))`；二元运算剥掉 kid 顶层的 `Cast`
     * 先求值、两次求值都做完再各自 `apply_cast` —— 旧 `convert` 就是这个顺序。 */
    V apply_cast(const sema::HNode& node, V v);
    std::pair<const sema::HNode*, V> eval_deferred(const sema::HNode& kid);
    V settle(const std::pair<const sema::HNode*, V>& d);
    V e_array_lit(const sema::HNode& node);
    V e_composite_lit(const sema::HNode& node);
    V e_template(const sema::HNode& node);

    /* ---- 串与模板插值（语义实测：docs/notes/2026-10-18-str-interp.md） */
    V string_literal(int string_id, int line);
    V template_expr(const sema::HNode& node);
    V str_concat(const V& a, const V& b, int line);
    LLVMValueRef render_part(LLVMValueRef cur, const V& v, int line);
    uint64_t part_capacity(const V& v, int line);
    LLVMValueRef write_literal(LLVMValueRef cur, const std::string& text, int line);
    LLVMValueRef len_of(const V& obj, int line);
    /* 产物内联的辅助函数（internal linkage，**不引外部符号**）。 */
    LLVMValueRef ensure_dec_fn();
    LLVMValueRef ensure_copy_fn();
    LLVMValueRef ensure_streq_fn();
    LLVMValueRef ensure_clz_fn();

    /* ---- 运算（逐 op 的参考行为；类型一律从 HIR 节点上读） */
    LLVMValueRef as_cond(const V& v, Span span);
    V compare(const sema::HNode& node);
    V str_compare(const V& a, const V& b, int line, bool ne);
    V logical(const sema::HNode& node);
    V binop(const sema::HNode& node);
    LLVMValueRef sext64(LLVMValueRef ll, int bits);
    LLVMValueRef div(const std::string& op, LLVMValueRef x, LLVMValueRef y, const Ty& ty);

    /* ---- 调用与按需发射 */
    LLVMValueRef emit_call(LLVMValueRef fnref, const std::vector<LLVMValueRef>& args);
    /* 符号名（方案 §2b 补漏 1）。规则与「逻辑名」的差别见定义处；顶层那份还缺 sema 的两个
     * 事实（`decl_line` / `line_suffix`），见定义处。 */
    std::string nested_owner() const;
    std::string symbol_of(const sema::HirFunc& f) const;
    std::string logical_name_of(const sema::HirFunc& f) const;
    LLVMValueRef ensure_func(int index);
    V call_func(const sema::HNode& node);
    V call_builtin(const sema::HNode& node);
    V builtin_load(const sema::HNode& node);
    V builtin_store(const sema::HNode& node);
    V builtin_raw_cast(const sema::HNode& node);
    V builtin_asr(const sema::HNode& node);
    V builtin_clz(const sema::HNode& node);
    V builtin_ror(const sema::HNode& node);
    V builtin_minmax(const sema::HNode& node, bool is_min);
    V builtin_array(const sema::HNode& node);
    V builtin_random(const sema::HNode& node);
    V builtin_len(const sema::HNode& node);
    V builtin_ms(const sema::HNode& node);
    /* 前缀函数的五个优化实现（方案 §2f：`HirFunc.pre_body != None` 时**不编译它的体**）。 */
    V call_prefix(const sema::HirFunc& f, const sema::HNode& node);
    LLVMValueRef get_extern(const std::string& name, const TyRef& ret_ty,
                            const std::vector<TyRef>& param_tys);
    LLVMValueRef ensure_prefix_fn(const std::string& name, const TyRef& ret_ty,
                                  const std::vector<TyRef>& param_tys, Span span);
    void body_get_time();
    void body_sleep();
    void body_memory_clear();
    void body_memory_copy();
    LLVMValueRef byte_loop_prologue(unsigned n_param, LLVMBasicBlockRef* head_out,
                                    LLVMValueRef* n_out);

    /* ---- 聚合 */
    V array_literal(const sema::HNode& node);
    V empty_array(const TyRef& arr_ty, int line);
    V composite_literal(const sema::HNode& node);

    /* ---- 状态（来自 `Hir`；HIR 已经算好，这里只存 LLVM 句柄） */
    const sema::Hir& hir_;
    std::map<std::string, LLVMTypeRef> struct_ll_;
    std::map<std::string, std::pair<LLVMValueRef, TyRef>> globals_; /* 全局名 → (全局, 类型) */
    std::map<int, V> str_pool_;                                    /* `Hir.strings` 下标 → 常量值 */
    /* `Hir.consts` 下标（非整数常量）→ 已发的常量值。引用 N 次只发一枚全局 —— 旧路径按声明
     * 节点备忘，没有它产物里会多出同名全局。 */
    std::map<int, V> const_pool_;
    int array_serial_ = 0;
    std::map<std::string, LLVMValueRef> helpers_;
    /* 前缀函数 / 外部符号的缓存（键形如 `prefix:get_time` / `extern:Sleep`）：同一个符号只声明一次。 */
    std::map<std::string, LLVMValueRef> fns_;
    std::vector<LLVMValueRef> fnrefs_; /* `Hir.funcs` 下标 → 已发的函数（顶层实例） */
    /* 嵌套 def 的实例：键 = (owner, `Hir.funcs` 下标)。旧路径的键是 (当前函数逻辑名, 声明行,
     * def 名)，所以同一个嵌套 def 被两个不同外层调用会各发一份 —— 合并不行，产物会变小。 */
    std::map<std::pair<std::string, int>, LLVMValueRef> nested_fnrefs_;

    std::unique_ptr<Context> ctx_;
    std::unique_ptr<Module> mod_;
    LLVMTypeRef i64_ = nullptr;
    LLVMTypeRef ptr_ = nullptr;
    LLVMTypeRef void_ty_ = nullptr;
    std::map<unsigned, LLVMTypeRef> int_types_;

    int fn_seen_ = 0;
    std::vector<std::string> warnings_;
};

/* ---------------------------------------------------------------- Program */
class Program : public Codegen {
public:
    explicit Program(const sema::Hir& hir);

    /* 生成整个模块：程序根 → `tc_main`，其余实例在引用处按需发射。 */
    void build();
    std::string ir_text() const;
    std::vector<uint8_t> emit_object();

private:
    void make_types();
    void make_globals();
    void emit_main();
};

}  // namespace codegen
}  // namespace simplexc
