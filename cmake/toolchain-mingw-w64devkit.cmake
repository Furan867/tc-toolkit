# 工具链 = w64devkit 的 MinGW-w64 GCC。固定，不走 MSVC。
#
# 为什么固定：`simplexc/tools/build.py` 的每条命令都是从既有产物反推确认过的
# （docs/notes/2026-10-17-build-script.md），换编译器就是换产物、换基线 —— 而 LLVM-C 的
# MinGW 导入库 `libLLVM-C.a` 也是照这个工具链造的（见 docs/facts/build-assemble.md）。
#
# 变量名与默认值与 build.py 相同（W64DEVKIT_BIN），所以一台机器上两条构建路径认的是同一套工具：
#   cmake --preset mingw                              # 用默认值
#   W64DEVKIT_BIN=D:/other/bin cmake --preset mingw    # 环境变量覆盖
#   cmake ... -DW64DEVKIT_BIN=D:/other/bin            # 命令行覆盖
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{W64DEVKIT_BIN})
  set(W64DEVKIT_BIN "$ENV{W64DEVKIT_BIN}" CACHE PATH "w64devkit 的 bin 目录（与 build.py 同一个变量）")
else()
  set(W64DEVKIT_BIN "D:/bin/w64devkit/bin" CACHE PATH "w64devkit 的 bin 目录（与 build.py 同一个变量）")
endif()

foreach(_tool gcc.exe g++.exe)
  if(NOT EXISTS "${W64DEVKIT_BIN}/${_tool}")
    message(FATAL_ERROR "找不到 ${W64DEVKIT_BIN}/${_tool}\n"
      "  本工具链固定用 w64devkit（MinGW-w64 GCC），不用 MSVC。\n"
      "  用环境变量 W64DEVKIT_BIN 指向工具链目录，或 -DW64DEVKIT_BIN=... 覆盖（默认 D:/bin/w64devkit/bin）。")
  endif()
endforeach()

set(CMAKE_C_COMPILER "${W64DEVKIT_BIN}/gcc.exe")
set(CMAKE_CXX_COMPILER "${W64DEVKIT_BIN}/g++.exe")
