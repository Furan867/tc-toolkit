/* lower.cc —— 类型化 AST → LLVM IR（接口与取舍见 lower.hh）。
 *
 * 行号引用形如 `oracle:1234`，指冻结原型 `simplexc/lower/ir_build.py` 的那一行 —— 语义以它为
 * 权威，本文件只做等价翻译。改动前先读那几行。 */
#include "lower.hh"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <utility>

namespace simplexc {
namespace lower {
namespace {

/* 前缀（stdlib）里定义、生成源码当作「已存在」用的名字：只登记参考实现里出现过的几个
 * （oracle:361-370）。**这不是内建表** —— 内建表的唯一来源是接口 §2 的 `builtins.def`。 */
const std::map<std::string, TyRef>& prefix_globals() {
    static const std::map<std::string, TyRef> m{{"global_seed", ty_seed()}};
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

/* `z = -x` 的 64 位取负：用无符号补码写法，避免 `INT64_MIN` 的 `-x`/`abs` 未定义行为
 * （Python 里是任意精度整数，不会溢出）。 */
uint64_t neg64(uint64_t x) { return ~x + 1; }

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

}  // namespace

/* ================================================================ 类型 */

TyRef make_int(int bits, bool is_signed) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Int;
    t->bits = bits;
    t->is_signed = is_signed;
    return t;
}

TyRef make_enum(const std::string& name) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Enum;
    t->name = name;
    return t;
}

TyRef make_struct(const std::string& name, int bits,
                  std::vector<std::pair<std::string, TyRef>> fields) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Struct;
    t->bits = bits;
    t->name = name;
    t->fields = std::move(fields);
    return t;
}

TyRef make_array(TyRef elem) {
    auto t = std::make_shared<Ty>();
    t->kind = TyKind::Array;
    t->elem = std::move(elem);
    return t;
}

const TyRef& ty_void() {
    static const TyRef t = [] {
        auto x = std::make_shared<Ty>();
        x->kind = TyKind::Void;
        return TyRef(x);
    }();
    return t;
}

const TyRef& ty_i64() {
    static const TyRef t = make_int(64, true);
    return t;
}

const TyRef& ty_bool() {
    static const TyRef t = [] {
        auto x = std::make_shared<Ty>();
        x->kind = TyKind::Int;
        x->bits = 1;
        x->is_bool = true;
        return TyRef(x);
    }();
    return t;
}

const TyRef& ty_ptr() {
    static const TyRef t = [] {
        auto x = std::make_shared<Ty>();
        x->kind = TyKind::Ptr;
        return TyRef(x);
    }();
    return t;
}

const TyRef& ty_str() {
    static const TyRef t = [] {
        auto x = std::make_shared<Ty>();
        x->kind = TyKind::Str;
        x->elem = make_int(8, false);   /* `Str = Arr[Char]`，Char = 1 字节无符号 */
        return TyRef(x);
    }();
    return t;
}

const TyRef& ty_seed() {
    static const TyRef t = make_int(64, false);
    return t;
}

const TyRef& ty_time() {
    static const TyRef t = make_int(64, false);   /* `Time` 的单位是纳秒 */
    return t;
}

uint64_t mask_of(int bits) {
    return bits >= 64 ? kMask64 : (uint64_t{1} << bits) - 1;
}

const char* ty_kind_name(TyKind k) {
    switch (k) {
        case TyKind::Void: return "void";
        case TyKind::Int: return "int";
        case TyKind::Ptr: return "ptr";
        case TyKind::Enum: return "enum";
        case TyKind::Struct: return "struct";
        case TyKind::Array: return "array";
        case TyKind::Str: return "str";
    }
    return "?";
}

int storage_bytes(const Ty& ty) {
    switch (ty.kind) {
        case TyKind::Ptr:
        case TyKind::Str:
        case TyKind::Array:
        case TyKind::Enum:
            return 8;
        case TyKind::Int:
            return (ty.bits + 7) / 8;   /* U-L7 指示 4：写宽度 = 值类型的字节宽度 */
        default:
            throw Unsupported("类型 " + std::string(ty_kind_name(ty.kind)) + " 没有字节宽度", 0);
    }
}

/* ================================================================ 常量折叠 */

int64_t as_signed(uint64_t x, int bits) {
    x &= mask_of(bits);
    if (bits < 64 && ((x >> (bits - 1)) & 1)) {
        return static_cast<int64_t>(x) - static_cast<int64_t>(uint64_t{1} << bits);
    }
    return static_cast<int64_t>(x);
}

uint64_t fold_arith(const std::string& op, uint64_t a, uint64_t b, const Ty& ty) {
    const int bits = ty.kind == TyKind::Int ? ty.bits : 64;
    a &= mask_of(bits);
    b &= mask_of(bits);
    if (op == "+") return (a + b) & mask_of(bits);
    if (op == "-") return (a - b) & mask_of(bits);
    if (op == "*") return (a * b) & mask_of(bits);
    if (op == "&") return a & b;
    if (op == "|") return a | b;
    if (op == "^") return a ^ b;
    if (op == "<<") return (a << (b & uint64_t(bits - 1))) & mask_of(bits);
    if (op == ">>") {
        /* U-L8 指示 1：`trunc_W(lshr(ext64(a), n & (W-1)))`，常量按符号性 sext。 */
        const uint64_t ext = ty.is_signed ? static_cast<uint64_t>(as_signed(a, bits)) : a;
        return (ext >> (b & uint64_t(bits - 1))) & mask_of(bits);
    }
    if (op == "/" || op == "%") {
        if (b == 0) return op == "/" ? 0 : a;   /* U-L8：除零不 trap，商 0、余数 = 被除数 */
        uint64_t q = 0, r = 0;
        if (ty.is_signed) {
            const int64_t sa = as_signed(a, bits);
            const int64_t sb = as_signed(b, bits);
            const uint64_t ma = sa < 0 ? neg64(static_cast<uint64_t>(sa)) : static_cast<uint64_t>(sa);
            const uint64_t mb = sb < 0 ? neg64(static_cast<uint64_t>(sb)) : static_cast<uint64_t>(sb);
            const uint64_t mq = mb != 0 ? ma / mb : 0;
            const uint64_t mr = mb != 0 ? ma % mb : 0;
            q = (sa < 0) != (sb < 0) ? neg64(mq) : mq;
            r = sa < 0 ? neg64(mr) : mr;
        } else {
            q = a / b;
            r = a % b;
        }
        return (op == "/" ? q : r) & mask_of(bits);
    }
    throw Unsupported("运算符 " + op, 0);
}

uint64_t fold_unary(const std::string& op, uint64_t a, const Ty& ty) {
    const int bits = ty.kind == TyKind::Int ? ty.bits : 64;
    a &= mask_of(bits);
    if (op == "-") return neg64(a) & mask_of(bits);
    if (op == "~") return (~a) & mask_of(bits);
    if (op == "+") return a;
    throw Unsupported("一元运算符 " + op, 0);
}

