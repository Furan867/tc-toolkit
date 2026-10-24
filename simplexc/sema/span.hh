/* sema/span.hh —— 源码位置（原先定义在前端 `ast.hh` 里，两个字段一个没改）。
 *
 * 为什么在 sema：HIR 的每个节点都要带 span（不变量 ⑤），而 `sema/` 不许 include 前端
 * （主设计 §3.2「codegen 不查 AST/源码」的同一条边界）。前端的 `ast.hh` 用 `using sema::Span;`
 * 把它引回 `simplexc::Span`，前端与 lowering 的写法一行没变。
 *
 * `col` 现在恒为 0：前端（parser/lexer）只记行号，没有列号可填。所以「带 span」的判据是
 * `line >= 1`，不是 `col >= 1` —— 别拿 col 当「有没有 span」的标志。 */
#pragma once

namespace simplexc {
namespace sema {

struct Span {
    int line = 0;
    int col = 0;
};

}  // namespace sema
}  // namespace simplexc
