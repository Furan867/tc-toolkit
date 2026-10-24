/* sema/hir_dump.cc —— HIR 的文本 dump 与六条不变量检查。
 *
 * 两个东西同住一处：都是**只读遍历**，且都只在判据/自检里用（不进产品路径）。dump 的实现先写，
 * 检查在后 —— 检查的报错路径（`funcs[0].body.kids[2]`）与 dump 的树形一一对应，出问题能直接对着看。
 *
 * dump 的字段形状（改它 = 改判据，改之前跟 ②c 的写者对齐）：
 *
 *   Hir funcs=… globals=… externs=… consts=… strings=…
 *     string[i] bytes="…" cstring=…
 *     global[i] name="…" ty=…
 *     extern[i] name="…" ty=…
 *     const[i] name="…" ty=… value=0x…
 *     main key="…" name="…" ret=… params=N caps=N
 *       param[i] name="…" ty=…
 *       capture[i] name="…" ty=… index=…
 *       <body 那棵树的根>
 *     func[i] key="…" name="…" ret=… params=N caps=N
 *       …
 *
 * 每个节点一行 `Kind 字段… span=行:列`，字段顺序固定、缩进 2 空格/层、子树跟在父行之后。
 * 不打印指针、地址、容量 —— dump 只能随语义变化。
 *
 * 类型拼写（`U32`/`S8`/`Bool`/`Arr[U8]`/`Struct.名字`）**只给 dump 看**，不是决策 7 的实例键
 * 规范化串（那个唯一一处是 `lower.cc` 的 `ty_key`，②b 搬进 sema 时一并收口）。
 */
#include "hir_dump.hh"

#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace simplexc {
namespace sema {
namespace {

const char* hkind_name(HKind k) {
    switch (k) {
        case HKind::Const: return "Const";
        case HKind::StrLit: return "StrLit";
        case HKind::Local: return "Local";
        case HKind::Global: return "Global";
        case HKind::Capture: return "Capture";
        case HKind::Unary: return "Unary";
        case HKind::Binary: return "Binary";
        case HKind::Cond: return "Cond";
        case HKind::Call: return "Call";
        case HKind::Member: return "Member";
        case HKind::Index: return "Index";
        case HKind::Cast: return "Cast";
        case HKind::ArrayLit: return "ArrayLit";
        case HKind::CompositeLit: return "CompositeLit";
        case HKind::Template: return "Template";
        case HKind::Block: return "Block";
        case HKind::VarDecl: return "VarDecl";
        case HKind::Assign: return "Assign";
        case HKind::ExprStmt: return "ExprStmt";
        case HKind::If: return "If";
        case HKind::While: return "While";
        case HKind::Switch: return "Switch";
        case HKind::Break: return "Break";
        case HKind::Return: return "Return";
    }
    return "?";
}

const char* hunop_name(HUnOp op) {
    switch (op) {
        case HUnOp::Neg: return "neg";
        case HUnOp::Not: return "not";
        case HUnOp::BitNot: return "bitnot";
    }
    return "?";
}

const char* hbinop_name(HBinOp op) {
    switch (op) {
        case HBinOp::Add: return "add";
        case HBinOp::Sub: return "sub";
        case HBinOp::Mul: return "mul";
        case HBinOp::Div: return "div";
        case HBinOp::Rem: return "rem";
        case HBinOp::BitAnd: return "bitand";
        case HBinOp::BitOr: return "bitor";
        case HBinOp::BitXor: return "bitxor";
        case HBinOp::Shl: return "shl";
        case HBinOp::Shr: return "shr";
        case HBinOp::Eq: return "eq";
        case HBinOp::Ne: return "ne";
        case HBinOp::Lt: return "lt";
        case HBinOp::Le: return "le";
        case HBinOp::Gt: return "gt";
        case HBinOp::Ge: return "ge";
        case HBinOp::LogAnd: return "logand";
        case HBinOp::LogOr: return "logor";
    }
    return "?";
}

const char* hcast_name(HCastKind k) {
    switch (k) {
        case HCastKind::Trunc: return "trunc";
        case HCastKind::Sext: return "sext";
        case HCastKind::Zext: return "zext";
        case HCastKind::PtrToInt: return "ptrtoint";
        case HCastKind::IntToPtr: return "inttoptr";
        case HCastKind::Raw: return "raw";
    }
    return "?";
}

/* ---------------------------------------------------------------- dump 的小件 */

std::string indent_of(int depth) {
    return std::string(static_cast<std::size_t>(depth) * 2, ' ');
}

/* 双引号 + 转义：串池里可能带换行/引号，直接打出来会破坏「一行一节点」。 */
std::string quoted(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20 || u == 0x7f) {
            out += "\\x";
            out.push_back(hex[(u >> 4) & 0xF]);
            out.push_back(hex[u & 0xF]);
        } else {
            out.push_back(c);
        }
    }
    out += "\"";
    return out;
}

