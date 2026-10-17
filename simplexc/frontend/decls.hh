// 词法掩码 → 顶层声明表 → 花括号配平 → 名字级可达性（M4.5 的 C++17 产品实现）。
//
// 语义 oracle = 冻结原型 `frontend/decls.py`（只读）：函数一条对一条搬，判定顺序、错误文本、
// 返回值的**顺序**都照抄（错误/警告/未支持项的顺序在判据里会被逐行比对）。
// 唯二的写法差异来自语言本身：
//   * Python dict → std::map、set → std::set（迭代顺序变确定；只在「集合」用途处出现，不影响输出）；
//   * Python `\w` 是 Unicode 感知的，这里用 ASCII（语料里代码位置的标识符全是 ASCII，
//     见 `work/tmp/ast` 的调查：mask 后的 code 行里没有非 ASCII 字节）。
//
// 本层只回答四件事（与原型一致）：
//   1. 有哪些声明（type / const / var / def / 带标签的块 / 顶层 #if），各自占哪几行；
//   2. 每个 def 的 `{}` 是否把体包住（只配平，**不解析体**）；
//   3. 每个 def 从顶层入口按名字是否可达；
//   4. 顶层有什么不认识的（不猜、不炸）。
#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simplexc {

// ---------------------------------------------------------------- 词法掩码

/* 一次扫描产出的两份等长文本（换行位置不变，行号一直对得上原文）。 */
struct Masked {
  std::string raw;   /* 注释 → 空格，字面量原样（判「这行是不是只有注释」） */
  std::string code;  /* 注释 → 空格，字面量文字 → 空格（结构/名字/花括号只看它） */
};

Masked mask_source(std::string_view text);

/* 按 '\n' 切行、去掉行尾 '\r'；末尾的空行不产生一行。 */
std::vector<std::string> split_lines(std::string_view text);

// ---------------------------------------------------------------- 声明识别

struct Decl {
  std::string kind;         /* type | const | var | def | label | pp_if */
  std::string name;
  int lo = 0;
  int hi = 0;
  int depth = 0;            /* 声明起始处的花括号深度（0 = 文件顶层） */
  std::string enclosing = "-";
  std::string params = "-";
  std::string ret = "-";
  std::string body = "none"; /* none | empty | braced */
  std::string note = "-";
  std::string reachable = "-";
  int parent = -1;
  std::vector<int> children;
};

struct ScanResult {
  std::vector<Decl> decls;
  std::vector<std::string> errors;                    /* 花括号/多余括号，按出现顺序 */
  std::vector<std::pair<int, std::string>> unsupported; /* (行号, raw 行 strip 后) */
};

ScanResult scan(const std::vector<std::string>& code_lines,
                const std::vector<std::string>& raw_lines);

/* 扫描后的两步体检（原型 main 里 `_body_is_empty` + `_paren_delta` 那一段）：
   空体判定 + 圆括号配平警告，就地改写 decls 的 body/note。返回 warnings（同序同文）。 */
std::vector<std::string> inspect_defs(std::vector<Decl>& decls,
                                      const std::vector<std::string>& code_lines);

bool body_is_empty(const std::vector<std::string>& code_lines, const Decl& d);

// ---------------------------------------------------------------- 可达性

using LineSet = std::set<int>;

/* 默认去掉嵌套声明的行（嵌套 def 的体不算外层调出来的调用）。 */
LineSet span_lines(const Decl& d, const std::vector<Decl>& decls, bool include_nested = false);
LineSet own_lines(const Decl& d, const std::vector<Decl>& decls);

/* 名字级匹配，不做重载解析：漏判调用 = 把活的当死的（危险），多判 = 把死的当活的（安全）。 */
std::set<std::string> calls_in(const LineSet& lines, const std::vector<std::string>& code_lines,
                               bool skip_def_header = true);

/* 真入口：顶层语句（标签块 / 顶层 var 初始化 / 顶层 #if）+ tc_main。 */
LineSet strict_root_lines(const std::vector<Decl>& decls, const LineSet& extra_root_lines);
/* 复现 dossier `reachability.py` 的口径：任何「不在 def 体里」的行都算顶层（hi 不含）。 */
LineSet lenient_root_lines(const std::vector<Decl>& decls, int n_lines);

struct Closure {
  std::map<std::string, std::vector<int>> by_name;  /* def 名 → 声明下标（可重载，故是列表） */
  std::set<std::string> live;
  std::set<std::string> roots;
};

Closure closure(const std::vector<Decl>& decls, const std::vector<std::string>& code_lines,
                const LineSet& root_lines, bool lenient = false);

// ---------------------------------------------------------------- 汇总

struct Summary {
  int lines_total = 0;
  int decls_total = 0;
  std::vector<std::pair<std::string, int>> kinds;  /* 按声明出现顺序 */
  int covered = 0;
  double covered_ratio = 0.0;
  int code_lines_total = 0;
  int code_lines_covered = 0;
  std::vector<int> uncovered_blank;
  std::vector<int> uncovered_comment;
  std::vector<int> uncovered_other;
  int def_decls = 0;
  int def_names = 0;
  std::vector<std::string> reachable_names;
  std::vector<std::string> unreachable_names;
  int reachable_decls = 0;
  int unreachable_decls = 0;
  int reachable_code_lines = 0;
  int unreachable_code_lines = 0;
  std::vector<std::string> roots;
  std::vector<std::string> reachable_names_lenient;
  std::vector<std::string> unreachable_names_lenient;
  std::vector<std::pair<int, std::string>> unsupported;
  std::vector<std::string> warnings;
  std::string roots_mode;  /* 原型在 summarize 之后才塞进去，JSON 里因此排在最后 */
};

Summary summarize(const std::vector<Decl>& decls, const std::vector<std::string>& code_lines,
                  const std::vector<std::string>& raw_lines,
                  const std::map<std::string, std::vector<int>>& by_name,
                  const std::set<std::string>& live, const std::vector<std::string>& roots_sorted,
                  const std::set<std::string>& lenient_live,
                  const std::vector<std::pair<int, std::string>>& unsupported,
                  const std::vector<std::string>& warnings);

std::string render_tsv(const std::vector<Decl>& decls);

}  // namespace simplexc
