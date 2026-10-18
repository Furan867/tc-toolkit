#include "decls.hh"

#include <algorithm>
#include <cmath>
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
bool is_hspace(char c) { return c == ' ' || c == '\t'; }

/* Python str.strip()/rstrip() 的 ASCII 版（原型只在这些字符上用到；语料没有 Unicode 空白）。 */
bool is_py_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
std::string strip(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && is_py_space(s[b])) ++b;
  while (e > b && is_py_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}
std::string rstrip(const std::string& s) {
  std::size_t e = s.size();
  while (e > 0 && is_py_space(s[e - 1])) --e;
  return s.substr(0, e);
}

/* Python `*_RE.match` 里「^[ \t]*」那一段：返回行首空白之后的第一个下标。 */
std::size_t skip_indent(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && is_hspace(s[i])) ++i;
  return i;
}

/* 从 i 起吃一个 `\w+`（ASCII）；没吃到返回 npos。 */
std::size_t eat_word(const std::string& s, std::size_t i, std::size_t& end) {
  std::size_t j = i;
  while (j < s.size() && is_word(s[j])) ++j;
  if (j == i) return kNone;
  end = j;
  return j;
}

bool word_boundary(const std::string& s, std::size_t i) {
  return i >= s.size() || !is_word(s[i]);
}

/* 原型 `_scan_literal`：从定界符扫到闭定界符之后；遇换行即截断（未闭合不跨行）。 */
std::size_t scan_literal(std::string_view text, std::size_t i) {
  const char delim = text[i];
  const std::size_t n = text.size();
  std::size_t j = i + 1;
  while (j < n) {
    if (text[j] == '\\') {
      j += 2;
      continue;
    }
    if (text[j] == delim) return j + 1;
    if (text[j] == '\n') return j;
    ++j;
  }
  return n;
}

/* 原型 `_blank_keep_interp`：模板字符串的文字部分抹成空格，`{expr}` 里是代码，原样留下。 */
std::string blank_keep_interp(const std::string& seg) {
  std::string out;
  out.reserve(seg.size());
  for (char ch : seg) out.push_back(ch == '\n' ? '\n' : ' ');
  std::size_t k = 0;
  while (k < seg.size()) {
    if (seg[k] != '{') {
      ++k;
      continue;
    }
    int depth = 1;
    std::size_t m = k + 1;
    while (m < seg.size() && depth != 0) {
      depth += (seg[m] == '{') ? 1 : 0;
      depth -= (seg[m] == '}') ? 1 : 0;
      ++m;
    }
    if (depth != 0) break;
    for (std::size_t t = k; t < m; ++t) out[t] = seg[t];
    k = m;
  }
  return out;
}

/* 原型 `_parse_def_tail`：code[open_idx] == '('；返回 (params, ret, 体是否在本行开 '{')。 */
void parse_def_tail(const std::string& code, std::size_t open_idx, std::string& params,
                    std::string& ret, bool& has_brace) {
  int depth = 0;
  std::size_t j = open_idx;
  while (j < code.size()) {
    const char ch = code[j];
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      --depth;
      if (depth == 0) break;
    }
    ++j;
  }
  if (depth != 0) {
    const std::string tail = strip(code.substr(std::min(open_idx + 1, code.size())));
    params = tail.empty() ? "-" : tail;
    ret = "?";
    has_brace = false;
    return;
  }
  const std::string inner = strip(code.substr(open_idx + 1, j - open_idx - 1));
  params = inner.empty() ? "-" : inner;
  const std::string rest = code.substr(j + 1);
  const std::size_t brace = rest.find('{');
  const std::string head = brace == kNone ? rest : rest.substr(0, brace);
  const std::string head_s = strip(head);
  ret = head_s.empty() ? "-" : head_s;
  has_brace = brace != kNone;
}

/* ---------------------------------------------------------------- 行级正则等价物 */

