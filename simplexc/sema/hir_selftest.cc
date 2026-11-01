/* sema/hir_selftest.cc —— `sema/hir.hh` 的自检入口（**不在产品路径**，只给判据用）。
 *
 * 两件事，各自有独立的 PASS 行：
 *   1. dump 的**逐字**比较：手搓一棵小 HIR（`main` 里 `Const` / `Local` / `Binary` / `If` / `Return`
 *      各至少一个，另加一个 `funcs[]` 里的实例好查 ③⑥），与 `testdata/hir_sample.dump.txt`
 *      一个字节一个字节地比。为什么逐字：②c 换 codegen 时唯一的硬判据就是 dump 的 diff，
 *      比较松了它就失去意义。
 *   2. 六条不变量：先确认完好的树全过，再对**同一棵树**的副本各破坏一条，验证报出来的编号正是
 *      那一条（规格 §2b 要求每条能单独报出是第几条）。
 *
 * 用法（golden 怎么来、`sample()` 对应哪段源码：`testdata/README.md`）：
 *   python simplexc/tools/build.py hir_selftest && build/hir_selftest.exe
 *   build/hir_selftest.exe --dump-only > simplexc/sema/testdata/hir_sample.dump.txt
 * 退出码：0 = 全过；1 = 有失败；2 = 命令行参数不认识。
 */
#include "hir.hh"
#include "hir_dump.hh"
#include "types.hh"

#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace sema = simplexc::sema;