uint64_t fold_cmp(const std::string& op, uint64_t a, uint64_t b, const Ty& ty) {
    const int bits = ty.kind == TyKind::Int ? ty.bits : 64;
    uint64_t x = a & mask_of(bits);
    uint64_t y = b & mask_of(bits);
    bool r = false;
    if (ty.kind == TyKind::Int && ty.is_signed) {
        const int64_t sx = as_signed(x, bits);
        const int64_t sy = as_signed(y, bits);
        if (op == "==") r = sx == sy;
        else if (op == "!=") r = sx != sy;
        else if (op == "<") r = sx < sy;
        else if (op == "<=") r = sx <= sy;
        else if (op == ">") r = sx > sy;
        else if (op == ">=") r = sx >= sy;
        else throw Unsupported("比较运算符 " + op, 0);
    } else {
        if (op == "==") r = x == y;
        else if (op == "!=") r = x != y;
        else if (op == "<") r = x < y;
        else if (op == "<=") r = x <= y;
        else if (op == ">") r = x > y;
        else if (op == ">=") r = x >= y;
        else throw Unsupported("比较运算符 " + op, 0);
    }
    return r ? 1 : 0;
}

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

Lower::~Lower() {
    if (bd_ != nullptr) LLVMDisposeBuilder(bd_);
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

Lower::FnState Lower::save_state() const {
    FnState s;
    s.bd = bd_;
    s.fnref = fnref_;
    s.ret_ty = ret_ty_;
    s.cur_block = cur_block_;
    s.entry = entry_;
    s.scopes = scopes_;
    s.breaks = breaks_;
    s.cur_name = cur_name_;
    s.cur_def = cur_def_;
    return s;
}

void Lower::restore_state(const FnState& s) {
    bd_ = s.bd;
    fnref_ = s.fnref;
    ret_ty_ = s.ret_ty;
    cur_block_ = s.cur_block;
    entry_ = s.entry;
    scopes_ = s.scopes;
    breaks_ = s.breaks;
    cur_name_ = s.cur_name;
    cur_def_ = s.cur_def;
}

void Lower::mark_no_builtin(LLVMValueRef fnref) {
    /* 参考实现给每个函数挂 `nobuiltin`（否则 LLVM 会把 `memory_copy` 之类的名字当内建去认）。 */
    const unsigned kind = LLVMGetEnumAttributeKindForName("nobuiltin", 9);
    if (kind != 0) {
        LLVMAddAttributeAtIndex(fnref, LLVMAttributeFunctionIndex,
                                LLVMCreateEnumAttribute(ctx(), kind, 0));
    }
}

void Lower::begin_function(LLVMValueRef fnref, const TyRef& ret_ty) {
    mark_no_builtin(fnref);
    bd_ = LLVMCreateBuilderInContext(ctx());
    fnref_ = fnref;
    ret_ty_ = ret_ty;
    entry_ = LLVMAppendBasicBlockInContext(ctx(), fnref, "entry");
    LLVMPositionBuilderAtEnd(bd_, entry_);
    cur_block_ = entry_;
    scopes_.clear();
    scopes_.emplace_back();
    breaks_.clear();
}

void Lower::finish_function() {
    if (LLVMGetBasicBlockTerminator(cur_block_) == nullptr) {
        if (ret_ty_->kind == TyKind::Void) {
            LLVMBuildRetVoid(bd_);
        } else {
            warnings_.push_back(cur_name_ + "：控制流落到函数末尾，补 ret 零值");
            LLVMBuildRet(bd_, LLVMConstNull(val_ll(*ret_ty_)));
        }
    }
    LLVMDisposeBuilder(bd_);
    bd_ = nullptr;
}

LLVMBasicBlockRef Lower::new_block(const char* name) {
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, name);
    LLVMPositionBuilderAtEnd(bd_, bb);
    cur_block_ = bb;
    return bb;
}

LLVMBasicBlockRef Lower::br_new_block(const char* name) {
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, name);
    LLVMBuildBr(bd_, bb);
    LLVMPositionBuilderAtEnd(bd_, bb);
    cur_block_ = bb;
    return bb;
}

LLVMValueRef Lower::alloca_in_entry(LLVMTypeRef ty, const std::string& name) {
    /* 插在 **entry 块最前面**：循环里声明也不会每次迭代吃栈，而且它是**静态** alloca
       （LLVM 只把「在 entry 块 + 尺寸是常量」的 alloca 当静态 —— 否则会拉出 `___chkstk_ms` 且每轮涨栈）。 */
    LLVMValueRef first = LLVMGetFirstInstruction(entry_);
    if (first == nullptr) {
        LLVMPositionBuilderAtEnd(bd_, entry_);
    } else {
        LLVMPositionBuilderBefore(bd_, first);
    }
    LLVMValueRef a = LLVMBuildAlloca(bd_, ty, name.c_str());
    LLVMSetAlignment(a, 1);
    LLVMPositionBuilderAtEnd(bd_, cur_block_);
    return a;
}

LLVMValueRef Lower::alloca_slot(const TyRef& ty, const std::string& name) {
    return alloca_in_entry(ll_type(*ty), name);
}

/* ================================================================ Lower：作用域与绑定 */

Lower::Binding Lower::Binding::local(LLVMValueRef p, TyRef t) {
    Binding b;
    b.kind = Kind::Local;
    b.ptr = p;
    b.ty = std::move(t);
    return b;
}

Lower::Binding Lower::Binding::capture(LLVMValueRef p, TyRef t) {
    Binding b;
    b.kind = Kind::Capture;
    b.ptr = p;
    b.ty = std::move(t);
    return b;
}

Lower::Binding Lower::Binding::constant(V v) {
    Binding b;
    b.kind = Kind::Const;
    b.value = std::move(v);
    return b;
}

Lower::Binding Lower::Binding::nested(const DefStmt* d) {
    Binding b;
    b.kind = Kind::Def;
    b.def = d;
    return b;
}

void Lower::bind(const std::string& name, Binding b) { scopes_.back()[name] = std::move(b); }

void Lower::bind_root_scope(const std::string& name, Binding b) {
    scopes_.front()[name] = std::move(b);
}

const Lower::Binding* Lower::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
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
    return LLVMBuildAnd(bd_, ll, const_int(mask_of(bits)), "m");
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
            return V::instr(LLVMBuildPtrToInt(bd_, llv(v), i64_, "pi"), target);
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
    return LLVMBuildIntToPtr(bd_, llv(v), ptr_, "p");
}

V Lower::load_addr(LLVMValueRef p, const TyRef& ty) {
    LLVMTypeRef st = ll_type(*ty);
    LLVMValueRef inst = LLVMBuildLoad2(bd_, st, p, "ld");
    LLVMSetAlignment(inst, 1);   /* U-L1：地址是字节地址、无对齐要求，且不许插对齐检查 */
    if (!is_int(*ty)) return V::instr(inst, ty);
    LLVMValueRef v = inst;
    if (st != i64_) v = LLVMBuildZExt(bd_, v, i64_, "z");
    if (ty->bits < 64) v = mask_ll(v, ty->bits);
    return V::instr(v, ty);
}

