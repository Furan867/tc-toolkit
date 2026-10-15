# 事实

被反复引用的结论、契约、机制、判据。**就地改写，不留旧值**（旧值看 git 历史）。
每条必须带「验证：」与「最后验证：」。验证方式写「静态核对」的，表示读代码/反汇编对过；
写命令的，表示那条命令跑通。

## compile.dll 转发层

### compile() 的调用契约
签名 `Result* compile(Result* out /*rcx*/, NimStr* src /*rdx*/, int flags /*r8d*/)`；
40 字节输出 = `{out_len, out_ptr, entry_off, 0, 0}`：`out_len` 是缓冲总长（含 8 字节头）、
`out_ptr` 指向缓冲、`entry_off` 是产物内锚点偏移，`out_len == 0` 即编译失败；`flags` 含义未知。
qword[3]/[4] 实测被写成 0（不是残留，判定依据见 `manifest_line` 注释）。
验证：`objdump -d compile_real.dll`（compile @ `0x2fddd5fd0`）+ `work/hook/manifest.tsv` 实测
最后验证：2026-10-13

### compile() 的内部流程
`rawNewString(len + 27711)` → 拷 27711 字节固定前缀 → 拷用户源码 → `compile_source__OOZcore_u145`。
验证：静态核对（`objdump -s` 27711 字节前缀）
最后验证：2026-10-11

### compile() 可脱离游戏进程调用，产物逐字节可复现
两个独立进程编译同一份源码，产物 md5 相同；且与「游戏内经转发层捕获」的产物三者相同：
`a18_run1.bin` = `a18_run2.bin` = `hook_out_002.bin` = `002.raw.bin`
（md5 `12cb7184…`，204623 B = 8 字节头 + 204615 机器码）。
验证：`python tools/try_compile_dll.py --compile --src <样本>` 跑两次比 md5
最后验证：2026-10-11

### 游戏内 JIT 代码与离线产物逐字节相同
故映射可直接用于采样地址。19 份产物 md5 全等，`locate()` 对 19 个内存候选做整段比对全 pass。
验证：跑 `tools/profile_by_image.py`，看 locate 的整段校验结果
最后验证：2026-10-11

### 产物文件前 8 字节是长度头
不是机器码；头字段的值**等于文件总长（含头自己）**，不是数据长度。
验证：`stat -c %s` 与 `od -An -tu8 -N8` 比对任一 `work/hook/hook_out_*.bin`
最后验证：2026-10-11

### 转发层与真编译器
部署期间游戏目录的 `compile.dll` **是转发层本身**，与 `build/compile_hook.dll` 逐字节同源；
真编译器是 `compile_real.dll`。查静态信息（符号 / 导入表 / 反汇编）一律认后者。
验证：`md5sum build/compile_hook.dll "<游戏目录>/compile.dll"` 应相同
最后验证：2026-10-11

### 编号机制
编号计数器是 DLL 全局变量，**每次加载（游戏启动）从 1 递增**；「同号即一对」——
一次 `compile()` 取一个号，`hook_src_005.si` 与 `hook_out_005.bin` 是同一次调用。
**跨会话判断新旧要看 mtime**，不能只按编号取最大。
验证：静态核对（`compile_hook.c`）
最后验证：2026-10-11

### 清空语义
`work/hook/` 与 `work/opt/` 同为转发层工作区、编号同出 `g_seq`，**每次加载一起清空**
（各清哪些扩展名见 `compile_hook.c` 的 `purge_captures` / `purge_workdir`）；
`work/freq/` 是采样数据，跨会话保留。
验证：静态核对（`compile_hook.c`）+ 实测（连续两次加载，`manifest.tsv` 都从 001 重新开始）
最后验证：2026-10-13

### 产物 rewrite 模式的权威来源
只认 `optimize.log`（为何不能读 `opt.ini`，见 `watch_freq.py` 的 `opt_info`）。
验证：静态核对（`watch_freq.py`）+ 历史铁证（`hook_out_005`/`006` 产物 sha 相同却标了不同模式）
最后验证：2026-10-11

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
（读法为何必须与转发层一致、旧实现错在哪，见 `rewrite_mode()` 的注释）；
非法值与读不到时都按 `on` 兜底。
验证：静态核对（`optimize_image.py:607-629`）
最后验证：2026-10-11

### 三份日志的语义
`hook.log` = 配置了什么；`optimize.log` = 实际执行的模式 / 改写处数 / 产物 sha256；
`opt_stderr.log` = 自检明细，**追加写**（`compile_hook.c` 用 `FILE_APPEND_DATA`），历史原因都在。
`hook.log` 与 `optimize.log` 不一致就是 short 退回了 on。
验证：静态核对（`compile_hook.c:210`）
最后验证：2026-10-11

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

## simctl

### 三条设计原则
① 不做自动检测（不监听 `opt.ini` 的 mtime）；② 不判断编译是否成功；③ 每次调用只发一个键。
验证：静态核对（`simctl.py:20-21`）
最后验证：2026-10-11

### 三个动作的机制
`run()` **注入 F6**（运行/暂停**切换**，非幂等）；`stop()` **注入 F4**（停止/复位）；
`recompile()` 注入 Ctrl+Z / Ctrl+Shift+Z。
验证：静态核对（`simctl.py:401-411`）
最后验证：2026-10-11

### 抢前台
有效手段 = 先 `SendInput` 发一次 Alt 解锁，再 `AttachThreadInput` 拼接输入队列。
**例外**：调用方是当前焦点进程或它的子进程时不需要 Alt（此时裸调用也成功）；
处于其他进程则必须走 Alt 路径。
验证：对照实测——前台是无关进程时裸调用返回 FALSE、前台不变；前台是本进程所在终端时返回 1
最后验证：2026-10-11

### 抢前台的前置校验与拦截
发键前必须校验前台就是游戏窗口，否则拒绝；有可见 `#32770` 错误框时一律不操作。
主窗口识别 = 「可见 + 有标题 + 非 `#32770` + 无 owner」。
验证：静态核对（`simctl.py:239-241, 351-354, 390-393`）
最后验证：2026-10-11

### recompile 的判据与记账
判据是 `next_sim_version`（`.data 0x14049c300`）**每次编译请求 +3**（游戏内部一次撤销固定发
3 次请求；实测变化序列 44→46→47 是分次 +1），纯读内存。撤销是消耗性操作，记**净消耗步数**，
退出时按净消耗补发等量反向键。
验证：读内存 → 发一次 recompile → 再读，差值应为 +3
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
