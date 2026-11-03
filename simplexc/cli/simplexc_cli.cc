/* simplexc-cli：`.si` → blob —— M4.5 的 C++ 产品路径入口（不碰游戏，也不经 Python）。
 *
 * 目录分工（接口冻结版 §0）：`frontend/` 出 AST、`sema/` 出 HIR、`codegen/` 出 COFF、
 * `assemble/` 出 blob；本文件只做**接线**：源 → 前端产物 → `sema::analyze` → `codegen::Program`
 * → `assemble()`。
 *
 * 为什么源 → 前端产物的那段不在这里：壳（`shell/simplexc_core.cc`）要的是同一段，
 * 两个调用方共用 `codegen/module_build.{hh,cc}`（接口冻结版 §0「一处定义」）。
 *
 * 用法：
 *   simplexc-cli --src <file.si> [--out <blob>] [--ir-out <ll>] [--obj-out <coff>]
 *                [--objs-dir simplexc/build/objs] [--entry 0xXXXX] [--list-unsupported] [--verbose]
 * 退出码：0 成功；1 编译/装配失败（含 unsupported）；2 读不了文件或参数问题。
 */
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assemble/assemble.hh"
#include "cli/win32_export.hh"
#include "codegen/codegen.hh"
#include "codegen/module_build.hh"
#include "codegen/module_source.hh"
#include "frontend/frontend.hh"
#include "frontend/json.hh"
#include "frontend/parser.hh"
#include "sema/analyze.hh"

namespace sx = simplexc;
namespace cg = simplexc::codegen;
namespace lw = simplexc::lower;
namespace sm = simplexc::sema;

namespace {

// ---------------------------------------------------------------- 外部符号
/* 与 `cli/tcblob.cc` 同形（那边多了个自测探针分支，这里不需要）：查名字的细节在
   `cli/win32_export.hh`，`__imp_X` 的剥离也在那里。 */
class Win32Resolver : public tc::ExternResolver {
 public:
  bool resolve(std::string_view symbol, uint64_t& addr) override {
    return simplexc_cli::resolve_win32_export(symbol, addr);
  }
};

// ---------------------------------------------------------------- 源 → 前端产物
/* 源 → 前端产物的那一段粘合（`OwnedModule` + `build_module`）住在
   `codegen/module_build.{hh,cc}` —— 壳（`shell/simplexc_core.cc`）与这里**共用同一份**，
   不许各留一份实现（接口冻结版 §0）。本文件只负责把它接起来。 */


bool read_file(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 0) {
    std::fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(n));
  const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  return got == out.size();
}

bool write_file(const std::string& path, const void* data, size_t n) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const size_t put = n == 0 ? 0 : std::fwrite(data, 1, n, f);
  std::fclose(f);
  return put == n;
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
  /* blob 的 sha256（`--offsets-out` 的 artifact 指纹）：口径 = 整份文件、含 8 字节头，与 hook
     manifest 的 `out_sha256` 同一个东西。用系统 CNG 而不是自己写一份哈希：这个数不对时症状是
     「分析的那份产物」与「跑的那份」静默错配，而这里没有第二个消费者值得养一份实现。 */
  if (data.size() > 0xFFFFFFFFull) return std::string();
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return std::string();
  BCRYPT_HASH_HANDLE hash = nullptr;
  uint8_t digest[32];
  bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                           static_cast<ULONG>(data.size()), 0) >= 0 &&
            BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
  if (hash != nullptr) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  if (!ok) return std::string();
  static const char* kHex = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (uint8_t b : digest) {
    hex.push_back(kHex[b >> 4]);
    hex.push_back(kHex[b & 0x0F]);
  }
  return hex;
}

