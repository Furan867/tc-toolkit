# hotspot/consumer 的构建唯一定义（语言标准、告警都只在这里）。
#
#   cmake -S hotspot/consumer -B hotspot/consumer/build
#   cmake --build hotspot/consumer/build
#   ctest --test-dir hotspot/consumer/build --output-on-failure
#
# `build/` 与 `compile_commands.json` 不入库（见根 .gitignore）。
cmake_minimum_required(VERSION 3.21)

project(hotspot_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clangd / LSP 用真实命令行解析源码；CMake 每次 configure 会重写它。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------- 告警开关（唯一一处）
add_library(hotspot_flags INTERFACE)
target_compile_options(hotspot_flags INTERFACE -Wall -Werror)

# ------------------------------------------------------------------------ 共享聚合层 + REPL
add_library(hotspot_core STATIC
    src/artifact.cc
    src/backend_suspend.cc
    src/commands.cc
    src/config.cc
    src/locate.cc
    src/lock.cc
    src/run_dir.cc
    src/samples.cc
    src/session.cc
    src/sha256.cc
    src/win_process.cc)
target_include_directories(hotspot_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hotspot_core PRIVATE hotspot_flags)

add_executable(hotspot-consumer src/main.cc)
target_link_libraries(hotspot-consumer PRIVATE hotspot_core hotspot_flags)

# ------------------------------------------------------------------------------ 单测
enable_testing()

add_executable(hotspot_tests
    tests/main.cc
    tests/test_commands.cc
    tests/test_features.cc
    tests/test_run_dir.cc
    tests/test_samples.cc
    tests/test_session.cc)
target_link_libraries(hotspot_tests PRIVATE hotspot_core hotspot_flags)

add_test(NAME hotspot_tests COMMAND hotspot_tests)

# ---------------------------------------------------------------- clangd 入口（最后一步）
# 把 CMake 生成的编译数据库拷到本文件同级：clangd 从源文件所在目录向上找
# compile_commands.json，而构建树是本目录的子目录，不在那条上行链上。用 target 而不是
# configure_file，是因为 json 在 generate 阶段结束才写（与 simplexc 同一套做法）。
add_custom_target(compile_commands ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/compile_commands.json"
          "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
  COMMENT "更新 hotspot/consumer/compile_commands.json（给 clangd）"
  VERBATIM)
