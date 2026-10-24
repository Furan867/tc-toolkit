/* lower.cc —— 类型化 AST → LLVM IR（接口与取舍见 lower.hh）。 */
#include "lower.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <set>
#include <utility>

namespace simplexc {
namespace lower {
namespace {

/* 前缀（stdlib）里定义、生成源码当作「已存在」用的名字：只登记原版编译器里出现过的几个。
 * **这不是内建表** —— 内建表的唯一来源是接口 §2 的 `builtins.def`。 */
struct PrefixGlobal {
    TyRef ty;
    uint64_t init;   /* 前缀那一行给的初值（`var global_seed = Seed 1`），源码还能再赋值 */
};

const std::map<std::string, PrefixGlobal>& prefix_globals() {
    static const std::map<std::string, PrefixGlobal> m{{"global_seed", {ty_seed(), 1}}};
    return m;
}

bool is_prefix_func(const std::string& n) {
    return n == "get_time" || n == "sleep" || n == "memory_clear" ||
           n == "memory_copy" || n == "thread_exit";
}

struct HashConst {
    uint64_t value;
    TyRef ty;
};

/* 源码里直接用、文件里没声明的编译期常量（`// -1` 是源码自带的注释）。 */
const std::map<std::string, HashConst>& hash_consts() {
    static const std::map<std::string, HashConst> m{{"#U64_HIGH", {kMask64, make_int(64, false)}}};
    return m;
}

const std::map<std::string, std::string>& cmp_ops() {
    static const std::map<std::string, std::string> m{
        {"==", "EQ"}, {"!=", "NE"}, {"<", "LT"}, {"<=", "LE"}, {">", "GT"}, {">=", "GE"}};
    return m;
}

bool is_arith(const std::string& op) {
    static const std::set<std::string> ops{"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"};
    return ops.count(op) != 0;
}

const std::map<std::string, std::string>& assign_binop() {
    static const std::map<std::string, std::string> m{
        {"+=", "+"}, {"-=", "-"}, {"*=", "*"}, {"/=", "/"}, {"%=", "%"},
        {"&=", "&"}, {"|=", "|"}, {"^=", "^"}, {"<<=", "<<"}, {">>=", ">>"}};
    return m;
}

constexpr uint64_t kFileTimeUnix = 116444736000000000ull;   /* 1970-01-01 的 FILETIME（100ns） */
constexpr uint64_t kNsPerMs = 1000000ull;

/* 前缀里 `xorshift` 乘的那个常数（`stdlib_prefix.si:1470`）：源码写的是 **15 位**十六进制，
 * 不是教科书的 `0x2545F4914F6CDD1D` —— 实测能分开（`docs/notes/2026-10-18-random.md`）。 */
constexpr uint64_t kSeedMul = 0x2545F4914F6CDD1ull;

/* 先序遍历一个节点的**子节点**（列表字段也下钻）。只给 `captures_of` 用：闭包捕获表要按原型
 * 的「遍历所有 dataclass 字段」来算，字段顺序必须与 `ast.hh` 的声明顺序一致（它逐条对应原型）。 */
template <class F>
void for_each_child(const Node& n, F&& f) {
    switch (n.kind) {
        case NK::Cast:
            f(*static_cast<const CastExpr&>(n).expr);
            break;
        case NK::Unary:
            f(*static_cast<const UnaryExpr&>(n).expr);
            break;
        case NK::Binary: {
            const auto& x = static_cast<const BinaryExpr&>(n);
            f(*x.left);
            f(*x.right);
            break;
        }
        case NK::Cond: {
            const auto& x = static_cast<const CondExpr&>(n);
            f(*x.cond);
            f(*x.then_);
            f(*x.else_);
            break;
        }
        case NK::Call: {
            const auto& x = static_cast<const CallExpr&>(n);
            f(*x.callee);
            for (const auto& a : x.args) f(*a);
            break;
        }
        case NK::Member:
            f(*static_cast<const MemberExpr&>(n).obj);
            break;
        case NK::Index: {
            const auto& x = static_cast<const IndexExpr&>(n);
            f(*x.obj);
            f(*x.index);
            break;
        }
        case NK::ArrayLit:
            for (const auto& it : static_cast<const ArrayLitExpr&>(n).items) f(*it);
            break;
        case NK::CompositeLit:
            for (const auto& fi : static_cast<const CompositeLitExpr&>(n).fields) f(*fi);
            break;
        case NK::Template:
            for (const auto& p : static_cast<const TemplateExpr&>(n).parts) f(*p);
            break;
        case NK::FieldInit:
            f(*static_cast<const FieldInit&>(n).value);
            break;
        case NK::Block:
            for (const auto& s : static_cast<const Block&>(n).stmts) f(*s);
            break;
        /* 裸块的体是 `BlockStmt` 自己那个字段，不走 `NK::Block` 那条 —— 漏掉它会让
         * `captures_of` 看不见块内的自由变量（闭包捕获表少一项）。 */
        case NK::BlockStmt:
            f(*static_cast<const BlockStmt&>(n).block);
            break;
        case NK::VarDecl:
            f(*static_cast<const VarDeclStmt&>(n).init);
            break;
        case NK::Assign: {
            const auto& x = static_cast<const AssignStmt&>(n);
            f(*x.target);
            f(*x.value);
            break;
        }
        case NK::ExprStmt:
            f(*static_cast<const ExprStmt&>(n).expr);
            break;
        case NK::IfCase: {
            const auto& x = static_cast<const IfCase&>(n);
            f(*x.cond);
            f(*x.block);
            break;
        }
        case NK::If: {
            const auto& x = static_cast<const IfStmt&>(n);
            for (const auto& c : x.cases) f(*c);
            if (x.else_block) f(*x.else_block);
            break;
        }
        case NK::PpIf: {
            const auto& x = static_cast<const PpIfStmt&>(n);
            for (const auto& c : x.cases) f(*c);
            if (x.else_block) f(*x.else_block);
            break;
        }
        case NK::While: {
            const auto& x = static_cast<const WhileStmt&>(n);
            f(*x.cond);
            f(*x.block);
            break;
        }
        case NK::SwitchCase: {
            const auto& x = static_cast<const SwitchCase&>(n);
            f(*x.block);
            if (x.value) f(*x.value);
            break;
        }
        case NK::Switch: {
            const auto& x = static_cast<const SwitchStmt&>(n);
            f(*x.subject);
            for (const auto& c : x.cases) f(*c);
            break;
        }
        case NK::Return:
            if (static_cast<const ReturnStmt&>(n).value) f(*static_cast<const ReturnStmt&>(n).value);
            break;
        case NK::Def: {
            const auto& x = static_cast<const DefStmt&>(n);
            for (const auto& p : x.params) f(*p);
            f(*x.body);
            break;
        }
        case NK::Function: {
            const auto& x = static_cast<const FunctionDef&>(n);
            for (const auto& p : x.params) f(*p);
            f(*x.body);
            break;
        }
        default:
            break;   /* 叶子节点与 Param（只有类型拼写） */
    }
}

/* 程序根（`run_sim:` 体）语句树里属于**整单元**的声明：嵌套 def 与 `const #`。
 * 两个消费者各取所需：`in_scope` ① 档的名字表（`Program::scan_scope_names`）、程序根函数里的
 * **前向可见**（`Program::emit_main` 先全部登记 —— 006/058 的调用点排在声明之前）。 */
struct RootDecls {
    std::vector<const DefStmt*> defs;
    std::vector<const VarDeclStmt*> consts;
};

/* 递归地把这些声明收进 `out`：块（While/If/#if/Switch）递归下去，但**不钻 `NK::Def` 的体** ——
 * def 体里的名字属于那个 def 自己的作用域（U-L14 T5/T5d：写在某个 def 体里的嵌套 def，别处查它是假）。 */
void collect_root_decls(const Block& block, RootDecls& out) {
    for (const StmtPtr& st : block.stmts) {
        if (const auto* d = as<DefStmt>(st.get())) {
            out.defs.push_back(d);
        } else if (const auto* v = as<VarDeclStmt>(st.get())) {
            if (v->kw == "const" && !v->name.empty() && v->name[0] == '#') out.consts.push_back(v);
        } else if (const auto* w = as<WhileStmt>(st.get())) {
            collect_root_decls(*w->block, out);
        } else if (const auto* i = as<IfStmt>(st.get())) {
            for (const auto& case_ : i->cases) collect_root_decls(*case_->block, out);
            if (i->else_block != nullptr) collect_root_decls(*i->else_block, out);
        } else if (const auto* p = as<PpIfStmt>(st.get())) {
            for (const auto& case_ : p->cases) collect_root_decls(*case_->block, out);
            if (p->else_block != nullptr) collect_root_decls(*p->else_block, out);
        } else if (const auto* s = as<SwitchStmt>(st.get())) {
            for (const auto& case_ : s->cases) collect_root_decls(*case_->block, out);
        }
    }
}

/* 模板里的一段是「按名字直接调用」时返回被调名，其它形态返回 nullptr。
 * 用途见 `template_expr` 里对参考「每个 def 一份帧」的模拟。 */
const std::string* callee_name_of(const Expr& e) {
    const auto* call = as<CallExpr>(&e);
    if (call == nullptr) return nullptr;
    const auto* id = as<IdentExpr>(call->callee.get());
    return id == nullptr ? nullptr : &id->name;
}

}  // namespace

/* `ty_key` / `size_bits` 已搬进 `sema/types.hh`（唯一实现）。这里**不能**留拷贝：
 * 参数类型 `Ty` 属于 `sema` 命名空间，ADL 会把 `sema::ty_key` 一起拉到调用点，
 * 与 `lower::ty_key` 撞成重载二义（实测：`cap_sig` 那行编译失败）。 */
const char* kind_name(NK k) {
    switch (k) {
        case NK::Literal: return "LiteralExpr";
        case NK::Ident: return "IdentExpr";
        case NK::Cast: return "CastExpr";
        case NK::Unary: return "UnaryExpr";
        case NK::Binary: return "BinaryExpr";
        case NK::Cond: return "CondExpr";
        case NK::Call: return "CallExpr";
        case NK::Member: return "MemberExpr";
        case NK::Index: return "IndexExpr";
        case NK::ArrayLit: return "ArrayLitExpr";
        case NK::CompositeLit: return "CompositeLitExpr";
        case NK::Template: return "TemplateExpr";
        case NK::FieldInit: return "FieldInit";
        case NK::Block: return "Block";
        case NK::BlockStmt: return "BlockStmt";
        case NK::Param: return "Param";
        case NK::VarDecl: return "VarDeclStmt";
        case NK::Assign: return "AssignStmt";
        case NK::ExprStmt: return "ExprStmt";
        case NK::If: return "IfStmt";
        case NK::PpIf: return "PpIfStmt";
        case NK::While: return "WhileStmt";
        case NK::Switch: return "SwitchStmt";
        case NK::Return: return "ReturnStmt";
        case NK::Break: return "BreakStmt";
        case NK::Def: return "DefStmt";
        case NK::Function: return "FunctionDef";
        case NK::IfCase: return "IfCase";
        case NK::SwitchCase: return "SwitchCase";
    }
    return "?";
}

/* ================================================================ Lower：骨架 */

/* 基座上下文：`cur_fn()` 在第一个函数开始之前也得有效（`make_globals` 求全局初值时会
 * 读 `ty_binds`）。它没有 builder、没有作用域，`finish_function` 拒绝弹它。 */
Lower::Lower() { fn_ctxs_.push_back(std::make_unique<FnCtx>()); }

Lower::~Lower() {
    if (cur_fn().bd != nullptr) LLVMDisposeBuilder(cur_fn().bd);
}

void Lower::open_module(const char* name) {
    init_native_target();
    ctx_ = std::make_unique<Context>();
    mod_ = std::make_unique<Module>(ctx_->get(), name);
    i64_ = LLVMInt64TypeInContext(ctx_->get());
    ptr_ = LLVMPointerType(i64_, 0);
    void_ty_ = LLVMVoidTypeInContext(ctx_->get());
    int_types_[64] = i64_;
}

void Lower::record(const std::string& msg, int line) {
    unsupported_.push_back(Diagnostic{line, msg});
    if (!collect_) throw Unsupported(msg, line);
}

LLVMTypeRef Lower::val_ll(const Ty& ty) {
    /* 一个**值**的 LLVM 表示类型（不是内存宽度，也不是 ABI 宽度）：整数一律 i64 位模式 + 掩码，
     * `Str`/`Arr` 是真 `ptr`（放进整数运算会引出 ADDR64 绝对重定位，装配契约 §4-2 直接拒）。 */
    if (ty.kind == TyKind::Struct || ty.kind == TyKind::Str || ty.kind == TyKind::Array) {
        return ll_type(ty);
    }
    return i64_;
}

LLVMTypeRef Lower::ll_type(const Ty& ty) {
    if (ty.kind == TyKind::Struct) {
        auto it = struct_ll_.find(ty.name);
        if (it == struct_ll_.end()) throw CompileFail("struct 类型没建好：" + ty.name);
        return it->second;
    }
    if (ty.kind == TyKind::Str || ty.kind == TyKind::Array) return ptr_;
    return storage_ll(ty);
}

LLVMTypeRef Lower::storage_ll(const Ty& ty) {
    const unsigned n = static_cast<unsigned>(std::max(8, storage_bytes(ty) * 8));
    auto it = int_types_.find(n);
    if (it != int_types_.end()) return it->second;
    LLVMTypeRef t = LLVMIntTypeInContext(ctx(), n);
    int_types_[n] = t;
    return t;
}

LLVMValueRef Lower::const_int(uint64_t v) const { return LLVMConstInt(i64_, v & kMask64, 0); }

LLVMValueRef Lower::const_ptr(uint64_t addr) const {
    return LLVMConstIntToPtr(const_int(addr), ptr_);
}

LLVMValueRef Lower::const_for_storage(const V& v) {
    if (v.ty->kind == TyKind::Struct) {
        auto it = struct_ll_.find(v.ty->name);
        if (it == struct_ll_.end()) throw CompileFail("struct 类型没建好：" + v.ty->name);
        return LLVMConstNull(it->second);
    }
    if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) {
        return v.ll != nullptr ? v.ll : const_ptr(v.c.value_or(0));
    }
    if (!v.c) throw Unsupported("（内部）const_for_storage 拿到非常量", 0);
    return LLVMConstInt(storage_ll(*v.ty), *v.c & mask_of(v.ty->bits), 0);
}

void Lower::mark_no_builtin(LLVMValueRef fnref) {
    /* 原版编译器给每个函数挂 `nobuiltin`（否则 LLVM 会把 `memory_copy` 之类的名字当内建去认）。 */
    const unsigned kind = LLVMGetEnumAttributeKindForName("nobuiltin", 9);
    if (kind != 0) {
        LLVMAddAttributeAtIndex(fnref, LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx(), kind, 0));
    }
}

FnCtx& Lower::begin_function(LLVMValueRef fnref, const TyRef& ret_ty) {
    mark_no_builtin(fnref);
    const FnCtx& outer = cur_fn();   /* 压栈只动 `unique_ptr`，所以这个引用在 push 后仍有效 */
    auto child = std::make_unique<FnCtx>();
    child->parent = &outer;
    /* 身份字段**沿用外层**（与改前逐字段存取时的可见值相同）：前缀函数与内联辅助函数
     * （`get_time`/`sleep`/`memory_*`、`__simplexc_*`）的 `def` 与类型参数绑定来自调用者；
     * `ensure_def_fn` 换上实例自己的 `ty_binds`、`emit_main` 清空 `def`。名字一律由调用点写 ——
     * 三种调用点的取名规则不同（含 `$c<n>` 后缀与 `owner$name#L行`）。 */
    child->name = outer.name;
    child->def = outer.def;
    child->ty_binds = outer.ty_binds;
    child->bd = LLVMCreateBuilderInContext(ctx());
    child->fnref = fnref;
    child->ret_ty = ret_ty;
    child->entry = LLVMAppendBasicBlockInContext(ctx(), fnref, "entry");
    LLVMPositionBuilderAtEnd(child->bd, child->entry);
    child->block = child->entry;
    child->scopes.emplace_back();
    fn_ctxs_.push_back(std::move(child));
    return cur_fn();
}

void Lower::finish_function() {
    if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) {
        if (cur_fn().ret_ty->kind == TyKind::Void) {
            LLVMBuildRetVoid(cur_fn().bd);
        } else {
            warnings_.push_back(cur_fn().name + "：控制流落到函数末尾，补 ret 零值");
            LLVMBuildRet(cur_fn().bd, LLVMConstNull(val_ll(*cur_fn().ret_ty)));
        }
    }
    LLVMDisposeBuilder(cur_fn().bd);
    cur_fn().bd = nullptr;
    /* 上下文与函数同生共死：基座（`parent == nullptr`）不是函数的，弹它就是漏了压栈。 */
    assert(cur_fn().parent != nullptr);
    fn_ctxs_.pop_back();
}

LLVMBasicBlockRef Lower::new_block(const char* name) {
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, name);
    LLVMPositionBuilderAtEnd(cur_fn().bd, bb);
    cur_fn().block = bb;
    return bb;
}

LLVMBasicBlockRef Lower::br_new_block(const char* name) {
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, name);
    LLVMBuildBr(cur_fn().bd, bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, bb);
    cur_fn().block = bb;
    return bb;
}

LLVMValueRef Lower::alloca_in_entry(LLVMTypeRef ty, const std::string& name) {
    /* 插在 **entry 块最前面**：循环里声明也不会每次迭代吃栈，而且它是**静态** alloca
       （LLVM 只把「在 entry 块 + 尺寸是常量」的 alloca 当静态 —— 否则会拉出 `___chkstk_ms` 且每轮涨栈）。 */
    LLVMValueRef first = LLVMGetFirstInstruction(cur_fn().entry);
    if (first == nullptr) {
        LLVMPositionBuilderAtEnd(cur_fn().bd, cur_fn().entry);
    } else {
        LLVMPositionBuilderBefore(cur_fn().bd, first);
    }
    LLVMValueRef a = LLVMBuildAlloca(cur_fn().bd, ty, name.c_str());
    LLVMSetAlignment(a, 1);
    LLVMPositionBuilderAtEnd(cur_fn().bd, cur_fn().block);
    return a;
}

LLVMValueRef Lower::alloca_slot(const TyRef& ty, const std::string& name) {
    return alloca_in_entry(ll_type(*ty), name);
}

/* ================================================================ Lower：作用域与绑定 */

void Lower::bind(const std::string& name, Binding b) { cur_fn().scopes.back()[name] = std::move(b); }

void Lower::bind_root_scope(const std::string& name, Binding b) {
    cur_fn().scopes.front()[name] = std::move(b);
}

