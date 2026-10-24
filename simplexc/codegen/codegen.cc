/* codegen/codegen.cc —— `sema::Hir` → LLVM IR（接口与取舍见 codegen.hh）。
 *
 * 本文件逐函数对应旧 `lower.cc` 的同名实现：「读 AST 节点」换成「读 HNode」，
 * 「按 AST 推类型/折常量/挑实例」整段消失（HIR 已经算好）。发射逻辑、块与指令的命名、alloca
 * 的位置、按需发函数体的顺序**一处不改** —— 那是产物逐字节相同的前提，也是双跑对照能定位
 * 差异的前提。
 */
#include "codegen.hh"

#include <algorithm>
#include <cassert>
#include <functional>
#include <utility>

#include "../sema/builtins.hh"

namespace simplexc {
namespace codegen {
namespace {

/* 前缀（stdlib）里的全局名：旧实现就是这张表（`lower.cc` 的 `prefix_globals()`，只有这一个 ——
 * `stdlib_prefix.si` 里也只有 `var global_seed = Seed 1` 一行）。它的用途只剩「这个全局是按需
 * 建的」这一条，建序见 `Program::make_globals`。 */
bool is_prefix_global(const std::string& n) { return n == "global_seed"; }

/* 1970-01-01 的 FILETIME（100ns）；`get_time` 的换算常数。 */
constexpr uint64_t kFileTimeUnix = 116444736000000000ull;
constexpr uint64_t kNsPerMs = 1000000ull;

/* 前缀里 `xorshift` 乘的那个常数（`stdlib_prefix.si:1470`）：源码写的是 **15 位**十六进制，
 * 不是教科书的 `0x2545F4914F6CDD1D` —— 实测能分开（`docs/notes/2026-10-18-random.md`）。 */
constexpr uint64_t kSeedMul = 0x2545F4914F6CDD1ull;

/* `HBuiltin` 的编号（方案 §2f 冻结：只许追加）。`Call.callee.index` 是 int，所以这里先落成
 * int 常量再进 switch —— case 标签必须是常量表达式。 */
constexpr int kBuiltinLoad = static_cast<int>(sema::HBuiltin::Load);
constexpr int kBuiltinStore = static_cast<int>(sema::HBuiltin::Store);
constexpr int kBuiltinRawCast = static_cast<int>(sema::HBuiltin::RawCast);
constexpr int kBuiltinAsr = static_cast<int>(sema::HBuiltin::Asr);
constexpr int kBuiltinClz = static_cast<int>(sema::HBuiltin::Clz);
constexpr int kBuiltinRor = static_cast<int>(sema::HBuiltin::Ror);
constexpr int kBuiltinMin = static_cast<int>(sema::HBuiltin::Min);
constexpr int kBuiltinMax = static_cast<int>(sema::HBuiltin::Max);
constexpr int kBuiltinArray = static_cast<int>(sema::HBuiltin::Array);
constexpr int kBuiltinRandom = static_cast<int>(sema::HBuiltin::Random);
constexpr int kBuiltinLen = static_cast<int>(sema::HBuiltin::Len);
constexpr int kBuiltinMs = static_cast<int>(sema::HBuiltin::Ms);

/* 节点类别名（报错文本用，与 `hir.hh` 的 `HKind` 一一对应）。 */
const char* hkind_name(sema::HKind k) {
    switch (k) {
        case sema::HKind::Const: return "Const";
        case sema::HKind::StrLit: return "StrLit";
        case sema::HKind::Local: return "Local";
        case sema::HKind::Global: return "Global";
        case sema::HKind::Capture: return "Capture";
        case sema::HKind::Unary: return "Unary";
        case sema::HKind::Binary: return "Binary";
        case sema::HKind::Cond: return "Cond";
        case sema::HKind::Call: return "Call";
        case sema::HKind::Member: return "Member";
        case sema::HKind::Index: return "Index";
        case sema::HKind::Cast: return "Cast";
        case sema::HKind::ArrayLit: return "ArrayLit";
        case sema::HKind::CompositeLit: return "CompositeLit";
        case sema::HKind::Template: return "Template";
        case sema::HKind::Block: return "Block";
        case sema::HKind::VarDecl: return "VarDecl";
        case sema::HKind::Assign: return "Assign";
        case sema::HKind::ExprStmt: return "ExprStmt";
        case sema::HKind::If: return "If";
        case sema::HKind::While: return "While";
        case sema::HKind::Switch: return "Switch";
        case sema::HKind::Break: return "Break";
        case sema::HKind::Return: return "Return";
    }
    return "?";
}

LLVMIntPredicate cmp_pred(sema::HBinOp op, bool sgn) {
    switch (op) {
        case sema::HBinOp::Eq: return LLVMIntEQ;
        case sema::HBinOp::Ne: return LLVMIntNE;
        case sema::HBinOp::Lt: return sgn ? LLVMIntSLT : LLVMIntULT;
        case sema::HBinOp::Le: return sgn ? LLVMIntSLE : LLVMIntULE;
        case sema::HBinOp::Gt: return sgn ? LLVMIntSGT : LLVMIntUGT;
        case sema::HBinOp::Ge: return sgn ? LLVMIntSGE : LLVMIntUGE;
        default: break;
    }
    return LLVMIntEQ; /* StrEq/StrNe 与算术 op 不走这里（`e_binary` 已分流） */
}

/* 运算是哪个 opcode（只给算术族用；比较与短路逻辑各自分流）。 */
const char* arith_op_name(sema::HBinOp op) {
    switch (op) {
        case sema::HBinOp::Add: return "+";
        case sema::HBinOp::Sub: return "-";
        case sema::HBinOp::Mul: return "*";
        case sema::HBinOp::Div: return "/";
        case sema::HBinOp::Rem: return "%";
        case sema::HBinOp::BitAnd: return "&";
        case sema::HBinOp::BitOr: return "|";
        case sema::HBinOp::BitXor: return "^";
        case sema::HBinOp::Shl: return "<<";
        case sema::HBinOp::Shr: return ">>";
        default: break;
    }
    return "";
}

/* 模板里的一段是「按名字直接调用」时返回被调者的逻辑名，其它形态返回 nullptr。
 * 用途见 `template_expr` 里对参考「每个 def 一份帧」的模拟。 */
const std::string* callee_name_of(const sema::Hir& hir, const sema::HNode& n) {
    if (n.kind != sema::HKind::Call || n.callee.kind != sema::HCalleeKind::Func) return nullptr;
    if (n.callee.index < 0 || n.callee.index >= static_cast<int>(hir.funcs.size())) return nullptr;
    return &hir.funcs[static_cast<std::size_t>(n.callee.index)].name;
}

}  // namespace

/* ================================================================ 骨架 */

/* 基座上下文：`cur_fn()` 在第一个函数开始之前也得有效（发全局初值时还要读它）。它没有 builder、
 * 没有 builder 作用域，`finish_function` 拒绝弹它。 */
Codegen::Codegen(const sema::Hir& hir) : hir_(hir) {
    fn_ctxs_.push_back(std::make_unique<FnCtx>());
    fnrefs_.assign(hir.funcs.size(), nullptr);
}

Codegen::~Codegen() {
    if (cur_fn().bd != nullptr) LLVMDisposeBuilder(cur_fn().bd);
}

void Codegen::open_module(const char* name) {
    init_native_target();
    ctx_ = std::make_unique<Context>();
    mod_ = std::make_unique<Module>(ctx_->get(), name);
    i64_ = LLVMInt64TypeInContext(ctx_->get());
    ptr_ = LLVMPointerType(i64_, 0);
    void_ty_ = LLVMVoidTypeInContext(ctx_->get());
    int_types_[64] = i64_;
}

LLVMTypeRef Codegen::val_ll(const Ty& ty) {
    /* 一个**值**的 LLVM 表示类型（不是内存宽度，也不是 ABI 宽度）：整数一律 i64 位模式 + 掩码，
     * `Str`/`Arr` 是真 `ptr`（放进整数运算会引出 ADDR64 绝对重定位，装配契约 §4-2 直接拒）。 */
    if (ty.kind == TyKind::Struct || ty.kind == TyKind::Str || ty.kind == TyKind::Array) {
        return ll_type(ty);
    }
    return i64_;
}

LLVMTypeRef Codegen::ll_type(const Ty& ty) {
    if (ty.kind == TyKind::Struct) {
        auto it = struct_ll_.find(ty.name);
        if (it == struct_ll_.end()) throw CompileFail("struct 类型没建好：" + ty.name);
        return it->second;
    }
    if (ty.kind == TyKind::Str || ty.kind == TyKind::Array) return ptr_;
    return storage_ll(ty);
}

LLVMTypeRef Codegen::storage_ll(const Ty& ty) {
    const unsigned n = static_cast<unsigned>(std::max(8, storage_bytes(ty) * 8));
    auto it = int_types_.find(n);
    if (it != int_types_.end()) return it->second;
    LLVMTypeRef t = LLVMIntTypeInContext(ctx(), n);
    int_types_[n] = t;
    return t;
}

LLVMValueRef Codegen::const_int(uint64_t v) const { return LLVMConstInt(i64_, v & kMask64, 0); }

LLVMValueRef Codegen::const_ptr(uint64_t addr) const {
    return LLVMConstIntToPtr(const_int(addr), ptr_);
}

LLVMValueRef Codegen::const_for_storage(const V& v) {
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

/* 只有位模式、没有值句柄时用它（全局初值）：Str/Arr 给常量指针，聚合给 null。 */
LLVMValueRef Codegen::const_for_bits(const TyRef& ty, uint64_t bits) {
    if (ty->kind == TyKind::Struct) {
        auto it = struct_ll_.find(ty->name);
        if (it == struct_ll_.end()) throw CompileFail("struct 类型没建好：" + ty->name);
        return LLVMConstNull(it->second);
    }
    if (ty->kind == TyKind::Str || ty->kind == TyKind::Array) return const_ptr(bits);
    return LLVMConstInt(storage_ll(*ty), bits & mask_of(ty->bits), 0);
}

std::pair<LLVMValueRef, TyRef> Codegen::add_global(const sema::HirGlobal& g) {
    LLVMValueRef ref = LLVMAddGlobal(module(), ll_type(*g.ty), g.name.c_str());
    LLVMSetLinkage(ref, LLVMInternalLinkage);
    LLVMSetInitializer(ref, const_for_bits(g.ty, g.init_bits.value_or(0)));
    globals_[g.name] = {ref, g.ty};
    return globals_[g.name];
}

void Codegen::mark_no_builtin(LLVMValueRef fnref) {
    /* 原版编译器给每个函数挂 `nobuiltin`（否则 LLVM 会把 `memory_copy` 之类的名字当内建去认）。 */
    const unsigned kind = LLVMGetEnumAttributeKindForName("nobuiltin", 9);
    if (kind != 0) {
        LLVMAddAttributeAtIndex(fnref, LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx(), kind, 0));
    }
}

FnCtx& Codegen::begin_function(LLVMValueRef fnref, const TyRef& ret_ty) {
    mark_no_builtin(fnref);
    const FnCtx& outer = cur_fn(); /* 压栈只动 `unique_ptr`，所以这个引用在 push 后仍有效 */
    auto child = std::make_unique<FnCtx>();
    child->parent = &outer;
    /* 身份字段**沿用外层**（与改前逐字段存取时的可见值相同）：内联辅助函数与前缀函数的
     * `is_top_def` 来自调用者；`ensure_func` 按实例改它、`emit_main` 清掉。 */
    child->name = outer.name;
    child->is_top_def = outer.is_top_def;
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

void Codegen::finish_function() {
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

LLVMBasicBlockRef Codegen::new_block(const char* name) {
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, name);
    LLVMPositionBuilderAtEnd(cur_fn().bd, bb);
    cur_fn().block = bb;
    return bb;
}

LLVMBasicBlockRef Codegen::br_new_block(const char* name) {
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, name);
    LLVMBuildBr(cur_fn().bd, bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, bb);
    cur_fn().block = bb;
    return bb;
}

LLVMValueRef Codegen::alloca_in_entry(LLVMTypeRef ty, const std::string& name) {
    /* 插在 **entry 块最前面**：循环里声明也不会每次迭代吃栈，而且它是**静态** alloca
       （LLVM 只把「在 entry 块 + 尺寸是常量」的 alloca 当静态 —— 否则会拉出 `___chkstk_ms`
       且每轮涨栈）。 */
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

LLVMValueRef Codegen::alloca_slot(const TyRef& ty, const std::string& name) {
    return alloca_in_entry(ll_type(*ty), name);
}

/* ================================================================ 作用域与绑定 */

void Codegen::bind(const std::string& name, Binding b) {
    cur_fn().scopes.back()[name] = std::move(b);
}

void Codegen::bind_root_scope(const std::string& name, Binding b) {
    cur_fn().scopes.front()[name] = std::move(b);
}

const Binding* Codegen::lookup(const std::string& name) const {
    for (auto it = cur_fn().scopes.rbegin(); it != cur_fn().scopes.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

/* ================================================================ 值工具 */

LLVMValueRef Codegen::llv(const V& v) {
    if (v.ll != nullptr) return v.ll;
    if (!v.c) throw Unsupported("（内部）常量与指令都为空", 0);
    if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) return const_ptr(*v.c);
    return const_int(*v.c);
}

std::optional<uint64_t> Codegen::pat_const(const V& v) {
    /* 常量形态下按**源类型符号性**扩展后的 64 位内容（U-L7 的第一路）。 */
    if (!v.c) return std::nullopt;
    if (!is_int(*v.ty)) return *v.c & kMask64;
    uint64_t n = *v.c & mask_of(v.ty->bits);
    if (v.ty->is_signed && v.ty->bits < 64 && ((n >> (v.ty->bits - 1)) & 1)) {
        n |= ~mask_of(v.ty->bits) & kMask64;
    }
    return n & kMask64;
}

LLVMValueRef Codegen::pattern_of(const V& v) {
    /* `ext64`：常量按符号性 sext/zext，运行期恒等（本来就是 `zext(trunc_W)` 形态）。 */
    auto c = pat_const(v);
    if (c) return const_int(*c);
    return llv(v);
}

LLVMValueRef Codegen::mask_ll(LLVMValueRef ll, int bits) {
    if (bits >= 64) return ll;
    return LLVMBuildAnd(cur_fn().bd, ll, const_int(mask_of(bits)), "m");
}

V Codegen::convert(const V& v, const TyRef& target, int line) {
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
        if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) { /* 真指针 → 整数 */
            return V::instr(LLVMBuildPtrToInt(cur_fn().bd, llv(v), i64_, "pi"), target);
        }
        if (c) return V::konst(target, *c & mask_of(target->bits));
        return V::instr(mask_ll(llv(v), target->bits), target);
    }
    if (target->kind == TyKind::Str || target->kind == TyKind::Array) { /* 整数地址 → 真指针 */
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

LLVMValueRef Codegen::to_ptr(const V& v) {
    /* `Str`/`Arr`/槽本来就是 ptr，直接用 —— 这样全局/局部地址只会出 `lea rip+…`（REL32），
     * 不出绝对重定位。`Ptr`（i64 游戏地址）走 inttoptr。 */
    if (v.ty->kind == TyKind::Str || v.ty->kind == TyKind::Array) return llv(v);
    return LLVMBuildIntToPtr(cur_fn().bd, llv(v), ptr_, "p");
}

V Codegen::load_addr(LLVMValueRef p, const TyRef& ty) {
    LLVMTypeRef st = ll_type(*ty);
    LLVMValueRef inst = LLVMBuildLoad2(cur_fn().bd, st, p, "ld");
    LLVMSetAlignment(inst, 1); /* U-L1：地址是字节地址、无对齐要求，且不许插对齐检查 */
    if (!is_int(*ty)) return V::instr(inst, ty);
    LLVMValueRef v = inst;
    if (st != i64_) v = LLVMBuildZExt(cur_fn().bd, v, i64_, "z");
    if (ty->bits < 64) v = mask_ll(v, ty->bits);
    return V::instr(v, ty);
}

void Codegen::store_addr(LLVMValueRef p, const V& v, const TyRef& ty, int line) {
    if (!is_int(*ty)) {
        LLVMValueRef inst = LLVMBuildStore(cur_fn().bd, llv(v), p);
        LLVMSetAlignment(inst, 1);
        return;
    }
    V conv = convert(v, ty, line);
    LLVMTypeRef st = storage_ll(*ty);
    LLVMValueRef ll = llv(conv);
    if (st != i64_) ll = LLVMBuildTrunc(cur_fn().bd, ll, st, "t"); /* 窄写只写该宽度 */
    LLVMValueRef inst = LLVMBuildStore(cur_fn().bd, ll, p);
    LLVMSetAlignment(inst, 1);
}

std::pair<LLVMValueRef, TyRef> Codegen::global_slot(int global_id, int line) {
    if (global_id < 0 || global_id >= static_cast<int>(hir_.globals.size())) {
        throw Unsupported("（内部）全局下标越界", line);
    }
    const sema::HirGlobal& g = hir_.globals[static_cast<std::size_t>(global_id)];
    auto it = globals_.find(g.name);
    if (it != globals_.end()) return it->second;
    return add_global(g);
}

std::pair<LLVMValueRef, TyRef> Codegen::lvalue(const sema::HNode& node) {
    const int line = node.span.line;
    switch (node.kind) {
        case sema::HKind::Local: {
            const Binding* b = lookup(node.name);
            if (b == nullptr) throw Unsupported("（内部）作用域里没有 " + node.name, line);
            if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
                return {b->ptr, b->ty};
            }
            throw Unsupported("给常量 " + node.name + " 赋值", line);
        }
        case sema::HKind::Capture: {
            const int idx = node.capture_index;
            if (idx < 0 || idx >= static_cast<int>(cur_fn().captures.size())) {
                throw Unsupported("（内部）捕获序号越界", line);
            }
            const Binding& b = cur_fn().captures[static_cast<std::size_t>(idx)];
            return {b.ptr, b.ty};
        }
        case sema::HKind::Global:
            return global_slot(node.global_id, line);
        case sema::HKind::Index: {
            V base = e(node.kids[0]);
            if (base.ty->kind != TyKind::Array && base.ty->kind != TyKind::Str) {
                throw Unsupported("下标用在 " + std::string(ty_kind_name(base.ty->kind)) + " 上",
                                  line);
            }
            V idx = convert(e(node.kids[1]), ty_i64(), line);
            const int step = storage_bytes(*base.ty->elem); /* 地址是字节地址（U-L1） */
            LLVMValueRef off =
                LLVMBuildMul(cur_fn().bd, llv(idx), const_int(uint64_t(step)), "io");
            std::vector<LLVMValueRef> gi{off};
            LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, storage_ll(*make_int(8, false)),
                                           to_ptr(base), gi.data(), 1, "ea");
            return {p, base.ty->elem};
        }
        case sema::HKind::Member: {
            auto base = lvalue(node.kids[0]);
            const TyRef& bty = base.second;
            if (bty->kind != TyKind::Struct) {
                throw Unsupported("字段访问用在 " + std::string(ty_kind_name(bty->kind)) + " 上",
                                  line);
            }
            /* 偏移由 sema 算好（`Hir.structs`，唯一实现是 `sema/layout.cc`）。偏移 0 的字段
             * 直接复用基址 —— 现状就是这样省的这一层 GEP。 */
            if (node.offset == 0) return {base.first, node.ty};
            std::vector<LLVMValueRef> gi{const_int(static_cast<uint64_t>(node.offset))};
            LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, storage_ll(*make_int(8, false)), base.first,
                                           gi.data(), 1, "fa");
            return {p, node.ty};
        }
        default:
            throw Unsupported("左值形态 " + std::string(hkind_name(node.kind)), line);
    }
}

/* ================================================================ 语句 */

void Codegen::lower_block(const sema::HNode& block) {
    cur_fn().scopes.emplace_back();
    for (const sema::HNode& st : block.kids) lower_stmt(st);
    cur_fn().scopes.pop_back();
}

void Codegen::lower_stmt(const sema::HNode& st) {
    switch (st.kind) {
        case sema::HKind::VarDecl: s_VarDecl(st); break;
        case sema::HKind::Assign: s_Assign(st); break;
        case sema::HKind::ExprStmt: s_ExprStmt(st); break;
        case sema::HKind::If: s_If(st); break;
        case sema::HKind::While: s_While(st); break;
        case sema::HKind::Break: s_Break(st); break;
        case sema::HKind::Return: s_Return(st); break;
        case sema::HKind::Switch: s_Switch(st); break;
        /* 裸块 = 一层作用域（实测 `work/tmp/bareblock/b01_scope.si`）：`lower_block` 自带
         * scope push/pop，正是要的语义。 */
        case sema::HKind::Block: lower_block(st); break;
        default:
            throw Unsupported("语句形态 " + std::string(hkind_name(st.kind)), st.span.line);
    }
    /* 块已被终止符收口（return / break）→ 后续语句放进一个不可达块继续发。 */
    if (LLVMGetBasicBlockTerminator(cur_fn().block) != nullptr) new_block("dead");
}

void Codegen::s_VarDecl(const sema::HNode& st) {
    /* `let`/`const` 带编译期常量值的情况在 HIR 里**没有节点**（sema 把引用点直接折成 `Const`），
     * 所以这里只剩「开一个槽 + store」这一条路。 */
    V v = e(st.kids[0]);
    if (v.ty->kind == TyKind::Void) throw Unsupported(st.name + " 的初始化式没有值", st.span.line);
    LLVMValueRef a = alloca_slot(v.ty, st.name);
    store_addr(a, v, v.ty, st.span.line);
    bind(st.name, Binding::local(a, v.ty));
}

void Codegen::s_Assign(const sema::HNode& st) {
    auto target = lvalue(st.kids[0]);
    /* 复合赋值在 sema 里已经展成「读一次 + binop」；右侧也已经是左值类型（`Cast` 已插）。 */
    V val = convert(e(st.kids[1]), target.second, st.span.line);
    store_addr(target.first, val, target.second, st.span.line);
}

void Codegen::s_ExprStmt(const sema::HNode& st) { e(st.kids[0]); }

/* `elif` 链在 HIR 里是**嵌套**的 `If`（`If` 只有 3 个位），而现状的块结构是**扁平**的
 * （所有 case 共用同一个 `ifend`）。这里把嵌套摊回扁平：elti 那一支的 `Block` 是 sema 合成的
 * （只装里层 `If`、不声明任何名字），判据是「只有一个 kid、且它与 Block 同一行」。
 * 源码自己写的 `else { if … }` 通常占两行，因此不会被误摊平。 */
namespace {
bool is_elif_wrapper(const sema::HNode& n) {
    return n.kind == sema::HKind::Block && n.kids.size() == 1 &&
           n.kids[0].kind == sema::HKind::If && n.span.line == n.kids[0].span.line;
}
}  // namespace

void Codegen::s_If(const sema::HNode& st) {
    std::vector<const sema::HNode*> conds;
    std::vector<const sema::HNode*> blocks;
    const sema::HNode* node = &st;
    const sema::HNode* else_block = nullptr;
    for (;;) {
        conds.push_back(&node->kids[0]);
        blocks.push_back(&node->kids[1]);
        if (node->kids.size() < 3) break;
        const sema::HNode& rest = node->kids[2];
        if (is_elif_wrapper(rest)) {
            node = &rest.kids[0];
            continue;
        }
        else_block = &rest;
        break;
    }
    lower_if_runtime(conds, blocks, else_block);
}

void Codegen::lower_if_runtime(const std::vector<const sema::HNode*>& conds,
                               const std::vector<const sema::HNode*>& blocks,
                               const sema::HNode* else_block) {
    /* 所有分支跑完汇到同一个 cont 块（惰性创建）。 */
    LLVMBasicBlockRef cont = nullptr;
    auto join = [&]() {
        if (cont == nullptr) cont = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "ifend");
        LLVMBuildBr(cur_fn().bd, cont);
    };
    for (std::size_t i = 0; i < conds.size(); ++i) {
        LLVMValueRef cond = as_cond(e(*conds[i]));
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "then");
        LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "next");
        LLVMBuildCondBr(cur_fn().bd, cond, then_bb, next_bb);
        LLVMPositionBuilderAtEnd(cur_fn().bd, then_bb);
        cur_fn().block = then_bb;
        lower_block(*blocks[i]);
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

