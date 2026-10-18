#include "lexer.hh"

#include <array>
#include <cstddef>

namespace simplexc {
namespace {

constexpr std::size_t kNone = std::string::npos;

bool is_word(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
bool is_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* 从 i 起吃一个 `[A-Za-z_][A-Za-z0-9_]*`；不成 → kNone。 */
std::size_t ident_at(std::string_view text, std::size_t i) {
  if (i >= text.size() || !is_ident_start(text[i])) return kNone;
  std::size_t j = i + 1;
  while (j < text.size() && is_word(text[j])) ++j;
  return j;
}

/* text[b,e) 的码点数（原型按 Python 字符计数，CJK 是一个码点三个字节）。 */
}  // namespace

int code_points_between(std::string_view text, std::size_t b, std::size_t e) {
  int n = 0;
  for (std::size_t k = b; k < e && k < text.size(); ++k) {
    if ((static_cast<unsigned char>(text[k]) & 0xC0) != 0x80) ++n;
  }
  return n;
}

/* 原型 `_scan_quoted`：定界符 → 闭合定界符之后；遇换行即截断；未闭合 = kNone。 */
std::size_t scan_quoted_at(std::string_view text, std::size_t i) {
  const char delim = text[i];
  const std::size_t n = text.size();
  std::size_t j = i + 1;
  while (j < n) {
    const char c = text[j];
    if (c == '\\') {
      j += 2;
      continue;
    }
    if (c == delim) return j + 1;
    if (c == '\n') return kNone;
    ++j;
  }
  return kNone;
}

namespace {

bool is_delim(char c) { return c == '"' || c == '\'' || c == '`'; }

/* 原型 OPS1 —— 单字符运算符的合法集合。 */
bool in_ops1(char c) {
  static const std::string ops1 = "+-*/%&|^~!<>()[]{},:;=?.";
  return ops1.find(c) != std::string::npos;
}

bool starts_with_at(std::string_view text, std::size_t i, const char* lit) {
  const std::string_view s(lit);
  return text.compare(i, s.size(), s) == 0;
}

const std::array<const char*, 2>& ops3() {
  static const std::array<const char*, 2> v = {"<<=", ">>="};
  return v;
}
const std::array<const char*, 16>& ops2() {
  static const std::array<const char*, 16> v = {"==", "!=", "<=", ">=", "&&", "||",
                                                "+=", "-=", "*=", "/=", "%=", "&=",
                                                "|=", "^=", "<<", ">>"};
  return v;
}

}  // namespace

std::vector<Tok> tokenize(std::string_view text) {
  std::vector<Tok> toks;
  const std::size_t n = text.size();
  std::size_t i = 0;
  int line = 1, col = 1;
  bool gap = true;

  while (i < n) {
    const char c = text[i];
    if (c == '\n') {
      ++i;
      ++line;
      col = 1;
      gap = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      ++col;
      gap = true;
      continue;
    }
    const int start_line = line, start_col = col;

    const std::size_t ident_end = ident_at(text, i);
    if (ident_end != kNone) {
      Tok t;
      t.kind = "ident";
      t.text = std::string(text.substr(i, ident_end - i));
      t.line = start_line;
      t.col = start_col;
      t.gap = gap;
      toks.push_back(std::move(t));
      col += code_points_between(text, i, ident_end);
      i = ident_end;
      gap = false;
      continue;
    }

    if (c == '#' || c == '@' || c == '$') {
      const std::size_t e = ident_at(text, i + 1);
      if (e == kNone) {
        throw LexError{start_line, start_col,
                       "第 " + std::to_string(start_line) + " 行：'" + std::string(1, c) +
                           "' 后面不是名字"};
      }
      Tok t;
      t.kind = (c == '#') ? "hash" : (c == '@' ? "at" : "dollar");
      t.text = std::string(text.substr(i + 1, e - i - 1));
      t.line = start_line;
      t.col = start_col;
      t.gap = gap;
      toks.push_back(std::move(t));
      col += code_points_between(text, i, e);
      i = e;
      gap = false;
      continue;
    }

    if (is_digit(c)) {
      std::size_t j = i;
      if (starts_with_at(text, i, "0x") || starts_with_at(text, i, "0X")) {
        j = i + 2;
        while (j < n && (is_hex_digit(text[j]) || text[j] == '_')) ++j;
      } else if (starts_with_at(text, i, "0b") || starts_with_at(text, i, "0B")) {
        j = i + 2;
        while (j < n && (text[j] == '0' || text[j] == '1' || text[j] == '_')) ++j;
      } else {
        while (j < n && (is_digit(text[j]) || text[j] == '_')) ++j;
      }
      if (j < n && text[j] == '.' && j + 1 < n && is_digit(text[j + 1])) {
        throw LexError{start_line, start_col,
                       "第 " + std::to_string(start_line) + " 行：浮点字面量未支持"};
      }
      Tok t;
      t.kind = "number";
      t.text = std::string(text.substr(i, j - i));
      t.line = start_line;
      t.col = start_col;
      t.gap = gap;
      toks.push_back(std::move(t));
      col += code_points_between(text, i, j);
      i = j;
      gap = false;
      continue;
    }

    if (is_delim(c)) {
      const std::size_t end = scan_quoted_at(text, i);
      if (end == kNone) {
        throw LexError{start_line, start_col,
                       "第 " + std::to_string(start_line) + " 行：" + std::string(1, c) +
                           " 字面量没有闭合"};
      }
      const std::string inner(text.substr(i + 1, end - 1 - (i + 1)));
      Tok t;
      t.kind = (c == '`') ? "template" : "string";
      t.text = inner;
      t.line = start_line;
      t.col = start_col;
      t.gap = gap;
      t.quote = std::string(1, c);
      t.interp = (t.kind == "template") && inner.find('{') != std::string::npos;
      toks.push_back(std::move(t));
      col += code_points_between(text, i, end);
      i = end;
      gap = false;
      continue;
    }

    bool matched = false;
    for (const char* op : ops3()) {
      if (starts_with_at(text, i, op)) {
        Tok t;
        t.kind = "op";
        t.text = op;
        t.line = start_line;
        t.col = start_col;
        t.gap = gap;
        toks.push_back(std::move(t));
        i += 3;
        col += 3;
        gap = false;
        matched = true;
        break;
      }
    }
    if (matched) continue;
    for (const char* op : ops2()) {
      if (starts_with_at(text, i, op)) {
        Tok t;
        t.kind = "op";
        t.text = op;
        t.line = start_line;
        t.col = start_col;
        t.gap = gap;
        toks.push_back(std::move(t));
        i += 2;
        col += 2;
        gap = false;
        matched = true;
        break;
      }
    }
    if (matched) continue;

    if (!in_ops1(c)) {
      std::string shown;
      if (static_cast<unsigned char>(c) < 0x20) {
        static const char* hex = "0123456789abcdef";
        shown = "\\x";
        shown.push_back(hex[(c >> 4) & 0xF]);
        shown.push_back(hex[c & 0xF]);
      } else {
        shown = std::string(1, c);
      }
      throw LexError{start_line, start_col, "第 " + std::to_string(start_line) +
                                                " 行：不认识的字符 '" + shown + "'"};
    }
    Tok t;
    t.kind = "op";
    t.text = std::string(1, c);
    t.line = start_line;
    t.col = start_col;
    t.gap = gap;
    toks.push_back(std::move(t));
    ++i;
    ++col;
    gap = false;
  }

  Tok eof;
  eof.kind = "eof";
  eof.line = line;
  eof.col = col;
  eof.gap = gap;
  toks.push_back(std::move(eof));
  return toks;
}

std::string describe(const Tok& t) {
  if (t.kind == "eof") return "这里就到头了";
  if (t.kind == "ident" || t.kind == "hash" || t.kind == "at") return "'" + t.text + "'";
  if (t.kind == "dollar") return "'$" + t.text + "'";
  if (t.kind == "number") return "数字 " + t.text;
  if (t.kind == "string") return "字符串字面量";
  if (t.kind == "template") return "模板字面量";
  return "'" + t.text + "'";
}

}  // namespace simplexc
