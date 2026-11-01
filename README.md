# tc-toolkit

《Turing Complete》的工具集：内建编译器的开源替代（`simplexc/`）与逆向分析工具（`tools/`）。

游戏用 `compile.dll` 把 Simplex 电路源码编译成可 JIT 执行的产物。`simplexc/` 提供一个同名转发 DLL
接管这个入口，把编译请求交给自研的 Simplex → LLVM IR → x64 装配流水线；不修改游戏 EXE，随时可还原。
`tools/` 里是理解游戏二进制与运行时的分析脚本：内存扫描、符号还原、热点采样等。

## 状态

离线判据跑 98 份语料（战役关卡、rv32im、stdlib 前缀）：**95 份逐 tick 状态全等**，3 份存在差异、原因待查。
明细与未解项见 [docs/status.md](docs/status.md)。

判据、产物布局与接口仍在演进，可能发生破坏性变更。

## 运行

依赖：GCC 14+（`g++` / `gcc`）、CMake 3.21+、Ninja、Python 3.13、`zstd`。

### 取 LLVM 的 C API 头与导入库

```sh
python simplexc/tools/fetch_llvm.py --tag llvmorg-23.1.3
```

落到 `simplexc/.deps/llvm`（约 260 MB，不进 git），编译与转发层都从这里取头文件和 `LLVM-C.dll`。

### 编译并部署到游戏

```sh
python tools/deploy_compile_hook.py build-deploy
```

这一条命令会编译转发层并装进游戏目录：官方 `compile.dll` 被改名为 `compile_real.dll`，
我们的转发层以 `compile.dll` 的名字被游戏加载。**游戏必须先退出**。

还原：

```sh
python tools/deploy_compile_hook.py restore
```

### 跑判据（可选）

```sh
python simplexc/tools/check.py
```

六步里 `corpus`、`host_selftest`、`lower_equiv`、`hook_offline` 需要游戏目录中存在
`compile_real.dll`（部署过转发层即有；也可以手工放一份官方 DLL 的副本）。

## 开发

- 代码：`simplexc/`（编译器与转发层）、`tools/`（逆向分析工具）。构建脚本在 `simplexc/CMakeLists.txt`，
  产物落仓库根的 `build/`。
- 文档索引：[docs/map.md](docs/map.md)。
- Simplex 的运行期语义：[docs/facts/semantics.md](docs/facts/semantics.md)。
- 官方编译器的行为（重载择优、`asm` 重新汇编、`def` 非重入……）：[docs/facts/ref-compiler.md](docs/facts/ref-compiler.md)。
- 转发层与游戏侧的契约：[docs/facts/forwarding.md](docs/facts/forwarding.md)。
- 已知的坑：[docs/pitfalls.md](docs/pitfalls.md)。

## 许可

[MIT](LICENSE)

第三方依赖及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
