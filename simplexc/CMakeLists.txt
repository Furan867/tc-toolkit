# simplexc 的构建**唯一定义**：目标、源文件表、flags 全在这里。
# `simplexc/tools/build.py` 只是本文件的一层薄壳（无 cache 时 configure，再 `cmake --build --target ...`），
# 它不再持有任何编译命令行 —— 改 flags 只改这里，别处没有第二份。
#
# flags 的来历：优化等级与链接开关都是从既有产物**反推确认**过的（见
# docs/notes/2026-10-17-build-script.md），但它们的布局是收成一处而不是逐目标重抄（见下面的
# `simplexc_flags`），语言标准也已从 C++17 升到 C++23。
# 产物因此**不**与那一版逐字节相同：升标准本身就会改 build/objs/*.o（它们会被原样嵌进 DLL），
# exe/dll 更早就不同 —— CMake 每次都会额外传 `-Wl,--out-implib` 与一票默认 Windows 库，ld 因此
# 把 .rdata 挪了几十字节；llvm_prog / codegen_selftest 的 LLVM-C 导入库也是按绝对路径给的，
# 不是旧命令的 `-L` + `-l`。判据不靠字节比（check.py 6 步 / blob_baseline 98 份 / hook_offline 11 case）。
#
# 用法
# ----
#   cmake --preset mingw                             # 在 simplexc/ 里跑 · = Ninja + ../cmake/toolchain-mingw-w64devkit.cmake + -B ../build/cmake
#   cmake --build build/cmake                        # = 旧 build.py 的 `all`（8 个目标）
#   cmake --build build/cmake --target hook           # = 旧 build.py hook（`simplexc_dll` 是同一个目标的别名）
#   cmake --build build/cmake --target spike          # 空程序 blob（依赖 objs + tcblob，自动补做）
#
# 二进制目录必须在外部指定（约定 `-B build/cmake`）：`build/` 里放着产物与产物基线（LLVM 资产在
# `simplexc/.deps/llvm`，只有 `LLVM-C.dll` 被拷进 `build/`），中间文件不能落在那儿 —— 本文件末尾会拒绝 `-B build`。
#
# 哪些目标在默认构建里（= 旧 build.py 的 `all`）
# ------------------------------------------------
# 在：`objs` / `tcblob` / `llvm_prog` / `hook` / `ast_dump` / `hir_selftest` /
#     `analyze_selftest` / `codegen_selftest`。
# 不在（`EXCLUDE_FROM_ALL`，要显式 `--target`）：`simplexc_cli`、`core_selftest`、`spike`、
#     `difftrace`、`difftrace_selftest`、`asm_x64_selftest`。理由见各自小节。

# 3.21 是 CMakePresets.json 的 version 3 的下限 —— build.py 一律走 `--preset`，
# 声明 3.20 只会让 3.20 的机器在 `--preset` 上失败一次才明白。
cmake_minimum_required(VERSION 3.21)

# ASM：`simplexc/shell/entry_shim.S` 走预处理汇编（.S 要过 cpp，所以是 ASM 而不是 ASM_NASM）。
project(simplexc LANGUAGES C CXX ASM)

