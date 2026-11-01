# LLVMC::LLVMC —— `build/llvm` 下 LLVM-C 的头文件与自造 MinGW 导入库。
#
# 为什么不用 find_package(LLVM)：这里用的不是 LLVM 的 CMake 导出包，而是官方 Windows 发行包里的
# `LLVM-C.dll`（C ABI）加一份用 gendef + dlltool 造的导入库 `libLLVM-C.a` —— 官方包里那份
# `LLVM-C.lib` 是 MSVC 的，MinGW 的 ld 吃不下（来历见 docs/facts/build-assemble.md）。
# 所以这里只做两件事：找到头目录与那份 .a，包成一个 IMPORTED target。
#
# 输入：LLVMC_ROOT（默认 <repo>/build/llvm，可用 -DLLVMC_ROOT=... 覆盖）。
# 产出：target LLVMC::LLVMC；变量 LLVMC_RUNTIME_DLL（`bin/LLVM-C.dll` 的路径，没有就是空）。

if(NOT LLVMC_ROOT)
  set(LLVMC_ROOT "${CMAKE_SOURCE_DIR}/build/llvm")
endif()

find_path(LLVMC_INCLUDE_DIR
  NAMES llvm-c/Core.h
  PATHS "${LLVMC_ROOT}/include"
  NO_DEFAULT_PATH)
find_library(LLVMC_LIBRARY
  NAMES LLVM-C
  PATHS "${LLVMC_ROOT}/lib"
  NO_DEFAULT_PATH)
find_file(LLVMC_RUNTIME_DLL
  NAMES LLVM-C.dll
  PATHS "${LLVMC_ROOT}/bin"
  NO_DEFAULT_PATH)

if(NOT LLVMC_INCLUDE_DIR)
  message(FATAL_ERROR "找不到 LLVM-C 头文件 ${LLVMC_ROOT}/include/llvm-c/Core.h\n"
    "  先取 LLVM：python simplexc/tools/fetch_llvm.py --dest D:/downloads（build/llvm 不进 git）")
endif()
if(NOT LLVMC_LIBRARY)
  message(FATAL_ERROR "找不到 MinGW 导入库 ${LLVMC_ROOT}/lib/libLLVM-C.a\n"
    "  缺 MinGW 导入库 libLLVM-C.a：见 docs/facts.md §构建与 LLVM（gendef + dlltool）")
endif()

add_library(LLVMC::LLVMC UNKNOWN IMPORTED)
set_target_properties(LLVMC::LLVMC PROPERTIES
  IMPORTED_LOCATION "${LLVMC_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${LLVMC_INCLUDE_DIR}")

mark_as_advanced(LLVMC_INCLUDE_DIR LLVMC_LIBRARY LLVMC_RUNTIME_DLL)