// ---------------------------------------------------------------- 元件表（el.tsv）
/* 元件 = `.si` 里「注释行引领的一段代码」。注释行格式（hotspot spec 2026-11-01 §2）是
   `// <第一段数字> com_<名字> <第三段=位宽> <大数字> [可选语义词]`，但段数可变（中间可能有
   `LATE`，末尾可能有 `BANK`/`Input`/`Output`，id 可为 0 或负数），所以按**宽容取值**：
     · type  = 第一个 `com_\w+` 词；
     · id    = com_* 词之前的第一个整数 token；
     · width = com_* 词之后的第一个整数 token；
     · tail  = width 之后的第一个整数 token（原样存文本，`4Kb` 这类词跳过）。
   取不齐（如 `// com_level_output_switched` 这种只留类型名的注释）就不算元件注释行。 */
struct ElementSegment {
  std::string id, type, width, tail;
  int line_lo = 0; /* 1 基，含 */
  int line_hi = 0; /* 1 基，不含 */
};

bool is_integer_token(std::string_view t) {
  if (t.empty()) return false;
  if (t[0] == '-' || t[0] == '+') t.remove_prefix(1);
  if (t.empty()) return false;
  return t.find_first_not_of("0123456789") == std::string_view::npos;
}

bool is_com_token(std::string_view t) {
  if (t.rfind("com_", 0) != 0 || t.size() == 4) return false;
  return t.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", 4) ==
         std::string_view::npos;
}

std::string_view trim_left(std::string_view s) {
  const std::size_t i = s.find_first_not_of(" \t\r\n");
  return i == std::string_view::npos ? std::string_view() : s.substr(i);
}

std::vector<std::string_view> split_tokens(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const std::size_t b = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    if (i > b) out.push_back(s.substr(b, i - b));
  }
  return out;
}

/* `// …` 行才算注释行（行内尾注释不是）——它是元件行区间的边界。 */
bool is_comment_line(std::string_view line) {
  const std::string_view t = trim_left(line);
  return t.size() >= 2 && t[0] == '/' && t[1] == '/';
}

/* 顶格 `}` = 顶层的块边界（def / `run_sim` 的结尾）。元件的机器码绝不会跨过它，而末元件的
   行区间如果只按「下一个注释行」截，会一路吞到下一个 def 里 —— 那是「末元件吞公共代码」的
   历史坑（虚高 89%，docs/facts/perf-tooling.md）。 */
bool is_top_block_close(std::string_view line) {
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
    line.remove_suffix(1);
  return line == "}";
}

bool element_comment(std::string_view line, ElementSegment& out) {
  if (!is_comment_line(line)) return false;
  const std::vector<std::string_view> toks = split_tokens(trim_left(line).substr(2));
  std::size_t type_at = toks.size();
  for (std::size_t k = 0; k < toks.size(); ++k) {
    if (is_com_token(toks[k])) {
      type_at = k;
      break;
    }
  }
  if (type_at == toks.size()) return false;
  std::size_t id_at = toks.size();
  for (std::size_t k = 0; k < type_at; ++k) {
    if (is_integer_token(toks[k])) {
      id_at = k;
      break;
    }
  }
  std::size_t width_at = toks.size();
  for (std::size_t k = type_at + 1; k < toks.size(); ++k) {
    if (is_integer_token(toks[k])) {
      width_at = k;
      break;
    }
  }
  if (id_at == toks.size() || width_at == toks.size()) return false;
  out.id = std::string(toks[id_at]);
  out.type = std::string(toks[type_at]);
  out.width = std::string(toks[width_at]);
  out.tail.clear();
  for (std::size_t k = width_at + 1; k < toks.size(); ++k) {
    if (is_integer_token(toks[k])) {
      out.tail = std::string(toks[k]);
      break;
    }
  }
  return true;
}

/* 逐注释段产一行；行区间 = [元件注释行, 下一个注释行 / 顶层 `}` / 文件尾)。每元件「代码段
   + 绑定段」各有自己的注释行，所以同一 (type,id) 会出现多行 —— 台账合并在 Provider 做。 */
