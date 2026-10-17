// AST → JSON / 人读树（M4.5 的 C++17 产品实现）。
//
// 语义 oracle = 冻结原型 `frontend/expr.py` 的 `node_to_dict` / `render_tree`（只读），
// 只给判据用（`frontend/ast_dump_main.cc`），不进产品路径。
//
// 两处必须照抄原型的地方：
//   1. `LiteralExpr` 的字段名 `kind` 与「节点类别名」**撞车** → 原型 `node_to_dict` 里字段覆盖
//      类别，于是 JSON 的 `kind` 是字面量类别（"int"/"str"/"template"），类别名只在人读树里出现。
//   2. `TypeRef` 在原型里是一种 Node（有 src_line、也进 `node_kinds`），但冻结的 `ast.hh` 没让它
//      继承 Node → JSON/人读树都要给它单独一条，好让输出与原型一致。
#pragma once

#include <string>

#include "ast.hh"
#include "json.hh"

namespace simplexc {

Json node_to_json(const Node* n);
Json type_ref_to_json(const TypeRef* t);

std::string render_tree(const Node* n, int indent);
std::string render_type_ref(const TypeRef* t, int indent);

}  // namespace simplexc