namespace {

int g_failed = 0;

void ok(const std::string& what) { std::cout << "PASS " << what << "\n"; }

void bad(const std::string& what) {
    std::cout << "FAIL " << what << "\n";
    g_failed++;
}

sema::Span at(int line) {
    sema::Span s;
    s.line = line;
    return s;
}

sema::HNode expr(sema::HKind kind, sema::TyRef ty, int line) {
    sema::HNode n;
    n.kind = kind;
    n.ty = std::move(ty);
    n.span = at(line);
    return n;
}

sema::HNode stmt(sema::HKind kind, int line) {
    sema::HNode n;
    n.kind = kind;
    n.span = at(line);
    return n;
}

sema::HNode konst(uint64_t bits, sema::TyRef ty, int line) {
    sema::HNode n = expr(sema::HKind::Const, std::move(ty), line);
    n.const_bits = bits;
    return n;
}

sema::HNode local(const std::string& name, sema::TyRef ty, int line) {
    sema::HNode n = expr(sema::HKind::Local, std::move(ty), line);
    n.name = name;
    return n;
}

sema::HNode binary(sema::HBinOp op, sema::HNode l, sema::HNode r, sema::TyRef ty, int line) {
    sema::HNode n = expr(sema::HKind::Binary, std::move(ty), line);
    n.bin_op = op;
    n.kids.push_back(std::move(l));
    n.kids.push_back(std::move(r));
    return n;
}

sema::HNode var_decl(const std::string& name, sema::HNode init, sema::TyRef ty, int line) {
    sema::HNode n = stmt(sema::HKind::VarDecl, line);
    n.ty = std::move(ty);
    n.name = name;
    n.kids.push_back(std::move(init));
    return n;
}

/* 样例程序（= golden `testdata/hir_sample.dump.txt` 那棵树）。每一处都刻意选在能被六条不变量
 * 覆盖到的形状上；行号与下面这段清单一一对应（`span=` 就是它）：
 *
 *   1  main(n: U64) U64 {
 *   2    var x = 7                       //  VarDecl + Const(7)
 *   3    if x < 10 {                     //  Binary(lt) → Bool
 *   4      var y = 42                    //  VarDecl + Const(42)
 *   5    }
 *   6    return x + n                  //  Binary(add) → Return
 *   7  }
 *   8
 *   9  f[T](k: T) T { return k }         //  funcs[0] = T=U64 的实例（查 ③⑥）
 *
 * 名字一律用产物里的名字（接口 §1 / 决策 7），不另造 mangling：`main` 是样例里一个普通函数
 * （产物的程序根 `tc_main` 没有实参也没有返回值，那样就查不到 `param` 与 `Return`），它的 key
 * 就写 def 名（规格允许 main 的 key 为空串）；`f$u64` 是泛型实例的键**兼**符号名。
 */
sema::Hir sample() {
    const sema::TyRef u64 = sema::make_int(64, false);

    sema::Hir hir;
    hir.main.key = "main";
    hir.main.name = "main";
    hir.main.ret = u64;
    hir.main.params.push_back(sema::HParam{"n", u64});
    hir.main.body = stmt(sema::HKind::Block, 1);
    hir.main.body.kids.push_back(var_decl("x", konst(7, u64, 2), u64, 2));

    sema::HNode cond = binary(sema::HBinOp::Lt, local("x", u64, 3), konst(10, u64, 3),
                              sema::ty_bool(), 3);
    sema::HNode then = stmt(sema::HKind::Block, 3);
    then.kids.push_back(var_decl("y", konst(42, u64, 4), u64, 4));
    sema::HNode if_stmt = stmt(sema::HKind::If, 3);
    if_stmt.kids.push_back(std::move(cond));
    if_stmt.kids.push_back(std::move(then));
    hir.main.body.kids.push_back(std::move(if_stmt));

    sema::HNode ret = stmt(sema::HKind::Return, 6);
    ret.kids.push_back(binary(sema::HBinOp::Add, local("x", u64, 6), local("n", u64, 6), u64, 6));
    hir.main.body.kids.push_back(std::move(ret));

    sema::HirFunc f;
    f.key = "f$u64";
    f.name = "f$u64";
    f.ret = u64;
    f.params.push_back(sema::HParam{"k", u64});
    f.body = stmt(sema::HKind::Block, 9);
    sema::HNode fret = stmt(sema::HKind::Return, 9);
    fret.kids.push_back(local("k", u64, 9));
    f.body.kids.push_back(std::move(fret));
    hir.funcs.push_back(std::move(f));
    return hir;
}

/* golden 从文件读，**不**写成源码里的字符串常量：期望文本有二十多行，写进源码就只能靠手抄，
 * 抄错一个空格之后判据判的是手抄的水平。它就是 `--dump-only` 的输出，重新生成一行命令
 * （`testdata/README.md`）。
 *
 * 路径从 `__FILE__` 推：拼出来就是仓库根下的 golden，不需要传参、也不怕 cwd（`__FILE__`
 * 是相对还是绝对由构建决定，CMake 传绝对路径）。 */
std::string golden_path() {
    std::string self = __FILE__;
    const std::size_t slash = self.find_last_of("/\\");
    self.resize(slash == std::string::npos ? 0 : slash + 1);
    return self + "testdata/hir_sample.dump.txt";
}

const char* GOLDEN_REGEN =
    "build/hir_selftest.exe --dump-only > simplexc/sema/testdata/hir_sample.dump.txt";

/* 读成字节串，读的时候把 CRLF 折成 LF：本仓库 `core.autocrlf=true`，`.gitattributes` 只对
 * `simplexc/corpus` 那一棵树关掉了行尾归一化，所以检出后的 `.txt` 可能是 CRLF，而 dump 只出 LF。
 * 折一下判的才是内容，不是行尾。 */
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

/* 「报出来的编号是不是 N」：错误串的格式固定 `不变量#N：<路径> —— <为什么>`。 */
int invariant_no(const std::string& err) {
    static const std::string kPrefix = "不变量#";
    if (err.compare(0, kPrefix.size(), kPrefix) != 0) return 0;
    int n = 0;
    for (std::size_t i = kPrefix.size(); i < err.size(); ++i) {
        const char c = err[i];
        if (c < '0' || c > '9') break;
        n = n * 10 + (c - '0');
    }
    return n;
}

/* 六条各破坏一次。每个破坏都**只**碰一条：能报出正确编号才说明检查是分得开的。 */
struct Break {
    int expect;
    const char* what;
    std::function<void(sema::Hir&)> apply;
};

std::vector<Break> breaks() {
    std::vector<Break> out;
    out.push_back({1, "把 VarDecl 初始化式的 ty 清空（①每节点带类型）",
                   [](sema::Hir& h) { h.main.body.kids[0].kids[0].ty = nullptr; }});
    out.push_back({2, "把 Return 里的局部名改成不存在的名字（②引用已解析）",
                   [](sema::Hir& h) { h.main.body.kids[2].kids[0].kids[0].name = "zzz"; }});
    out.push_back({3, "塞一个指向 funcs[9] 的 Call（③实例闭包已收敛）",
                   [](sema::Hir& h) {
                       sema::HNode call = expr(sema::HKind::Call, sema::make_int(64, false), 7);
                       call.callee = sema::HCallee{sema::HCalleeKind::Func, 9};
                       h.main.body.kids.push_back(call);
                   }});
    out.push_back({4, "把初始化式改成 U32（④无隐式转换）", [](sema::Hir& h) {
                       h.main.body.kids[0].kids[0].ty = sema::make_int(32, false);
                   }});
    out.push_back({5, "把 If 的 span 行号清零（⑤每节点带 span）",
                   [](sema::Hir& h) { h.main.body.kids[1].span.line = 0; }});
    out.push_back({6, "把一个实例复制一份（⑥实例键唯一）",
                   [](sema::Hir& h) { h.funcs.push_back(h.funcs[0]); }});
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const bool dump_only = argc == 2 && std::string(argv[1]) == "--dump-only";
    if (argc > 2 || (argc == 2 && !dump_only)) {
        std::cerr << "用法：" << argv[0] << " [--dump-only]\n"
                  << "  --dump-only  只打印 dump（golden 用它生成），不跑比较、不检查不变量\n";
        return 2;
    }

    const sema::Hir hir = sample();
    const std::string got = sema::dump(hir);
    if (dump_only) {
        std::cout << got;
        return 0;
    }

    const std::string path = golden_path();
    std::string want;
    if (!read_golden(path, &want)) {
        bad("读不到 golden " + path);
        std::cout << "  重新生成（仓库根）：" << GOLDEN_REGEN << "\n";
    } else if (got == want) {
        ok("dump 与 golden 逐字相同（" + std::to_string(got.size()) + " 字节）");
    } else {
        bad("dump 与 golden 不同（" + path + "）");
        std::cout << "---- 实际 ----\n" << got << "---- 期望 ----\n" << want << "----\n"
                  << "  重生成前先判哪一版对：" << GOLDEN_REGEN << "\n";
    }

    std::string err;
    if (sema::check_invariants(hir, &err)) {
        ok("完好的树六条不变量全过");
    } else {
        bad("完好的树没过不变量：" + err);
    }

    for (const Break& b : breaks()) {
        sema::Hir broken = hir; /* 值语义：拷贝一份再破坏，原树留着给下一条用 */
        b.apply(broken);
        err.clear();
        const bool passed = sema::check_invariants(broken, &err);
        const int no = invariant_no(err);
        const std::string label = "不变量#" + std::to_string(b.expect) + " 破坏 → " +
                                  (passed ? std::string("没报错") : err);
        if (!passed && no == b.expect) {
            ok(label);
        } else {
            bad(label + "（" + b.what + "）");
        }
    }

    if (g_failed != 0) {
        std::cout << g_failed << " 条失败\n";
        return 1;
    }
    std::cout << "全部通过\n";
    return 0;
}