const Binding* Lower::lookup(const std::string& name) const {
    for (auto it = cur_fn().scopes.rbegin(); it != cur_fn().scopes.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

/* ================================================================ Lower：值工具 */

LLVMValueRef Lower::llv(const V& v) {
    if (v.ll != nullptr) return v.ll;
    if (!v.c) throw Unsupported("（内部）常量与指令都为空", 0);
    if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) return const_ptr(*v.c);
    return const_int(*v.c);
}

std::optional<uint64_t> Lower::pat_const(const V& v) {
    /* 常量形态下按**源类型符号性**扩展后的 64 位内容（U-L7 的第一路）。 */
    if (!v.c) return std::nullopt;
    if (!is_int(*v.ty)) return *v.c & kMask64;
    uint64_t n = *v.c & mask_of(v.ty->bits);
    if (v.ty->is_signed && v.ty->bits < 64 && ((n >> (v.ty->bits - 1)) & 1)) {
        n |= ~mask_of(v.ty->bits) & kMask64;
    }
    return n & kMask64;
}

LLVMValueRef Lower::pattern_of(const V& v) {
    /* `ext64`：常量按符号性 sext/zext，运行期恒等（本来就是 `zext(trunc_W)` 形态）。 */
    auto c = pat_const(v);
    if (c) return const_int(*c);
    return llv(v);
}

uint64_t Lower::need_const(const V& v, int line) {
    if (!v.c) throw Unsupported("（内部）常量求值拿到非常量", line);
    return *v.c;
}

LLVMValueRef Lower::mask_ll(LLVMValueRef ll, int bits) {
    if (bits >= 64) return ll;
    return LLVMBuildAnd(cur_fn().bd, ll, const_int(mask_of(bits)), "m");
}

V Lower::convert(const V& v, const TyRef& target, int line) {
    if (target->kind == TyKind::Void) throw Unsupported("转换成 None", line);
    if (v.ty->kind == TyKind::Struct || target->kind == TyKind::Struct) {
        if (v.ty->kind == TyKind::Struct && target->kind == TyKind::Struct &&
            v.ty->name == target->name) {
            return v;
        }
        throw Unsupported("转换 " + std::string(ty_kind_name(v.ty->kind)) + " → " +
                              ty_kind_name(target->kind),
                          line);
    }
    auto c = pat_const(v);
    if (target->kind == TyKind::Int) {
        if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) {   /* 真指针 → 整数 */
            return V::instr(LLVMBuildPtrToInt(cur_fn().bd, llv(v), i64_, "pi"), target);
        }
        if (c) return V::konst(target, *c & mask_of(target->bits));
        return V::instr(mask_ll(llv(v), target->bits), target);
    }
    if (target->kind == TyKind::Str || target->kind == TyKind::Array) {   /* 整数地址 → 真指针 */
        if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) {
            return V::instr(llv(v), target);
        }
        if (c) return V::konst(target, *c & kMask64);
        return V::instr(to_ptr(v), target);
    }
    /* ptr / enum：i64 之间位重解释，只换类型标签 */
    if (c) return V::konst(target, *c & kMask64);
    return V::instr(llv(v), target);
}

LLVMValueRef Lower::to_ptr(const V& v) {
    /* `Str`/`Arr`/槽本来就是 ptr，直接用 —— 这样全局/局部地址只会出 `lea rip+…`（REL32），
     * 不出绝对重定位。`Ptr`（i64 游戏地址）走 inttoptr。 */
    if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) return llv(v);
    return LLVMBuildIntToPtr(cur_fn().bd, llv(v), ptr_, "p");
}

V Lower::load_addr(LLVMValueRef p, const TyRef& ty) {
    LLVMTypeRef st = ll_type(*ty);
    LLVMValueRef inst = LLVMBuildLoad2(cur_fn().bd, st, p, "ld");
    LLVMSetAlignment(inst, 1);   /* U-L1：地址是字节地址、无对齐要求，且不许插对齐检查 */
    if (!is_int(*ty)) return V::instr(inst, ty);
    LLVMValueRef v = inst;
    if (st != i64_) v = LLVMBuildZExt(cur_fn().bd, v, i64_, "z");
    if (ty->bits < 64) v = mask_ll(v, ty->bits);
    return V::instr(v, ty);
}

void Lower::store_addr(LLVMValueRef p, const V& v, const TyRef& ty, int line) {
    if (!is_int(*ty)) {
        LLVMValueRef inst = LLVMBuildStore(cur_fn().bd, llv(v), p);
        LLVMSetAlignment(inst, 1);
        return;
    }
    V conv = convert(v, ty, line);
    LLVMTypeRef st = storage_ll(*ty);
    LLVMValueRef ll = llv(conv);
    if (st != i64_) ll = LLVMBuildTrunc(cur_fn().bd, ll, st, "t");   /* 窄写只写该宽度 */
    LLVMValueRef inst = LLVMBuildStore(cur_fn().bd, ll, p);
    LLVMSetAlignment(inst, 1);
}

std::pair<LLVMValueRef, TyRef> Lower::prefix_global(const std::string& name, int line) {
    auto it = prefix_globals().find(name);
    if (it == prefix_globals().end()) throw Unsupported("前缀全局 " + name, line);
    const TyRef& ty = it->second.ty;
    LLVMValueRef g = LLVMAddGlobal(module(), ll_type(*ty), name.c_str());
    LLVMSetLinkage(g, LLVMInternalLinkage);
    LLVMSetInitializer(g, const_int(it->second.init));
    globals_[name] = {g, ty};
    return {g, ty};
}

std::pair<LLVMValueRef, TyRef> Lower::lvalue(const Node& node) {
    switch (node.kind) {
        case NK::Ident: {
            const auto& n = static_cast<const IdentExpr&>(node);
            const Binding* b = lookup(n.name);
            if (b == nullptr) {
                auto g = globals_.find(n.name);
                if (g != globals_.end()) return g->second;
                if (prefix_globals().count(n.name) != 0) return prefix_global(n.name, n.src_line);
                throw Unsupported("未定义的名字 " + n.name, n.src_line);
            }
            if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
                return {b->ptr, b->ty};
            }
            if (b->kind == Binding::Kind::Const) {
                throw Unsupported("给常量 " + n.name + " 赋值", n.src_line);
            }
            throw Unsupported(n.name + " 不是左值", n.src_line);
        }
        case NK::Index: {
            const auto& n = static_cast<const IndexExpr&>(node);
            V base = e(*n.obj);
            if (base.ty->kind != TyKind::Array && base.ty->kind != TyKind::Str) {
                throw Unsupported("下标用在 " + std::string(ty_kind_name(base.ty->kind)) + " 上",
                                  n.src_line);
            }
            V idx = convert(e(*n.index), ty_i64(), n.src_line);
            const int step = storage_bytes(*base.ty->elem);   /* 地址是字节地址（U-L1） */
            LLVMValueRef off = LLVMBuildMul(cur_fn().bd, llv(idx), const_int(uint64_t(step)), "io");
            std::vector<LLVMValueRef> gi{off};
            LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, storage_ll(*make_int(8, false)), to_ptr(base),
                                           gi.data(), 1, "ea");
            return {p, base.ty->elem};
        }
        case NK::Member: {
            const auto& n = static_cast<const MemberExpr&>(node);
            auto base = lvalue(*n.obj);
            const TyRef& bty = base.second;
            if (bty->kind != TyKind::Struct) {
                throw Unsupported("字段访问用在 " + std::string(ty_kind_name(bty->kind)) + " 上",
                                  n.src_line);
            }
            auto st = field_offsets_.find(bty->name);
            if (st == field_offsets_.end()) {
                throw Unsupported("struct " + bty->name + " 没有字段 " + n.name, n.src_line);
            }
            auto f = st->second.find(n.name);
            if (f == st->second.end()) {
                throw Unsupported("struct " + bty->name + " 没有字段 " + n.name, n.src_line);
            }
            if (f->second.off == 0) return {base.first, f->second.ty};
            std::vector<LLVMValueRef> gi{const_int(uint64_t(f->second.off))};
            LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, storage_ll(*make_int(8, false)), base.first,
                                           gi.data(), 1, "fa");
            return {p, f->second.ty};
        }
        default:
            throw Unsupported("左值形态 " + std::string(kind_name(node.kind)), node.src_line);
    }
}

/* ================================================================ Lower：类型解析 */

TyRef Lower::ty_of_name(const std::string& name, int line) {
    if (!name.empty() && name[0] == '@') throw Unsupported("编译期类型参数 " + name, line);
    if (name == "Int") return ty_i64();
    if (name == "Uint" || name == "Sint") return ty_i64();   /* 重载族约束（接口 §1） */
    if (name == "Bool") return ty_bool();
    if (name == "Ptr") return ty_ptr();
    if (name == "Str") return ty_str();
    if (name == "Time") return ty_time();
    if (name == "Seed") return ty_seed();
    if (name == "None" || name == "Void") return ty_void();
    if (enums_.count(name) != 0) return make_enum(name);
    if (structs_.count(name) != 0) return structs_[name];
    if ((name[0] == 'U' || name[0] == 'S') && name.size() > 1) {
        bool digits = true;
        for (size_t i = 1; i < name.size(); ++i) digits = digits && name[i] >= '0' && name[i] <= '9';
        if (digits) {
            const int bits = std::stoi(name.substr(1));
            if (bits < 1 || bits > 64) throw Unsupported("位宽 " + name, line);
            return make_int(bits, name[0] == 'S');
        }
    }
    throw Unsupported("类型 " + name, line);
}

TyRef Lower::ty_of_ref(const TypeRef& ref, int line) {
    if (ref.at) throw Unsupported("编译期类型参数 " + ref.name, line);
    if (ref.elem != nullptr) {
        if (ref.name != "Arr") throw Unsupported("泛型类型 " + ref.name + "[…]", line);
        return make_array(ty_of_ref(*ref.elem, line));
    }
    return ty_of_name(ref.name, line);
}
/* ================================================================ Lower：语句 */

void Lower::lower_block(const Block& block) {
    cur_fn().scopes.emplace_back();
    for (const auto& st : block.stmts) lower_stmt(*st);
    cur_fn().scopes.pop_back();
}

void Lower::lower_stmt(const Stmt& st) {
    try {
        switch (st.kind) {
            case NK::VarDecl: s_VarDeclStmt(static_cast<const VarDeclStmt&>(st)); break;
            case NK::Assign: s_AssignStmt(static_cast<const AssignStmt&>(st)); break;
            case NK::ExprStmt: s_ExprStmt(static_cast<const ExprStmt&>(st)); break;
            case NK::If: s_IfStmt(static_cast<const IfStmt&>(st)); break;
            case NK::PpIf: s_PpIfStmt(static_cast<const PpIfStmt&>(st)); break;
            case NK::While: s_WhileStmt(static_cast<const WhileStmt&>(st)); break;
            case NK::Break: s_BreakStmt(static_cast<const BreakStmt&>(st)); break;
            case NK::Return: s_ReturnStmt(static_cast<const ReturnStmt&>(st)); break;
            case NK::Switch: s_SwitchStmt(static_cast<const SwitchStmt&>(st)); break;
            case NK::Def: s_DefStmt(static_cast<const DefStmt&>(st)); break;
            /* 裸块 = 一层作用域（实测 `work/tmp/bareblock/b01_scope.si`）：`lower_block`
             * 自带 scope push/pop，正是要的语义；break/return 是语句位置的关键字，
             * 块不构成边界，所以不需要额外处理。 */
            case NK::BlockStmt:
                lower_block(*static_cast<const BlockStmt&>(st).block);
                break;
            default:
                throw Unsupported("语句形态 " + std::string(kind_name(st.kind)), st.src_line);
        }
    } catch (const Unsupported& e) {
        unsupported_.push_back(Diagnostic{e.line() != 0 ? e.line() : st.src_line, e.what()});
        if (!collect_) throw;
    }
    /* 块已被终止符收口（return / break）→ 后续语句放进一个不可达块继续发。 */
    if (LLVMGetBasicBlockTerminator(cur_fn().block) != nullptr) new_block("dead");
}

V Lower::root_const_value(const VarDeclStmt& decl) {
    auto hit = root_const_values_.find(&decl);
    if (hit != root_const_values_.end()) return hit->second;
    std::optional<V> v = const_value(*decl.init);
    if (!v) throw Unsupported("编译期常量 " + decl.name + " 的初始化式不是常量", decl.src_line);
    root_const_values_[&decl] = *v;
    return *v;
}

void Lower::s_VarDeclStmt(const VarDeclStmt& st) {
    if (st.kw == "const" && !st.name.empty() && st.name[0] == '#') {
        /* 程序根体里的声明是整单元的（见 `root_const_decls_`）：值走那条备忘，别的函数体里的
         * 引用点取到的就是这一份 —— 两边各求一次会多出一个同内容、不同名的私有全局。 */
        auto root = root_const_decls_.find(st.name);
        if (root != root_const_decls_.end() && root->second == &st) {
            bind(st.name, Binding::constant(root_const_value(st)));
            return;
        }
        auto v = const_value(*st.init);
        if (!v) throw Unsupported("编译期常量 " + st.name + " 的初始化式不是常量", st.src_line);
        bind(st.name, Binding::constant(*v));
        return;
    }
    V v = e(*st.init);
    if (v.ty->kind == TyKind::Void) {
        throw Unsupported(st.name + " 的初始化式没有值", st.src_line);
    }
    if ((st.kw == "let" || st.kw == "const") && v.c) {
        /* 原版编译器把「常量变量」当常量（`ul7_narrow_var.md` 的 fE +304）→ 保真。 */
        bind(st.name, Binding::constant(v));
        return;
    }
    LLVMValueRef a = alloca_slot(v.ty, st.name);
    store_addr(a, v, v.ty, st.src_line);
    bind(st.name, Binding::local(a, v.ty));
}

void Lower::s_AssignStmt(const AssignStmt& st) {
    auto target = lvalue(*st.target);
    V val;
    if (st.op == "=") {
        val = convert(e(*st.value), target.second, st.src_line);
    } else {
        V cur = load_addr(target.first, target.second);
        auto it = assign_binop().find(st.op);
        if (it == assign_binop().end()) throw Unsupported("复合赋值运算符 " + st.op, st.src_line);
        V merged = binop(it->second, cur, e(*st.value), st.src_line);
        val = convert(merged, target.second, st.src_line);
    }
    store_addr(target.first, val, target.second, st.src_line);
}

void Lower::s_ExprStmt(const ExprStmt& st) { e(*st.expr); }

void Lower::s_IfStmt(const IfStmt& st) {
    /* `in_scope("…")` 是**编译期谓词**，哪怕写在裸 `if` 里也一样：判定为假的块**整块丢掉**，
     * 连里面的名字都不解析 —— 与原版编译器一致（facts.md §006 的语义）。 */
    std::vector<const IfCase*> cases;
    for (const auto& case_ : st.cases) {
        auto decided = in_scope_const(*case_->cond);
        if (decided) {
            if (*decided) {
                lower_block(*case_->block);
                return;
            }
            continue;
        }
        cases.push_back(case_.get());
    }
    if (cases.empty()) {
        if (st.else_block != nullptr) lower_block(*st.else_block);
        return;
    }
    lower_if_runtime(cases, st.else_block.get());
}

void Lower::lower_if_runtime(const std::vector<const IfCase*>& cases, const Block* else_block) {
    /* 所有分支跑完汇到同一个 cont 块（惰性创建）。 */
    LLVMBasicBlockRef cont = nullptr;
    auto join = [&]() {
        if (cont == nullptr) cont = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "ifend");
        LLVMBuildBr(cur_fn().bd, cont);
    };
    for (const IfCase* case_ : cases) {
        LLVMValueRef cond = as_cond(e(*case_->cond));
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "then");
        LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "next");
        LLVMBuildCondBr(cur_fn().bd, cond, then_bb, next_bb);
        LLVMPositionBuilderAtEnd(cur_fn().bd, then_bb);
        cur_fn().block = then_bb;
        lower_block(*case_->block);
        if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) join();
        LLVMPositionBuilderAtEnd(cur_fn().bd, next_bb);
        cur_fn().block = next_bb;
    }
    if (else_block != nullptr) {
        lower_block(*else_block);
        if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) join();
    }
    if (cont != nullptr) {
        /* 没有 else 时，`next_bb` 就是「条件全不成立」那条路 —— 它也要汇进 cont。 */
        if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) LLVMBuildBr(cur_fn().bd, cont);
        LLVMPositionBuilderAtEnd(cur_fn().bd, cont);
        cur_fn().block = cont;
    }
}

std::optional<bool> Lower::in_scope_const(const Expr& cond) {
    /* `in_scope("名字")` → true/false；不是这个形态 → nullopt（按运行期处理）。
     * 判定 = ① 整单元预扫的名字表 ∪ ② 词法作用域（参数 / 局部 / 同体嵌套 def）∪ ③ 前缀（stdlib）名。
     * 三档的边界由 U-L14 的片段矩阵定死（docs/notes/2026-10-18-ul14-in-scope.md §0）：内建不算、
     * `var` 不算（它的可见性跟声明顺序走）、别的 def 体里的嵌套名字不算。 */
    const auto* call = as<CallExpr>(&cond);
    if (call == nullptr) return std::nullopt;
    const auto* id = as<IdentExpr>(call->callee.get());
    if (id == nullptr || id->name != "in_scope" || call->args.size() != 1) return std::nullopt;
    const auto* arg = as<LiteralExpr>(call->args[0].get());
    if (arg == nullptr || (arg->lit_kind != "str" && arg->lit_kind != "template")) {
        throw Unsupported("in_scope() 的参数不是字符串字面量", cond.src_line);
    }
    const std::string& t = arg->text;
    /* 编译期常量在源码里带 `#`，查询串带不带都要算（U-L14 V2/V2b）。 */
    if (in_scope_names_.count(t) != 0) return true;
    if (t.empty() || t[0] != '#') {
        if (in_scope_names_.count('#' + t) != 0) return true;
    } else if (in_scope_names_.count(t.substr(1)) != 0) {
        return true;
    }
    if (lookup(t) != nullptr) return true;
    return is_prefix_func(t) || prefix_globals().count(t) != 0;
}

bool Lower::ppif_true(const Expr& cond) {
    auto decided = in_scope_const(cond);
    if (!decided) throw Unsupported("#if 的条件不是 in_scope(\"名字\")", cond.src_line);
    return *decided;
}

void Lower::s_PpIfStmt(const PpIfStmt& st) {
    for (const auto& case_ : st.cases) {
        if (ppif_true(*case_->cond)) {
            lower_block(*case_->block);
            return;
        }
    }
    if (st.else_block != nullptr) lower_block(*st.else_block);
}

void Lower::s_WhileStmt(const WhileStmt& st) {
    LLVMBasicBlockRef head = br_new_block("loop");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "body");
    LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "exit");
    LLVMValueRef cond = as_cond(e(*st.cond));
    LLVMBuildCondBr(cur_fn().bd, cond, body_bb, exit_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, body_bb);
    cur_fn().block = body_bb;
    cur_fn().breaks.push_back({st.label, exit_bb});
    lower_block(*st.block);
    cur_fn().breaks.pop_back();
    if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, exit_bb);
    cur_fn().block = exit_bb;
}

void Lower::s_BreakStmt(const BreakStmt& st) {
    LLVMBasicBlockRef target = nullptr;
    for (auto it = cur_fn().breaks.rbegin(); it != cur_fn().breaks.rend(); ++it) {
        if (st.label.empty() || st.label == it->first) {
            target = it->second;
            break;
        }
    }
    if (target == nullptr) throw Unsupported("break " + st.label, st.src_line);
    LLVMBuildBr(cur_fn().bd, target);
}

void Lower::s_ReturnStmt(const ReturnStmt& st) {
    if (st.value == nullptr) {
        if (cur_fn().ret_ty->kind != TyKind::Void) {
            throw Unsupported("非 None 函数里裸 return", st.src_line);
        }
        LLVMBuildRetVoid(cur_fn().bd);
        return;
    }
    if (cur_fn().ret_ty->kind == TyKind::Void) throw Unsupported("None 函数里 return 值", st.src_line);
    V v = convert(e(*st.value), cur_fn().ret_ty, st.src_line);
    LLVMBuildRet(cur_fn().bd, llv(v));
}

