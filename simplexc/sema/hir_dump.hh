/* sema/hir_dump.hh —— HIR 的一行一节点文本 dump（接缝判据用，不进产品路径）。
 *
 * 为什么需要它：②c 改 codegen 之前，唯一能证明「sema 侧语义没动」的硬判据就是 dump 的
 * 逐字 diff（方案 §3 步骤表的 ②b/②c）。所以输出必须是**稳定的**：字段顺序固定、嵌套缩进、
 * 不打印任何指针/地址/容器容量 —— 同一棵树每次 dump 出同样的字节。
 */
#pragma once

#include <string>

#include "hir.hh"

namespace simplexc {
namespace sema {

/* 整棵 `Hir` 的文本形态，每个节点一行，末尾带换行。字段形状见 `hir_dump.cc` 顶部。 */
std::string dump(const Hir& hir);

}  // namespace sema
}  // namespace simplexc
