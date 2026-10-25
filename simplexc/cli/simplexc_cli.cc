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
 *                [--objs-dir build/objs] [--entry 0xXXXX] [--list-unsupported] [--verbose]
 * 退出码：0 成功；1 编译/装配失败（含 unsupported）；2 读不了文件或参数问题。
 */
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "assemble/assemble.hh"
#include "cli/win32_export.hh"
#include "codegen/codegen.hh"
#include "codegen/module_build.hh"
#include "codegen/module_source.hh"
#include "frontend/frontend.hh"
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
  /* `--list-unsupported` 还在命令行里（调用点与文档都还在用它），但它列不出清单了：
     sema 遇到第一个未支持构造就抛 `CompileError`，没有「把全部缺口收齐再报」这一档。
     保留参数是为了让旧调用方不至于撞上「不认识的参数」的 rc=2。 */
  if (list_unsupported) {
    std::fprintf(stderr, "[cli] --list-unsupported：sema 遇首个未支持构造即失败，不再收集清单\n");
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

    lw::OwnedModule owned = lw::build_module(fe, src);
    auto t1 = Clock::now();
    if (verbose) {
      std::fprintf(stderr, "[cli] decls=%zu defs=%zu top_vars=%zu enums=%zu structs=%zu label=%s\n",
                   fe.decls.size(), owned.ast.defs.size(), owned.ast.top_vars.size(),
                   owned.ast.enums.size(), owned.ast.structs.size(),
                   owned.ast.label ? owned.ast.label->name.c_str() : "(无)");
    }

    const sm::Hir hir = sm::analyze(cg::module_source_of(owned.ast));
    cg::Program prog(hir);
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
