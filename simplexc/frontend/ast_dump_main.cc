// ast_dump —— 前端与判据之间的接口（**调试用 main，不进产品 DLL**）。
//
// 为什么有它：`frontend/` 的判据是「同一份源码，两边产出 deep-equal」。所以要有一个能
// 原样复现三种产出的入口：
//
//   ast_dump --src <file.si> --def <name> [--json out]
//   ast_dump --src <file.si> --all       [--json out]
//   ast_dump --src <file.si> --decls     [--json out]    （声明表 + 汇总；
//                                                        paren 警告只在这一路里出现）
//
// 输出口径（与原型逐字节对齐，`diff` 就能当判据）：
//   * stdout / stderr 走文本模式（CRLF）—— 原型 `print` 在 Windows 上就是 CRLF；
//   * `--json` 的文件用二进制模式写、只写 LF —— 原型 `open(..., newline='\n')` 就是不翻译；
//   * 退出码：解析失败 / 找不到 def → 1；源文件读不了 → 2（`--all` 只在**可达** def 失败时给 1）。
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <algorithm>
#include <string>
#include <vector>

#include "ast_dump.hh"
#include "decls.hh"
#include "frontend.hh"
#include "json.hh"
#include "lexer.hh"
#include "parser.hh"

namespace {

using simplexc::Decl;
using simplexc::Frontend;

using simplexc::Json;
using simplexc::LexError;
using simplexc::ParseError;
using simplexc::Stats;

struct Opts {
  std::string src;
  std::string name;
  bool has_def = false;
  bool all = false;
  bool decls = false;
  std::string json;
  bool has_json = false;
  bool stats = false;
  bool quiet = false;
  std::string roots = "strict";
};

std::string where_of(int line, int col) {
  return "第 " + std::to_string(line) + " 行" + (col ? " 第 " + std::to_string(col) + " 列" : "");
}

/* Python 的 `s[:n]`（按码点截断，不是按字节）。 */
std::string trunc_cp(const std::string& s, std::size_t n) {
  std::size_t i = 0, cps = 0;
  while (i < s.size() && cps < n) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    ++cps;
  }
  return s.substr(0, i);
}

std::string strip_ws(const std::string& s) {
  auto sp = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
  };
  std::size_t b = 0, e = s.size();
  while (b < e && sp(s[b])) ++b;
  while (e > b && sp(s[e - 1])) --e;
  return s.substr(b, e - b);
}

bool write_json_file(const std::string& path, const Json& doc) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << doc.dump(1) << "\n";
  return true;
}

Json str_array(const std::vector<std::string>& v) {
  Json a = Json::array();
  for (const std::string& s : v) a.push(Json::string(s));
  return a;
}

Json int_array(const std::vector<int>& v) {
  Json a = Json::array();
  for (int i : v) a.push(Json::integer(i));
  return a;
}

Json stats_json(const Stats& st) {
  Json d = Json::object();
  d.set("def", Json::string(st.def));
  d.set("lo", Json::integer(st.lo));
  d.set("hi", Json::integer(st.hi));
  d.set("stmts", Json::integer(st.stmts));
  d.set("exprs", Json::integer(st.exprs));
  d.set("nested_defs", Json::integer(st.nested_defs));
  d.set("code_lines_own", Json::integer(st.code_lines_own));
  d.set("code_lines_nested", Json::integer(st.code_lines_nested));
  d.set("code_lines_span", Json::integer(st.code_lines_span));
  d.set("covered_own", Json::integer(st.covered_own));
  d.set("covered_ratio", Json::real(st.covered_ratio));
  d.set("uncovered_own", int_array(st.uncovered_own));
  d.set("uncovered_span", int_array(st.uncovered_span));
  Json kinds = Json::object();
  for (const auto& kv : st.node_kinds) kinds.set(kv.first, Json::integer(kv.second));
  d.set("node_kinds", kinds);
  d.set("unsupported", Json::array());
  d.set("warnings", str_array(st.warnings));
  return d;
}