# clangd / 任何 LSP 用同一个构建定义解析源码：CMake 把每个编译单元的**真实**命令行导成
# `compile_commands.json`（含 -D / -I / -isystem / -std / --target），它比手写 `compile_flags.txt`
# 更准也不会漂。`simplexc/` 下那份是构建时拷过去的副本（clangd 从源文件向上找，源文件都在
# `simplexc/**` 里），所以它也进 .gitignore。
# 首次 configure 后到第一次 build 之前它还不存在 —— 那时 clangd 按默认方式解析，无害。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 语言标准写在全局而不是逐个目标上：这里没有任何一个目标要别样标准，逐个写只是多几处会漂的
# 重复。`_STANDARD_REQUIRED ON` 让「工具链不支持这个标准」在 configure 期就失败，而不是退到
# 一个更老的标准继续编；`_EXTENSIONS OFF` 关掉 gnu++ 那套扩展。
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------- 公共编译选项（只有这一处）
#
# 优化等级与告警开关在这里定义一次：各业务目标 `target_link_libraries(<t> PRIVATE simplexc_flags)`
# 就能继承到 —— 换优化等级、加减一条告警只改下面这一行（INTERFACE 库没有编译单元，只运选项）。
# 手工调编译器的那四个 .o 也取同一个变量（见 simplexc_add_object），它们不是 CMake 目标，
# 拿不到 INTERFACE 库的继承。
#
# `-Werror` 与 `-Wall` 一起挂：告警修干净之后它才是「下次不许新增告警」的判据，新告警当场编不过。
set(SIMPLEXC_COMMON_COMPILE_OPTIONS -O2 -Wall -Werror)
add_library(simplexc_flags INTERFACE)
target_compile_options(simplexc_flags INTERFACE ${SIMPLEXC_COMMON_COMPILE_OPTIONS})

# 三条「工具判据」目标（difftrace / difftrace_selftest / asm_x64_selftest）比产线多两条告警 ——
# 它们是新写的工具代码，吃得住。单独一个 INTERFACE 档而不是铺到全部目标：铺开会在 Win32 的
# `GetProcAddress` 函数指针强转上炸出一串（tools/compile_hook.c 那个 C 壳），那是另一个决定。
set(SIMPLEXC_STRICT_COMPILE_OPTIONS -Wextra -Wpedantic)
add_library(simplexc_strict_flags INTERFACE)
target_compile_options(simplexc_strict_flags INTERFACE ${SIMPLEXC_STRICT_COMPILE_OPTIONS})
target_link_libraries(simplexc_strict_flags INTERFACE simplexc_flags)

# 本文件在 `simplexc/` 下，仓库根是上一级。凡是「相对仓库根」的路径（build/、work/、源文件表）
# 都从 SIMPLEXC_ROOT 派生，所以它们不必随本文件的位置改。
#
# 源文件也一律写成 `${SIMPLEXC_ROOT}/<仓库根相对路径>`：相对路径是按本文件所在目录算的，
# 裸写 `simplexc/…` 会指到 `simplexc/simplexc/…`（不存在的路径）。
# ABSOLUTE 是为了把末尾的 `..` 吃掉：这个值会原样编进 DLL 当 `TC_TOOLKIT_ROOT`（`simplexc/..\work\hook`
# 能跑，但它同时出现在 hook.log 与 opt.ini 的对照里，不该带一段 `..`）。
get_filename_component(SIMPLEXC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SIMPLEXC_DIR "${SIMPLEXC_ROOT}/simplexc")
# 产物目录 = 外部契约：check.py / m5/batch.py / blob_baseline.py / gen_embedded_objs.py 都按
# `build/<名字>` 找东西，所以路径不能改。
set(SIMPLEXC_BUILD "${SIMPLEXC_ROOT}/build")
set(SIMPLEXC_OBJ "${SIMPLEXC_BUILD}/objs")

if(CMAKE_BINARY_DIR STREQUAL "${SIMPLEXC_BUILD}")
  message(FATAL_ERROR "二进制目录不能是 ${SIMPLEXC_BUILD}：那里放着 LLVM 资产与产物基线。\n"
    "  用 -B build/cmake（或 `cmake --preset mingw`，在 simplexc/ 里跑）。")
endif()

# exe 与 DLL 都落在 build/：Windows 上 DLL 算 RUNTIME 产物（LIBRARY 那条为跨平台对称留着）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SIMPLEXC_BUILD}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SIMPLEXC_BUILD}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)   # gen_embedded_objs.py
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")  # FindLLVMC / FindUnicorn / FindCapstone
find_package(LLVMC REQUIRED)

