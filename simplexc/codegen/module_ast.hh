/* module_ast.hh —— 前端 → sema/codegen 的输入载体：声明表 + 已解析的 def（`ModuleAst`）。
 *
 * 为什么这一层单独有名字：`ast.hh` 没有顶层 `type` 声明的节点（它只管语句与表达式），
 * 所以 `EnumDecl` / `StructDecl` 在这里给两个最小载体，字段类型仍用 `TypeRef` 的**拼写**表示；
 * `ModuleAst` 再把声明表与已解析的 `def` / 顶层 `var` 捆成一份交给下一层。
 * 填它的是 `module_build.{hh,cc}`（壳与 CLI 共用那一份）。
 *
 * **AST 节点不归本模块所有**：`ModuleAst` 只持裸指针，调用方必须保证它们在 `ModuleAst`
 * 活着期间有效 —— 前端把整棵树一次解析完、`OwnedModule` 持有着它，天然满足这点。
 */
#pragma once

#include "../frontend/ast.hh"

#include <string>
#include <utility>
#include <vector>

namespace simplexc {
namespace lower {

struct StructDecl {
    std::string name;
    std::vector<std::pair<std::string, const TypeRef*>> fields;
};

struct EnumDecl {
    std::string name;
    std::vector<std::string> members;   /* 按声明序 = 序号（sema 把它们当整数常量） */
};

/* `type X <类型拼写>`：右手是源码拼写（`U64` / `Arr[Char]`），解析在 sema。 */
struct AliasDecl {
    std::string name;
    const TypeRef* target;
};

struct ModuleAst {
    const FunctionDef* label = nullptr;          /* 程序根（`form == "label"`）；空 → CompileFail */
    std::vector<const FunctionDef*> defs;        /* 顶层 def */
    std::vector<const VarDeclStmt*> top_vars;    /* 顶层 var / const / let */
    std::vector<EnumDecl> enums;
    std::vector<StructDecl> structs;
    std::vector<AliasDecl> aliases;
    std::string source_name;                     /* 只进报错文本 */
};

}  // namespace lower
}  // namespace simplexc
