/* module_build.hh —— 前端产物（声明表 + 已解析 def）→ `ModuleAst`：壳与 CLI 共用的那一段粘合。
 *
 * 为什么这一层存在
 * ----------------
 * `lower::Program` 拿的是**已解析**的 `ModuleAst`（`label`/`defs`/`top_vars`/`enums`/`structs` 都由调用方填），
 * 而源文件到它之间还差一段：`_scan_decls` / `_scan_type` / `parse_decl_stmt`（原型的三个扫描步骤）。
 * 它既不属于 lowering（lowering 只认类型化 AST），也不属于 parser（parser 只认 token）——
 * 所以放在本目录里单独一个文件，**壳（`shell/simplexc_core.cc`）与 CLI（`cli/simplexc_cli.cc`）
 * 都调这一份**，不许各留一份实现（接口冻结版 §0 的「一处定义」）。
 *
 * 与冻结原型的差别只在于：那边用正则，这边手写扫描 ——
 * 生成代码子集里 type 声明只有 `type X Enum [a, b]` 与 `type X Struct { f: T, … }` 两种形态。
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../frontend/ast.hh"
#include "../frontend/frontend.hh"
#include "../sema/errors.hh"
#include "module_ast.hh"

namespace simplexc {
namespace lower {

/* `build_module` 的失败类型来自 `sema/errors.hh`（错误模型只有那一处定义）。用 using 引入本
 * 命名空间是为了让调用方照旧写 `lw::Unsupported` / `lw::CompileFail` —— 它们 `catch` 的就是
 * 这一层抛出来的东西，不该逼调用方去记类型住在哪个命名空间。 */
using sema::CompileError;
using sema::CompileFail;
using sema::Responsibility;
using sema::Span;
using sema::Stage;
using sema::Unsupported;

/* `ModuleAst` 只持裸指针（它的注释写明「调用方保证生命周期」），所以整棵树由这里持有。
 * 不可拷贝（`unique_ptr` 成员），可移动 —— 移动的是 unique_ptr 与指针数组本身，
 * 节点地址不变，`ast` 里的裸指针因此仍然有效。 */
struct OwnedModule {
  ModuleAst ast;
  std::vector<std::unique_ptr<FunctionDef>> fns;
  std::vector<std::unique_ptr<VarDeclStmt>> vars;
  std::vector<std::unique_ptr<TypeRef>> refs;

  OwnedModule() = default;
  OwnedModule(OwnedModule&&) = default;
  OwnedModule& operator=(OwnedModule&&) = default;
  OwnedModule(const OwnedModule&) = delete;
  OwnedModule& operator=(const OwnedModule&) = delete;

  FunctionDef* keep(std::unique_ptr<FunctionDef> fn);
  const TypeRef* keep_type(std::unique_ptr<TypeRef> t);
};

/* 按 `fe.decls` 拼出 `ModuleAst`。**只解析可达 def** —— 不可达的 def 里可能有语法错
 * （`rv32im.si:154` 的多余 `)`），原型也不解析它们；`Frontend` 已经算好 `live`。
 * `source_name` 只进报错文本（`ModuleAst::source_name`）。
 * 失败与 CLI 一致：抛 `lower::Unsupported`（没实现的构造）/ `lower::CompileFail`（内部失败）
 * / 前端自己的 `ParseError`。 */
OwnedModule build_module(const Frontend& fe, std::string source_name);

}  // namespace lower
}  // namespace simplexc
