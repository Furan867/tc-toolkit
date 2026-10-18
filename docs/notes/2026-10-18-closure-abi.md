# 顶层 def 闭包化：捕获按调用点算，无捕获的签名逐字节不变

日期 2026-10-18 · 改 `simplexc/lower/{lower.hh,lower.cc}` · 现场 `work/tmp/closureabi/`
前一轮（嵌套 def 的捕获摊平 + root 体 `var` 口径）见 `2026-10-18-closure-var.md`；本轮补的是
**跨顶层 `def` 边界**的那一段。

## 0. 结论先行

1. **闭包捕获这一类归零**：98 份里 `未定义的名字` **30 → 0**（counter 12 · test 4 · instructions 3 ·
   input 2 · memory 2 · 另 7 个各 1）；其中 **13 份**当场变成可装配（**139 → 逐 tick 全等**，
   024/075/076/077/078/096/098/108/110/112/114/116 → diff），**17 份**换到下一道墙
   （`编译期类型参数 Size`）。
2. **无捕获的顶层 def：键、函数名、参数表、函数体、产物逐字节不变**。判据不是推理，是实测：
   改前/改后两个二进制各跑一遍 98 份 + rv32im，`blob + .entry + .stubs` 的 md5
   **86 份相同**，13 份不同**恰好就是改前编不过的那 13 份**。
   ⇒ 20 份全等与 rv32im 的产物**一个字节没动**（`check.py` 的 `lower_equiv` 10 tick 差异 0 复核）。
3. 98 份口径：**57 可装配 · 20 全等 · 28 diff · 9 host_fail · 41 cli_fail** →
   **70 可装配 · 21 全等 · 40 diff · 9 host_fail · 28 cli_fail**（装配回退率 41.8% → 28.6%）。

## 1. 方案（三条）

**捕获怎么传** —— 与嵌套 def 同一形状：声明参数在前、捕获按**引用**（只传 `ptr`，不复制内存）
追加在后；函数体里把捕获绑成 `Binding::capture`，于是它再调 def / 嵌套 def 时 `lookup` 能查到，
并按同样规则继续往下传。捕获表**在调用点算**（`call_user` 里 `def_captures(d)`），
与参考实现「自由名在调用点解析、顺序敏感」一致（`closure-var.md` §3 的 E/K/F 片段）。

**`fns_` 键怎么带签名** —— 顶层 def 的键原本是 `(1, name, lo, "")`，第 4 位空着；现在放
**捕获签名** `name:ty,...`（按首次出现序，空捕获 = `""`）。签名带**类型**而不只是名字：
同名在不同调用点绑不同宽度时，实例里的读写宽度不同，不能复用。实例名 `name$c0` / `name$c1`
（按实例化序），嵌套 def 的键与名字改用它算（`cur_name_`），否则同名 def 的两个实例会共用
同一个嵌套 def 函数。

**能否做到「无捕获的函数签名/ABI 逐字节不变」——能，已实测**（§0 第 2 条）。捕获表是从被调体里
**真的被引用、且调用点绑在栈槽/捕获上**的名字算出来的：自由名全落在模块级（`globals_` /
`consts_` / 枚举 / 前缀 / 其它 def）时表为空 ⇒ 追加参数 0 个、`LLVMFunctionType` 不变、调用点
实参不变、函数体里没有额外的 `Binding::capture` ⇒ 同键同 `Fn`、连生成顺序都不变。
所以「统一改所有函数签名」不必要。

## 2. 改动落点（`lower.cc`）

| 位置 | 改了什么 |
| --- | --- |
| `collect_captures` | 从吃 `DefStmt` 改成吃 `(body, params, into_own_defs)`：顶层 `FunctionDef` 与嵌套 `DefStmt` 共用一套走查 |
| `def_captures`（新） | 顶层 def 的捕获表，入口处 `into_own_defs = true` |
| `call_user` | 调用点算捕获 → 传给 `ensure_def_fn` → 实参尾部追加捕获指针 |
| `ensure_def_fn` | 签名入键、`ptr` 捕获参数、绑 `Binding::capture`、实例名 `$cN`；`cur_name_` 带实例名 |
| `ensure_nested_fn` | 嵌套 def 的 owner 从 `cur_def_->name` 改成 `cur_name_`（只在使用捕获实例时不同） |

**`into_own_defs` 为什么只有顶层 def 要**：自己体里声明的嵌套 def，捕获是在它被调用时**懒生成**的
（`call_nested` → `captures_of`），那一刻看到的是**调用者（顶层 def）的作用域** —— 所以顶层 def
必须在自己的调用点就把那些名字捕进来。110 的链路：root → `mode_run` → `handle_test_result`
（mode_run 体里的嵌套 def）→ `get_input_switched`（root 体 `run_sim:` 里的嵌套 def）→ `seen`
（`run_sim:` 的局部）。嵌套 def 那条路不能同样处理：它会改动已经全等那批的嵌套函数 ABI。

## 3. 判据（原始输出）

```text
# 改前/改后产物 md5（blob + .entry + .stubs），98 份 + rv32im：
  bash work/tmp/closureabi/cmp.sh
    86 same · 13 DIFF        ← 13 份全是改前 rc=1（编不过）的那 13 份
  逐份清单 work/tmp/closureabi/ident.tsv（rv32im= same · 002/004/…/180 = same）

python simplexc/tools/m5/coverage.py
  ── 总 98 · 可装配 70 · 逐 tick 全等 21 · cli_fail 28 · host_fail 9 · diff 40
     装配回退率 28.6% · 差分回退率 78.6%
  状态迁移（前→后）：equal 20→21 · diff 28→40 · host_fail 9→9 · cli_fail 41→28
                     （cli_fail→diff 12 · cli_fail→equal 1（139）· 其余不动；掉出全等的：无）
                     「未定义的名字」30 → 0

python simplexc/tools/check.py
  corpus PASS · host_selftest PASS · lower_equiv PASS（ticks 10/10 · diff_count=0 · blob 26340 B）
  hook_offline PASS · probe_006 PASS   → 5 PASS · 0 FAIL

pi-lens（ast-grep 同规则集）→ simplexc/lower/lower.{cc,hh} 0 条（probe.cc 同命令 4 条，入口有效）
```

## 4. 遗留风险

1. **互调 + 捕获的发散**：两个顶层 def 互相调用、捕获集又不同时，按签名各发一份实例；签名集
   理论上有限但可能很大。现在有硬上限（每个 def 64 个实例，超了报 `Unsupported` → 回退参考产物），
   没有在语料里触发过。要更省得做「强连通分量取捕获并集」。
2. **按名字摊平的语义**：链条上同名遮蔽（不同层各有一个同名局部）时，捕获按**名字**合并 ——
   与参考实现的严格动态作用域可能不同。语料里没观测到（未验）。
3. 12 份新 diff 全落在已知的两类（`TickDiff/段哈希` 25 份、`extern` 15 份），没有新错误类；
   但它们现在是「装得出来但值不对」，属于下一层语义缺口，不是本轮的回归。
4. `未定义的名字` 归零后浮出的下一道墙是 **`编译期类型参数 Size`（17 份）** 与
   **`kernel#!memcpy`（5 份）**。

## 5. 复现

```bash
python simplexc/tools/build.py simplexc_cli
python simplexc/tools/m5/coverage.py
python simplexc/tools/check.py
bash work/tmp/closureabi/cmp.sh        # 改前/改后产物 md5（要先备好 cli_old.exe / cli_new.exe）
```
