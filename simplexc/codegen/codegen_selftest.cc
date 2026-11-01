/* codegen/codegen_selftest.cc —— codegen 的 L4 判据入口：**IR 形状断言**（不在产品路径）。
 *
 * 为什么需要它：`docs/facts/semantics.md` 里那几条语义（U-L1 访存宽度、U-L7 窄位宽、
 * U-L8 除零/移位、`/` `%` 的符号性）的**契约形状**是在 IR 上成立的 —— 「窄写只写 1 字节」在
 * IR 上是 `store i8`，「`>>` 永远是逻辑右移」是 `lshr` 不是 `ashr`。这些断言原先在
 * `lower/lower_selftest.cc`（已删）里，新路径只能靠 `blob_baseline` 的逐字节比间接兜住；
 * 这个入口把那几条断言最小化地重建在 codegen 上，**不判产物、只判 IR 形状**。
 *
 * 断言为什么是「去前导空白后包含」而不是整文件逐字比：IR 的临时值编号（`%ld7`）与块名编号随
 * 指令增删和 LLVM 版本变，逐字比会把「上游改一个字」报成语义回归。所以每个用例旁边放一份
 * `<同名>.irkeys`：一行一条**期望出现在某一行 IR 里的子串**，命中任意一行即算这条成立。
 * 代价是 `.irkeys` 写不出「不许出现 X」（`llvm.trap` / `align 8` / `sext`）—— 那一类只能靠
 * 正向证据兜：「对齐只声明 1」的正面形状是每条 load/store 带 `, align 1`，「运行期窄值不符号
 * 扩展」的正面形状是 `zext i32` + 掩码 `(1<<32)-1`。
 *
 * 判据的口径：每个 `.si` 必须有同名 `.irkeys`（少一份 = FAIL，不静默跳过）；`.irkeys` 里
 * 一条没命中就 FAIL 并打印那个子串。修期望之前先回 `docs/facts/semantics.md` 判哪一边对 ——
 * 用例的 `.si` 注释写着每份期望引用的是哪一条。
 *
 * 用法：
 *   codegen_selftest                                跑 `testdata/cases/` 下所有用例
 *   codegen_selftest --dump-only <case.si>          只打印该用例的 IR（核对/写期望时用）
 * 退出码：0 全过；1 有用例不过；2 参数不对或用例目录读不了。
 */
#include "codegen.hh"
#include "module_build.hh"
#include "module_source.hh"

#include "../frontend/frontend.hh"
#include "../sema/analyze.hh"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace cg = simplexc::codegen;
namespace lw = simplexc::lower;
namespace sm = simplexc::sema;
namespace sx = simplexc;