std::vector<ElementSegment> parse_elements(const std::string& text) {
  std::vector<std::string_view> lines;
  for (std::size_t i = 0; i <= text.size();) {
    const std::size_t nl = text.find('\n', i);
    const std::size_t end = nl == std::string::npos ? text.size() : nl;
    std::string_view line(text.data() + i, end - i);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    if (nl == std::string::npos) break;
    i = nl + 1;
  }
  /* 末尾换行会多切出一个空「行」：它不是源文件的行，去掉（否则 line_hi 会多 1）。 */
  if (!lines.empty() && lines.back().empty() && !text.empty() && text.back() == '\n') lines.pop_back();

  const int line_count = static_cast<int>(lines.size());
  std::vector<ElementSegment> segs;
  /* `next_bnd[L]` = L 之后（不含 L）的第一条边界行的行号，没有边界就是 N+1（文件尾）。
     边界行 = 注释行（元件行本身就是，所以元件区间到下一行注释为止）/ 顶格 `}`。 */
  std::vector<int> next_bnd(static_cast<std::size_t>(line_count) + 2, line_count + 1);
  for (int L = line_count; L >= 1; --L) {
    const int after = L + 1;
    const bool bnd = after <= line_count &&
                     (is_comment_line(lines[static_cast<std::size_t>(after) - 1]) ||
                      is_top_block_close(lines[static_cast<std::size_t>(after) - 1]));
    next_bnd[static_cast<std::size_t>(L)] = bnd ? after : next_bnd[static_cast<std::size_t>(after)];
  }
  for (int L = 1; L <= line_count; ++L) {
    ElementSegment seg;
    if (!element_comment(lines[static_cast<std::size_t>(L) - 1], seg)) continue;
    seg.line_lo = L;
    seg.line_hi = next_bnd[static_cast<std::size_t>(L)];
    segs.push_back(std::move(seg));
  }
  return segs;
}

bool write_elements(const std::string& path, const std::vector<ElementSegment>& segs,
                    int line_offset) {
  std::string out = "# el.tsv v1\n";
  for (const ElementSegment& s : segs) {
    out += s.id;
    out += '\t';
    out += s.type;
    out += '\t';
    out += s.width;
    out += '\t';
    out += s.tail;
    out += '\t';
    out += std::to_string(s.line_lo + line_offset);
    out += '\t';
    out += std::to_string(s.line_hi + line_offset);
    out += '\n';
  }
  return write_file(path, out.data(), out.size());
}

// ---------------------------------------------------------------- 节布局（offsets JSON）
/* `--offsets-out`：把「对象 `.text` 节内偏移」换算成 blob 偏移所需的两样东西（hotspot spec §6）：
   字节级节布局 + 行表的锚点。为什么必须另吐一个文件：行程序的首址是一条 `ADDR64 → .text`
   重定位（对象节 VMA=0），而装配器对 `.debug*` 整节丢弃 —— 对象里的地址在产物里没有直接对应，
   W3 只能靠「这个对象的 `.text` 被摆在 blob 哪里」推回去。 */