void Lower::store_addr(LLVMValueRef p, const V& v, const TyRef& ty, int line) {
    if (!is_int(*ty)) {
        LLVMValueRef inst = LLVMBuildStore(bd_, llv(v), p);
        LLVMSetAlignment(inst, 1);
        return;
    }
    V conv = convert(v, ty, line);
    LLVMTypeRef st = storage_ll(*ty);
    LLVMValueRef ll = llv(conv);
    if (st != i64_) ll = LLVMBuildTrunc(bd_, ll, st, "t");   /* 窄写只写该宽度 */
    LLVMValueRef inst = LLVMBuildStore(bd_, ll, p);
    LLVMSetAlignment(inst, 1);
}

std::pair<LLVMValueRef, TyRef> Lower::prefix_global(const std::string& name, int line) {
    auto it = prefix_globals().find(name);
    if (it == prefix_globals().end()) throw Unsupported("前缀全局 " + name, line);
    const TyRef& ty = it->second;
    LLVMValueRef g = LLVMAddGlobal(module(), ll_type(*ty), name.c_str());
    LLVMSetLinkage(g, LLVMInternalLinkage);
    LLVMSetInitializer(g, LLVMConstNull(ll_type(*ty)));
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
            LLVMValueRef off = LLVMBuildMul(bd_, llv(idx), const_int(uint64_t(step)), "io");
            std::vector<LLVMValueRef> gi{off};
            LLVMValueRef p = LLVMBuildGEP2(bd_, storage_ll(*make_int(8, false)), to_ptr(base),
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
            LLVMValueRef p = LLVMBuildGEP2(bd_, storage_ll(*make_int(8, false)), base.first,
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
    scopes_.emplace_back();
    for (const auto& st : block.stmts) lower_stmt(*st);
    scopes_.pop_back();
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
            default:
                throw Unsupported("语句形态 " + std::string(kind_name(st.kind)), st.src_line);
        }
    } catch (const Unsupported& e) {
        unsupported_.push_back(Diagnostic{e.line() != 0 ? e.line() : st.src_line, e.what()});
        if (!collect_) throw;
    }
    /* 块已被终止符收口（return / break）→ 后续语句放进一个不可达块继续发。 */
    if (LLVMGetBasicBlockTerminator(cur_block_) != nullptr) new_block("dead");
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
        /* 参考实现把「常量变量」当常量（`ul7_narrow_var.md` 的 fE +304）→ 保真。 */
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
     * 连里面的名字都不解析 —— 与参考实现一致（facts.md §006 的语义）。 */
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
        if (cont == nullptr) cont = LLVMAppendBasicBlockInContext(ctx(), fnref_, "ifend");
        LLVMBuildBr(bd_, cont);
    };
    for (const IfCase* case_ : cases) {
        LLVMValueRef cond = as_cond(e(*case_->cond));
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "then");
        LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "next");
        LLVMBuildCondBr(bd_, cond, then_bb, next_bb);
        LLVMPositionBuilderAtEnd(bd_, then_bb);
        cur_block_ = then_bb;
        lower_block(*case_->block);
        if (LLVMGetBasicBlockTerminator(cur_block_) == nullptr) join();
        LLVMPositionBuilderAtEnd(bd_, next_bb);
        cur_block_ = next_bb;
    }
    if (else_block != nullptr) {
        lower_block(*else_block);
        if (LLVMGetBasicBlockTerminator(cur_block_) == nullptr) join();
    }
    if (cont != nullptr) {
        /* 没有 else 时，`next_bb` 就是「条件全不成立」那条路 —— 它也要汇进 cont。 */
        if (LLVMGetBasicBlockTerminator(cur_block_) == nullptr) LLVMBuildBr(bd_, cont);
        LLVMPositionBuilderAtEnd(bd_, cont);
        cur_block_ = cont;
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
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "body");
    LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "exit");
    LLVMValueRef cond = as_cond(e(*st.cond));
    LLVMBuildCondBr(bd_, cond, body_bb, exit_bb);
    LLVMPositionBuilderAtEnd(bd_, body_bb);
    cur_block_ = body_bb;
    breaks_.push_back({st.label, exit_bb});
    lower_block(*st.block);
    breaks_.pop_back();
    if (LLVMGetBasicBlockTerminator(cur_block_) == nullptr) LLVMBuildBr(bd_, head);
    LLVMPositionBuilderAtEnd(bd_, exit_bb);
    cur_block_ = exit_bb;
}

void Lower::s_BreakStmt(const BreakStmt& st) {
    LLVMBasicBlockRef target = nullptr;
    for (auto it = breaks_.rbegin(); it != breaks_.rend(); ++it) {
        if (st.label.empty() || st.label == it->first) {
            target = it->second;
            break;
        }
    }
    if (target == nullptr) throw Unsupported("break " + st.label, st.src_line);
    LLVMBuildBr(bd_, target);
}

void Lower::s_ReturnStmt(const ReturnStmt& st) {
    if (st.value == nullptr) {
        if (ret_ty_->kind != TyKind::Void) {
            throw Unsupported("非 None 函数里裸 return", st.src_line);
        }
        LLVMBuildRetVoid(bd_);
        return;
    }
    if (ret_ty_->kind == TyKind::Void) throw Unsupported("None 函数里 return 值", st.src_line);
    V v = convert(e(*st.value), ret_ty_, st.src_line);
    LLVMBuildRet(bd_, llv(v));
}

void Lower::s_SwitchStmt(const SwitchStmt& st) {
    V subj = e(*st.subject);
    LLVMValueRef subj_ll = subj.c ? const_int(pat_const(subj).value_or(0)) : subj.ll;
    std::vector<const SwitchCase*> plain;
    std::vector<const SwitchCase*> defaults;
    for (const auto& c : st.cases) {
        (c->value != nullptr ? plain : defaults).push_back(c.get());
    }
    LLVMBasicBlockRef end = nullptr;
    auto join = [&]() {
        if (end == nullptr) end = LLVMAppendBasicBlockInContext(ctx(), fnref_, "swend");
        LLVMBuildBr(bd_, end);
    };
    for (const SwitchCase* case_ : plain) {
        auto cvv = const_value(*case_->value);
        if (!cvv) throw Unsupported("switch case 的值不是常量", case_->src_line);
        LLVMValueRef cval = const_int(pat_const(*cvv).value_or(0));
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "case");
        LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "casenext");
        LLVMValueRef cnd = LLVMBuildICmp(bd_, LLVMIntEQ, subj_ll, cval, "sc");
        LLVMBuildCondBr(bd_, cnd, then_bb, next_bb);
        LLVMPositionBuilderAtEnd(bd_, then_bb);
        cur_block_ = then_bb;
        lower_block(*case_->block);
        if (LLVMGetBasicBlockTerminator(cur_block_) == nullptr) join();
        LLVMPositionBuilderAtEnd(bd_, next_bb);
        cur_block_ = next_bb;
    }
    if (!defaults.empty()) {
        lower_block(*defaults[0]->block);
        if (LLVMGetBasicBlockTerminator(cur_block_) == nullptr) join();
    } else if (end != nullptr) {
        LLVMBuildBr(bd_, end);
    }
    if (end != nullptr) {
        LLVMPositionBuilderAtEnd(bd_, end);
        cur_block_ = end;
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
    collect_captures(st, out, visiting);
    return out;
}