void Codegen::s_While(const sema::HNode& st) {
    LLVMBasicBlockRef head = br_new_block("loop");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "body");
    LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "exit");
    LLVMValueRef cond = as_cond(e(st.kids[0]));
    LLVMBuildCondBr(cur_fn().bd, cond, body_bb, exit_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, body_bb);
    cur_fn().block = body_bb;
    cur_fn().breaks.push_back({st.label, exit_bb});
    lower_block(st.kids[1]);
    cur_fn().breaks.pop_back();
    if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, exit_bb);
    cur_fn().block = exit_bb;
}

void Codegen::s_Break(const sema::HNode& st) {
    LLVMBasicBlockRef target = nullptr;
    for (auto it = cur_fn().breaks.rbegin(); it != cur_fn().breaks.rend(); ++it) {
        if (st.label.empty() || st.label == it->first) {
            target = it->second;
            break;
        }
    }
    if (target == nullptr) throw Unsupported("break " + st.label, st.span.line);
    LLVMBuildBr(cur_fn().bd, target);
}

void Codegen::s_Return(const sema::HNode& st) {
    if (st.kids.empty()) {
        if (cur_fn().ret_ty->kind != TyKind::Void) {
            throw Unsupported("非 None 函数里裸 return", st.span.line);
        }
        LLVMBuildRetVoid(cur_fn().bd);
        return;
    }
    if (cur_fn().ret_ty->kind == TyKind::Void) {
        throw Unsupported("None 函数里 return 值", st.span.line);
    }
    /* 返回值的转换已经是 kid 里的 `Cast` 节点（sema 的 `ret_stmt` 发）。 */
    LLVMBuildRet(cur_fn().bd, llv(e(st.kids[0])));
}