std::string hex64(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

std::string ty_text(const TyRef& ty) {
    if (ty == nullptr) return "?";
    switch (ty->kind) {
        case TyKind::Void: return "Void";
        case TyKind::Int:
            if (ty->is_bool) return "Bool";
            return std::string(ty->is_signed ? "S" : "U") + std::to_string(ty->bits);
        case TyKind::Ptr: return "Ptr";
        case TyKind::Str: return "Str";
        case TyKind::Enum: return "Enum." + ty->name;
        case TyKind::Struct: return "Struct." + ty->name;
        case TyKind::Array: return "Arr[" + ty_text(ty->elem) + "]";
    }
    return "?";
}

void dump_node(std::string& out, const HNode& n, int depth) {
    out += indent_of(depth);
    out += hkind_name(n.kind);
    switch (n.kind) {
        case HKind::Const:
            out += " ty=" + ty_text(n.ty) + " const=" + hex64(n.const_bits);
            break;
        case HKind::StrLit:
            out += " ty=" + ty_text(n.ty) + " string=" + std::to_string(n.string_id);
            break;
        case HKind::Local:
        case HKind::VarDecl:
            out += " ty=" + ty_text(n.ty) + " name=" + quoted(n.name);
            break;
        case HKind::Global:
            out += " ty=" + ty_text(n.ty) + " global=" + std::to_string(n.global_id);
            break;
        case HKind::Capture:
            out += " ty=" + ty_text(n.ty) + " capture=" + std::to_string(n.capture_index);
            break;
        case HKind::Unary:
            out += " ty=" + ty_text(n.ty) + " op=" + hunop_name(n.un_op);
            break;
        case HKind::Binary:
            out += " ty=" + ty_text(n.ty) + " op=" + hbinop_name(n.bin_op);
            break;
        case HKind::Cond:
        case HKind::Index:
        case HKind::ArrayLit:
        case HKind::CompositeLit:
        case HKind::Template:
            out += " ty=" + ty_text(n.ty);
            break;
        case HKind::Call:
            out += " ty=" + ty_text(n.ty) + " callee=";
            out += (n.callee.kind == HCalleeKind::Func ? "func:" : "builtin:");
            out += std::to_string(n.callee.index);
            break;
        case HKind::Member:
            out += " ty=" + ty_text(n.ty) + " field=" + std::to_string(n.field_index) +
                   " offset=" + std::to_string(n.offset);
            break;
        case HKind::Cast:
            out += " ty=" + ty_text(n.ty) + " cast=" + hcast_name(n.cast_kind);
            break;
        case HKind::While:
        case HKind::Break:
            out += " label=" + quoted(n.label);
            break;
        case HKind::Block:
        case HKind::Assign:
        case HKind::ExprStmt:
        case HKind::If:
        case HKind::Switch:
        case HKind::Return:
            break;
    }
    out += " span=" + std::to_string(n.span.line) + ":" + std::to_string(n.span.col) + "\n";
    for (const HNode& kid : n.kids) dump_node(out, kid, depth + 1);
}

void dump_func(std::string& out, const char* tag, const HirFunc& fn, int depth) {
    out += indent_of(depth) + tag + " key=" + quoted(fn.key) + " name=" + quoted(fn.name) +
           " ret=" + ty_text(fn.ret) + " params=" + std::to_string(fn.params.size()) +
           " caps=" + std::to_string(fn.caps.size()) + "\n";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        out += indent_of(depth + 1) + "param[" + std::to_string(i) + "] name=" +
               quoted(fn.params[i].name) + " ty=" + ty_text(fn.params[i].ty) + "\n";
    }
    for (std::size_t i = 0; i < fn.caps.size(); ++i) {
        out += indent_of(depth + 1) + "capture[" + std::to_string(i) + "] name=" +
               quoted(fn.caps[i].name) + " ty=" + ty_text(fn.caps[i].ty) +
               " index=" + std::to_string(fn.caps[i].index) + "\n";
    }
    dump_node(out, fn.body, depth + 1);
}