void print_stats(const Frontend& fe, const Stats& st, bool detail) {
  std::ostream& e = std::cerr;
  e << fe.src_path << " " << st.def << "  L" << st.lo << "-" << st.hi << "\n";
  e << "  语句 " << st.stmts << " 条（含嵌套 def " << st.nested_defs << " 个）；表达式节点 "
    << st.exprs << " 个\n";
  char pct[32];
  std::snprintf(pct, sizeof(pct), "%.1f", 100.0 * st.covered_ratio);
  e << "  代码行 " << st.code_lines_own << "（嵌套 def 另 " << st.code_lines_nested
    << "，span 合计 " << st.code_lines_span << "）；覆盖 " << st.covered_own << "/"
    << st.code_lines_own << " = " << pct << "%\n";
  e << "  未支持构造：无（不认识的构造一律报错退出，不静默跳过；整文件的清单看 --all）\n";
  for (const std::string& w : st.warnings) e << "  警告：" << w << "\n";
  if (detail) {
    if (!st.uncovered_own.empty()) {
      e << "  未覆盖行（" << st.uncovered_own.size() << "）：\n";
      std::size_t shown = 0;
      for (int ln : st.uncovered_own) {
        if (shown++ >= 40) break;
        const std::string text =
            (ln >= 1 && static_cast<std::size_t>(ln) <= fe.raw_lines.size())
                ? trunc_cp(strip_ws(fe.raw_lines[ln - 1]), 100)
                : std::string();
        e << "    第 " << ln << " 行：" << text << "\n";
      }
    }
    std::vector<std::pair<std::string, int>> kinds = st.node_kinds;
    std::stable_sort(kinds.begin(), kinds.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string joined;
    for (std::size_t i = 0; i < kinds.size(); ++i) {
      if (i) joined += " ";
      joined += kinds[i].first + "×" + std::to_string(kinds[i].second);
    }
    e << "  节点：" << joined << "\n";
  }
}

int run_one(const Frontend& fe, const Opts& o) {
  const std::vector<int> cands = fe.candidates(o.name);
  if (cands.empty()) {
    std::set<std::string> names;
    for (const Decl& d : fe.decls) {
      if (d.kind == "def" || d.kind == "label") names.insert(d.name);
    }
    std::string list;
    for (const std::string& n : names) {
      if (!list.empty()) list += ", ";
      list += n;
    }
    std::cerr << "找不到 " << o.name << "；本文件里有：" << list << "\n";
    return 1;
  }
  std::size_t pick = 0;
  for (std::size_t k = 0; k < cands.size(); ++k) {
    const Decl& c = fe.decls[static_cast<std::size_t>(cands[k])];
    if (c.body == "braced" || c.kind == "label") {
      pick = k;
      break;
    }
  }
  const Decl& d = fe.decls[static_cast<std::size_t>(cands[pick])];
  if (cands.size() > 1) {
    std::cerr << "注意：" << o.name << " 有 " << cands.size() << " 处声明，用第 " << (pick + 1)
              << " 处（L" << d.lo << "-" << d.hi << "）\n";
  }
  if (d.kind == "def" && d.body != "braced") {
    std::cerr << o.name << "（L" << d.lo << "）没有函数体，没什么可解析的\n";
    return 1;
  }
  simplexc::ParsedDef pd;
  try {
    pd = fe.parse_def(cands[pick]);
  } catch (const ParseError& e) {
    const std::string line = (e.line >= 1 && static_cast<std::size_t>(e.line) <= fe.raw_lines.size())
                                 ? fe.raw_lines[e.line - 1]
                                 : std::string();
    std::cerr << "错误：" << where_of(e.line, e.col) << "：" << e.msg << "\n";
    std::cerr << "      " << e.line << " | " << line << "\n";
    return 1;
  } catch (const LexError& e) {
    std::cerr << "错误：" << where_of(e.line, e.col) << "：" << e.msg << "\n";
    return 1;
  }
  Stats st = simplexc::stats_for(fe, d, *pd.fn);
  st.warnings = pd.notes;
  if (!o.quiet) print_stats(fe, st, o.stats);
  std::cout << simplexc::render_tree(pd.fn.get(), 0) << "\n";
  if (o.has_json) {
    Json doc = Json::object();
    doc.set("source", Json::string(o.src));
    doc.set("def", Json::string(d.name));
    doc.set("form", Json::string(pd.fn->form));
    doc.set("lo", Json::integer(d.lo));
    doc.set("hi", Json::integer(d.hi));
    doc.set("stats", stats_json(st));
    doc.set("ast", simplexc::node_to_json(pd.fn.get()));
    if (!write_json_file(o.json, doc)) {
      std::cerr << "写不了 " << o.json << "\n";
      return 1;
    }
  }
  return 0;
}

int run_all(Frontend& fe, const Opts& o) {
  std::cout << "name\tform\tlo\thi\treachable\tstmts\texprs\tcode\tcovered\t结果\n";
  int n_fail = 0, n_live_fail = 0;
  Json rows = Json::array();
  for (std::size_t di = 0; di < fe.decls.size(); ++di) {
    const Decl& d = fe.decls[di];
    if (d.kind != "def" && d.kind != "label") continue;
    if (d.kind == "def" && d.body != "braced") continue;
    const bool reach = (d.kind == "label") || fe.live.count(d.name) != 0;
    const std::string reach_s = reach ? "是" : "否";
    Json row = Json::object();
    row.set("name", Json::string(d.name));
    row.set("form", Json::string(d.kind));
    row.set("lo", Json::integer(d.lo));
    row.set("hi", Json::integer(d.hi));
    row.set("reachable", Json::string(reach_s));
    simplexc::ParsedDef pd;
    try {
      pd = fe.parse_def(static_cast<int>(di));
    } catch (const ParseError& e) {
      ++n_fail;
      if (reach) ++n_live_fail;
      const std::string msg = "失败 第 " + std::to_string(e.line) + " 行：" + trunc_cp(e.msg, 80);
      std::cout << d.name << "\t" << d.kind << "\t" << d.lo << "\t" << d.hi << "\t" << reach_s
                << "\t-\t-\t-\t-\t" << msg << "\n";
      row.set("ok", Json::boolean(false));
      row.set("error_line", Json::integer(e.line));
      row.set("error_msg", Json::string(trunc_cp(e.msg, 80)));
      rows.push(std::move(row));
      continue;
    } catch (const LexError& e) {
      ++n_fail;
      if (reach) ++n_live_fail;
      const std::string msg = "失败 第 " + std::to_string(e.line) + " 行：" + trunc_cp(e.msg, 80);
      std::cout << d.name << "\t" << d.kind << "\t" << d.lo << "\t" << d.hi << "\t" << reach_s
                << "\t-\t-\t-\t-\t" << msg << "\n";
      row.set("ok", Json::boolean(false));
      row.set("error_line", Json::integer(e.line));
      row.set("error_msg", Json::string(trunc_cp(e.msg, 80)));
      rows.push(std::move(row));
      continue;
    }
    const Stats st = simplexc::stats_for(fe, d, *pd.fn);
    std::cout << d.name << "\t" << d.kind << "\t" << d.lo << "\t" << d.hi << "\t" << reach_s
              << "\t" << st.stmts << "\t" << st.exprs << "\t" << st.code_lines_own << "\t"
              << st.covered_own << "/" << st.code_lines_own << "\t"
              << (st.covered_ratio == 1.0 ? "OK" : "有未覆盖行") << "\n";
    row.set("stmts", Json::integer(st.stmts));
    row.set("exprs", Json::integer(st.exprs));
    row.set("code_lines_own", Json::integer(st.code_lines_own));
    row.set("covered_own", Json::integer(st.covered_own));
    row.set("ok", Json::boolean(st.covered_ratio == 1.0));
    if (st.covered_ratio != 1.0) row.set("covered_ratio", Json::real(st.covered_ratio));
    rows.push(std::move(row));
  }
  if (!o.quiet) {
    std::cerr << "共 " << n_fail << " 个 def 解析失败（其中可达 " << n_live_fail << " 个）\n";
  }
  if (o.has_json) {
    Json doc = Json::object();
    doc.set("source", Json::string(o.src));
    doc.set("defs", std::move(rows));
    if (!write_json_file(o.json, doc)) {
      std::cerr << "写不了 " << o.json << "\n";
      return 1;
    }
  }
  return n_live_fail ? 1 : 0;
}

/* 声明表那一路：声明表 + 汇总（`--roots` 口径也照抄）。 */
int run_decls(const Opts& o) {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<Decl> decls;
  std::vector<std::string> code_lines, raw_lines;
  try {
    /* 只做「读 + 掩码 + 扫描」：声明表那一路不分词（不可达 def 里的词法错也不该拦住它）。 */
    simplexc::ScannedSource scanned = simplexc::load_and_scan(o.src);
    decls = std::move(scanned.scan.decls);
    code_lines = std::move(scanned.code_lines);
    raw_lines = std::move(scanned.raw_lines);
    errors = std::move(scanned.scan.errors);
    auto unsupported = std::move(scanned.scan.unsupported);
    warnings = simplexc::inspect_defs(decls, code_lines);

    simplexc::LineSet extra;
    for (const auto& u : unsupported) extra.insert(u.first);
    const simplexc::Closure strict =
        simplexc::closure(decls, code_lines, simplexc::strict_root_lines(decls, extra), false);
    const simplexc::Closure lenient = simplexc::closure(
        decls, code_lines, simplexc::lenient_root_lines(decls, static_cast<int>(code_lines.size())),
        true);
    const std::set<std::string>& live = (o.roots == "lenient") ? lenient.live : strict.live;

    for (Decl& d : decls) {
      if (d.kind == "def") d.reachable = live.count(d.name) ? "是" : "否";
    }
    std::vector<std::string> roots_sorted(strict.roots.begin(), strict.roots.end());
    simplexc::Summary s = simplexc::summarize(decls, code_lines, raw_lines, strict.by_name, live,
                                              roots_sorted, lenient.live, unsupported, warnings);
    s.roots_mode = o.roots;

    std::cout << simplexc::render_tsv(decls) << "\n";
    if (!o.quiet) {
      std::ostream& e = std::cerr;
      std::vector<std::pair<std::string, int>> kinds = s.kinds;
      std::sort(kinds.begin(), kinds.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      std::string kinds_s;
      for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (i) kinds_s += " / ";
        kinds_s += kinds[i].first + " " + std::to_string(kinds[i].second);
      }
      e << "总行 " << s.lines_total << "；声明 " << s.decls_total << " 条（" << kinds_s << "）\n";
      char cov[32], cov_lines[32];
      std::snprintf(cov_lines, sizeof(cov_lines), "%.1f", 100.0 * s.covered_ratio);
      const double r = s.code_lines_total ? 100.0 * s.code_lines_covered / s.code_lines_total : 0.0;
      std::snprintf(cov, sizeof(cov), "%.1f", r);
      e << "覆盖：行 " << s.covered << "/" << s.lines_total << " = " << cov_lines << "%；代码行 "
        << s.code_lines_covered << "/" << s.code_lines_total << " = " << cov << "%\n";
      e << "未覆盖：空行 " << s.uncovered_blank.size() << "、注释行 " << s.uncovered_comment.size()
        << "、其它 " << s.uncovered_other.size() << "\n";
      if (!s.uncovered_other.empty()) {
        e << "  其它未覆盖行：";
        for (std::size_t i = 0; i < s.uncovered_other.size() && i < 40; ++i) {
          if (i) e << ", ";
          e << s.uncovered_other[i];
        }
        e << "\n";
      }
      e << "def：" << s.def_decls << " 处声明 / " << s.def_names << " 个名字；可达 "
        << s.reachable_names.size() << " 个名字（" << s.reachable_decls << " 处，代码行 "
        << s.reachable_code_lines << "） / 不可达 " << s.unreachable_names.size() << " 个名字（"
        << s.unreachable_decls << " 处，代码行 " << s.unreachable_code_lines << "）\n";
      e << "入口（顶层块里的调用）：";
      for (std::size_t i = 0; i < s.roots.size(); ++i) {
        if (i) e << ", ";
        e << s.roots[i];
      }
      e << "\n可达（" << s.roots_mode << " 口径）：";
      for (std::size_t i = 0; i < s.reachable_names.size(); ++i) {
        if (i) e << ", ";
        e << s.reachable_names[i];
      }
      e << "\n可达（另一口径：" << (s.roots_mode == "lenient" ? "strict" : "lenient") << "） "
        << s.reachable_names_lenient.size() << " 个名字\n";
      e << "不可达：";
      for (std::size_t i = 0; i < s.unreachable_names.size(); ++i) {
        if (i) e << ", ";
        e << s.unreachable_names[i];
      }
      e << "\n";
      for (const std::string& w : s.warnings) e << "警告：" << w << "\n";
      if (!s.unsupported.empty()) {
        e << "未支持的顶层构造 " << s.unsupported.size() << " 条：\n";
        for (std::size_t i = 0; i < s.unsupported.size() && i < 20; ++i) {
          e << "  第 " << s.unsupported[i].first << " 行：" << trunc_cp(s.unsupported[i].second, 100)
            << "\n";
        }
      } else {
        e << "未支持的顶层构造：无\n";
      }
    }
    if (o.has_json) {
      Json doc = Json::object();
      doc.set("source", Json::string(o.src));
      Json sum = Json::object();
      sum.set("lines_total", Json::integer(s.lines_total));
      sum.set("decls_total", Json::integer(s.decls_total));
      Json kinds = Json::object();
      for (const auto& kv : s.kinds) kinds.set(kv.first, Json::integer(kv.second));
      sum.set("kinds", std::move(kinds));
      sum.set("covered", Json::integer(s.covered));
      sum.set("covered_ratio", Json::real(s.covered_ratio));
      sum.set("code_lines_total", Json::integer(s.code_lines_total));
      sum.set("code_lines_covered", Json::integer(s.code_lines_covered));
      sum.set("uncovered_blank", int_array(s.uncovered_blank));
      sum.set("uncovered_comment", int_array(s.uncovered_comment));
      sum.set("uncovered_other", int_array(s.uncovered_other));
      sum.set("def_decls", Json::integer(s.def_decls));
      sum.set("def_names", Json::integer(s.def_names));
      sum.set("reachable_names", str_array(s.reachable_names));
      sum.set("unreachable_names", str_array(s.unreachable_names));
      sum.set("reachable_decls", Json::integer(s.reachable_decls));
      sum.set("unreachable_decls", Json::integer(s.unreachable_decls));
      sum.set("reachable_code_lines", Json::integer(s.reachable_code_lines));
      sum.set("unreachable_code_lines", Json::integer(s.unreachable_code_lines));
      sum.set("roots", str_array(s.roots));
      sum.set("reachable_names_lenient", str_array(s.reachable_names_lenient));
      sum.set("unreachable_names_lenient", str_array(s.unreachable_names_lenient));
      Json unsup = Json::array();
      for (const auto& u : s.unsupported) {
        Json pair = Json::array();
        pair.push(Json::integer(u.first));
        pair.push(Json::string(u.second));
        unsup.push(std::move(pair));
      }
      sum.set("unsupported", std::move(unsup));
      sum.set("warnings", str_array(s.warnings));
      sum.set("roots_mode", Json::string(s.roots_mode));
      doc.set("summary", std::move(sum));
      Json decl_rows = Json::array();
      for (const Decl& d : decls) {
        Json row = Json::object();
        row.set("kind", Json::string(d.kind));
        row.set("name", Json::string(d.name));
        row.set("lo", Json::integer(d.lo));
        row.set("hi", Json::integer(d.hi));
        row.set("params", Json::string(d.params));
        row.set("return_type", Json::string(d.ret));
        row.set("body_parsed", Json::string(d.kind == "def" ? (d.body == "empty" ? "是" : "否") : "-"));
        row.set("reachable", Json::string(d.reachable));
        row.set("depth", Json::integer(d.depth));
        row.set("enclosing", Json::string(d.enclosing));
        row.set("note", Json::string(d.note));
        decl_rows.push(std::move(row));
      }
      doc.set("decls", std::move(decl_rows));
      if (!write_json_file(o.json, doc)) {
        std::cerr << "写不了 " << o.json << "\n";
        return 1;
      }
    }
    if (!errors.empty()) {
      for (const std::string& er : errors) std::cerr << "错误：" << er << "\n";
      return 1;
    }
    return 0;
  } catch (const simplexc::ReadError& e) {
    std::cerr << e.msg << "\n";
    return 2;
  }
}

void usage() {
  std::cerr << "用法：ast_dump --src <file.si> (--def <名字> | --all | --decls) [--json out.json]\n"
               "              [--stats] [--quiet] [--roots strict|lenient]\n"
               "  --def    dump 一个 def（或 `run_sim:` 标签块）的 AST\n"
               "  --all    体检全文件：每个有体的 def 一行 TSV\n"
               "  --decls  声明表 + 汇总（含 paren 警告）\n";
}

}  // namespace