void Codegen::s_Switch(const sema::HNode& st) {
    V subj = e(st.kids[0]);
    /* `Str` 主体按**内容**派发（case 值就是字面量）。拿 `pat_const` 当整数比会在 IR 校验就报
     * `icmp eq ptr, i64` —— 语料里那些 `ui_set_*(\`table[{i}][{j}]\`, …)` 正是要靠内容命中。 */
    const bool str_subj = subj.ty->kind == TyKind::Str;
    LLVMValueRef subj_ll =
        str_subj ? nullptr : (subj.c ? const_int(pat_const(subj).value_or(0)) : subj.ll);
    /* HIR 的 kid 顺序：主体 + 「[值]? Block」逐 case，default 在所有 case 之后。 */
    std::vector<const sema::HNode*> values;
    std::vector<const sema::HNode*> blocks;
    const sema::HNode* default_block = nullptr;
    for (std::size_t i = 1; i < st.kids.size(); ++i) {
        if (st.kids[i].kind == sema::HKind::Block) {
            if (values.empty() || values.size() == blocks.size()) {
                default_block = &st.kids[i];
                continue;
            }
            blocks.push_back(&st.kids[i]);
            continue;
        }
        values.push_back(&st.kids[i]);
        if (i + 1 >= st.kids.size() || st.kids[i + 1].kind != sema::HKind::Block) {
            throw Unsupported("（内部）switch 的 case 值后面没有块", st.span.line);
        }
        blocks.push_back(&st.kids[i + 1]);
        ++i;
    }
    LLVMBasicBlockRef end = nullptr;
    auto join = [&]() {
        if (end == nullptr) end = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "swend");
        LLVMBuildBr(cur_fn().bd, end);
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        LLVMValueRef cnd = nullptr;
        if (str_subj) {
            V cvv = e(*values[i]);
            if (cvv.ty->kind != TyKind::Str) {
                throw Unsupported("Str 主体 switch 的 case 值不是 Str 常量", values[i]->span.line);
            }
            cnd = as_cond(str_compare(subj, cvv, values[i]->span.line, false));
        } else {
            auto cvv = pat_const(e(*values[i]));
            if (!cvv) throw Unsupported("switch case 的值不是常量", values[i]->span.line);
            cnd = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, subj_ll, const_int(*cvv), "sc");
        }
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "case");
        LLVMBasicBlockRef next_bb =
            LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "casenext");
        LLVMBuildCondBr(cur_fn().bd, cnd, then_bb, next_bb);
        LLVMPositionBuilderAtEnd(cur_fn().bd, then_bb);
        cur_fn().block = then_bb;
        lower_block(*blocks[i]);
        if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) join();
        LLVMPositionBuilderAtEnd(cur_fn().bd, next_bb);
        cur_fn().block = next_bb;
    }
    if (default_block != nullptr) {
        lower_block(*default_block);
        if (LLVMGetBasicBlockTerminator(cur_fn().block) == nullptr) join();
    } else if (end != nullptr) {
        LLVMBuildBr(cur_fn().bd, end);
    }
    if (end != nullptr) {
        LLVMPositionBuilderAtEnd(cur_fn().bd, end);
        cur_fn().block = end;
    }
}

/* ================================================================ 表达式 */

V Codegen::e(const sema::HNode& node) {
    switch (node.kind) {
        case sema::HKind::Const: return e_const(node);
        case sema::HKind::StrLit: return e_strlit(node);
        case sema::HKind::Local: return e_local(node);
        case sema::HKind::Global: return e_global(node);
        case sema::HKind::Capture: return e_capture(node);
        case sema::HKind::Unary: return e_unary(node);
        case sema::HKind::Binary: return e_binary(node);
        case sema::HKind::Cond: return e_cond(node);
        case sema::HKind::Call: return e_call(node);
        case sema::HKind::Member: return e_member(node);
        case sema::HKind::Index: return e_index(node);
        case sema::HKind::Cast: return e_cast(node);
        case sema::HKind::ArrayLit: return e_array_lit(node);
        case sema::HKind::CompositeLit: return e_composite_lit(node);
        case sema::HKind::Template: return e_template(node);
        default:
            throw Unsupported("表达式形态 " + std::string(hkind_name(node.kind)), node.span.line);
    }
}

V Codegen::e_const(const sema::HNode& node) {
    /* 整数常量：位模式已在 `const_bits`（决策 4），不发任何东西。 */
    if (node.const_id < 0) return V::konst(node.ty, node.const_bits);
    /* 非整数常量（串/数组/聚合）：值在池里，**一枚全局一个常量**。同一份 `const #` 引用 N 次
     * 必须发同一枚全局（旧路径按声明节点备忘：`root_const_values_`）；每次引用各发一枚会把
     * 产物变大，而且全局重名。 */
    const std::size_t id = static_cast<std::size_t>(node.const_id);
    if (id >= hir_.consts.size()) throw Unsupported("（内部）常量下标越界", node.span.line);
    auto hit = const_pool_.find(node.const_id);
    if (hit != const_pool_.end()) return hit->second;
    V v = e(hir_.consts[id].value);
    const_pool_[node.const_id] = v;
    return v;
}

V Codegen::e_strlit(const sema::HNode& node) {
    return string_literal(node.string_id, node.span.line);
}

V Codegen::e_local(const sema::HNode& node) {
    const Binding* b = lookup(node.name);
    if (b == nullptr) throw Unsupported("（内部）作用域里没有 " + node.name, node.span.line);
    if (b->kind == Binding::Kind::Local || b->kind == Binding::Kind::Capture) {
        return load_addr(b->ptr, b->ty);
    }
    return *b->value;
}

V Codegen::e_global(const sema::HNode& node) {
    auto g = global_slot(node.global_id, node.span.line);
    return load_addr(g.first, g.second);
}

V Codegen::e_capture(const sema::HNode& node) {
    const int idx = node.capture_index;
    if (idx < 0 || idx >= static_cast<int>(cur_fn().captures.size())) {
        throw Unsupported("（内部）捕获序号越界", node.span.line);
    }
    const Binding& b = cur_fn().captures[static_cast<std::size_t>(idx)];
    return load_addr(b.ptr, b.ty);
}

V Codegen::e_unary(const sema::HNode& node) {
    const V v = e(node.kids[0]);
    const int line = node.span.line;
    if (node.un_op == sema::HUnOp::Not) {
        /* `!`：非零为假的布尔化（`as_cond` 的反面），结果恒 `Bool`。 */
        LLVMValueRef cnd = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, llv(v), const_int(0), "n");
        return V::instr(LLVMBuildZExt(cur_fn().bd, cnd, i64_, "nb"), ty_bool());
    }
    if (!is_int(*v.ty)) {
        throw Unsupported("一元运算用在 " + std::string(ty_kind_name(v.ty->kind)), line);
    }
    LLVMValueRef ll = nullptr;
    if (node.un_op == sema::HUnOp::Neg) {
        ll = LLVMBuildSub(cur_fn().bd, const_int(0), llv(v), "neg");
    } else {
        ll = LLVMBuildXor(cur_fn().bd, llv(v), const_int(mask_of(v.ty->bits)), "bnot");
    }
    return V::instr(mask_ll(ll, v.ty->bits), v.ty);
}

V Codegen::e_binary(const sema::HNode& node) {
    switch (node.bin_op) {
        case sema::HBinOp::LogAnd:
        case sema::HBinOp::LogOr: return logical(node);
        case sema::HBinOp::Eq:
        case sema::HBinOp::Ne:
        case sema::HBinOp::Lt:
        case sema::HBinOp::Le:
        case sema::HBinOp::Gt:
        case sema::HBinOp::Ge: return compare(node);
        case sema::HBinOp::StrEq:
            return str_compare(e(node.kids[0]), e(node.kids[1]), node.span.line, false);
        case sema::HBinOp::StrNe:
            return str_compare(e(node.kids[0]), e(node.kids[1]), node.span.line, true);
        default: break;
    }
    return binop(node);
}

V Codegen::e_cond(const sema::HNode& node) {
    /* 形状是 **condbr + phi**，不是 `select`：两臂里都可能有 `load(…)`，`select` 会两支都求值
     * —— 与短路语义不一致。每个臂的类型转换已经在 sema 里做完（`Cast` 节点在各自的臂里）。 */
    LLVMValueRef cond = as_cond(e(node.kids[0]));
    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "t_then");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "t_else");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "t_end");
    LLVMBuildCondBr(cur_fn().bd, cond, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(cur_fn().bd, then_bb);
    cur_fn().block = then_bb;
    auto dt = eval_deferred(node.kids[1]);
    LLVMBasicBlockRef t_bb = cur_fn().block;
    LLVMPositionBuilderAtEnd(cur_fn().bd, else_bb);
    cur_fn().block = else_bb;
    auto de = eval_deferred(node.kids[2]);
    LLVMBasicBlockRef e_bb = cur_fn().block;

    const V& tv = dt.second;
    const V& ev = de.second;
    if (tv.ty->kind == TyKind::Void || ev.ty->kind == TyKind::Void) {
        throw Unsupported("三目的一支没有值", node.span.line);
    }
    const TyRef& ty = node.ty;
    /* 每个臂顶层的 `Cast` 必须在**它自己那个块**里发，而且是在两个臂都求值**之后**
     * ——旧 `convert` 就是这个顺序，号编也跟着它。 */
    LLVMPositionBuilderAtEnd(cur_fn().bd, t_bb);
    const V t_fin = settle(dt);
    LLVMBuildBr(cur_fn().bd, end_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, e_bb);
    const V e_fin = settle(de);
    LLVMBuildBr(cur_fn().bd, end_bb);

    LLVMPositionBuilderAtEnd(cur_fn().bd, end_bb);
    cur_fn().block = end_bb;
    /* phi 的结果类型是**值的类型**（窄整数一律 i64 位模式），不是内存宽度。 */
    LLVMValueRef phi = LLVMBuildPhi(cur_fn().bd, val_ll(*ty), "cnd");
    std::vector<LLVMValueRef> vals{llv(t_fin), llv(e_fin)};
    std::vector<LLVMBasicBlockRef> blks{t_bb, e_bb};
    LLVMAddIncoming(phi, vals.data(), blks.data(), 2);
    return V::instr(phi, ty);
}

