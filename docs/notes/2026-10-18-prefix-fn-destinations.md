# 前缀函数与 runtime 的现状：三种符号去向（2026-10-18）

**一句话**：stdlib 前缀**不参与编译**（走路线 B），而「路线 B 的 runtime」现在主要不在
`runtime/` 目录里 —— 它在 lowering 里按需生成 IR。**别拿 `runtime/` 的体量去估 runtime 的覆盖度。**

## 三种符号去向（rv32im 实测）

| 符号类别 | 谁提供 | 证据 |
| --- | --- | --- |
| 内建（`load`/`store`/`raw_cast`/`index_read`/`index_write`/…） | lowering 直接映射成 LLVM IR，不经运行时 | `simplexc/lower/builtins.def`（接口 §2 的唯一定义） |
| `get_time` / `sleep` / `memory_clear` / `memory_copy` | **lowering 现场生成 IR 函数体**，`LLVMInternalLinkage`（本地函数，不是外部符号） | `lower/lower.cc:1946 ensure_prefix_fn()` → `body_get_time` / `body_sleep` / `body_memory_clear` / `body_memory_copy` |
| `Sleep` / `GetSystemTimePreciseAsFileTime` / `CreateFileA` / `WriteFile` / `CloseHandle` / `ExitThread` | 桩 / 槽（`GetProcAddress` 编译期解析） | rv32im 产物的 `.stubs` 恰好这 6 条（2 桩 + 4 槽） |

`thread_exit` 是唯一走 `runtime/` 的：`runtime/rt_min.cc`（**48 行**）随 `rt_min.o` 装配进 blob。

## 两条容易搞错的推论

1. **`.stubs` 里没有 `memory_copy`/`memory_clear` 不代表它们被内联进调用点** —— 它们是
   `ensure_prefix_fn()` 生成的**内部链接函数**，所以既不是外部符号、也不进 `.stubs`。
2. **stdlib 前缀不参与编译**：产品路径里没有任何代码去拼那 27711 B 前缀
   （`grep stdlib_prefix` 在 `cli/`、`frontend/`、`shell/` 零命中），`simplexc_cli --src` 只吃用户源码。
   `runtime/README.md` 自己也写着「现状只有 `rt_min.cc`，不是终态；终态由 stdlib 源码取代（路线 A）」。

## 对 M5 的含义

这条路只覆盖了「hook 语料实际用到的那 5 个前缀函数」。覆盖面一变宽（campaign 92 份、
stdlib 路线 A），撞到 `body_*` 里没有的前缀函数时只有两种结局：`Unsupported` 报错，或者
成为 `ExternResolver` 解析不了的未定义符号。这也是 M5 第三块（路线 A）存在的理由。

## 复现方式

```bash
cat work/tmp/check/lower_equiv/ours.bin.stubs        # rv32im 的 6 条桩/槽
grep -n 'ensure_prefix_fn' simplexc/lower/lower.cc   # 1946 起
grep -rn 'stdlib_prefix' simplexc/cli simplexc/frontend simplexc/shell   # 零命中
wc -l simplexc/runtime/*.cc simplexc/runtime/README.md
```