int main(int argc, char** argv) {
  Opts o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "FAIL " << what << " 需要一个值\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--src") {
      o.src = val("--src");
    } else if (a == "--def") {
      o.name = val("--def");
      o.has_def = true;
    } else if (a == "--all") {
      o.all = true;
    } else if (a == "--decls") {
      o.decls = true;
    } else if (a == "--json") {
      o.json = val("--json");
      o.has_json = true;
    } else if (a == "--stats") {
      o.stats = true;
    } else if (a == "--quiet") {
      o.quiet = true;
    } else if (a == "--roots") {
      o.roots = val("--roots");
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "FAIL 不认识的参数 " << a << "\n";
      usage();
      return 2;
    }
  }
  if (o.src.empty() || (!o.has_def && !o.all && !o.decls)) {
    usage();
    return 2;
  }
  if (o.roots != "strict" && o.roots != "lenient") {
    std::cerr << "FAIL --roots 只能是 strict 或 lenient\n";
    return 2;
  }

  if (o.decls) return run_decls(o);

  try {
    Frontend fe{o.src};
    return o.all ? run_all(fe, o) : run_one(fe, o);
  } catch (const simplexc::ReadError& e) {
    std::cerr << e.msg << "\n";
    return 2;
  } catch (const LexError& e) {
    std::cerr << "错误：" << where_of(e.line, e.col) << "：" << e.msg << "\n";
    return 1;
  }
}