void Lower::s_SwitchStmt(const SwitchStmt& st) {
    V subj = e(*st.subject);
    /* `Str` 主体按**内容**派发（case 值就是字面量）。拿 `pat_const` 当整数比会在 IR 校验就报
     * `icmp eq ptr, i64` —— 语料里那些 `ui_set_*(\`table[{i}][{j}]\`, …)` 正是要靠内容命中。 */
    const bool str_subj = subj.ty->kind == TyKind::Str;
    LLVMValueRef subj_ll =
        str_subj ? nullptr : (subj.c ? const_int(pat_const(subj).value_or(0)) : subj.ll);
    std::vector<const SwitchCase*> plain;
    std::vector<const SwitchCase*> defaults;
    for (const auto& c : st.cases) {
        (c->value != nullptr ? plain : defaults).push_back(c.get());
    }
    LLVMBasicBlockRef end = nullptr;
    auto join = [&]() {
        if (end == nullptr) end = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "swend");
        LLVMBuildBr(cur_fn().bd, end);
    };
    for (const SwitchCase* case_ : plain) {
        LLVMValueRef cnd = nullptr;
        if (str_subj) {
            auto cvv = const_value(*case_->value);
            if (!cvv || cvv->ty->kind != TyKind::Str) {
                throw Unsupported("Str 主体 switch 的 case 值不是 Str 常量", case_->src_line);
            }
            cnd = as_cond(str_compare("==", subj, *cvv, case_->src_line));
        } else {
            auto cvv = const_value(*case_->value);
            if (!cvv) throw Unsupported("switch case 的值不是常量", case_->src_line);
            LLVMValueRef cval = const_int(pat_const(*cvv).value_or(0));
            cnd = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, subj_ll, cval, "sc");
        }
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "case");
        LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "casenext");
        LLVMBuildCondBr(cur_fn().bd, cnd, then_bb, next_bb);
        LLVMPositionBuilderAtEnd(cur_fn().bd, then_bb);
        cur_fn().block = then_bb;
        lower_block(*case_->block);
        if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) join();
        LLVMPositionBuilderAtEnd(cur_fn().bd, next_bb);
        cur_fn().block = next_bb;
    }
    if (!defaults.empty()) {
        lower_block(*defaults[0]->block);
        if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) join();
    } else if (end != nullptr) {
        LLVMBuildBr(cur_fn().bd, end);
    }
    if (end != nullptr) {
        LLVMPositionBuilderAtEnd(cur_fn().bd, end);
        cur_fn().block = end;
    }
}

void Lower::s_DefStmt(const DefStmt& st) {
    /* 嵌套 def 只登记、不生成 —— 真被调用时才发（死代码不该进 IR）。 */
    bind(st.name, Binding::nested(&st));
}

Closure Lower::captures_of(const DefStmt& st) {
    Closure out;
    std::set<const DefStmt*> visiting;
    visiting.insert(&st);   /* 兄弟 def 互调会绕回来，先把自己放进环里 */
    std::vector<std::string> params;
    for (const auto& p : st.params) params.push_back(p->name);
    collect_captures(*st.body, params, false, out, visiting);
    return out;
}

Closure Lower::def_captures(const FunctionDef& d) {
    /* 顶层 def 是**真函数**、在调用点展开的效果只能靠捕获表还原：这里算的是「它（连同它体里
     * 按名调用的嵌套 def）真正用到、而调用点绑在栈槽/捕获上的名字」。一个 def 的自由名全落在
     * 模块级（全局 / const / 枚举 / 前缀 / 其它 def）时结果是空的 ⇒ 签名与应用面都不变。
     * `into_own_defs`：顶层 def 体里**自己声明**的嵌套 def 也要往下摊平 —— 它的捕获在它被
     * 调用时才懒生成，而那一刻看到的是顶层 def 的作用域，所以顶层 def 必须先把这个名字捕进来
     * （110：root → `mode_run` → `handle_test_result` → `get_input_switched` 的 `seen`）。 */
    Closure out;
    std::set<const DefStmt*> visiting;
    std::vector<std::string> params;
    for (const auto& p : d.params) params.push_back(p->name);
    collect_captures(*d.body, params, true, out, visiting);
    return out;
}

void Lower::collect_captures(const Block& body, const std::vector<std::string>& params,
                             bool into_own_defs, Closure& out,
                             std::set<const DefStmt*>& visiting) {
    /* 嵌套 def 体里引用了哪些**外层局部**（按引用捕获 → 传 alloca 指针）。自己的参数、`var`/
     * `let`/内层 `def` 都算「自己的」，同名遮蔽外层 —— 不往内层 `DefStmt` 的体里钻：内层 def 真被
     * 调用时再算它自己的捕获，那时本函数的参数/局部/捕获都已经在作用域里。
     *
     * 但**兄弟 def**（本函数体里按名调用、定义在外面的嵌套 def）必须在这里就摊平：原版编译器按
     * **最外层调用点**解析 free variable（`docs/notes/2026-10-18-in-scope-fix.md` §6 第 1 条 ——
     * L/M 片段里被查的 def 体直接出现在调用者代码里），而我们把它生成成一个真函数，它的捕获
     * 只能由**调用它的那一层**带进来。006：`handle_test_result` 调 `halt`，`halt` 用
     * `run_sim` 体的 `burst_target_cycle` —— 后者得先被 `handle_test_result` 捕获、再原样传给
     * `halt`（110 的 `seen` / 075 的 `test` 同形）。 */
    std::set<std::string> own(params.begin(), params.end());
    /* 本体内声明的嵌套 def：名字进 `own`（不当变量捕获），但在 `into_own_defs` 时要顺着它摊平。 */
    std::map<std::string, const DefStmt*> own_defs;

    std::function<void(const Node&)> collect = [&](const Node& n) {
        if (n.kind == NK::VarDecl) own.insert(static_cast<const VarDeclStmt&>(n).name);
        if (n.kind == NK::Def) {
            const auto& d = static_cast<const DefStmt&>(n);
            own.insert(d.name);
            if (into_own_defs) own_defs.emplace(d.name, &d);
        }
        for_each_child(n, collect);
    };
    collect(body);

    std::function<void(const Node&)> rec = [&](const Node& n) {
        if (n.kind == NK::Def) return;   /* 内层 def 有自己的作用域 */
        if (n.kind == NK::Ident) {
            const auto& id = static_cast<const IdentExpr&>(n);
            const DefStmt* target = nullptr;
            if (own.count(id.name) == 0) {
                const Binding* b = lookup(id.name);
                if (b != nullptr) {
                    if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
                        bool known = false;
                        for (const auto& c : out.caps) known = known || c.name == id.name;
                        if (!known) out.caps.push_back(Capture{id.name, b->ptr, b->ty});
                        return;
                    }
                    if (b->kind == Binding::Kind::Def) target = b->def;
                } else {
                    /* 程序根语句树里的嵌套 def 不在词法作用域里（`call_named` 走 root_defs_ 那条路），
                     * 这里用同一张表认名字，否则它捕获的外层变量传不进去。 */
                    auto rd = root_defs_.find(id.name);
                    if (rd != root_defs_.end()) target = rd->second;
                }
            }
            if (target == nullptr && into_own_defs) {
                auto od = own_defs.find(id.name);
                if (od != own_defs.end()) target = od->second;
            }
            if (target != nullptr && visiting.insert(target).second) {
                bool known = false;
                for (const DefStmt* d : out.defs) known = known || d == target;
                if (!known) out.defs.push_back(target);
                std::vector<std::string> inner_params;
                for (const auto& p : target->params) inner_params.push_back(p->name);
                collect_captures(*target->body, inner_params, into_own_defs, out, visiting);
            }
        }
        for_each_child(n, rec);
    };
    rec(body);
}

/* ================================================================ Lower：表达式 */

V Lower::e(const Expr& node) {
    switch (node.kind) {
        case NK::Literal: return e_LiteralExpr(static_cast<const LiteralExpr&>(node));
        case NK::Ident: return e_IdentExpr(static_cast<const IdentExpr&>(node));
        case NK::Cast: return e_CastExpr(static_cast<const CastExpr&>(node));
        case NK::Unary: return e_UnaryExpr(static_cast<const UnaryExpr&>(node));
        case NK::Binary: return e_BinaryExpr(static_cast<const BinaryExpr&>(node));
        case NK::Cond: return e_CondExpr(static_cast<const CondExpr&>(node));
        case NK::Call: return e_CallExpr(static_cast<const CallExpr&>(node));
        case NK::Member: return e_MemberExpr(static_cast<const MemberExpr&>(node));
        case NK::Index: return e_IndexExpr(static_cast<const IndexExpr&>(node));
        case NK::ArrayLit: return e_ArrayLitExpr(static_cast<const ArrayLitExpr&>(node));
        case NK::CompositeLit:
            return e_CompositeLitExpr(static_cast<const CompositeLitExpr&>(node));
        case NK::Template: return e_TemplateExpr(static_cast<const TemplateExpr&>(node));
        default:
            throw Unsupported("表达式形态 " + std::string(kind_name(node.kind)), node.src_line);
    }
}

V Lower::e_LiteralExpr(const LiteralExpr& node) {
    if (node.lit_kind == "int") {
        const uint64_t v = static_cast<uint64_t>(node.value) & kMask64;
        V out;
        out.ll = const_int(v);
        out.ty = ty_i64();
        out.c = v;
        return out;
    }
    if (node.lit_kind == "str" || node.lit_kind == "template") return string_literal(node);
    throw Unsupported("字面量 " + node.lit_kind, node.src_line);
}

V Lower::e_IdentExpr(const IdentExpr& node) {
    const std::string& name = node.name;
    if (name == "true" || name == "false") {
        const uint64_t c = name == "true" ? 1 : 0;
        V v;
        v.ll = const_int(c);
        v.ty = ty_bool();
        v.c = c;
        return v;
    }
    if (const Binding* b = lookup(name)) {
        if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
            return load_addr(b->ptr, b->ty);
        }
        if (b->kind == Binding::Kind::Const) return *b->value;
        throw Unsupported(name + " 只能作为被调函数", node.src_line);
    }
    auto g = globals_.find(name);
    if (g != globals_.end()) return load_addr(g->second.first, g->second.second);
    if (prefix_globals().count(name) != 0) {
        auto pg = prefix_global(name, node.src_line);
        return load_addr(pg.first, pg.second);
    }
    auto em = enum_member_.find(name);
    if (em != enum_member_.end()) {
        V v;
        v.ll = const_int(uint64_t(em->second.second));
        v.ty = make_enum(em->second.first);
        v.c = uint64_t(em->second.second);
        return v;
    }
    auto cst = consts_.find(name);
    if (cst != consts_.end()) return cst->second;
    auto hc = hash_consts().find(name);
    if (hc != hash_consts().end()) {
        V v;
        v.ll = const_int(hc->second.value);
        v.ty = hc->second.ty;
        v.c = hc->second.value;
        return v;
    }
    auto rc = root_const_decls_.find(name);
    if (rc != root_const_decls_.end()) return root_const_value(*rc->second);
    throw Unsupported("未定义的名字 " + name, node.src_line);
}

V Lower::e_CastExpr(const CastExpr& node) {
    TyRef target = ty_of_ref(*node.target, node.src_line);
    if (target->kind == TyKind::Array && node.expr->kind == NK::ArrayLit &&
        static_cast<const ArrayLitExpr&>(*node.expr).items.empty()) {
        return empty_array(target, node.src_line);
    }
    return convert(e(*node.expr), target, node.src_line);
}

V Lower::e_UnaryExpr(const UnaryExpr& node) {
    if (node.op == "+") return e(*node.expr);
    if (node.op == "!") {
        V v = e(*node.expr);
        auto c = pat_const(v);
        if (c) {
            const uint64_t r = *c == 0 ? 1 : 0;
            V out;
            out.ll = const_int(r);
            out.ty = ty_bool();
            out.c = r;
            return out;
        }
        LLVMValueRef cnd = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, llv(v), const_int(0), "n");
        return V::instr(LLVMBuildZExt(cur_fn().bd, cnd, i64_, "nb"), ty_bool());
    }
    V v = e(*node.expr);
    if (!is_int(*v.ty)) {
        throw Unsupported("一元 " + node.op + " 用在 " + ty_kind_name(v.ty->kind), node.src_line);
    }
    auto c = pat_const(v);
    if (c) return V::konst(v.ty, fold_unary(node.op, *c, *v.ty));
    LLVMValueRef ll = nullptr;
    if (node.op == "-") {
        ll = LLVMBuildSub(cur_fn().bd, const_int(0), llv(v), "neg");
    } else if (node.op == "~") {
        ll = LLVMBuildXor(cur_fn().bd, llv(v), const_int(mask_of(v.ty->bits)), "bnot");
    } else {
        throw Unsupported("一元运算符 " + node.op, node.src_line);
    }
    return V::instr(mask_ll(ll, v.ty->bits), v.ty);
}

V Lower::e_BinaryExpr(const BinaryExpr& node) {
    if (node.op == "&&" || node.op == "||") return logical(node);
    V a = e(*node.left);
    V b = e(*node.right);
    if (cmp_ops().count(node.op) != 0) return compare(node.op, a, b, node.src_line);
    return binop(node.op, a, b, node.src_line);
}

V Lower::e_CondExpr(const CondExpr& node) {
    /* 形状是 **condbr + phi**，不是 `select`：两臂里都可能有 `load(…)`，`select` 会两支都求值
     * —— 与短路语义不一致。每个臂的 `convert` 必须在**它自己那个块**里做完。 */
    LLVMValueRef cond = as_cond(e(*node.cond));
    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "t_then");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "t_else");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "t_end");
    LLVMBuildCondBr(cur_fn().bd, cond, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(cur_fn().bd, then_bb);
    cur_fn().block = then_bb;
    V tv = e(*node.then_);
    LLVMBasicBlockRef t_bb = cur_fn().block;
    LLVMPositionBuilderAtEnd(cur_fn().bd, else_bb);
    cur_fn().block = else_bb;
    V ev = e(*node.else_);
    LLVMBasicBlockRef e_bb = cur_fn().block;

    if (tv.ty->kind == TyKind::Void || ev.ty->kind == TyKind::Void) {
        throw Unsupported("三目的一支没有值", node.src_line);
    }
    TyRef ty = unify(tv.ty, ev.ty, node.src_line);
    LLVMPositionBuilderAtEnd(cur_fn().bd, t_bb);
    tv = convert(tv, ty, node.src_line);
    LLVMBuildBr(cur_fn().bd, end_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, e_bb);
    ev = convert(ev, ty, node.src_line);
    LLVMBuildBr(cur_fn().bd, end_bb);

    LLVMPositionBuilderAtEnd(cur_fn().bd, end_bb);
    cur_fn().block = end_bb;
    /* phi 的结果类型是**值的类型**（窄整数一律 i64 位模式），不是内存宽度。 */
    LLVMValueRef phi = LLVMBuildPhi(cur_fn().bd, val_ll(*ty), "cnd");
    std::vector<LLVMValueRef> vals{llv(tv), llv(ev)};
    std::vector<LLVMBasicBlockRef> blocks{t_bb, e_bb};
    LLVMAddIncoming(phi, vals.data(), blocks.data(), 2);
    return V::instr(phi, ty);
}

V Lower::e_CallExpr(const CallExpr& node) {
    const auto* callee = as<IdentExpr>(node.callee.get());
    if (callee != nullptr) return call_named(callee->name, node);
    const auto* mem = as<MemberExpr>(node.callee.get());
    if (mem != nullptr) return call_method(*mem, node);
    throw Unsupported("被调者形态 " + std::string(kind_name(node.callee->kind)), node.src_line);
}

V Lower::e_MemberExpr(const MemberExpr& node) {
    auto addr = lvalue(node);
    return load_addr(addr.first, addr.second);
}

V Lower::e_IndexExpr(const IndexExpr& node) {
    auto addr = lvalue(node);
    return load_addr(addr.first, addr.second);
}

V Lower::e_CompositeLitExpr(const CompositeLitExpr& node) {
    return composite_literal(node);
}

V Lower::e_ArrayLitExpr(const ArrayLitExpr& node) { return array_literal(node); }

V Lower::e_TemplateExpr(const TemplateExpr& node) { return template_expr(node); }

/* ================================================================ Lower：运算 */

TyRef Lower::unify(const TyRef& a, const TyRef& b, int line) {
    if (a->kind == TyKind::Ptr || b->kind == TyKind::Ptr) {
        if ((a->kind == TyKind::Ptr || a->kind == TyKind::Int) &&
            (b->kind == TyKind::Ptr || b->kind == TyKind::Int)) {
            return ty_ptr();
        }
        throw Unsupported("指针与 " +
                              std::string(ty_kind_name(a->kind == TyKind::Ptr ? b->kind : a->kind)) +
                              " 混用",
                          line);
    }
    if (a->kind == TyKind::Struct || b->kind == TyKind::Struct) {
        throw Unsupported("struct 参与运算", line);
    }
    if (a->kind == TyKind::Array || b->kind == TyKind::Array) {
        throw Unsupported("Arr 参与运算", line);
    }
    if (a->kind == TyKind::Str || b->kind == TyKind::Str) {
        if (a->kind == TyKind::Str && b->kind == TyKind::Str) return ty_str();
        throw Unsupported("Str 与整数混用", line);
    }
    if (a->kind == TyKind::Enum && b->kind == TyKind::Enum) {
        if (a->name != b->name) {
            throw Unsupported("枚举 " + a->name + " 与 " + b->name + " 混用", line);
        }
        return a;
    }
    if (a->kind == TyKind::Enum) return b;
    if (b->kind == TyKind::Enum) return a;
    if (is_int(*a) && is_int(*b)) {
        if (a->bits == b->bits) return a;   /* 同宽不同符号性：以左操作数为准 */
        return a->bits > b->bits ? a : b;
    }
    throw Unsupported("类型 " + std::string(ty_kind_name(a->kind)) + " 与 " +
                          ty_kind_name(b->kind) + " 混用",
                      line);
}

TyRef Lower::arith_ty(const V& a, const V& b, const std::string& op, int line) {
    /* 整型**字面量**取窄的那一侧的位宽（字面量多态）。参考在 `U8 x` 上把 `~x + 1` 算成
     * 8 位回绕（x == 0 得 0）；按 `unify` 的「宽者胜」会得 64 位的 256，于是
     * `check_output` 每轮都早退 fail —— 实测 `docs/notes/2026-10-18-test-result-chain2.md`。
     * 只铺给 `+`：同一条规则套到 `%`/`<<`/`/` 上会让 176/177/180/rv32im 从全等掉成 diff。 */
    if (op == "+" && is_int(*a.ty) && is_int(*b.ty) && a.ty->bits != b.ty->bits) {
        const bool narrow_is_a = a.ty->bits < b.ty->bits;
        const TyRef& narrow = narrow_is_a ? a.ty : b.ty;
        const V& literal = narrow_is_a ? b : a;
        if (narrow->bits < 64 && pat_const(literal).has_value()) return narrow;
    }
    return unify(a.ty, b.ty, line);
}

LLVMValueRef Lower::as_cond(const V& v) {
    if (v.ty->kind == TyKind::Void) throw Unsupported("None 值当条件", 0);
    return LLVMBuildICmp(cur_fn().bd, LLVMIntNE, llv(v), const_int(0), "c");
}

LLVMValueRef Lower::sext64(LLVMValueRef ll, int bits) {
    if (bits >= 64) return ll;
    const int sh = 64 - bits;
    return LLVMBuildAShr(
        cur_fn().bd, LLVMBuildShl(cur_fn().bd, ll, const_int(uint64_t(sh)), "sh"), const_int(uint64_t(sh)),
        "sa");
}

V Lower::compare(const std::string& op, const V& a, const V& b, int line) {
    TyRef ty = unify(a.ty, b.ty, line);
    if (ty->kind == TyKind::Str) return str_compare(op, a, b, line);
    V av = convert(a, ty, line);
    V bv = convert(b, ty, line);
    auto ca = pat_const(av);
    auto cb = pat_const(bv);
    if (ca && cb) return V::konst(ty_bool(), fold_cmp(op, *ca, *cb, *ty));
    LLVMValueRef x = llv(av);
    LLVMValueRef y = llv(bv);
    const bool sgn = is_int(*ty) && ty->is_signed;
    if (sgn && ty->bits < 64) {
        x = sext64(x, ty->bits);
        y = sext64(y, ty->bits);
    }
    const std::string& base = cmp_ops().at(op);
    LLVMIntPredicate pred = LLVMIntEQ;
    if (base == "EQ") pred = LLVMIntEQ;
    else if (base == "NE") pred = LLVMIntNE;
    else if (sgn) {
        pred = base == "LT" ? LLVMIntSLT : base == "LE" ? LLVMIntSLE
               : base == "GT" ? LLVMIntSGT : LLVMIntSGE;
    } else {
        pred = base == "LT" ? LLVMIntULT : base == "LE" ? LLVMIntULE
               : base == "GT" ? LLVMIntUGT : LLVMIntUGE;
    }
    LLVMValueRef cnd = LLVMBuildICmp(cur_fn().bd, pred, x, y, "cmp");
    return V::instr(LLVMBuildZExt(cur_fn().bd, cnd, i64_, "cmpz"), ty_bool());
}

