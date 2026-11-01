# LLVMC::LLVMC —— `simplexc/.deps/llvm` 下 LLVM-C 的头文件与自造 MinGW 导入库。
#
# 为什么不用 find_package(LLVM)：这里用的不是 LLVM 的 CMake 导出包，而是官方 Windows 发行包里的
# `LLVM-C.dll`（C ABI）加一份用 gendef + dlltool 造的导入库 `libLLVM-C.a` —— 官方包里那份
# `LLVM-C.lib` 是 MSVC 的，MinGW 的 ld 吃不下（来历见 docs/facts/build-assemble.md）。
# 所以这里只做两件事：找到头目录与那份 .a，包成一个 IMPORTED target。
#
# 找哪里（优先级从高到低）
# -----------------------
#   1. `-DLLVMC_ROOT=…` / `-DLLVM_C_ROOT=…`   命令行显式给（换一份 LLVM 时用这个）
#   2. `$TC_LLVM_DIR`                         环境变量
#   3. `<repo>/simplexc/.deps/llvm`           `fetch_llvm.py` 的默认落点
#   4. `<repo>/build/llvm`                    旧落点，仍然认 —— 免得没迁移的工作区直接 configure 失败
# 3、4 的判据是「有 include/llvm-c/Core.h」而不只是目录存在：半途而废的解包目录不该被认成一份 LLVM。
#
# 默认值的锚是**本模块所在目录**而不是 CMAKE_SOURCE_DIR：后者是顶层工程的目录，
# 这个模块一旦被 add_subdirectory 引进来就会指到别人家去。
#
# 产出：target LLVMC::LLVMC；变量 LLVMC_RUNTIME_DLL（`bin/LLVM-C.dll` 的路径，没有就是空）。

if(NOT LLVMC_ROOT AND LLVM_C_ROOT)
  set(LLVMC_ROOT "${LLVM_C_ROOT}")
endif()
if(NOT LLVMC_ROOT AND DEFINED ENV{TC_LLVM_DIR} AND NOT "$ENV{TC_LLVM_DIR}" STREQUAL "")
  set(LLVMC_ROOT "$ENV{TC_LLVM_DIR}")
endif()
if(NOT LLVMC_ROOT)
  foreach(_llvmc_candidate "${CMAKE_CURRENT_LIST_DIR}/../simplexc/.deps/llvm"
                           "${CMAKE_CURRENT_LIST_DIR}/../build/llvm")
    if(EXISTS "${_llvmc_candidate}/include/llvm-c/Core.h")
      set(LLVMC_ROOT "${_llvmc_candidate}")
      break()
    endif()
  endforeach()
endif()
if(NOT LLVMC_ROOT)
  set(LLVMC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../simplexc/.deps/llvm")
endif()
message(STATUS "LLVM-C: 用 ${LLVMC_ROOT}")

# NO_DEFAULT_PATH 是有意的：这份 LLVM-C 的位置由 fetch_llvm.py 钉死，环境里另一份同名的
# 头 / .a（换 ABI 的、MSVC 的）被找进来只会得到看不懂的链接错误。
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
    "  先取 LLVM：python simplexc/tools/fetch_llvm.py（默认解到 simplexc/.deps/llvm，资产不进 git）；\n"
    "  已经在别处备好就用 -DLLVM_C_ROOT=<目录> 或环境变量 TC_LLVM_DIR 指过去。")
endif()
if(NOT LLVMC_LIBRARY)
  message(FATAL_ERROR "找不到 MinGW 导入库 ${LLVMC_ROOT}/lib/libLLVM-C.a\n"
    "  缺 MinGW 导入库 libLLVM-C.a：见 docs/facts.md §构建与 LLVM（gendef + dlltool）")
endif()

add_library(LLVMC::LLVMC UNKNOWN IMPORTED)
# 头目录走 SYSTEM（-isystem）：它们是别人的头，其告警不该撞本项目的 -Werror。
# 两个属性都要写：`INTERFACE_INCLUDE_DIRECTORIES` 把路径交给消费者，
# `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` 才把它标成 SYSTEM（只写后者，目录压根不会进命令行）。
set_target_properties(LLVMC::LLVMC PROPERTIES
  IMPORTED_LOCATION "${LLVMC_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${LLVMC_INCLUDE_DIR}"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${LLVMC_INCLUDE_DIR}")

mark_as_advanced(LLVMC_INCLUDE_DIR LLVMC_LIBRARY LLVMC_RUNTIME_DLL)
