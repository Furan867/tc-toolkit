/* lower_selftest.cc —— B 路 lowering 的自测入口（**不在产品路径**，只给 M4.5 的判据用）。
 *
 * 覆盖三件事：
 *   L2 手写 AST → COFF：节只应有 `.text/.data/.bss`，`.text` 的重定位只有 `REL32`；
 *   L3 接 `assemble::assemble()` → `Blob`：头 word = 大小 | bit62、entry 在码长内、`status == Ok`；
 *   L4 四条语义档案（U-L1 / U-L7 / U-L8 / `/`、`%`）的最小 AST：打印生成的 IR 片段 + 断言，
 *      每条断言都能对着 `lower/ir_build.py` 的对应行检查。
 *
 * 用法（在仓库根 `reversing/` 下跑；运行期 PATH 里要有 `build/llvm/bin` 才能找到 LLVM-C.dll）：
 *
 *   D:/bin/w64devkit/bin/g++.exe -std=c++17 -O2 -I. -I build/llvm/include \
 *       -c simplexc/lower/lower_selftest.cc -o work/tmp/lower/lower_selftest.o
 *   D:/bin/w64devkit/bin/g++.exe -std=c++17 -O2 -static \
 *       work/tmp/lower/lower_selftest.o work/tmp/lower/lower.o work/tmp/lower/llvm_ffi.o \
 *       build/objs/assemble.o -o work/tmp/lower/lower_selftest.exe \
 *       -L build/llvm/lib -lLLVM-C
 *   work/tmp/lower/lower_selftest.exe all        # 或 coff / blob / ir
 *
 * L2 跑两个模块：L2a 无栈帧（任务判据要的「只有 .text/.data/.bss」就断在它上）、
 * L2b 带栈帧（`get_time`/`memory_clear` 把 alloca 地址交给内核 → 多出 `.pdata/.xdata`，
 * 那是接口 §4-4 明令整节丢弃的形态）。L3 对两个 COFF 各装一次。
 *
 * 退出码：0 = 全部断言过；1 = 有失败；2 = 用法 / 环境问题。
 */
#include "lower.hh"
#include "../assemble/assemble.hh"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sl = simplexc::lower;
using simplexc::BinaryExpr;
using simplexc::AssignStmt;
using simplexc::Block;
using simplexc::CallExpr;
using simplexc::CastExpr;
using simplexc::ExprPtr;
using simplexc::ExprStmt;
using simplexc::FunctionDef;
using simplexc::IdentExpr;
using simplexc::IfCase;
using simplexc::IfStmt;
using simplexc::LiteralExpr;
using simplexc::NodePtr;
using simplexc::Param;
using simplexc::ReturnStmt;
using simplexc::StmtPtr;
using simplexc::TypeRef;
using simplexc::TypeRefPtr;
using simplexc::UnaryExpr;
using simplexc::VarDeclStmt;
using simplexc::WhileStmt;

namespace {

int g_failed = 0;

void ok(const std::string& what) { std::printf("   ok   %s\n", what.c_str()); }

void bad(const std::string& what) {
    std::printf("   FAIL %s\n", what.c_str());
    g_failed++;
}

void want_contains(const std::string& ir, const char* needle, const std::string& why) {
    if (ir.find(needle) != std::string::npos) {
        ok(why);
    } else {
        bad(why + "（IR 里没有 `" + std::string(needle) + "`）");
    }
}

void want_absent(const std::string& ir, const char* needle, const std::string& why) {
    if (ir.find(needle) == std::string::npos) {
        ok(why);
    } else {
        bad(why + "（IR 里出现了不该有的 `" + std::string(needle) + "`）");
    }
}

/* ---------------------------------------------------------------- 手写 AST 的小工具
 *
 * 所有权：父节点用 unique_ptr 持有子节点，只有**顶层声明**挂到 `Tree` 上（`Program` 只借裸指针，
 * 树必须在它活着期间有效）。 */
struct Tree {
    std::vector<NodePtr> roots;