V Lower::str_compare(const std::string& op, const V& a, const V& b, int line) {
    if (op != "==" && op != "!=") throw Unsupported("Str 比大小（" + op + "）", line);
    LLVMValueRef la = len_of(a, line);
    LLVMValueRef lb = len_of(b, line);
    LLVMValueRef same = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, la, lb, "sleq");
    /* 长度不等时把比较长度取 0 —— 否则逐字节会读越过短的那一条的 payload。 */
    LLVMValueRef n = LLVMBuildSelect(cur_fn().bd, same, la, const_int(0), "sln");
    LLVMValueRef eqv = emit_call(ensure_streq_fn(), {to_ptr(a), to_ptr(b), n});
    LLVMValueRef eq = LLVMBuildICmp(cur_fn().bd, LLVMIntNE, eqv, const_int(0), "seq");
    LLVMValueRef res = LLVMBuildAnd(cur_fn().bd, eq, same, "seqb");
    if (op == "!=") res = LLVMBuildNot(cur_fn().bd, res, "snot");
    return V::instr(LLVMBuildZExt(cur_fn().bd, res, i64_, "seqz"), ty_bool());
}

V Lower::logical(const BinaryExpr& node) {
    /* `&&` / `||` 短路（`get_command` 读的是游戏内存，不能随意重排）。 */
    LLVMValueRef lhs = as_cond(e(*node.left));
    LLVMBasicBlockRef lhs_bb = cur_fn().block;
    LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "rhs");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "lend");
    if (node.op == "&&") LLVMBuildCondBr(cur_fn().bd, lhs, rhs_bb, end_bb);
    else LLVMBuildCondBr(cur_fn().bd, lhs, end_bb, rhs_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, rhs_bb);
    cur_fn().block = rhs_bb;
    LLVMValueRef rhs = as_cond(e(*node.right));
    LLVMBasicBlockRef rhs_end = cur_fn().block;
    LLVMBuildBr(cur_fn().bd, end_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, end_bb);
    cur_fn().block = end_bb;
    LLVMTypeRef i1 = LLVMIntTypeInContext(ctx(), 1);
    LLVMValueRef phi = LLVMBuildPhi(cur_fn().bd, i1, "lg");
    std::vector<LLVMValueRef> vals{LLVMConstInt(i1, node.op == "&&" ? 0 : 1, 0), rhs};
    std::vector<LLVMBasicBlockRef> blocks{lhs_bb, rhs_end};
    LLVMAddIncoming(phi, vals.data(), blocks.data(), 2);
    return V::instr(LLVMBuildZExt(cur_fn().bd, phi, i64_, "lgz"), ty_bool());
}

V Lower::binop(const std::string& op, const V& a, const V& b, int line) {
    if (!is_arith(op)) throw Unsupported("运算符 " + op, line);
    TyRef ty = arith_ty(a, b, op, line);
    V av = convert(a, ty, line);
    V bv = convert(b, ty, line);
    if (ty->kind == TyKind::Str) {
        /* `+` 是唯一的 Str 运算（实测：`"ab" + "cd"` → 长度 4 的 `abcd`）。 */
        if (op != "+") throw Unsupported("Str 的 " + op, line);
        return str_concat(av, bv, line);
    }
    auto ca = pat_const(av);
    auto cb = pat_const(bv);
    if (ty->kind == TyKind::Ptr) {
        if (op != "+" && op != "-") throw Unsupported("指针 " + op, line);
        if (ca && cb) {
            const uint64_t r = op == "+" ? *ca + *cb : *ca - *cb;
            return V::konst(ty_ptr(), r & kMask64);
        }
        LLVMValueRef ll = op == "+" ? LLVMBuildAdd(cur_fn().bd, llv(av), llv(bv), "pa")
                                     : LLVMBuildSub(cur_fn().bd, llv(av), llv(bv), "pa");
        return V::instr(ll, ty);
    }
    if (ca && cb) return V::konst(ty, fold_arith(op, *ca, *cb, *ty));
    const int bits = ty->bits;
    LLVMValueRef x = llv(av);
    LLVMValueRef y = llv(bv);
    if (op == "/" || op == "%") {
        /* 运行期结果**不**截断到 W（facts.md §`/`、`%` 的第二条），所以这里不套掩码。 */
        return V::instr(div(op, x, y, *ty), ty);
    }
    if (op == "<<") {
        LLVMValueRef amt = LLVMBuildAnd(cur_fn().bd, y, const_int(uint64_t(bits - 1)), "amt");
        return V::instr(mask_ll(LLVMBuildShl(cur_fn().bd, x, amt, "shl"), bits), ty);
    }
    if (op == ">>") {
        /* U-L8：**永远 lshr**（不是 ashr），左操作数走 `pattern_of`（常量按符号性扩），再截回 W。 */
        LLVMValueRef amt = LLVMBuildAnd(cur_fn().bd, y, const_int(uint64_t(bits - 1)), "amt");
        LLVMValueRef ll = LLVMBuildLShr(cur_fn().bd, pattern_of(av), amt, "shr");
        return V::instr(mask_ll(ll, bits), ty);
    }
    LLVMValueRef r = nullptr;
    if (op == "+") r = LLVMBuildAdd(cur_fn().bd, x, y, "op");
    else if (op == "-") r = LLVMBuildSub(cur_fn().bd, x, y, "op");
    else if (op == "*") r = LLVMBuildMul(cur_fn().bd, x, y, "op");
    else if (op == "&") r = LLVMBuildAnd(cur_fn().bd, x, y, "op");
    else if (op == "|") r = LLVMBuildOr(cur_fn().bd, x, y, "op");
    else r = LLVMBuildXor(cur_fn().bd, x, y, "op");
    return V::instr(mask_ll(r, bits), ty);
}

LLVMValueRef Lower::div(const std::string& op, LLVMValueRef x, LLVMValueRef y, const Ty& ty) {
    /* opcode 跟**被除数**的符号性（facts.md §`/`、`%`），零除数不 trap（商 0、余数 = 被除数），
     * 有符号再避开 `INT_MIN / -1`。注意操作数**没有**过 `pattern_of`：常量在这里用的是
     * W 位掩码后的裸位模式 —— 原版编译器那个「窄常量零扩展成 64 位再送有符号除法」的不自洽
     * 就是从这个形状出来的，别顺手改成 pattern_of。 */
    const int bits = ty.bits;
    LLVMValueRef zero = const_int(0);
    LLVMValueRef bz = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, y, zero, "bz");
    LLVMValueRef safe = LLVMBuildSelect(cur_fn().bd, bz, const_int(1), y, "sb");
    LLVMValueRef bm1 = nullptr;
    if (ty.is_signed) {
        bm1 = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, y, const_int(mask_of(bits)), "bm1");
        safe = LLVMBuildSelect(cur_fn().bd, bm1, const_int(1), safe, "sb2");
    }
    LLVMValueRef raw = nullptr;
    if (op == "/") {
        raw = ty.is_signed ? LLVMBuildSDiv(cur_fn().bd, x, safe, "q") : LLVMBuildUDiv(cur_fn().bd, x, safe, "q");
    } else {
        raw = ty.is_signed ? LLVMBuildSRem(cur_fn().bd, x, safe, "r") : LLVMBuildURem(cur_fn().bd, x, safe, "r");
    }
    LLVMValueRef res = nullptr;
    if (op == "/") {
        res = LLVMBuildSelect(cur_fn().bd, bz, zero, raw, "qz");
        if (bm1 != nullptr) {
            LLVMValueRef neg = mask_ll(LLVMBuildSub(cur_fn().bd, zero, x, "nq"), bits);
            res = LLVMBuildSelect(cur_fn().bd, bm1, neg, res, "qz2");
        }
    } else {
        res = LLVMBuildSelect(cur_fn().bd, bz, x, raw, "rz");
        if (bm1 != nullptr) res = LLVMBuildSelect(cur_fn().bd, bm1, zero, res, "rz2");
    }
    return res;
}

/* ================================================================ Lower：调用 */

TyRef Lower::static_type(const Node& node) {
    /* 只用于**重载粗判**；拿不到就返回 nullptr（不猜）。 */
    try {
        switch (node.kind) {
            case NK::Literal: {
                const auto& n = static_cast<const LiteralExpr&>(node);
                if (n.lit_kind == "int") return ty_i64();
                if (n.lit_kind == "str" || n.lit_kind == "template") return ty_str();
                return nullptr;
            }
            case NK::Ident: {
                const auto& n = static_cast<const IdentExpr&>(node);
                /* `true`/`false` 不是绑定（只在 `e_IdentExpr` 里特判）：少了这两行，
                   `ui_set_hidden("x", false)` 这种实参的重载粗判拿不到 Bool → 无候选。 */
                if (n.name == "true" || n.name == "false") return ty_bool();
                if (const Binding* b = lookup(n.name)) {
                    if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
                        return b->ty;
                    }
                    if (b->kind == Binding::Kind::Const) return b->value->ty;
                    return nullptr;
                }
                auto g = globals_.find(n.name);
                if (g != globals_.end()) return g->second.second;
                auto pg = prefix_globals().find(n.name);
                if (pg != prefix_globals().end()) return pg->second.ty;
                auto em = enum_member_.find(n.name);
                if (em != enum_member_.end()) return make_enum(em->second.first);
                auto cst = consts_.find(n.name);
                if (cst != consts_.end()) return cst->second.ty;
                auto hc = hash_consts().find(n.name);
                if (hc != hash_consts().end()) return hc->second.ty;
                return nullptr;
            }
            case NK::Cast: {
                const auto& n = static_cast<const CastExpr&>(node);
                return ty_of_ref(*n.target, n.src_line);
            }
            case NK::Unary: {
                const auto& n = static_cast<const UnaryExpr&>(node);
                return n.op == "!" ? ty_bool() : static_type(*n.expr);
            }
            case NK::Binary: {
                const auto& n = static_cast<const BinaryExpr&>(node);
                if (cmp_ops().count(n.op) != 0 || n.op == "&&" || n.op == "||") return ty_bool();
                TyRef a = static_type(*n.left);
                TyRef b = static_type(*n.right);
                if (a && b) return unify(a, b, n.src_line);
                return nullptr;
            }
            case NK::Call: {
                const auto& n = static_cast<const CallExpr&>(node);
                if (const auto* mem = as<MemberExpr>(n.callee.get())) {
                    if (mem->name == "len") return ty_i64();
                    if (mem->name == "ms") return ty_time();
                    return nullptr;
                }
                const auto* id = as<IdentExpr>(n.callee.get());
                if (id == nullptr) return nullptr;
                const std::string& name = id->name;
                if ((name == "load" || name == "raw_cast") && !n.type_args.empty()) {
                    return ty_of_ref(*n.type_args[0], n.src_line);
                }
                /* `load(type_of(x), p)`：类型实参写成类型值表达式时 `type_args` 是空的。 */
                if (name == "load" && n.type_args.empty() && n.args.size() == 2) {
                    try {
                        return type_value_of(*n.args[0], n.src_line);
                    } catch (const Unsupported&) {
                        return nullptr;
                    }
                }
                /* `array(<T>, n)`：`var a = array(<U8>, 16)` 的静态类型决定 `a[i]` 的步长。 */
                if (name == "array" && !n.type_args.empty()) {
                    return make_array(ty_of_ref(*n.type_args[0], n.src_line));
                }
                if ((name == "min" || name == "max" || name == "asr" || name == "clz") &&
                    !n.args.empty()) {
                    return static_type(*n.args[0]);
                }
                if (name == "sleep" || name == "memory_clear" || name == "memory_copy" ||
                    name == "thread_exit") {
                    return ty_void();
                }
                if (name == "get_time") return ty_time();
                if (const FunctionDef* d = pick_def(name, n)) {
                    Inst inst = resolve_inst(d->params, n, d->lo);
                    return inst_ret_type(d->ret.get(), inst, d->lo);
                }
                if (const Binding* b = lookup(name)) {
                    if (b->kind == Binding::Kind::Def) return defstmt_ret(*b->def);
                }
                return nullptr;
            }
            case NK::Member: {
                const auto& n = static_cast<const MemberExpr&>(node);
                TyRef obj = static_type(*n.obj);
                if (obj && obj->kind == TyKind::Struct) {
                    auto st = field_offsets_.find(obj->name);
                    if (st != field_offsets_.end()) {
                        auto f = st->second.find(n.name);
                        if (f != st->second.end()) return f->second.ty;
                    }
                }
                return nullptr;
            }
            case NK::Index: {
                const auto& n = static_cast<const IndexExpr&>(node);
                TyRef obj = static_type(*n.obj);
                if (obj && (obj->kind == TyKind::Array || obj->kind == TyKind::Str)) {
                    return obj->elem;
                }
                return nullptr;
            }
            case NK::CompositeLit: {
                const auto& n = static_cast<const CompositeLitExpr&>(node);
                return ty_of_ref(*n.typ, n.src_line);
            }
            case NK::Template: return ty_str();
            default:
                return nullptr;
        }
    } catch (const Unsupported&) {
        return nullptr;
    }
}

bool Lower::compatible(const TyRef& arg, const TyRef& param) {
    if (!arg || arg->kind != param->kind) return false;
    if (arg->kind == TyKind::Int) return true;
    if (arg->kind == TyKind::Enum || arg->kind == TyKind::Struct) {
        return arg->name == param->name;
    }
    return true;
}

const FunctionDef* Lower::pick_def(const std::string& name, const CallExpr& node) {
    /* 同名重载：参数类型对得上的本文件 def。 */
    auto it = defs_.find(name);
    if (it == defs_.end()) return nullptr;
    for (const FunctionDef* d : it->second) {
        if (has_type_params(d->params)) {
            /* 泛型候选：先定实例化（类型实参 / 推型），再只比**运行期**形参。定不下来（个数对
             * 不上、推不出类型）就跳过这个候选 —— 不能把异常泄出去，否则同一名字的其它重载
             * 都再也选不到（新增的这条路径只包住泛型候选，非泛型候选行为不变）。 */
            Inst inst;
            try {
                inst = resolve_inst(d->params, node, d->lo);
            } catch (const Unsupported&) {
                continue;
            }
            bool ok = true;
            for (size_t i = 0; i < inst.rt_types.size(); ++i) {
                if (!compatible(static_type(*node.args[inst.rt_arg[i]]), inst.rt_types[i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return d;
            continue;
        }
        auto types = decl_param_types(*d);
        if (!types || types->size() != node.args.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < types->size(); ++i) {
            if (!compatible(static_type(*node.args[i]), (*types)[i])) {
                ok = false;
                break;
            }
        }
        if (ok) return d;
    }
    return nullptr;
}

/* ---------------------------------------------------------------- 泛型实例化 */

bool Lower::is_type_param(const TypeRef& ref) {
    return ref.at && (ref.name == "Type" || ref.name == "Size" || ref.name == "Any");
}

bool Lower::has_type_params(const std::vector<std::unique_ptr<Param>>& params) {
    for (const auto& p : params) {
        if (p->typ != nullptr && is_type_param(*p->typ)) return true;
    }
    return false;
}

Lower::Inst Lower::resolve_inst(const std::vector<std::unique_ptr<Param>>& params,
                                const CallExpr& node, int line) {
    Inst out;
    std::vector<size_t> tp;   /* 类型参数的声明位置（按声明序） */
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i]->typ != nullptr && is_type_param(*params[i]->typ)) tp.push_back(i);
    }
    for (size_t i = 0; i < params.size(); ++i) out.rt_params.push_back(params[i].get());
    if (tp.empty()) {
        if (node.args.size() != params.size() || !node.type_args.empty()) {
            throw Unsupported("实参个数对不上", line);
        }
        for (size_t i = 0; i < params.size(); ++i) {
            if (params[i]->typ == nullptr) throw Unsupported("参数 " + params[i]->name + " 缺类型", line);
            out.rt_types.push_back(ty_of_ref(*params[i]->typ, line));
            out.rt_arg.push_back(i);
        }
        return out;
    }

    /* 写法一（类型实参，不占运行期槽）：`g(a, <U8>)` —— 实参个数 = 声明参数 - 类型参数个数。
     * 写法二（值实参，推型）：`g(load(<U8>, p))` —— 实参个数 = 声明参数个数。 */
    const bool by_value = node.args.size() == params.size();
    if (!by_value && node.args.size() + tp.size() != params.size()) {
        throw Unsupported("泛型实参个数对不上", line);
    }
    if (node.type_args.size() > tp.size()) throw Unsupported("类型实参个数超过类型参数", line);
    if (tp.size() > 1 && !node.type_args.empty() && node.type_args.size() != tp.size()) {
        throw Unsupported("类型实参个数少于类型参数", line);
    }

    size_t ti = 0;   /* 下一个待定类型的类型参数（声明序） */
    for (size_t i = 0; i < params.size(); ++i) {
        const bool is_tp = params[i]->typ != nullptr && is_type_param(*params[i]->typ);
        if (!is_tp) {
            if (params[i]->typ == nullptr) throw Unsupported("参数 " + params[i]->name + " 缺类型", line);
            out.rt_types.push_back(ty_of_ref(*params[i]->typ, line));
            out.rt_arg.push_back(by_value ? i : i - (ti > tp.size() ? tp.size() : ti));
            continue;
        }
        TyRef bound = nullptr;
        if (ti < node.type_args.size()) {
            bound = ty_of_ref(*node.type_args[ti], line);
        } else if (by_value) {
            bound = static_type(*node.args[i]);
            if (!bound) throw Unsupported("推不出类型 " + params[i]->name, line);
        } else {
            throw Unsupported("类型实参没给 " + params[i]->name, line);
        }
        out.bind_order.push_back(bound);
        out.binds[params[i]->name] = bound;
        ++ti;
        if (by_value) {
            out.rt_types.push_back(bound);
            out.rt_arg.push_back(i);
        }
    }
    /* 同一 (函数, 实参) 只实例化一次的判据就是这个串（接口 §1）：缓存键与函数名的 `$` 后缀共用。 */
    for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) out.sig += ",";
        if (params[i]->typ != nullptr && is_type_param(*params[i]->typ)) {
            out.sig += "@" + ty_key(*out.binds.at(params[i]->name));
        } else {
            out.sig += ty_key(*ty_of_ref(*params[i]->typ, line));
        }
    }
    if (by_value) out.sig += "#v";
    return out;
}

TyRef Lower::inst_ret_type(const TypeRef* ret, const Inst& inst, int line) {
    if (ret == nullptr) return ty_void();
    if (!is_type_param(*ret)) return ty_of_ref(*ret, line);
    /* 返回类型写成类型参数（`@Size` / `@Type`）时取**第一个**类型参数的绑定。实测语料里只有
     * 一个类型参数（`get_ram_value(…, size: @Size) @Size`），多类型参数的形状未验。 */
    if (inst.bind_order.empty()) throw Unsupported("返回类型 " + ret->name + " 没有绑定", line);
    return inst.bind_order.front();
}

TyRef Lower::type_value_of(const Expr& node, int line) {
    /* 类型值只出现在类型实参位置，语料里唯一形态是 `load(type_of(x), p)`。 */
    const auto* call = as<CallExpr>(&node);
    if (call == nullptr) throw Unsupported("类型值形态", line);
    const auto* id = as<IdentExpr>(call->callee.get());
    if (id == nullptr || id->name != "type_of" || call->args.size() != 1) {
        throw Unsupported("类型值形态", line);
    }
    return operand_type(*call->args[0], line);
}

