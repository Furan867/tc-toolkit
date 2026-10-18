# 目录摸底：`simplexc/corpus/campaign` 的 92 份能不能装出来（只装配、不差分）

生成 2026-10-18 14:18:54 · 已完成 92/92 份 · 跑完

判据口径：每份语料只做第 ①步 `build/simplexc_cli.exe --src <子目录>/test.si --out <blob>`；这个目录没有 manifest、也没有参考产物，所以**不做差分、不生成参考产物**。错误类别按 CLI 的 catch 分支前缀归类，细节里行号/列号/数字已抹掉（同一类缺口合并计数）。注意：没参考产物就分不清「我们缺功能」与「文件本身不是完整程序」—— 92 份里的失败，有一部分可能属于后者，要它自己编一遍才知道。

重跑：`python simplexc/tools/m5/coverage.py --dir simplexc/corpus/campaign --no-diff --out-dir work/tmp/survey --report docs/notes/2026-10-18-campaign-survey.md` · 机器可读：`work/tmp/survey/survey.tsv`

## 汇总

| 指标 | 值 |
| --- | --- |
| 语料总数 | 92（本次已跑 92） |
| 可装配 | 0（0.0%） |
| 装不出来 | 92（100.0%） |
| 其中 cli_fail | 92 |
| CLI 耗时 | 中位 18 ms / 最大 94 ms |
| 总墙上时间 | 1.8 s |
| 超时 | 无 |

## 失败分类

按 `err_kind`：CompileFail 76 · Unsupported 14 · LexError 1 · ParseError 1

逐类（同一个键 = 同一类；细节里的标识符抹成 `<id>`，见 `family_key`）：

| 类别 | 细节 | 数量 | 语料 |
| --- | --- | --- | --- |
| CompileFail | 没有顶层语句块（程序根） | 76 | always_on、and_gate、and_gate_3、and_gate_ctmtest、any_doubles、binary_racer、binary_search、bit_adder、bit_inverter、bit_switch、byte_adder、byte_asr、byte_constant、byte_divide、byte_equal、byte_less_s、byte_less_u、byte_lsr、byte_mod、byte_mux、byte_nand、byte_not、byte_xor、capitalize、conditional_jumps、count_leading_zeroes、counter、counting_signals、decoder_1、decoder_2、decoder_3、dependency、double_buffer、double_number、foundry、full_adder、hex_racer、introduction、logic_gate_racer、main、multiply、nand_gate、negative_numbers、nim、nor_gate、not_gate、odd_number_of_signals、odd_ticks、one_hot_encoding、or_gate、or_gate_3、overture_4_program、overture_5_conditionals、overture_alu_1、overture_alu_2、overture_conditions、overture_decoder、preview_architecture、preview_foundry、ram_component、sandbox、saving_bytes、saving_gracefully、score、second_tick、signed_negator、symphony_11_functions、symphony_9_aliases、symphony_alu、symphony_cmp、symphony_condition_test、symphony_counter_1、symphony_decoder、the_bus、xnor、xor_gate |
| Unsupported | UNSUPPORTED: 顶层 var input 的初始化式不是编译期常量（运行期初始化未做） @ 第 # 行（9 种原文，细节取首见） | 14 | assembly_programming、binary_programming、circumference、mod_4、overture_1_registers、overture_2_alu、overture_3_immediates、overture_6_budget、rng、symphony、symphony_10_stack、symphony_1_nand、symphony_8_io_devices、tower |
| LexError | 第 # 行 第 # 列：第 # 行：不认识的字符 '$' | 1 | sort |
| ParseError | 第 # 行 第 # 列：表达式里出现意外的 这里就到头了 | 1 | maze |

### 每一类的原文错误首行

**CompileFail：没有顶层语句块（程序根）**（76 份，例 always_on）

```
# 原始错误首行：编译失败：没有顶层语句块（程序根）
build/simplexc_cli.exe --src simplexc/corpus/campaign/always_on/test.si --out work/tmp/survey/out/always_on.bin
```