V Codegen::e_call(const sema::HNode& node) {
    if (node.callee.kind == sema::HCalleeKind::Builtin) return call_builtin(node);
    return call_func(node);
}

V Codegen::e_member(const sema::HNode& node) {
    auto addr = lvalue(node);
    return load_addr(addr.first, addr.second);
}

V Codegen::e_index(const sema::HNode& node) {
    auto addr = lvalue(node);
    return load_addr(addr.first, addr.second);
}

V Codegen::e_cast(const sema::HNode& node) { return apply_cast(node, e(node.kids[0])); }

/* 隐式转换发指令的唯一落点（不变量 ④）。逐种与旧 `convert` 的分支一一对应 —— 值的表示是
 * 「i64 位模式 + 掩码」，所以 `trunc`/`zext` 都是**掩码**，不是 LLVM 的 `trunc`/`zext`。 */
V Codegen::apply_cast(const sema::HNode& node, V v) {
    const int line = node.span.line;
    switch (node.cast_kind) {
        case sema::HCastKind::Trunc:
        case sema::HCastKind::Zext:
            return V::instr(mask_ll(llv(v), node.ty->bits), node.ty);
        case sema::HCastKind::Sext: {
            /* sema 今天不产生 `Sext`（加宽只出 `Zext`：运行期窄值本来就是 `zext(trunc_W)` 的
             * 64 位位模式，按符号扩的那一路只属于编译期常量）。留着是为了这条 switch 覆盖全集。 */
            LLVMValueRef ll = sext64(llv(v), v.ty->bits);
            return V::instr(mask_ll(ll, node.ty->bits), node.ty);
        }
        case sema::HCastKind::PtrToInt:
            return V::instr(LLVMBuildPtrToInt(cur_fn().bd, llv(v), i64_, "pi"), node.ty);
        case sema::HCastKind::IntToPtr:
            return V::instr(to_ptr(v), node.ty);
        case sema::HCastKind::Raw:
            /* 整数目标也要掩码：同宽、只换符号性/名义位（`Bool` ↔ `U1`、`U8` ↔ `S8`）在 sema 里
             * 是 `Raw`，而旧 `convert` 对整数目标**无条件**补掩码 —— 漏了它就比旧路径少一道 `and`。 */
            if (is_int(*node.ty)) return V::instr(mask_ll(llv(v), node.ty->bits), node.ty);
            return V::instr(llv(v), node.ty);
    }
    throw Unsupported("转换形态", line);
}

/* 旧 `convert` 在二元运算里是「两个操作数**都**求值 → 各自转换」，HIR 把转换摆在 kid 的位置上
 * （`Cast` 节点）。直接在 kid 里发会把左侧的掩码插到右侧的 load 前面 —— 指令顺序变了，产物就
 * 不同。所以这里只剥 kid **顶层**那一个 `Cast`：先求它的操作数，两次求值都做完之后才 `settle`。 */
std::pair<const sema::HNode*, V> Codegen::eval_deferred(const sema::HNode& kid) {
    if (kid.kind != sema::HKind::Cast) return {nullptr, e(kid)};
    return {&kid, e(kid.kids[0])};
}

V Codegen::settle(const std::pair<const sema::HNode*, V>& d) {
    return d.first == nullptr ? d.second : apply_cast(*d.first, d.second);
}

V Codegen::e_array_lit(const sema::HNode& node) {
    /* 空数组（`Arr[T] []`）在 HIR 里是**没有 kid 的 ArrayLit**，类型在外层转换上。 */
    if (node.kids.empty()) return empty_array(node.ty, node.span.line);
    return array_literal(node);
}

V Codegen::e_composite_lit(const sema::HNode& node) { return composite_literal(node); }

V Codegen::e_template(const sema::HNode& node) { return template_expr(node); }

/* ================================================================ 运算 */

LLVMValueRef Codegen::as_cond(const V& v) {
    if (v.ty->kind == TyKind::Void) throw Unsupported("None 值当条件", 0);
    return LLVMBuildICmp(cur_fn().bd, LLVMIntNE, llv(v), const_int(0), "c");
}

LLVMValueRef Codegen::sext64(LLVMValueRef ll, int bits) {
    if (bits >= 64) return ll;
    const int sh = 64 - bits;
    return LLVMBuildAShr(
        cur_fn().bd, LLVMBuildShl(cur_fn().bd, ll, const_int(uint64_t(sh)), "sh"),
        const_int(uint64_t(sh)), "sa");
}

V Codegen::compare(const sema::HNode& node) {
    /* 两侧的转换是 kid 顶层的 `Cast` 节点（sema 的 `compare` 按 `unify` 发），比较的结果类型恒
     * `Bool`，所以类型直接读左操作数 —— 那正是旧 `unify` 的返回值。求值顺序照旧：**两个操作数
     * 先都求值**，之后才各自转换（见 `eval_deferred` 的注释）。 */
    const TyRef& ty = node.kids[0].ty;
    auto da = eval_deferred(node.kids[0]);
    auto db = eval_deferred(node.kids[1]);
    const V a = settle(da);
    const V b = settle(db);
    LLVMValueRef x = llv(a);
    LLVMValueRef y = llv(b);
    const bool sgn = is_int(*ty) && ty->is_signed;
    if (sgn && ty->bits < 64) {
        x = sext64(x, ty->bits);
        y = sext64(y, ty->bits);
    }
    LLVMValueRef cnd = LLVMBuildICmp(cur_fn().bd, cmp_pred(node.bin_op, sgn), x, y, "cmp");
    return V::instr(LLVMBuildZExt(cur_fn().bd, cnd, i64_, "cmpz"), ty_bool());
}

V Codegen::str_compare(const V& a, const V& b, int line, bool ne) {
    LLVMValueRef la = len_of(a, line);
    LLVMValueRef lb = len_of(b, line);
    LLVMValueRef same = LLVMBuildICmp(cur_fn().bd, LLVMIntEQ, la, lb, "sleq");
    /* 长度不等时把比较长度取 0 —— 否则逐字节会读越过短的那一条的 payload。 */
    LLVMValueRef n = LLVMBuildSelect(cur_fn().bd, same, la, const_int(0), "sln");
    LLVMValueRef eqv = emit_call(ensure_streq_fn(), {to_ptr(a), to_ptr(b), n});
    LLVMValueRef eq = LLVMBuildICmp(cur_fn().bd, LLVMIntNE, eqv, const_int(0), "seq");
    LLVMValueRef res = LLVMBuildAnd(cur_fn().bd, eq, same, "seqb");
    if (ne) res = LLVMBuildNot(cur_fn().bd, res, "snot");
    return V::instr(LLVMBuildZExt(cur_fn().bd, res, i64_, "seqz"), ty_bool());
}

V Codegen::logical(const sema::HNode& node) {
    /* `&&` / `||` 短路（`get_command` 读的是游戏内存，不能随意重排）。两侧**不** convert
     * （`as_cond` 只看非零），所以类型可以不同。 */
    const bool is_and = node.bin_op == sema::HBinOp::LogAnd;
    LLVMValueRef lhs = as_cond(e(node.kids[0]));
    LLVMBasicBlockRef lhs_bb = cur_fn().block;
    LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "rhs");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(ctx(), cur_fn().fnref, "lend");
    if (is_and) LLVMBuildCondBr(cur_fn().bd, lhs, rhs_bb, end_bb);
    else LLVMBuildCondBr(cur_fn().bd, lhs, end_bb, rhs_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, rhs_bb);
    cur_fn().block = rhs_bb;
    LLVMValueRef rhs = as_cond(e(node.kids[1]));
    LLVMBasicBlockRef rhs_end = cur_fn().block;
    LLVMBuildBr(cur_fn().bd, end_bb);
    LLVMPositionBuilderAtEnd(cur_fn().bd, end_bb);
    cur_fn().block = end_bb;
    LLVMTypeRef i1 = LLVMIntTypeInContext(ctx(), 1);
    LLVMValueRef phi = LLVMBuildPhi(cur_fn().bd, i1, "lg");
    std::vector<LLVMValueRef> vals{LLVMConstInt(i1, is_and ? 0 : 1, 0), rhs};
    std::vector<LLVMBasicBlockRef> blks{lhs_bb, rhs_end};
    LLVMAddIncoming(phi, vals.data(), blks.data(), 2);
    return V::instr(LLVMBuildZExt(cur_fn().bd, phi, i64_, "lgz"), ty_bool());
}

V Codegen::binop(const sema::HNode& node) {
    const char* op = arith_op_name(node.bin_op);
    const int line = node.span.line;
    const TyRef& ty = node.ty;
    /* 求值顺序照旧：两侧先都求值，再各自 convert（见 `compare` 的同一条注释）。 */
    /* 求值顺序照旧：两侧先都求值，再各自转换（见 `eval_deferred` 的注释）。 */
    auto da = eval_deferred(node.kids[0]);
    auto db = eval_deferred(node.kids[1]);
    const V a = settle(da);
    const V b = settle(db);
    if (ty->kind == TyKind::Str) {
        /* `+` 是唯一的 Str 运算（实测：`"ab" + "cd"` → 长度 4 的 `abcd`）。 */
        if (node.bin_op != sema::HBinOp::Add) throw Unsupported(std::string("Str 的 ") + op, line);
        return str_concat(a, b, line);
    }
    if (ty->kind == TyKind::Ptr) {
        if (node.bin_op != sema::HBinOp::Add && node.bin_op != sema::HBinOp::Sub) {
            throw Unsupported(std::string("指针 ") + op, line);
        }
        if (node.bin_op == sema::HBinOp::Add) {
            return V::instr(LLVMBuildAdd(cur_fn().bd, llv(a), llv(b), "pa"), ty);
        }
        return V::instr(LLVMBuildSub(cur_fn().bd, llv(a), llv(b), "pa"), ty);
    }
    const int bits = ty->bits;
    LLVMValueRef x = llv(a);
    LLVMValueRef y = llv(b);
    if (node.bin_op == sema::HBinOp::Div || node.bin_op == sema::HBinOp::Rem) {
        /* 运行期结果**不**截断到 W（facts.md §`/`、`%` 的第二条），所以这里不套掩码。 */
        const std::string kind(node.bin_op == sema::HBinOp::Div ? "/" : "%");
        return V::instr(div(kind, x, y, *ty), ty);
    }
    if (node.bin_op == sema::HBinOp::Shl) {
        LLVMValueRef amt = LLVMBuildAnd(cur_fn().bd, y, const_int(uint64_t(bits - 1)), "amt");
        return V::instr(mask_ll(LLVMBuildShl(cur_fn().bd, x, amt, "shl"), bits), ty);
    }
    if (node.bin_op == sema::HBinOp::Shr) {
        /* U-L8：**永远 lshr**（不是 ashr），左操作数走 `pattern_of`（常量按符号性扩），再截回 W。 */
        LLVMValueRef amt = LLVMBuildAnd(cur_fn().bd, y, const_int(uint64_t(bits - 1)), "amt");
        LLVMValueRef ll = LLVMBuildLShr(cur_fn().bd, pattern_of(a), amt, "shr");
        return V::instr(mask_ll(ll, bits), ty);
    }
    LLVMValueRef r = nullptr;
    switch (node.bin_op) {
        case sema::HBinOp::Add: r = LLVMBuildAdd(cur_fn().bd, x, y, "op"); break;
        case sema::HBinOp::Sub: r = LLVMBuildSub(cur_fn().bd, x, y, "op"); break;
        case sema::HBinOp::Mul: r = LLVMBuildMul(cur_fn().bd, x, y, "op"); break;
        case sema::HBinOp::BitAnd: r = LLVMBuildAnd(cur_fn().bd, x, y, "op"); break;
        case sema::HBinOp::BitOr: r = LLVMBuildOr(cur_fn().bd, x, y, "op"); break;
        case sema::HBinOp::BitXor: r = LLVMBuildXor(cur_fn().bd, x, y, "op"); break;
        default: throw Unsupported(std::string("运算符 ") + op, line);
    }
    return V::instr(mask_ll(r, bits), ty);
}

