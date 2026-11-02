/* embedded_objs.hh —— 构建期嵌进 DLL 的两个目标文件（声明；定义在生成的 `embedded_objs.cc` 里）。
 *
 * 为什么需要它们：装配要有**三个**目标文件（模块 COFF + `entry_shim.o` + `rt_min.o`，接口 §4），
 * 而壳在游戏进程里编译 —— 那里没有 `simplexc/build/objs/`，也没有安全的临时目录。所以后两个在构建期
 * 由 `simplexc/tools/gen_embedded_objs.py` 变成字节数组链进 DLL（生成物不入库）。
 *
 * 声明与生成物分开写：两者对不上时是**链接错误**，不会静默变成别的语义。
 */
#pragma once

#include <cstddef>

namespace simplexc {
namespace shell {

extern const unsigned char kEntryShimObj[];
extern const std::size_t kEntryShimObjSize;
extern const unsigned char kRtMinObj[];
extern const std::size_t kRtMinObjSize;

}  // namespace shell
}  // namespace simplexc
