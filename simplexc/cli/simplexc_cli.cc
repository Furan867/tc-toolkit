/* simplexc-cli：`.si` → blob —— M4.5 的 C++ 产品路径入口（不碰游戏，也不经 Python）。
 *
 * 目录分工（接口冻结版 §0）：`frontend/` 出 AST、`lower/` 出 COFF、`assemble/` 出 blob；
 * 本文件只做**接线**：源 → `ModuleAst` → `Program` → `assemble()`。
 *
 * 为什么 `ModuleAst` 要在这里组装：`lower::Program` 拿的是**已解析**的 `ModuleAst`
 * （`label`/`defs`/`top_vars`/`enums`/`structs` 都由调用方填），源文件到它之间的那段
 * （原型 `ir_build.py` 的 `_scan_decls` / `_scan_type` / `parse_decl_stmt`）属于「拿前端产物拼模块」，
 * 不归 lowering、也不归 parser。
 *
 * 语义 oracle = `ir_build.py` 的同名三段（只读）。差别只在于：那边用正则，这边手写扫描 ——
 * 因为 type 声明的形态在生成代码子集里只有 `type X Enum [a, b]` 与 `type X Struct { f: T, … }` 两种。
 *
 * 用法：
 *   simplexc-cli --src <file.si> [--out <blob>] [--ir-out <ll>] [--obj-out <coff>]
 *                [--objs-dir build/objs] [--entry 0xXXXX] [--list-unsupported] [--verbose]
 * 退出码：0 成功；1 编译/装配失败（含 unsupported）；2 读不了文件或参数问题。
 */
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "assemble/assemble.hh"
#include "frontend/ast.hh"
#include "frontend/decls.hh"
#include "frontend/frontend.hh"
#include "frontend/lexer.hh"
#include "frontend/parser.hh"
#include "lower/lower.hh"

namespace sx = simplexc;
namespace lw = simplexc::lower;

namespace {

// ---------------------------------------------------------------- 外部符号
/* 与 `cli/tcblob.cc` 的 `Kernel32Resolver` 同形（那边多了个自测探针分支，这里不需要）。
   `__imp_X` 是 mingw 对 dllimport 的写法：装配器把它造成 8 字节数据槽，要查的仍是 `X`。 */
class Kernel32Resolver : public tc::ExternResolver {
 public:
  bool resolve(std::string_view symbol, uint64_t& addr) override {
    std::string name(symbol);
    const std::string imp = "__imp_";
    if (name.rfind(imp, 0) == 0) name = name.substr(imp.size());
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!k) return false;
    FARPROC p = GetProcAddress(k, name.c_str());
    if (!p) {
      std::fprintf(stderr, "外部符号解析失败：kernel32!%s\n", name.c_str());
      return false;
    }
    addr = reinterpret_cast<uint64_t>(p);
    return true;
  }
};

// ---------------------------------------------------------------- 源 → ModuleAst
/* `ModuleAst` 只持裸指针（它的注释写明「调用方保证生命周期」），所以整棵树由这里持有。 */
struct OwnedAst {
  lw::ModuleAst ast;
  std::vector<std::unique_ptr<sx::FunctionDef>> fns;
  std::vector<std::unique_ptr<sx::VarDeclStmt>> vars;
  std::vector<std::unique_ptr<sx::TypeRef>> refs;

  sx::FunctionDef* keep(std::unique_ptr<sx::FunctionDef> fn) {
    fns.push_back(std::move(fn));
    return fns.back().get();
  }
  const sx::TypeRef* keep_type(std::unique_ptr<sx::TypeRef> t) {
    refs.push_back(std::move(t));
    return refs.back().get();
  }
};

std::string join_lines(const std::vector<std::string>& lines, int lo, int hi) {
  std::string out;
  for (int i = lo - 1; i < hi && i < static_cast<int>(lines.size()); ++i) {
    if (i >= 0) {
      if (!out.empty()) out += ' ';
      out += lines[static_cast<size_t>(i)];
    }
  }
  return out;
}

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

/* 跳过空白（type 声明可能被摆成多行）。 */
size_t skip_ws(const std::string& s, size_t i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return i;
}