/* ---------------------------------------------------------------- 检查的小件
 *
 * 报错格式固定成 `不变量#N：<路径> —— <为什么>`：N 是规格里那条的编号（自检逐条断言它），
 * 路径与 dump 的树形同构（`funcs[0].body.kids[2]`）。
 */

bool fail(std::string* err, int inv, const std::string& where, const std::string& why) {
    *err = "不变量#" + std::to_string(inv) + "：" + where + " —— " + why;
    return false;
}

std::string kid_path(const std::string& where, std::size_t i) {
    return where + ".kids[" + std::to_string(i) + "]";
}

std::string index_path(const std::string& where, const char* field, std::size_t i) {
    return where + "." + field + "[" + std::to_string(i) + "]";
}

/* `main` 与 `funcs[]` 一起遍历：检查的每条都要两者都过，路径前缀不同。 */
std::vector<std::pair<std::string, const HirFunc*>> all_funcs(const Hir& hir) {
    std::vector<std::pair<std::string, const HirFunc*>> out;
    out.emplace_back("main", &hir.main);
    for (std::size_t i = 0; i < hir.funcs.size(); ++i) {
        out.emplace_back("funcs[" + std::to_string(i) + "]", &hir.funcs[i]);
    }
    return out;
}

/* 结构相等。类型**不是** interned（`make_int(64,false)` 每次都是新对象），所以不能比指针。
 * 这不是决策 7 的规范化函数；②b 把 `ty_key` 搬进 sema 之后，两者可以合成一处。 */
bool same_ty(const TyRef& a, const TyRef& b) {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    if (a->kind != b->kind || a->bits != b->bits || a->is_signed != b->is_signed ||
        a->is_bool != b->is_bool || a->name != b->name) {
        return false;
    }
    if (a->fields.size() != b->fields.size()) return false;
    for (std::size_t i = 0; i < a->fields.size(); ++i) {
        if (a->fields[i].first != b->fields[i].first) return false;
        if (!same_ty(a->fields[i].second, b->fields[i].second)) return false;
    }
    return same_ty(a->elem, b->elem);
}

bool is_bool_ty(const TyRef& ty) {
    return ty != nullptr && ty->kind == TyKind::Int && ty->is_bool;
}

/* ① 类型位：表达式有值类型、语句没有 —— 唯一例外是 `VarDecl`，它的 `ty` 是被声明者的类型
 *    （语句没有值，但 alloca 的宽度得有个地方放，见 `hir.hh` 的形状决定 2）。
 *    函数签名的三个类型位（ret/参数/捕获）一并查 —— 它们为空时 ④ 会直接解引用，
 *    所以必须先在 ① 里挡住。 */
