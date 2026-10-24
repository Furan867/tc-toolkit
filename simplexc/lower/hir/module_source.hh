/* lower/hir/module_source.hh —— `ModuleAst` → `sema::ModuleSource`（**过渡态的桥**）。
 *
 * 两个载体同形（`label` / `defs` / `top_vars` / `enums` / `structs` + `source_name`），差别只在
 * 类型的名字与所在的命名空间：`ModuleAst` 在 `lower`（旧路径的入口），`ModuleSource` 在 `sema`
 * （`sema/` 不反向 include `lower/`，方案 §1）。所以这里是纯字段搬运，**不读源码文本** ——
 * `type` 声明的字段与成员已经由 `module_build.cc` 切好，两边拿到的是同一批 `TypeRef` / `FunctionDef`。
 * 生命周期照旧：AST 节点不归谁所有，调用方（`OwnedModule`）要活到 `analyze` 与 codegen 之后。
 *
 * 为什么放在这里而不是两个调用方里：`cli/` 与 `shell/` 都要它，而它依赖的 `ModuleAst` 与它自己
 * 一样是删 `lower/` 时一起消失的过渡物 —— 与旧路径同寿，所以不落在任何一方的目录里。
 */
#pragma once

#include "../../sema/analyze.hh"
#include "../lower.hh"

namespace simplexc {
namespace codegen {

inline sema::ModuleSource module_source_of(const lower::ModuleAst& ast) {
    sema::ModuleSource out;
    out.label = ast.label;
    out.defs = ast.defs;
    out.top_vars = ast.top_vars;
    out.source_name = ast.source_name;
    for (const lower::EnumDecl& e : ast.enums) {
        out.enums.push_back(sema::EnumSource{e.name, e.members});
    }
    for (const lower::StructDecl& s : ast.structs) {
        out.structs.push_back(sema::StructSource{s.name, s.fields});
    }
    return out;
}

}  // namespace codegen
}  // namespace simplexc
