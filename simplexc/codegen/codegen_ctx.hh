/* codegen/codegen_ctx.hh —— HIR codegen 的**函数级**状态：值表示 `V`、名字绑定 `Binding`、
 * 每函数一份的上下文 `FnCtx`。
 *
 * 与旧 AST 路径的 `fn_ctx.hh`（随旧 lowering 一起删了）的差别只有一处，但这处是本质的：`Binding` 里没有
 * `const DefStmt*`——HIR 里「按名调一个 def」已经变成 `Call{FnId}`，名字绑定只剩「有地址的局部/
 * 捕获」与「编译期常量」两种。另加 `FnCtx.captures`：HIR 的 `Capture` 节点只有序号，没有名字，
 * 所以捕获按**下标**绑（`HirFunc.caps` 的顺序 = 闭包环境槽位顺序，见 `sema/hir.hh` 的 `HCapture`）。
 *
 * 为什么每函数一份上下文、按栈进退：按需发函数体是递归的（发外层时发现新实例就要当场发它的体），
 * 内层的 builder / 基本块 / 作用域会盖掉外层的。进函数压一个、出函数弹掉，于是「忘了恢复某个
 * 字段」写不出来（设计 §3.3）。
 */
#pragma once

#include "../sema/types.hh"

#include <llvm-c/Core.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace simplexc {
namespace codegen {

/* 表达式的 lowering 结果：i64 位模式值（或 struct 聚合 / 真指针）+ 类型 + 可选的位模式常量。
 * `c` 只由 `HKind::Const` 节点填（sema 已折好、已按 `ty` 截断），其余指令值一律 `nullopt`。 */
struct V {
    LLVMValueRef ll = nullptr;
    sema::TyRef ty;
    std::optional<uint64_t> c;

    static V instr(LLVMValueRef v, sema::TyRef t) { return V{v, std::move(t), std::nullopt}; }
    static V konst(sema::TyRef t, uint64_t c) { return V{nullptr, std::move(t), c}; }
};

/* 一个名字在**本函数作用域表**里绑到了什么。`Local`/`Capture` 有地址，`Const` 只有值。
 * 捕获也进作用域表（按名字），另外按序号存一份 `FnCtx.captures` 供 `HKind::Capture` 用 ——
 * 一份给「按名字读」（旧 AST 的写法），一份给「按序号读」（HIR 的写法），两者指同一个 alloca。 */
struct Binding {
    enum class Kind { Local, Capture, Const } kind = Kind::Local;
    LLVMValueRef ptr = nullptr;
    sema::TyRef ty;
    std::optional<V> value;

    static Binding local(LLVMValueRef p, sema::TyRef t) {
        Binding b;
        b.kind = Kind::Local;
        b.ptr = p;
        b.ty = std::move(t);
        return b;
    }
    static Binding capture(LLVMValueRef p, sema::TyRef t) {
        Binding b;
        b.kind = Kind::Capture;
        b.ptr = p;
        b.ty = std::move(t);
        return b;
    }
    static Binding constant(V v) {
        Binding b;
        b.kind = Kind::Const;
        b.value = std::move(v);
        return b;
    }
};

/* 一层词法作用域。外层在前、内层在后；`Codegen::lookup` 从后往前找。 */
using Scope = std::map<std::string, Binding>;

struct FnCtx {
    LLVMBuilderRef bd = nullptr;
    LLVMValueRef fnref = nullptr;
    sema::TyRef ret_ty;
    LLVMBasicBlockRef block = nullptr;
    LLVMBasicBlockRef entry = nullptr;
    std::vector<Scope> scopes;
    std::vector<std::pair<std::string, LLVMBasicBlockRef>> breaks; /* label（空 = 无标签）→ 出口块 */
    /* `HirFunc.caps` 按序绑进来（形参之后的位置）；`HKind::Capture` 用 `capture_index` 取。 */
    std::vector<Binding> captures;
    std::string name;
    /* 这个上下文是不是**顶层 def 的实例**：嵌套 def 的产物名取 `owner + "$" + …`，其中 owner
     * 现状取「外层如果是个顶层 def 实例就用它的名字，否则用 `closure`」（`lower.cc` 的
     * `ensure_nested_fn`）。程序根、前缀函数、内联辅助函数都不是 def 实例。 */
    bool is_top_def = false;
    /* 调试信息：本函数的 DISubprogram（`set_loc` 的 scope）与当前行。没开调试信息时两者
     * 恒为空/0；`dbg_line` 让内联辅助函数能沿用调用点的行，而不是记成一个假行号。 */
    LLVMMetadataRef dbg_scope = nullptr;
    int dbg_line = 0;
    const FnCtx* parent = nullptr;
};

}  // namespace codegen
}  // namespace simplexc