bool check_types_node(const HNode& n, const std::string& where, std::string* err) {
    if (is_expr(n.kind)) {
        if (n.ty == nullptr) {
            return fail(err, 1, where, std::string(hkind_name(n.kind)) + " 没有值类型");
        }
    } else if (n.kind == HKind::VarDecl) {
        if (n.ty == nullptr) return fail(err, 1, where, "VarDecl 没有声明类型");
    } else if (n.ty != nullptr) {
        return fail(err, 1, where, std::string("语句 ") + hkind_name(n.kind) + " 不该带值类型");
    }
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        if (!check_types_node(n.kids[i], kid_path(where, i), err)) return false;
    }
    return true;
}

bool check_types_func(const HirFunc& fn, const std::string& where, std::string* err) {
    if (fn.ret == nullptr) return fail(err, 1, where, "函数的 ret 没有类型");
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (fn.params[i].ty == nullptr) {
            return fail(err, 1, index_path(where, "params", i), "参数没有类型");
        }
    }
    for (std::size_t i = 0; i < fn.caps.size(); ++i) {
        if (fn.caps[i].ty == nullptr) {
            return fail(err, 1, index_path(where, "caps", i), "捕获没有类型");
        }
    }
    if (fn.body.kind != HKind::Block) {
        return fail(err, 1, where + ".body", "函数体必须是 Block");
    }
    return check_types_node(fn.body, where + ".body", err);
}

/* ② 引用：局部名按作用域查（`VarDecl` 声明之后才可见，与参考的块作用域一致），
 *    全局/捕获查表的大小。 */
using ScopeStack = std::vector<std::set<std::string>>;

bool in_scope(const ScopeStack& scopes, const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (it->count(name) != 0) return true;
    }
    return false;
}

bool check_refs_node(const HNode& n, ScopeStack& scopes, const HirFunc& fn, std::size_t nglobals,
                     const std::string& where, std::string* err) {
    switch (n.kind) {
        case HKind::Local:
            if (!in_scope(scopes, n.name)) {
                return fail(err, 2, where, "局部名 " + quoted(n.name) + " 不在作用域里");
            }
            break;
        case HKind::Global:
            if (n.global_id < 0 || static_cast<std::size_t>(n.global_id) >= nglobals) {
                return fail(err, 2, where,
                            "global 下标 " + std::to_string(n.global_id) + " 越界（globals=" +
                                std::to_string(nglobals) + "）");
            }
            break;
        case HKind::Capture:
            if (n.capture_index < 0 || static_cast<std::size_t>(n.capture_index) >= fn.caps.size()) {
                return fail(err, 2, where,
                            "capture 序号 " + std::to_string(n.capture_index) + " 越界（caps=" +
                                std::to_string(fn.caps.size()) + "）");
            }
            break;
        case HKind::Block: {
            scopes.emplace_back();
            for (std::size_t i = 0; i < n.kids.size(); ++i) {
                if (!check_refs_node(n.kids[i], scopes, fn, nglobals, kid_path(where, i), err)) {
                    return false;
                }
            }
            scopes.pop_back();
            return true;
        }
        default:
            break;
    }
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        if (!check_refs_node(n.kids[i], scopes, fn, nglobals, kid_path(where, i), err)) return false;
    }
    /* 初始化式里的同名引用取**外层**的那个（`var x = x`），所以声明放在子节点之后。 */
    if (n.kind == HKind::VarDecl && !n.name.empty()) scopes.back().insert(n.name);
    return true;
}

bool check_refs_func(const HirFunc& fn, std::size_t nglobals, const std::string& where,
                     std::string* err) {
    ScopeStack scopes;
    scopes.emplace_back();
    for (const HParam& p : fn.params) scopes.back().insert(p.name);
    for (const HCapture& c : fn.caps) scopes.back().insert(c.name);
    return check_refs_node(fn.body, scopes, fn, nglobals, where + ".body", err);
}

