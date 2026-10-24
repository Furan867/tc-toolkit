/* sema/analyze_selftest.cc —— `sema/analyze` 的判据入口（**不在产品路径**）。
 *
 * 对 `testdata/cases/` 里每个 `.si`：`.si` → 前端 AST → `sema::analyze` → `dump(hir)`，与同名
 * `.dump` **逐字**比较；再对同一棵树跑 `check_invariants`。为什么逐字：②c 让 codegen 改吃 HIR
 * 之前，能证明「sema 侧语义没动」的硬判据就是 dump 的 diff（方案 §3）。
 *
 * golden 一律**重定向生成**，不许手抄：
 *   python simplexc/tools/build.py analyze_selftest
 *   build/analyze_selftest.exe --dump-only simplexc/sema/testdata/cases/<case>.si \
 *       > simplexc/sema/testdata/cases/<case>.dump
 * 生成之后要对着规格（方案 §2b）与 `lower.cc` 逐行核：值、类型、节点顺序、span。
 *
 * 为什么这个文件 include 了 `frontend/frontend.hh`：判据的输入是**源码**，`.si` → AST 那一段
 * 需要 `Frontend`（前端自己的模块）。`analyze.cc` 本身只吃 `frontend/ast.hh`；这一段粘合是
 * 临时的 —— ②c 接线时它搬去壳/CLI 那一侧（与 `lower::build_module` 同一个位置，两边只留一份）。
 *
 * 用例的命令行形状与后缀都是判据的一部分：`.`si` 源码 + 同名 `.dump` golden（golden 与 `.si`
 * 同目录，所以删掉一个 `.dump` 就是「少了一份期望」，不会静默跳过）。
 *
 * 退出码：0 = 全过；1 = 有失败；2 = 命令行参数不认识。
 */
#include "analyze.hh"
#include "errors.hh"
#include "hir.hh"
#include "hir_dump.hh"

#include "../frontend/ast.hh"
#include "../frontend/frontend.hh"
#include "../frontend/parser.hh"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sema = simplexc::sema;
using simplexc::Decl;
using simplexc::Frontend;
using simplexc::FunctionDef;
using simplexc::LexError;
using simplexc::ParseError;
using simplexc::ParsedDef;
using simplexc::Parser;
using simplexc::ReadError;
using simplexc::Stmt;
using simplexc::Tok;
using simplexc::VarDeclStmt;

namespace {

int g_failed = 0;

void ok(const std::string& what) { std::cout << "PASS " << what << "\n"; }

void bad(const std::string& what) {
    std::cout << "FAIL " << what << "\n";
    g_failed++;
}

/* 用例目录从 `__FILE__` 推：build.py 在仓库根用相对路径编这个文件，所以 `__FILE__` =
 * `simplexc/sema/analyze_selftest.cc`，拼出来既是仓库根下的路径、也是打印重生成命令时
 * 能原样粘回去的写法（不用传参、不怕 cwd）。 */
std::filesystem::path cases_dir() {
    std::string self = __FILE__;
    const std::size_t slash = self.find_last_of("/\\");
    self.resize(slash == std::string::npos ? 0 : slash + 1);
    return std::filesystem::path(self) / "testdata" / "cases";
}

/* `.si` → `sema::ModuleSource`。AST 节点由本结构持有（`ModuleSource` 只持裸指针）。 */
struct CaseSource {
    std::vector<std::unique_ptr<FunctionDef>> fns;
    std::vector<std::unique_ptr<VarDeclStmt>> vars;
    sema::ModuleSource src;
};

/* 顶层 `var`/`const` 的切法照 `module_build.cc` 的 `parse_top_var`：切出这条声明占的行上
 * 的 token，交给 parser 当一条**语句**解析 —— 不另写一份声明解析。 */
std::unique_ptr<VarDeclStmt> parse_top_var(const Frontend& fe, const Decl& d) {
    std::vector<Tok> toks;
    for (const Tok& t : fe.toks) {
        if (t.line >= d.lo && t.line <= d.hi) toks.push_back(t);
    }
    Tok eof;
    eof.kind = "eof";
    eof.line = d.hi;
    eof.col = 1;
    eof.gap = true;
    toks.push_back(eof);
    Parser p(std::move(toks), fe.type_names);
    std::unique_ptr<Stmt> st = p.parse_statement();
    auto* vd = simplexc::as<VarDeclStmt>(st.get());
    if (vd == nullptr) {
        throw sema::CompileFail("第 " + std::to_string(d.lo) + " 行：顶层 " + d.kind +
                               " 不是 var/const 声明");
    }
    return std::unique_ptr<VarDeclStmt>(static_cast<VarDeclStmt*>(st.release()));
}

bool load_case(const std::string& path, CaseSource* out, std::string* err) {
    try {
        const Frontend fe(path);
        for (std::size_t i = 0; i < fe.decls.size(); ++i) {
            const Decl& d = fe.decls[i];
            const int idx = static_cast<int>(i);
            if (d.kind == "label") {
                if (out->src.label != nullptr) {
                    throw sema::CompileFail("一个文件里有两个顶层语句块（程序根只能有一个）");
                }
                ParsedDef pd = fe.parse_def(idx);
                out->src.label = pd.fn.get();
                out->fns.push_back(std::move(pd.fn));
            } else if ((d.kind == "var" || d.kind == "const") && d.depth == 0) {
                out->vars.push_back(parse_top_var(fe, d));
                out->src.top_vars.push_back(out->vars.back().get());
            } else if (d.kind == "def" && d.depth == 0) {
                /* 顶层 def 本步**不接**：解析好传进去，让 `analyze` 自己抛 `Unsupported`
                   （用例里真出现时的报错才来自被测代码，而不是这段粘合）。 */
                ParsedDef pd = fe.parse_def(idx);
                out->src.defs.push_back(pd.fn.get());
                out->fns.push_back(std::move(pd.fn));
            } else {
                throw sema::CompileFail("第 " + std::to_string(d.lo) + " 行：用例里不该出现 " +
                                       d.kind + " 声明");
            }
        }
        out->src.source_name = path;
        return true;
    } catch (const ReadError& e) {
        *err = "读不了源文件：" + e.msg;
    } catch (const LexError& e) {
        *err = "第 " + std::to_string(e.line) + " 行词法错：" + e.msg;
    } catch (const ParseError& e) {
        *err = "第 " + std::to_string(e.line) + " 行语法错：" + e.msg;
    } catch (const std::exception& e) {
        *err = e.what();
    }
    return false;
}

/* golden 读成字节串时把 CRLF 折成 LF：本仓库 `core.autocrlf=true`，而 dump 只出 LF —— 折一下
 * 判的才是内容，不是行尾（与 `hir_selftest.cc` 同一条写法）。 */
bool read_golden(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    out->clear();
    out->reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
        out->push_back(raw[i]);
    }
    return true;
}