bool match_def(const std::string& s, std::string& name, std::size_t& open_paren) {
  std::size_t i = skip_indent(s);
  if (s.compare(i, 3, "def") != 0) return false;
  i += 3;
  std::size_t j = i;
  while (j < s.size() && is_hspace(s[j])) ++j;
  if (j == i) return false;
  std::size_t end = 0;
  if (eat_word(s, j, end) == kNone) return false;
  name = s.substr(j, end - j);
  std::size_t m = end;
  while (m < s.size() && is_hspace(s[m])) ++m;
  if (m >= s.size() || s[m] != '(') return false;
  open_paren = m;
  return true;
}

bool match_type(const std::string& s, std::string& name, std::string& kw) {
  std::size_t i = skip_indent(s);
  if (s.compare(i, 4, "type") != 0) return false;
  i += 4;
  std::size_t j = i;
  while (j < s.size() && is_hspace(s[j])) ++j;
  if (j == i) return false;
  std::size_t end = 0;
  if (eat_word(s, j, end) == kNone) return false;
  name = s.substr(j, end - j);
  std::size_t m = end;
  while (m < s.size() && is_hspace(s[m])) ++m;
  if (m == end) return false;  /* 这里也要至少一个空白 */
  if (s.compare(m, 4, "Enum") == 0 && word_boundary(s, m + 4)) {
    kw = "Enum";
    return true;
  }
  if (s.compare(m, 6, "Struct") == 0 && word_boundary(s, m + 6)) {
    kw = "Struct";
    return true;
  }
  return false;
}

bool match_const(const std::string& s, std::string& name) {
  std::size_t i = skip_indent(s);
  if (s.compare(i, 5, "const") != 0) return false;
  i += 5;
  std::size_t j = i;
  while (j < s.size() && is_hspace(s[j])) ++j;
  if (j == i) return false;
  if (j >= s.size() || s[j] != '#') return false;
  std::size_t end = 0;
  if (eat_word(s, j + 1, end) == kNone) return false;
  name = "#" + s.substr(j + 1, end - j - 1);
  std::size_t m = end;
  while (m < s.size() && is_hspace(s[m])) ++m;
  return m < s.size() && s[m] == '=';
}

bool match_var(const std::string& s, std::string& name) {
  std::size_t i = skip_indent(s);
  if (s.compare(i, 3, "var") != 0) return false;
  i += 3;
  std::size_t j = i;
  while (j < s.size() && is_hspace(s[j])) ++j;
  if (j == i) return false;
  std::size_t end = 0;
  if (eat_word(s, j, end) == kNone) return false;
  name = s.substr(j, end - j);
  std::size_t m = end;
  while (m < s.size() && is_hspace(s[m])) ++m;
  return m < s.size() && s[m] == '=';
}

bool match_label(const std::string& s, std::string& name) {
  std::size_t i = skip_indent(s);
  std::size_t end = 0;
  if (eat_word(s, i, end) == kNone) return false;
  name = s.substr(i, end - i);
  std::size_t m = end;
  while (m < s.size() && is_hspace(s[m])) ++m;
  if (m >= s.size() || s[m] != ':') return false;
  ++m;
  while (m < s.size() && is_hspace(s[m])) ++m;
  for (const char* kw : {"while", "for", "loop"}) {
    const std::size_t len = std::string(kw).size();
    if (s.compare(m, len, kw) == 0 && word_boundary(s, m + len)) return true;
  }
  return false;
}

/* `^[ \t]*#(if|else|elif)\b`；返回命中的关键字。 */
bool match_ppif(const std::string& s, std::string& kw) {
  std::size_t i = skip_indent(s);
  if (i >= s.size() || s[i] != '#') return false;
  ++i;
  for (const char* k : {"if", "else", "elif"}) {
    const std::size_t len = std::string(k).size();
    if (s.compare(i, len, k) == 0 && word_boundary(s, i + len)) {
      kw = k;
      return true;
    }
  }
  return false;
}

/* `re.fullmatch(r'[}\])]+[ \t]*;?', s)` */
bool only_closing_chars(const std::string& s) {
  std::size_t i = 0;
  bool any = false;
  while (i < s.size() && (s[i] == '}' || s[i] == ')' || s[i] == ']')) {
    ++i;
    any = true;
  }
  if (!any) return false;
  while (i < s.size() && is_hspace(s[i])) ++i;
  if (i < s.size() && s[i] == ';') ++i;
  return i == s.size();
}