# ---------------------------------------------------------------- 产品核心的源文件表
#
# DLL 与 `core_selftest` / `simplexc_cli` 共用这一处（`build/objs/assemble.o` 不在表里：
# 它由下面的 `objs` 目标出，见 OBJ_RULES）。共用不是风格问题：表不一样时，判据判的可能是
# 别的东西 —— 判据要判的就是交付物。
set(SIMPLEXC_CORE_SOURCES
  "${SIMPLEXC_ROOT}/simplexc/frontend/decls.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/lexer.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/parser.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/frontend.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/ast_dump.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/json.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/module_build.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/codegen.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/asm_x64.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/llvm_ffi.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/analyze.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/layout.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/types.cc"
)

# ---------------------------------------------------------------- build/objs（四个 COFF）
#
# 为什么用 add_custom_command 而不是 add_library(OBJECT)：这四条产物路径是外部契约
# （`entry_shim.o` / `rt_min.o` 会被 gen_embedded_objs.py 原样嵌进 DLL，其余两个是 CLI / spike
# 的输入），而 CMake 的 OBJECT 库只能把 .o 放在自己的中间目录里。
#
# `.o` 会被 gen_embedded_objs.py 原样嵌进 DLL，所以它由源码 + 语言标准 + 优化等级唯一决定：
# 换标准/换优化等级就换字节（语义由 blob_baseline 的 98 份判）。`-o` 必须是绝对路径
# （CMake 的 OUTPUT 是绝对的），这不影响 .o 的字节：COFF 不记输出路径。
#
# 选项不在这里手写：语言标准从 CMAKE_CXX_STANDARD 推、告警与优化等级取
# SIMPLEXC_COMMON_COMPILE_OPTIONS，与 CMake 目标共享同一处定义。.S 只要预处理 + 汇编，
# 不拿 C++ 那一套。
set(SIMPLEXC_OBJ_COMMON_FLAGS
  "-std=c++${CMAKE_CXX_STANDARD}" ${SIMPLEXC_COMMON_COMPILE_OPTIONS})
function(simplexc_add_object name source)
  if(source MATCHES "\\.S$")
    set(_flags)
  else()
    set(_flags ${SIMPLEXC_OBJ_COMMON_FLAGS})
  endif()
  add_custom_command(
    OUTPUT "${SIMPLEXC_OBJ}/${name}.o"
    COMMAND ${CMAKE_CXX_COMPILER} ${_flags} -c "${SIMPLEXC_ROOT}/${source}"
            -o "${SIMPLEXC_OBJ}/${name}.o"
    DEPENDS "${SIMPLEXC_ROOT}/${source}"
    COMMENT "编译 ${source} → build/objs/${name}.o"
    VERBATIM)
  set(SIMPLEXC_OBJ_OUTPUTS "${SIMPLEXC_OBJ_OUTPUTS}" "${SIMPLEXC_OBJ}/${name}.o" PARENT_SCOPE)
endfunction()

set(SIMPLEXC_OBJ_OUTPUTS "")
# 不加 -fno-exceptions：既有产物是 GCC 默认（异常开着）编的，加/不加会得到不同的目标文件。
simplexc_add_object(assemble   simplexc/assemble/assemble.cc)
simplexc_add_object(entry_shim simplexc/shell/entry_shim.S)
simplexc_add_object(rt_min     simplexc/runtime/rt_min.cc)
simplexc_add_object(prog_empty simplexc/cli/prog_empty.cc)

# ALL：默认构建要的正是这四个 .o（`prog_empty.o` 只有 spike 用，旧 build.py 的 `all` 里也在）。
add_custom_target(objs ALL DEPENDS ${SIMPLEXC_OBJ_OUTPUTS})

