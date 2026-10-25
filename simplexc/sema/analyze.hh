/* sema/analyze.hh —— 已解析的模块 → `Hir`（sema 的入口；**不接线**）。
 *
 * 输入的载体为什么不叫 `ModuleAst`：那个名字在 `lower.hh` 里（`label`/`defs`/`top_vars`
 * /`enums`/`structs`），而 `sema/` 不许 include `lower.hh`（方案 §1 的依赖方向）。这里放的是
 * sema 自己的一份**最小同形载体**。字段的指针语义与 `ModuleAst` 相同：**节点不归本模块所有**，
 * 调用方负责让它们在 `analyze` 期间有效。
 *
 * `type` 声明（enum / struct）为什么也在这里而不在 AST：`frontend/ast.hh` 只管语句与表达式，
 * 顶层 `type` 只有声明表（`frontend/decls.hh` 的 `Decl`：名字 + 行范围 + 一个 `note`），字段与
 * 成员要从源码文本里切（`codegen/module_build.cc` 的 `scan_type` 干这件事，②c 之后只剩一处）。
 * sema 只吃**切好的形状**：字段类型仍是源码拼写（`TypeRef`），解析在 `make_types` 里。
 */
#pragma once

#include <string>
#include <vector>

#include "hir.hh"

namespace simplexc {

/* 前端的 AST 节点（定义在 `frontend/ast.hh`）。这里只前向声明：sema 的头里除了消费 AST 的
 * 实现文件以外不许碰 `frontend/`。 */
struct FunctionDef;
struct VarDeclStmt;
struct TypeRef;

namespace sema {

/* 一个 struct 声明：字段名 + 类型拼写（按声明序）。 */
struct StructSource {
    std::string name;
    std::vector<std::pair<std::string, const TypeRef*>> fields;
};

/* 一个 enum 声明：成员名按声明序 = 序号。 */
struct EnumSource {
    std::string name;
    std::vector<std::string> members;
};

/* 一个 `type X <类型拼写>` 别名：右手仍是源码拼写，按**用点**解（所以不看声明顺序）。 */
struct AliasSource {
    std::string name;
    const TypeRef* target;
};

struct ModuleSource {
    const FunctionDef* label = nullptr;  /* 程序根（`run_sim:`，`form == "label"`）；空 → `CompileError{OurBug,…}` */
    std::vector<const FunctionDef*> defs;
    std::vector<const VarDeclStmt*> top_vars;  /* 顶层 var / const，按源码顺序 */
    std::vector<EnumSource> enums;
    std::vector<StructSource> structs;
    std::vector<AliasSource> aliases;
    /* `extern windows_x64 kernel32` 登记的外部库名（声明本身不给签名，只给「这是库」）。 */
    std::vector<std::string> extern_libs;
    std::string source_name;                   /* 只进报错文本 */
};

/* 失败抛 `sema::CompileError`（三轴：责任 / 阶段 / span，见 `errors.hh` 与方案 §2d）。
 * `sema/` 已无旧分型（S3-B）—— 本模块的每一个抛点都是 `CompileError`。
 *
 * 本步收：程序根（`while`/`if`/`#if`/`switch`/`break`）+ 顶层 var/const + `type` 声明（enum /
 * struct）+ 整数/布尔/字符串/模板/标识符/一元/二元/比较/三目/`cast`/数组字面量/复合字面量/
 * 下标/字段这些表达式与 `VarDecl`/`Assign`/`ExprStmt`/`Return`/`Block`/`While`/`If`/`Switch`/
 * `Break`；`in_scope(…)` 在 sema 里判定并剪枝；**实例闭包**（顶层 def 调用与同名重载、泛型的
 * 两种类型实参写法、嵌套 def 与捕获、递归与互调、不收敛时按 64 上限报错）；`Str` 拼接与
 * 转义字符串（U-L10 的表）。**没做的**（内建与前缀函数的体、方法调用、`$` 参数、`asm`）
 * 一律显式报错，不猜语义。 */
Hir analyze(const ModuleSource& src);

}  // namespace sema
}  // namespace simplexc