/* 本行是否起一个声明。open_decls 只用于判「是不是声明层」与外层名。 */
bool match_decl(const std::string& code, const std::string& raw, int ln_no, int depth,
                const std::vector<Decl>& decls, const std::vector<int>& open_decls, Decl& out) {
  const bool at_decl_level = std::all_of(open_decls.begin(), open_decls.end(),
                                        [&](int i) { return decls[i].kind == "pp_if"; });
  const std::string enclosing = open_decls.empty() ? "-" : decls[open_decls.back()].name;

  std::string kw;
  if (match_ppif(code, kw) && at_decl_level) {
    /* 只有顶层（或顶层 #if 里）的 #if 算声明级块；函数体里的 #if 属于体，不记 */
    std::string cond = strip(raw);
    while (!cond.empty() && cond.back() == '{') cond.pop_back();
    cond = strip(cond);
    out = Decl{};
    out.kind = "pp_if";
    out.name = cond;
    out.lo = ln_no;
    out.depth = depth;
    out.enclosing = enclosing;
    out.note = "#" + kw;
    return true;
  }

  std::string name;
  std::size_t open_paren = 0;
  if (match_def(code, name, open_paren)) {
    std::string params, ret;
    bool has_brace = false;
    parse_def_tail(code, open_paren, params, ret, has_brace);
    out = Decl{};
    out.kind = "def";
    out.name = name;
    out.lo = ln_no;
    out.depth = depth;
    out.enclosing = enclosing;
    out.params = params;
    out.ret = ret;
    out.note = enclosing == "-" ? "-" : "nested";
    out.body = has_brace ? "braced" : "none";
    return true;
  }

  if (!at_decl_level) return false;

  std::string tname, tkw;
  if (match_type(code, tname, tkw)) {
    out = Decl{};
    out.kind = "type";
    out.name = tname;
    out.lo = ln_no;
    out.depth = depth;
    out.enclosing = enclosing;
    out.note = tkw;
    return true;
  }
  std::string cname;
  if (match_const(code, cname)) {
    out = Decl{};
    out.kind = "const";
    out.name = cname;
    out.lo = ln_no;
    out.depth = depth;
    out.enclosing = enclosing;
    return true;
  }
  std::string vname;
  if (match_var(code, vname)) {
    out = Decl{};
    out.kind = "var";
    out.name = vname;
    out.lo = ln_no;
    out.depth = depth;
    out.enclosing = enclosing;
    return true;
  }
  std::string lname;
  if (match_label(code, lname)) {
    out = Decl{};
    out.kind = "label";
    out.name = lname;
    out.lo = ln_no;
    out.depth = depth;
    out.enclosing = enclosing;
    return true;
  }
  return false;
}

/* 名字级调用识别：`(?<![\w.])([A-Za-z_]\w*)[ \t]*\(` 的手写版。 */
const std::set<std::string>& not_call_words() {
  static const std::set<std::string> v = {
      "if",   "elif",  "else",   "while", "for",  "switch", "case",
      "return", "def", "let",    "var",   "const", "type",  "and",
      "or",   "not",   "in"};
  return v;
}

}  // namespace

// ---------------------------------------------------------------- 词法掩码