TyRef Lower::operand_type(const Expr& node, int line) {
    /* 类型参数不占运行期槽（`g(a, <U8>)` 那种写法），所以它在作用域里**没有**变量绑定 ——
     * 名字 → 具体类型只能从本实例的类型环境拿。 */
    const auto* id = as<IdentExpr>(&node);
    if (id != nullptr) {
        auto it = cur_fn().ty_binds.find(id->name);
        if (it != cur_fn().ty_binds.end()) return it->second;
    }
    TyRef ty = static_type(node);
    if (!ty) throw Unsupported("拿不到类型", line);
    return ty;
}

V Lower::builtin_size(const CallExpr& node) {
    if (node.args.size() != 1) throw Unsupported("size(x) 的形态", node.src_line);
    return V::konst(ty_i64(), uint64_t(size_bits(*operand_type(*node.args[0], node.src_line))));
}

TyRef Lower::defstmt_ret(const DefStmt& st) {
    return st.ret != nullptr ? ty_of_ref(*st.ret, st.src_line) : ty_void();
}

std::vector<TyRef> Lower::defstmt_params(const DefStmt& st) {
    std::vector<TyRef> out;
    for (const auto& p : st.params) {
        if (p->typ == nullptr) throw Unsupported("参数 " + p->name + " 缺类型", p->src_line);
        out.push_back(ty_of_ref(*p->typ, p->src_line));
    }
    return out;
}

std::optional<std::vector<TyRef>> Lower::decl_param_types(const FunctionDef& d) {
    std::vector<TyRef> out;
    for (const auto& p : d.params) {
        if (p->typ == nullptr) return std::nullopt;   /* 重载解析里这会跳过这个候选 */
        if (is_type_param(*p->typ)) return std::nullopt;   /* 泛型参数的实例化走 `resolve_inst` */
        out.push_back(ty_of_ref(*p->typ, d.lo));
    }
    return out;
}

V Lower::call_named(const std::string& name, const CallExpr& node) {
    if (const FunctionDef* d = pick_def(name, node)) return call_user(*d, node);
    if (const Binding* b = lookup(name)) {
        if (b->kind == Binding::Kind::Def) return call_nested(*b, node);
    }
    /* 程序根语句树里的嵌套 def 与顶层 def 一样**整单元可见、且前向可见**：006/058 的
     * `if in_scope("get_input") { … get_input(…) }` 的调用点在顶层 def `mode_run` 里，
     * 而声明在程序根的 `run_sim` 体里（U-L14 §2 的 T8/U1：两种位置的引用点都判真）。 */
    auto rd = root_defs_.find(name);
    if (rd != root_defs_.end()) return call_nested(Binding::nested(rd->second), node);
    if (name == "load") return builtin_load(node);
    if (name == "store") return builtin_store(node);
    if (name == "raw_cast") return builtin_raw_cast(node);
    if (name == "asr") return builtin_asr(node);
    if (name == "clz") return builtin_clz(node);
    if (name == "ror") return builtin_ror(node);
    if (name == "min" || name == "max") return builtin_minmax(name, node);
    if (name == "array") return builtin_array(node);
    if (name == "random") return builtin_random(node);
    if (name == "size") return builtin_size(node);
    if (name == "in_scope" || name == "type_of") {
        throw Unsupported(name + "() 只支持编译期形态", node.src_line);
    }
    if (is_prefix_func(name)) return call_prefix(name, node);
    if (defs_.count(name) != 0) {
        throw Unsupported(name + "() 的重载解析拿不到参数类型", node.src_line);
    }
    throw Unsupported("未定义的函数 " + name, node.src_line);
}

LLVMValueRef Lower::emit_call(LLVMValueRef fnref, const std::vector<LLVMValueRef>& args) {
    std::vector<LLVMValueRef> a = args;
    return LLVMBuildCall2(cur_fn().bd, LLVMGlobalGetValueType(fnref), fnref, a.data(),
                          static_cast<unsigned>(a.size()), "");
}

V Lower::call_user(const FunctionDef& d, const CallExpr& node) {
    Inst inst = resolve_inst(d.params, node, d.lo);
    TyRef ret_ty = inst_ret_type(d.ret.get(), inst, d.lo);
    /* 捕获**在调用点**算（原版编译器也是在调用点解析自由名），声明参数只是它的一部分。 */
    Closure cl = def_captures(d);
    LLVMValueRef fnref = ensure_def_fn(d.name, d.lo, d, ret_ty, inst, cl);
    std::vector<LLVMValueRef> vals;
    for (size_t i = 0; i < inst.rt_types.size(); ++i) {
        V conv = convert(e(*node.args[inst.rt_arg[i]]), inst.rt_types[i], node.src_line);
        vals.push_back(inst.rt_types[i]->kind == TyKind::Struct ? conv.ll : llv(conv));
    }
    for (const auto& c : cl.caps) vals.push_back(c.ptr);
    LLVMValueRef ll = emit_call(fnref, vals);
    if (ret_ty->kind == TyKind::Void) return V::instr(nullptr, ty_void());
    return V::instr(ll, ret_ty);
}

V Lower::call_nested(const Binding& binding, const CallExpr& node) {
    const DefStmt& st = *binding.def;
    Closure cl = captures_of(st);
    TyRef ret_ty = defstmt_ret(st);
    std::vector<TyRef> param_tys = defstmt_params(st);
    if (param_tys.size() != node.args.size()) {
        throw Unsupported(st.name + "() 的参数个数对不上", node.src_line);
    }
    LLVMValueRef fnref = ensure_nested_fn(st, ret_ty, param_tys, cl);
    std::vector<LLVMValueRef> vals;
    for (size_t i = 0; i < node.args.size(); ++i) {
        vals.push_back(llv(convert(e(*node.args[i]), param_tys[i], node.src_line)));
    }
    for (const auto& c : cl.caps) vals.push_back(c.ptr);
    LLVMValueRef ll = emit_call(fnref, vals);
    if (ret_ty->kind == TyKind::Void) return V::instr(nullptr, ty_void());
    return V::instr(ll, ret_ty);
}

V Lower::builtin_load(const CallExpr& node) {
    /* 两种形态：`load(<T>, p)` 与 `load(type_of(x), p)`（后者 `type_args` 是空的、实参是 2 个）。 */
    if (!node.type_args.empty() && node.args.size() == 1) {
        TyRef ty = ty_of_ref(*node.type_args[0], node.src_line);
        return load_addr(to_ptr(e(*node.args[0])), ty);
    }
    if (node.type_args.empty() && node.args.size() == 2) {
        TyRef ty = type_value_of(*node.args[0], node.src_line);
        return load_addr(to_ptr(e(*node.args[1])), ty);
    }
    throw Unsupported("load(<T>, p) 的形态", node.src_line);
}

V Lower::builtin_store(const CallExpr& node) {
    if (node.args.size() != 2) throw Unsupported("store(p, v) 的形态", node.src_line);
    LLVMValueRef addr = to_ptr(e(*node.args[0]));
    V val = e(*node.args[1]);
    if (val.ty->kind == TyKind::Void) throw Unsupported("store 的值是 None", node.src_line);
    store_addr(addr, val, val.ty, node.src_line);
    return V::instr(nullptr, ty_void());
}

V Lower::builtin_raw_cast(const CallExpr& node) {
    if (node.type_args.empty() || node.args.size() != 1) {
        throw Unsupported("raw_cast(<T>, v) 的形态", node.src_line);
    }
    TyRef ty = ty_of_ref(*node.type_args[0], node.src_line);
    V v = e(*node.args[0]);
    auto c = pat_const(v);
    if (c) return V::konst(ty, *c & kMask64);
    return V::instr(llv(v), ty);
}

V Lower::builtin_asr(const CallExpr& node) {
    /* `asr`：按 W 位符号位算术右移，与 `>>` 不同（U-L8 指示 3）：n ≥ W 时**饱和**。 */
    if (node.args.size() != 2) throw Unsupported("asr(x, n) 的形态", node.src_line);
    V x = e(*node.args[0]);
    V n = e(*node.args[1]);
    if (!is_int(*x.ty)) {
        throw Unsupported(std::string("asr 用在 ") + ty_kind_name(x.ty->kind), node.src_line);
    }
    const int bits = x.ty->bits;
    auto cx = pat_const(x);
    auto cn = pat_const(n);
    if (cx && cn) {
        const int64_t amt = bits == 64 ? static_cast<int64_t>(*cn & 63)
                                       : static_cast<int64_t>(std::min<uint64_t>(*cn, uint64_t(bits - 1)));
        const uint64_t r = static_cast<uint64_t>(as_signed(*cx, bits) >> amt) & mask_of(bits);
        return V::konst(x.ty, r);
    }
    LLVMValueRef amt = llv(convert(n, x.ty, node.src_line));
    if (bits == 64) {
        amt = LLVMBuildAnd(cur_fn().bd, amt, const_int(63), "amt");
    } else {
        amt = LLVMBuildSelect(
            cur_fn().bd,
            LLVMBuildICmp(cur_fn().bd, LLVMIntULT, amt, const_int(uint64_t(bits)), "lt"), amt,
            const_int(uint64_t(bits - 1)), "sat");
    }
    LLVMValueRef ll = LLVMBuildAShr(cur_fn().bd, sext64(llv(x), bits), amt, "asr");
    return V::instr(mask_ll(ll, bits), x.ty);
}

/* `llvm.ctlz.i64` 的声明。i1 那个参数是 `is_zero_undef`，传 false：clz(0) 给 64。 */
LLVMValueRef Lower::ensure_clz_fn() {
    auto hit = helpers_.find("ctlz");
    if (hit != helpers_.end()) return hit->second;
    LLVMTypeRef params[2] = {i64_, LLVMInt1TypeInContext(ctx())};
    LLVMValueRef fn =
        LLVMAddFunction(module(), "llvm.ctlz.i64", LLVMFunctionType(i64_, params, 2, 0));
    helpers_["ctlz"] = fn;
    return fn;
}

V Lower::builtin_clz(const CallExpr& node) {
    /* 数前导零的范围是**操作数声明的位宽**，不是 64：137 的注释把 `Int` 操作数钉在 64
     * （`clz(input.input)` 的 `input.input` 是 U8 → 数 8 位里的前导零）。所以用 i64 的 ctlz
     * 再减去「64 - 位宽」——不必为每种位宽各声明一条 intrinsic。 */
    if (node.args.size() != 1) throw Unsupported("clz(x) 的形态", node.src_line);
    V x = e(*node.args[0]);
    if (!is_int(*x.ty)) {
        throw Unsupported(std::string("clz 用在 ") + ty_kind_name(x.ty->kind), node.src_line);
    }
    const int bits = x.ty->bits;
    LLVMValueRef v = mask_ll(llv(x), bits);
    /* 第二个参数是 i1（`const_int` 给的是 i64，直接传会撞「参数类型不匹配」）。 */
    LLVMValueRef zero = LLVMConstInt(LLVMInt1TypeInContext(ctx()), 0, 0);
    LLVMValueRef call = emit_call(ensure_clz_fn(), {v, zero});
    if (bits == 64) return V::instr(call, x.ty);
    return V::instr(LLVMBuildSub(cur_fn().bd, call, const_int(uint64_t(64 - bits)), "clz"), x.ty);
}

V Lower::builtin_ror(const CallExpr& node) {
    /* `ror` 在参考里**两条路不一致**（实测：`work/tmp/fixsmall/ror_probe.py` 读 ui_buffer 里
     * 参考渲染出的字符串，`ror_cmp.py` 再与我们的产物逐值对照）：
     *
     * · 两个操作数都是常量时参考在**编译期折叠**，折的是「按源类型符号性扩成 64 位的值与
     *   旋转量按 64 取模」的 64 位旋转：`ror(U8 0x81, 1)` 给 64（按 W=8 旋转会给 192）、
     *   `ror(U32 1, 32)` 给 0、`ror(S32 1, 1)` 给 0。
     * · 有一个是运行期值时，参考按**操作数位宽**算：旋转量按 W 取模，再做
     *   `(x >> n) | (x << (W - n))` 截回 W 位（W=32 时就是普通的 32 位循环右移，
     *   `ror(U32 0x80000001, 1)` 给 0xC0000000，与折叠那条路的 0x40000000 不同）。
     *
     * 只写一条路就会与参考分叉，所以两条路各按实测来。旋转量只看低位，不必像 `asr` 那样
     * 先转成操作数的类型。 */
    if (node.args.size() != 2) throw Unsupported("ror(x, n) 的形态", node.src_line);
    V x = e(*node.args[0]);
    V n = e(*node.args[1]);
    if (!is_int(*x.ty)) {
        throw Unsupported(std::string("ror 用在 ") + ty_kind_name(x.ty->kind), node.src_line);
    }
    if (!is_int(*n.ty)) {
        throw Unsupported(std::string("ror 的旋转量是 ") + ty_kind_name(n.ty->kind),
                          node.src_line);
    }
    const int bits = x.ty->bits;
    auto cx = pat_const(x);
    auto cn = pat_const(n);
    if (cx && cn) {
        /* `pat_const` 给的就 是按源类型符号性扩展后的 64 位内容，正好是折叠那一路的操作数。 */
        const uint64_t amt = *cn & 63;
        const uint64_t r = (*cx >> amt) | (*cx << ((64 - amt) & 63));
        return V::konst(x.ty, r & mask_of(bits));
    }
    const uint64_t sm = uint64_t(bits - 1);
    LLVMValueRef v = mask_ll(llv(x), bits);
    LLVMValueRef amt = LLVMBuildAnd(cur_fn().bd, llv(n), const_int(sm), "amt");
    LLVMValueRef inv = LLVMBuildAnd(
        cur_fn().bd, LLVMBuildSub(cur_fn().bd, const_int(uint64_t(bits)), amt, "amtc"), const_int(sm), "amti");
    LLVMValueRef lo = LLVMBuildLShr(cur_fn().bd, v, amt, "rr");
    LLVMValueRef hi = LLVMBuildShl(cur_fn().bd, v, inv, "rl");
    return V::instr(mask_ll(LLVMBuildOr(cur_fn().bd, lo, hi, "ror"), bits), x.ty);
}

V Lower::builtin_minmax(const std::string& name, const CallExpr& node) {
    if (node.args.size() != 2) throw Unsupported(name + "(a, b) 的形态", node.src_line);
    TyRef ta = static_type(*node.args[0]);
    TyRef tb = static_type(*node.args[1]);
    TyRef ty = unify(ta ? ta : ty_i64(), tb ? tb : ty_i64(), node.src_line);
    V a = convert(e(*node.args[0]), ty, node.src_line);
    V b = convert(e(*node.args[1]), ty, node.src_line);
    auto ca = pat_const(a);
    auto cb = pat_const(b);
    if (ca && cb) {
        const int64_t x = ty->is_signed ? as_signed(*ca, ty->bits) : static_cast<int64_t>(*ca);
        const int64_t y = ty->is_signed ? as_signed(*cb, ty->bits) : static_cast<int64_t>(*cb);
        const int64_t pick = name == "min" ? std::min(x, y) : std::max(x, y);
        return V::konst(ty, static_cast<uint64_t>(pick) & mask_of(ty->bits));
    }
    LLVMIntPredicate pred = LLVMIntEQ;
    if (name == "min") pred = ty->is_signed ? LLVMIntSLT : LLVMIntULT;
    else pred = ty->is_signed ? LLVMIntSGT : LLVMIntUGT;
    LLVMValueRef x = llv(a);
    LLVMValueRef y = llv(b);
    if (ty->is_signed && ty->bits < 64) {
        x = sext64(x, ty->bits);
        y = sext64(y, ty->bits);
    }
    LLVMValueRef cnd = LLVMBuildICmp(cur_fn().bd, pred, x, y, "mm");
    LLVMValueRef ll = LLVMBuildSelect(cur_fn().bd, cnd, llv(a), llv(b), "mmv");
    return V::instr(mask_ll(ll, ty->bits), ty);
}

V Lower::builtin_array(const CallExpr& node) {
    /* `array(<T>, n)`：参考内建（前缀与 98 份语料里都没有 `def array`），返回定长数组。
     * 语义实测（参考编译器 + 差分宿主，片段/原始输出见 `work/tmp/arraytask/` 与
     * `docs/notes/2026-10-18-array.md`）：
     *   · 值是**指向载荷的指针**，载荷前 8 字节是 Nim 头，`头 >> 16 == n`（与 `.len()`
     *     的读法一致，`len_of` 已支持 Array）；
     *   · 载荷**零初始化**，元素按 U-L1 的字节步长紧密排列（`<U8>` → 1、`<Int>` → 8）；
     *   · 每个调用点得到**自己的**缓冲区（参考在堆上分配，首址固定）。
     * 我们把它做成**每个调用点一枚私有全局**（头 + 零载荷）。语料里 `array()` 都在
     * 「建表」函数里（每次运行只调一次），所以加载期的零初始化等价于参考的每次分配零初始化。 */
    if (node.type_args.size() != 1 || node.args.size() != 1) {
        throw Unsupported("array(<T>, n) 的形态", node.src_line);
    }
    TyRef elem = ty_of_ref(*node.type_args[0], node.src_line);
    const uint64_t count = need_const(cv(*node.args[0]), node.src_line);
    if (count > (uint64_t{1} << 32)) {
        throw Unsupported("array 的长度 " + std::to_string(count) + " 太大", node.src_line);
    }
    const uint64_t payload = count * static_cast<uint64_t>(storage_bytes(*elem));
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    const uint64_t header = count << 16;
    std::vector<LLVMValueRef> elems;
    for (int i = 0; i < 8; ++i) {
        elems.push_back(LLVMConstInt(i8, (header >> (8 * i)) & 0xFF, 0));
    }
    elems.resize(static_cast<size_t>(8 + payload), LLVMConstInt(i8, 0, 0));
    LLVMTypeRef arrty = LLVMArrayType2(i8, elems.size());
    const std::string gname =
        ".array" + std::to_string(node.src_line) + "_" + std::to_string(array_serial_++);
    LLVMValueRef g = LLVMAddGlobal(module(), arrty, gname.c_str());
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetInitializer(g, LLVMConstArray2(i8, elems.data(), elems.size()));
    std::vector<LLVMValueRef> gi{const_int(0), const_int(8)};
    return V::instr(LLVMConstGEP2(arrty, g, gi.data(), 2), make_array(elem));
}

V Lower::empty_array(const TyRef& arr_ty, int line) {
    /* `Arr[U8] []`：**空**数组字面量自己不带元素类型（`ArrayLitExpr` 只存 items），类型只在外面
     * 那层转换上 —— 所以只能从 `e_CastExpr` / `cv` 的转换处进来。表示与我们 `array(<T>, 0)`
     * 同一套：8 字节零头（`len = 头 >> 16 = 0`）+ 空载荷，也就是一个指向它的私有常量全局。
     * 空数组写不进去，所以不需要按调用点分开（参考侧那个值本身是它的 arena 分配结果，
     * 只有「长度为 0」这一条被消费者用）。 */
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    std::vector<LLVMValueRef> elems(8, LLVMConstInt(i8, 0, 0));
    LLVMTypeRef arrty = LLVMArrayType2(i8, elems.size());
    const std::string gname =
        ".empty" + std::to_string(line) + "_" + std::to_string(array_serial_++);
    LLVMValueRef g = LLVMAddGlobal(module(), arrty, gname.c_str());
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(g, 1);
    LLVMSetInitializer(g, LLVMConstArray2(i8, elems.data(), elems.size()));
    std::vector<LLVMValueRef> gi{const_int(0), const_int(8)};
    return V::instr(LLVMConstGEP2(arrty, g, gi.data(), 2), arr_ty);
}

std::pair<LLVMValueRef, TyRef> Lower::seed_global(int line) {
    auto it = globals_.find("global_seed");
    if (it != globals_.end()) return it->second;
    return prefix_global("global_seed", line);
}

