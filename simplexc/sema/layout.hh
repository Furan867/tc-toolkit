/* sema/layout.hh —— struct 布局与 enum 底层宽度的**唯一实现**（方案 §2b 的 `Hir.structs[]`）。
 *
 * 为什么单独一个文件：这两条规则是「同一份结论只能有一处实现」的典型。原先 `lower.cc` 在
 * `Program::scan_decls` / `Program::make_types` 里算一遍布局、在 `lvalue(Member)` 里再用一遍，
 * 改成 sema 定死、codegen 只查表之后，实现只有这里一份。依据是 `docs/facts/semantics.md`
 * 的实测结论（U-L4 / U-L5）—— 那两条**没写到的情形**一律报错，不猜。
 *
 * 已验的事实（引原文，改之前先回去看）：
 *   · U-L4：字段偏移 / padding / 嵌套布局**未验**（三次尝试都不可观测）。现有实现是
 *     「`ceil(bits/8)`、声明序、无 padding」，这轮既没改也没验 —— 所以这里照现状实现，并显式
 *     拒绝唯一已知走不通的那种（字段本身是 struct：`storage_bytes` 没有 Struct 分支）。
 *   · U-L4（部分，已验）：复合字面量的未列字段默认 0、按**名字**匹配（与书写顺序无关）、
 *     求值顺序 = **源码书写顺序**。后一条是 `analyze.cc` 的 `composite_lit_node` 负责的
 *     （先按源码序求值，再按声明序摆 kid），不是这里的布局。
 *   · U-L5：enum 底层宽度**只有两档** —— ≤256 个成员 1 字节、257…65535 2 字节；4/8 字节档
 *     不可达（65536 个成员时参考自己断言崩溃）。所以 `enum_storage_bits` 对 >65535 报错。
 */
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "hir.hh"
#include "types.hh"

namespace simplexc {
namespace sema {

/* enum 的底层宽度（**位**）：成员数 → 8 / 16（U-L5）。>65535 抛 `CompileError`。 */
int enum_storage_bits(std::size_t member_count);

/* enum 类型对象：`bits` 装的是上面那档底层宽度（不是我们值表示的机器字宽 —— 那是
 * `storage_bytes(Enum)` = 8，两者故意分开：值是一整个 64 位字，底层宽度只用于 `size()` 与
 * 数组步长）。 */
TyRef layout_enum(const std::string& name, std::size_t member_count);

/* 一个 struct 的布局。`ty` 是类型对象（位宽与字段表），`fields` 是声明序 + 字节偏移，
 * 直接就是 `Hir.structs[i]` 的字段表。空 struct 取 **1 字节占位**（`bits = 8`）：零宽度的
 * alloca / 全局在参考那条路上没验证过（U-L4）。 */
struct StructLayout {
    TyRef ty;
    std::vector<HirField> fields;
};

/* 字段类型里出现 struct（嵌套布局未验，U-L4）或没有字节宽度的类型（`None`）→ 抛
 * `CompileError`。`fields` 必须是**已解析**的字段类型，顺序 = 声明序。 */
StructLayout layout_struct(const std::string& name,
                           const std::vector<std::pair<std::string, TyRef>>& fields);

}  // namespace sema
}  // namespace simplexc