Masked mask_source(std::string_view text) {
  Masked out;
  const std::size_t n = text.size();
  std::size_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      std::size_t j = text.find('\n', i);
      if (j == kNone) j = n;
      out.raw.append(j - i, ' ');
      out.code.append(j - i, ' ');
      i = j;
    } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      std::size_t j = text.find("*/", i + 2);
      j = (j == kNone) ? n : j + 2;
      for (std::size_t t = i; t < j; ++t) {
        const char ch = text[t] == '\n' ? '\n' : ' ';
        out.raw.push_back(ch);
        out.code.push_back(ch);
      }
      i = j;
    } else if (c == '"' || c == '\'' || c == '`') {
      const std::size_t j = scan_literal(text, i);
      const std::string seg(text.substr(i, j - i));
      out.raw += seg;
      if (c == '`') {
        out.code += blank_keep_interp(seg);
      } else {
        out.code.append(j - i, ' ');
      }
      i = j;
    } else {
      out.raw.push_back(c);
      out.code.push_back(c);
      ++i;
    }
  }
  return out;
}

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t b = 0;
  while (true) {
    const std::size_t e = text.find('\n', b);
    if (e == kNone) {
      lines.emplace_back(text.substr(b));
      break;
    }
    lines.emplace_back(text.substr(b, e - b));
    b = e + 1;
  }
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  for (std::string& ln : lines) {
    std::size_t e = ln.size();
    while (e > 0 && ln[e - 1] == '\r') --e;
    ln.resize(e);
  }
  return lines;
}

// ---------------------------------------------------------------- 声明扫描

ScanResult scan(const std::vector<std::string>& code_lines,
                const std::vector<std::string>& raw_lines) {
  ScanResult res;
  int depth = 0;
  int bracket = 0;               /* 未闭合 '[' 的个数 */
  std::vector<int> open_braces;  /* 未闭合 '{' 所在行号 */
  std::vector<int> open_decls;   /* 已开体、还没收口的声明下标 */

  for (std::size_t li = 0; li < code_lines.size(); ++li) {
    const int ln_no = static_cast<int>(li) + 1;
    const std::string& code = code_lines[li];
    const int d0 = depth;
    const std::string raw = li < raw_lines.size() ? raw_lines[li] : std::string();

    Decl started;
    const bool hit = match_decl(code, raw, ln_no, d0, res.decls, open_decls, started);

    /* 花括号配平（字面量与注释已被掩码，所以只剩结构性的 {}）；
       '[' 一起算，因为 `type Char Enum[` 的成员表就跨好几行 —— 不算的话那段成员行
       会被当成「未支持的顶层构造」，而声明本身的 hi 停在 '[' 那一行，lowering 拿不到右括号。 */
    for (char ch : code) {
      if (ch == '{') {
        open_braces.push_back(ln_no);
        ++depth;
      } else if (ch == '}') {
        if (open_braces.empty()) {
          res.errors.push_back("第 " + std::to_string(ln_no) +
                               " 行：多余的 '}'（此处没有未闭合的 '{'）");
        } else {
          open_braces.pop_back();
          --depth;
        }
      } else if (ch == '[') {
        ++bracket;
      } else if (ch == ']' && bracket > 0) {
        --bracket;
      }
    }

    if (hit) {
      const int idx = static_cast<int>(res.decls.size());
      res.decls.push_back(started);
      if (idx > 0 && res.decls[idx].enclosing != "-") {
        for (int k = idx - 1; k >= 0; --k) {
          if (res.decls[k].name == res.decls[idx].enclosing && res.decls[k].depth < d0) {
            res.decls[idx].parent = k;
            res.decls[k].children.push_back(idx);
            break;
          }
        }
      }
      if (depth > d0 || bracket > 0) {
        open_decls.push_back(idx);
      } else {
        res.decls[idx].hi = ln_no;
      }
    }

    /* 收口：本行结束后回到（或越过）声明起始深度、且 '[' 也配平了的，说明体已闭合 */
    while (!open_decls.empty() && bracket == 0 &&
           depth <= res.decls[open_decls.back()].depth) {
      res.decls[open_decls.back()].hi = ln_no;
      open_decls.pop_back();
    }

    /* 未支持：声明层上既不是声明、也不是单独的闭合括号/空行/注释 */
    const std::string trimmed = strip(code);
    if (open_decls.empty() && !hit && depth == 0 && !trimmed.empty() &&
        !only_closing_chars(trimmed)) {
      res.unsupported.emplace_back(ln_no, strip(raw));
    }
  }

  if (!open_braces.empty()) {
    std::string where;
    if (open_braces.size() == 1) {
      where = "第 " + std::to_string(open_braces[0]) + " 行";
    } else {
      std::string head;
      const std::size_t show = std::min<std::size_t>(open_braces.size(), 5);
      for (std::size_t k = 0; k < show; ++k) {
        if (k) head += ", ";
        head += std::to_string(open_braces[k]);
      }
      const std::string more = open_braces.size() <= 5 ? "" : " 等";
      where = "起始行 " + head + more + "（第一个在第 " + std::to_string(open_braces[0]) +
              " 行，最近一个在第 " + std::to_string(open_braces.back()) + " 行）";
    }
    res.errors.push_back("花括号未配平：文件结束时还有 " + std::to_string(open_braces.size()) +
                         " 个 '{' 未闭合，" + where);
  }

  for (Decl& d : res.decls) {
    if (d.hi == 0) d.hi = d.lo;
  }
  return res;
}