/* ③ 被调者：实例下标进 `funcs`（闭包没收敛就会指到不存在的实例），内建编号非负。 */
bool check_calls_node(const HNode& n, std::size_t nfuncs, const std::string& where,
                      std::string* err) {
    if (n.kind == HKind::Call) {
        if (n.callee.kind == HCalleeKind::Func) {
            if (n.callee.index < 0 || static_cast<std::size_t>(n.callee.index) >= nfuncs) {
                return fail(err, 3, where,
                            "被调实例 funcs[" + std::to_string(n.callee.index) + "] 不在 funcs（" +
                                std::to_string(nfuncs) + " 个）里 —— 实例闭包没收敛");
            }
        } else if (n.callee.index < 0) {
            return fail(err, 3, where, "内建编号 " + std::to_string(n.callee.index) + " < 0");
        }
    }
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        if (!check_calls_node(n.kids[i], nfuncs, kid_path(where, i), err)) return false;
    }
    return true;
}

/* ④ 无隐式转换 + 操作数形状。
 *
 * 查的范围就是参考实现里 `convert`/`unify` 出现过的那几个接缝（`lower.cc`）：
 * 一元、二元（算术/比较/逻辑）、`Cond` 的两臂、`Assign` 的两侧、`VarDecl` 的初始化式、
 * `Return` 的值。这些地方的隐式转换必须已经是 `Cast` 节点，所以「两侧类型相等」成立。
 * 不查的：`Call` 的实参（要按被调者的签名，②c 之后 codegen 自己读签名）、`Member`/`Index`
 * 的结果类型（要从布局表推）。
 *
 * 操作数个数也在这里查：个数不对时「两侧类型一致」无从谈起，所以它比 ① 更适合放在这一条。
 */
bool expect_kids(const HNode& n, std::size_t lo, std::size_t hi, const std::string& where,
                 std::string* err) {
    if (n.kids.size() >= lo && n.kids.size() <= hi) return true;
    const std::string want = (lo == hi) ? std::to_string(lo) : std::to_string(lo) + ".." + std::to_string(hi);
    return fail(err, 4, where,
                std::string(hkind_name(n.kind)) + " 的操作数个数是 " + std::to_string(n.kids.size()) +
                    "，应为 " + want);
}