V Lower::builtin_random(const CallExpr& node) {
    /* `random` 定义在前缀里（`simplexc/corpus/stdlib_prefix.si`:1464–1495），98 份语料里 32 份在用，
     * 但它只要「一个 8 字节全局 + 4 行算术」——不必把前缀整体搬进来。
     * 实测（器材 `work/tmp/randomtask/probe_seq.py`：参考产物 + 差分宿主，8 次**连续** `random()`
     * 落到 `ui_buffer+20000…+20056`，8/8 逐位相同）：
     *   · 三步 xorshift 是**顺序赋值**：`x ^= x<<12; x ^= x>>25; x ^= x>>27`，再乘
     *     `0x2545F4914F6CDD1`。常数只有 **15 位**（前缀原样），别拿教科书的 16 位那个替换；
     *   · 返回**新** state 的全 64 位；`random(n) = U64(state) % U64(n)`，即 `[0, n)`；
     *   · 种子就是前缀的 `var global_seed = Seed 1`：语料自己会写 `global_seed = Seed (…)`
     *     （98 份里 98 份），所以两边必须是**同一份**全局（`seed_global`），且不许每 tick 重置。 */
    if (node.args.size() > 1) throw Unsupported("random 的参数个数", node.src_line);
    auto sg = seed_global(node.src_line);
    const TyRef& ty = sg.second;
    LLVMValueRef old = llv(load_addr(sg.first, ty));
    LLVMValueRef x = LLVMBuildXor(cur_fn().bd, old, LLVMBuildShl(cur_fn().bd, old, const_int(12), "l"), "x1");
    x = LLVMBuildXor(cur_fn().bd, x, LLVMBuildLShr(cur_fn().bd, x, const_int(25), "s"), "x2");
    x = LLVMBuildXor(cur_fn().bd, x, LLVMBuildLShr(cur_fn().bd, x, const_int(27), "s"), "x3");
    LLVMValueRef next = LLVMBuildMul(cur_fn().bd, x, const_int(kSeedMul), "seed");
    store_addr(sg.first, V::instr(next, ty), ty, node.src_line);
    V out = V::instr(next, ty);
    if (node.args.empty()) return out;
    /* `%` 口径照抄 `div`（零除数不 trap，余数 = 被除数）：`random(0)` 返回被除数。 */
    V m = convert(e(*node.args[0]), ty, node.src_line);
    return V::instr(div("%", llv(out), llv(m), *ty), ty);
}

V Lower::call_method(const MemberExpr& callee, const CallExpr& node) {
    V obj = e(*callee.obj);
    if (callee.name == "len") {
        if (obj.ty->kind != TyKind::Str && obj.ty->kind != TyKind::Array) {
            throw Unsupported(".len() 用在 " + std::string(ty_kind_name(obj.ty->kind)),
                              node.src_line);
        }
        return V::instr(len_of(obj, node.src_line), ty_i64());
    }
    if (callee.name == "ms") {
        if (!is_int(*obj.ty)) {
            throw Unsupported(".ms() 用在 " + std::string(ty_kind_name(obj.ty->kind)),
                              node.src_line);
        }
        auto c = pat_const(obj);
        if (c) return V::konst(ty_time(), (*c * kNsPerMs) & kMask64);
        return V::instr(LLVMBuildMul(cur_fn().bd, llv(obj), const_int(kNsPerMs), "ns"), ty_time());
    }
    throw Unsupported("方法 ." + callee.name + "()", node.src_line);
}

V Lower::call_prefix(const std::string& name, const CallExpr& node) {
    if (name == "thread_exit") {
        LLVMValueRef fn = get_extern("thread_exit", ty_void(), {});
        emit_call(fn, {});
        return V::instr(nullptr, ty_void());
    }
    if (name == "get_time") {
        LLVMValueRef fn = ensure_prefix_fn("get_time", ty_time(), {});
        return V::instr(emit_call(fn, {}), ty_time());
    }
    if (name == "sleep") {
        if (node.args.size() != 1) throw Unsupported("sleep(t) 的形态", node.src_line);
        LLVMValueRef fn = ensure_prefix_fn("sleep", ty_void(), {ty_time()});
        emit_call(fn, {llv(convert(e(*node.args[0]), ty_time(), node.src_line))});
        return V::instr(nullptr, ty_void());
    }
    if (name == "memory_clear") {
        if (node.args.size() != 2) throw Unsupported("memory_clear(p, n) 的形态", node.src_line);
        LLVMValueRef fn = ensure_prefix_fn("memory_clear", ty_void(), {ty_ptr(), ty_i64()});
        emit_call(fn, {llv(e(*node.args[0])),
                       llv(convert(e(*node.args[1]), ty_i64(), node.src_line))});
        return V::instr(nullptr, ty_void());
    }
    if (name == "memory_copy") {
        if (node.args.size() != 3) throw Unsupported("memory_copy(src, dst, n) 的形态", node.src_line);
        LLVMValueRef fn =
            ensure_prefix_fn("memory_copy", ty_void(), {ty_ptr(), ty_ptr(), ty_i64()});
        emit_call(fn, {llv(e(*node.args[0])), llv(e(*node.args[1])),
                       llv(convert(e(*node.args[2]), ty_i64(), node.src_line))});
        return V::instr(nullptr, ty_void());
    }
    throw Unsupported("前缀函数 " + name, node.src_line);
}

LLVMValueRef Lower::get_extern(const std::string& name, const TyRef& ret_ty,
                               const std::vector<TyRef>& param_tys) {
    /* 只声明、不定义的外部符号（装配器给它造桩）。签名按**目标 ABI** 给：`Ptr` 参数就是真指针
     * （我们内部函数才用 i64 传地址）。 */
    auto key = std::make_tuple(static_cast<int>(4), name, int64_t(0), std::string());
    auto hit = fns_.find(key);
    if (hit != fns_.end()) return hit->second.fnref;
    std::vector<LLVMTypeRef> ptypes;
    for (const auto& t : param_tys) {
        ptypes.push_back(t->kind == TyKind::Ptr ? ptr_ : ll_type(*t));
    }
    LLVMTypeRef rty = ret_ty->kind == TyKind::Void ? void_ty_ : ll_type(*ret_ty);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), name.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    fns_[key] = Fn{fnref, ret_ty, param_tys, {}};
    return fnref;
}

void Lower::body_get_time() {
    LLVMValueRef slot = alloca_slot(make_int(64, false), "ft");
    LLVMValueRef ext = get_extern("GetSystemTimePreciseAsFileTime", ty_void(), {ty_ptr()});
    emit_call(ext, {slot});
    V ft = load_addr(slot, make_int(64, false));
    LLVMValueRef d = LLVMBuildSub(cur_fn().bd, llv(ft), const_int(kFileTimeUnix), "d");
    LLVMBuildRet(cur_fn().bd, LLVMBuildMul(cur_fn().bd, d, const_int(100), "ns"));
}

void Lower::body_sleep() {
    LLVMValueRef ms =
        LLVMBuildUDiv(cur_fn().bd, LLVMGetParam(cur_fn().fnref, 0), const_int(kNsPerMs), "ms");
    LLVMValueRef ext = get_extern("Sleep", ty_void(), {make_int(32, false)});
    emit_call(ext, {LLVMBuildTrunc(cur_fn().bd, ms, storage_ll(*make_int(32, false)), "m")});
    LLVMBuildRetVoid(cur_fn().bd);
}

LLVMValueRef Lower::byte_loop_prologue(unsigned n_param, LLVMBasicBlockRef* head_out,
                                    LLVMValueRef* n_out) {
    LLVMValueRef slot = alloca_slot(ty_i64(), "i");
    V zero;
    zero.ll = const_int(0);
    zero.ty = ty_i64();
    zero.c = 0;
    store_addr(slot, zero, ty_i64(), 0);
    *head_out = br_new_block("lp");
    *n_out = LLVMGetParam(cur_fn().fnref, n_param);
    return slot;
}

void Lower::body_memory_clear() {
    LLVMValueRef addr = LLVMBuildIntToPtr(cur_fn().bd, LLVMGetParam(cur_fn().fnref, 0), ptr_, "ap");
    LLVMBasicBlockRef head = nullptr;
    LLVMValueRef n = nullptr;
    LLVMValueRef slot = byte_loop_prologue(1, &head, &n);
    V i = load_addr(slot, ty_i64());
    LLVMValueRef cnd = LLVMBuildICmp(cur_fn().bd, LLVMIntULT, llv(i), n, "lt");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "lpb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "lpd");
    LLVMBuildCondBr(cur_fn().bd, cnd, body, done);
    LLVMPositionBuilderAtEnd(cur_fn().bd, body);
    cur_fn().block = body;
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    std::vector<LLVMValueRef> gi{llv(i)};
    LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, i8, addr, gi.data(), 1, "p");
    LLVMValueRef st = LLVMBuildStore(cur_fn().bd, LLVMConstInt(i8, 0, 0), p);
    LLVMSetAlignment(st, 1);
    LLVMSetVolatile(st, 1);
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, llv(i), const_int(1), "i1"), ty_i64()), ty_i64(),
               0);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    LLVMBuildRetVoid(cur_fn().bd);
}

void Lower::body_memory_copy() {
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMValueRef src = LLVMBuildIntToPtr(cur_fn().bd, LLVMGetParam(cur_fn().fnref, 0), ptr_, "sp0");
    LLVMValueRef dst = LLVMBuildIntToPtr(cur_fn().bd, LLVMGetParam(cur_fn().fnref, 1), ptr_, "dp0");
    LLVMBasicBlockRef head = nullptr;
    LLVMValueRef n = nullptr;
    LLVMValueRef slot = byte_loop_prologue(2, &head, &n);
    V i = load_addr(slot, ty_i64());
    LLVMValueRef cnd = LLVMBuildICmp(cur_fn().bd, LLVMIntULT, llv(i), n, "lt");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "lpb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "lpd");
    LLVMBuildCondBr(cur_fn().bd, cnd, body, done);
    LLVMPositionBuilderAtEnd(cur_fn().bd, body);
    cur_fn().block = body;
    std::vector<LLVMValueRef> gi{llv(i)};
    LLVMValueRef s = LLVMBuildGEP2(cur_fn().bd, i8, src, gi.data(), 1, "sp");
    LLVMValueRef d = LLVMBuildGEP2(cur_fn().bd, i8, dst, gi.data(), 1, "dp");
    LLVMValueRef ld = LLVMBuildLoad2(cur_fn().bd, i8, s, "b");
    LLVMSetAlignment(ld, 1);
    LLVMSetVolatile(ld, 1);
    LLVMValueRef st = LLVMBuildStore(cur_fn().bd, ld, d);
    LLVMSetAlignment(st, 1);
    LLVMSetVolatile(st, 1);
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, llv(i), const_int(1), "i1"), ty_i64()), ty_i64(),
               0);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    LLVMBuildRetVoid(cur_fn().bd);
}

LLVMValueRef Lower::ensure_prefix_fn(const std::string& name, const TyRef& ret_ty,
                                     const std::vector<TyRef>& param_tys) {
    auto key = std::make_tuple(static_cast<int>(3), name, int64_t(0), std::string());
    auto hit = fns_.find(key);
    if (hit != fns_.end()) return hit->second.fnref;
    std::vector<LLVMTypeRef> ptypes;
    for (const auto& t : param_tys) ptypes.push_back(val_ll(*t));
    LLVMTypeRef rty = ret_ty->kind == TyKind::Void ? void_ty_ : val_ll(*ret_ty);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), name.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    LLVMSetLinkage(fnref, LLVMInternalLinkage);
    fns_[key] = Fn{fnref, ret_ty, param_tys, {}};
    fn_seen_++;
    FnCtx& f = begin_function(fnref, ret_ty);
    f.name = name;
    if (name == "get_time") body_get_time();
    else if (name == "sleep") body_sleep();
    else if (name == "memory_clear") body_memory_clear();
    else if (name == "memory_copy") body_memory_copy();
    else throw Unsupported("前缀函数 " + name, 0);
    finish_function();
    return fnref;
}

LLVMValueRef Lower::ensure_def_fn(const std::string& name, int64_t lo, const FunctionDef& d,
                                  const TyRef& ret_ty, const Inst& inst,
                                  const Closure& closure) {
    const std::vector<Capture>& caps = closure.caps;
    const std::vector<TyRef>& param_tys = inst.rt_types;
    /* 捕获签名：空串 = 无捕获 —— 那种实例的键、函数名、参数表与闭包化之前**逐字节相同**
     * （参数只看**声明**参数，多出来的只是 `ptr` 捕获），所以没碰到 root 体本地的 def
     * 产物不变。 */
    std::string cap_sig;
    for (size_t j = 0; j < caps.size(); ++j) {
        if (j != 0) cap_sig += ',';
        cap_sig += caps[j].name;
        cap_sig += ':';
        cap_sig += ty_key(*caps[j].ty);
    }
    /* 键 = 捕获签名 + 实例化签名。非泛型 def 的 `inst.sig` 是空串 ⇒ 键与改前完全相同。 */
    std::string sig = cap_sig;
    if (!inst.sig.empty()) sig += "|" + inst.sig;
    auto key = std::make_tuple(static_cast<int>(1), name, lo, sig);
    auto hit = fns_.find(key);
    if (hit != fns_.end()) return hit->second.fnref;
    int& instances = cap_instances_[{name, lo}];
    /* 互调的顶层 def 按捕获签名各发一份实例，理论上签名集是有限的，但代价可能会长（名字集合
     * 的幂集）；超过这个数就报掉 —— 报错会回退到参考产物，比在这里无界发函数安全。 */
    if (!cap_sig.empty() && instances >= 64) {
        throw Unsupported(name + " 的闭包实例数不收敛（" + cap_sig + "）", d.lo);
    }
    std::vector<LLVMTypeRef> ptypes;
    for (const auto& t : param_tys) ptypes.push_back(val_ll(*t));
    for (size_t j = 0; j < caps.size(); ++j) ptypes.push_back(ptr_);
    LLVMTypeRef rty = ret_ty->kind == TyKind::Void ? void_ty_ : val_ll(*ret_ty);
    std::string gname =
        defs_.count(name) != 0 && defs_[name].size() == 1
            ? name
            : name + "#L" + std::to_string(lo);
    /* 接口 §1：实例化名 = `<函数名>$<实参规范化串>`。`$` 后绀只在泛型实例上有，所以非泛型的
     * 函数名不变。 */
    if (!inst.sig.empty()) gname += "$" + inst.sig;
    if (!cap_sig.empty()) gname += "$c" + std::to_string(instances++);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), gname.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    LLVMSetLinkage(fnref, LLVMInternalLinkage);
    fns_[key] = Fn{fnref, ret_ty, param_tys, caps};
    fn_seen_++;
    FnCtx& f = begin_function(fnref, ret_ty);
    /* 嵌套 def 的键与名字都靠上下文的名字分开（同名 def 的不同捕获实例里，同一个嵌套 def
     * 的捕获表不同，不能合并）；无捕获时与改前一致（就是 def 名）。 */
    f.name = sig.empty() ? name : gname;
    f.def = &d;
    f.ty_binds = inst.binds;
    for (size_t i = 0; i < param_tys.size(); ++i) {
        const std::string& pname = inst.rt_params[i]->name;
        const TyRef& ty = param_tys[i];
        LLVMValueRef a = alloca_slot(ty, pname);
        store_addr(a, V::instr(LLVMGetParam(fnref, static_cast<unsigned>(i)), ty), ty,
                   inst.rt_params[i]->src_line);
        bind_root_scope(pname, Binding::local(a, ty));
    }
    for (size_t j = 0; j < caps.size(); ++j) {
        bool is_param = false;
        for (const auto& p : d.params) is_param = is_param || p->name == caps[j].name;
        if (is_param) continue;
        /* 捕获只传**指针**（按引用），不复制内存 —— 赋值/读回都落在调用者那块栈槽上。 */
        bind_root_scope(
            caps[j].name,
            Binding::capture(LLVMGetParam(fnref, static_cast<unsigned>(param_tys.size() + j)),
                             caps[j].ty));
    }
    lower_block(*d.body);
    finish_function();
    return fnref;
}

LLVMValueRef Lower::ensure_nested_fn(const DefStmt& st, const TyRef& ret_ty,
                                     const std::vector<TyRef>& param_tys,
                                     const Closure& closure) {
    const std::vector<Capture>& caps = closure.caps;
    const std::string owner = cur_fn().def != nullptr ? cur_fn().name : std::string("closure");
    auto key = std::make_tuple(static_cast<int>(2), cur_fn().name, int64_t(st.src_line), st.name);
    auto hit = fns_.find(key);
    if (hit != fns_.end()) return hit->second.fnref;
    std::vector<LLVMTypeRef> ptypes;
    for (const auto& t : param_tys) ptypes.push_back(val_ll(*t));
    for (size_t i = 0; i < caps.size(); ++i) ptypes.push_back(ptr_);
    LLVMTypeRef rty = ret_ty->kind == TyKind::Void ? void_ty_ : val_ll(*ret_ty);
    const std::string gname = owner + "$" + st.name + "#L" + std::to_string(st.src_line);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), gname.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    LLVMSetLinkage(fnref, LLVMInternalLinkage);
    fns_[key] = Fn{fnref, ret_ty, param_tys, caps};
    fn_seen_++;
    FnCtx& f = begin_function(fnref, ret_ty);
    f.name = owner + "$" + st.name;
    /* 先绑外部嵌套 def 的名字，再绑参数与捕获：同名时**变量优先**（调用仍能按名查到 def ——
     * `call_named` 在捕获上找不到 Def 时会走 root_defs_）。 */
    for (const DefStmt* d : closure.defs) bind_root_scope(d->name, Binding::nested(d));
    for (size_t i = 0; i < st.params.size(); ++i) {
        if (i >= param_tys.size()) throw Unsupported(st.name + "() 的参数个数对不上", st.src_line);
        const TyRef& ty = param_tys[i];
        LLVMValueRef a = alloca_slot(ty, st.params[i]->name);
        store_addr(a, V::instr(LLVMGetParam(fnref, static_cast<unsigned>(i)), ty), ty,
                   st.params[i]->src_line);
        bind_root_scope(st.params[i]->name, Binding::local(a, ty));
    }
    for (size_t j = 0; j < caps.size(); ++j) {
        bool is_param = false;
        for (const auto& p : st.params) is_param = is_param || p->name == caps[j].name;
        if (is_param) continue;
        /* 捕获变量按**引用**绑到外层那块 alloca 上（指针由调用方传进来）。 */
        bind_root_scope(caps[j].name,
                        Binding::capture(LLVMGetParam(fnref, static_cast<unsigned>(param_tys.size() + j)),
                                         caps[j].ty));
    }
    lower_block(*st.body);
    finish_function();
    return fnref;
}

/* ================================================================ Lower：常量求值 */

std::optional<V> Lower::const_value(const Node& node) {
    try {
        return cv(node);
    } catch (const Unsupported&) {
        return std::nullopt;
    }
}

