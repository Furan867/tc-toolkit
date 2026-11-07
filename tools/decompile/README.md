# tools/decompile/

反编译 / PE 解析脚本。**零产品依赖**：没有别的代码 import 它们，它们也不 import `tools/` 里的东西（只用标准库 + `libs.tc_common`）。

| 脚本 | 干什么 | 输入 / 输出 |
|---|---|---|
| `pdata_bounds.py` | x64 PE 的函数精确边界（`.pdata`） | 游戏 exe → `work/function_bounds.tsv` |
| `find_sym.py` | 按模块 / 名字 / 地址查符号 | `work/symbols_raw.txt` + 上面的 tsv |
| `nim_demangle.py` | Nim 符号 → 项目目录树 | `work/symbols_raw.txt` → `recovered/src/` |
| `demangle_dll.py` | 解析 dll 里的 Nim 符号 | 游戏 dll → `recovered/compiler-src/` |
| `coff_symbols.py` | COFF 符号表 → 编译单元映射 | 游戏 exe → `work/compile_units.txt` |
| `pe_overlay.py` | PE 结构与 overlay dump | 游戏 exe → `work/overlay.bin` |
| `extract_nim_strings.py` | 从 PE 提取 Nim 字面量 / 指针表 | 游戏 dll |
| `organize_strings.py` | 整理 `work/exe_strings.txt` 分类去重 | 一次性：**会删输入** |

**约定**：

- 判断标准是"**通用性**"，不是"最近跑过没有"——这些工具都是按需用的，任何 PE / Nim 产物都能复用。
- 除 `find_sym` / `extract_nim_strings` 外都没有 argparse，`--help` 会**真跑并落盘**，别拿它当冒烟测试。
- `symbols_raw.txt` 没有生成器，是手工 `nm` 输出的约定。
- 入口在仓库根跑：`python tools/decompile/<x>.py`（脚本自己按三层 `dirname` 算仓库根）。