    template <class T, class... A>
    T* root(A&&... a) {
        auto p = std::make_unique<T>(std::forward<A>(a)...);
        T* raw = p.get();
        roots.push_back(std::move(p));
        return raw;
    }
};

TypeRefPtr tref(int line, const char* name) {
    auto t = std::make_unique<TypeRef>();
    t->src_line = line;
    t->name = name;
    return t;
}

/* `{a, b}` 这种 braced list 对 `unique_ptr` 是**拷贝**（initializer_list 的元素恒为 const），
 * 所以子节点一律走这个显式移动的拼装器。 */
template <class T>
std::vector<T> mklist(T a) {
    std::vector<T> v;
    v.push_back(std::move(a));
    return v;
}

template <class T>
std::vector<T> mklist(T a, T b) {
    std::vector<T> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    return v;
}

template <class T>
std::vector<T> mklist(T a, T b, T c) {
    std::vector<T> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    v.push_back(std::move(c));
    return v;
}

ExprPtr lit(int line, uint64_t v) {
    auto e = std::make_unique<LiteralExpr>(line);
    e->lit_kind = "int";
    e->text = std::to_string(v);
    e->has_value = true;
    e->value = static_cast<int64_t>(v);
    return e;
}

ExprPtr id(int line, const char* name) {
    auto e = std::make_unique<IdentExpr>(line);
    e->name = name;
    return e;
}

ExprPtr castx(int line, const char* tname, ExprPtr inner) {
    auto e = std::make_unique<CastExpr>(line);
    e->target = tref(line, tname);
    e->expr = std::move(inner);
    return e;
}

ExprPtr unary(int line, const char* op, ExprPtr inner) {
    auto e = std::make_unique<UnaryExpr>(line);
    e->op = op;
    e->expr = std::move(inner);
    return e;
}

ExprPtr bin(int line, const char* op, ExprPtr l, ExprPtr r) {
    auto e = std::make_unique<BinaryExpr>(line);
    e->op = op;
    e->left = std::move(l);
    e->right = std::move(r);
    return e;
}

ExprPtr callx(int line, ExprPtr callee, std::vector<ExprPtr> args,
              std::vector<TypeRefPtr> targs = {}) {
    auto e = std::make_unique<CallExpr>(line);
    e->callee = std::move(callee);
    e->args = std::move(args);
    e->type_args = std::move(targs);
    return e;
}

std::unique_ptr<Block> blk(int line, std::vector<StmtPtr> stmts) {
    auto b = std::make_unique<Block>(line);
    b->stmts = std::move(stmts);
    return b;
}

StmtPtr stmt_expr(int line, ExprPtr e) {
    auto s = std::make_unique<ExprStmt>(line);
    s->expr = std::move(e);
    return s;
}

StmtPtr stmt_var(int line, const char* kw, const char* name, ExprPtr init) {
    auto s = std::make_unique<VarDeclStmt>(line);
    s->kw = kw;
    s->name = name;
    s->init = std::move(init);
    return s;
}

StmtPtr stmt_return(int line, ExprPtr v) {
    auto s = std::make_unique<ReturnStmt>(line);
    s->value = std::move(v);
    return s;
}

StmtPtr stmt_assign(int line, ExprPtr target, const char* op, ExprPtr value) {
    auto s = std::make_unique<AssignStmt>(line);
    s->target = std::move(target);
    s->op = op;
    s->value = std::move(value);
    return s;
}

StmtPtr stmt_if(int line, ExprPtr cond, std::unique_ptr<Block> body) {
    auto s = std::make_unique<IfStmt>(line);
    auto c = std::make_unique<IfCase>(line);
    c->cond = std::move(cond);
    c->block = std::move(body);
    s->cases.push_back(std::move(c));
    return s;
}

StmtPtr stmt_while(int line, ExprPtr cond, std::unique_ptr<Block> body) {
    auto s = std::make_unique<WhileStmt>(line);
    s->cond = std::move(cond);
    s->block = std::move(body);
    return s;
}

std::unique_ptr<Param> param(int line, const char* name, const char* tname) {
    auto p = std::make_unique<Param>(line);
    p->name = name;
    p->typ = tref(line, tname);
    return p;
}

/* `Ptr 0x1000` = 到 `Ptr` 的显式转换（U-L1：地址是**字节**地址，常数直接是那个地址）。 */
ExprPtr ptrlit(int line, uint64_t addr) { return castx(line, "Ptr", lit(line, addr)); }

/* `store(<T>, p, v)` —— 注意 `store` 的写入宽度取 **v 的类型**（oracle:1385），类型实参不参与。 */
StmtPtr stmt_store(int line, ExprPtr addr, ExprPtr value) {
    return stmt_expr(line, callx(line, id(line, "store"), mklist(std::move(addr), std::move(value))));
}

/* `load(<T>, p)` */
ExprPtr call_load(int line, const char* tname, ExprPtr addr) {
    return callx(line, id(line, "load"), mklist(std::move(addr)), mklist(tref(line, tname)));
}

/* `def 名(参数) 返回 { 体 }`；`form == "label"` 的那个是程序根。 */
FunctionDef* func(Tree& tree, int line, const char* name, const char* form,
                  std::vector<std::unique_ptr<Param>> params, const char* ret,
                  std::unique_ptr<Block> body) {
    FunctionDef* d = tree.root<FunctionDef>(line);
    d->name = name;
    d->form = form;
    d->params = std::move(params);
    if (ret != nullptr) d->ret = tref(line, ret);
    d->body = std::move(body);
    d->lo = line;
    d->hi = line;
    return d;
}

/* ---------------------------------------------------------------- 判据用的小模块 */

struct Case {
    Tree tree;
    sl::ModuleAst ast;
};

/* L2：一个最小但形状完整的模块 —— 内部调用（→ REL32）、外部调用（→ 桩）、全局引用
 * （→ 内部 RIP 相对）、窄写、分支、循环。
 *
 * `with_stack_frame = true` 时额外用 `get_time`/`memory_clear`：它们把 alloca 的地址交给内核，
 * 于是真的有栈帧，LLVM 就会多出 `.pdata/.xdata` —— 那是接口 §4-4 明令整节丢弃的形态，
 * 所以两种形态分开断言。（任务判据说的「只有 .text/.data/.bss」对无栈帧的那种成立。） */
Case build_l2(bool with_stack_frame) {
    Case c;
    const int line = 1;
    auto* g_zero = c.tree.root<VarDeclStmt>(line);
    g_zero->kw = "var";
    g_zero->name = "g_zero";
    g_zero->init = lit(line, 0);                     /* 全零 → .bss */
    auto* g_seven = c.tree.root<VarDeclStmt>(line);
    g_seven->kw = "var";
    g_seven->name = "g_seven";
    g_seven->init = lit(line, 7);                    /* 非零 → .data */
    c.ast.top_vars = {g_zero, g_seven};

    sl::EnumDecl en;
    en.name = "Cmd";
    en.members = {"com_off", "com_on", "com_halt"};
    c.ast.enums.push_back(en);

    /* def helper(x: U64) U64 { return x + 7 } */
    std::vector<std::unique_ptr<Param>> p1;
    p1.push_back(param(line, "x", "U64"));
    c.ast.defs.push_back(func(c.tree, line, "helper", "def", std::move(p1), "U64",
                              blk(line, mklist(stmt_return(
                                                  line, bin(line, "+", id(line, "x"), lit(line, 7)))))));

    /* def f(v: U64) None {
     *     store(<U64>, Ptr 0x1000, v + helper(v))
     *     if v { store(<U64>, Ptr 0x1008, com_halt) }          // 枚举成员 → 常量 2
     *     var i = 0
     *     while i < v { store(<U64>, Ptr 0x1020 + i * 8, i); i += 1 }
     * }
     */
    std::vector<std::unique_ptr<Param>> p2;
    p2.push_back(param(line, "v", "U64"));
    std::vector<StmtPtr> body2;
    body2.push_back(stmt_store(line, ptrlit(line, 0x1000),
                               bin(line, "+", id(line, "v"),
                                   callx(line, id(line, "helper"), mklist(id(line, "v"))))));
    body2.push_back(stmt_if(line, id(line, "v"),
                            blk(line, mklist(stmt_store(line, ptrlit(line, 0x1008),
                                                        id(line, "com_halt"))))));
    body2.push_back(stmt_var(line, "var", "i", lit(line, 0)));
    std::vector<StmtPtr> loop_body;
    loop_body.push_back(stmt_store(
        line,
        bin(line, "+", ptrlit(line, 0x1020), bin(line, "*", id(line, "i"), lit(line, 8))),
        id(line, "i")));
    loop_body.push_back(stmt_assign(line, id(line, "i"), "+=", lit(line, 1)));
    body2.push_back(stmt_while(line, bin(line, "<", id(line, "i"), id(line, "v")),
                               blk(line, std::move(loop_body))));
    c.ast.defs.push_back(func(c.tree, line, "f", "def", std::move(p2), "None", blk(line, std::move(body2))));

    /* 程序根（form == "label"）：f(41) + 往两个内部全局写**运行期**读到的值再读回来
     * （常量值会被 O2 折掉，全局就没了 —— 这里要的是 `.data`/`.bss` 真的非空）。 */
    std::vector<StmtPtr> root_body;
    root_body.push_back(stmt_expr(line, callx(line, id(line, "f"), mklist(lit(line, 41)))));
    root_body.push_back(stmt_assign(line, id(line, "g_seven"), "=",
                                    call_load(line, "U64", ptrlit(line, 0x1030))));
    root_body.push_back(stmt_store(line, ptrlit(line, 0x1010), id(line, "g_seven")));
    root_body.push_back(stmt_assign(line, id(line, "g_zero"), "=",
                                    call_load(line, "U64", ptrlit(line, 0x1038))));
    root_body.push_back(stmt_store(line, ptrlit(line, 0x1018), id(line, "g_zero")));
    if (with_stack_frame) {
        /* 前缀（stdlib）路径 + 外部符号：`memory_clear` 走 volatile 字节循环（O2 消不掉）、
         * `get_time` 把槽的地址交给 kernel32（→ 真栈帧 → `.pdata/.xdata`）。两者都给 `.text`
         * 留下指向 extern 的 REL32。 */
        root_body.push_back(stmt_expr(line, callx(line, id(line, "memory_clear"),
                                                  mklist(ptrlit(line, 0x2000), lit(line, 32)))));
        root_body.push_back(stmt_expr(line, callx(line, id(line, "get_time"), {})));
    }
    c.ast.label = func(c.tree, line, "run_sim", "label", {}, "None",
                       blk(line, std::move(root_body)));
    c.ast.source_name = "lower_selftest:l2";
    return c;
}

/* ---------------- L4 的四个最小案 */

/* U-L1：字节地址、无对齐要求、窄写只写该宽度、不插对齐检查。 */
Case build_ul1() {
    Case c;
    const int line = 1;
    std::vector<StmtPtr> body;
    body.push_back(stmt_store(line, ptrlit(line, 0x1000), lit(line, 42)));
    /* `Ptr 0x1000 + 8` 常量折叠 → 4104 = 0x1008：地址算术就是字节算术（不是「元素 +1」）。 */
    body.push_back(stmt_store(line, bin(line, "+", ptrlit(line, 0x1000), lit(line, 8)), lit(line, 1)));
    body.push_back(stmt_store(line, ptrlit(line, 0x1004), castx(line, "U8", lit(line, 0x1FF))));
    body.push_back(stmt_store(line, ptrlit(line, 0x100C), castx(line, "U32", lit(line, 0x1FFFFFFFFull))));
    c.ast.label = func(c.tree, line, "run_sim", "label", {}, "None", blk(line, std::move(body)));
    c.ast.source_name = "lower_selftest:ul1";
    return c;
}

/* U-L7：加宽分两路（常量按符号性 / 运行期 zext(trunc_W)）、转换与运算结果两处掩码、窄写宽度。 */
Case build_ul7() {
    Case c;
    const int line = 1;
    std::vector<StmtPtr> body;
    /* 常量路径：`S32 -8` 折成 0xFFFFFFF8，加宽到 U64 时按**源类型符号性** sext → 全 F 的 -8。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x2000), castx(line, "U64", unary(line, "-", lit(line, 8)))));
    /* 运行期路径：先窄写 4 字节，再窄读（zext(trunc_32) 形态），再「加宽」到 U64。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x2008), bin(line, "-", castx(line, "S32", lit(line, 0)), castx(line, "S32", lit(line, 8)))));
    body.push_back(stmt_var(line, "let", "rt", call_load(line, "S32", ptrlit(line, 0x2008))));
    body.push_back(stmt_store(line, ptrlit(line, 0x2010), castx(line, "U64", id(line, "rt"))));
    /* 转换处的掩码（U8）、运算结果处的掩码（U32 乘法）。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x2018), castx(line, "U8", lit(line, 0x1FF))));
    body.push_back(stmt_store(line, ptrlit(line, 0x2020),
                              castx(line, "U32", bin(line, "*", call_load(line, "U32", ptrlit(line, 0x1000)),
                                                     castx(line, "U32", lit(line, 3))))));
    c.ast.label = func(c.tree, line, "run_sim", "label", {}, "None", blk(line, std::move(body)));
    c.ast.source_name = "lower_selftest:ul7";
    return c;
}

/* U-L8：`>>` 恒 lshr、移位量按 W 取模、`asr` 在 n ≥ W 时饱和。 */
Case build_ul8() {
    Case c;
    const int line = 1;
    std::vector<StmtPtr> body;
    /* 运行期 S64 >> 运行期 U64：`lshr` + `amt & 63`（不是 ashr）。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x3000),
                              bin(line, ">>", call_load(line, "S64", ptrlit(line, 0x1000)),
                                  call_load(line, "U64", ptrlit(line, 0x1008)))));
    /* 运行期 S32 >> 运行期 U32：`amt & 31`（按 W 取模，不是「≥ 位宽就 0」）。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x3010),
                              bin(line, ">>", call_load(line, "S32", ptrlit(line, 0x1000)),
                                  call_load(line, "U32", ptrlit(line, 0x1008)))));
    /* `asr` 与 `>>` 不同：n ≥ W 时饱和（select 夹到 W-1），不是回绕。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x3018),
                              callx(line, id(line, "asr"),
                                    mklist(call_load(line, "S32", ptrlit(line, 0x1000)),
                                           call_load(line, "S32", ptrlit(line, 0x1008))))));
    /* 常量 `S32 -8 >> 1`：sext 到 64 → lshr → 截回 32 = 0xFFFFFFFC。移位量显式给 S32，
     * 与档案里那个片段的形状一致；`>` 的类型统一（unify）决定谁能截断到 W。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x3020),
                              bin(line, ">>", bin(line, "-", castx(line, "S32", lit(line, 0)),
                                                  castx(line, "S32", lit(line, 8))),
                                  castx(line, "S32", lit(line, 1)))));
    /* 同一个表达式、移位量写成**无类型字面量**：unify 把整体拉成 I64（64 位），于是截断到 32 这一步
     * 不发生 —— 结果就是事实档案里 `S64 -8 >> 1` = `0x7FFF…FFFC` 那一行（两者都是 lshr 语义）。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x3028),
                              bin(line, ">>", bin(line, "-", castx(line, "S32", lit(line, 0)),
                                                  castx(line, "S32", lit(line, 8))),
                                  lit(line, 1))));
    c.ast.label = func(c.tree, line, "run_sim", "label", {}, "None", blk(line, std::move(body)));
    c.ast.source_name = "lower_selftest:ul8";
    return c;
}

/* `/`、`%`：opcode 看被除数；窄常量做被除数时被零扩展成 64 位再送有符号除法（复刻参考的不自洽）。 */
Case build_div() {
    Case c;
    const int line = 1;
    std::vector<StmtPtr> body;
    /* 与被除数同宽、符号性不同：U32 被除数 → udiv；S32 被除数 → sdiv（除数完全不参与）。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x4000),
                              castx(line, "U64", bin(line, "/", call_load(line, "U32", ptrlit(line, 0x1000)),
                                                     call_load(line, "S32", ptrlit(line, 0x1008))))));
    body.push_back(stmt_store(line, ptrlit(line, 0x4008),
                              castx(line, "U64", bin(line, "/", call_load(line, "S32", ptrlit(line, 0x1000)),
                                                     call_load(line, "U32", ptrlit(line, 0x1008))))));
    /* 折叠出来的窄常量被除数：`S32 0 - S32 7` → 0xFFFFFFF9 → **零扩展**成 4294967289 再 sdiv。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x4010),
                              bin(line, "/", bin(line, "-", castx(line, "S32", lit(line, 0)),
                                                 castx(line, "S32", lit(line, 7))),
                                  call_load(line, "S32", ptrlit(line, 0x1008)))));
    /* 两个操作数都是常量 → 编译期折叠：按被除数的符号性算，结果截断到 W（`S8 0 - S8 7` ÷ `S8 2` = 0xFD）。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x4018),
                              bin(line, "/", bin(line, "-", castx(line, "S8", lit(line, 0)),
                                                 castx(line, "S8", lit(line, 7))),
                                  castx(line, "S8", lit(line, 2)))));
    /* 除零不 trap：商用 select 兜 0；`%` 走 urem/srem，除零时余数 = 被除数。 */
    body.push_back(stmt_store(line, ptrlit(line, 0x4020),
                              castx(line, "U64", bin(line, "%", call_load(line, "U64", ptrlit(line, 0x1000)),
                                                     call_load(line, "U64", ptrlit(line, 0x1008))))));
    c.ast.label = func(c.tree, line, "run_sim", "label", {}, "None", blk(line, std::move(body)));
    c.ast.source_name = "lower_selftest:div";
    return c;
}

/* ---------------------------------------------------------------- oracle 对照用的探针
 *
 * `work/tmp/lower/probe.si` 是同一段源码的 **参考实现**版本；本函数手写它的 AST。
 * 两份模块的 IR 文本必须逐字节相同（同一个 LLVM 版本 + 同一条发射顺序），
 * 所以这是目前唯一能直接拿冻结 oracle 对账的手段（真源接入等 A 路 —— 见报告里的 L5）。
 */
Case build_probe() {
    Case c;
    const int line = 1;
    auto* g_seven = c.tree.root<VarDeclStmt>(line);
    g_seven->kw = "var";
    g_seven->name = "g_seven";
    g_seven->init = castx(line, "U64", lit(line, 7));
    auto* g_zero = c.tree.root<VarDeclStmt>(line);
    g_zero->kw = "var";
    g_zero->name = "g_zero";
    g_zero->init = castx(line, "U64", lit(line, 0));
    c.ast.top_vars = {g_seven, g_zero};
    sl::EnumDecl en;
    en.name = "Cmd";
    en.members = {"com_off", "com_on", "com_halt"};
    c.ast.enums.push_back(en);

    std::vector<std::unique_ptr<Param>> p1;
    p1.push_back(param(line, "x", "U64"));
    c.ast.defs.push_back(func(c.tree, line, "helper", "def", std::move(p1), "U64",
                              blk(line, mklist(stmt_return(
                                                  line, bin(line, "+", id(line, "x"), lit(line, 7)))))));

    std::vector<std::unique_ptr<Param>> p2;
    p2.push_back(param(line, "v", "U64"));
    std::vector<StmtPtr> b;
    b.push_back(stmt_store(line, ptrlit(line, 0x1000),
                           bin(line, "+", id(line, "v"),
                               callx(line, id(line, "helper"), mklist(id(line, "v"))))));
    b.push_back(stmt_if(line, id(line, "v"),
                        blk(line, mklist(stmt_store(line, ptrlit(line, 0x1008), id(line, "com_halt"))))));
    b.push_back(stmt_var(line, "var", "i", lit(line, 0)));
    std::vector<StmtPtr> loop_body;
    loop_body.push_back(stmt_store(
        line, bin(line, "+", ptrlit(line, 0x1020), bin(line, "*", id(line, "i"), lit(line, 8))),
        id(line, "i")));
    loop_body.push_back(stmt_assign(line, id(line, "i"), "+=", lit(line, 1)));
    b.push_back(stmt_while(line, bin(line, "<", id(line, "i"), id(line, "v")),
                           blk(line, std::move(loop_body))));
    /* U-L1 */
    b.push_back(stmt_store(line, ptrlit(line, 0x1000), lit(line, 42)));
    b.push_back(stmt_store(line, ptrlit(line, 0x1004), castx(line, "U8", lit(line, 0x1FF))));
    /* U-L7 */
    b.push_back(stmt_store(line, ptrlit(line, 0x2000),
                           castx(line, "U64", bin(line, "-", castx(line, "S32", lit(line, 0)),
                                                  castx(line, "S32", lit(line, 8))))));
    b.push_back(stmt_var(line, "let", "rt", call_load(line, "S32", ptrlit(line, 0x2008))));
    b.push_back(stmt_store(line, ptrlit(line, 0x2010), castx(line, "U64", id(line, "rt"))));
    b.push_back(stmt_store(line, ptrlit(line, 0x2020),
                           castx(line, "U32", bin(line, "*", call_load(line, "U32", ptrlit(line, 0x5000)),
                                                   castx(line, "U32", lit(line, 3))))));
    /* U-L8 */
    b.push_back(stmt_store(line, ptrlit(line, 0x3000),
                           bin(line, ">>", call_load(line, "S64", ptrlit(line, 0x5000)),
                               call_load(line, "U64", ptrlit(line, 0x5008)))));
    b.push_back(stmt_store(line, ptrlit(line, 0x3018),
                           callx(line, id(line, "asr"),
                                 mklist(call_load(line, "S32", ptrlit(line, 0x5000)),
                                        call_load(line, "S32", ptrlit(line, 0x5008))))));
    b.push_back(stmt_store(line, ptrlit(line, 0x3020),
                           bin(line, ">>", bin(line, "-", castx(line, "S32", lit(line, 0)),
                                               castx(line, "S32", lit(line, 8))),
                               castx(line, "S32", lit(line, 1)))));
    /* `/`、`%` */
    b.push_back(stmt_store(line, ptrlit(line, 0x4000),
                           castx(line, "U64", bin(line, "/", call_load(line, "U32", ptrlit(line, 0x5000)),
                                                   call_load(line, "S32", ptrlit(line, 0x5008))))));
    b.push_back(stmt_store(line, ptrlit(line, 0x4008),
                           castx(line, "U64", bin(line, "/", call_load(line, "S32", ptrlit(line, 0x5000)),
                                                   call_load(line, "U32", ptrlit(line, 0x5008))))));
    b.push_back(stmt_store(line, ptrlit(line, 0x4010),
                           bin(line, "/", bin(line, "-", castx(line, "S32", lit(line, 0)),
                                              castx(line, "S32", lit(line, 7))),
                               call_load(line, "S32", ptrlit(line, 0x5008)))));
    b.push_back(stmt_store(line, ptrlit(line, 0x4018),
                           bin(line, "/", bin(line, "-", castx(line, "S8", lit(line, 0)),
                                              castx(line, "S8", lit(line, 7))),
                               castx(line, "S8", lit(line, 2)))));
    b.push_back(stmt_store(line, ptrlit(line, 0x4020),
                           castx(line, "U64", bin(line, "%", call_load(line, "U64", ptrlit(line, 0x5000)),
                                                   call_load(line, "U64", ptrlit(line, 0x5008))))));
    /* 全局 + 前缀 */
    b.push_back(stmt_store(line, ptrlit(line, 0x1010), id(line, "g_seven")));
    b.push_back(stmt_assign(line, id(line, "g_zero"), "=",
                            call_load(line, "U64", ptrlit(line, 0x6010))));
    b.push_back(stmt_store(line, ptrlit(line, 0x1018), id(line, "g_zero")));
    c.ast.defs.push_back(func(c.tree, line, "f", "def", std::move(p2), "None", blk(line, std::move(b))));

    std::vector<StmtPtr> root;
    root.push_back(stmt_expr(line, callx(line, id(line, "f"), mklist(lit(line, 41)))));
    root.push_back(stmt_expr(line, callx(line, id(line, "memory_clear"),
                                         mklist(ptrlit(line, 0x2000), lit(line, 32)))));
    root.push_back(stmt_var(line, "let", "t", callx(line, id(line, "get_time"), {})));
    c.ast.label = func(c.tree, line, "run_sim", "label", {}, "None", blk(line, std::move(root)));
    c.ast.source_name = "lower_selftest:probe";
    return c;
}

/* ---------------------------------------------------------------- COFF 的小解析器
 *
 * 只为判据：读节名与重定位类型直方图（完整解析在 `assemble/assemble.cc`，不在这儿抄第二份）。 */
struct CoffInfo {
    struct Sec {
        std::string name;
        uint32_t raw_size = 0;
        uint32_t nreloc = 0;
        std::vector<uint16_t> reloc_types;
    };
    std::vector<Sec> sections;

    bool parse(const std::vector<uint8_t>& b, std::string* err) {
        if (b.size() < 20) { *err = "比 COFF 头还短"; return false; }
        auto u16 = [&](size_t o) { return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8); };
        auto u32 = [&](size_t o) {
            return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) |
                   (uint32_t(b[o + 3]) << 24);
        };
        const uint32_t nsec = u16(2);
        const uint32_t opt = u16(16);
        const size_t tbl = 20 + opt;
        if (tbl + size_t(nsec) * 40 > b.size()) { *err = "节表越界"; return false; }
        for (uint32_t i = 0; i < nsec; ++i) {
            const size_t o = tbl + i * 40;
            Sec s;
            char nm[9] = {0};
            std::memcpy(nm, b.data() + o, 8);
            s.name = nm;
            s.raw_size = u32(o + 16);
            const uint32_t reloc_ptr = u32(o + 24);
            s.nreloc = u16(o + 32);
            if (reloc_ptr != 0) {
                for (uint32_t r = 0; r < s.nreloc; ++r) {
                    const size_t ro = reloc_ptr + size_t(r) * 10;
                    if (ro + 10 > b.size()) { *err = "重定位表越界"; return false; }
                    s.reloc_types.push_back(uint16_t(u16(ro + 8)));
                }
            }
            sections.push_back(std::move(s));
        }
        return true;
    }
};

const char* reloc_name(uint16_t t) {
    switch (t) {
        case 0x01: return "ADDR64";
        case 0x02: return "ADDR32";
        case 0x03: return "ADDR32NB";
        case 0x04: return "REL32";
        case 0x05: return "REL32_1";
        case 0x06: return "REL32_2";
        case 0x07: return "REL32_3";
        case 0x08: return "REL32_4";
        case 0x09: return "REL32_5";
        default: return "?";
    }
}

std::vector<uint8_t> read_file(const std::string& path, bool* okflag) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { *okflag = false; return {}; }
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    *okflag = true;
    return out;
}

