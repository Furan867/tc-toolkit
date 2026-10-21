# 事实（分流）

结论分文件存放，本文件不再承载条目。**禁止在本文件写条目**。

纪律（作用于 `facts/` 全体）：**就地改写，不留旧值**（旧值看 git 历史）；每条必须带「验证：」与「最后验证：」。
验证方式写「静态核对」的，表示读代码/反汇编对过；写命令的，表示那条命令跑通。

| 文件 | 管什么（检索关键词） |
|---|---|
| `facts/forwarding.md` | compile.dll 转发层、参考实现 compile_real.dll（extern / stub / IAT / manifest / Nim 字符串 / 内建清单来源） |
| `facts/corpus-host.md` | 语料与判据、差分宿主、simctl、M1.5 探针（tick 全等、reset / seed、回退、判据入口） |
| `facts/semantics.md` | 语言与 lowering 语义档案（U-L1…U-L14、位宽 / 掩码 / zext、内建、捕获 / 闭包、泛型、数组、enum、插值、`Str`、`random`） |
| `facts/build-assemble.md` | 构建与 LLVM（版本 / 导入库）、装配器与产物确定性（stub 槽、entry、reloc、`.bss`、可复现） |
| `facts/perf-tooling.md` | 组件映射、采样、机器码改写、主频读数、静态逆向（收益归因散在以上各节的实测里） |

## 新条目进哪个文件

- 按结论的**主语**归属，不按它是哪次任务发现的：转发层 → `forwarding`；语言 / lowering 语义 → `semantics`；
  判据与器材 → `corpus-host`；产物生成、装配、可确定复现 → `build-assemble`；测量与优化手段 → `perf-tooling`。
- 跨界的，取「查它时最常从哪一节进来」。
- 五个都不合适 → 新建 `facts/<主题>.md`，**只在本文件的分流表注册一行**；别处不复述范围。
- 一条结论只属于一个文件；别的文件要用就写一行指针，不复述内容。