void Lower::collect_captures(const DefStmt& st, Closure& out,
                             std::set<const DefStmt*>& visiting) {
    /* 嵌套 def 体里引用了哪些**外层局部**（按引用捕获 → 传 alloca 指针）。自己的参数、`var`/
     * `let`/内层 `def` 都算「自己的」，同名遮蔽外层 —— 不往内层 `DefStmt` 的体里钻：内层 def 真被
     * 调用时再算它自己的捕获，那时本函数的参数/局部/捕获都已经在作用域里。
     *
     * 但**兄弟 def**（本函数体里按名调用、定义在外面的嵌套 def）必须在这里就摊平：参考实现按
     * **最外层调用点**解析 free variable（`docs/notes/2026-10-18-in-scope-fix.md` §6 第 1 条 ——
     * L/M 片段里被查的 def 体直接出现在调用者代码里），而我们把它生成成一个真函数，它的捕获
     * 只能由**调用它的那一层**带进来。006：`handle_test_result` 调 `halt`，`halt` 用
     * `run_sim` 体的 `burst_target_cycle` —— 后者得先被 `handle_test_result` 捕获、再原样传给
     * `halt`（110 的 `seen` / 075 的 `test` 同形）。 */
    std::set<std::string> own;
    for (const auto& p : st.params) own.insert(p->name);

    std::function<void(const Node&)> collect = [&](const Node& n) {
        if (n.kind == NK::VarDecl) own.insert(static_cast<const VarDeclStmt&>(n).name);
        if (n.kind == NK::Def) own.insert(static_cast<const DefStmt&>(n).name);
        for_each_child(n, collect);
    };
    collect(*st.body);

    std::function<void(const Node&)> rec = [&](const Node& n) {
        if (n.kind == NK::Def) return;   /* 内层 def 有自己的作用域 */
        if (n.kind == NK::Ident) {
            const auto& id = static_cast<const IdentExpr&>(n);
            if (own.count(id.name) != 0) return;
            const DefStmt* target = nullptr;
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
            if (target != nullptr && visiting.insert(target).second) {
                bool known = false;
                for (const DefStmt* d : out.defs) known = known || d == target;
                if (!known) out.defs.push_back(target);
                collect_captures(*target, out, visiting);
            }
        }
        for_each_child(n, rec);
    };
    rec(*st.body);
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
    return convert(e(*node.expr), ty_of_ref(*node.target, node.src_line), node.src_line);
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
        LLVMValueRef cnd = LLVMBuildICmp(bd_, LLVMIntEQ, llv(v), const_int(0), "n");
        return V::instr(LLVMBuildZExt(bd_, cnd, i64_, "nb"), ty_bool());
    }
    V v = e(*node.expr);
    if (!is_int(*v.ty)) {
        throw Unsupported("一元 " + node.op + " 用在 " + ty_kind_name(v.ty->kind), node.src_line);
    }
    auto c = pat_const(v);
    if (c) return V::konst(v.ty, fold_unary(node.op, *c, *v.ty));
    LLVMValueRef ll = nullptr;
    if (node.op == "-") {
        ll = LLVMBuildSub(bd_, const_int(0), llv(v), "neg");
    } else if (node.op == "~") {
        ll = LLVMBuildXor(bd_, llv(v), const_int(mask_of(v.ty->bits)), "bnot");
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
    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "t_then");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "t_else");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "t_end");
    LLVMBuildCondBr(bd_, cond, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(bd_, then_bb);
    cur_block_ = then_bb;
    V tv = e(*node.then_);
    LLVMBasicBlockRef t_bb = cur_block_;
    LLVMPositionBuilderAtEnd(bd_, else_bb);
    cur_block_ = else_bb;
    V ev = e(*node.else_);
    LLVMBasicBlockRef e_bb = cur_block_;

    if (tv.ty->kind == TyKind::Void || ev.ty->kind == TyKind::Void) {
        throw Unsupported("三目的一支没有值", node.src_line);
    }
    TyRef ty = unify(tv.ty, ev.ty, node.src_line);
    LLVMPositionBuilderAtEnd(bd_, t_bb);
    tv = convert(tv, ty, node.src_line);
    LLVMBuildBr(bd_, end_bb);
    LLVMPositionBuilderAtEnd(bd_, e_bb);
    ev = convert(ev, ty, node.src_line);
    LLVMBuildBr(bd_, end_bb);

    LLVMPositionBuilderAtEnd(bd_, end_bb);
    cur_block_ = end_bb;
    /* phi 的结果类型是**值的类型**（窄整数一律 i64 位模式），不是内存宽度。 */
    LLVMValueRef phi = LLVMBuildPhi(bd_, val_ll(*ty), "cnd");
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

LLVMValueRef Lower::as_cond(const V& v) {
    if (v.ty->kind == TyKind::Void) throw Unsupported("None 值当条件", 0);
    return LLVMBuildICmp(bd_, LLVMIntNE, llv(v), const_int(0), "c");
}

LLVMValueRef Lower::sext64(LLVMValueRef ll, int bits) {
    if (bits >= 64) return ll;
    const int sh = 64 - bits;
    return LLVMBuildAShr(
        bd_, LLVMBuildShl(bd_, ll, const_int(uint64_t(sh)), "sh"), const_int(uint64_t(sh)),
        "sa");
}

V Lower::compare(const std::string& op, const V& a, const V& b, int line) {
    TyRef ty = unify(a.ty, b.ty, line);
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
    LLVMValueRef cnd = LLVMBuildICmp(bd_, pred, x, y, "cmp");
    return V::instr(LLVMBuildZExt(bd_, cnd, i64_, "cmpz"), ty_bool());
}

V Lower::logical(const BinaryExpr& node) {
    /* `&&` / `||` 短路（`get_command` 读的是游戏内存，不能随意重排）。 */
    LLVMValueRef lhs = as_cond(e(*node.left));
    LLVMBasicBlockRef lhs_bb = cur_block_;
    LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "rhs");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(ctx(), fnref_, "lend");
    if (node.op == "&&") LLVMBuildCondBr(bd_, lhs, rhs_bb, end_bb);
    else LLVMBuildCondBr(bd_, lhs, end_bb, rhs_bb);
    LLVMPositionBuilderAtEnd(bd_, rhs_bb);
    cur_block_ = rhs_bb;
    LLVMValueRef rhs = as_cond(e(*node.right));
    LLVMBasicBlockRef rhs_end = cur_block_;
    LLVMBuildBr(bd_, end_bb);
    LLVMPositionBuilderAtEnd(bd_, end_bb);
    cur_block_ = end_bb;
    LLVMTypeRef i1 = LLVMIntTypeInContext(ctx(), 1);
    LLVMValueRef phi = LLVMBuildPhi(bd_, i1, "lg");
    std::vector<LLVMValueRef> vals{LLVMConstInt(i1, node.op == "&&" ? 0 : 1, 0), rhs};
    std::vector<LLVMBasicBlockRef> blocks{lhs_bb, rhs_end};
    LLVMAddIncoming(phi, vals.data(), blocks.data(), 2);
    return V::instr(LLVMBuildZExt(bd_, phi, i64_, "lgz"), ty_bool());
}