/* 桩解析器：所有未定义符号都给同一个假地址（判据只看形状，不看桩指向哪）。 */
char g_dummy_target = 0;

class DummyResolver : public tc::ExternResolver {
public:
    std::vector<std::string> names;
    bool resolve(std::string_view symbol, uint64_t& addr) override {
        names.emplace_back(symbol);
        addr = reinterpret_cast<uint64_t>(&g_dummy_target);
        return true;
    }
};

/* ---------------------------------------------------------------- L2 */

std::vector<uint8_t> run_l2_coff(const std::string& objdir, bool with_stack_frame,
                                const char* title) {
    (void)objdir;
    std::printf("[L2] %s\n", title);
    Case c = build_l2(with_stack_frame);
    sl::Program prog(c.ast);
    prog.build();
    for (const auto& d : prog.diagnostics()) {
        std::printf("   diagnostic: 第 %d 行 %s\n", d.line, d.msg.c_str());
    }
    const std::vector<uint8_t> coff = prog.emit_object();
    std::printf("[L2] COFF %zu 字节（函数 %d 个）\n", coff.size(), prog.function_count());
    if (coff.empty()) {
        bad("COFF 非空");
    } else {
        ok("COFF 非空");
    }

    CoffInfo info;
    std::string err;
    if (!info.parse(coff, &err)) {
        bad("COFF 能解析：" + err);
        return coff;
    }
    std::printf("[L2] 节表：\n");
    bool only_allowed = true;
    uint32_t text_relocs = 0, text_non_rel32 = 0, dropped_relocs = 0, bad_abs = 0;
    for (const auto& s : info.sections) {
        std::printf("   %-10s raw=%6u  reloc=%3u", s.name.c_str(), s.raw_size,
                    static_cast<unsigned>(s.nreloc));
        for (uint16_t t : s.reloc_types) std::printf(" %s", reloc_name(t));
        std::printf("\n");
        const bool allowed = s.name == ".text" || s.name == ".data" || s.name == ".bss" ||
                             (with_stack_frame && (s.name == ".pdata" || s.name == ".xdata"));
        if (!allowed) only_allowed = false;
        for (uint16_t t : s.reloc_types) {
            if (t == 0x01 || t == 0x02) bad_abs++;   /* 指向内部符号的绝对重定位：接口 §4-2 硬拒 */
        }
        if (s.name == ".text") {
            text_relocs += static_cast<uint32_t>(s.reloc_types.size());
            for (uint16_t t : s.reloc_types) {
                if (t != 0x04) text_non_rel32++;
            }
        } else {
            dropped_relocs += static_cast<uint32_t>(s.reloc_types.size());
            for (uint16_t t : s.reloc_types) {
                if (t != 0x03) bad_abs++;            /* 非 .text 只允许 .pdata 的 ADDR32NB */
            }
        }
    }
    if (only_allowed) {
        ok(std::string("节名集合 = ") + (with_stack_frame ? ".text/.data/.bss + 可丢的 .pdata/.xdata"
                                                        : "只有 .text/.data/.bss"));
    } else {
        bad("节名集合合法");
    }
    if (text_relocs > 0 && text_non_rel32 == 0) ok(".text 的重定位只有 REL32");
    else bad(".text 的重定位只有 REL32（共 " + std::to_string(text_relocs) + " 条，非 REL32 " +
             std::to_string(text_non_rel32) + " 条）");
    if (bad_abs == 0) ok("没有 ADDR32/ADDR64 这类绝对重定位（接口 §4-2）");
    else bad("没有绝对重定位（实到 " + std::to_string(bad_abs) + " 条）");
    if (text_relocs > 0) ok(".text 里确实有重定位（内部调用 + extern 桩都是 PC 相对）");
    else bad(".text 里确实有重定位");
    if (with_stack_frame && dropped_relocs > 0) {
        std::printf("[L2]   另有 %u 条 ADDR32NB 在 .pdata/.xdata（装配器按接口 §4-4 整节丢弃）\n",
                    dropped_relocs);
    }
    return coff;
}