LLVMValueRef Codegen::div(const std::string& op, LLVMValueRef x, LLVMValueRef y, const Ty& ty) {
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
        raw = ty.is_signed ? LLVMBuildSDiv(cur_fn().bd, x, safe, "q")
                           : LLVMBuildUDiv(cur_fn().bd, x, safe, "q");
    } else {
        raw = ty.is_signed ? LLVMBuildSRem(cur_fn().bd, x, safe, "r")
                           : LLVMBuildURem(cur_fn().bd, x, safe, "r");
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

/* ================================================================ 调用 */

namespace {

/* 嵌套 def 的判定。sema 的实例键（`sema/analyze.cc` 的 `lex_of`）对嵌套 def 是
 * `归属|名字@声明行[|捕获签名]`；顶层 def 的键要么是裸名字、要么是 `名字$<实例串>[|捕获签名]`。
 * 判据因此是「用 `|` 切开后有一节恰好是 `名字@<数字>`」：顶层键里那一节只可能是 `<名字>$…`
 * （带 `$`）或者捕获签名（`名字:类型`，带 `:`）—— 泛型实参串里会有 `@`（`@u8`），但那个 `@`
 * 在 `$` 后面、不在单独一节里，所以不会误判。 */
bool hir_func_is_nested(const sema::HirFunc& f) {
    const std::string want = f.name + "@";
    std::size_t start = 0;
    while (start <= f.key.size()) {
        std::size_t end = f.key.find('|', start);
        if (end == std::string::npos) end = f.key.size();
        const std::string part = f.key.substr(start, end - start);
        if (part.size() > want.size() && part.compare(0, want.size(), want) == 0 &&
            part.find_first_not_of("0123456789", want.size()) == std::string::npos) {
            return true;
        }
        if (end == f.key.size()) break;
        start = end + 1;
    }
    return false;
}

}  // namespace

LLVMValueRef Codegen::emit_call(LLVMValueRef fnref, const std::vector<LLVMValueRef>& args) {
    std::vector<LLVMValueRef> a = args;
    return LLVMBuildCall2(cur_fn().bd, LLVMGlobalGetValueType(fnref), fnref, a.data(),
                          static_cast<unsigned>(a.size()), "");
}

/* 符号名（方案 §2b 补漏 1：sema 只给纯 def 名 + 四个事实，产物名由这里派生）。旧路径三处
 * （`lower.cc:2301` 的 `ensure_def_fn` / `2356` 的 `ensure_nested_fn` / `3121` 的 `tc_main`）逐字复现：
 *   程序根：`tc_main`；
 *   顶层实例：`name`（`line_suffix` 时 `name + "#L" + decl_line`，`#L` 插在 `$sig` 前），
 *             再挂 `$inst_sig`（只泛型有）、最后挂 `$c<cap_index>`；
 *   嵌套 def：`owner + "$" + name + "#L" + decl_line`，`owner` 是**调用点**当前函数的逻辑名
 *             （不在 def 实例里时是 `closure`）—— 不是声明处的词法归属。
 * 「逻辑名」与产物名是两个东西（旧 `FnCtx.name` vs `LLVMAddFunction` 的那个）：顶层实例只有在
 * 有捕获或泛型 sig 时逻辑名才等于产物名，否则就是纯 def 名；嵌套实例的逻辑名是 `owner$name`，
 * 不带 `#L` 段。再往里一层的 `owner` 取的就是这个逻辑名。 */
std::string Codegen::nested_owner() const {
    return cur_fn().is_top_def ? cur_fn().name : std::string("closure");
}

std::string Codegen::symbol_of(const sema::HirFunc& f) const {
    if (hir_func_is_nested(f)) {
        return nested_owner() + "$" + f.name + "#L" + std::to_string(f.decl_line);
    }
    std::string name = f.name;
    if (f.line_suffix) name += "#L" + std::to_string(f.decl_line);
    if (!f.inst_sig.empty()) name += "$" + f.inst_sig;
    if (f.cap_index >= 0) name += "$c" + std::to_string(f.cap_index);
    return name;
}

std::string Codegen::logical_name_of(const sema::HirFunc& f) const {
    if (hir_func_is_nested(f)) return nested_owner() + "$" + f.name;
    /* 旧 `f.name = sig.empty() ? name : gname`：捕获签名与泛型 sig 都为空时才是纯 def 名。 */
    return f.cap_index >= 0 || !f.inst_sig.empty() ? symbol_of(f) : f.name;
}

LLVMValueRef Codegen::ensure_func(int index) {
    const std::size_t idx = static_cast<std::size_t>(index);
    const sema::HirFunc& f = hir_.funcs[idx];
    const bool nested = hir_func_is_nested(f);
    /* 旧路径的实例键：顶层 = 实例本身（sema 已把 `$sig` / 捕获签名收口成下标）；
     * 嵌套 def = **(调用点当前函数, 声明位置)** —— 同一个嵌套 def 被两个不同外层调用要各发一份
     * （`lower.cc:2347`）。合并它们会让产物比参考小（实测少 64/256/768 字节）。
     * 键用**调用点的逻辑名**而不是产物名的 owner 段（旧 `cur_fn().name`）：两者在「不在 def 实例
     * 里」时不同（各自是 `closure`），那时旧路径也会给每个调用点各发一份。 */
    const auto nested_key = std::make_pair(cur_fn().name, index);
    if (!nested) {
        if (fnrefs_[idx] != nullptr) return fnrefs_[idx];
    } else {
        auto hit = nested_fnrefs_.find(nested_key);
        if (hit != nested_fnrefs_.end()) return hit->second;
    }
    std::vector<LLVMTypeRef> ptypes;
    for (const sema::HParam& p : f.params) ptypes.push_back(val_ll(*p.ty));
    for (std::size_t j = 0; j < f.caps.size(); ++j) ptypes.push_back(ptr_);
    LLVMTypeRef rty = f.ret->kind == TyKind::Void ? void_ty_ : val_ll(*f.ret);
    const std::string gname = symbol_of(f);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), gname.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    LLVMSetLinkage(fnref, LLVMInternalLinkage);
    if (nested) nested_fnrefs_[nested_key] = fnref;
    else fnrefs_[idx] = fnref;
    fn_seen_++;
    FnCtx& fc = begin_function(fnref, f.ret);
    fc.name = logical_name_of(f);
    /* 嵌套 def 身份**沿用外层**（旧 `begin_function` 的 `child->def = outer.def`）：旧路径里
     * `def` 不是 null 才用逻辑名当 owner，而它正是「词法上在某个顶层实例体内」。 */
    if (!nested) fc.is_top_def = true;
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        const std::string& pname = f.params[i].name;
        const TyRef& ty = f.params[i].ty;
        LLVMValueRef a = alloca_slot(ty, pname);
        store_addr(a, V::instr(LLVMGetParam(fnref, static_cast<unsigned>(i)), ty), ty,
                   f.body.span.line);
        bind_root_scope(pname, Binding::local(a, ty));
    }
    fc.captures.clear();
    for (std::size_t j = 0; j < f.caps.size(); ++j) {
        /* 捕获只传**指针**（按引用），不复制内存 —— 赋值/读回都落在调用者那块栈槽上。 */
        Binding b = Binding::capture(LLVMGetParam(fnref, static_cast<unsigned>(f.params.size() + j)),
                                     f.caps[j].ty);
        fc.captures.push_back(b);
        bind_root_scope(f.caps[j].name, b);
    }
    lower_block(f.body);
    finish_function();
    return fnref;
}

V Codegen::call_func(const sema::HNode& node) {
    const int idx = node.callee.index;
    if (idx < 0 || idx >= static_cast<int>(hir_.funcs.size())) {
        throw Unsupported("（内部）被调函数下标越界", node.span.line);
    }
    const sema::HirFunc& f = hir_.funcs[static_cast<std::size_t>(idx)];
    /* 前缀（stdlib）里的 5 个 def：产物里用 codegen 自己的优化实现（`body_*`），**不编译它的体**
     * （方案 §2f）—— 与改前 `call_named` 按名字分流一致。 */
    if (f.pre_body != sema::HPreBody::None) return call_prefix(f, node);
    /* 先建（含当场发它的体）再求实参：顺序决定模块里函数的排列与 `$c<n>` 的编号，改不得。 */
    LLVMValueRef fnref = ensure_func(idx);
    std::vector<LLVMValueRef> vals;
    for (std::size_t i = 0; i < node.kids.size(); ++i) {
        /* 实参已经是形参类型（sema 在调用点发了 `Cast`；捕获指针另算，见下）。 */
        const V arg = e(node.kids[i]);
        vals.push_back(arg.ty->kind == TyKind::Struct ? arg.ll : llv(arg));
    }
    /* 捕获指针按 `HirFunc.caps` 的顺序追加在实参之后，取**调用点**同名绑定的地址。 */
    for (const sema::HCapture& c : f.caps) {
        const Binding* b = lookup(c.name);
        if (b == nullptr || b->ptr == nullptr) {
            throw Unsupported("（内部）捕获 " + c.name + " 在调用点没有地址", node.span.line);
        }
        vals.push_back(b->ptr);
    }
    LLVMValueRef ll = emit_call(fnref, vals);
    if (f.ret->kind == TyKind::Void) return V::instr(nullptr, ty_void());
    return V::instr(ll, f.ret);
}

V Codegen::call_builtin(const sema::HNode& node) {
    switch (node.callee.index) {
        case kBuiltinLoad: return builtin_load(node);
        case kBuiltinStore: return builtin_store(node);
        case kBuiltinRawCast: return builtin_raw_cast(node);
        case kBuiltinAsr: return builtin_asr(node);
        case kBuiltinClz: return builtin_clz(node);
        case kBuiltinRor: return builtin_ror(node);
        case kBuiltinMin: return builtin_minmax(node, true);
        case kBuiltinMax: return builtin_minmax(node, false);
        case kBuiltinArray: return builtin_array(node);
        case kBuiltinRandom: return builtin_random(node);
        case kBuiltinLen: return builtin_len(node);
        case kBuiltinMs: return builtin_ms(node);
        default: break;
    }
    throw Unsupported("内建编号 " + std::to_string(node.callee.index), node.span.line);
}

V Codegen::builtin_load(const sema::HNode& node) {
    /* `load(<T>, p)`：结果类型就是 `Call.ty`（sema 已定），实参唯一。 */
    if (node.kids.size() != 1) throw Unsupported("load(<T>, p) 的形态", node.span.line);
    return load_addr(to_ptr(e(node.kids[0])), node.ty);
}

V Codegen::builtin_store(const sema::HNode& node) {
    if (node.kids.size() != 2) throw Unsupported("store(p, v) 的形态", node.span.line);
    LLVMValueRef addr = to_ptr(e(node.kids[0]));
    V val = e(node.kids[1]);
    if (val.ty->kind == TyKind::Void) throw Unsupported("store 的值是 None", node.span.line);
    store_addr(addr, val, val.ty, node.span.line);
    return V::instr(nullptr, ty_void());
}

V Codegen::builtin_raw_cast(const sema::HNode& node) {
    /* sema 已把它折成 `Cast{Raw}` 或 `Const`；这条只在直接构造 HIR 时才会走到。 */
    if (node.kids.size() != 1) throw Unsupported("raw_cast(<T>, v) 的形态", node.span.line);
    return V::instr(llv(e(node.kids[0])), node.ty);
}

V Codegen::builtin_asr(const sema::HNode& node) {
    /* `asr`：按 W 位符号位算术右移，与 `>>` 不同（U-L8 指示 3）：n ≥ W 时**饱和**。 */
    if (node.kids.size() != 2) throw Unsupported("asr(x, n) 的形态", node.span.line);
    const int line = node.span.line;
    const V x = e(node.kids[0]);
    if (!is_int(*x.ty)) {
        throw Unsupported(std::string("asr 用在 ") + ty_kind_name(x.ty->kind), line);
    }
    const int bits = x.ty->bits;
    /* 常量在 sema 折好（决策 4）；移位量的位模式由 sema 的 `Cast` 转好（旧 `convert` 同）。 */
    const V n = e(node.kids[1]);
    LLVMValueRef amt = llv(n);
    if (bits == 64) {
        amt = LLVMBuildAnd(cur_fn().bd, amt, const_int(63), "amt");
    } else {
        amt = LLVMBuildSelect(
            cur_fn().bd, LLVMBuildICmp(cur_fn().bd, LLVMIntULT, amt, const_int(uint64_t(bits)), "lt"),
            amt, const_int(uint64_t(bits - 1)), "sat");
    }
    LLVMValueRef ll = LLVMBuildAShr(cur_fn().bd, sext64(llv(x), bits), amt, "asr");
    return V::instr(mask_ll(ll, bits), x.ty);
}

