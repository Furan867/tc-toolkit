# simplexc 的 CMake 构建（阶段 1：只做 `simplexc_cli` 与 `hook` 两个目标）。
#
# flags 的权威来源仍是 `simplexc/tools/build.py`（每条 g++ 命令都是从既有产物反推确认过的，
# 见 docs/notes/2026-10-17-build-script.md），这里逐条搬；阶段 1 不动那支脚本，阶段 2 再让它转调。
#
# 用法
# ----
#   cmake --preset mingw                             # = Ninja + cmake/toolchain-mingw-w64devkit.cmake + -B build/cmake
#   cmake --build build/cmake --target hook           # 等价于 build.py hook
#   cmake --build build/cmake --target simplexc_cli hook
#
# 二进制目录必须在外部指定（约定 `-B build/cmake`）：`build/` 里放着 LLVM 资产（build/llvm）与
# 产物基线，中间文件不能落在那儿 —— 本文件末尾会拒绝 `-B build`。
cmake_minimum_required(VERSION 3.20)

# ASM：`simplexc/shell/entry_shim.S` 走预处理汇编（.S 要过 cpp，所以是 ASM 而不是 ASM_NASM）。
project(simplexc LANGUAGES C CXX ASM)

set(SIMPLEXC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(SIMPLEXC_DIR "${SIMPLEXC_ROOT}/simplexc")
# 产物目录 = 外部契约：check.py / m5/batch.py / blob_baseline.py / gen_embedded_objs.py 都按
# `build/<名字>` 找东西（build.py 的产物也在这里），所以路径不能改。
set(SIMPLEXC_BUILD "${SIMPLEXC_ROOT}/build")
set(SIMPLEXC_OBJ "${SIMPLEXC_BUILD}/objs")

if(CMAKE_BINARY_DIR STREQUAL "${SIMPLEXC_BUILD}")
  message(FATAL_ERROR "二进制目录不能是 ${SIMPLEXC_BUILD}：那里放着 LLVM 资产与产物基线。\n"
    "  用 -B build/cmake（或 cmake --preset mingw）。")
endif()

# exe 与 DLL 都落在 build/：Windows 上 DLL 算 RUNTIME 产物（LIBRARY 那条为跨平台对称留着）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SIMPLEXC_BUILD}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SIMPLEXC_BUILD}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)   # gen_embedded_objs.py
list(APPEND CMAKE_MODULE_PATH "${SIMPLEXC_ROOT}/cmake")  # FindLLVMC.cmake
find_package(LLVMC REQUIRED)

# ---------------------------------------------------------------- 产品核心的源文件表
#
# 与 build.py 的 CORE_SOURCES **逐项对应、顺序相同**（`build/objs/assemble.o` 那一项不在表里：
# 它由下面的 `objs` 目标出，见 OBJ_RULES）。DLL 与将来的 `core_selftest` 必须共用这一处 ——
# 表不一样时，判据判的可能是别的东西（build.py 里那段注释强调的就是这件事）。
set(SIMPLEXC_CORE_SOURCES
  simplexc/frontend/decls.cc
  simplexc/frontend/lexer.cc
  simplexc/frontend/parser.cc
  simplexc/frontend/frontend.cc
  simplexc/frontend/ast_dump.cc
  simplexc/frontend/json.cc
  simplexc/codegen/module_build.cc
  simplexc/codegen/codegen.cc
  simplexc/codegen/asm_x64.cc
  simplexc/codegen/llvm_ffi.cc
  simplexc/sema/analyze.cc
  simplexc/sema/layout.cc
  simplexc/sema/types.cc
)

# ---------------------------------------------------------------- build/objs（四个 COFF）
#
# 为什么用 add_custom_command 而不是 add_library(OBJECT)：这四条产物路径是外部契约
# （`entry_shim.o` / `rt_min.o` 会被 gen_embedded_objs.py 原样嵌进 DLL，其余两个是 build.py
# 的同名产物），而 CMake 的 OBJECT 库只能把 .o 放在自己的中间目录里。
#
# 命令与 build.py 的 step_objs 逐字符相同（同样的 -std/-O2、同样的相对写法），所以这四个 .o
# 与 build.py 编出来的逐字节相同 —— 嵌进 DLL 的字节一致，产物才有可比性。
# `-o` 必须是绝对路径（CMake 的 OUTPUT 是绝对的），这不影响 .o 的字节：COFF 不记输出路径。
function(simplexc_add_object name source)
  add_custom_command(
    OUTPUT "${SIMPLEXC_OBJ}/${name}.o"
    COMMAND ${CMAKE_CXX_COMPILER} ${ARGN} -c "${SIMPLEXC_ROOT}/${source}"
            -o "${SIMPLEXC_OBJ}/${name}.o"
    DEPENDS "${SIMPLEXC_ROOT}/${source}"
    COMMENT "编译 ${source} → build/objs/${name}.o"
    VERBATIM)
  set(SIMPLEXC_OBJ_OUTPUTS "${SIMPLEXC_OBJ_OUTPUTS}" "${SIMPLEXC_OBJ}/${name}.o" PARENT_SCOPE)
endfunction()

set(SIMPLEXC_OBJ_OUTPUTS "")
# 不加 -fno-exceptions：既有产物是 GCC 默认（异常开着）编的，加/不加会得到不同的目标文件。
simplexc_add_object(assemble   simplexc/assemble/assemble.cc -std=c++17 -O2)
simplexc_add_object(entry_shim simplexc/shell/entry_shim.S)          # .S 不加 -std
simplexc_add_object(rt_min     simplexc/runtime/rt_min.cc   -std=c++17 -O2)
simplexc_add_object(prog_empty simplexc/cli/prog_empty.cc   -std=c++17 -O2)

add_custom_target(objs DEPENDS ${SIMPLEXC_OBJ_OUTPUTS})

set_source_files_properties("${SIMPLEXC_OBJ}/assemble.o"
  PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

# ---------------------------------------------------------------- simplexc_cli
#
# build.py step_simplexc_cli：`g++ -std=c++17 -O2 -I simplexc -I <llvm>/include
#   simplexc/cli/simplexc_cli.cc *CORE_SOURCES -L <llvm>/lib -lLLVM-C -o build/simplexc_cli.exe`
# 没有 -static（与 hook 不同），-lLLVM-C 压在源文件之后 —— CMake 自己就按这个顺序排。
add_executable(simplexc_cli
  simplexc/cli/simplexc_cli.cc
  ${SIMPLEXC_CORE_SOURCES}
  "${SIMPLEXC_OBJ}/assemble.o")
set_target_properties(simplexc_cli PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)                       # → -std=c++17，与 build.py 这条命令一致
target_include_directories(simplexc_cli PRIVATE "${SIMPLEXC_DIR}")
target_compile_options(simplexc_cli PRIVATE -O2)
target_link_libraries(simplexc_cli PRIVATE LLVMC::LLVMC)

# Windows 先从 exe 所在目录找依赖 DLL（再看 PATH）→ 把 LLVM-C.dll 放到 build/ 旁边，
# 判据（check.py 等）就不用为这一步改 PATH。build.py 的同名步骤做的是同一件事。
if(LLVMC_RUNTIME_DLL)
  add_custom_command(TARGET simplexc_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LLVMC_RUNTIME_DLL}"
            "${SIMPLEXC_BUILD}/LLVM-C.dll"
    COMMENT "拷贝 LLVM-C.dll → build/"
    VERBATIM)
endif()

# ---------------------------------------------------------------- hook（= build/compile_hook.dll）
#
# 转发层 = **C 壳**（tools/compile_hook.c）+ **C++ 编译器核心**（frontend/sema/codegen/assemble/shell）。
# 壳必须是 C（交给 g++ 会把 .c 当 C++ 编），核心与链接要用 g++（要 C++ 运行库）—— CMake 按扩展名分。
# 目标名与 build.py 一致：`cmake --build build/cmake --target hook` ≡ `build.py hook`。
set(SIMPLEXC_EMBEDDED_OBJS "${SIMPLEXC_DIR}/shell/embedded_objs.cc")

# 构建期把两个装配用的 COFF（entry_shim.o / rt_min.o）变成字节数组：游戏进程里没有
# build/objs/（为什么必须嵌进 DLL 见 gen_embedded_objs.py 的 docstring）。
add_custom_command(
  OUTPUT "${SIMPLEXC_EMBEDDED_OBJS}"
  COMMAND ${Python3_EXECUTABLE} "${SIMPLEXC_DIR}/tools/gen_embedded_objs.py"
  DEPENDS "${SIMPLEXC_OBJ}/entry_shim.o" "${SIMPLEXC_OBJ}/rt_min.o"
          "${SIMPLEXC_DIR}/tools/gen_embedded_objs.py"
  COMMENT "生成 simplexc/shell/embedded_objs.cc"
  VERBATIM)

add_library(hook SHARED
  tools/compile_hook.c
  simplexc/shell/simplexc_core.cc
  "${SIMPLEXC_EMBEDDED_OBJS}"
  ${SIMPLEXC_CORE_SOURCES}
  "${SIMPLEXC_OBJ}/assemble.o")
# 产物名是 compile_hook.dll（不是 libhook.dll）：部署入口与游戏按这个名字找它。
set_target_properties(hook PROPERTIES
  PREFIX ""
  OUTPUT_NAME "compile_hook"
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
target_include_directories(hook PRIVATE "${SIMPLEXC_DIR}")
target_compile_options(hook PRIVATE -O2)
# -static：把 libstdc++/libgcc/libwinpthread 一起包进来，否则游戏目录还要多拷三个 DLL
# （build.py 同一条命令上的 -O2 也照搬：无 LTO 时它不起作用，留着只是免得两条路径的 flags 分叉）。
target_link_options(hook PRIVATE -O2 -static)
target_link_libraries(hook PRIVATE LLVMC::LLVMC)