V Lower::binop(const std::string& op, const V& a, const V& b, int line) {
    if (!is_arith(op)) throw Unsupported("运算符 " + op, line);
    TyRef ty = unify(a.ty, b.ty, line);
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
        LLVMValueRef ll = op == "+" ? LLVMBuildAdd(bd_, llv(av), llv(bv), "pa")
                                     : LLVMBuildSub(bd_, llv(av), llv(bv), "pa");
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
        LLVMValueRef amt = LLVMBuildAnd(bd_, y, const_int(uint64_t(bits - 1)), "amt");
        return V::instr(mask_ll(LLVMBuildShl(bd_, x, amt, "shl"), bits), ty);
    }
    if (op == ">>") {
        /* U-L8：**永远 lshr**（不是 ashr），左操作数走 `pattern_of`（常量按符号性扩），再截回 W。 */
        LLVMValueRef amt = LLVMBuildAnd(bd_, y, const_int(uint64_t(bits - 1)), "amt");
        LLVMValueRef ll = LLVMBuildLShr(bd_, pattern_of(av), amt, "shr");
        return V::instr(mask_ll(ll, bits), ty);
    }
    LLVMValueRef r = nullptr;
    if (op == "+") r = LLVMBuildAdd(bd_, x, y, "op");
    else if (op == "-") r = LLVMBuildSub(bd_, x, y, "op");
    else if (op == "*") r = LLVMBuildMul(bd_, x, y, "op");
    else if (op == "&") r = LLVMBuildAnd(bd_, x, y, "op");
    else if (op == "|") r = LLVMBuildOr(bd_, x, y, "op");
    else r = LLVMBuildXor(bd_, x, y, "op");
    return V::instr(mask_ll(r, bits), ty);
}

LLVMValueRef Lower::div(const std::string& op, LLVMValueRef x, LLVMValueRef y, const Ty& ty) {
    /* opcode 跟**被除数**的符号性（facts.md §`/`、`%`），零除数不 trap（商 0、余数 = 被除数），
     * 有符号再避开 `INT_MIN / -1`。注意操作数**没有**过 `pattern_of`：常量在这里用的是
     * W 位掩码后的裸位模式 —— 参考实现那个「窄常量零扩展成 64 位再送有符号除法」的不自洽
     * 就是从这个形状出来的，别顺手改成 pattern_of。 */
    const int bits = ty.bits;
    LLVMValueRef zero = const_int(0);
    LLVMValueRef bz = LLVMBuildICmp(bd_, LLVMIntEQ, y, zero, "bz");
    LLVMValueRef safe = LLVMBuildSelect(bd_, bz, const_int(1), y, "sb");
    LLVMValueRef bm1 = nullptr;
    if (ty.is_signed) {
        bm1 = LLVMBuildICmp(bd_, LLVMIntEQ, y, const_int(mask_of(bits)), "bm1");
        safe = LLVMBuildSelect(bd_, bm1, const_int(1), safe, "sb2");
    }
    LLVMValueRef raw = nullptr;
    if (op == "/") {
        raw = ty.is_signed ? LLVMBuildSDiv(bd_, x, safe, "q") : LLVMBuildUDiv(bd_, x, safe, "q");
    } else {
        raw = ty.is_signed ? LLVMBuildSRem(bd_, x, safe, "r") : LLVMBuildURem(bd_, x, safe, "r");
    }
    LLVMValueRef res = nullptr;
    if (op == "/") {
        res = LLVMBuildSelect(bd_, bz, zero, raw, "qz");
        if (bm1 != nullptr) {
            LLVMValueRef neg = mask_ll(LLVMBuildSub(bd_, zero, x, "nq"), bits);
            res = LLVMBuildSelect(bd_, bm1, neg, res, "qz2");
        }
    } else {
        res = LLVMBuildSelect(bd_, bz, x, raw, "rz");
        if (bm1 != nullptr) res = LLVMBuildSelect(bd_, bm1, zero, res, "rz2");
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
                if (pg != prefix_globals().end()) return pg->second;
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
                    return d->ret != nullptr ? ty_of_ref(*d->ret, d->lo) : ty_void();
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
    if (name == "in_scope" || name == "type_of" || name == "size") {
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
    return LLVMBuildCall2(bd_, LLVMGlobalGetValueType(fnref), fnref, a.data(),
                          static_cast<unsigned>(a.size()), "");
}

V Lower::call_user(const FunctionDef& d, const CallExpr& node) {
    TyRef ret_ty = d.ret != nullptr ? ty_of_ref(*d.ret, d.lo) : ty_void();
    auto param_tys = decl_param_types(d);
    if (!param_tys || param_tys->size() != node.args.size()) {
        throw Unsupported(d.name + "() 的参数表对不上调用", node.src_line);
    }
    LLVMValueRef fnref = ensure_def_fn(d.name, d.lo, d, ret_ty, *param_tys);
    std::vector<LLVMValueRef> vals;
    for (size_t i = 0; i < node.args.size(); ++i) {
        V conv = convert(e(*node.args[i]), (*param_tys)[i], node.src_line);
        vals.push_back((*param_tys)[i]->kind == TyKind::Struct ? conv.ll : llv(conv));
    }
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
    if (node.type_args.empty() || node.args.size() != 1) {
        throw Unsupported("load(<T>, p) 的形态", node.src_line);
    }
    TyRef ty = ty_of_ref(*node.type_args[0], node.src_line);
    return load_addr(to_ptr(e(*node.args[0])), ty);
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
        amt = LLVMBuildAnd(bd_, amt, const_int(63), "amt");
    } else {
        amt = LLVMBuildSelect(
            bd_,
            LLVMBuildICmp(bd_, LLVMIntULT, amt, const_int(uint64_t(bits)), "lt"), amt,
            const_int(uint64_t(bits - 1)), "sat");
    }
    LLVMValueRef ll = LLVMBuildAShr(bd_, sext64(llv(x), bits), amt, "asr");
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
    return V::instr(LLVMBuildSub(bd_, call, const_int(uint64_t(64 - bits)), "clz"), x.ty);
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
    LLVMValueRef amt = LLVMBuildAnd(bd_, llv(n), const_int(sm), "amt");
    LLVMValueRef inv = LLVMBuildAnd(
        bd_, LLVMBuildSub(bd_, const_int(uint64_t(bits)), amt, "amtc"), const_int(sm), "amti");
    LLVMValueRef lo = LLVMBuildLShr(bd_, v, amt, "rr");
    LLVMValueRef hi = LLVMBuildShl(bd_, v, inv, "rl");
    return V::instr(mask_ll(LLVMBuildOr(bd_, lo, hi, "ror"), bits), x.ty);
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
    LLVMValueRef cnd = LLVMBuildICmp(bd_, pred, x, y, "mm");
    LLVMValueRef ll = LLVMBuildSelect(bd_, cnd, llv(a), llv(b), "mmv");
    return V::instr(mask_ll(ll, ty->bits), ty);
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
        return V::instr(LLVMBuildMul(bd_, llv(obj), const_int(kNsPerMs), "ns"), ty_time());
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
    LLVMValueRef d = LLVMBuildSub(bd_, llv(ft), const_int(kFileTimeUnix), "d");
    LLVMBuildRet(bd_, LLVMBuildMul(bd_, d, const_int(100), "ns"));
}

void Lower::body_sleep() {
    LLVMValueRef ms =
        LLVMBuildUDiv(bd_, LLVMGetParam(fnref_, 0), const_int(kNsPerMs), "ms");
    LLVMValueRef ext = get_extern("Sleep", ty_void(), {make_int(32, false)});
    emit_call(ext, {LLVMBuildTrunc(bd_, ms, storage_ll(*make_int(32, false)), "m")});
    LLVMBuildRetVoid(bd_);
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
    *n_out = LLVMGetParam(fnref_, n_param);
    return slot;
}

void Lower::body_memory_clear() {
    LLVMValueRef addr = LLVMBuildIntToPtr(bd_, LLVMGetParam(fnref_, 0), ptr_, "ap");
    LLVMBasicBlockRef head = nullptr;
    LLVMValueRef n = nullptr;
    LLVMValueRef slot = byte_loop_prologue(1, &head, &n);
    V i = load_addr(slot, ty_i64());
    LLVMValueRef cnd = LLVMBuildICmp(bd_, LLVMIntULT, llv(i), n, "lt");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), fnref_, "lpb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), fnref_, "lpd");
    LLVMBuildCondBr(bd_, cnd, body, done);
    LLVMPositionBuilderAtEnd(bd_, body);
    cur_block_ = body;
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    std::vector<LLVMValueRef> gi{llv(i)};
    LLVMValueRef p = LLVMBuildGEP2(bd_, i8, addr, gi.data(), 1, "p");
    LLVMValueRef st = LLVMBuildStore(bd_, LLVMConstInt(i8, 0, 0), p);
    LLVMSetAlignment(st, 1);
    LLVMSetVolatile(st, 1);
    store_addr(slot, V::instr(LLVMBuildAdd(bd_, llv(i), const_int(1), "i1"), ty_i64()), ty_i64(),
               0);
    LLVMBuildBr(bd_, head);
    LLVMPositionBuilderAtEnd(bd_, done);
    cur_block_ = done;
    LLVMBuildRetVoid(bd_);
}