LLVMValueRef Codegen::ensure_clz_fn() {
    auto hit = helpers_.find("ctlz");
    if (hit != helpers_.end()) return hit->second;
    /* i1 那个参数是 `is_zero_undef`，传 false：clz(0) 给 64。 */
    LLVMTypeRef params[2] = {i64_, LLVMInt1TypeInContext(ctx())};
    LLVMValueRef fn =
        LLVMAddFunction(module(), "llvm.ctlz.i64", LLVMFunctionType(i64_, params, 2, 0));
    helpers_["ctlz"] = fn;
    return fn;
}

V Codegen::builtin_clz(const sema::HNode& node) {
    /* 数前导零的范围是**操作数声明的位宽**，不是 64：137 的注释把 `Int` 操作数钉在 64
     * （`clz(input.input)` 的 `input.input` 是 U8 → 数 8 位里的前导零）。所以用 i64 的 ctlz
     * 再减去「64 - 位宽」——不必为每种位宽各声明一条 intrinsic。 */
    if (node.kids.size() != 1) throw Unsupported("clz(x) 的形态", node.span.line);
    V x = e(node.kids[0]);
    if (!is_int(*x.ty)) {
        throw Unsupported(std::string("clz 用在 ") + ty_kind_name(x.ty->kind), node.span.line);
    }
    const int bits = x.ty->bits;
    LLVMValueRef v = mask_ll(llv(x), bits);
    /* 第二个参数是 i1（`const_int` 给的是 i64，直接传会撞「参数类型不匹配」）。 */
    LLVMValueRef zero = LLVMConstInt(LLVMInt1TypeInContext(ctx()), 0, 0);
    LLVMValueRef call = emit_call(ensure_clz_fn(), {v, zero});
    if (bits == 64) return V::instr(call, x.ty);
    return V::instr(LLVMBuildSub(cur_fn().bd, call, const_int(uint64_t(64 - bits)), "clz"), x.ty);
}

V Codegen::builtin_ror(const sema::HNode& node) {
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
    if (node.kids.size() != 2) throw Unsupported("ror(x, n) 的形态", node.span.line);
    const int line = node.span.line;
    V x = e(node.kids[0]);
    V n = e(node.kids[1]);
    if (!is_int(*x.ty)) {
        throw Unsupported(std::string("ror 用在 ") + ty_kind_name(x.ty->kind), line);
    }
    if (!is_int(*n.ty)) {
        throw Unsupported(std::string("ror 的旋转量是 ") + ty_kind_name(n.ty->kind), line);
    }
    const int bits = x.ty->bits;
    /* 「两个操作数都是常量」那一路（64 位旋转）在 sema 折好；这里只剩按 W 位宽算的运行期那一路。 */
    const uint64_t sm = uint64_t(bits - 1);
    LLVMValueRef v = mask_ll(llv(x), bits);
    LLVMValueRef amt = LLVMBuildAnd(cur_fn().bd, llv(n), const_int(sm), "amt");
    LLVMValueRef inv = LLVMBuildAnd(
        cur_fn().bd, LLVMBuildSub(cur_fn().bd, const_int(uint64_t(bits)), amt, "amtc"),
        const_int(sm), "amti");
    LLVMValueRef lo = LLVMBuildLShr(cur_fn().bd, v, amt, "rr");
    LLVMValueRef hi = LLVMBuildShl(cur_fn().bd, v, inv, "rl");
    return V::instr(mask_ll(LLVMBuildOr(cur_fn().bd, lo, hi, "ror"), bits), x.ty);
}

V Codegen::builtin_minmax(const sema::HNode& node, bool is_min) {
    if (node.kids.size() != 2) throw Unsupported("min/max 的形态", node.span.line);
    const TyRef& ty = node.ty;
    /* 两侧已在 sema `unify` + `convert`（常量对比也在那里折好），这里只剩运行期的 `select`。
     * 转换的求值位置照旧是**逐个紧跟操作数**（`min`/`max` 的旧实现与 `binop` 不同，它每求一个
     * 操作数就当场转），所以这里直接在 kid 里发 `Cast`，不用 `eval_deferred`。 */
    const V a = e(node.kids[0]);
    const V b = e(node.kids[1]);
    LLVMIntPredicate pred = LLVMIntEQ;
    if (is_min) pred = ty->is_signed ? LLVMIntSLT : LLVMIntULT;
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

V Codegen::builtin_array(const sema::HNode& node) {
    /* `array(<T>, n)`：参考内建，返回定长数组。语义实测（`docs/notes/2026-10-18-array.md`）：
     *   · 值是**指向载荷的指针**，载荷前 8 字节是 Nim 头，`头 >> 16 == n`（与 `.len()` 的读法一致）；
     *   · 载荷**零初始化**，元素按 U-L1 的字节步长紧密排列；
     *   · 每个调用点得到**自己的**缓冲区（参考在堆上分配，首址固定）。
     * 我们把它做成**每个调用点一枚私有全局**（头 + 零载荷）：语料里 `array()` 都在「建表」函数里
     * （每次运行只调一次），所以加载期的零初始化等价于参考的每次分配零初始化。 */
    if (node.kids.size() != 1 || node.ty->kind != TyKind::Array) {
        throw Unsupported("array(<T>, n) 的形态", node.span.line);
    }
    const TyRef& elem = node.ty->elem;
    auto count_opt = pat_const(e(node.kids[0]));
    if (!count_opt) throw Unsupported("array 的长度不是常量", node.span.line);
    const uint64_t count = *count_opt;
    if (count > (uint64_t{1} << 32)) {
        throw Unsupported("array 的长度 " + std::to_string(count) + " 太大", node.span.line);
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
    const std::string gname = ".array" + std::to_string(node.span.line) + "_" +
                              std::to_string(array_serial_++);
    LLVMValueRef g = LLVMAddGlobal(module(), arrty, gname.c_str());
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetInitializer(g, LLVMConstArray2(i8, elems.data(), elems.size()));
    std::vector<LLVMValueRef> gi{const_int(0), const_int(8)};
    return V::instr(LLVMConstGEP2(arrty, g, gi.data(), 2), make_array(elem));
}

V Codegen::empty_array(const TyRef& arr_ty, int line) {
    /* `Arr[T] []`：**空**数组字面量自己不带元素类型（HIR 的 `ArrayLit` 没有 kid，类型在 `ty` 上），
     * 所以只能从外层转换处进来。表示与 `array(<T>, 0)` 同一套：8 字节零头（`len = 头 >> 16 = 0`）
     * + 空载荷，也就是一个指向它的私有常量全局。空数组写不进去，所以不需要按调用点分开。 */
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

V Codegen::builtin_random(const sema::HNode& node) {
    /* `random` 定义在前缀里（`simplexc/corpus/stdlib_prefix.si`:1464–1495），98 份语料里 32 份在用，
     * 但它只要「一个 8 字节全局 + 4 行算术」——不必把前缀整体搬进来。
     * 实测（器材 `work/tmp/randomtask/probe_seq.py`）：三步 xorshift 是**顺序赋值**，再乘常数
     * `0x2545F4914F6CDD1`（只有 15 位，别换成教科书的 16 位那个）；返回新 state 的全 64 位；
     * `random(n) = U64(state) % U64(n)`；种子就是前缀的 `var global_seed = Seed 1`，与源码写
     * `global_seed = Seed (…)` 的是**同一份**全局。 */
    if (node.kids.empty() || node.kids.size() > 2) throw Unsupported("random 的参数个数", node.span.line);
    if (node.kids[0].kind != sema::HKind::Global) {
        throw Unsupported("（内部）random 的第一枚操作数不是种子全局", node.span.line);
    }
    const auto sg = global_slot(node.kids[0].global_id, node.span.line);
    const TyRef& ty = sg.second;
    LLVMValueRef old = llv(load_addr(sg.first, ty));
    LLVMValueRef x =
        LLVMBuildXor(cur_fn().bd, old, LLVMBuildShl(cur_fn().bd, old, const_int(12), "l"), "x1");
    x = LLVMBuildXor(cur_fn().bd, x, LLVMBuildLShr(cur_fn().bd, x, const_int(25), "s"), "x2");
    x = LLVMBuildXor(cur_fn().bd, x, LLVMBuildLShr(cur_fn().bd, x, const_int(27), "s"), "x3");
    LLVMValueRef next = LLVMBuildMul(cur_fn().bd, x, const_int(kSeedMul), "seed");
    store_addr(sg.first, V::instr(next, ty), ty, node.span.line);
    V out = V::instr(next, ty);
    if (node.kids.size() == 1) return out;
    /* `%` 口径照抄 `div`（零除数不 trap，余数 = 被除数）：`random(0)` 返回被除数。 */
    V m = e(node.kids[1]);
    return V::instr(div("%", llv(out), llv(m), *ty), ty);
}

/* `.len()`（内建 10）：**读 `p[-8]` 的头**（`len_of`），不是 inttoptr/ptrtoint 算术 —— 换成算术
 * 之后 IR 形状就变了，产物逐字节相同这条主判据就没有保证了（方案 §2f 追加段）。 */
V Codegen::builtin_len(const sema::HNode& node) {
    if (node.kids.size() != 1) throw Unsupported("len(obj) 的形态", node.span.line);
    V obj = e(node.kids[0]);
    if (obj.ty->kind != TyKind::Str && obj.ty->kind != TyKind::Array) {
        throw Unsupported(".len() 用在 " + std::string(ty_kind_name(obj.ty->kind)), node.span.line);
    }
    return V::instr(len_of(obj, node.span.line), ty_i64());
}

/* `.ms()`（内建 11）：毫秒 → `Time`（纳秒）。常量折在**这里**（`pat_const` 是「按源类型符号性
 * 扩过的 64 位值」，折法只该有一处），运行期是一乘。 */
V Codegen::builtin_ms(const sema::HNode& node) {
    if (node.kids.size() != 1) throw Unsupported("ms(obj) 的形态", node.span.line);
    V obj = e(node.kids[0]);
    if (!is_int(*obj.ty)) {
        throw Unsupported(".ms() 用在 " + std::string(ty_kind_name(obj.ty->kind)), node.span.line);
    }
    auto c = pat_const(obj);
    if (c) return V::konst(ty_time(), (*c * kNsPerMs) & kMask64);
    return V::instr(LLVMBuildMul(cur_fn().bd, llv(obj), const_int(kNsPerMs), "ns"), ty_time());
}

V Codegen::call_prefix(const sema::HirFunc& f, const sema::HNode& node) {
    /* 签名取 `HirFunc` 自己的形参表（sema 照 `lower.cc` 的 `call_prefix` 给死，见 `pre_body` 的
     * 注释）；实参在 sema 里已经 `convert` 到形参类型，所以这里只取位模式。
     * `thread_exit` 是**外部符号**（装配器造桩），其余四个是产物内联的优化实现。 */
    if (f.pre_body == sema::HPreBody::ThreadExit) {
        if (!node.kids.empty()) throw Unsupported("thread_exit 的参数个数", node.span.line);
        LLVMValueRef fn = get_extern(f.name, f.ret, {});
        emit_call(fn, {});
        return V::instr(nullptr, ty_void());
    }
    if (node.kids.size() != f.params.size()) {
        throw Unsupported(f.name + "(…) 的参数个数", node.span.line);
    }
    std::vector<TyRef> param_tys;
    for (const sema::HParam& p : f.params) param_tys.push_back(p.ty);
    LLVMValueRef fn = ensure_prefix_fn(f.name, f.ret, param_tys);
    std::vector<LLVMValueRef> vals;
    vals.reserve(node.kids.size());
    for (const sema::HNode& kid : node.kids) vals.push_back(llv(e(kid)));
    LLVMValueRef ll = emit_call(fn, vals);
    if (f.ret->kind == TyKind::Void) return V::instr(nullptr, ty_void());
    return V::instr(ll, f.ret);
}

LLVMValueRef Codegen::get_extern(const std::string& name, const TyRef& ret_ty,
                                 const std::vector<TyRef>& param_tys) {
    /* 只声明、不定义的外部符号（装配器给它造桩）。签名按**目标 ABI** 给：`Ptr` 参数就是真指针
     * （我们内部函数才用 i64 传地址）。 */
    const std::string key = "extern:" + name;
    auto hit = fns_.find(key);
    if (hit != fns_.end()) return hit->second;
    std::vector<LLVMTypeRef> ptypes;
    for (const auto& t : param_tys) {
        ptypes.push_back(t->kind == TyKind::Ptr ? ptr_ : ll_type(*t));
    }
    LLVMTypeRef rty = ret_ty->kind == TyKind::Void ? void_ty_ : ll_type(*ret_ty);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), name.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    fns_[key] = fnref;
    return fnref;
}

void Codegen::body_get_time() {
    LLVMValueRef slot = alloca_slot(make_int(64, false), "ft");
    LLVMValueRef ext = get_extern("GetSystemTimePreciseAsFileTime", ty_void(), {ty_ptr()});
    emit_call(ext, {slot});
    V ft = load_addr(slot, make_int(64, false));
    LLVMValueRef d = LLVMBuildSub(cur_fn().bd, llv(ft), const_int(kFileTimeUnix), "d");
    LLVMBuildRet(cur_fn().bd, LLVMBuildMul(cur_fn().bd, d, const_int(100), "ns"));
}

void Codegen::body_sleep() {
    LLVMValueRef ms =
        LLVMBuildUDiv(cur_fn().bd, LLVMGetParam(cur_fn().fnref, 0), const_int(kNsPerMs), "ms");
    LLVMValueRef ext = get_extern("Sleep", ty_void(), {make_int(32, false)});
    emit_call(ext, {LLVMBuildTrunc(cur_fn().bd, ms, storage_ll(*make_int(32, false)), "m")});
    LLVMBuildRetVoid(cur_fn().bd);
}

LLVMValueRef Codegen::byte_loop_prologue(unsigned n_param, LLVMBasicBlockRef* head_out,
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

void Codegen::body_memory_clear() {
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
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, llv(i), const_int(1), "i1"), ty_i64()),
               ty_i64(), 0);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    LLVMBuildRetVoid(cur_fn().bd);
}

