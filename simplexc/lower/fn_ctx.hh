/* fn_ctx.hh —— lowering 的**函数级**状态：值表示 `V`、名字绑定 `Binding`、作用域 `Scope`、
 * 每个正在生成的函数一份的上下文 `FnCtx`。
 *
 * 为什么 `FnCtx` 是独立对象、按栈进退（而不是 `Lower` 的 10 个成员 + 手工存取）
 * ------------------------------------------------------------------------
 * 按需生成会**递归**：发外层函数体的过程中要发它调用的函数体，内层的 builder / 基本块 /
 * 作用域表会盖掉外层的。以前是「进函数存一份快照、出函数再拷回来」逐字段手工搬 ——
 * 加一个字段要改 3 处，漏一处就是「回到外层后 block 或作用域还是内层的」这种错序 bug
 * （设计 §3.3：已经出过两起）。现在每个函数一个 `FnCtx`：进函数时新压一个、出函数时弹掉，
 * 于是「忘了恢复某个字段」不再是**可能写错**的代码，而是**写不出来**的代码。
 *
 * 只放**每函数一份**的东西。跨函数共享的表（`fns_` / `structs_` / `globals_` / `defs_` /
 * `helpers_` / `cap_instances_` …）留在 `Lower`：它们是模块级的，塞进这里会让「一份还是多份」
 * 由注释而不是由结构决定。
 *
 * 为什么 header-only：`simplexc/tools/build.py` 的源文件表是**显式清单**（不是 glob），
 * 新加 `.cc` 就得改它 —— 那属于 `tools/` 写者分区，本步不动。
 */
#pragma once

#include "../frontend/ast.hh"
#include "../sema/types.hh"

#include <llvm-c/Core.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace simplexc {
namespace lower {

/* 表达式的 lowering 结果：i64 位模式值（或 struct 聚合）+ 类型 + 可选的编译期常量。 */
struct V {
    LLVMValueRef ll = nullptr;
    sema::TyRef ty;
    std::optional<uint64_t> c;

    static V instr(LLVMValueRef v, sema::TyRef t) { return V{v, std::move(t), std::nullopt}; }
    static V konst(sema::TyRef t, uint64_t c) { return V{nullptr, std::move(t), c}; }
};

/* 一个名字在本函数的作用域里绑到了什么。`Local`/`Capture` 有地址，`Const`/`Def` 没有。 */
struct Binding {
    enum class Kind { Local, Capture, Const, Def } kind = Kind::Local;
    LLVMValueRef ptr = nullptr;      /* Local / Capture：地址 */
    sema::TyRef ty;                  /* Local / Capture */
    std::optional<V> value;          /* Const */
    const DefStmt* def = nullptr;    /* Def */

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
    static Binding nested(const DefStmt* d) {
        Binding b;
        b.kind = Kind::Def;
        b.def = d;
        return b;
    }
};

/* 一层词法作用域。外层在前、内层在后；`Lower::lookup` 从后往前找。 */
using Scope = std::map<std::string, Binding>;

struct FnCtx {
    LLVMBuilderRef bd = nullptr;
    LLVMValueRef fnref = nullptr;
    sema::TyRef ret_ty;
    LLVMBasicBlockRef block = nullptr;   /* 当前块 */
    LLVMBasicBlockRef entry = nullptr;
    std::vector<Scope> scopes;
    std::vector<std::pair<std::string, LLVMBasicBlockRef>> breaks;   /* label（空 = 无标签）→ 出口块 */
    std::string name;
    const FunctionDef* def = nullptr;
    std::map<std::string, sema::TyRef> ty_binds;
    /* 外层函数。基座上下文（`Lower` 构造时压的那个）的 `parent` 为空 —— `finish_function`
     * 靠这一点拒绝把基座弹掉。 */
    const FnCtx* parent = nullptr;
};

}  // namespace lower
}  // namespace simplexc