std::string regen_cmd(const std::filesystem::path& src) {
    return "build/analyze_selftest.exe --dump-only " + src.generic_string() +
           " > " + (src.parent_path() / (src.stem().string() + ".dump")).generic_string();
}

/* `--dump-only <case.si>`：只打印 dump（golden 用它生成）。 */
int dump_only(const std::string& path) {
    CaseSource cs;
    std::string err;
    if (!load_case(path, &cs, &err)) {
        std::cerr << "FAIL 载入 " << path << "：" << err << "\n";
        return 1;
    }
    try {
        std::cout << sema::dump(sema::analyze(cs.src));
    } catch (const std::exception& e) {
        std::cerr << "FAIL analyze " << path << "：" << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump-only") return dump_only(argv[2]);
    if (argc != 1) {
        std::cerr << "用法：" << argv[0] << " [--dump-only <case.si>]\n"
                  << "  无参数          跑 testdata/cases 下所有用例（dump 逐字比 + 六条不变量）\n"
                  << "  --dump-only     只打印某个用例的 dump（golden 用它生成）\n";
        return 2;
    }

    std::vector<std::filesystem::path> cases;
    const std::filesystem::path dir = cases_dir();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".si") {
            cases.push_back(entry.path());
        }
    }
    if (ec) {
        bad("读不了用例目录 " + dir.generic_string() + "：" + ec.message());
        return 1;
    }
    std::sort(cases.begin(), cases.end());
    if (cases.empty()) bad("用例目录里没有 .si：" + dir.generic_string());

    for (const std::filesystem::path& src : cases) {
        const std::string name = src.stem().string();
        CaseSource cs;
        std::string err;
        if (!load_case(src.generic_string(), &cs, &err)) {
            bad(name + " 载入失败：" + err);
            continue;
        }
        sema::Hir hir;
        try {
            hir = sema::analyze(cs.src);
        } catch (const std::exception& e) {
            bad(name + " analyze 抛异常：" + e.what());
            continue;
        }
        const std::string got = sema::dump(hir);
        const std::filesystem::path golden = src.parent_path() / (name + ".dump");
        std::string want;
        if (!read_golden(golden.string(), &want)) {
            bad(name + " 读不到 golden " + golden.generic_string());
            std::cout << "  重新生成（仓库根）：" << regen_cmd(src) << "\n";
        } else if (got == want) {
            ok(name + " dump 与 golden 相同");
        } else {
            bad(name + " dump 与 golden 不同（" + golden.generic_string() + "）");
            std::cout << "---- 实际 ----\n" << got << "---- 期望 ----\n" << want << "----\n"
                      << "  重生成前先判哪一版对：" << regen_cmd(src) << "\n";
        }
        std::string ierr;
        if (sema::check_invariants(hir, &ierr)) ok(name + " 六条不变量全过");
        else bad(name + " 六条不变量没过：" + ierr);
    }

    if (g_failed != 0) {
        std::cout << g_failed << " 条失败\n";
        return 1;
    }
    std::cout << cases.size() << " 个用例全部通过\n";
    return 0;
}