void Lower::body_memory_copy() {
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMValueRef src = LLVMBuildIntToPtr(bd_, LLVMGetParam(fnref_, 0), ptr_, "sp0");
    LLVMValueRef dst = LLVMBuildIntToPtr(bd_, LLVMGetParam(fnref_, 1), ptr_, "dp0");
    LLVMBasicBlockRef head = nullptr;
    LLVMValueRef n = nullptr;
    LLVMValueRef slot = byte_loop_prologue(2, &head, &n);
    V i = load_addr(slot, ty_i64());
    LLVMValueRef cnd = LLVMBuildICmp(bd_, LLVMIntULT, llv(i), n, "lt");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), fnref_, "lpb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), fnref_, "lpd");
    LLVMBuildCondBr(bd_, cnd, body, done);
    LLVMPositionBuilderAtEnd(bd_, body);
    cur_block_ = body;
    std::vector<LLVMValueRef> gi{llv(i)};
    LLVMValueRef s = LLVMBuildGEP2(bd_, i8, src, gi.data(), 1, "sp");
    LLVMValueRef d = LLVMBuildGEP2(bd_, i8, dst, gi.data(), 1, "dp");
    LLVMValueRef ld = LLVMBuildLoad2(bd_, i8, s, "b");
    LLVMSetAlignment(ld, 1);
    LLVMSetVolatile(ld, 1);
    LLVMValueRef st = LLVMBuildStore(bd_, ld, d);
    LLVMSetAlignment(st, 1);
    LLVMSetVolatile(st, 1);
    store_addr(slot, V::instr(LLVMBuildAdd(bd_, llv(i), const_int(1), "i1"), ty_i64()), ty_i64(),
               0);
    LLVMBuildBr(bd_, head);
    LLVMPositionBuilderAtEnd(bd_, done);
    cur_block_ = done;
    LLVMBuildRetVoid(bd_);
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
    FnState saved = save_state();
    cur_name_ = name;
    begin_function(fnref, ret_ty);
    if (name == "get_time") body_get_time();
    else if (name == "sleep") body_sleep();
    else if (name == "memory_clear") body_memory_clear();
    else if (name == "memory_copy") body_memory_copy();
    else throw Unsupported("前缀函数 " + name, 0);
    finish_function();
    restore_state(saved);
    return fnref;
}

LLVMValueRef Lower::ensure_def_fn(const std::string& name, int64_t lo, const FunctionDef& d,
                                  const TyRef& ret_ty, const std::vector<TyRef>& param_tys) {
    auto key = std::make_tuple(static_cast<int>(1), name, lo, std::string());
    auto hit = fns_.find(key);
    if (hit != fns_.end()) return hit->second.fnref;
    std::vector<LLVMTypeRef> ptypes;
    for (const auto& t : param_tys) ptypes.push_back(val_ll(*t));
    LLVMTypeRef rty = ret_ty->kind == TyKind::Void ? void_ty_ : val_ll(*ret_ty);
    const std::string gname =
        defs_.count(name) != 0 && defs_[name].size() == 1
            ? name
            : name + "#L" + std::to_string(lo);
    LLVMValueRef fnref = LLVMAddFunction(
        module(), gname.c_str(),
        LLVMFunctionType(rty, ptypes.data(), static_cast<unsigned>(ptypes.size()), 0));
    LLVMSetLinkage(fnref, LLVMInternalLinkage);
    fns_[key] = Fn{fnref, ret_ty, param_tys, {}};
    fn_seen_++;
    FnState saved = save_state();
    cur_name_ = name;
    cur_def_ = &d;
    begin_function(fnref, ret_ty);
    for (size_t i = 0; i < d.params.size(); ++i) {
        if (i >= param_tys.size()) throw Unsupported(d.name + "() 的参数表对不上", d.lo);
        const TyRef& ty = param_tys[i];
        LLVMValueRef a = alloca_slot(ty, d.params[i]->name);
        store_addr(a, V::instr(LLVMGetParam(fnref, static_cast<unsigned>(i)), ty), ty,
                   d.params[i]->src_line);
        bind_root_scope(d.params[i]->name, Binding::local(a, ty));
    }
    lower_block(*d.body);
    finish_function();
    restore_state(saved);
    return fnref;
}

LLVMValueRef Lower::ensure_nested_fn(const DefStmt& st, const TyRef& ret_ty,
                                     const std::vector<TyRef>& param_tys,
                                     const Closure& closure) {
    const std::vector<Capture>& caps = closure.caps;
    const std::string owner = cur_def_ != nullptr ? cur_def_->name : std::string("closure");
    auto key = std::make_tuple(static_cast<int>(2), cur_name_, int64_t(st.src_line), st.name);
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
    FnState saved = save_state();
    cur_name_ = owner + "$" + st.name;
    begin_function(fnref, ret_ty);
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
    restore_state(saved);
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
            return convert(cv(*n.expr), ty_of_ref(*n.target, n.src_line), n.src_line);
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
    LLVMValueRef lp = LLVMBuildGEP2(bd_, storage_ll(*make_int(8, false)), to_ptr(obj), gi.data(), 1,
                                    "lp");
    V raw = load_addr(lp, make_int(64, false));
    return LLVMBuildLShr(bd_, llv(raw), const_int(16), "len");
}