void Codegen::body_memory_copy() {
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
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, llv(i), const_int(1), "i1"), ty_i64()),
               ty_i64(), 0);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    LLVMBuildRetVoid(cur_fn().bd);
}

LLVMValueRef Codegen::ensure_prefix_fn(const std::string& name, const TyRef& ret_ty,
                                       const std::vector<TyRef>& param_tys) {
    const std::string key = "prefix:" + name;
    auto hit = fns_.find(key);
    if (hit != fns_.end()) return hit->second;
    std::vector<LLVMTypeRef> ptypes;
    for (const auto& t : param_tys) ptypes.push_back(val_ll(*t));
    LLVMTypeRef rty = ret_ty->kind == TyKind::Void ? void_ty_ : val_ll(*ret_ty);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), name.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    LLVMSetLinkage(fnref, LLVMInternalLinkage);
    fns_[key] = fnref;
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

/* ================================================================ 模板插值与串 */
/* 渲染语义**全部实测**（参考编译器 + 差分宿主，见 docs/notes/2026-10-18-str-interp.md）：
 *   `Int` / `S*` → 有符号十进制（按声明位宽符号扩展）；`U*` → 无符号十进制；`Bool` → `true`/`false`；
 *   `Str` → 原样字节；`Ptr` → 地址的无符号十进制。
 * 形状：各插值段先求值**一次** → 按最坏尺寸在栈上开缓冲 → 顺序写入 → 按实际游标回填
 * `p[-8] = len << 16`。十进制转换与拷贝都是**产物内联**的（`__simplexc_dec` / `__simplexc_copy`）：
 * 参考产物那边 `extern` 也是空的，不许引出外部符号。 */

LLVMValueRef Codegen::len_of(const V& obj, int line) {
    if (obj.ty->kind != TyKind::Str && obj.ty->kind != TyKind::Array) {
        throw Unsupported("长度用在 " + std::string(ty_kind_name(obj.ty->kind)), line);
    }
    std::vector<LLVMValueRef> gi{const_int(static_cast<uint64_t>(-8))};
    LLVMValueRef lp = LLVMBuildGEP2(cur_fn().bd, storage_ll(*make_int(8, false)), to_ptr(obj),
                                    gi.data(), 1, "lp");
    V raw = load_addr(lp, make_int(64, false));
    return LLVMBuildLShr(cur_fn().bd, llv(raw), const_int(16), "len");
}

V Codegen::str_concat(const V& a, const V& b, int line) {
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMValueRef la = len_of(a, line);
    LLVMValueRef lb = len_of(b, line);
    LLVMValueRef total = LLVMBuildAdd(cur_fn().bd, la, lb, "sl");
    LLVMValueRef buf =
        LLVMBuildArrayAlloca(cur_fn().bd, i8,
                             LLVMBuildAdd(cur_fn().bd, total, const_int(8), "sb"), "sbuf");
    LLVMSetAlignment(buf, 8);
    std::vector<LLVMValueRef> g8{const_int(8)};
    LLVMValueRef payload = LLVMBuildGEP2(cur_fn().bd, i8, buf, g8.data(), 1, "sp");
    LLVMValueRef copy = ensure_copy_fn();
    LLVMValueRef cur = emit_call(copy, {payload, to_ptr(a), la});
    emit_call(copy, {cur, to_ptr(b), lb});
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef head = LLVMBuildGEP2(cur_fn().bd, i8, buf, g0.data(), 1, "shdr");
    store_addr(head, V::instr(LLVMBuildShl(cur_fn().bd, total, const_int(16), "shl"), ty_i64()),
               ty_i64(), line);
    return V::instr(payload, ty_str());
}

V Codegen::template_expr(const sema::HNode& node) {
    /* HIR 的 kid 是**交替**的：偶数下标是文字段（`StrLit`，允许空串）、奇数下标是插值表达式，
     * 文字段比插值多一个。 */
    std::vector<const sema::HNode*> parts;
    std::vector<std::string> texts;
    for (std::size_t i = 0; i < node.kids.size(); ++i) {
        if (i % 2 == 0) {
            const sema::HNode& seg = node.kids[i];
            if (seg.kind != sema::HKind::StrLit) {
                throw Unsupported("（内部）模板的文字段不是 StrLit", seg.span.line);
            }
            const int id = seg.string_id;
            if (id < 0 || id >= static_cast<int>(hir_.strings.size())) {
                throw Unsupported("（内部）串池下标越界", seg.span.line);
            }
            texts.push_back(hir_.strings[static_cast<std::size_t>(id)].bytes);
        } else {
            parts.push_back(&node.kids[i]);
        }
    }
    if (node.kids.size() % 2 == 0) {
        throw Unsupported("（内部）模板的 kid 个数应是奇数", node.span.line);
    }
    std::vector<V> vals;
    vals.reserve(parts.size());
    for (const sema::HNode* p : parts) vals.push_back(e(*p));

    /* 参考的每个 def 只有**一份帧**（非重入），所以同一模板里对**同一个 def** 的多次调用共用同一块
       结果缓冲：后一次调用连内容带头里的长度一起覆写前一次的，于是**每一段都渲染出最后一次调用的
       结果**（实测 `work/tmp/uibuf/probe_ui.py`：A/S/W 例如此；T/U/V/Z 例说明两个不同 def 之间不串，
       B/E/F/G/H 例说明字面量与变量不串）。调用**已经**求过值了，这里只把前一段的值改成后一段的。 */
    std::map<std::string, std::size_t> last_same_callee;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string* name = callee_name_of(hir_, *parts[i]);
        if (name != nullptr && vals[i].ty->kind == TyKind::Str) last_same_callee[*name] = i;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string* name = callee_name_of(hir_, *parts[i]);
        if (name == nullptr || vals[i].ty->kind != TyKind::Str) continue;
        if (last_same_callee[*name] != i) vals[i] = vals[last_same_callee[*name]];
    }

    /* 缓冲尺寸：文字段 + 每段的最坏字节数。**没有 Str 段时它是编译期常量** → 把 alloca 放进
       entry 块（静态 alloca：循环里不涨栈，也不会拉出 `___chkstk_ms`）；只有 Str 段的长度是
       运行期的，那时才只能开动态 alloca。 */
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    uint64_t fixed = 8;
    bool const_size = true;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        fixed += texts[i].size();
        if (i >= vals.size()) break;
        if (vals[i].ty->kind == TyKind::Str) {
            const_size = false;
        } else {
            fixed += part_capacity(vals[i], node.span.line);
        }
    }
    LLVMValueRef buf = nullptr;
    if (const_size) {
        buf = alloca_in_entry(LLVMArrayType2(i8, fixed), "tbuf");
    } else {
        LLVMValueRef size = const_int(fixed);
        for (const V& v : vals) {
            if (v.ty->kind == TyKind::Str) {
                size = LLVMBuildAdd(cur_fn().bd, size, len_of(v, node.span.line), "ts");
            }
        }
        buf = LLVMBuildArrayAlloca(cur_fn().bd, i8, size, "tbuf");
        LLVMSetAlignment(buf, 8);
    }
    std::vector<LLVMValueRef> g8{const_int(8)};
    LLVMValueRef payload = LLVMBuildGEP2(cur_fn().bd, i8, buf, g8.data(), 1, "tpl");
    LLVMValueRef cur = payload;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        cur = write_literal(cur, texts[i], node.span.line);
        if (i < vals.size()) cur = render_part(cur, vals[i], node.span.line);
    }
    /* 长度按**实际游标**算，不按缓冲尺寸 —— 所以尺寸只是个上界，不需要预先知道十进制位数。 */
    LLVMValueRef len = LLVMBuildSub(cur_fn().bd, LLVMBuildPtrToInt(cur_fn().bd, cur, i64_, "tce"),
                                    LLVMBuildPtrToInt(cur_fn().bd, payload, i64_, "tpe"), "tlen");
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef head = LLVMBuildGEP2(cur_fn().bd, i8, buf, g0.data(), 1, "thdr");
    store_addr(head, V::instr(LLVMBuildShl(cur_fn().bd, len, const_int(16), "thl"), ty_i64()),
               ty_i64(), node.span.line);
    return V::instr(payload, ty_str());
}

/* 一段值的**最坏字节数**（缓冲开多大）：`Bool` 5、其它整数/`Ptr` 21（20 位十进制 + '-'）。
   `Str` 的长度是运行期的，不算在这里（调用方单独用 `len_of` 加）——所以返回 0。 */
uint64_t Codegen::part_capacity(const V& v, int line) {
    if (v.ty->kind == TyKind::Str) return 0;
    if (v.ty->kind == TyKind::Ptr) return 21;
    if (v.ty->kind == TyKind::Int) return v.ty->is_bool ? 5 : 21;
    throw Unsupported("模板插值遇到 " + std::string(ty_kind_name(v.ty->kind)), line);
}

LLVMValueRef Codegen::write_literal(LLVMValueRef cur, const std::string& text, int line) {
    /* 含反斜杠的字面量现状**报错**（U-L10 的转义语义没定，方案 §2e 决定 1：S2c 期间不放宽）。
     * 注意 HIR 的串已经过 sema 的转义解码，所以这条只能拦住「源码里就是反斜杠」那种；
     * 解码过的转义在 sema 侧就该拦（已回报）。 */
    if (text.find('\\') != std::string::npos) {
        throw Unsupported("含转义的字符串字面量（U-L10 未定）", line);
    }
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    for (std::size_t k = 0; k < text.size(); ++k) {
        std::vector<LLVMValueRef> gi{const_int(k)};
        LLVMValueRef p = LLVMBuildGEP2(cur_fn().bd, i8, cur, gi.data(), 1, "lp");
        LLVMValueRef st = LLVMBuildStore(cur_fn().bd,
                                         LLVMConstInt(i8, static_cast<unsigned char>(text[k]), 0), p);
        LLVMSetAlignment(st, 1);
    }
    std::vector<LLVMValueRef> gn{const_int(text.size())};
    return LLVMBuildGEP2(cur_fn().bd, i8, cur, gn.data(), 1, "lc");
}