V Lower::cv(const Node& node) {
    switch (node.kind) {
        case NK::Literal: {
            const auto& n = static_cast<const LiteralExpr&>(node);
            if (n.lit_kind == "int") {
                return V::konst(ty_i64(), static_cast<uint64_t>(n.value) & kMask64);
            }
            if (n.lit_kind == "str" || n.lit_kind == "template") return string_literal(n);
            throw Unsupported("字面量 " + n.lit_kind, n.src_line);
        }
        case NK::Ident: {
            const auto& n = static_cast<const IdentExpr&>(node);
            if (n.name == "true" || n.name == "false") {
                return V::konst(ty_bool(), n.name == "true" ? 1 : 0);
            }
            /* 局部 `const` 只活在作用域里（`e_IdentExpr` 同样先查 lookup）——
               不查这里，函数内的 `const #N = #A | #B` 会被判成「初始化式不是常量」。 */
            if (const Binding* b = lookup(n.name)) {
                if (b->kind == Binding::Kind::Const) return *b->value;
            }
            auto cst = consts_.find(n.name);
            if (cst != consts_.end()) return cst->second;
            auto em = enum_member_.find(n.name);
            if (em != enum_member_.end()) {
                return V::konst(make_enum(em->second.first), uint64_t(em->second.second));
            }
            auto hc = hash_consts().find(n.name);
            if (hc != hash_consts().end()) return V::konst(hc->second.ty, hc->second.value);
            auto rc = root_const_decls_.find(n.name);
            if (rc != root_const_decls_.end()) return root_const_value(*rc->second);
            throw Unsupported("名字 " + n.name, n.src_line);
        }
        case NK::Cast: {
            const auto& n = static_cast<const CastExpr&>(node);
            TyRef target = ty_of_ref(*n.target, n.src_line);
            if (target->kind == TyKind::Array && n.expr->kind == NK::ArrayLit &&
                static_cast<const ArrayLitExpr&>(*n.expr).items.empty()) {
                return empty_array(target, n.src_line);
            }
            return convert(cv(*n.expr), target, n.src_line);
        }
        case NK::Unary: {
            const auto& n = static_cast<const UnaryExpr&>(node);
            V inner = cv(*n.expr);
            if (n.op == "+") return inner;
            if (!is_int(*inner.ty)) {
                throw Unsupported(std::string("一元 ") + n.op + " 用在 " +
                                      ty_kind_name(inner.ty->kind),
                                  n.src_line);
            }
            return V::konst(inner.ty, fold_unary(n.op, need_const(inner, n.src_line), *inner.ty));
        }
        case NK::Binary: {
            const auto& n = static_cast<const BinaryExpr&>(node);
            if (n.op == "&&" || n.op == "||") throw Unsupported("常量逻辑运算", n.src_line);
            TyRef ty = unify(cv(*n.left).ty, cv(*n.right).ty, n.src_line);
            V a = convert(cv(*n.left), ty, n.src_line);
            V b = convert(cv(*n.right), ty, n.src_line);
            if (cmp_ops().count(n.op) != 0) {
                return V::konst(ty_bool(),
                                fold_cmp(n.op, need_const(a, n.src_line),
                                         need_const(b, n.src_line), *ty));
            }
            if (!is_arith(n.op)) throw Unsupported("运算符 " + n.op, n.src_line);
            if (ty->kind == TyKind::Ptr) {
                if (n.op != "+" && n.op != "-") throw Unsupported("指针 " + n.op, n.src_line);
                auto ca = pat_const(a);
                auto cb = pat_const(b);
                if (!ca || !cb) throw Unsupported("指针算术的操作数不是常量", n.src_line);
                const uint64_t r = n.op == "+" ? *ca + *cb : *ca - *cb;
                return V::konst(ty_ptr(), r & kMask64);
            }
            return V::konst(ty, fold_arith(n.op, need_const(a, n.src_line),
                                           need_const(b, n.src_line), *ty));
        }
        case NK::Call: {
            const auto& n = static_cast<const CallExpr&>(node);
            const auto* id = as<IdentExpr>(n.callee.get());
            if (id != nullptr && id->name == "raw_cast" && !n.type_args.empty() &&
                n.args.size() == 1) {
                TyRef ty = ty_of_ref(*n.type_args[0], n.src_line);
                V v = cv(*n.args[0]);
                return V::konst(ty, pat_const(v).value_or(0) & kMask64);
            }
            throw Unsupported("常量求值遇到函数调用", n.src_line);
        }
        case NK::ArrayLit:
            return array_literal(static_cast<const ArrayLitExpr&>(node));
        case NK::CompositeLit:
            return composite_literal(static_cast<const CompositeLitExpr&>(node));
        default:
            throw Unsupported("常量求值遇到 " + std::string(kind_name(node.kind)), node.src_line);
    }
}

V Lower::string_literal(const LiteralExpr& node) {
    /* 字符串字面量 → Nim 布局的常量（`p[-8] = (len << 16) | cap_log2`，接口 §3）。U-L3/U-L10
     * 未定：只支持**不含反斜杠**的字面量（逃逸语义没查）；`cap_log2` 取 0（只有 `len` 那半被用到）；
     * 末尾照加一个 NUL。 */
    if (node.text.find('\\') != std::string::npos) {
        throw Unsupported("含转义的字符串字面量（U-L10 未定）", node.src_line);
    }
    auto hit = str_pool_.find(node.text);
    if (hit != str_pool_.end()) return hit->second;
    std::string raw;
    const uint64_t header = static_cast<uint64_t>(node.text.size()) << 16;
    for (int i = 0; i < 8; ++i) raw.push_back(static_cast<char>((header >> (8 * i)) & 0xFF));
    raw += node.text;
    raw.push_back('\0');
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    std::vector<LLVMValueRef> elems;
    elems.reserve(raw.size());
    for (char ch : raw) {
        elems.push_back(LLVMConstInt(i8, static_cast<unsigned char>(ch), 0));
    }
    LLVMTypeRef arrty = LLVMArrayType2(i8, elems.size());
    LLVMValueRef g = LLVMAddGlobal(module(), arrty, ".str");
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(g, 1);
    LLVMSetInitializer(g, LLVMConstArray2(i8, elems.data(), elems.size()));
    std::vector<LLVMValueRef> gi{const_int(0), const_int(8)};
    LLVMValueRef payload = LLVMConstGEP2(arrty, g, gi.data(), 2);
    V v = V::instr(payload, ty_str());
    str_pool_[node.text] = v;
    return v;
}

/* ================================================================ Lower：模板插值 / Str 拼接
 *
 * 渲染语义**全部实测**（参考编译器 + 差分宿主，见 docs/notes/2026-10-18-str-interp.md）：
 *   `Int` / `S*` → 有符号十进制（按声明位宽符号扩展）；`U*` → 无符号十进制；`Bool` → `true`/`false`；
 *   `Str` → 原样字节；`Ptr` → 地址的无符号十进制。
 * 形状：各插值段先求值**一次** → 按最坏尺寸在栈上开缓冲 → 顺序写入 → 按实际游标回填
 * `p[-8] = len << 16`（长度显式；不靠 NUL 终止 —— 消费方全是 `.len()` + `s[i]` + `set_text`）。
 * 十进制转换与拷贝都是**产物内联**的（`__simplexc_dec` / `__simplexc_copy`）：参考产物那边 `extern`
 * 也是空的，不许引出外部符号（`memcpy` 之类的 libcall 会变成未解析符号）。
 */

/* `len = u64(p-8) >> 16`（接口 §3）。`.len()` 与插值里的 Str 段共用这一处。 */
LLVMValueRef Lower::len_of(const V& obj, int line) {
    if (obj.ty->kind != TyKind::Str && obj.ty->kind != TyKind::Array) {
        throw Unsupported("长度用在 " + std::string(ty_kind_name(obj.ty->kind)), line);
    }
    std::vector<LLVMValueRef> gi{const_int(static_cast<uint64_t>(-8))};
    LLVMValueRef lp = LLVMBuildGEP2(cur_fn().bd, storage_ll(*make_int(8, false)), to_ptr(obj), gi.data(), 1,
                                    "lp");
    V raw = load_addr(lp, make_int(64, false));
    return LLVMBuildLShr(cur_fn().bd, llv(raw), const_int(16), "len");
}

/* 栈上一块带头的串缓冲：`buf[0..8)` 是头、payload = `buf+8`。 */
V Lower::str_concat(const V& a, const V& b, int line) {
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMValueRef la = len_of(a, line);
    LLVMValueRef lb = len_of(b, line);
    LLVMValueRef total = LLVMBuildAdd(cur_fn().bd, la, lb, "sl");
    LLVMValueRef buf = LLVMBuildArrayAlloca(cur_fn().bd, i8, LLVMBuildAdd(cur_fn().bd, total, const_int(8), "sb"),
                                            "sbuf");
    LLVMSetAlignment(buf, 8);
    std::vector<LLVMValueRef> g8{const_int(8)};
    LLVMValueRef payload = LLVMBuildGEP2(cur_fn().bd, i8, buf, g8.data(), 1, "sp");
    LLVMValueRef copy = ensure_copy_fn();
    LLVMValueRef cur = emit_call(copy, {payload, to_ptr(a), la});
    emit_call(copy, {cur, to_ptr(b), lb});
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef head = LLVMBuildGEP2(cur_fn().bd, i8, buf, g0.data(), 1, "shdr");
    store_addr(head, V::instr(LLVMBuildShl(cur_fn().bd, total, const_int(16), "shl"), ty_i64()), ty_i64(),
               line);
    return V::instr(payload, ty_str());
}

V Lower::template_expr(const TemplateExpr& node) {
    std::vector<V> vals;
    vals.reserve(node.parts.size());
    for (const ExprPtr& p : node.parts) vals.push_back(e(*p));

    /* 参考的每个 def 只有**一份帧**（非重入），所以同一模板里对**同一个 def** 的多次调用共用同一块
       结果缓冲：后一次调用连内容带头里的长度一起覆写前一次的，于是**每一段都渲染出最后一次调用的
       结果**（实测 `work/tmp/uibuf/probe_ui.py`：A/S/W 例如此；T/U/V/Z 例说明两个不同 def 之间不串，
       B/E/F/G/H 例说明字面量与变量不串）。我们这边每次调用各拿一块栈缓冲，不模拟这条就差字节
       （语料 001/047/…/054 正是 `{get_binary_repr(x,1)}    {get_binary_repr(y,8)}` 这一形）。
       调用**已经**求过值了，这里只把前一段的值改成后一段的（同一个 SSA 指针，不会重复调用）。 */
    std::map<std::string, std::size_t> last_same_callee;
    for (std::size_t i = 0; i < node.parts.size(); ++i) {
        const std::string* name = callee_name_of(*node.parts[i]);
        if (name != nullptr && vals[i].ty->kind == TyKind::Str) last_same_callee[*name] = i;
    }
    for (std::size_t i = 0; i < node.parts.size(); ++i) {
        const std::string* name = callee_name_of(*node.parts[i]);
        if (name == nullptr || vals[i].ty->kind != TyKind::Str) continue;
        if (last_same_callee[*name] != i) vals[i] = vals[last_same_callee[*name]];
    }

    /* 缓冲尺寸：文字段 + 每段的最坏字节数。**没有 Str 段时它是编译期常量** → 把 alloca 放进
       entry 块（静态 alloca：循环里不涨栈，也不会拉出 `___chkstk_ms`）；只有 Str 段的长度是
       运行期的，那时才只能开动态 alloca。 */
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    uint64_t fixed = 8;
    bool const_size = true;
    for (std::size_t i = 0; i < node.texts.size(); ++i) {
        fixed += node.texts[i].size();
        if (i >= vals.size()) break;
        if (vals[i].ty->kind == TyKind::Str) {
            const_size = false;
        } else {
            fixed += part_capacity(vals[i], node.src_line);
        }
    }
    LLVMValueRef buf = nullptr;
    if (const_size) {
        buf = alloca_in_entry(LLVMArrayType2(i8, fixed), "tbuf");
    } else {
        LLVMValueRef size = const_int(fixed);
        for (const V& v : vals) {
            if (v.ty->kind == TyKind::Str) size = LLVMBuildAdd(cur_fn().bd, size, len_of(v, node.src_line), "ts");
        }
        buf = LLVMBuildArrayAlloca(cur_fn().bd, i8, size, "tbuf");
        LLVMSetAlignment(buf, 8);
    }
    std::vector<LLVMValueRef> g8{const_int(8)};
    LLVMValueRef payload = LLVMBuildGEP2(cur_fn().bd, i8, buf, g8.data(), 1, "tpl");
    LLVMValueRef cur = payload;
    for (std::size_t i = 0; i < node.texts.size(); ++i) {
        cur = write_literal(cur, node.texts[i], node.src_line);
        if (i < vals.size()) cur = render_part(cur, vals[i], node.src_line);
    }
    /* 长度按**实际游标**算，不按缓冲尺寸 —— 所以尺寸只是个上界，不需要预先知道十进制位数。 */
    LLVMValueRef len = LLVMBuildSub(cur_fn().bd, LLVMBuildPtrToInt(cur_fn().bd, cur, i64_, "tce"),
                                    LLVMBuildPtrToInt(cur_fn().bd, payload, i64_, "tpe"), "tlen");
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef head = LLVMBuildGEP2(cur_fn().bd, i8, buf, g0.data(), 1, "thdr");
    store_addr(head, V::instr(LLVMBuildShl(cur_fn().bd, len, const_int(16), "thl"), ty_i64()), ty_i64(),
               node.src_line);
    return V::instr(payload, ty_str());
}

/* 一段值的**最坏字节数**（缓冲开多大）：`Bool` 5、其它整数/`Ptr` 21（20 位十进制 + '-'）。
   `Str` 的长度是运行期的，不算在这里（调用方单独用 `len_of` 加）——所以返回 0。
   不可渲染的类型在这里就报错，不拖到写入时。 */
uint64_t Lower::part_capacity(const V& v, int line) {
    if (v.ty->kind == TyKind::Str) return 0;
    if (v.ty->kind == TyKind::Ptr) return 21;
    if (v.ty->kind == TyKind::Int) return v.ty->is_bool ? 5 : 21;
    throw Unsupported("模板插值遇到 " + std::string(ty_kind_name(v.ty->kind)), line);
}

/* 文字段：字节常量逐个写（内容在编译期就定死了，不需要拷贝循环）。 */
LLVMValueRef Lower::write_literal(LLVMValueRef cur, const std::string& text, int line) {
    if (text.find('\\') != std::string::npos) {
        throw Unsupported("模板里含转义的文字段（U-L10 未定）", line);
    }
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    for (std::size_t k = 0; k < text.size(); ++k) {
        std::vector<LLVMValueRef> gi{const_int(k)};
        LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, i8, cur, gi.data(), 1, "lp");
        LLVMValueRef st =
            LLVMBuildStore(cur_fn().bd, LLVMConstInt(i8, static_cast<unsigned char>(text[k]), 0), p);
        LLVMSetAlignment(st, 1);
    }
    std::vector<LLVMValueRef> gn{const_int(text.size())};
    return LLVMBuildGEP2(cur_fn().bd, i8, cur, gn.data(), 1, "lc");
}

LLVMValueRef Lower::render_part(LLVMValueRef cur, const V& v, int line) {
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    if (v.ty->kind == TyKind::Str) {
        return emit_call(ensure_copy_fn(), {cur, to_ptr(v), len_of(v, line)});
    }
    if (v.ty->kind == TyKind::Int && v.ty->is_bool) {
        /* `true` / `false`：前 4 个字节按条件选，第 5 个恒为 'e'，长度 4/5。 */
        const char* yes = "true";
        const char* no = "false";
        LLVMValueRef nz = LLVMBuildICmp(cur_fn().bd, LLVMIntNE, llv(v), const_int(0), "bn");
        for (std::size_t k = 0; k < 4; ++k) {
            std::vector<LLVMValueRef> gi{const_int(k)};
            LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, i8, cur, gi.data(), 1, "bp");
            LLVMValueRef ch = LLVMBuildSelect(
                cur_fn().bd, nz, const_int(static_cast<unsigned char>(yes[k])),
                const_int(static_cast<unsigned char>(no[k])), "bc");
            LLVMValueRef st = LLVMBuildStore(cur_fn().bd, LLVMBuildTrunc(cur_fn().bd, ch, i8, "bt"), p);
            LLVMSetAlignment(st, 1);
        }
        std::vector<LLVMValueRef> g4{const_int(4)};
        LLVMValueRef p4 = LLVMBuildGEP2(cur_fn().bd, i8, cur, g4.data(), 1, "bp");
        LLVMValueRef st4 = LLVMBuildStore(cur_fn().bd, LLVMConstInt(i8, 'e', 0), p4);
        LLVMSetAlignment(st4, 1);
        std::vector<LLVMValueRef> gn{
            LLVMBuildSelect(cur_fn().bd, nz, const_int(4), const_int(5), "bl")};
        return LLVMBuildGEP2(cur_fn().bd, i8, cur, gn.data(), 1, "bend");
    }
    if (v.ty->kind == TyKind::Int || v.ty->kind == TyKind::Ptr) {
        LLVMValueRef x = llv(v);
        LLVMValueRef minus = const_int(0);
        if (v.ty->kind == TyKind::Int) {
            x = mask_ll(x, v.ty->bits);
            if (v.ty->is_signed) {
                x = sext64(x, v.ty->bits);
                LLVMValueRef neg = LLVMBuildICmp(cur_fn().bd, LLVMIntSLT, x, const_int(0), "dneg");
                minus = LLVMBuildZExt(cur_fn().bd, neg, i64_, "dm");
                /* 幅度按**无符号取负**算 —— 这样 S64 最小值也对（实测：`-9223372036854775808`）。 */
                x = LLVMBuildSelect(cur_fn().bd, neg, LLVMBuildSub(cur_fn().bd, const_int(0), x, "dmg"), x, "dv");
            }
        }
        return emit_call(ensure_dec_fn(), {cur, x, minus});
    }
    throw Unsupported("模板插值遇到 " + std::string(ty_kind_name(v.ty->kind)), line);
}

LLVMValueRef Lower::ensure_copy_fn() {
    auto hit = helpers_.find("copy");
    if (hit != helpers_.end()) return hit->second;
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMTypeRef params[3] = {ptr_, ptr_, i64_};
    LLVMValueRef fn =
        LLVMAddFunction(module(), "__simplexc_copy", LLVMFunctionType(ptr_, params, 3, 0));
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    helpers_["copy"] = fn;
    fn_seen_++;
    FnCtx& f = begin_function(fn, ty_ptr());
    f.name = "__simplexc_copy";
    LLVMValueRef dst = LLVMGetParam(fn, 0);
    LLVMValueRef src = LLVMGetParam(fn, 1);
    LLVMValueRef n = LLVMGetParam(fn, 2);
    LLVMValueRef slot = alloca_slot(ty_i64(), "ci");
    store_addr(slot, V::instr(const_int(0), ty_i64()), ty_i64(), 0);
    LLVMBasicBlockRef head = br_new_block("ch");
    LLVMValueRef i = llv(load_addr(slot, ty_i64()));
    LLVMValueRef lt = LLVMBuildICmp(cur_fn().bd, LLVMIntULT, i, n, "clt");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), fn, "cb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), fn, "cd");
    LLVMBuildCondBr(cur_fn().bd, lt, body, done);
    LLVMPositionBuilderAtEnd(cur_fn().bd, body);
    cur_fn().block = body;
    std::vector<LLVMValueRef> gi{i};
    LLVMValueRef sp = LLVMBuildGEP2(cur_fn().bd, i8, src, gi.data(), 1, "csp");
    LLVMValueRef dp = LLVMBuildGEP2(cur_fn().bd, i8, dst, gi.data(), 1, "cdp");
    LLVMValueRef b = LLVMBuildLoad2(cur_fn().bd, i8, sp, "cb0");
    LLVMSetAlignment(b, 1);
    LLVMValueRef st = LLVMBuildStore(cur_fn().bd, b, dp);
    LLVMSetAlignment(st, 1);
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, i, const_int(1), "ci1"), ty_i64()), ty_i64(), 0);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    std::vector<LLVMValueRef> gn{n};
    LLVMBuildRet(cur_fn().bd, LLVMBuildGEP2(cur_fn().bd, i8, dst, gn.data(), 1, "cr"));
    finish_function();
    return fn;
}