bool write_offsets(const std::string& path, const std::vector<uint8_t>& blob,
                   const std::vector<std::string>& obj_names, const tc::AsmResult& r,
                   uint64_t entry, std::string* err) {
  const std::string sha = sha256_hex(blob);
  if (sha.empty()) {
    *err = "算不出产物的 sha256（BCrypt 失败）";
    return false;
  }
  /* 首版只接受「一个带行表的对象有且只有一个 `.text` 节」。多节时锚点要按重定位逐条展开，
     而那套展开没实现 —— 与其静默取第一条让下游偏移少算一截，不如当场报错。 */
  sx::Json debug_lines = sx::Json::array();
  for (const uint32_t obj_index : r.debug_line_objs) {
    sx::Json anchors = sx::Json::array();
    for (const tc::AsmResult::SectionPlacement& s : r.sections) {
      if (s.obj != obj_index || s.cls != "text") continue;
      sx::Json a = sx::Json::object();
      a.set("text_sec", sx::Json::integer(static_cast<long long>(s.sec)));
      a.set("blob_off", sx::Json::integer(static_cast<long long>(s.blob_off)));
      anchors.push(a);
    }
    if (anchors.size() != 1) {
      *err = "对象 " + std::to_string(obj_index) + " 有 " + std::to_string(anchors.size()) +
             " 个 .text 节（首版只支持 1 个）";
      return false;
    }
    sx::Json d = sx::Json::object();
    d.set("obj", sx::Json::integer(static_cast<long long>(obj_index)));
    d.set("obj_path", sx::Json::string(obj_index < obj_names.size() ? obj_names[obj_index]
                                                                     : std::string()));
    d.set("anchors", std::move(anchors));
    debug_lines.push(std::move(d));
  }

  sx::Json sections = sx::Json::array();
  for (const tc::AsmResult::SectionPlacement& s : r.sections) {
    sx::Json e = sx::Json::object();
    e.set("obj", sx::Json::integer(static_cast<long long>(s.obj)));
    e.set("obj_path",
          sx::Json::string(s.obj < obj_names.size() ? obj_names[s.obj] : std::string()));
    e.set("sec", sx::Json::integer(static_cast<long long>(s.sec)));
    e.set("name", sx::Json::string(s.name));
    e.set("class", sx::Json::string(s.cls));
    e.set("blob_off", sx::Json::integer(static_cast<long long>(s.blob_off)));
    e.set("size", sx::Json::integer(static_cast<long long>(s.size)));
    e.set("align", sx::Json::integer(static_cast<long long>(s.align)));
    sections.push(std::move(e));
  }

  sx::Json artifact = sx::Json::object();
  artifact.set("len", sx::Json::integer(static_cast<long long>(blob.size())));
  artifact.set("sha256", sx::Json::string(sha));

  sx::Json root = sx::Json::object();
  root.set("version", sx::Json::integer(1));
  root.set("artifact", std::move(artifact));
  root.set("header_len", sx::Json::integer(8));
  root.set("code_size", sx::Json::integer(static_cast<long long>(r.blob.code_size())));
  root.set("entry", sx::Json::integer(static_cast<long long>(entry)));
  root.set("body_end", sx::Json::integer(static_cast<long long>(r.body_end)));
  root.set("stubs_base", sx::Json::integer(static_cast<long long>(r.stubs_base)));
  root.set("slots_base", sx::Json::integer(static_cast<long long>(r.slots_base)));
  root.set("sections", std::move(sections));
  root.set("debug_lines", std::move(debug_lines));

  const std::string text = root.dump(2) + "\n";
  return write_file(path, text.data(), text.size());
}