/* 栈上一块带头的串缓冲：`buf[0..8)` 是头、payload = `buf+8`。 */
V Lower::str_concat(const V& a, const V& b, int line) {
    LLVMTypeRef i8 = storage_ll(*make_int(8, false));
    LLVMValueRef la = len_of(a, line);
    LLVMValueRef lb = len_of(b, line);
    LLVMValueRef total = LLVMBuildAdd(bd_, la, lb, "sl");
    LLVMValueRef buf = LLVMBuildArrayAlloca(bd_, i8, LLVMBuildAdd(bd_, total, const_int(8), "sb"),
                                            "sbuf");
    LLVMSetAlignment(buf, 8);
    std::vector<LLVMValueRef> g8{const_int(8)};
    LLVMValueRef payload = LLVMBuildGEP2(bd_, i8, buf, g8.data(), 1, "sp");
    LLVMValueRef copy = ensure_copy_fn();
    LLVMValueRef cur = emit_call(copy, {payload, to_ptr(a), la});
    emit_call(copy, {cur, to_ptr(b), lb});
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef head = LLVMBuildGEP2(bd_, i8, buf, g0.data(), 1, "shdr");
    store_addr(head, V::instr(LLVMBuildShl(bd_, total, const_int(16), "shl"), ty_i64()), ty_i64(),
               line);
    return V::instr(payload, ty_str());
}

V Lower::template_expr(const TemplateExpr& node) {
    std::vector<V> vals;
    vals.reserve(node.parts.size());
    for (const ExprPtr& p : node.parts) vals.push_back(e(*p));

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
            if (v.ty->kind == TyKind::Str) size = LLVMBuildAdd(bd_, size, len_of(v, node.src_line), "ts");
        }
        buf = LLVMBuildArrayAlloca(bd_, i8, size, "tbuf");
        LLVMSetAlignment(buf, 8);
    }
    std::vector<LLVMValueRef> g8{const_int(8)};
    LLVMValueRef payload = LLVMBuildGEP2(bd_, i8, buf, g8.data(), 1, "tpl");
    LLVMValueRef cur = payload;
    for (std::size_t i = 0; i < node.texts.size(); ++i) {
        cur = write_literal(cur, node.texts[i], node.src_line);
        if (i < vals.size()) cur = render_part(cur, vals[i], node.src_line);
    }
    /* 长度按**实际游标**算，不按缓冲尺寸 —— 所以尺寸只是个上界，不需要预先知道十进制位数。 */
    LLVMValueRef len = LLVMBuildSub(bd_, LLVMBuildPtrToInt(bd_, cur, i64_, "tce"),
                                    LLVMBuildPtrToInt(bd_, payload, i64_, "tpe"), "tlen");
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef head = LLVMBuildGEP2(bd_, i8, buf, g0.data(), 1, "thdr");
    store_addr(head, V::instr(LLVMBuildShl(bd_, len, const_int(16), "thl"), ty_i64()), ty_i64(),
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
        LLVMValueRef p = LLVMBuildGEP2(bd_, i8, cur, gi.data(), 1, "lp");
        LLVMValueRef st =
            LLVMBuildStore(bd_, LLVMConstInt(i8, static_cast<unsigned char>(text[k]), 0), p);
        LLVMSetAlignment(st, 1);
    }
    std::vector<LLVMValueRef> gn{const_int(text.size())};
    return LLVMBuildGEP2(bd_, i8, cur, gn.data(), 1, "lc");
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
        LLVMValueRef nz = LLVMBuildICmp(bd_, LLVMIntNE, llv(v), const_int(0), "bn");
        for (std::size_t k = 0; k < 4; ++k) {
            std::vector<LLVMValueRef> gi{const_int(k)};
            LLVMValueRef p = LLVMBuildGEP2(bd_, i8, cur, gi.data(), 1, "bp");
            LLVMValueRef ch = LLVMBuildSelect(
                bd_, nz, const_int(static_cast<unsigned char>(yes[k])),
                const_int(static_cast<unsigned char>(no[k])), "bc");
            LLVMValueRef st = LLVMBuildStore(bd_, LLVMBuildTrunc(bd_, ch, i8, "bt"), p);
            LLVMSetAlignment(st, 1);
        }
        std::vector<LLVMValueRef> g4{const_int(4)};
        LLVMValueRef p4 = LLVMBuildGEP2(bd_, i8, cur, g4.data(), 1, "bp");
        LLVMValueRef st4 = LLVMBuildStore(bd_, LLVMConstInt(i8, 'e', 0), p4);
        LLVMSetAlignment(st4, 1);
        std::vector<LLVMValueRef> gn{
            LLVMBuildSelect(bd_, nz, const_int(4), const_int(5), "bl")};
        return LLVMBuildGEP2(bd_, i8, cur, gn.data(), 1, "bend");
    }
    if (v.ty->kind == TyKind::Int || v.ty->kind == TyKind::Ptr) {
        LLVMValueRef x = llv(v);
        LLVMValueRef minus = const_int(0);
        if (v.ty->kind == TyKind::Int) {
            x = mask_ll(x, v.ty->bits);
            if (v.ty->is_signed) {
                x = sext64(x, v.ty->bits);
                LLVMValueRef neg = LLVMBuildICmp(bd_, LLVMIntSLT, x, const_int(0), "dneg");
                minus = LLVMBuildZExt(bd_, neg, i64_, "dm");
                /* 幅度按**无符号取负**算 —— 这样 S64 最小值也对（实测：`-9223372036854775808`）。 */
                x = LLVMBuildSelect(bd_, neg, LLVMBuildSub(bd_, const_int(0), x, "dmg"), x, "dv");
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
    FnState saved = save_state();
    cur_name_ = "__simplexc_copy";
    begin_function(fn, ty_ptr());
    LLVMValueRef dst = LLVMGetParam(fn, 0);
    LLVMValueRef src = LLVMGetParam(fn, 1);
    LLVMValueRef n = LLVMGetParam(fn, 2);
    LLVMValueRef slot = alloca_slot(ty_i64(), "ci");
    store_addr(slot, V::instr(const_int(0), ty_i64()), ty_i64(), 0);
    LLVMBasicBlockRef head = br_new_block("ch");
    LLVMValueRef i = llv(load_addr(slot, ty_i64()));
    LLVMValueRef lt = LLVMBuildICmp(bd_, LLVMIntULT, i, n, "clt");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), fn, "cb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), fn, "cd");
    LLVMBuildCondBr(bd_, lt, body, done);
    LLVMPositionBuilderAtEnd(bd_, body);
    cur_block_ = body;
    std::vector<LLVMValueRef> gi{i};
    LLVMValueRef sp = LLVMBuildGEP2(bd_, i8, src, gi.data(), 1, "csp");
    LLVMValueRef dp = LLVMBuildGEP2(bd_, i8, dst, gi.data(), 1, "cdp");
    LLVMValueRef b = LLVMBuildLoad2(bd_, i8, sp, "cb0");
    LLVMSetAlignment(b, 1);
    LLVMValueRef st = LLVMBuildStore(bd_, b, dp);
    LLVMSetAlignment(st, 1);
    store_addr(slot, V::instr(LLVMBuildAdd(bd_, i, const_int(1), "ci1"), ty_i64()), ty_i64(), 0);
    LLVMBuildBr(bd_, head);
    LLVMPositionBuilderAtEnd(bd_, done);
    cur_block_ = done;
    std::vector<LLVMValueRef> gn{n};
    LLVMBuildRet(bd_, LLVMBuildGEP2(bd_, i8, dst, gn.data(), 1, "cr"));
    finish_function();
    restore_state(saved);
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
    FnState saved = save_state();
    cur_name_ = "__simplexc_dec";
    begin_function(fn, ty_ptr());
    LLVMValueRef p = LLVMGetParam(fn, 0);
    LLVMValueRef v = LLVMGetParam(fn, 1);
    LLVMValueRef minus = LLVMGetParam(fn, 2);
    /* 位数：19 次比较（10^1…10^19），不写循环 —— 每个插值点都要走这一段，形状越平越好。 */
    LLVMValueRef n = const_int(1);
    uint64_t thr = 1;
    for (int d = 1; d <= 19; ++d) {
        thr *= 10;
        LLVMValueRef ge = LLVMBuildICmp(bd_, LLVMIntUGE, v, const_int(thr), "dge");
        n = LLVMBuildSelect(bd_, ge, const_int(static_cast<uint64_t>(d + 1)), n, "dn");
    }
    LLVMValueRef total = LLVMBuildAdd(bd_, n, minus, "dtot");
    std::vector<LLVMValueRef> gt{total};
    LLVMValueRef end = LLVMBuildGEP2(bd_, i8, p, gt.data(), 1, "dend");
    LLVMValueRef has_minus = LLVMBuildICmp(bd_, LLVMIntNE, minus, const_int(0), "dhm");
    LLVMBasicBlockRef neg = LLVMAppendBasicBlockInContext(ctx(), fn, "dneg");
    LLVMBasicBlockRef head = LLVMAppendBasicBlockInContext(ctx(), fn, "dh");
    LLVMBuildCondBr(bd_, has_minus, neg, head);
    LLVMPositionBuilderAtEnd(bd_, neg);
    cur_block_ = neg;
    std::vector<LLVMValueRef> g0{const_int(0)};
    LLVMValueRef p0 = LLVMBuildGEP2(bd_, i8, p, g0.data(), 1, "dp0");
    LLVMValueRef sm = LLVMBuildStore(bd_, LLVMConstInt(i8, '-', 0), p0);
    LLVMSetAlignment(sm, 1);
    LLVMBuildBr(bd_, head);
    LLVMPositionBuilderAtEnd(bd_, head);
    cur_block_ = head;
    /* 从末尾往前填：先算长度，所以 `i` 初值是总长，每轮先减再写。 */
    LLVMValueRef slot_i = alloca_slot(ty_i64(), "di");
    store_addr(slot_i, V::instr(total, ty_i64()), ty_i64(), 0);
    LLVMValueRef slot_x = alloca_slot(ty_i64(), "dx");
    store_addr(slot_x, V::instr(v, ty_i64()), ty_i64(), 0);
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx(), fn, "db");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx(), fn, "dd");
    LLVMBuildBr(bd_, body);
    LLVMPositionBuilderAtEnd(bd_, body);
    cur_block_ = body;
    LLVMValueRef i2 = LLVMBuildSub(bd_, llv(load_addr(slot_i, ty_i64())), const_int(1), "di2");
    store_addr(slot_i, V::instr(i2, ty_i64()), ty_i64(), 0);
    LLVMValueRef x = llv(load_addr(slot_x, ty_i64()));
    LLVMValueRef dg = LLVMBuildURem(bd_, x, const_int(10), "ddg");
    LLVMValueRef ch = LLVMBuildAdd(bd_, dg, const_int('0'), "dch");
    std::vector<LLVMValueRef> gi{i2};
    LLVMValueRef dp = LLVMBuildGEP2(bd_, i8, p, gi.data(), 1, "ddp");
    LLVMValueRef st = LLVMBuildStore(bd_, LLVMBuildTrunc(bd_, ch, i8, "dbt"), dp);
    LLVMSetAlignment(st, 1);
    LLVMValueRef x2 = LLVMBuildUDiv(bd_, x, const_int(10), "dqx");
    store_addr(slot_x, V::instr(x2, ty_i64()), ty_i64(), 0);
    LLVMValueRef more = LLVMBuildICmp(bd_, LLVMIntNE, x2, const_int(0), "dmr");
    LLVMBuildCondBr(bd_, more, body, done);
    LLVMPositionBuilderAtEnd(bd_, done);
    cur_block_ = done;
    LLVMBuildRet(bd_, end);
    finish_function();
    restore_state(saved);
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
    LLVMTypeRef arrty = LLVMArrayType2(el, elems.size());
    const std::string gname = ".arr" + std::to_string(node.src_line);
    LLVMValueRef g = LLVMAddGlobal(module(), arrty, gname.c_str());
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(g, 1);
    LLVMSetInitializer(g, LLVMConstArray2(el, elems.data(), elems.size()));
    return V::instr(g, make_array(ty));
}