bool body_is_empty(const std::vector<std::string>& code_lines, const Decl& d) {
  if (d.kind != "def" || d.body != "braced") return false;
  std::string text;
  for (int ln = d.lo; ln <= d.hi; ++ln) {
    if (ln >= 1 && static_cast<std::size_t>(ln) <= code_lines.size()) text += code_lines[ln - 1];
  }
  const std::size_t open_at = text.find('{');
  const std::size_t close_at = text.rfind('}');
  if (open_at == kNone || close_at == kNone || close_at < open_at) return false;
  return strip(text.substr(open_at + 1, close_at - open_at - 1)).empty();
}

std::vector<std::string> inspect_defs(std::vector<Decl>& decls,
                                      const std::vector<std::string>& code_lines) {
  std::vector<std::string> warnings;
  for (Decl& d : decls) {
    if (d.kind != "def") continue;
    if (body_is_empty(code_lines, d)) d.body = "empty";
    std::string span;
    for (int ln = d.lo; ln <= d.hi; ++ln) {
      if (ln >= 1 && static_cast<std::size_t>(ln) <= code_lines.size()) span += code_lines[ln - 1];
    }
    int delta = 0;
    for (char c : span) {
      if (c == '(') ++delta;
      if (c == ')') --delta;
    }
    if (delta != 0) {
      d.note = (d.note != "-" ? d.note + "; " : "") + "paren" + (delta > 0 ? "+" : "") +
               std::to_string(delta);
      warnings.push_back(d.name + "（L" + std::to_string(d.lo) + "-" + std::to_string(d.hi) +
                         "）圆括号不配平（" + (delta > 0 ? "+" : "") + std::to_string(delta) +
                         " 个）—— 按纪律只配平花括号，不报错");
    }
  }
  return warnings;
}

// ---------------------------------------------------------------- 可达性

LineSet span_lines(const Decl& d, const std::vector<Decl>& decls, bool include_nested) {
  LineSet lines;
  for (int ln = d.lo; ln <= d.hi; ++ln) lines.insert(ln);
  if (include_nested) return lines;
  for (int ci : d.children) {
    for (int ln = decls[ci].lo; ln <= decls[ci].hi; ++ln) lines.erase(ln);
  }
  return lines;
}

LineSet own_lines(const Decl& d, const std::vector<Decl>& decls) {
  return span_lines(d, decls, false);
}

std::set<std::string> calls_in(const LineSet& lines, const std::vector<std::string>& code_lines,
                               bool skip_def_header) {
  std::set<std::string> names;
  for (int ln : lines) {
    if (ln < 1 || static_cast<std::size_t>(ln) > code_lines.size()) continue;
    const std::string& line = code_lines[ln - 1];
    if (line.empty()) continue;
    std::size_t p = 0;
    while (p < line.size()) {
      if (!is_ident_start(line[p]) ||
          (p > 0 && (is_word(line[p - 1]) || line[p - 1] == '.'))) {
        if (is_ident_start(line[p])) {
          while (p < line.size() && is_word(line[p])) ++p;  /* 前缀是 \w/. → 整段不是起点 */
        } else {
          ++p;
        }
        continue;
      }
      std::size_t end = p;
      while (end < line.size() && is_word(line[end])) ++end;
      std::size_t m = end;
      while (m < line.size() && is_hspace(line[m])) ++m;
      const std::size_t start = p;
      p = end;
      if (m >= line.size() || line[m] != '(') continue;
      const std::string head = rstrip(line.substr(0, start));
      if (skip_def_header && head.size() >= 3 && head.compare(head.size() - 3, 3, "def") == 0) {
        continue;  /* 声明头 `def name(` 不是调用 */
      }
      const std::string name = line.substr(start, end - start);
      if (!not_call_words().count(name)) names.insert(name);
    }
  }
  return names;
}