bool check_conv_node(const HNode& n, const TyRef& ret, const std::string& where, std::string* err) {
    switch (n.kind) {
        case HKind::Const:
        case HKind::StrLit:
        case HKind::Local:
        case HKind::Global:
        case HKind::Capture:
        case HKind::Break:
            if (!expect_kids(n, 0, 0, where, err)) return false;
            break;
        case HKind::Unary: {
            if (!expect_kids(n, 1, 1, where, err)) return false;
            if (n.un_op == HUnOp::Not) {
                if (!is_bool_ty(n.ty)) return fail(err, 4, where, "逻辑非的结果类型必须是 Bool");
            } else if (!same_ty(n.kids[0].ty, n.ty)) {
                return fail(err, 4, where,
                            "一元运算的操作数类型与结果类型不一致（隐式转换必须已经是 Cast）");
            }
            break;
        }
        case HKind::Binary: {
            if (!expect_kids(n, 2, 2, where, err)) return false;
            switch (n.bin_op) {
                case HBinOp::LogAnd:
                case HBinOp::LogOr:
                    if (!is_bool_ty(n.ty)) {
                        return fail(err, 4, where, "逻辑运算的结果类型必须是 Bool");
                    }
                    break;
                case HBinOp::Eq:
                case HBinOp::Ne:
                case HBinOp::Lt:
                case HBinOp::Le:
                case HBinOp::Gt:
                case HBinOp::Ge:
                    if (!is_bool_ty(n.ty)) {
                        return fail(err, 4, where, "比较的结果类型必须是 Bool");
                    }
                    if (!same_ty(n.kids[0].ty, n.kids[1].ty)) {
                        return fail(err, 4, where,
                                    "比较两侧的类型不一致（隐式转换必须已经是 Cast）");
                    }
                    break;
                default:
                    if (!same_ty(n.kids[0].ty, n.kids[1].ty) || !same_ty(n.kids[0].ty, n.ty)) {
                        return fail(err, 4, where,
                                    "算术两侧与结果的类型不一致（隐式转换必须已经是 Cast）");
                    }
                    break;
            }
            break;
        }
        case HKind::Cond:
            if (!expect_kids(n, 3, 3, where, err)) return false;
            if (!same_ty(n.kids[1].ty, n.kids[2].ty) || !same_ty(n.kids[1].ty, n.ty)) {
                return fail(err, 4, where,
                            "三目两臂的类型与结果类型不一致（隐式转换必须已经是 Cast）");
            }
            break;
        case HKind::Call:
            break; /* 实参个数/类型按被调者签名，不在这里查（见本函数开头的说明） */
        case HKind::Member:
            if (!expect_kids(n, 1, 1, where, err)) return false;
            break;
        case HKind::Index:
            if (!expect_kids(n, 2, 2, where, err)) return false;
            break;
        case HKind::Cast:
            if (!expect_kids(n, 1, 1, where, err)) return false;
            break;
        case HKind::ArrayLit:
        case HKind::CompositeLit:
            break;
        case HKind::Template:
            if (n.kids.size() % 2 != 0) {
                return fail(err, 4, where,
                            "Template 的操作数是（文字段, 表达式）交替，个数必须是偶数，现在是 " +
                                std::to_string(n.kids.size()));
            }
            for (std::size_t i = 0; i < n.kids.size(); i += 2) {
                if (n.kids[i].kind != HKind::StrLit) {
                    return fail(err, 4, kid_path(where, i), "Template 的偶数下标必须是 StrLit 文字段");
                }
            }
            break;
        case HKind::Block:
            break;
        case HKind::VarDecl:
            if (!expect_kids(n, 0, 1, where, err)) return false;
            if (n.kids.size() == 1 && !same_ty(n.kids[0].ty, n.ty)) {
                return fail(err, 4, where,
                            "初始化式的类型与变量类型不一致（隐式转换必须已经是 Cast）");
            }
            break;
        case HKind::Assign:
            if (!expect_kids(n, 2, 2, where, err)) return false;
            if (!same_ty(n.kids[0].ty, n.kids[1].ty)) {
                return fail(err, 4, where, "赋值的两侧类型不一致（隐式转换必须已经是 Cast）");
            }
            break;
        case HKind::ExprStmt:
            if (!expect_kids(n, 1, 1, where, err)) return false;
            break;
        case HKind::If:
            if (!expect_kids(n, 2, 3, where, err)) return false;
            break;
        case HKind::While:
            if (!expect_kids(n, 2, 2, where, err)) return false;
            break;
        case HKind::Switch: {
            if (n.kids.empty()) return fail(err, 4, where, "Switch 至少要有 case 的主体");
            std::size_t i = 1;
            while (i < n.kids.size()) {
                if (is_expr(n.kids[i].kind)) ++i; /* `case 值:` */
                if (i >= n.kids.size() || n.kids[i].kind != HKind::Block) {
                    return fail(err, 4, where,
                                "Switch 的每个 case 都是「[值表达式] + Block」（直接出现 Block = default）");
                }
                ++i;
            }
            break;
        }
        case HKind::Return:
            if (!expect_kids(n, 0, 1, where, err)) return false;
            if (n.kids.size() == 1 && !same_ty(n.kids[0].ty, ret)) {
                return fail(err, 4, where,
                            "返回值的类型与函数的返回类型不一致（隐式转换必须已经是 Cast）");
            }
            break;
    }
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        if (!check_conv_node(n.kids[i], ret, kid_path(where, i), err)) return false;
    }
    return true;
}

/* ⑤ span：`line >= 1` 就算带位置（列前端从来不填，见 span.hh）。 */
bool check_span_node(const HNode& n, const std::string& where, std::string* err) {
    if (n.span.line < 1) {
        return fail(err, 5, where, std::string(hkind_name(n.kind)) + " 没有源码位置（line=0）");
    }
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        if (!check_span_node(n.kids[i], kid_path(where, i), err)) return false;
    }
    return true;
}

}  // namespace