void usage() {
  std::fprintf(stderr,
               "用法：simplexc-cli --src <file.si> [--stdlib <stdlib_prefix.si>] [--out <blob>]\n"
               "                    [--ir-out <ll>] [--obj-out <coff>] [--elements-out <el.tsv>]\n"
               "                    [--offsets-out <json>] [--debug-info] [--objs-dir simplexc/build/objs]\n"
               "                    [--entry 0xXXXX] [--list-unsupported] [--verbose]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src, stdlib_path, out_path, ir_path, obj_path, elements_path, offsets_path;
  std::string objs_dir = "simplexc/build/objs";
  bool list_unsupported = false, verbose = false, timing = false, debug_info = false;
  uint64_t entry_override = 0;
  bool has_entry_override = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s 缺参数\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--src") {
      const char* p = need("--src");
      if (!p) return 2;
      src = p;
    } else if (a == "--out") {
      const char* p = need("--out");
      if (!p) return 2;
      out_path = p;
    } else if (a == "--ir-out") {
      const char* p = need("--ir-out");
      if (!p) return 2;
      ir_path = p;
    } else if (a == "--obj-out") {
      const char* p = need("--obj-out");
      if (!p) return 2;
      obj_path = p;
    } else if (a == "--debug-info") {
      debug_info = true;
    } else if (a == "--stdlib") {
      const char* p = need("--stdlib");
      if (!p) return 2;
      stdlib_path = p;
    } else if (a == "--elements-out") {
      const char* p = need("--elements-out");
      if (!p) return 2;
      elements_path = p;
    } else if (a == "--offsets-out") {
      const char* p = need("--offsets-out");
      if (!p) return 2;
      offsets_path = p;
    } else if (a == "--objs-dir") {
      const char* p = need("--objs-dir");
      if (!p) return 2;
      objs_dir = p;
    } else if (a == "--entry") {
      const char* p = need("--entry");
      if (!p) return 2;
      entry_override = std::strtoull(p, nullptr, 0);
      has_entry_override = true;
    } else if (a == "--list-unsupported") {
      list_unsupported = true;
    } else if (a == "--verbose") {
      verbose = true;
    } else if (a == "--timing") {
      timing = true;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "不认识的参数：%s\n", a.c_str());
      usage();
      return 2;
    }
  }
  if (src.empty()) {
    usage();
    return 2;
  }
  /* `--offsets-out` 要写 artifact 指纹与 blob 布局，没有产物就没东西可写 —— 这是调用方漏了
     参数，不是编译失败，所以报在跑编译之前。 */
  if (!offsets_path.empty() && out_path.empty()) {
    std::fprintf(stderr, "--offsets-out 需要 --out（要拿整份产物算 sha256 与节布局）\n");
    return 2;
  }
  /* `--list-unsupported` 还在命令行里（调用点与文档都还在用它），但它列不出清单了：
     sema 遇到第一个未支持构造就抛 `CompileError`，没有「把全部缺口收齐再报」这一档。
     保留参数是为了让旧调用方不至于撞上「不认识的参数」的 rc=2。 */
  if (list_unsupported) {
    std::fprintf(stderr, "[cli] --list-unsupported：sema 遇首个未支持构造即失败，不再收集清单\n");
  }

  try {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    /* 元件表只依赖源文件原文（注释在 `mask_source` 里被抹成空格，raw_lines 里也没有原文），
       所以它在前端跑之前就能落盘：编译失败时这份表依然是对的。
       `--stdlib` 把前缀拼在源码前，于是源码第 1 行变成第 `line_offset + 1` 行 —— el.tsv 的行区间
       必须跟着后移，否则「元件表推的行」与 `.debug_line` 里的行差一个前缀。 */
    std::string src_text;
    if (!elements_path.empty() || !stdlib_path.empty()) {
      if (!lw::read_source_file(src, &src_text)) {
        std::fprintf(stderr, "读不了 %s\n", src.c_str());
        return 2;
      }
    }
    std::string spliced;
    int line_offset = 0;
    if (!stdlib_path.empty()) {
      std::string prefix;
      if (!lw::read_source_file(stdlib_path, &prefix)) {
        std::fprintf(stderr, "读不了 stdlib 前缀 %s\n", stdlib_path.c_str());
        return 2;
      }
      /* 传**拷贝**：`src_text` 还要给元件表用（拼后的文本只喂前端）。 */
      const lw::SplicedSource sp = lw::splice_prefix(prefix, src_text);
      spliced = sp.text;
      line_offset = sp.line_offset;
      if (verbose) {
        std::fprintf(stderr, "[cli] stdlib %zu B（%d 行）拼在源码前 → %zu B\n", prefix.size(),
                     line_offset, spliced.size());
      }
    }
    if (!elements_path.empty()) {
      const std::vector<ElementSegment> segs = parse_elements(src_text);
      if (!write_elements(elements_path, segs, line_offset)) {
        std::fprintf(stderr, "写不了元件表 %s\n", elements_path.c_str());
        return 2;
      }
      if (verbose) std::fprintf(stderr, "[cli] 元件段 %zu 条 → %s\n", segs.size(), elements_path.c_str());
    }
    /* 没拼前缀就走**路径**入口（与不带 `--stdlib` 的旧行为逐字节相同：`SrcText` 入口会把
       `src_path` 填成 `<memory>`，报错文本与 `.debug_line` 的文件名都会变）。 */
    std::optional<sx::Frontend> fe_storage;
    if (stdlib_path.empty()) {
      fe_storage.emplace(src);
    } else {
      fe_storage.emplace(sx::SrcText{std::move(spliced)});
    }
    const sx::Frontend& fe = *fe_storage;
    if (!fe.decl_errors.empty()) {
      std::string msg;
      for (const std::string& e : fe.decl_errors) {
        if (!msg.empty()) msg += "；";
        msg += e;
      }
      throw lw::CompileFail("声明层错误：" + msg);
    }

    lw::OwnedModule owned = lw::build_module(fe, src);
    auto t1 = Clock::now();
    if (verbose) {
      std::fprintf(stderr, "[cli] decls=%zu defs=%zu top_vars=%zu enums=%zu structs=%zu label=%s\n",
                   fe.decls.size(), owned.ast.defs.size(), owned.ast.top_vars.size(),
                   owned.ast.enums.size(), owned.ast.structs.size(),
                   owned.ast.label ? owned.ast.label->name.c_str() : "(无)");
    }

    sm::ModuleSource ms = cg::module_source_of(owned.ast);
    /* 拼没拼前缀要让 sema 知道：`str` / `hex` / `bin` 的硬编码实现只在**没拼**时注册
       （`Analyzer::call_formatter`）—— 无条件注册会改掉 `stdlib = on` 的产物字节。 */
    ms.stdlib_prefix_included = !stdlib_path.empty();
    const sm::Hir hir = sm::analyze(ms);
    cg::CodegenOptions copts;
    copts.debug_info = debug_info;
    copts.source_path = src;
    cg::Program prog(hir, copts);
    prog.build();
    auto t2 = Clock::now();

    const std::string ir = prog.ir_text();
    if (!ir_path.empty()) {
      if (!write_file(ir_path, ir.data(), ir.size())) {
        std::fprintf(stderr, "写不了 IR 文件 %s\n", ir_path.c_str());
        return 2;
      }
      if (verbose) std::fprintf(stderr, "[cli] IR %zu B → %s\n", ir.size(), ir_path.c_str());
    }

    std::vector<uint8_t> obj = prog.emit_object();
    auto t3 = Clock::now();
    if (!obj_path.empty()) {
      if (!write_file(obj_path, obj.data(), obj.size())) {
        std::fprintf(stderr, "写不了 COFF 文件 %s\n", obj_path.c_str());
        return 2;
      }
      if (verbose) std::fprintf(stderr, "[cli] COFF %zu B → %s\n", obj.size(), obj_path.c_str());
    }
    if (out_path.empty()) {
      if (timing) {
        auto ms = [](Clock::duration d) {
          return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
        };
        std::fprintf(stderr, "[timing] 前端=%lld ms  lowering=%lld ms  LLVM(O2+代码生成)=%lld ms  合计=%lld ms\n",
                     static_cast<long long>(ms(t1 - t0)), static_cast<long long>(ms(t2 - t1)),
                     static_cast<long long>(ms(t3 - t2)), static_cast<long long>(ms(t3 - t0)));
      }
      return 0;
    }

    /* `--offsets-out` 的 `obj_path` 用本侧的名字（`ObjectFile::name` 是报错用的，`module`
       那个不吃路径），所以这里同时留一份与 `objs` 平行的名字表。 */
    std::vector<std::string> obj_names;
    std::vector<tc::ObjectFile> objs;
    obj_names.push_back(obj_path.empty() ? std::string("module.obj") : obj_path);
    objs.push_back({"module", std::move(obj)});
    for (const char* name : {"entry_shim.o", "rt_min.o"}) {
      const std::string p = objs_dir + "/" + name;
      tc::ObjectFile of;
      of.name = p;
      if (!read_file(p.c_str(), of.bytes)) {
        std::fprintf(stderr, "读不了 %s（先跑 python simplexc/tools/build.py objs）\n", p.c_str());
        return 2;
      }
      obj_names.push_back(p);
      objs.push_back(std::move(of));
    }
    Win32Resolver resolver;
    tc::AsmOptions opt;
    opt.verbose = verbose;
    tc::AsmResult r = tc::assemble(objs, "__tc_entry", resolver, opt);
    if (r.status != tc::AsmStatus::Ok) {
      std::fprintf(stderr, "装配失败：status=%d %s\n", static_cast<int>(r.status), r.detail.c_str());
      return 1;
    }
    if (!write_file(out_path, r.blob.bytes.data(), r.blob.bytes.size())) {
      std::fprintf(stderr, "写不了产物 %s\n", out_path.c_str());
      return 2;
    }
    uint64_t entry = has_entry_override ? entry_override : r.blob.entry_off;
    {
      FILE* fe = std::fopen((out_path + ".entry").c_str(), "w");
      if (fe) {
        std::fprintf(fe, "%llX\n", static_cast<unsigned long long>(entry));
        std::fclose(fe);
      }
    }
    {
      FILE* fs = std::fopen((out_path + ".stubs").c_str(), "w");
      if (fs) {
        std::fprintf(fs, "offset\tkind\tsymbol\n");
        for (const tc::StubRef& s : r.stubs) {
          std::fprintf(fs, "%llx\t%s\t%s\n", static_cast<unsigned long long>(s.offset),
                       s.symbol.rfind("__imp_", 0) == 0 ? "slot" : "stub", s.symbol.c_str());
        }
        std::fclose(fs);
      }
    }
    if (!offsets_path.empty()) {
      std::string why;
      if (!write_offsets(offsets_path, r.blob.bytes, obj_names, r, entry, &why)) {
        std::fprintf(stderr, "写不了节布局 %s：%s\n", offsets_path.c_str(), why.c_str());
        return 2;
      }
      if (verbose)
        std::fprintf(stderr, "[cli] 节 %zu 个、带行表的对象 %zu 个 → %s\n", r.sections.size(),
                     r.debug_line_objs.size(), offsets_path.c_str());
    }
    std::cout << "blob " << r.blob.bytes.size() << " B（机器码 " << r.blob.code_size() << "）→ "
              << out_path << "（entry=" << std::hex << std::uppercase << entry << std::dec << "）"
              << std::endl;
    if (timing) {
      auto t4 = Clock::now();
      auto ms = [](Clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
      };
      std::fprintf(stderr, "[timing] 前端=%lld ms  lowering=%lld ms  LLVM(O2+代码生成)=%lld ms  装配=%lld ms  合计=%lld ms\n",
                   static_cast<long long>(ms(t1 - t0)), static_cast<long long>(ms(t2 - t1)),
                   static_cast<long long>(ms(t3 - t2)), static_cast<long long>(ms(t4 - t3)),
                   static_cast<long long>(ms(t4 - t0)));
    }
    return 0;
  } catch (const lw::Unsupported& e) {
    std::fprintf(stderr, "UNSUPPORTED: %s @ 第 %d 行\n", e.what(), e.line());
    return 1;
  } catch (const sm::CompileError& e) {
    std::fprintf(stderr, "编译失败：%s\n", e.what());
    return 1;
  } catch (const lw::CompileFail& e) {
    std::fprintf(stderr, "编译失败：%s\n", e.what());
    return 1;
  } catch (const sx::ParseError& e) {
    std::fprintf(stderr, "语法错误：第 %d 行 第 %d 列：%s\n", e.line, e.col, e.msg.c_str());
    return 1;
  } catch (const sx::ReadError& e) {
    std::fprintf(stderr, "读不了 %s：%s\n", src.c_str(), e.msg.c_str());
    return 2;
  } catch (const sx::LexError& e) {
    std::fprintf(stderr, "词法错误：第 %d 行 第 %d 列：%s\n", e.line, e.col, e.msg.c_str());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "内部错误：%s\n", e.what());
    return 1;
  }
}
