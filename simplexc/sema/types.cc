/* sema/types.cc —— 类型系统与常量折叠的实现（步①b 从 `lower/lower.cc` 原样搬来，逐行未改）。
 *
 * `neg64` 也一起搬：它在 lower.cc 里只被 fold_arith / fold_unary 用，跟着搬才不用复制一份。 */
#include "types.hh"

#include "errors.hh"

namespace simplexc {
namespace sema {
namespace {

/* `z = -x` 的 64 位取负：用无符号补码写法，避免 `INT64_MIN` 的 `-x`/`abs` 未定义行为
 * （Python 里是任意精度整数，不会溢出）。 */
uint64_t neg64(uint64_t x) { return ~x + 1; }

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
/* 实例键 / 捕获签名用的类型拼写（方案 §2 决策 7 的**唯一**一处；`lower/lower.cc` 里还有一份
 * 逐行相同的拷贝，②c 接线时删那一份）。只要「同一个变量在不同宽度/种类下能分开」就够：
 * 同名变量在两个调用点绑不同类型时，实例体里的读写宽度不同，不能复用同一个实例。 */
std::string ty_key(const Ty& ty) {
    switch (ty.kind) {
        case TyKind::Int:
            if (ty.is_bool) return "bool";
            return std::string(ty.is_signed ? "s" : "u") + std::to_string(ty.bits);
        case TyKind::Ptr: return "ptr";
        case TyKind::Str: return "str";
        case TyKind::Enum: return "enum:" + ty.name;
        case TyKind::Struct: return "struct:" + ty.name;
        case TyKind::Array:
            return "arr:" + (ty.elem != nullptr ? ty_key(*ty.elem) : std::string("?"));
        case TyKind::Void: return "void";
    }
    return "?";
}

/* `size(x)` 的单位是**位**（`docs/facts/semantics.md` 的泛型一条），宽度来源与 `storage_bytes`
 * 同一处。Enum 是唯一例外（U-L5：只有 1/2 字节两档，而它的存储宽度是 1 个机器字），所以它在
 * 这里报掉，不静默给一个错的 64。 */
int size_bits(const Ty& ty) {
    if (ty.kind == TyKind::Enum) throw Unsupported("size() 用在枚举上", 0);
    return storage_bytes(ty) * 8;
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

}  // namespace sema
}  // namespace simplexc