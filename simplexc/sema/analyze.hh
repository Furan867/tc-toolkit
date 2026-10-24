/* sema/analyze.hh —— 已解析的模块 → `Hir`（sema 的入口；②b-1 只跑通最小闭环，**不接线**）。
 *
 * 输入的载体为什么不叫 `ModuleAst`：那个名字在 `lower.hh` 里（`label`/`defs`/`top_vars`
 * /`enums`/`structs`），而 `sema/` 不许 include `lower.hh`（方案 §1 的依赖方向）。这里放的是
 * sema 自己的一份**最小同形载体**：本步只认程序根与顶层 var/const，`enums`/`structs` 留给
 * 处理 `type` 声明的那一步（在那之前它们只会被当作 `Unsupported`）。字段的指针语义与
 * `ModuleAst` 相同：**节点不归本模块所有**，调用方负责让它们在 `analyze` 期间有效。
 */
#pragma once

#include <string>
#include <vector>

#include "hir.hh"

namespace simplexc {

/* 前端的 AST 节点（定义在 `frontend/ast.hh`）。这里只前向声明：sema 的头里除了消费 AST 的
 * 实现文件以外不许碰 `frontend/`，而输入只需要三个裸指针。 */
struct FunctionDef;
struct VarDeclStmt;

namespace sema {

struct ModuleSource {
    const FunctionDef* label = nullptr;  /* 程序根（`run_sim:`，`form == "label"`）；空 → CompileFail */
    std::vector<const FunctionDef*> defs;
    std::vector<const VarDeclStmt*> top_vars;  /* 顶层 var / const，按源码顺序 */
    std::string source_name;                   /* 只进报错文本 */
};

/* 失败抛 `sema::Unsupported`（没实现的构造，带源码行号）或 `sema::CompileFail`（内部失败）；
 * 与 `lower::Program` 同一种分型，两轴之外的映射（阶段/责任）是步③ 的事。
 * 本步只收：程序根（含 `while`/`break`）+ 顶层 var/const + 整数/布尔/标识符/一元/二元/比较/
 * `Cond`/`cast` 这些表达式与 `VarDecl`/`Assign`/`ExprStmt`/`Return`/`Block`/`While`/`Break`；
 * 其余的形态一律显式 `Unsupported`（不猜语义）。 */
Hir analyze(const ModuleSource& src);

}  // namespace sema
}  // namespace simplexc
