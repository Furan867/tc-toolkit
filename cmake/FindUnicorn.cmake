# Unicorn::Unicorn —— difftrace 用的 Unicorn 2.1.4（`work/tools/difftrace-deps/unicorn`）。
#
# 为什么不用 find_package 找环境里的包：这套 C SDK 是从 Python wheel 里抠出来、再用 dlltool
# 造成 MinGW 导入库的（`simplexc/tools/difftrace/fetch_deps.py`），位置固定在 work/tools/ 下、
# 不进 git。所以这里**不 REQUIRED**：缺 SDK 只该让 difftrace 那一个目标不可用，
# 默认构建（不含 difftrace）不能因此失败。
#
# 输入：UNICORN_ROOT（默认 <repo>/work/tools/difftrace-deps/unicorn，可 -D 覆盖）
# 产出：target Unicorn::Unicorn（UNKNOWN IMPORTED）；变量 UNICORN_RUNTIME_DLL（lib/unicorn.dll）

if(NOT UNICORN_ROOT)
  set(UNICORN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../work/tools/difftrace-deps/unicorn")
endif()

find_path(UNICORN_INCLUDE_DIR
  NAMES unicorn/unicorn.h
  PATHS "${UNICORN_ROOT}/include"
  NO_DEFAULT_PATH)
find_library(UNICORN_LIBRARY
  NAMES unicorn
  PATHS "${UNICORN_ROOT}/lib"
  NO_DEFAULT_PATH)
find_file(UNICORN_RUNTIME_DLL
  NAMES unicorn.dll
  PATHS "${UNICORN_ROOT}/lib"
  NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Unicorn DEFAULT_MSG UNICORN_INCLUDE_DIR UNICORN_LIBRARY)

if(Unicorn_FOUND)
  add_library(Unicorn::Unicorn UNKNOWN IMPORTED)
  # SYSTEM 头（-isystem）：Unicorn 是别人的头，其告警不该撞本项目的 -Werror。
  # 两个属性都要写：前者给路径，后者才把它标成 SYSTEM（见 FindLLVMC.cmake）。
  set_target_properties(Unicorn::Unicorn PROPERTIES
    IMPORTED_LOCATION "${UNICORN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${UNICORN_INCLUDE_DIR}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${UNICORN_INCLUDE_DIR}")
endif()

mark_as_advanced(UNICORN_INCLUDE_DIR UNICORN_LIBRARY UNICORN_RUNTIME_DLL)
