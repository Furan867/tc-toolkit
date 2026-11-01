# 第三方依赖声明

本仓库的**源码**以 [MIT](LICENSE) 许可发布。构建与可选工具依赖下列第三方组件，它们**都不随本仓库分发**，
由使用者按各节给出的脚本或路径自行获取。

## LLVM 23.1.3

- 许可证：Apache-2.0 WITH LLVM-exception
- 用在哪：`compile.dll` 与 `simplexc_cli` 链接它的 C API（`LLVMC::LLVMC`）
- 是否随仓库分发：**否**。由 `python simplexc/tools/fetch_llvm.py` 下载到本地（`simplexc/.deps/`，已在 `.gitignore`）

## Capstone 5

- 许可证：BSD-3-Clause
- 用在哪：仅 `difftrace` 目标（可选，`EXCLUDE_FROM_ALL`，不在默认构建里）
- 是否随仓库分发：**否**。由 `python simplexc/tools/difftrace/fetch_deps.py` 备齐

## Unicorn 2.1.4

- 许可证：GPL-2.0
- 用在哪：仅 `difftrace` 目标（可选，`EXCLUDE_FROM_ALL`，不在默认构建里）
- 是否随仓库分发：**否**。由 `python simplexc/tools/difftrace/fetch_deps.py` 备齐

## 关于 difftrace 二进制

`difftrace` 链接 GPL-2.0 的 Unicorn，因此它的**二进制产物不受本仓库 MIT 许可覆盖**；
本仓库只提供 `difftrace` 的源码，由使用者自行获取依赖并编译。不用 `difftrace` 时其余部分与 Unicorn 无关
（默认构建不含该目标）。
