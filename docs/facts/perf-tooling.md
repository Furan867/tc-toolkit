## 组件映射

### 映射原理
每个组件代码块前插 `store(唯一地址, 唯一魔数)`，编译后在机器码里搜魔数立即数定锚点，
再坐标变换回无标记版本。标记恒 **17 字节**，变换契约 `pos_无标记 = pos_带标记 − 17×(i+1)`。
验证：静态核对（`tools/build_component_map.py`）
最后验证：2026-10-11

### 组件区之后必须插终点标记
它界定最后一个组件的结束，后果见 `tools/build_component_map.py` 文档头。
验证：静态核对（`tools/build_component_map.py`）
最后验证：2026-10-11

### 每个组件 ID 在源码里出现两次
组件代码 + 绑定各一次，所以「ID 不重复」不能用来判断完整性。游戏源码里每个组件本来就有
**2 条注释**（两段各生成机器码），正确做法是**按 `comp_id` 聚合**而非按注释行。
验证：静态核对（源码注释直方图）
最后验证：2026-10-11

### 完整性判据
唯一可靠的是源码自报的 `get_component_count()`（顶层组件数）与组件注释唯一 ID 数比对。
验证：静态核对
最后验证：2026-10-11

### 建映射不再污染捕获目录
`try_compile_dll.load_dll()` 默认 `prefer_real=True`，优先加载 `compile_real.dll` 而非转发层，
不会往 `work/hook/` 写新捕获；只有显式 `--hook` 时才走转发层。
验证：静态核对（`tools/try_compile_dll.py:39,53`）
最后验证：2026-10-11

## 采样

### RIP 判据
「RIP 是否落在目标机器码的**字节范围内**」（旧判据「是否落在非 IMAGE 可执行区」不够精确）。
实测 8773 个采样点中 8770 个在范围内（99.97%），3 个越界点全是同一个系统 dll 地址。
验证：跑 `python tools/profile_by_image.py`，看落在范围内的比例
最后验证：2026-10-11

### 多副本
同一份机器码在内存里可能同时存在多个副本，**每编译一次占一个新 JIT 区且旧区不释放**。
实测：19 份相同产物 ↔ 19 个不同 JIT 基址的副本（1:1），只有地址最高的最新副本在执行。
验证：跑剖析，数内存里匹配的副本数
最后验证：2026-10-11

### `com_none` 不是真组件
它是空注释；区间归属错误，把注释之后的公共代码算给了它，所以「平均字节」列无意义。
验证：静态核对（现成产物结构吻合）
最后验证：2026-10-11

## 机器码改写

### 四态与两条规则
`rewrite = on / short / short_cse / off`。规则①：`mov ecx,imm32` + `移位 r,cl` →
`移位 r,imm8`（恒缩 4 B）；规则②**仅 `short_cse`**：删紧邻重复装载（`short` 的 CSE 列表恒为空）。
验证：静态核对（`optimize_image.py:676` `cse = find_cse(...) if mode == "short_cse" else []`）
最后验证：2026-10-11

### 填充一律用单条多字节 nop
理由见 `optimize_image.py` 的 `NOPS` 表注释。
验证：静态核对（`optimize_image.py` 的 NOPS 表）
最后验证：2026-10-11

### 收益来自去掉「变量移位」依赖链
不是指令数量本身。旁证：`v_off.bin` 与 `v_onnew.bin` **都是 50403 条指令**却主频不同。
验证：对比两份产物的指令条数与主频（历史实测）
最后验证：2026-10-11

### 模式传递链
`rewrite` 的值只被读一次、只被判定一次；转发层只负责原样搬运（原样写 `hook.log`、
原样经 `--mode` 传），**合法值集合只在 `resolve_mode()` 的 `VALID_MODES` 定义**。
验证：静态核对（`compile_hook.c` + `optimize_image.py`）
最后验证：2026-10-11

### `opt.ini` 规则
不在 git 里；切换靠搬注释符；**若多行都放开：取第一个匹配行生效、不报错**
（读法为何必须与转发层一致、旧实现错在哪，见 `resolve_mode()` 的注释）；
非法值与读不到时都按 `on` 兜底。
验证：静态核对（`optimize_image.py:607-629`）
最后验证：2026-10-11