LLVMValueRef Codegen::render_part(LLVMValueRef cur, const V& v, int line) {
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
            LLVMValueRef st =
                LLVMBuildStore(cur_fn().bd, LLVMBuildTrunc(cur_fn().bd, ch, i8, "bt"), p);
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
                x = LLVMBuildSelect(cur_fn().bd, neg,
                                    LLVMBuildSub(cur_fn().bd, const_int(0), x, "dmg"), x, "dv");
            }
        }
        return emit_call(ensure_dec_fn(), {cur, x, minus});
    }
    throw Unsupported("模板插值遇到 " + std::string(ty_kind_name(v.ty->kind)), line);
}

LLVMValueRef Codegen::ensure_copy_fn() {
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
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, i, const_int(1), "ci1"), ty_i64()),
               ty_i64(), 0);
    LLVMBuildBr(cur_fn().bd, head);
    LLVMPositionBuilderAtEnd(cur_fn().bd, done);
    cur_fn().block = done;
    std::vector<LLVMValueRef> gn{n};
    LLVMBuildRet(cur_fn().bd, LLVMBuildGEP2(cur_fn().bd, i8, dst, gn.data(), 1, "cr"));
    finish_function();
    return fn;
}

LLVMValueRef Codegen::ensure_streq_fn() {
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
    store_addr(slot, V::instr(LLVMBuildAdd(cur_fn().bd, i, const_int(1), "qi1"), ty_i64()),
               ty_i64(), 0);
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

LLVMValueRef Codegen::ensure_dec_fn() {
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
    LLVMValueRef i2 =
        LLVMBuildSub(cur_fn().bd, llv(load_addr(slot_i, ty_i64())), const_int(1), "di2");
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

/* ================================================================ 聚合 */

V Codegen::array_literal(const sema::HNode& node) {
    std::vector<V> ev;
    ev.reserve(node.kids.size());
    for (const sema::HNode& item : node.kids) ev.push_back(e(item));
    if (ev.empty()) throw Unsupported("空数组字面量", node.span.line);
    /* 元素类型取**第一个**元素的类型（后面各元素不 convert，写入时按自己的宽度截断）。 */
    const TyRef& ty = ev[0].ty;
    LLVMTypeRef el = storage_ll(*ty);
    std::vector<LLVMValueRef> elems;
    for (const auto& e : ev) elems.push_back(const_for_storage(e));
    /* 载荷前必须带 8 字节 Nim 头（`p[-8] = len << 16`，读法见 `len_of` 与 facts §数组/字符串布局）：
     * 少了它，`.len()` 那条越界的**常量** `load` 在 O2 里是 undef，靠它判分支的整条 `if`
     * 会被折掉 —— 实测 008 的 `check_output` 因此恒返回 fail（`docs/notes/2026-10-18-test-result-chain.md`）。
     * 头低 16 位是 log2 容量，静态数据取 0（与 `array(<T>, n)` / `empty_array` 同口径）。 */
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    const uint64_t header = elems.size() << 16;
    std::vector<LLVMValueRef> head;
    for (int i = 0; i < 8; ++i) head.push_back(LLVMConstInt(i8, (header >> (8 * i)) & 0xFF, 0));
    LLVMTypeRef head_ty = LLVMArrayType2(i8, head.size());
    LLVMTypeRef payload_ty = LLVMArrayType2(el, elems.size());
    LLVMTypeRef fields[2] = {head_ty, payload_ty};
    LLVMTypeRef sty = LLVMStructTypeInContext(ctx(), fields, 2, 0);
    const std::string gname = ".arr" + std::to_string(node.span.line);
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
LLVMValueRef Codegen::field_storage_value(const V& v, const TyRef& ty, int line) {
    if (!is_int(*ty)) return llv(v);
    V conv = convert(v, ty, line);
    LLVMTypeRef st = storage_ll(*ty);
    LLVMValueRef ll = llv(conv);
    if (st != i64_) ll = LLVMBuildTrunc(cur_fn().bd, ll, st, "t");
    return ll;
}

V Codegen::composite_literal(const sema::HNode& node) {
    const TyRef& ty = node.ty;
    if (ty->kind != TyKind::Struct) {
        throw Unsupported(std::string("复合字面量类型 ") + ty_kind_name(ty->kind), node.span.line);
    }
    auto it = struct_ll_.find(ty->name);
    if (it == struct_ll_.end()) throw CompileFail("struct 类型没建好：" + ty->name);
    /* HIR 的 kid 顺序 = **声明序**，每个字段一个 kid（没写的是零值 `Const`），所以 `kids[i]`
     * 恒对应 `ty->fields[i]` —— 不再需要「按字段名匹配、补零」那一趟。
     *
     * 但**求值与拼接的先后**是源码书写序（求值有副作用时看得见，实验见 `work/tmp/ul4/`），
     * 而书写序在 HIR 里只剩每个 kid 的 span 行号（前端不填 `col`，见 `span.hh`）。所以按行号
     * 稳定排序：字段各占一行时正好还原书写序；同一行里几个字段的先后恢复不出来，退回声明序
     * —— 这种退回**只在同一行内声明序倒置时**与旧路径不同，sema 那边已经把它改成报错
     * （`analyze.cc` 的 `composite_lit_node`），所以这里看到同行字段就是声明序 = 书写序。
     * 补出来的零值 `Const` 带的是字面量首行的 span，排在最前，但下面会跳过它们。 */
    std::vector<std::size_t> order(node.kids.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&node](std::size_t a, std::size_t b) {
        return node.kids[a].span.line < node.kids[b].span.line;
    });
    std::vector<V> vals(node.kids.size());
    bool all_const = true;
    for (std::size_t i : order) {
        /* 每个 kid 已经是字段类型（sema 在复合字面量处发了 `Cast`，没写的字段是零值 `Const`）。 */
        vals[i] = e(node.kids[i]);
        all_const = all_const && vals[i].c.has_value();
    }
    if (all_const) {
        /* 常量聚合：顶层 `var x = T {…}` 的初始化式也走这里（拿不到 builder）。 */
        std::vector<LLVMValueRef> elems;
        elems.reserve(vals.size());
        for (const V& v : vals) elems.push_back(const_for_storage(v));
        LLVMValueRef agg = LLVMConstStructInContext(ctx(), elems.data(),
                                                    static_cast<unsigned>(elems.size()), 1);
        return V::instr(agg, ty);
    }
    if (cur_fn().bd == nullptr || cur_fn().block == nullptr) {
        throw Unsupported("复合字面量里的字段值不是编译期常量", node.span.line);
    }
    LLVMPositionBuilderAtEnd(cur_fn().bd, cur_fn().block);
    LLVMValueRef agg = LLVMConstNull(it->second);
    for (std::size_t i : order) {
        /* HIR 的 kid 按**声明序补满**（没写的字段是零值 `Const`），规则见 `hir.hh`；旧
         * `lower.cc` 只给**写了**的字段发 `insertvalue`。补出来的零和 `zeroinitializer`
         * 本来就没区别，所以零值常量字段不再发 —— 否则聚合返回会多一条 `insertvalue …, i8 0, n`。 */
        const TyRef& fty = ty->fields[i].second;
        const uint64_t mask = is_int(*fty) ? mask_of(fty->bits) : kMask64;
        if (vals[i].c && (*vals[i].c & mask) == 0) continue;
        LLVMValueRef el = vals[i].c ? const_for_storage(vals[i])
                                    : field_storage_value(vals[i], fty, node.span.line);
        agg = LLVMBuildInsertValue(cur_fn().bd, agg, el, static_cast<unsigned>(i), "cl");
    }
    return V::instr(agg, ty);
}

/* ================================================================ 串字面量 */

V Codegen::string_literal(int string_id, int line) {
    auto hit = str_pool_.find(string_id);
    if (hit != str_pool_.end()) return hit->second;
    if (string_id < 0 || string_id >= static_cast<int>(hir_.strings.size())) {
        throw Unsupported("（内部）串池下标越界", line);
    }
    const sema::HirString& s = hir_.strings[static_cast<std::size_t>(string_id)];
    /* 字符串字面量 → Nim 布局的常量（`p[-8] = (len << 16) | cap_log2`，接口 §3）。`cap_log2` 取 0
     * （只有 `len` 那半被用到）；末尾照加一个 NUL。转义语义没定（U-L10），含反斜杠的仍报错：
     * 方案 §2e 决定 1 —— S2c 期间保持现状行为，放开是单独一步。 */
    if (s.bytes.find('\\') != std::string::npos) {
        throw Unsupported("含转义的字符串字面量（U-L10 未定）", line);
    }
    std::string raw;
    const uint64_t header = static_cast<uint64_t>(s.bytes.size()) << 16;
    for (int i = 0; i < 8; ++i) raw.push_back(static_cast<char>((header >> (8 * i)) & 0xFF));
    raw += s.bytes;
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
    str_pool_[string_id] = v;
    return v;
}

/* ================================================================ Program */

Program::Program(const sema::Hir& hir) : Codegen(hir) {
    open_module("simplex");
    make_types();
    make_globals();
}

void Program::make_types() {
    /* 按**名字序**建（旧实现从 `std::map<std::string, TyRef>` 迭代出来）—— LLVM 的类型表顺序
     * 跟着建序走，换序就换 IR 的打印顺序。 */
    std::map<std::string, const sema::HirStruct*> by_name;
    for (const sema::HirStruct& s : hir_.structs) by_name[s.name] = &s;
    for (const auto& entry : by_name) {
        const sema::HirStruct& s = *entry.second;
        std::vector<LLVMTypeRef> elems;
        for (const sema::HirField& f : s.fields) elems.push_back(storage_ll(*f.ty));
        /* 空 struct（rv32im 的 `Input`/`Output`）：取 **1 字节占位**而不是零宽度聚合 ——
         * 零宽度的 alloca / 全局大小是 0，那条路没验证过。 */
        if (elems.empty()) elems.push_back(storage_ll(*make_int(s.bits, false)));
        struct_ll_[entry.first] = LLVMStructTypeInContext(ctx(), elems.data(),
                                                          static_cast<unsigned>(elems.size()), 1);
    }
}

void Program::make_globals() {
    /* 初值由 sema 折成位模式（`HirGlobal.init_bits`）：全局初值必须是编译期常量，少了它 codegen
     * 只能把初值降级成 `tc_main` 里的 store，产物就变了（方案 §2b 补漏 2）。 */
    /* 建序照旧：**源码顶层声明的全局**在这里按 `hir.globals` 的顺序预建；前缀全局
     * （`global_seed`）是**按需**建的 —— 旧实现第一次被引用才 `LLVMAddGlobal`，于是它排在引用
     * 它的那条语句旁边的私有全局之后。HIR 没有「这个全局是哪种来源」的字段，所以这张前缀名表
     * 留在 codegen（`stdlib_prefix.si` 只给了这一个名字）。 */
    for (const sema::HirGlobal& g : hir_.globals) {
        if (is_prefix_global(g.name)) continue;
        add_global(g);
    }
}

void Program::build() { emit_main(); }

void Program::emit_main() {
    LLVMValueRef fnref =
        LLVMAddFunction(module(), "tc_main", LLVMFunctionType(void_ty_, nullptr, 0, 0));
    FnCtx& f = begin_function(fnref, ty_void());
    f.name = "tc_main";
    f.is_top_def = false; /* 程序根不属于任何 def（嵌套 def 的 owner 因此是 `closure`） */
    lower_block(hir_.main.body);
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

}  // namespace codegen
}  // namespace simplexc