**Unsupported：UNSUPPORTED: 顶层 var input 的初始化式不是编译期常量（运行期初始化未做） @ 第 # 行**（14 份，例 assembly_programming）

```
# 原始错误首行：UNSUPPORTED: 顶层 var input 的初始化式不是编译期常量（运行期初始化未做） @ 第 2 行
build/simplexc_cli.exe --src simplexc/corpus/campaign/assembly_programming/test.si --out work/tmp/survey/out/assembly_programming.bin
```

**LexError：第 # 行 第 # 列：第 # 行：不认识的字符 '$'**（1 份，例 sort）

```
# 原始错误首行：词法错误：第 22 行 第 6 列：第 22 行：不认识的字符 '$'
build/simplexc_cli.exe --src simplexc/corpus/campaign/sort/test.si --out work/tmp/survey/out/sort.bin
```

**ParseError：第 # 行 第 # 列：表达式里出现意外的 这里就到头了**（1 份，例 maze）

```
# 原始错误首行：语法错误：第 1 行 第 1 列：表达式里出现意外的 这里就到头了
build/simplexc_cli.exe --src simplexc/corpus/campaign/maze/test.si --out work/tmp/survey/out/maze.bin
```

## 与 98 份（hook 抓的生成代码）失败分类的对照

这一节是本次摸底的主要产出：两轮跑的是**两类不同的语料**（campaign = 关卡自带的手写测试程序，98 份 = hook 抓的游戏生成代码），比的是它们「卡在哪一类」是否同一批。

98 份那轮（同一口径）：总 98 · 可装配 29 · 逐 tick 全等 10（`work/tmp/m5/coverage.summary.json`）。

### 按 err_kind

| err_kind | 本次目录 | 98 份 |
| --- | --- | --- |
| CompileFail | 76 | 0 |
| HostRc | 0 | 2 |
| LexError | 1 | 0 |
| ParseError | 1 | 66 |
| TickDiff | 0 | 17 |
| Unsupported | 14 | 3 |

### 逐类配对（标识符抹成 `<id>`）

| 缺口（err_kind / err_detail） | 本次目录 | 98 份 | 两边都有？ |
| --- | --- | --- | --- |
| CompileFail / 没有顶层语句块（程序根） | 76 | — | **否（本轮新增）** |
| Unsupported / UNSUPPORTED: 顶层 var input 的初始化式不是编译期常量（运行期初始化未做） @ 第 # 行 | 14 | — | **否（本轮新增）** |
| LexError / 第 # 行 第 # 列：第 # 行：不认识的字符 '$' | 1 | — | **否（本轮新增）** |
| ParseError / 第 # 行 第 # 列：表达式里出现意外的 这里就到头了 | 1 | — | **否（本轮新增）** |

- 本轮有、98 份没有的类：CompileFail/没有顶层语句块（程序根）（76 份）；Unsupported/UNSUPPORTED: 顶层 var input 的初始化式不是编译期常量（运行期初始化未做） @ 第 # 行（14 份）；LexError/第 # 行 第 # 列：第 # 行：不认识的字符 '$'（1 份）；ParseError/第 # 行 第 # 列：表达式里出现意外的 这里就到头了（1 份）
- 98 份有、本轮没有的类：ParseError/第 # 行 第 # 列：模板字面量的 `{…}` 插值语义未定（U-L# 未决）→ 未支持（49 份）；ParseError/第 # 行 第 # 列：表达式里出现意外的 ')'（17 份）；TickDiff/段哈希（17 份）；HostRc/ref rc=3 快照 0 条（试 5 次）（2 份）；Unsupported/UNSUPPORTED: 未定义的函数 ror @ 第 # 行（2 份）；Unsupported/UNSUPPORTED: ui_set_hidden() 的重载解析拿不到参数类型 @ 第 # 行（1 份）
- **结论：两轮没有一类重合**（本轮 4 类 vs 98 份 6 类） —— 两类语料卡住的不是同一类缺口。