### 日志的语义与格式
`work/hook/hook.log` 是**唯一**的记录型日志（2026-10-18 从 `work/opt/` 迁来），转发层与改写子进程共写、追加写；
Python 侧取路径走 `tools/hook_log.py` 的 `resolve()`：两文件只存在一个就用它、都在取 mtime 更新的、
都不存在则新路径，且 **`append()` 写到刚刚 `read()` 到的那个文件**（窗口期不分裂 `cfg`/`opt`）；
每行 `时间 \t 编号 \t 事件 \t key=value…`（事件表与键表见 `tools/hook_log.py`，
C 侧实现是 `compile_hook.c` 的 `hook_log()`）。`cfg` 行 = 配置了什么（原样搬运），
`opt` 行 = 实际执行的模式 / 改写处数 / 产物 sha256 —— 两者不一致就是 short 退回了 on。
`impl` 行 = **实际走了哪个实现**（每次编译都写、含 reference），键 `impl`/`mode`/`result`（ours 行可选 `budget_ms`、
首次加 `guard=veh`）；⚠️ `impl` 行的 `mode` 是配的 `compiler`，与 `opt` 行的 `mode`（改写器实际做了什么）**不是一个东西**；
词表唯一出处是 `docs/specs/2026-10-15-simplexc-interfaces.md` §5。
`opt_stderr.log` 是子进程 stdout/stderr 的原样重定向（多行、非结构化、**GBK 编码**，
不是 UTF-8），**不是**记录项，单独放。
验证：静态核对（`tools/hook_log.py` + `compile_hook.c`）+ 游戏内 10 次编译实测：
`hook.log` 32 行全可解析（无坏行）、每次 `cfg`/`opt`/`writeback` 齐全、无 `error` 行、
`opt` 行的 sha256 与 `NNN.opt.bin` 逐条相符、`entry` 与 `NNN.entry.txt` 相符
最后验证：2026-10-15

## 主频读数

### UI 上那个 MHz 的来源
`sim_speed__presenterZutilities_u29531` —— `.bss`，链接地址 `0x14624d618` → 模块内偏移
**`0x624D618`**，float32，单位 Hz。由游戏每秒重算一次，算法 `(cycle 增量)/(时间增量)`；
紧邻 `last_cycle_count`（`0x624D608`，int）与 `last_speed_check`（`0x624D610`，Unix 时间戳 double）。
验证：`python tools/read_freq.py`；旁证 `work/freq_1.txt:4-10`（内存值与 cycle/时间自算逐条一致）
最后验证：2026-10-11

### 采样判据四条
① 暂停/停止 → `sim_speed` 归 0（实测停止后 +1.11 s 起恒为 0）；
② 电路自带限速时停在上限——是**非 0 的极低值**，不能当噪声丢；
③ 启动/停止那一秒读到的是**混合值**（窗口横跨切换点），统计要排除；
④ **停机时 `last_cycle_count` 冻结，但 `last_speed_check` 仍每秒刷新**（逐秒 +1.000~1.006 s）
→ **不能用时间戳判断仿真是否在跑**，要看 `cycle` 是否前进。
验证：`python tools/read_freq.py --watch`；④ 另有直读内存复核
最后验证：2026-10-11

## 静态逆向

### DWARF 节不含 Nim 信息
`compile_real.dll` 的 `.debug_info` / `.debug_line` 是 MinGW CRT 的：`objdump --dwarf=info`
得 25 个 CU、含 `nim` 的 **0 个**、`OOZ` 出现 **0 次**。
验证：`objdump --dwarf=info compile_real.dll | grep -ci nim` 应为 0
最后验证：2026-10-11

### `scan_game_mem.py` 的截断风险
存在真实截断路径（窗口与停止条件见该脚本），历史消费者用「`{` 与 `}` 数量相等」做拦截；
但从未观测到实际不平衡（现存源远小于窗口）。
验证：静态核对（`tools/scan_game_mem.py:205,212,220-221,230,237`）
最后验证：2026-10-11

### 契约重验脚本（Steam 更新后先跑它）
`python simplexc/tools/verify_game_contract.py`：按**符号名**定位（自己解析 PE 的导出表/COFF 符号/`.pdata` 取函数边界，
不复用 `work/symbols/*` 快照），断言 A–E：`compile` 导出 + 全 exe 唯一调用点 + `flags` 置 0 / `jit` 内 memcpy(`ptr+8`) 与尾跳 /
`handle_request_compile_and_run` 内 `out[3]` 的 test+jne 失败分支（跳过 `send_command`/`createThread`）/ `eqdestroy` 的
`testb $0x40,0x7(p)` + `rawDealloc` bit62 门控 / 产物头除 bit62 外不被读。
退出码：无 FAIL=0、有 FAIL=1、跑不起来=2。E 是**否定性命题**，静态判定不了 → 恒 `UNKNOWN`（真判定要动态实验）。
实测当前 buildid 25294874：A–D PASS、E UNKNOWN。
验证：跑该脚本看每条断言的定位方式与反汇编证据；变异自检 `work/tmp/negtest/make_mutants.py`
最后验证：2026-10-17
