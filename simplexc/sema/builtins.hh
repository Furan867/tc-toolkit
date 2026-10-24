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
 * 这张表**不是**「源码里所有内建」的清单 —— 只是本步已经接上的那些。其余（`size` 已直接折成
 * `Const`；`load`/`store`/`raw_cast` 之外的内建体在 codegen 侧）在 sema 里显式抛
 * `CompileError{Unimplemented, Sema, …}`，不猜语义、也不静默降级。
 *
 * 操作数（`kids`）的形状逐条跟着 `lower/lower.cc` 的 `builtin_*` 走：
 *   Load / Store / RawCast   见各自的声明处
 *   Asr(x, n)                kids = [x, n→x.ty]；饱和与 `ashr` 是 codegen 的事
 *   Clz(x)                   kids = [x]；数的是**操作数声明位宽**里的前导零
 *   Ror(x, n)                kids = [x, n]；两条路的旋转量口径不同，见 `call_ror`
 *   Min(a, b) / Max(a, b)    kids = [a, b]，已经 `unify` + 两侧 `convert` 过
 *   Array(<T>, n)            kids = [Const n]；`ty` 是 `Arr[T]`，每调用点一枚私有全局
 *   Random([m])              kids = [Global 种子] 或 [Global 种子, m] —— 种子的全局 id 必须进
 *                            kids：HIR 的引用一律是已解析的编号（`hir.hh` 形状决定 3），
 *                            不许 codegen 按名字 `global_seed` 去查表
 *   Len(obj)  / Ms(obj)      kids = [obj]；方法糖（`.len()` / `.ms()`）的落点，HIR 里不留方法名
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
    /* 以下七个是方案 §2f 冻结的追加编号（3..9），语义逐条照 `lower.cc` 的 `builtin_*`。 */
    Asr = 3,      /* `asr(x, n)`：按 W 位符号位算术右移，n ≥ W 时饱和。结果类型 = x 的类型 */
    Clz = 4,      /* `clz(x)`：数**操作数声明位宽**里的前导零 */
    Ror = 5,      /* `ror(x, n)`：循环右移（常量折叠与运行期两条路口径不同，见 `call_ror`） */
    Min = 6,      /* `min(a, b)`：窄侧类型规则照 `builtin_minmax`（先 `unify` 再两侧 `convert`） */
    Max = 7,
    Array = 8,    /* `array(<T>, n)`：返回 `Arr[T]`，每个调用点一枚私有全局（头 + 零载荷） */
    Random = 9,   /* `random([m])`：前缀的 xorshift 递推，状态就是 `global_seed` 那枚全局 */
    /* 10/11 是方法糖消解出来的两个（方案 §2f 的追加段，2026-10-24 主程裁定）：HIR 里**不留方法名**，
     * 但也不把它们拆成已有节点 —— `.len()` 拆开就是 `inttoptr/ptrtoint` 算术，`gep -8` 的 IR 形状
     * 就没了，而「产物逐字节相同」是主判据，不能靠 O2 折叠去赌。 */
    /* `.len()`（`lower.cc` 的 `call_method` → `len_of`）：结果 `I64`，kids = `[obj]`（`Str` 或 `Arr`）。
     * 语义：读 `p[-8]` 那 8 字节头再 `>> 16`（头是 `len << 16`）。接收者类型只允许 Str/Arr。 */
    Len = 10,
    /* `.ms()`（`call_method` 的 `.ms` 分支）：结果 `Time`（与 `U64` 同类型），kids = `[obj]`（**已算出的
     * Int 值**，不预 convert）。语义：`obj * 1000000`（纳秒）。常量折不折由 codegen 定（现状 lower
     * 在常量时折、且折的是 `pat_const` 那个按源类型符号性扩过的 64 位值 —— 折法只有一处，归 codegen）。 */
    Ms = 11,
};

/* dump 与诊断用的拼写；`hir_dump` 只用编号，这个名字是给人看的。 */
inline const char* hbuiltin_name(HBuiltin b) {
    switch (b) {
        case HBuiltin::Load: return "load";
        case HBuiltin::Store: return "store";
        case HBuiltin::RawCast: return "raw_cast";
        case HBuiltin::Asr: return "asr";
        case HBuiltin::Clz: return "clz";
        case HBuiltin::Ror: return "ror";
        case HBuiltin::Min: return "min";
        case HBuiltin::Max: return "max";
        case HBuiltin::Array: return "array";
        case HBuiltin::Random: return "random";
        case HBuiltin::Len: return "len";
        case HBuiltin::Ms: return "ms";
    }
    return "?";
}

}  // namespace sema
}  // namespace simplexc