LLVMValueRef Lower::ensure_streq_fn() {
    auto hit = helpers_.find("streq");
    if (hit != helpers_.end()) return hit->second;
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMTypeRef params[3] = {ptr_, ptr_, i64_};
    LLVMValueRef fn =
        LLVMAddFunction(module(), "__simplexc_streq", LLVMFunctionType(i64_, params, 3, 0));
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    helpers_["streq"] = fn;
    fn_seen_++;
    FnCtx& f = begin_function(fn, ty_i64());
    f.name = "__simplexc_streq";
    LLVMValueRef a = LLVMGetParam(fn, 0);
    LLVMValueRef b = LLVMGetParam(fn, 1);
    LLVMValueRef n = LLVMGetParam(fn, 2);
    LLVMValueRef slot = alloca_slot(ty_i64(), "qi");
    store_addr(slot, V::instr(const_int(0), ty_i64()), ty_i64(), 0);
    LLVMBasicBlockRef head = br_new_block("qh");
    LLVMValueRef i = llv(load_addr(slot, ty_i64()));
    LLVMValueRef lt = LLVMBuildICmp(cur_fn().bd, LLVMIntULT, i, n, "qlt");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), fn, "qb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), fn, "qd");
    LLVMBuildCondBr(cur_fn().bd, lt, body, done);
    LLVMPositionBuilderAtEnd(cur_fn().bd, body);
    cur_fn().block = body;
    std::vector<LLVMValueRef> gi{i};
    LLVMValueRef ap = LLVMBuildGEP2(cur_fn().bd, i8, a, gi.data(), 1, "qap");
    LLVMValueRef bp = LLVMBuildGEP2(cur_fn().bd, i8, b, gi.data(), 1, "qbp");
    LLVMValueRef av = LLVMBuildLoad2(cur_fn().bd, i8, ap, "qa0");
    LLVMSetAlignment(av, 1);
    LLVMValueRef bv = LLVMBuildLoad2(cur_fn().bd, i8, bp, "qb0");
    LLVMSetAlignment(bv, 1);
    LLVMValueRef ne = LLVMBuildICmp(cur_fn().bd, LLVMIntNE, av, bv, "qne");
    LLVMBasicBlockRef cont = LLVMAppendBasicBlockInContext(ctx(), fn, "qc");
    LLVMBasicBlockRef diff = LLVMAppendBasicBlockInContext(ctx(), fn, "qx");
    LLVMBuildCondBr(cur_fn().bd, ne, diff, cont);
    LLVMPositionBuilderAtEnd(cur_fn().bd, cont);
    cur_fn().block = cont;
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, i, const_int(1), "qi1"), ty_i64()), ty_i64(), 0);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, diff);
    cur_fn().block = diff;
    LLVMBuildRet(cur_fn().bd, const_int(0));
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    LLVMBuildRet(cur_fn().bd, const_int(1));
    finish_function();
    return fn;
}

LLVMValueRef Lower::ensure_dec_fn() {
    auto hit = helpers_.find("dec");
    if (hit != helpers_.end()) return hit->second;
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMTypeRef params[3] = {ptr_, i64_, i64_};
    LLVMValueRef fn =
        LLVMAddFunction(module(), "__simplexc_dec", LLVMFunctionType(ptr_, params, 3, 0));
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    helpers_["dec"] = fn;
    fn_seen_++;
    FnCtx& f = begin_function(fn, ty_ptr());
    f.name = "__simplexc_dec";
    LLVMValueRef p = LLVMGetParam(fn, 0);
    LLVMValueRef v = LLVMGetParam(fn, 1);
    LLVMValueRef minus = LLVMGetParam(fn, 2);
    /* 位数：19 次比较（10^1…10^19），不写循环 —— 每个插值点都要走这一段，形状越平越好。 */
    LLVMValueRef n = const_int(1);
    uint64_t thr = 1;
    for (int d = 1; d <= 19; ++d) {
        thr *= 10;
        LLVMValueRef ge = LLVMBuildICmp(cur_fn().bd, LLVMIntUGE, v, const_int(thr), "dge");
        n = LLVMBuildSelect(cur_fn().bd, ge, const_int(static_cast<uint64_t>(d + 1)), n, "dn");
    }
    LLVMValueRef total = LLVMBuildAdd(cur_fn().bd, n, minus, "dtot");
    std::vector<LLVMValueRef> gt{total};
    LLVMValueRef end = LLVMBuildGEP2(cur_fn().bd, i8, p, gt.data(), 1, "dend");
    LLVMValueRef has_minus = LLVMBuildICmp(cur_fn().bd, LLVMIntNE, minus, const_int(0), "dhm");
    LLVMBasicBlockRef neg = LLVMAppendBasicBlockInContext(ctx(), fn, "dneg");
    LLVMBasicBlockRef head = LLVMAppendBasicBlockInContext(ctx(), fn, "dh");
    LLVMBuildCondBr(cur_fn().bd, has_minus, neg, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, neg);
    cur_fn().block = neg;
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef p0 = LLVMBuildGEP2(cur_fn().bd, i8, p, g0.data(), 1, "dp0");
    LLVMValueRef sm = LLVMBuildStore(cur_fn().bd, LLVMConstInt(i8, '-', 0), p0);
    LLVMSetAlignment(sm, 1);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, head);
    cur_fn().block = head;
    /* 从末尾往前填：先算长度，所以 `i` 初值是总长，每轮先减再写。 */
    LLVMValueRef slot_i = alloca_slot(ty_i64(), "di");
    store_addr(slot_i, V::instr(total, ty_i64()), ty_i64(), 0);
    LLVMValueRef slot_x = alloca_slot(ty_i64(), "dx");
    store_addr(slot_x, V::instr(v, ty_i64()), ty_i64(), 0);
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), fn, "db");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), fn, "dd");
    LLVMBuildBr(cur_fn().bd, body);
    LLVMPositionBuilderAtEnd(cur_fn().bd, body);
    cur_fn().block = body;
    LLVMValueRef i2 = LLVMBuildSub(cur_fn().bd, llv(load_addr(slot_i, ty_i64())), const_int(1), "di2");
    store_addr(slot_i, V::instr(i2, ty_i64()), ty_i64(), 0);
    LLVMValueRef x = llv(load_addr(slot_x, ty_i64()));
    LLVMValueRef dg = LLVMBuildURem(cur_fn().bd, x, const_int(10), "ddg");
    LLVMValueRef ch = LLVMBuildAdd(cur_fn().bd, dg, const_int('0'), "dch");
    std::vector<LLVMValueRef> gi{i2};
    LLVMValueRef dp = LLVMBuildGEP2(cur_fn().bd, i8, p, gi.data(), 1, "ddp");
    LLVMValueRef st = LLVMBuildStore(cur_fn().bd, LLVMBuildTrunc(cur_fn().bd, ch, i8, "dbt"), dp);
    LLVMSetAlignment(st, 1);
    LLVMValueRef x2 = LLVMBuildUDiv(cur_fn().bd, x, const_int(10), "dqx");
    store_addr(slot_x, V::instr(x2, ty_i64()), ty_i64(), 0);
    LLVMValueRef more = LLVMBuildICmp(cur_fn().bd, LLVMIntNE, x2, const_int(0), "dmr");
    LLVMBuildCondBr(cur_fn().bd, more, body, done);
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    LLVMBuildRet(cur_fn().bd, end);
    finish_function();
    return fn;
}

V Lower::array_literal(const ArrayLitExpr& node) {
    std::vector<V> ev;
    for (const auto& item : node.items) ev.push_back(cv(*item));
    if (ev.empty()) throw Unsupported("空数组字面量", node.src_line);
    const TyRef& ty = ev[0].ty;
    LLVMTypeRef el = storage_ll(*ty);
    std::vector<LLVMValueRef> elems;
    for (const auto& e : ev) elems.push_back(const_for_storage(e));
    /* 载荷前必须带 8 字节 Nim 头（`p[-8] = len << 16`，读法见 `len_of` 与 facts §数组/字符串布局）：
     * 少了它，`.len()` 那条越界的**常量** `load` 在 O2 里是 undef，靠它判分支的整条 `if`
     * 会被折掉 —— 实测 008 的 `check_output` 因此恒返回 fail（`sim_test_result` 2 而非 1、
     * `sim_cycle` 每 tick 少 1），机理与探针在 `docs/notes/2026-10-18-test-result-chain.md`。
     * 头低 16 位是 log2 容量，静态数据取 0（与 `array(<T>, n)` / `empty_array` 同口径）。 */
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    const uint64_t header = elems.size() << 16;
    std::vector<LLVMValueRef> head;
    for (int i = 0; i < 8; ++i) head.push_back(LLVMConstInt(i8, (header >> (8 * i)) & 0xFF, 0));
    LLVMTypeRef head_ty = LLVMArrayType2(i8, head.size());
    LLVMTypeRef payload_ty = LLVMArrayType2(el, elems.size());
    LLVMTypeRef fields[2] = {head_ty, payload_ty};
    LLVMTypeRef sty = LLVMStructTypeInContext(ctx(), fields, 2, 0);
    const std::string gname = ".arr" + std::to_string(node.src_line);
    LLVMValueRef g = LLVMAddGlobal(module(), sty, gname.c_str());
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(g, 1);
    LLVMValueRef parts[2] = {LLVMConstArray2(i8, head.data(), head.size()),
                             LLVMConstArray2(el, elems.data(), elems.size())};
    LLVMSetInitializer(g, LLVMConstStructInContext(ctx(), parts, 2, 0));
    std::vector<LLVMValueRef> gi{const_int(0), const_int(1)};
    return V::instr(LLVMConstGEP2(sty, g, gi.data(), 2), make_array(ty));
}

/* 元素（结构体字段）的**存储形态**值 —— 与 `store_addr` 同一套口径：整数按字段宽度截断
 * （窄写只写该宽度，U-L1），非整数原样。 */
LLVMValueRef Lower::field_storage_value(const V& v, const TyRef& ty, int line) {
    if (!is_int(*ty)) return llv(v);
    V conv = convert(v, ty, line);
    LLVMTypeRef st = storage_ll(*ty);
    LLVMValueRef ll = llv(conv);
    if (st != i64_) ll = LLVMBuildTrunc(cur_fn().bd, ll, st, "t");
    return ll;
}

V Lower::composite_literal(const CompositeLitExpr& node) {
    TyRef ty = ty_of_ref(*node.typ, node.src_line);
    if (ty->kind != TyKind::Struct) {
        throw Unsupported(std::string("复合字面量类型 ") + ty_kind_name(ty->kind), node.src_line);
    }
    auto it = struct_ll_.find(ty->name);
    if (it == struct_ll_.end()) throw CompileFail("struct 类型没建好：" + ty->name);
    /* 未列字段取**该字段元素的零值**：聚合的零值底座 + 逐字段覆盖。
     * 字段初始化按**源码顺序**求值（求值有副作用时看得见），与字段名顺序无关 —— 实验见
     * `work/tmp/ul4/`。先求值再拼聚合，两趟分开就是为了保住这个顺序。 */
    struct FieldVal {
        size_t idx;
        V v;
    };
    std::vector<FieldVal> vals;
    bool all_const = true;
    /* 不在函数体里（顶层 `var x = T {…}`）就只能求常量：这时**不能**走 `e()` —— 它按「有
     * builder」的前提发指令，碰到字段值里的函数调用会在 `nullptr` 上摊（实测 0xC0000005）。
     * `const_value()` 遇运行期表达式返回 `nullopt`，于是下面报一句可读的错。 */
    const bool in_function = cur_fn().bd != nullptr && cur_fn().block != nullptr;
    for (const auto& f : node.fields) {
        size_t idx = ty->fields.size();
        for (size_t i = 0; i < ty->fields.size(); ++i) {
            if (ty->fields[i].first == f->name) {
                idx = i;
                break;
            }
        }
        if (idx == ty->fields.size()) {
            throw Unsupported("struct " + ty->name + " 没有字段 " + f->name, f->src_line);
        }
        const TyRef& fty = ty->fields[idx].second;
        std::optional<V> raw = in_function ? std::optional<V>(e(*f->value))
                                          : const_value(*f->value);
        if (!raw) {
            throw Unsupported("复合字面量的字段 " + f->name + " 不是编译期常量", f->src_line);
        }
        V conv = convert(*raw, fty, f->src_line);
        all_const = all_const && conv.c.has_value();
        vals.push_back(FieldVal{idx, conv});
    }
    if (all_const) {
        /* 常量聚合：把**每个**字段都放进去（未列的放零），于是顶层 `var x = T {…}` 的
         * 初始化式也能用 —— 那条路拿不到 builder，只能靠聚合常量。 */
        std::vector<LLVMValueRef> elems;
        elems.reserve(ty->fields.size());
        for (const auto& f : ty->fields) elems.push_back(LLVMConstNull(storage_ll(*f.second)));
        for (const FieldVal& fv : vals) {
            elems[fv.idx] = const_for_storage(fv.v);
        }
        LLVMValueRef agg = LLVMConstStructInContext(ctx(), elems.data(),
                                                    static_cast<unsigned>(elems.size()), 1);
        return V::instr(agg, ty);
    }
    if (cur_fn().bd == nullptr || cur_fn().block == nullptr) {
        /* 常量环境里出现**没有常量值**的字段（数组字面量那种「有 ll、无 c」）：不许在 nullptr
         * 上发指令。 */
        throw Unsupported("复合字面量里的字段值不是编译期常量", node.src_line);
    }
    LLVMPositionBuilderAtEnd(cur_fn().bd, cur_fn().block);
    LLVMValueRef agg = LLVMConstNull(it->second);
    for (const FieldVal& fv : vals) {
        const TyRef& fty = ty->fields[fv.idx].second;
        unsigned pos = static_cast<unsigned>(fv.idx);
        LLVMValueRef el = fv.v.c ? const_for_storage(fv.v)
                                 : field_storage_value(fv.v, fty, node.src_line);
        agg = LLVMBuildInsertValue(cur_fn().bd, agg, el, pos, "cl");
    }
    return V::instr(agg, ty);
}

/* ================================================================ Program */

Program::Program(const ModuleAst& ast, bool collect) : ast_(ast) {
    collect_ = collect;
    scan_decls();
    scan_scope_names();
    make_context();
    make_types();
    make_globals();
}

/* `in_scope` ① 档的整单元预扫。名字表取**未经 `in_scope` 裁剪的原 AST**（F1/F2：写在由自己
 * 守卫的块里、或写在被丢掉的块里的 def 都算），所以它只需一遍、不必到不动点。 */
void Program::scan_scope_names() {
    for (const FunctionDef* d : ast_.defs) in_scope_names_.insert(d->name);
    for (const EnumDecl& d : ast_.enums) {
        in_scope_names_.insert(d.name);
        for (const std::string& member : d.members) in_scope_names_.insert(member);
    }
    for (const StructDecl& d : ast_.structs) in_scope_names_.insert(d.name);
    for (const VarDeclStmt* st : ast_.top_vars) {
        if (st->kw == "const" && !st->name.empty() && st->name[0] == '#') {
            in_scope_names_.insert(st->name);
        }
    }
    if (ast_.label != nullptr) {
        RootDecls root;
        collect_root_decls(*ast_.label->body, root);
        for (const DefStmt* d : root.defs) {
            in_scope_names_.insert(d->name);
            root_defs_[d->name] = d;
        }
        for (const VarDeclStmt* v : root.consts) {
            in_scope_names_.insert(v->name);
            root_const_decls_[v->name] = v;
        }
    }
}

void Program::scan_decls() {
    for (const EnumDecl& d : ast_.enums) {
        enums_[d.name] = d.members;
        for (size_t i = 0; i < d.members.size(); ++i) {
            if (enum_member_.count(d.members[i]) != 0) {
                throw CompileFail("枚举成员 " + d.members[i] + " 重名（枚举 " + d.name + "）");
            }
            enum_member_[d.members[i]] = {d.name, static_cast<int>(i)};
        }
    }
    for (const StructDecl& d : ast_.structs) {
        try {
            std::vector<std::pair<std::string, TyRef>> fields;
            for (const auto& f : d.fields) fields.push_back({f.first, ty_of_ref(*f.second, 0)});
            if (fields.empty()) {
                /* 空 struct（rv32im 的 `Input`/`Output`）：U-L4 布局未定，取 **1 字节占位**
                 * 而不是零宽度聚合 —— 零宽度的 alloca / 全局大小是 0，那条路没验证过。 */
                structs_[d.name] = make_struct(d.name, 8, {});
                continue;
            }
            int bits = 0;
            for (const auto& f : fields) bits += storage_bytes(*f.second) * 8;
            structs_[d.name] = make_struct(d.name, bits, fields);
        } catch (const Unsupported& e) {
            record(e.what(), e.line());
        }
    }
    for (const FunctionDef* d : ast_.defs) defs_[d->name].push_back(d);
    top_vars_ = ast_.top_vars;
}

void Program::make_context() { open_module("simplex"); }

void Program::make_types() {
    for (const auto& entry : structs_) {
        const TyRef ty = entry.second;
        std::vector<LLVMTypeRef> elems;
        for (const auto& f : ty->fields) elems.push_back(storage_ll(*f.second));
        if (elems.empty()) elems.push_back(storage_ll(*make_int(ty->bits, false)));
        struct_ll_[entry.first] = LLVMStructTypeInContext(ctx(), elems.data(),
                                                          static_cast<unsigned>(elems.size()), 1);
    }
    for (const auto& entry : structs_) {
        const TyRef ty = entry.second;
        std::map<std::string, FieldRef> offs;
        int off = 0;
        for (const auto& f : ty->fields) {
            offs[f.first] = FieldRef{off, f.second};
            off += storage_bytes(*f.second);
        }
        field_offsets_[entry.first] = offs;
    }
}

void Program::make_globals() {
    for (const VarDeclStmt* st : top_vars_) {
        try {
            if (st->kw == "const" && !st->name.empty() && st->name[0] == '#') {
                auto v = const_value(*st->init);
                if (!v) {
                    throw Unsupported("编译期常量 " + st->name + " 的初始化式不是常量", st->src_line);
                }
                consts_[st->name] = *v;
                continue;
            }
            auto v = const_value(*st->init);
            if (!v) {
                throw Unsupported("顶层 var " + st->name +
                                      " 的初始化式不是编译期常量（运行期初始化未做）",
                                  st->src_line);
            }
            LLVMValueRef g = LLVMAddGlobal(module(), ll_type(*v->ty), st->name.c_str());
            LLVMSetLinkage(g, LLVMInternalLinkage);
            LLVMSetInitializer(g, const_for_storage(*v));
            globals_[st->name] = {g, v->ty};
        } catch (const Unsupported& e) {
            if (!collect_) throw;
            unsupported_.push_back(Diagnostic{e.line() != 0 ? e.line() : st->src_line, e.what()});
            LLVMValueRef g = LLVMAddGlobal(module(), i64_, st->name.c_str());
            LLVMSetLinkage(g, LLVMInternalLinkage);
            LLVMSetInitializer(g, LLVMConstNull(i64_));
            globals_[st->name] = {g, ty_i64()};
        }
    }
}

void Program::build() {
    if (ast_.label == nullptr) throw CompileFail("没有顶层语句块（程序根）");
    try {
        emit_main(*ast_.label);
    } catch (const Unsupported& e) {
        record(e.what(), e.line());
    }
}

void Program::emit_main(const FunctionDef& label) {
    LLVMValueRef fnref = LLVMAddFunction(
        module(), "tc_main", LLVMFunctionType(void_ty_, nullptr, 0, 0));
    FnCtx& f = begin_function(fnref, ty_void());
    f.name = "tc_main";
    f.def = nullptr;   /* 程序根不属于任何 def（基座的 `def` 本来就是空，这里写死意图） */
    lower_block(*label.body);
    /* 程序根末尾隐式 thread_exit（接口 §1「程序根」）。 */
    LLVMValueRef ext = get_extern("thread_exit", ty_void(), {});
    emit_call(ext, {});
    LLVMBuildRetVoid(cur_fn().bd);
    finish_function();
}

std::string Program::ir_text() const { return print_module(module()); }

std::vector<uint8_t> Program::emit_object() {
    std::string err;
    std::unique_ptr<TargetMachine> tm(TargetMachine::create(kTriple, &err));
    if (!tm) throw CompileFail(err);
    if (!verify_module(module(), &err)) throw CompileFail("IR 校验失败：" + err);
    if (!tm->run_default_o2(module(), &err)) throw CompileFail(err);
    std::vector<uint8_t> out;
    if (!tm->emit_object(module(), &out, &err)) throw CompileFail(err);
    return out;
}

}  // namespace lower
}  // namespace simplexc