set_source_files_properties("${SIMPLEXC_OBJ}/assemble.o"
  PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

# ---------------------------------------------------------------- simplexc_cli
#
# 编译选项走 `simplexc_flags`（见文件开头）；链 LLVM-C、无 -static。
# LLVM-C 的导入库压在源文件之后（CMake 自己就按这个顺序排）。
#
# EXCLUDE_FROM_ALL：旧 build.py 的 `all` 里没有它（见 docs/status.md「环境」一节），
# 默认构建不该为它变慢。
add_executable(simplexc_cli
  "${SIMPLEXC_ROOT}/simplexc/cli/simplexc_cli.cc"
  ${SIMPLEXC_CORE_SOURCES}
  "${SIMPLEXC_OBJ}/assemble.o")
set_target_properties(simplexc_cli PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_include_directories(simplexc_cli PRIVATE "${SIMPLEXC_DIR}")
target_link_libraries(simplexc_cli PRIVATE simplexc_flags LLVMC::LLVMC)

# Windows 先从 exe 所在目录找依赖 DLL（再看 PATH）→ 把 LLVM-C.dll 放到 build/ 旁边，
# 判据（check.py 等）就不用为这一步改 PATH。
if(LLVMC_RUNTIME_DLL)
  add_custom_command(TARGET simplexc_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LLVMC_RUNTIME_DLL}"
            "${SIMPLEXC_BUILD}/LLVM-C.dll"
    COMMENT "拷贝 LLVM-C.dll → build/"
    VERBATIM)
endif()

# ---------------------------------------------------------------- tcblob（装配器 CLI）
# 源文件顺序 = 旧命令的链接顺序（tcblob.cc 在前、assemble.o 在后）：.o 的拼接顺序会改变
# 段布局，换了顺序产物就不再与基线逐字节相同。
add_executable(tcblob
  "${SIMPLEXC_ROOT}/simplexc/cli/tcblob.cc"
  "${SIMPLEXC_OBJ}/assemble.o")
# -static：w64devkit 默认就是静态链 libstdc++/libgcc（实测加不加产物逐字节相同），
# 显式写上是为了换工具链时不悄悄多出 DLL 依赖。
target_link_options(tcblob PRIVATE -static)
target_link_libraries(tcblob PRIVATE simplexc_flags)

# ---------------------------------------------------------------- llvm_prog（LLVM-C 生成程序根）
#
# flags 同 hook（公共选项 + -static），链 LLVM-C。运行期要 PATH 里有 `simplexc/.deps/llvm/bin`
# （或 `TC_LLVM_DIR` 指定的目录）才找得到 LLVM-C.dll
# （本目标不拷那份 DLL：拷了也只对 simplexc_cli 那条判据有用）。
add_executable(llvm_prog "${SIMPLEXC_ROOT}/simplexc/cli/llvm_prog.cc")
target_link_options(llvm_prog PRIVATE -static)
target_link_libraries(llvm_prog PRIVATE simplexc_flags LLVMC::LLVMC)

# ---------------------------------------------------------------- hook（= build/compile_hook.dll）
#
# 转发层 = **C 壳**（tools/compile_hook.c）+ **C++ 编译器核心**（frontend/sema/codegen/assemble/shell）。
# 壳必须是 C（交给 C++ 编译器会把 .c 当 C++ 编），核心与链接要用 C++ 编译器（要 C++ 运行库）——
# CMake 按扩展名分。
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
  "${SIMPLEXC_ROOT}/tools/compile_hook.c"
  "${SIMPLEXC_ROOT}/simplexc/shell/simplexc_core.cc"
  "${SIMPLEXC_EMBEDDED_OBJS}"
  ${SIMPLEXC_CORE_SOURCES}
  "${SIMPLEXC_OBJ}/assemble.o")
# 产物名是 compile_hook.dll（不是 libhook.dll）：部署入口与游戏按这个名字找它。
set_target_properties(hook PROPERTIES
  PREFIX ""
  OUTPUT_NAME "compile_hook")
target_include_directories(hook PRIVATE "${SIMPLEXC_DIR}")
# 仓库根编进库（"为什么不是运行时推导"见 tools/compile_hook.c 的路径宏注释）：
# 壳允许被 -DOUT_DIR=... 覆盖（离线判据靠它隔离工作目录），但**不再自己持有绝对路径**。
target_compile_definitions(hook PRIVATE "TC_TOOLKIT_ROOT=\"${SIMPLEXC_ROOT}\"")
# -static：把 libstdc++/libgcc/libwinpthread 一起包进来，否则游戏目录还要多拷三个 DLL。
# 旧命令行在链接行上还抄了个优化开关：无 LTO 时链接器不看它，产物逐字节相同，已删。
target_link_options(hook PRIVATE -static)
target_link_libraries(hook PRIVATE simplexc_flags LLVMC::LLVMC)