/* ---------------------------------------------------------------- L3 */

void run_l3_blob(const std::vector<uint8_t>& coff, const std::string& objdir, const char* title) {
    std::printf("[L3] %s\n", title);
    bool okf = false;
    const std::vector<uint8_t> shim = read_file(objdir + "/entry_shim.o", &okf);
    if (!okf) {
        bad("读得了 " + objdir + "/entry_shim.o（先跑 tools/build.py objs）");
        return;
    }
    const std::vector<uint8_t> rt = read_file(objdir + "/rt_min.o", &okf);
    if (!okf) {
        bad("读得了 " + objdir + "/rt_min.o（先跑 tools/build.py objs）");
        return;
    }
    std::vector<tc::ObjectFile> objs;
    objs.push_back({"llvm.obj", coff});
    objs.push_back({"entry_shim.o", shim});
    objs.push_back({"rt_min.o", rt});
    DummyResolver resolver;
    const tc::AsmResult r = tc::assemble(objs, "__tc_entry", resolver);
    std::printf("[L3] assemble: status=%d detail=`%s`\n", static_cast<int>(r.status),
                r.detail.c_str());
    std::printf("[L3] text=%llu rdata=%llu data=%llu bss=%llu stubs=%u relocs=%u\n",
                (unsigned long long)r.text_size, (unsigned long long)r.rdata_size,
                (unsigned long long)r.data_size, (unsigned long long)r.bss_size, r.stub_count,
                r.reloc_count);
    for (const auto& s : r.stubs) {
        std::printf("[L3]   桩 +0x%llX %s\n", (unsigned long long)s.offset, s.symbol.c_str());
    }
    if (r.status != tc::AsmStatus::Ok) {
        bad("assemble status == Ok");
        return;
    }
    ok("assemble status == Ok");
    const std::vector<uint8_t>& b = r.blob.bytes;
    if (b.size() < 16) {
        bad("blob 至少要 8 字节头 + 一点机器码");
        return;
    }
    uint64_t header = 0;
    for (int i = 0; i < 8; ++i) header |= uint64_t(b[i]) << (8 * i);
    const uint64_t want = b.size() | (1ull << 62);
    if (header == want) ok("头 word == 大小 | bit62");
    else {
        char buf[128];
        std::snprintf(buf, sizeof buf, "头 word == 大小 | bit62（得到 0x%llX，要 0x%llX）",
                      (unsigned long long)header, (unsigned long long)want);
        bad(buf);
    }
    if (r.blob.entry_off < r.blob.code_size()) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "entry_off 在码长内（entry=0x%llX code=%llu）",
                      (unsigned long long)r.blob.entry_off,
                      (unsigned long long)r.blob.code_size());
        ok(buf);
    } else {
        bad("entry_off 在码长内");
    }
    std::printf("[L3] blob %zu 字节（机器码 %llu，entry=0x%llX）\n", b.size(),
                (unsigned long long)r.blob.code_size(), (unsigned long long)r.blob.entry_off);
}