/* ---------------------------------------------------------------- dump */

std::string dump(const Hir& hir) {
    std::string out;
    out += "Hir funcs=" + std::to_string(hir.funcs.size()) + " globals=" +
           std::to_string(hir.globals.size()) + " externs=" + std::to_string(hir.externs.size()) +
           " consts=" + std::to_string(hir.consts.size()) +
           " strings=" + std::to_string(hir.strings.size()) + "\n";
    for (std::size_t i = 0; i < hir.strings.size(); ++i) {
        out += indent_of(1) + "string[" + std::to_string(i) + "] bytes=" +
               quoted(hir.strings[i].bytes) +
               " cstring=" + (hir.strings[i].cstring ? "true" : "false") + "\n";
    }
    for (std::size_t i = 0; i < hir.globals.size(); ++i) {
        out += indent_of(1) + "global[" + std::to_string(i) + "] name=" +
               quoted(hir.globals[i].name) + " ty=" + ty_text(hir.globals[i].ty) + "\n";
    }
    for (std::size_t i = 0; i < hir.externs.size(); ++i) {
        out += indent_of(1) + "extern[" + std::to_string(i) + "] name=" +
               quoted(hir.externs[i].name) + " ty=" + ty_text(hir.externs[i].ty) + "\n";
    }
    for (std::size_t i = 0; i < hir.consts.size(); ++i) {
        out += indent_of(1) + "const[" + std::to_string(i) + "] name=" +
               quoted(hir.consts[i].name) + " ty=" + ty_text(hir.consts[i].ty) +
               " value=" + hex64(hir.consts[i].bits) + "\n";
    }
    dump_func(out, "main", hir.main, 1);
    for (std::size_t i = 0; i < hir.funcs.size(); ++i) {
        dump_func(out, ("func[" + std::to_string(i) + "]").c_str(), hir.funcs[i], 1);
    }
    return out;
}

/* ---------------------------------------------------------------- 六条不变量 */

bool check_invariants(const Hir& hir, std::string* err) {
    std::string scratch;
    std::string* e = (err != nullptr) ? err : &scratch;
    e->clear();
    const std::vector<std::pair<std::string, const HirFunc*>> funcs = all_funcs(hir);

    /* ① 类型位 */
    for (const auto& fn : funcs) {
        if (!check_types_func(*fn.second, fn.first, e)) return false;
    }
    /* ② 引用已解析 */
    for (const auto& fn : funcs) {
        if (!check_refs_func(*fn.second, hir.globals.size(), fn.first, e)) return false;
    }
    /* ③ 被调实例已收敛 */
    for (const auto& fn : funcs) {
        if (!check_calls_node(fn.second->body, hir.funcs.size(), fn.first + ".body", e)) {
            return false;
        }
    }
    /* ④ 无隐式转换（含操作数形状） */
    for (const auto& fn : funcs) {
        if (!check_conv_node(fn.second->body, fn.second->ret, fn.first + ".body", e)) return false;
    }
    /* ⑤ 每节点带 span */
    for (const auto& fn : funcs) {
        if (!check_span_node(fn.second->body, fn.first + ".body", e)) return false;
    }
    /* ⑥ 实例键唯一 */
    std::set<std::string> keys;
    for (std::size_t i = 0; i < hir.funcs.size(); ++i) {
        const std::string& key = hir.funcs[i].key;
        if (key.empty()) return fail(e, 6, "funcs[" + std::to_string(i) + "]", "实例键为空");
        if (!keys.insert(key).second) {
            return fail(e, 6, "funcs[" + std::to_string(i) + "]",
                        "实例键 " + quoted(key) + " 与前面的重复");
        }
    }
    return true;
}

}  // namespace sema
}  // namespace simplexc
