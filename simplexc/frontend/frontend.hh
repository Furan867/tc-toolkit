// 文件级上下文：decls + 按 def 解析（M4.5 的 C++17 产品实现）。
//
// 按 `decls.hh` 给的 `lo`/`hi` 切出某个 def 的 token 流，
// 解析成语法树 —— **只到语法层**，类型仍只是源里的拼写。
//
// 错误三轴从第一层就带全（方案 §2d），词法错与语法错**都抛 `sema::CompileError`**：
//   * `ReadError` —— 源文件读不了（原型是 OSError；它不是源码错，CLI 退出码 2）；
//   * `CompileError{UserSource, Lex, span, detail}` —— 整文件分词失败（构造 Frontend 时就抛，
//                    **不可达 def 里的词法错也会拦下来**，这是原型的行为：
//                    `self.toks = tokenize(raw_text)` 在构造期做）；
//   * `CompileError{UserSource, Parse, span, detail}` —— 某个 def 的语法错。
// `detail` 与原型逐字一致（判据要的就是那个文本）；三轴是给壳的：责任轴决定回不回退，
// 位置轴进 `simplexc_last_error()`（游戏进程里看不到 stderr）。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ast.hh"
#include "decls.hh"
#include "lexer.hh"

namespace simplexc {

struct ReadError {
  std::string msg;
};

/* 内存里的源码。为什么包一层而不是重载 `Frontend(const std::string& text)`：
   源码路径与源码正文都是 `std::string`，`Frontend(s)` 会在两个重载之间歧义。 */
struct SrcText {
  std::string text;
};

/* 读源文件 + 词法掩码 + 切行 + 声明扫描（**不分词**）。
   为什么单独拆一步：声明表那一路从不分词 —— 语料里的 `stdlib_prefix.si` 第 172 行有个
   `$`，分词会报错（整文件分词就是整个文件失败），但没有词法错误的文件仍应出声明表。 */
struct ScannedSource {
  std::vector<std::string> code_lines;
  std::vector<std::string> raw_lines;
  std::string masked_raw; /* 分词用：注释已抹成空格、字面量原样 */
  ScanResult scan;
};

ScannedSource load_and_scan(const std::string& path); /* 读不了 → ReadError */

/* 源里出现过的类型名：`type` 声明 + 每个 def 头的参数/返回类型字面。 */
std::set<std::string> declared_type_names(const std::vector<Decl>& decls);

/* 一个 def（或 `run_sim:` 顶层块）的解析结果；notes = 原型 `Parser.notes`。 */
struct ParsedDef {
  std::unique_ptr<FunctionDef> fn;
  std::vector<std::string> notes;
};

/* 原型的 `walk`：先序遍历所有 Node（列表字段也下钻），顺序 = dataclass 字段顺序。
   `TypeRef` 是例外 —— 原型里它是一种 Node（有 src_line，也进 node_kinds），但冻结的 `ast.hh`
   没让它继承 Node（它只有 src_line/name/at/elem）→ 所以遍历分两个回调，TypeRef 单独下钻一层。 */
using NodeVisitor = std::function<void(const Node&)>;
using TypeRefVisitor = std::function<void(const TypeRef&)>;
void walk_nodes(const Node* n, const NodeVisitor& on_node, const TypeRefVisitor& on_type);

/* 产生过节点的源码行 + 每个块的收尾 '}' 行 —— 覆盖率的分母口径在这里。 */
std::set<int> covered_lines(const FunctionDef& fd);

struct Stats {
  std::string def;
  int lo = 0;
  int hi = 0;
  int stmts = 0;
  int exprs = 0;
  int nested_defs = 0;
  int code_lines_own = 0;
  int code_lines_nested = 0;
  int code_lines_span = 0;
  int covered_own = 0;
  double covered_ratio = 1.0;
  std::vector<int> uncovered_own;
  std::vector<int> uncovered_span;
  std::vector<std::pair<std::string, int>> node_kinds; /* 首次出现顺序 = 原型 dict 的插入顺序 */
  std::vector<std::string> warnings;
};

/* `Frontend(SrcText)` 填进 `src_path` 的占位名（内存源码没有文件名）。 */
inline constexpr const char* kMemorySrcPath = "<memory>";

class Frontend {
 public:
  /* 读文件 + 掩码 + 扫描 + 整文件分词。ReadError / 词法 CompileError 从构造期抛。 */
  explicit Frontend(const std::string& src_path);

  /* 同一套处理，只是源码来自内存（壳在游戏进程里编译，没有临时文件可写）。
     `src_path` 填 kMemorySrcPath —— 它只进报错与 ast_dump 文本，没有别的用途。 */
  explicit Frontend(SrcText src);

  std::vector<int> candidates(const std::string& name) const;
  ParsedDef parse_def(int decl_idx) const;

  /* span 内的代码行（去掉空行/注释行），并标出嵌套 def 占的行。 */
  void code_line_numbers(const Decl& d, std::vector<int>& lines, std::set<int>& nested) const;

  std::string src_path;
  std::vector<std::string> code_lines;
  std::vector<std::string> raw_lines;
  std::vector<Decl> decls;
  std::vector<std::string> decl_errors;
  std::vector<std::pair<int, std::string>> top_unsupported;
  std::vector<Tok> toks;
  std::set<std::string> type_names;
  std::map<std::string, std::vector<int>> by_name;
  std::set<std::string> live;
};

Stats stats_for(const Frontend& fe, const Decl& d, const FunctionDef& fd);

}  // namespace simplexc
