/* sema/hir.hh —— HIR：AST 的**去糖投影 + 显式 cast + 已解析引用**。
 *
 * 规格是 `docs/notes/2026-10-24-refactor-plan.md` §2b（冻结），节点集一条不多、一条不少；
 * 语法糖（`#if`、`const #`、`let/var` 的区别、体内 `def` 声明、`if_case` 容器）在 sema 之前就没了。
 * 为什么不复用 AST：不变量 ④ 要求隐式转换显式化（要能插节点），而 codegen 不许再看源码/AST。
 * 为什么不做三地址码：主设计 §3.2 —— 后端的中间表示本身就是 MIR。
 *
 * 四条形状上的决定（都是「值语义」这一个要求的直接后果，改之前先想清楚下游谁在依赖它们）：
 *   1. **一个 HNode 结构 + kind 判别**，不是继承、没有虚函数：可拷贝、`std::vector<HNode>` 存子树，
 *      dump / 自检 / 不变量检查都不用管所有权。代价是每个节点带着别的 kind 才用的字段。
 *   2. 表达式节点的 `ty` 必须非空、语句节点必须为空 —— 「没有值类型」只有这一种写法。
 *      唯一例外是 `VarDecl`：它的 `ty` 是**被声明者的类型**（语句确实没有值，但 codegen 的
 *      alloca 要知道宽度，而规格表给语句那几行没列任何载荷位 —— 不给这个例外就无处可放）。
 *      别把这个例外扩到其它语句。
 *   3. 引用一律用**已解析的编号**：局部用名字（决策 3，让 alloca 顺序/命名不变），
 *      全局/捕获/串池/实例用下标，不给指针、不留源码拼写。
 *   4. 载荷只写有语义的那几个字段（`label` 是唯一的例外，见 While/Break）：规格表里语句那几行
 *      没列载荷，这里的载荷是从 `lower/lower.cc` 的实际语义反推的，不添没有消费者的字段。
 *
 * 六条不变量（`check_invariants` 逐条单独报编号；生产路径将来在 sema 之后 assert）：
 *   ① 每个表达式节点的 `ty` 非空、每个语句节点的 `ty` 为空（`VarDecl` 除外，见形状决定 2）；
 *      `HirFunc.ret` 非空、`body` 是 Block。
 *   ② `Local` 指向作用域里已存在的声明、`Global` 的 id 在 `globals` 里、`Capture` 的序号在 `caps` 里。
 *   ③ `Call` 的 `Func` 被调者一定已在 `funcs` 里（实例闭包已收敛）。
 *   ④ 无隐式转换：宽度/符号/种类的变化必须已经是 `Cast` 节点（查的范围见 `check_invariants` 的实现）。
 *   ⑤ 每个节点都带 span（`line >= 1`；`col` 前端从不填，见 `span.hh`）。
 *   ⑥ `funcs[].key` 唯一。
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "span.hh"
#include "types.hh"

namespace simplexc {
namespace sema {

/* ---------------------------------------------------------------- 节点类别
 * 前 15 个是表达式（有值），后 9 个是语句（无值）。`is_expr` 按这个分界答。 */
enum class HKind {
    // 表达式
    Const, StrLit, Local, Global, Capture, Unary, Binary, Cond, Call, Member, Index, Cast,
    ArrayLit, CompositeLit, Template,
    // 语句
    Block, VarDecl, Assign, ExprStmt, If, While, Switch, Break, Return,
};

/* 语句/表达式分界，不变量 ① 就落在这个判断上。 */
inline bool is_expr(HKind k) {
    switch (k) {
        case HKind::Const:
        case HKind::StrLit:
        case HKind::Local:
        case HKind::Global:
        case HKind::Capture:
        case HKind::Unary:
        case HKind::Binary:
        case HKind::Cond:
        case HKind::Call:
        case HKind::Member:
        case HKind::Index:
        case HKind::Cast:
        case HKind::ArrayLit:
        case HKind::CompositeLit:
        case HKind::Template:
            return true;
        case HKind::Block:
        case HKind::VarDecl:
        case HKind::Assign:
        case HKind::ExprStmt:
        case HKind::If:
        case HKind::While:
        case HKind::Switch:
        case HKind::Break:
        case HKind::Return:
            return false;
    }
    return false;
}

