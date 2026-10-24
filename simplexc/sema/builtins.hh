/* sema/builtins.hh —— 内建的编号表（方案 §2 决策 6：内建在 sema 解析成 `BuiltinId`，
 * HIR 里不留源码名字）。
 *
 * 为什么编号而不是名字：`HNode.callee` 只有 `{kind, index}` 两个位（`hir.hh`），内建的语义
 * （是否写内存、返回什么、对齐多少）在 sema 就定死，codegen 见到编号直接发指令，不必再查源码
 * 拼写、也不必再按操作数类型猜该发哪条。
 *
 * 编号是 `Hir` 的一部分（`Call callee=builtin:<index>` 会进 golden dump），**只许追加**：
 * 改动已有编号等于悄悄改掉所有 dump 的含义。
 *
 * 这张表**不是**「源码里所有内建」的清单 —— 只是本步已经接上的那些。其余（`asr`/`clz`/`ror`/
 * `min`/`max`/`array`/`random`/前缀函数/方法）在 sema 里显式抛
 * `CompileError{Unimplemented, Sema, …}`，不猜语义、也不静默降级。
 */
#pragma once

namespace simplexc {
namespace sema {

enum class HBuiltin {
    /* `load(<T>, p)` / `load(type_of(x), p)`：`ty` = 要读的类型（槽位宽度与对齐由 codegen 按它
     * 决定），唯一实参是地址。 */
    Load = 0,
    /* `store(p, v)`：结果类型是 `None`。 */
    Store = 1,
    /* `raw_cast(<T>, v)`：位重解释，等价于 `Cast{Raw}`，但源码形态是调用（`cv` 也认它）。 */
    RawCast = 2,
};

/* dump 与诊断用的拼写；`hir_dump` 只用编号，这个名字是给人看的。 */
inline const char* hbuiltin_name(HBuiltin b) {
    switch (b) {
        case HBuiltin::Load: return "load";
        case HBuiltin::Store: return "store";
        case HBuiltin::RawCast: return "raw_cast";
    }
    return "?";
}

}  // namespace sema
}  // namespace simplexc