/* 元素（结构体字段）的**存储形态**值 —— 与 `store_addr` 同一套口径：整数按字段宽度截断
 * （窄写只写该宽度，U-L1），非整数原样。 */
LLVMValueRef Lower::field_storage_value(const V& v, const TyRef& ty, int line) {
    if (!is_int(*ty)) return llv(v);
    V conv = convert(v, ty, line);
    LLVMTypeRef st = storage_ll(*ty);
    LLVMValueRef ll = llv(conv);
    if (st != i64_) ll = LLVMBuildTrunc(bd_, ll, st, "t");
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
    const bool in_function = bd_ != nullptr && cur_block_ != nullptr;
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
    if (bd_ == nullptr || cur_block_ == nullptr) {
        /* 常量环境里出现**没有常量值**的字段（数组字面量那种「有 ll、无 c」）：不许在 nullptr
         * 上发指令。 */
        throw Unsupported("复合字面量里的字段值不是编译期常量", node.src_line);
    }
    LLVMPositionBuilderAtEnd(bd_, cur_block_);
    LLVMValueRef agg = LLVMConstNull(it->second);
    for (const FieldVal& fv : vals) {
        const TyRef& fty = ty->fields[fv.idx].second;
        unsigned pos = static_cast<unsigned>(fv.idx);
        LLVMValueRef el = fv.v.c ? const_for_storage(fv.v)
                                 : field_storage_value(fv.v, fty, node.src_line);
        agg = LLVMBuildInsertValue(bd_, agg, el, pos, "cl");
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
    FnState saved = save_state();
    cur_name_ = "tc_main";
    cur_def_ = nullptr;
    begin_function(fnref, ty_void());
    lower_block(*label.body);
    /* 程序根末尾隐式 thread_exit（接口 §1「程序根」）。 */
    LLVMValueRef ext = get_extern("thread_exit", ty_void(), {});
    emit_call(ext, {});
    LLVMBuildRetVoid(bd_);
    finish_function();
    restore_state(saved);
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