/* 一元：源码的 `+x` 是恒等、在 sema 里就没了，所以这里只有三种。`Not` 是逻辑非（结果 Bool，
 * 操作数按「非零为真」看，见 `lower.cc` 的 `as_cond`）。 */
enum class HUnOp { Neg, Not, BitNot };

/* 二元：算术/位运算/移位 + 比较 + 短路逻辑 + 按字节内容比较。查到这一步就不再是源码字符串
 * （决策 6 同一条理由）。
 *
 * `StrEq` / `StrNe` 是 `Str` 的 `==` / `!=`：它**不是** `Eq` 的一个参数，而是一种独立的语义
 * （参考实现走 `__simplexc_streq(ptr, ptr, n)`，先比长度再逐字节比；`lower.cc` 的 `str_compare`）。
 * 保留成独立的 op 就是为了让「按内容比」这个事实留在 HIR 里 —— codegen 见到 `StrEq` 不必再去
 * 查操作数类型猜该发哪条指令。`Str` 的 `<` 比大小参考自己就报错，所以没有对应的 op。 */
enum class HBinOp {
    Add, Sub, Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge, StrEq, StrNe,
    LogAnd, LogOr,
};

/* `Cast` 的种类 = 参考实现的 `convert` 分出来的那几种：窄化、符号扩展、零扩展、
 * 真指针转整数、整数转真指针，以及「只换类型标签的位重解释」（`Raw`，指针对整数、枚举等）。 */
enum class HCastKind { Trunc, Sext, Zext, PtrToInt, IntToPtr, Raw };

/* `Call` 的被调者：实例下标或内建编号（决策 6）。`Func` 的下标进 `Hir.funcs`，`Builtin`
 * 的编号属于 sema 的内建表（②b 才定表，所以这里只保证非负）。 */
enum class HCalleeKind { Func, Builtin };

struct HCallee {
    HCalleeKind kind = HCalleeKind::Func;
    int index = -1;
};

/* ---------------------------------------------------------------- 节点
 *
 * `kids` 的形状（每种 kind 只用自己的那一种）：
 *   Const / StrLit / Local / Global / Capture / Break      0
 *   Unary / Cast / Member / ExprStmt                      1（Member 的 kid 是取字段的对象）
 *   Binary / Index / Assign / While                       2（Assign: lhs 左值, rhs；While: 条件, 块）
 *   Cond                                                  3（条件, 真臂, 假臂）
 *   If                                                    2 或 3（条件, then 块[, else 块]）
 *   VarDecl / Return                                      0 或 1（初始化式 / 返回值）
 *   Block / ArrayLit                                      语句 / 元素按序
 *   CompositeLit                                          字段按**声明序**每个字段一个 kid（字面量里
 *                                                         没写的字段是它的零值 `Const`），所以
 *                                                         kids.size() == 字段数。为什么不按源码
 *                                                         书写序：那是**求值**顺序（U-L4），由
 *                                                         sema 在求值时保证，HIR 只描述聚合的形状
 *   Call                                                  实参按序（被调者在 `callee`，不在 kids 里）
 *   Template                                              文字段与插值**交替**：偶数下标是 StrLit 段
 *                                                         （允许空串）、奇数下标是表达式。文字段比
 *                                                         插值多一个，所以 kid 个数是**奇数**
 *                                                         （2×插值数 + 1）—— 末段文字不能省，
 *                                                         否则最后一个 `}` 之后的字节就丢了
 *   Switch                                                主体 + 各 case 的「[值表达式]? Block」：
 *                                                         紧跟一个表达式再跟 Block = `case 值:`，
 *                                                         直接出现 Block = `default:`（参考把
 *                                                         default 放到所有 case 之后跑，所以这里
 *                                                         也按「有值的 case 在前、default 在后」写）
 */
struct HNode {
    HKind kind = HKind::Const;
    Span span;
    TyRef ty; /* 表达式：值的类型；语句：为空（VarDecl 例外：被声明者的类型） */