# `simplexc_dll` 是**同一个**目标的另一个名字（与其它 simplexc 目标对齐；部署入口转调它）。
# 别名不做成第二个库：两份产物只会漂。
add_custom_target(simplexc_dll DEPENDS hook)

# ---------------------------------------------------------------- core_selftest（判据入口）
#
# 判据入口（不进 DLL）：直接把源码字节喂给 `simplexc_compile` 并落盘，供宿主跑。
# 与 DLL 同一套核心、同一套源文件表 —— 否则判据判的不是交付物。
# 没有 -static（与 hook 的差别保持原样）。EXCLUDE_FROM_ALL：不在 `all` 里。
add_executable(core_selftest
  "${SIMPLEXC_ROOT}/simplexc/shell/core_selftest.cc"
  "${SIMPLEXC_ROOT}/simplexc/shell/simplexc_core.cc"
  "${SIMPLEXC_EMBEDDED_OBJS}"
  ${SIMPLEXC_CORE_SOURCES}
  "${SIMPLEXC_OBJ}/assemble.o")
set_target_properties(core_selftest PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_include_directories(core_selftest PRIVATE "${SIMPLEXC_DIR}")
target_link_libraries(core_selftest PRIVATE simplexc_flags LLVMC::LLVMC)

# ---------------------------------------------------------------- hir_selftest
#
# sema 的自检入口（不进产品路径）：只要 sema 自己的文件，不链前端、不链 LLVM ——
# 它的 golden 是 `simplexc/sema/testdata/hir_sample.dump.txt`，路径由 `__FILE__` 推出，
# 所以 `-I simplexc/sema` 与源文件路径都不能省（见 sema/hir_selftest.cc 的 golden_path）。
add_executable(hir_selftest
  "${SIMPLEXC_ROOT}/simplexc/sema/hir_selftest.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/hir_dump.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/types.cc")
target_include_directories(hir_selftest PRIVATE "${SIMPLEXC_DIR}/sema")
target_link_options(hir_selftest PRIVATE -static)
target_link_libraries(hir_selftest PRIVATE simplexc_flags)

# ---------------------------------------------------------------- analyze_selftest
#
# sema 的第二个判据入口：`.si` → 前端 AST → `sema::analyze` → dump，与 `sema/testdata/cases/*.dump`
# 逐字比 + 六条不变量。**不链 LLVM**：sema 里本来就没有 LLVM，判据要把这件事当场证明。
# 链前端是因为输入是源码；`.si` → AST 那一段粘合属于判据侧（见 analyze_selftest.cc 头注释）。
add_executable(analyze_selftest
  "${SIMPLEXC_ROOT}/simplexc/sema/analyze_selftest.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/analyze.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/layout.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/hir_dump.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/types.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/decls.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/lexer.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/parser.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/frontend.cc")
target_include_directories(analyze_selftest PRIVATE "${SIMPLEXC_DIR}/sema")
target_link_options(analyze_selftest PRIVATE -static)
target_link_libraries(analyze_selftest PRIVATE simplexc_flags)

# ---------------------------------------------------------------- ast_dump（前端的判据入口）
#
# 前端（simplexc/frontend/*.cc）的实现 + 它自己的调试 main：不依赖 LLVM，
# 也不链进产品 DLL（判据入口，不是产线）。
add_executable(ast_dump
  "${SIMPLEXC_ROOT}/simplexc/frontend/ast_dump_main.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/decls.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/lexer.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/parser.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/frontend.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/ast_dump.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/json.cc")
target_link_options(ast_dump PRIVATE -static)
target_link_libraries(ast_dump PRIVATE simplexc_flags)

# ---------------------------------------------------------------- codegen_selftest
#
# codegen 的 L4 判据入口：`.si` 走产品链（Frontend → build_module → analyze → `codegen::Program`）
# 出 IR，再逐条查 `codegen/testdata/cases/*.irkeys`。要链 LLVM 是因为被测对象
# （codegen.cc + llvm_ffi.cc）本来就在 LLVM 上；不用 assemble.o —— 它只出 IR，不出产物
# （产物那一路由 blob_baseline 判）。
add_executable(codegen_selftest
  "${SIMPLEXC_ROOT}/simplexc/codegen/codegen_selftest.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/decls.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/lexer.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/parser.cc"
  "${SIMPLEXC_ROOT}/simplexc/frontend/frontend.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/module_build.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/codegen.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/asm_x64.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/llvm_ffi.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/analyze.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/layout.cc"
  "${SIMPLEXC_ROOT}/simplexc/sema/types.cc")
target_include_directories(codegen_selftest PRIVATE "${SIMPLEXC_DIR}")
target_link_options(codegen_selftest PRIVATE -static)
target_link_libraries(codegen_selftest PRIVATE simplexc_flags LLVMC::LLVMC)

# ---------------------------------------------------------------- spike（空程序 blob）
#
# 「空程序」blob —— 只证明链路通，**不能拿进游戏**（会让游戏在下一次编译请求时 abort，见
# docs/pitfalls.md）。现在没有判据读它：新版壳在进程内编源码，基线壳只在 reference 模式下被驱动。
# --obj 顺序 = 链接顺序；entry 是 shim 的符号。cwd = 仓库根、输出路径写相对 —— 与旧命令一致，
# blob 的 sha256 才是可比的。
add_custom_target(spike
  COMMAND "$<TARGET_FILE:tcblob>"
          --obj build/objs/rt_min.o --obj build/objs/entry_shim.o
          --obj build/objs/prog_empty.o
          --entry __tc_entry --out build/blob_empty.bin
  DEPENDS tcblob objs
  WORKING_DIRECTORY "${SIMPLEXC_ROOT}"
  COMMENT "tcblob 装出 build/blob_empty.bin（空程序，只证明链路通）"
  VERBATIM)

# ---------------------------------------------------------------- difftrace（判据工具，不在默认构建里）
#
# 要 Unicorn / Capstone 的 C SDK（work/tools/difftrace-deps/，不进 git，由 fetch_deps.py 备齐）。
# 缺 SDK 只让这一个目标不可用 —— 默认构建的成功不能依赖它。
set(SIMPLEXC_DIFFTRACE_DEPS_HINT
  "difftrace 的 C SDK（Unicorn / Capstone）不进 git，先跑 python simplexc/tools/difftrace/fetch_deps.py 备齐头文件与导入库。")
find_package(Unicorn QUIET)
find_package(Capstone QUIET)

if(Unicorn_FOUND AND Capstone_FOUND)
  # difftrace 默认读的 `compile_real.dll` 所在目录（`image.hh` 的 `kDllReal`）。
  # 空 = 用 image.hh 的占位默认值；运行期还能用 `--dll-dir` / `DIFFTRACE_DLL_DIR` 覆盖。
  set(SIMPLEXC_GAME_DIR "$ENV{TC_GAME_DIR}" CACHE PATH
    "游戏目录（difftrace 默认从这里读 compile_real.dll）")
  # 源文件表按目录列而不是写死清单：唯一规则是「`*_selftest.cc` 自带 main，不能进 exe」。
  file(GLOB SIMPLEXC_DIFFTRACE_SOURCES CONFIGURE_DEPENDS "${SIMPLEXC_DIR}/tools/difftrace/*.cc")
  list(FILTER SIMPLEXC_DIFFTRACE_SOURCES EXCLUDE REGEX "_selftest\\.cc$")
  add_executable(difftrace ${SIMPLEXC_DIFFTRACE_SOURCES})
  set_target_properties(difftrace PROPERTIES EXCLUDE_FROM_ALL TRUE)
  target_link_libraries(difftrace PRIVATE simplexc_strict_flags Unicorn::Unicorn Capstone::Capstone)
  if(SIMPLEXC_GAME_DIR)
    file(TO_CMAKE_PATH "${SIMPLEXC_GAME_DIR}" _game_dir)
    target_compile_definitions(difftrace PRIVATE "TC_GAME_DIR=\"${_game_dir}\"")
  endif()
  # Windows 先从 exe 所在目录找依赖 DLL → 两个 dll 放 build/ 旁边，跑判据不用改 PATH。
  add_custom_command(TARGET difftrace POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNICORN_RUNTIME_DLL}"
            "${SIMPLEXC_BUILD}/unicorn.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CAPSTONE_RUNTIME_DLL}"
            "${SIMPLEXC_BUILD}/capstone.dll"
    COMMENT "拷贝 unicorn.dll / capstone.dll → build/"
    VERBATIM)