## 覆盖表（每份一行）

| id | 装配 | blob B | entry | cli ms | 状态 | 失败类别 | 原始错误首行 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| always_on | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| and_gate | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| and_gate_3 | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| and_gate_ctmtest | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| any_doubles | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| assembly_programming | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var input 的初始化式不是编译期常量（运行期初始化未做） @ 第 2 行 |
| binary_programming | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var input 的初始化式不是编译期常量（运行期初始化未做） @ 第 2 行 |
| binary_racer | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| binary_search | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| bit_adder | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| bit_inverter | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| bit_switch | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_adder | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_asr | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_constant | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_divide | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_equal | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_less_s | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_less_u | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_lsr | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_mod | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_mux | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_nand | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_not | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| byte_xor | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| capitalize | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| circumference | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var size 的初始化式不是编译期常量（运行期初始化未做） @ 第 5 行 |
| conditional_jumps | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| count_leading_zeroes | fail | — | — | 23 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| counter | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| counting_signals | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| decoder_1 | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| decoder_2 | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| decoder_3 | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| dependency | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| double_buffer | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| double_number | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| foundry | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| full_adder | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| hex_racer | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| introduction | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| logic_gate_racer | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| main | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| maze | fail | — | — | 18 | cli_fail | ParseError | 语法错误：第 1 行 第 1 列：表达式里出现意外的 这里就到头了 |
| mod_4 | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var actual_input 的初始化式不是编译期常量（运行期初始化未做） @ 第 2 行 |
| multiply | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| nand_gate | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| negative_numbers | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| nim | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| nor_gate | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| not_gate | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| odd_number_of_signals | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| odd_ticks | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| one_hot_encoding | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| or_gate | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| or_gate_3 | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| overture_1_registers | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var instructions 的初始化式不是编译期常量（运行期初始化未做） @ 第 5 行 |
| overture_2_alu | fail | — | — | 19 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var instructions 的初始化式不是编译期常量（运行期初始化未做） @ 第 9 行 |
| overture_3_immediates | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var instructions 的初始化式不是编译期常量（运行期初始化未做） @ 第 9 行 |
| overture_4_program | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| overture_5_conditionals | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| overture_6_budget | fail | — | — | 94 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var outputs 的初始化式不是编译期常量（运行期初始化未做） @ 第 68 行 |
| overture_alu_1 | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| overture_alu_2 | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| overture_conditions | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| overture_decoder | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| preview_architecture | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| preview_foundry | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| ram_component | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| rng | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var initial_seed 的初始化式不是编译期常量（运行期初始化未做） @ 第 3 行 |
| sandbox | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| saving_bytes | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| saving_gracefully | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| score | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| second_tick | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| signed_negator | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| sort | fail | — | — | 18 | cli_fail | LexError | 词法错误：第 22 行 第 6 列：第 22 行：不认识的字符 '$' |
| symphony | fail | — | — | 23 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var registers 的初始化式不是编译期常量（运行期初始化未做） @ 第 11 行 |
| symphony_10_stack | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var ram 的初始化式不是编译期常量（运行期初始化未做） @ 第 3 行 |
| symphony_11_functions | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| symphony_1_nand | fail | — | — | 19 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var registers 的初始化式不是编译期常量（运行期初始化未做） @ 第 11 行 |
| symphony_8_io_devices | fail | — | — | 18 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var registers 的初始化式不是编译期常量（运行期初始化未做） @ 第 11 行 |
| symphony_9_aliases | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| symphony_alu | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| symphony_cmp | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| symphony_condition_test | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| symphony_counter_1 | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| symphony_decoder | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| the_bus | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| tower | fail | — | — | 19 | cli_fail | Unsupported | UNSUPPORTED: 顶层 var source 的初始化式不是编译期常量（运行期初始化未做） @ 第 3 行 |
| xnor | fail | — | — | 19 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
| xor_gate | fail | — | — | 18 | cli_fail | CompileFail | 编译失败：没有顶层语句块（程序根） |