    /* Const：已折叠、已按 `ty` 截断的位模式（决策 4 —— codegen 不再有 fold 分支）。 */
    uint64_t const_bits = 0;
    /* Local / VarDecl：局部名（决策 3 —— 不引入 slot 编号）。 */
    std::string name;
    /* While / Break：循环标签（`break run_sim`，语料里到处都是）。空 = 无标签。
     * 这是语句载荷里唯一一个「规格表没列但实情要」的字段：没有它，`break <标签>` 没法表达。 */
    std::string label;

    HUnOp un_op = HUnOp::Neg;
    HBinOp bin_op = HBinOp::Add;
    HCastKind cast_kind = HCastKind::Raw;
    HCallee callee; /* Call */

    int string_id = -1;     /* StrLit：`Hir.strings` 下标 */
    int global_id = -1;     /* Global：`Hir.globals` 下标 */
    int capture_index = -1; /* Capture：第几个捕获（`HirFunc.caps` 下标） */
    int field_index = -1;   /* Member：字段序号（按声明序） */
    int offset = -1;        /* Member：字段字节偏移（布局由 sema 定，codegen 不再算） */

    std::vector<HNode> kids;
};

/* ---------------------------------------------------------------- 顶层
 * `HirFunc.key` 是规范化实例键（决策 7）、`name` 是产物里的符号名；`main` 是程序根那个块
 * （没有实参也没有实例键，所以它的 key/name 可以是空串，不变量 ⑥ 只查 `funcs[]`）。 */
struct HParam {
    std::string name;
    TyRef ty;
};

struct HCapture {
    std::string name;
    TyRef ty;
    int index = -1; /* 闭包环境里的槽位序号 */
};

struct HirFunc {
    std::string key;
    std::string name;
    std::vector<HParam> params;
    std::vector<HCapture> caps;
    HNode body; /* 恒为 Block */
    TyRef ret;
};

/* `init_bits` 是硬缺口（方案 §2b 补漏 2）：全局初值必须是编译期常量，由 sema 折好放这里；
 * 少了它 codegen 只能把初值降级成程序根里的 store，产物就变了。`Const` 的约定同样适用：
 * 位模式已按 `ty` 截断（非 Int 类型按 64 位）。空 = 没有初值（本步不会出现）。 */
struct HirGlobal {
    std::string name;
    TyRef ty;
    std::optional<uint64_t> init_bits;
};

struct HirExtern {
    std::string name;
    TyRef ty;
};

struct HirString {
    std::string bytes;
    bool cstring = false; /* 是不是 `c'…'` 那条「末尾补 0」的形态 */
};

/* 一个 struct 的布局：`bits` 是整个聚合的位宽、`offset` 是字段的**字节**偏移。
 * 规则唯一实现是 `sema/layout.cc` 的 `layout_struct()`（`lower.cc` 原先自己算一份，②c 收口），
 * 依据 `docs/facts/semantics.md` 的 U-L4。codegen 不再算偏移，只查这张表。 */
struct HirField {
    std::string name;
    TyRef ty;
    int offset = 0;
};

struct HirStruct {
    std::string name;
    int bits = 0;
    std::vector<HirField> fields; /* 声明序 */
};

/* 已折叠的具名编译期常量（`const #SIMULATION_STATE = …`）：值已经在用到它的 `Const` 节点里，
 * 这张表只为「按名查值/诊断」活着。 */
struct HirConst {
    std::string name;
    TyRef ty;
    uint64_t bits = 0;
};

struct Hir {
    HirFunc main;
    std::vector<HirFunc> funcs;
    std::vector<HirGlobal> globals;
    std::vector<HirExtern> externs;
    std::vector<HirString> strings;
    std::vector<HirConst> consts;
    std::vector<HirStruct> structs;
};

/* 六条不变量全查一遍：成立返回 true；否则返回 false 并把「第几条 + 出问题的路径」写进 `err`
 * （格式 `不变量#N：…`）。**不 abort** —— 生产路径将来自己在 sema 之后 assert。
 * 实现和 `dump` 放在一起（`hir_dump.cc`）：两者都是同一趟只读遍历的两种输出。 */
bool check_invariants(const Hir& hir, std::string* err);

}  // namespace sema
}  // namespace simplexc
