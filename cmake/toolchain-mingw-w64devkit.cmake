# 工具链 = MinGW-w64 GCC（w64devkit），从 **PATH** 找。固定，不走 MSVC。
#
# 为什么固定 GCC：`simplexc/tools/build.py` 的每条命令都是从既有产物反推确认过的
# （docs/notes/2026-10-17-build-script.md），换编译器就是换产物、换基线 —— 而 LLVM-C 的
# MinGW 导入库 `libLLVM-C.a` 也是照这个工具链造的（见 docs/facts/build-assemble.md）。
#
# 为什么不写死路径：工具链装在哪是**机器的事**，不是仓库的事。要换工具链就把另一份
# gcc/g++ 放到 PATH 前面，本文件不用改（也不再提供「工具链目录」这类变量 ——
# 同一件事只允许一种说法）。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(SIMPLEXC_C_COMPILER NAMES gcc REQUIRED)
find_program(SIMPLEXC_CXX_COMPILER NAMES g++ REQUIRED)

set(CMAKE_C_COMPILER "${SIMPLEXC_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${SIMPLEXC_CXX_COMPILER}")
