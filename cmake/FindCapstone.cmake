# Capstone::Capstone —— difftrace 用的 Capstone 5.0.7（`work/tools/difftrace-deps/capstone`）。
#
# 与 FindUnicorn.cmake 同一套约定（那份说明在这里不重复）：SDK 不进 git、找不到不算错误，
# 缺它只让 difftrace 目标不可用。
#
# 输入：CAPSTONE_ROOT（默认 <repo>/work/tools/difftrace-deps/capstone，可 -D 覆盖）
# 产出：target Capstone::Capstone（UNKNOWN IMPORTED）；变量 CAPSTONE_RUNTIME_DLL（lib/capstone.dll）

if(NOT CAPSTONE_ROOT)
  set(CAPSTONE_ROOT "${CMAKE_SOURCE_DIR}/work/tools/difftrace-deps/capstone")
endif()

find_path(CAPSTONE_INCLUDE_DIR
  NAMES capstone/capstone.h
  PATHS "${CAPSTONE_ROOT}/include"
  NO_DEFAULT_PATH)
find_library(CAPSTONE_LIBRARY
  NAMES capstone
  PATHS "${CAPSTONE_ROOT}/lib"
  NO_DEFAULT_PATH)
find_file(CAPSTONE_RUNTIME_DLL
  NAMES capstone.dll
  PATHS "${CAPSTONE_ROOT}/lib"
  NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Capstone DEFAULT_MSG CAPSTONE_INCLUDE_DIR CAPSTONE_LIBRARY)

if(Capstone_FOUND)
  add_library(Capstone::Capstone UNKNOWN IMPORTED)
  set_target_properties(Capstone::Capstone PROPERTIES
    IMPORTED_LOCATION "${CAPSTONE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CAPSTONE_INCLUDE_DIR}")
endif()

mark_as_advanced(CAPSTONE_INCLUDE_DIR CAPSTONE_LIBRARY CAPSTONE_RUNTIME_DLL)