/* ---------------------------------------------------------------- L4 */

void run_ir_case(const char* title, Case& c, const std::function<void(const std::string&)>& checks) {
    std::printf("\n[L4] %s\n", title);
    sl::Program prog(c.ast);
    prog.build();
    const std::string ir = prog.ir_text();
    std::printf("----- IR\n%s----- 断言\n", ir.c_str());
    checks(ir);
}

void run_l4() {
    {
        Case c = build_ul1();
        run_ir_case("U-L1：load/store 是字节地址、无对齐要求、窄写只写该宽度", c, [](const std::string& ir) {
            want_contains(ir, "inttoptr (i64 4096 to ptr)", "地址就是给定的字节地址（0x1000）");
            want_contains(ir, "inttoptr (i64 4104 to ptr)", "`Ptr 0x1000 + 8` = 0x1008（字节算术，不乘元素宽度）");
            want_contains(ir, "store i64 42, ptr", "i64 的写入宽度 = 8 字节");
            want_contains(ir, "store i8 -1, ptr", "U8 的窄写只写 1 字节（0x1FF 被掩码成 0xFF）");
            want_contains(ir, "store i32 -1, ptr", "U32 的窄写只写 4 字节（不是整字读写）");
            want_absent(ir, "llvm.trap", "不插对齐检查 / 陷阱");
            want_absent(ir, "align 8", "对齐只声明 1（U-L1）");
        });
    }
    {
        Case c = build_ul7();
        run_ir_case("U-L7：加宽分两路 + 掩码只有转换与运算结果两处", c, [](const std::string& ir) {
            want_contains(ir, "store i64 -8, ptr", "常量 `S32 -8` 加宽到 U64 按**源类型符号性** sext（全 F）");
            want_contains(ir, "zext i32", "运行期窄值从内存读上来是 zext(trunc_32) 形态");
            want_contains(ir, "and i64 ", "窄值读上来后按 W 掩码");
            want_contains(ir, "4294967295", "掩码 = (1<<32)-1");
            want_absent(ir, "sext", "运行期窄值**不**做符号扩展（与常量那一路相反）");
            want_contains(ir, "store i8 -1, ptr", "转换处的掩码：`U8 0x1FF` → 0xFF");
            want_contains(ir, "and i64 ", "运算结果处也要掩码（U32 乘法）");
        });
    }
    {
        Case c = build_ul8();
        run_ir_case("U-L8：`>>` 恒 lshr、移位量按 W 取模、`asr` 饱和", c, [](const std::string& ir) {
            want_contains(ir, "lshr i64 ", "`>>` 是**逻辑**右移（不是 ashr）");
            want_contains(ir, ", 63", "64 位：移位量 `& 63`");
            want_contains(ir, ", 31", "32 位：移位量 `& 31`（按 W 取模，不是「≥ 位宽就 0」）");
            want_contains(ir, "icmp ult i64", "`asr` 的饱和：先把移位量夹到 W-1");
            want_contains(ir, "sat", "饱和 select（n ≥ W 时取 W-1）");
            want_contains(ir, "ashr i64", "`asr` 才是算术右移（`>>` 与它是两回事）");
            want_contains(ir, "store i32 -4, ptr",
                          "常量 `S32 -8 >> 1`（移位量也是 S32）：sext → lshr → 截回 32 = 0xFFFFFFFC");
            want_contains(ir, "store i64 9223372036854775804, ptr",
                          "同一个表达式换成无类型字面量：unify 拉成 I64 → 不截断，= 0x7FFF…FFFC（`S64` 那一行）");
        });
    }
    {
        Case c = build_div();
        run_ir_case("`/`、`%`：opcode 看被除数；折叠窄常量被零扩展后送有符号除法", c, [](const std::string& ir) {
            want_contains(ir, "udiv i64 ", "U32 被除数 → udiv（除数 S32 不参与）");
            want_contains(ir, "sdiv i64 ", "S32 被除数 → sdiv（除数 U32 不参与）");
            want_contains(ir, "sdiv i64 4294967289", "折叠窄常量 `S32 0 - S32 7` 零扩展成 4294967289 再 sdiv");
            want_contains(ir, "store i8 -3, ptr",
                          "两个常量 → 编译期折叠：`S8 0 - S8 7` ÷ `S8 2` 折出 0xFD（无符号 253 / i8 -3）");
            want_contains(ir, "urem i64 ", "U64 被除数 → urem");
            want_contains(ir, "icmp eq i64 ", "除零不 trap：先判 0 再 select 兜底");
            want_contains(ir, "select i1 ", "商/余数的 0 兜底走 select，不产生陷阱");
            want_absent(ir, "llvm.trap", "除零不 trap（U-L8）");
        });
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    const std::string objdir = argc > 2 ? argv[2] : "build/objs";
    const bool sections = mode == "all" || mode == "coff" || mode == "blob";
    if (mode != "all" && mode != "coff" && mode != "blob" && mode != "ir" && mode != "probe") {
        std::printf("用法：lower_selftest [all|coff|blob|ir|probe] [objdir=build/objs]\n");
        return 2;
    }
    if (sections) {
        std::printf("\n== L2 手写 AST → COFF ==\n");
        const std::vector<uint8_t> plain =
            run_l2_coff(objdir, false, "L2a 无栈帧（任务判据要的形态：只有 .text/.data/.bss）");
        if (plain.empty()) return 1;
        if (mode != "coff") {
            std::printf("\n== L3a COFF → assemble::assemble() ==\n");
            run_l3_blob(plain, objdir, "L3a 无栈帧那个 COFF");
        }
        std::printf("\n");
        const std::vector<uint8_t> framed = run_l2_coff(
            objdir, true,
            "L2b 带栈帧（get_time/memory_clear）—— 额外的 .pdata/.xdata 是接口 §4-4 要丢的那一类");
        if (framed.empty()) return 1;
        if (mode != "coff") {
            std::printf("\n== L3b COFF → assemble::assemble() ==\n");
            run_l3_blob(framed, objdir, "L3b 带 .pdata/.xdata 的那个 COFF（验 §4-4 整节丢弃）");
        }
    } else if (mode == "ir") {
        std::printf("[L3] 本次只请求 L4；L2/L3 用 `coff` / `blob` / `all` 跑\n");
    }
    if (mode == "all" || mode == "ir") {
        std::printf("\n== L4 语义档案的 IR 证据 ==\n");
        run_l4();
    }
    if (mode == "probe") {
        Case c = build_probe();
        sl::Program prog(c.ast);
        prog.build();
        for (const auto& d : prog.diagnostics()) {
            std::fprintf(stderr, "diagnostic: 第 %d 行 %s\n", d.line, d.msg.c_str());
        }
        std::printf("%s", prog.ir_text().c_str());
    }
    std::printf("\n%s（失败 %d 条）\n", g_failed == 0 ? "全部断言通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