LineSet strict_root_lines(const std::vector<Decl>& decls, const LineSet& extra_root_lines) {
  LineSet lines = extra_root_lines;
  for (const Decl& d : decls) {
    if (d.kind == "label" || d.kind == "var" || d.kind == "const" || d.kind == "pp_if") {
      const LineSet own = own_lines(d, decls);
      lines.insert(own.begin(), own.end());
    }
  }
  for (const Decl& d : decls) {
    if (d.kind == "def" && d.name == "tc_main") {
      const LineSet own = own_lines(d, decls);
      lines.insert(own.begin(), own.end());
    }
  }
  return lines;
}

LineSet lenient_root_lines(const std::vector<Decl>& decls, int n_lines) {
  LineSet lines;
  for (int ln = 1; ln <= n_lines; ++ln) lines.insert(ln);
  for (const Decl& d : decls) {
    if (d.kind == "def") {
      for (int ln = d.lo; ln < d.hi; ++ln) lines.erase(ln);
    }
  }
  return lines;
}

Closure closure(const std::vector<Decl>& decls, const std::vector<std::string>& code_lines,
                const LineSet& root_lines, bool lenient) {
  Closure out;
  for (std::size_t i = 0; i < decls.size(); ++i) {
    if (decls[i].kind == "def") out.by_name[decls[i].name].push_back(static_cast<int>(i));
  }
  for (const std::string& n : calls_in(root_lines, code_lines, !lenient)) {
    if (out.by_name.count(n)) out.roots.insert(n);
  }
  std::vector<std::string> work(out.roots.begin(), out.roots.end());
  while (!work.empty()) {
    const std::string n = work.back();
    work.pop_back();
    if (out.live.count(n)) continue;
    out.live.insert(n);
    for (int idx : out.by_name[n]) {
      const LineSet body = span_lines(decls[idx], decls, lenient);
      for (const std::string& c : calls_in(body, code_lines, !lenient)) {
        if (out.by_name.count(c) && !out.live.count(c)) work.push_back(c);
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------- 汇总

namespace {

std::string line_class(const std::string& code_line, const std::string& raw_line) {
  if (!strip(code_line).empty()) return "code";
  if (!strip(raw_line).empty()) return "comment";
  return "blank";
}

std::string body_parsed_value(const Decl& d) {
  if (d.kind != "def") return "-";
  return d.body == "empty" ? "是" : "否";
}

}  // namespace

Summary summarize(const std::vector<Decl>& decls, const std::vector<std::string>& code_lines,
                  const std::vector<std::string>& raw_lines,
                  const std::map<std::string, std::vector<int>>& by_name,
                  const std::set<std::string>& live, const std::vector<std::string>& roots_sorted,
                  const std::set<std::string>& lenient_live,
                  const std::vector<std::pair<int, std::string>>& unsupported,
                  const std::vector<std::string>& warnings) {
  Summary s;
  const int total = static_cast<int>(code_lines.size());
  s.lines_total = total;
  s.decls_total = static_cast<int>(decls.size());

  std::set<int> covered;
  for (const Decl& d : decls) {
    for (int ln = d.lo; ln <= d.hi; ++ln) covered.insert(ln);
  }
  s.covered = static_cast<int>(covered.size());
  /* 原型的 `round(x, 4)`：JSON 里就是这个四位小数值，不 round 两边就对不上。 */
  const double ratio = total ? static_cast<double>(covered.size()) / total : 0.0;
  s.covered_ratio = std::nearbyint(ratio * 10000.0) / 10000.0;

  for (int ln = 1; ln <= total; ++ln) {
    if (covered.count(ln)) continue;
    const std::string& code = code_lines[ln - 1];
    const std::string raw = (ln - 1 < static_cast<int>(raw_lines.size())) ? raw_lines[ln - 1] : "";
    const std::string cls = line_class(code, raw);
    if (cls == "blank") {
      s.uncovered_blank.push_back(ln);
    } else if (cls == "comment") {
      s.uncovered_comment.push_back(ln);
    } else {
      s.uncovered_other.push_back(ln);
    }
  }

  int code_lines_total = 0;
  for (const std::string& c : code_lines) {
    if (!strip(c).empty()) ++code_lines_total;
  }
  s.code_lines_total = code_lines_total;
  int code_covered = 0;
  for (int ln : covered) {
    if (ln >= 1 && ln <= total && !strip(code_lines[ln - 1]).empty()) ++code_covered;
  }
  s.code_lines_covered = code_covered;

  int def_decls = 0;
  for (const Decl& d : decls) {
    if (d.kind == "def") ++def_decls;
  }
  s.def_decls = def_decls;
  s.def_names = static_cast<int>(by_name.size());

  std::vector<int> live_idx, dead_idx;
  for (const std::string& name : live) {
    auto it = by_name.find(name);
    if (it == by_name.end()) continue;
    for (int i : it->second) live_idx.push_back(i);
  }
  for (std::size_t i = 0; i < decls.size(); ++i) {
    if (decls[i].kind == "def" && !live.count(decls[i].name)) dead_idx.push_back((int)i);
  }

  auto code_count = [&](const std::vector<int>& idx_list) {
    if (idx_list.empty()) return 0;
    std::set<int> lines;
    for (int i : idx_list) {
      const LineSet own = own_lines(decls[i], decls);
      lines.insert(own.begin(), own.end());
    }
    int n = 0;
    for (int ln : lines) {
      if (ln >= 1 && ln <= total && !strip(code_lines[ln - 1]).empty()) ++n;
    }
    return n;
  };

  s.reachable_decls = static_cast<int>(live_idx.size());
  s.unreachable_decls = static_cast<int>(dead_idx.size());
  s.reachable_code_lines = code_count(live_idx);
  s.unreachable_code_lines = code_count(dead_idx);

  for (const Decl& d : decls) {
    bool found = false;
    for (auto& kv : s.kinds) {
      if (kv.first == d.kind) {
        ++kv.second;
        found = true;
        break;
      }
    }
    if (!found) s.kinds.emplace_back(d.kind, 1);
  }

  s.reachable_names.assign(live.begin(), live.end());          /* std::set 已排序 */
  std::set<std::string> unreachable;
  for (const auto& kv : by_name) {
    if (!live.count(kv.first)) unreachable.insert(kv.first);
  }
  s.unreachable_names.assign(unreachable.begin(), unreachable.end());
  s.roots = roots_sorted;
  s.reachable_names_lenient.assign(lenient_live.begin(), lenient_live.end());
  std::set<std::string> unreachable_lenient;
  for (const auto& kv : by_name) {
    if (!lenient_live.count(kv.first)) unreachable_lenient.insert(kv.first);
  }
  s.unreachable_names_lenient.assign(unreachable_lenient.begin(), unreachable_lenient.end());
  s.unsupported = unsupported;
  s.warnings = warnings;
  return s;
}

std::string render_tsv(const std::vector<Decl>& decls) {
  std::string out = "kind\tname\tlo\thi\tparams\tret\tbody_parsed\treachable\tdepth\tenclosing\tnote";
  for (const Decl& d : decls) {
    const std::string span =
        d.hi != d.lo ? std::to_string(d.lo) + "-" + std::to_string(d.hi) : std::to_string(d.lo);
    out += "\n" + d.kind + "\t" + d.name + "\t" + span + "\t" + d.params + "\t" + d.ret + "\t" +
           body_parsed_value(d) + "\t" + d.reachable + "\t" + std::to_string(d.depth) + "\t" +
           d.enclosing + "\t" + d.note;
  }
  return out;
}

}  // namespace simplexc
