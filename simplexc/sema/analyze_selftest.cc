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
 * **报错用例**用同名 `.err` 代替 `.dump`：`analyze` 必须抛异常，且 `what()` 与 `.err` 逐字相同
 * （两边都把 CRLF 折成 LF）。一份 `.si` 只能有 `.dump` 与 `.err` 之一 —— 两条都要就等于说「既该
 * 编过又该编不过」。判报错这一档是必须单独有的：不收敛、未实现的构造**必须报错**，而不是死循环
 * 或者静默生一棵半成品树。
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
using simplexc::TypeRef;
using simplexc::VarDeclStmt;

namespace {

int g_failed = 0;

void ok(const std::string& what) { std::cout << "PASS " << what << "\n"; }

void bad(const std::string& what) {
    std::cout << "FAIL " << what << "\n";
    g_failed++;
}

/* 用例目录从 `__FILE__` 推：拼出来是仓库根下的路径，打印重生成命令时能原样粘回去
 * （不用传参、不怕 cwd）。`__FILE__` 是相对还是绝对由构建决定（CMake 传绝对路径）。 */
std::filesystem::path cases_dir() {
    std::string self = __FILE__;
    const std::size_t slash = self.find_last_of("/\\");
    self.resize(slash == std::string::npos ? 0 : slash + 1);
    return std::filesystem::path(self) / "testdata" / "cases";
}

/* `.si` → `sema::ModuleSource`。AST 节点与类型拼写由本结构持有（`ModuleSource` 只持裸指针）。 */
struct CaseSource {
    std::vector<std::unique_ptr<FunctionDef>> fns;
    std::vector<std::unique_ptr<VarDeclStmt>> vars;
    std::vector<std::unique_ptr<TypeRef>> types; /* `type` 声明里的字段类型拼写 */
    std::vector<sema::EnumSource> enums;
    std::vector<sema::StructSource> structs;
    sema::ModuleSource src;
};

std::string join_lines(const std::vector<std::string>& lines, int lo, int hi) {
    std::string out;
    for (int i = lo - 1; i < hi && i < static_cast<int>(lines.size()); ++i) {
        if (i < 0) continue;
        if (!out.empty()) out += ' ';
        out += lines[static_cast<std::size_t>(i)];
    }
    return out;
}

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::size_t skip_ws(const std::string& s, std::size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
    return i;
}

std::string read_ident(const std::string& s, std::size_t& i) {
    const std::size_t b = i;
    if (i < s.size() && is_ident_start(s[i])) {
        ++i;
        while (i < s.size() && is_ident_char(s[i])) ++i;
    }
    return s.substr(b, i - b);
}

std::unique_ptr<TypeRef> make_type_ref(const std::string& word) {
    auto t = std::make_unique<TypeRef>();
    if (!word.empty() && word[0] == '@') {
        t->at = true;
        t->name = word.substr(1);
    } else {
        t->name = word;
    }
    return t;
}

/* `type X Enum [A, B]` / `type X Struct { f: T, g: U }` → `EnumSource` / `StructSource`。
 * 切法与 `codegen/module_build.cc` 的 `scan_type` 同形（改一处要同时改另一处）：这里是**判据侧的
 * 粘合**，与 `parse_top_var` 同一个位置 —— 接线（②c）之后它属于壳/CLI，不属于 sema。 */
void add_type_decl(const Frontend& fe, const Decl& d, CaseSource* out) {
    const std::string text = join_lines(fe.code_lines, d.lo, d.hi);
    if (d.note == "Enum") {
        const std::size_t lb = text.find("Enum");
        const std::size_t l = lb == std::string::npos ? std::string::npos : text.find('[', lb);
        const std::size_t r = text.rfind(']');
        if (l == std::string::npos || r == std::string::npos || r < l) {
            throw sema::CompileFail("第 " + std::to_string(d.lo) + " 行：枚举声明看不懂：" + text);
        }
        sema::EnumSource e;
        e.name = d.name;
        std::stringstream ss(text.substr(l + 1, r - l - 1));
        std::string item;
        while (std::getline(ss, item, ',')) {
            const std::size_t i = skip_ws(item, 0);
            std::size_t j = i;
            std::string member = read_ident(item, j);
            if (!member.empty()) e.members.push_back(std::move(member));
        }
        out->enums.push_back(std::move(e));
        out->src.enums = out->enums; /* 下面是最后一步，见 load_case 的收尾 */
        return;
    }
    if (d.note == "Struct") {
        const std::size_t l = text.find('{');
        const std::size_t r = text.rfind('}');
        if (l == std::string::npos || r == std::string::npos || r < l) {
            throw sema::CompileFail("第 " + std::to_string(d.lo) + " 行：结构声明看不懂：" + text);
        }
        sema::StructSource s;
        s.name = d.name;
        const std::string body = text.substr(l + 1, r - l - 1);
        std::size_t pos = 0;
        while (pos < body.size()) {
            const std::size_t comma = body.find(',', pos);
            const std::string part =
                body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            std::size_t i = skip_ws(part, 0);
            const std::string fname = read_ident(part, i);
            if (!fname.empty()) {
                i = skip_ws(part, i);
                if (i < part.size() && part[i] == ':') {
                    i = skip_ws(part, i + 1);
                    std::size_t j = i;
                    if (j < part.size() && part[j] == '@') ++j;
                    const std::string tname = read_ident(part, j);
                    if (!tname.empty()) {
                        const bool at = i < part.size() && part[i] == '@';
                        out->types.push_back(make_type_ref(at ? "@" + tname : tname));
                        s.fields.emplace_back(fname, out->types.back().get());
                    }
                }
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        out->structs.push_back(std::move(s));
        out->src.structs = out->structs;
        return;
    }
    throw sema::CompileFail("第 " + std::to_string(d.lo) + " 行：type 声明形态（" + d.note + "）");
}

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
            } else if (d.kind == "type" && d.depth == 0) {
                add_type_decl(fe, d, out);
            } else if (d.kind == "def" && d.depth == 0) {
                /* 顶层 def：解析好传进去，`analyze` 的 `Module::defs` 在那里建重载表。 */
                ParsedDef pd = fe.parse_def(idx);
                out->src.defs.push_back(pd.fn.get());
                out->fns.push_back(std::move(pd.fn));
            } else if (d.kind == "def" && d.depth > 0) {
                /* 体里的**嵌套** def：`parse_def` 解析程序根时已经连体把它解进 AST 了，decls 里
                 * 这一条只是记账（`enclosing`/`depth`），再解一次会得到一份不被引用的副本。 */
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

std::string regen_err_cmd(const std::filesystem::path& src) {
    return "build/analyze_selftest.exe --err-only " + src.generic_string() +
           " > " + (src.parent_path() / (src.stem().string() + ".err")).generic_string();
}

/* `--dump-only <case.si>`：只打印 dump（golden 用它生成）。
 * `--err-only <case.si>`：只打印 `analyze` 抛出的 `what()`（报错用例的 golden 用它生成）；
 * 没报错则返回 1 —— 那样生成的 `.err` 会是空的，本函数不能让它看起来像成功。 */
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

int err_only(const std::string& path) {
    CaseSource cs;
    std::string err;
    if (!load_case(path, &cs, &err)) {
        std::cerr << "FAIL 载入 " << path << "：" << err << "\n";
        return 1;
    }
    try {
        sema::analyze(cs.src);
    } catch (const std::exception& e) {
        std::cout << e.what() << "\n";
        return 0;
    }
    std::cerr << "FAIL analyze " << path << "：没有报错（这条用例该报错）\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump-only") return dump_only(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--err-only") return err_only(argv[2]);
    if (argc != 1) {
        std::cerr << "用法：" << argv[0] << " [--dump-only <case.si> | --err-only <case.si>]\n"
                  << "  无参数          跑 testdata/cases 下所有用例（dump 逐字比 + 六条不变量；"
                     "有同名 .err 的用例改比报错文本）\n"
                  << "  --dump-only     只打印某个用例的 dump（golden 用它生成）\n"
                  << "  --err-only      只打印某个用例 analyze 抛出的报错（.err golden 用它生成）\n";
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
        const std::filesystem::path err_golden = src.parent_path() / (name + ".err");
        std::string want_err;
        const bool is_error_case = read_golden(err_golden.string(), &want_err);
        sema::Hir hir;
        std::string got_err;
        bool threw = false;
        try {
            hir = sema::analyze(cs.src);
        } catch (const std::exception& e) {
            threw = true;
            got_err = std::string(e.what()) + "\n";
        }
        if (is_error_case) {
            if (!threw) {
                bad(name + " 该报错却编过了（" + err_golden.generic_string() + "）");
            } else if (got_err == want_err) {
                ok(name + " 报错文本与 golden 相同");
            } else {
                bad(name + " 报错文本与 golden 不同（" + err_golden.generic_string() + "）");
                std::cout << "---- 实际 ----\n" << got_err << "---- 期望 ----\n" << want_err << "----\n"
                          << "  重生成前先判哪一版对：" << regen_err_cmd(src) << "\n";
            }
            continue; /* 报错用例没有树可比、也没有不变量要查 */
        }
        if (threw) {
            bad(name + " analyze 抛异常：" + got_err.substr(0, got_err.size() - 1));
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
