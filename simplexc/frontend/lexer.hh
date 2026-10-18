// 分词（M4.5 的 C++17 产品实现）。
//
// 语义 oracle = 冻结原型 `frontend/expr.py` 的 `tokenize` / `Tok` / `LexError`（只读）。
// token 里带「前面有没有空白」——那是消歧 `f(x)`（调用）与 `T (x)`（转换）的唯一依据。
//
// 列号按**码点**计（原型是 Python str，列号也是码点）：语料里有 CJK（`label == '内存'`），
// 按字节计会让同一行后面的 token 列号偏移 —— 错误消息要逐字对得上，所以不能省。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace simplexc {

struct Tok {
  std::string kind;  /* ident | hash | at | dollar | number | string | template | op | eof */
  /* hash / at / dollar 的 text 只装 sigil 后面的名字（`#X` / `@X` / `$x`），sigil 本身不进 text */
  std::string text;  /* ident/number/op 是原文；string/template 是定界符之间的内容 */
  int line = 0;      /* 1-based */
  int col = 0;       /* 1-based，按码点 */
  bool gap = true;   /* 与上一个 token 之间是否有空白 */
  std::string quote; /* string/template 的定界符：'"' / '\'' / '`' */
  bool interp = false; /* 模板里是否有 `{…}` 插值 */
};

struct LexError {
  int line = 0;
  int col = 0;
  std::string msg;
};

/* 末尾补一个 eof token。未闭合字面量 / 不认识的字符 → 抛 LexError。 */
std::vector<Tok> tokenize(std::string_view text);

/* 词法器内部的两个小工具，公开给 parser 拆模板插值用：模板 token 的文本是**原文**
   （词法器不拆 `{…}`），parser 要自己找配对的花括号，那需要同一套「引号扫到哪」与「码点怎么数」。 */
std::size_t scan_quoted_at(std::string_view text, std::size_t i);
int code_points_between(std::string_view text, std::size_t b, std::size_t e);

std::string describe(const Tok& t);

}  // namespace simplexc
