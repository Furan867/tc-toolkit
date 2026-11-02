# tc-toolkit

《Turing Complete》的工具集：内建编译器的开源替代（`simplexc/`）与逆向分析工具（`tools/`）。

游戏用 `compile.dll` 把 Simplex 电路源码编译成可 JIT 执行的产物。`simplexc/` 提供一个同名转发 DLL
接管这个入口，把编译请求交给自研的 Simplex → LLVM IR → x64 装配流水线；不修改游戏 EXE，随时可还原。
`tools/` 里是理解游戏二进制与运行时的分析脚本：内存扫描、符号还原、热点采样等。

## 状态

离线判据跑 98 份语料（战役关卡、rv32im、stdlib 前缀）：**95 份逐 tick 状态全等**，3 份存在差异、原因待查。
明细与未解项见 [docs/status.md](docs/status.md)。

游戏内还有一个未解的崩溃：关卡「逻辑试炼」（031/032）曾观察到 `pc` 落进未映射页，后来在离线与参考侧
都复现不出来，原先的根因判断已被推翻，目前缺现场重新取证 —— 见
[docs/bugs/2026-10-26-031-unreproducible-crash.md](docs/bugs/2026-10-26-031-unreproducible-crash.md)。
它不在 98 份语料里，不影响上面的离线判据，只影响游戏内跑这一关。

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

## 切换编译器参数

`opt.ini` 是运行时开关，转发层每次编译都重读它 —— 改完下次编译生效，不用重启游戏。
`build-deploy` 会在仓库根没有这个文件时从模板生成一份。

| 键 | 取值 | 作用 |
| --- | --- | --- |
| `compiler` | `ref`（默认）/ `ours` | 用官方实现；或只用我们的（失败就报错，不转交） |
| `stdlib` | `on`（默认）/ `off` | 编译时拼不拼 stdlib 前缀（只对 `compiler = ours` 生效） |
| `rewrite` | `off`（默认）/ `on` / `short` / `short_cse` | 产物改写；默认关（它是自写编译器之前的性能产物，现在作用很小） |

其余键与注意事项写在 [opt.ini.example](opt.ini.example) 的注释里。

## 开发

- 代码：`simplexc/`（编译器与转发层）、`tools/`（逆向分析工具）。构建脚本在 `simplexc/CMakeLists.txt`，
  产物落 `simplexc/build/`。
- 文档索引：[docs/map.md](docs/map.md)。
- Simplex 的运行期语义：[docs/facts/semantics.md](docs/facts/semantics.md)。
- 官方编译器的行为（重载择优、`asm` 重新汇编、`def` 非重入……）：[docs/facts/ref-compiler.md](docs/facts/ref-compiler.md)。
- 转发层与游戏侧的契约：[docs/facts/forwarding.md](docs/facts/forwarding.md)。
- 已知的坑：[docs/pitfalls.md](docs/pitfalls.md)。

## 许可

[MIT](LICENSE)

第三方依赖及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