std::string read_ident(const std::string& s, size_t& i) {
  size_t b = i;
  if (i < s.size() && is_ident_start(s[i])) {
    ++i;
    while (i < s.size() && is_ident_char(s[i])) ++i;
  }
  return s.substr(b, i - b);
}

/* 字段/成员的类型拼写：`U32` / `@Size`（接口 §1 的 `TypeRef` 只存拼写）。 */
std::unique_ptr<sx::TypeRef> make_type_ref(const std::string& word) {
  auto t = std::make_unique<sx::TypeRef>();
  if (!word.empty() && word[0] == '@') {
    t->at = true;
    t->name = word.substr(1);
  } else {
    t->name = word;
  }
  return t;
}

/* `type X Enum [A, B, C]` 与 `type X Struct { f: T, g: U }` —— 生成代码子集里只有这两种。 */
void scan_type(const std::vector<std::string>& code_lines, const sx::Decl& d, OwnedAst& out) {
  const std::string text = join_lines(code_lines, d.lo, d.hi);
  if (d.note == "Enum") {
    const size_t lb = text.find("Enum");
    const size_t l = lb == std::string::npos ? std::string::npos : text.find('[', lb);
    const size_t r = text.rfind(']');
    if (l == std::string::npos || r == std::string::npos || r < l) {
      throw lw::CompileFail("第 " + std::to_string(d.lo) + " 行：枚举声明看不懂：" + text);
    }
    lw::EnumDecl e;
    e.name = d.name;
    std::stringstream ss(text.substr(l + 1, r - l - 1));
    std::string item;
    while (std::getline(ss, item, ',')) {
      size_t i = skip_ws(item, 0);
      std::string mem = read_ident(item, i);
      if (!mem.empty()) e.members.push_back(mem);
    }
    out.ast.enums.push_back(std::move(e));
    return;
  }
  if (d.note == "Struct") {
    const size_t l = text.find('{');
    const size_t r = text.rfind('}');
    if (l == std::string::npos || r == std::string::npos || r < l) {
      throw lw::CompileFail("第 " + std::to_string(d.lo) + " 行：结构声明看不懂：" + text);
    }
    lw::StructDecl s;
    s.name = d.name;
    const std::string body = text.substr(l + 1, r - l - 1);
    size_t pos = 0;
    while (pos < body.size()) {
      size_t comma = body.find(',', pos);
      const std::string part = body.substr(pos, comma == std::string::npos ? std::string::npos
                                                                          : comma - pos);
      size_t i = skip_ws(part, 0);
      std::string fname = read_ident(part, i);
      if (!fname.empty()) {
        i = skip_ws(part, i);
        if (i < part.size() && part[i] == ':') {
          i = skip_ws(part, i + 1);
          size_t j = i;
          if (j < part.size() && part[j] == '@') ++j;
          std::string tname = read_ident(part, j);
          if (!tname.empty()) {
            const std::string word = (i < part.size() && part[i] == '@') ? ("@" + tname) : tname;
            s.fields.emplace_back(fname, out.keep_type(make_type_ref(word)));
          }
        }
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    out.ast.structs.push_back(std::move(s));
    return;
  }
  throw lw::Unsupported("type 声明形态（" + d.note + "）", d.lo);
}

/* 顶层 `var`/`const`：切出这个声明占的行上的 token，交给 parser 当一条语句解析
   （原型 `parse_decl_stmt` 就是这么做的 —— 不另写一份声明解析）。 */
std::unique_ptr<sx::VarDeclStmt> parse_top_var(const sx::Frontend& fe, const sx::Decl& d) {
  std::vector<sx::Tok> toks;
  for (const sx::Tok& t : fe.toks) {
    if (t.line >= d.lo && t.line <= d.hi) toks.push_back(t);
  }
  sx::Tok eof;
  eof.kind = "eof";
  eof.line = d.hi;
  eof.col = 1;
  eof.gap = true;
  toks.push_back(eof);
  sx::Parser p(std::move(toks), fe.type_names);
  std::unique_ptr<sx::Stmt> st = p.parse_statement();
  auto* vd = sx::as<sx::VarDeclStmt>(st.get());
  if (vd == nullptr) {
    throw lw::CompileFail("第 " + std::to_string(d.lo) + " 行：顶层 " + d.kind +
                          " 不是 var/const 声明");
  }
  return std::unique_ptr<sx::VarDeclStmt>(static_cast<sx::VarDeclStmt*>(st.release()));
}

/* 按声明表拼出 `ModuleAst`。**只解析可达 def** —— 不可达的 def 里可能有语法错
   （`rv32im.si:154` 的多余 `)`），原型也不解析它们；`Frontend` 已经算好 `live`。 */
void build_module(const sx::Frontend& fe, const std::string& src_path, OwnedAst& out) {
  for (size_t i = 0; i < fe.decls.size(); ++i) {
    const sx::Decl& d = fe.decls[i];
    const int idx = static_cast<int>(i);
    if (d.kind == "type" && d.depth == 0) {
      scan_type(fe.code_lines, d, out);
    } else if (d.kind == "def" && d.depth == 0) {
      if (fe.live.count(d.name) == 0) continue; /* 不可达：不解析、不进模块 */
      sx::ParsedDef pd = fe.parse_def(idx);
      out.ast.defs.push_back(out.keep(std::move(pd.fn)));
    } else if (d.kind == "label") {
      if (out.ast.label != nullptr) {
        throw lw::CompileFail("一个文件里有两个顶层语句块（程序根只能有一个）");
      }
      sx::ParsedDef pd = fe.parse_def(idx);
      out.ast.label = out.keep(std::move(pd.fn));
    } else if ((d.kind == "var" || d.kind == "const") && d.depth == 0) {
      out.vars.push_back(parse_top_var(fe, d));
      out.ast.top_vars.push_back(out.vars.back().get());
    }
  }
  out.ast.source_name = src_path;
}

// ---------------------------------------------------------------- 文件
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

std::string dir_of(const std::string& p) {
  const size_t k = p.find_last_of("/\\");
  return k == std::string::npos ? std::string(".") : p.substr(0, k);
}

void usage() {
  std::fprintf(stderr,
               "用法：simplexc-cli --src <file.si> [--out <blob>] [--ir-out <ll>] [--obj-out <coff>]\n"
               "                    [--objs-dir build/objs] [--entry 0xXXXX] [--list-unsupported] [--verbose]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src, out_path, ir_path, obj_path, objs_dir = "build/objs";
  bool list_unsupported = false, verbose = false, timing = false;
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

  try {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    sx::Frontend fe(src);
    if (!fe.decl_errors.empty()) {
      std::string msg;
      for (const std::string& e : fe.decl_errors) {
        if (!msg.empty()) msg += "；";
        msg += e;
      }
      throw lw::CompileFail("声明层错误：" + msg);
    }

    OwnedAst owned;
    build_module(fe, src, owned);
    auto t1 = Clock::now();
    if (verbose) {
      std::fprintf(stderr, "[cli] decls=%zu defs=%zu top_vars=%zu enums=%zu structs=%zu label=%s\n",
                   fe.decls.size(), owned.ast.defs.size(), owned.ast.top_vars.size(),
                   owned.ast.enums.size(), owned.ast.structs.size(),
                   owned.ast.label ? owned.ast.label->name.c_str() : "(无)");
    }

    lw::Program prog(owned.ast, list_unsupported);
    prog.build();
    auto t2 = Clock::now();
    if (list_unsupported && !prog.diagnostics().empty()) {
      for (const lw::Diagnostic& d : prog.diagnostics()) {
        std::printf("UNSUPPORTED: %s @ 第 %d 行\n", d.msg.c_str(), d.line);
      }
      return 3;
    }

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

    std::vector<tc::ObjectFile> objs;
    objs.push_back({"module", std::move(obj)});
    for (const char* name : {"entry_shim.o", "rt_min.o"}) {
      const std::string p = objs_dir + "/" + name;
      tc::ObjectFile of;
      of.name = p;
      if (!read_file(p.c_str(), of.bytes)) {
        std::fprintf(stderr, "读不了 %s（先跑 python simplexc/tools/build.py objs）\n", p.c_str());
        return 2;
      }
      objs.push_back(std::move(of));
    }
    Kernel32Resolver resolver;
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
    std::printf("blob %zu B（机器码 %llu）→ %s（entry=%#llX）\n", r.blob.bytes.size(),
                static_cast<unsigned long long>(r.blob.code_size()), out_path.c_str(),
                static_cast<unsigned long long>(entry));
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
