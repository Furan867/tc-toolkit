/* codegen/module_source.hh —— `ModuleAst` → `sema::ModuleSource`（**过渡态的桥**）。
 *
 * 两个载体同形（`label` / `defs` / `top_vars` / `enums` / `structs` + `source_name`），差别只在
 * 类型的名字与所在的命名空间：`ModuleAst`（`module_ast.hh`），`ModuleSource` 在 `sema`
 * （`sema/` 不反向 include 本目录，方案 §1）。所以这里是纯字段搬运，**不读源码文本** ——
 * `type` 声明的字段与成员已经由 `module_build.cc` 切好，两边拿到的是同一批 `TypeRef` / `FunctionDef`。
 * 生命周期照旧：AST 节点不归谁所有，调用方（`OwnedModule`）要活到 `analyze` 与 codegen 之后。
 *
 * 为什么放在这里而不是两个调用方里：`cli/` 与 `shell/` 都要它，而它与它依赖的 `ModuleAst` 都是
 * 过渡物 —— 下一步该让 `frontend` 直接出 `ModuleSource`，那时这个桥与 `module_ast.hh` 一起消失。
 * 过渡物不落在任何一方的目录里，否则删它的时候要跨两个调用方。
 */
#pragma once

#include "../sema/analyze.hh"
#include "module_ast.hh"

namespace simplexc {
namespace codegen {

inline sema::ModuleSource module_source_of(const lower::ModuleAst& ast) {
    sema::ModuleSource out;
    out.label = ast.label;
    out.defs = ast.defs;
    out.dot_units = ast.dot_units;
    out.top_vars = ast.top_vars;
    out.extern_libs = ast.extern_libs;
    out.source_name = ast.source_name;
    for (const lower::EnumDecl& e : ast.enums) {
        out.enums.push_back(sema::EnumSource{e.name, e.members});
    }
    for (const lower::StructDecl& s : ast.structs) {
        out.structs.push_back(sema::StructSource{s.name, s.fields});
    }
    for (const lower::AliasDecl& a : ast.aliases) {
        out.aliases.push_back(sema::AliasSource{a.name, a.target});
    }
    return out;
}

}  // namespace codegen
}  // namespace simplexc