namespace {

int g_failed = 0;

void ok(const std::string& what) { std::cout << "PASS " << what << "\n"; }

void bad(const std::string& what) {
    std::cout << "FAIL " << what << "\n";
    g_failed++;
}

/* 用例目录从 `__FILE__` 推（相对还是绝对由构建决定）：既不依赖 cwd，
 * 打印出来的重生成命令也能原样粘回仓库根。 */
std::filesystem::path cases_dir() {
    std::string self = __FILE__;
    const std::size_t slash = self.find_last_of("/\\");
    self.resize(slash == std::string::npos ? 0 : slash + 1);
    return std::filesystem::path(self) / "testdata" / "cases";
}

/* `.si` → IR 文本：与 `cli/simplexc_cli.cc` 同一条链（`Frontend` → `build_module` →
 * `analyze` → `Program::build` → `ir_text`）。用例要走**产品那条链**，否则断言的形状不是
 * 交付物编译出来的形状。 */
std::string ir_of(const std::string& path) {
    sx::Frontend fe(path);
    if (!fe.decl_errors.empty()) {
        throw sm::CompileFail("声明层错误：" + fe.decl_errors.front());
    }
    lw::OwnedModule owned = lw::build_module(fe, path);
    const sm::Hir hir = sm::analyze(cg::module_source_of(owned.ast));
    cg::Program prog(hir);
    prog.build();
    return prog.ir_text();
}

/* CRLF 折成 LF：本仓库 `core.autocrlf=true`，判的应是内容、不是行尾（与 `analyze_selftest`
 * 的 `read_golden` 同一条写法）。 */
bool read_text(const std::string& path, std::string* out) {
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

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::size_t first_non_space(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
    return i;
}

/* `.irkeys` 一行一条期望子串；空行（以及只剩空白的行）不算一条。 */
std::vector<std::string> load_keys(const std::string& path) {
    std::string text;
    if (!read_text(path, &text)) return {};
    std::vector<std::string> keys;
    for (const std::string& line : split_lines(text)) {
        const std::string key = line.substr(first_non_space(line));
        if (!key.empty()) keys.push_back(key);
    }
    return keys;
}

std::string regen_cmd(const std::filesystem::path& src) {
    return "build/codegen_selftest.exe --dump-only " + src.generic_string();
}

/* 一个用例：生成 IR，逐条查 `.irkeys` 的每一行是否出现在某一行 IR 里（去前导空白后包含）。
 * 一条不命中就 FAIL 并打印那个子串 —— 打印子串而不是行号，是为了能直接 `grep -F` 回 IR 里找。 */
void check_case(const std::filesystem::path& src) {
    const std::string name = src.stem().string();
    const std::filesystem::path keys_path = src.parent_path() / (name + ".irkeys");
    if (!std::filesystem::exists(keys_path)) {
        bad(name + " 没有期望文件 " + keys_path.generic_string());
        std::cout << "  期望文件的写法：一行一条「该出现在某一行 IR 里的子串」\n";
        return;
    }
    const std::vector<std::string> keys = load_keys(keys_path.generic_string());
    if (keys.empty()) {
        bad(name + " 的 " + keys_path.generic_string() + " 是空的（空期望等于没判）");
        return;
    }

    std::string ir;
    try {
        ir = ir_of(src.generic_string());
    } catch (const std::exception& e) {
        bad(name + " 生成 IR 失败：" + e.what());
        return;
    }
    std::vector<std::string> lines = split_lines(ir);
    for (std::string& line : lines) line = line.substr(first_non_space(line));

    std::vector<std::string> missing;
    for (const std::string& key : keys) {
        const bool hit = std::any_of(lines.begin(), lines.end(), [&key](const std::string& line) {
            return line.find(key) != std::string::npos;
        });
        if (!hit) missing.push_back(key);
    }
    if (missing.empty()) {
        ok(name + "（" + std::to_string(keys.size()) + " 条期望全中）");
        return;
    }
    bad(name + " 缺 " + std::to_string(missing.size()) + " 条期望（" +
        keys_path.generic_string() + "）");
    for (const std::string& key : missing) std::cout << "  缺：" << key << "\n";
    std::cout << "  看实际 IR（仓库根）：" << regen_cmd(src) << "\n";
    std::cout << "  改期望之前先回 docs/facts/semantics.md 判哪一边对\n";
}

int dump_only(const std::string& path) {
    try {
        std::cout << ir_of(path);
    } catch (const std::exception& e) {
        std::cerr << "FAIL 生成 " << path << " 的 IR：" << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump-only") return dump_only(argv[2]);
    if (argc != 1) {
        std::cerr << "用法：" << argv[0] << " [--dump-only <case.si>]\n"
                  << "  无参数        跑 testdata/cases 下所有用例（逐条查同名 .irkeys 里的期望）\n"
                  << "  --dump-only   只打印某个用例的 IR（核对/写期望时用）\n";
        return 2;
    }

    const std::filesystem::path dir = cases_dir();
    std::error_code ec;
    std::vector<std::filesystem::path> cases;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".si") {
            cases.push_back(entry.path());
        }
    }
    if (ec) {
        bad("读不了用例目录 " + dir.generic_string() + "：" + ec.message());
        return 2;
    }
    std::sort(cases.begin(), cases.end());
    if (cases.empty()) {
        bad("用例目录里没有 .si：" + dir.generic_string());
        return 2;
    }

    for (const std::filesystem::path& src : cases) check_case(src);

    if (g_failed != 0) {
        std::cout << g_failed << " 个用例失败\n";
        return 1;
    }
    std::cout << cases.size() << " 个用例全部通过\n";
    return 0;
}
