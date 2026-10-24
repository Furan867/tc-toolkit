/* sema/types.hh —— 类型表示与常量折叠（步①b 从 旧 `lower.hh` 原样搬来）。
 *
 * 为什么在 sema：`docs/notes/2026-10-24-refactor-plan.md` §2 决策 4 —— 常量折叠全部归 sema，
 * codegen 只读已折好的 `Const`。搬的时候一行逻辑没改，旧的 `lower.hh` 用 using 把名字引入
 * `simplexc::lower`（那个 using 组随旧 lowering 一起删了，调用点已改指 `sema::`）。
 *
 * 留在这里不搬的：`V`（带值句柄，属 codegen 侧）。`ty_key` / `size_bits` 已随 S2b-3 搬进来
 * （实例键与 `size()` 都要用），旧 `lower.hh` 里那两份拷贝 ②c 接线时已删。 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "span.hh"

namespace simplexc {
namespace sema {

/* ---------------------------------------------------------------- 类型
 * 位宽是**真语义**（U-L7），`bits` 可以是 1..64 的任意值。 */
enum class TyKind { Void, Int, Ptr, Enum, Struct, Array, Str };

struct Ty;
using TyRef = std::shared_ptr<const Ty>;

struct Ty {
    TyKind kind = TyKind::Void;
    int bits = 64;
    bool is_signed = false;
    /* `Bool` 与 `U1` 在算术语义上等价（接口 §1），但**插值渲染不同**：`Bool` → `true`/`false`、
     * `U1` → `1`/`0`（实测见 docs/notes/2026-10-18-str-interp.md）—— 所以这个名义标记必须留着。 */
    bool is_bool = false;
    std::string name;                                   /* Enum / Struct 的名字 */
    std::vector<std::pair<std::string, TyRef>> fields;  /* Struct 字段（按声明序） */
    TyRef elem;                                         /* Array / Str 的元素类型 */
};

/* 模块级单例（`I64` / `BOOL` / `PTR` / `STR` / `SEED` / `TIME`）。 */
const TyRef& ty_void();
const TyRef& ty_i64();
const TyRef& ty_bool();
const TyRef& ty_ptr();
const TyRef& ty_str();
const TyRef& ty_seed();
const TyRef& ty_time();

TyRef make_int(int bits, bool is_signed);
TyRef make_enum(const std::string& name);
TyRef make_struct(const std::string& name, int bits, std::vector<std::pair<std::string, TyRef>> fields);
TyRef make_array(TyRef elem);

inline constexpr uint64_t kMask64 = ~uint64_t{0};
uint64_t mask_of(int bits);
int storage_bytes(const Ty& ty, Span span);   /* 内存字节宽度；Void / 未知识 → 抛 `CompileError` */
/* 实例键 / 捕获签名的类型拼写（决策 7 的唯一一处；②c 之前旧 `lower.hh` 里还有一份拷贝，已删）。 */
std::string ty_key(const Ty& ty);
/* `size(x)` 的返回值，单位 = 位（`docs/facts/semantics.md` 的泛型一条）。 */
int size_bits(const Ty& ty, Span span);
const char* ty_kind_name(TyKind k);
inline bool is_int(const Ty& ty) { return ty.kind == TyKind::Int; }

/* ---------------------------------------------------------------- 常量折叠
 * 结果是**该类型的位模式**（`0 <= v < 2^bits`）。三条参考行为（见 `docs/facts/semantics.md`）：截断发生在运算
 * 结果上（U-L7）、`>>` 是 lshr 语义、除法的符号性看被除数且除数为 0 时商 0 余数为被除数。 */
int64_t as_signed(uint64_t x, int bits);
/* `span` 是抛点唯一的位置来源（这三条都是「运算符填错了」的内部守卫，调用点自己都有行号）。 */
uint64_t fold_arith(const std::string& op, uint64_t a, uint64_t b, const Ty& ty, Span span);
uint64_t fold_unary(const std::string& op, uint64_t a, const Ty& ty, Span span);
uint64_t fold_cmp(const std::string& op, uint64_t a, uint64_t b, const Ty& ty, Span span);   /* 0 / 1 */

}  // namespace sema
}  // namespace simplexc