else()
  # 目标名照样占住：缺 SDK 时 `--target difftrace` 要给出那句 hint 原文并以非 0 退出，
  # 而不是让 ninja 报一句 unknown target 把原因吞掉。
  add_custom_target(difftrace
    COMMAND ${CMAKE_COMMAND} -E echo "${SIMPLEXC_DIFFTRACE_DEPS_HINT}"
    COMMAND ${CMAKE_COMMAND} -E false
    COMMENT "difftrace 不可用（缺 C SDK）"
    VERBATIM)
endif()

# ---------------------------------------------------------------- difftrace_selftest
#
# 判据核心（diff.*）不碰 Unicorn/Capstone —— 这个目标因此不需要 SDK，能单独编、单独跑。
# 编完当场跑：用例 FAIL 是判据的结论（不是构建出错），退出码原样透传。
add_executable(difftrace_selftest
  "${SIMPLEXC_ROOT}/simplexc/tools/difftrace/diff.cc"
  "${SIMPLEXC_ROOT}/simplexc/tools/difftrace/diff_selftest.cc")
set_target_properties(difftrace_selftest PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_link_libraries(difftrace_selftest PRIVATE simplexc_strict_flags)
add_custom_command(TARGET difftrace_selftest POST_BUILD
  COMMAND "$<TARGET_FILE:difftrace_selftest>"
  WORKING_DIRECTORY "${SIMPLEXC_ROOT}"
  COMMENT "跑 difftrace_selftest"
  VERBATIM)

# ---------------------------------------------------------------- asm_x64_selftest
#
# stdlib `asm x86_64` 块编码器的判据：白名单助记符 → 写死的字节（标 `ref` 的取自参考产物）。
# 只编这两个 .cc，不链前端、不链 LLVM、不链 assemble.o —— 被测对象是纯函数。
# 不在默认构建里：它还没接进编译流水线。
add_executable(asm_x64_selftest
  "${SIMPLEXC_ROOT}/simplexc/codegen/asm_x64.cc"
  "${SIMPLEXC_ROOT}/simplexc/codegen/asm_x64_selftest.cc")
set_target_properties(asm_x64_selftest PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_link_options(asm_x64_selftest PRIVATE -static)
target_link_libraries(asm_x64_selftest PRIVATE simplexc_strict_flags)
add_custom_command(TARGET asm_x64_selftest POST_BUILD
  COMMAND "$<TARGET_FILE:asm_x64_selftest>"
  WORKING_DIRECTORY "${SIMPLEXC_ROOT}"
  COMMENT "跑 asm_x64_selftest"
  VERBATIM)

# ---------------------------------------------------------------- 离线判据的两份壳（hook_offline.py）
#
# 判据要编**测试用**的壳：核心与产品 `hook` 相同，但路径宏（OUT_DIR / OPT_DIR / OPT_INI /
# OPT_SCRIPT）指向 work/tmp/hook_offline/ —— 判据不能往 work/hook/ 的活数据里掺行，所以不能
# 直接用 `hook`。这些宏是构建定义的一部分，因此写在这里；判据只传「工作目录在哪」这一个值，
# 它不再自带一份编译命令行。
set(SIMPLEXC_HOOK_OFFLINE_WORK "${SIMPLEXC_ROOT}/work/tmp/hook_offline" CACHE PATH
  "hook_offline 的工作目录（目录布局由 hook_offline.py 决定，它每次 configure 都会传进来）")
set(SIMPLEXC_HOOK_OFFLINE_BIN "${SIMPLEXC_HOOK_OFFLINE_WORK}/bin")
# 基线壳源码：hook_offline.py 从 git 历史里取出那份「改动前」的壳并落盘，不在工作区里。
set(SIMPLEXC_HOOK_BASE_SRC "${SIMPLEXC_HOOK_OFFLINE_WORK}/compile_hook_base.c")
set_source_files_properties("${SIMPLEXC_HOOK_BASE_SRC}" PROPERTIES GENERATED TRUE)

# 路径宏的值：`work/<variant>/{out,opt,ini,opt.ini}` 与替身改写器，逐条与 hook_offline.py 的
# vdirs()/STUB 对应。
function(simplexc_hook_offline_defs variant target)
  set(_v "${SIMPLEXC_HOOK_OFFLINE_WORK}/${variant}")
  target_compile_definitions(${target} PRIVATE
    "TC_TOOLKIT_ROOT=\"${SIMPLEXC_ROOT}\""
    "OUT_DIR=\"${_v}/out\""
    "OPT_DIR=\"${_v}/opt\""
    # 工作区壳默认把 hook.log 写进 OUT_DIR；pin 到 opt/ 才与 case 的 snapshot() 读的位置一致
    # （基线壳本来就写 OPT_DIR = opt/，多一个宏对它无害）。
    "HOOK_LOG_DIR=\"${_v}/opt\""
    "OPT_INI=\"${_v}/opt.ini\""
    "OPT_SCRIPT=\"${SIMPLEXC_HOOK_OFFLINE_WORK}/tools/rewrite_stub.py\"")
endfunction()

add_library(hook_offline_new SHARED
  "${SIMPLEXC_ROOT}/tools/compile_hook.c"
  "${SIMPLEXC_ROOT}/simplexc/shell/simplexc_core.cc"
  "${SIMPLEXC_EMBEDDED_OBJS}"
  ${SIMPLEXC_CORE_SOURCES}
  "${SIMPLEXC_OBJ}/assemble.o")
set_target_properties(hook_offline_new PROPERTIES
  PREFIX "" OUTPUT_NAME "compile_hook_new" EXCLUDE_FROM_ALL TRUE
  RUNTIME_OUTPUT_DIRECTORY "${SIMPLEXC_HOOK_OFFLINE_BIN}")
target_include_directories(hook_offline_new PRIVATE "${SIMPLEXC_DIR}")
target_link_options(hook_offline_new PRIVATE -static)
target_link_libraries(hook_offline_new PRIVATE simplexc_flags LLVMC::LLVMC)
simplexc_hook_offline_defs(new hook_offline_new)

# 基线壳（`BASELINE_REF` 的壳）不引用核心：单文件 C，不需要 C++ 运行库。
add_library(hook_offline_base SHARED "${SIMPLEXC_HOOK_BASE_SRC}")
set_target_properties(hook_offline_base PROPERTIES
  PREFIX "" OUTPUT_NAME "compile_hook_base" EXCLUDE_FROM_ALL TRUE
  RUNTIME_OUTPUT_DIRECTORY "${SIMPLEXC_HOOK_OFFLINE_BIN}")
target_link_options(hook_offline_base PRIVATE -static)
target_link_libraries(hook_offline_base PRIVATE simplexc_flags)
simplexc_hook_offline_defs(base hook_offline_base)

# ---------------------------------------------------------------- clangd 入口（最后一步）
# 把 CMake 生成的编译数据库拷到 `simplexc/`（与本文件同级）：clangd 从源文件所在目录向上找
# `compile_commands.json`，而源文件都在 `simplexc/**` 下，`build/cmake/` 不在那条上行链上。
# 用 target 而不是 configure_file 是因为 json 在 **generate 阶段结束**才写，configure 期拿到的会是上一轮的。
add_custom_target(compile_commands ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/compile_commands.json"
          "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
  COMMENT "更新 simplexc/compile_commands.json（给 clangd）"
  VERBATIM)
