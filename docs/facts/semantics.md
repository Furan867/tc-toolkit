## M3 语义档案（实验后定死；没测的别当已定）

> ⚠️ 本文件提到 **Python 原型**（`lower/ir_build.py`、`frontend/decls.py`）的地方都是**历史记录**：
> 该原型已于 2026-10-18 删除（git 历史可查）。部分条目的「验证：」命令就是调它的 → **不能直接跑**；
> 复现时改用 C++ 路径（`build/simplexc_cli.exe`）并把新命令补回条目。在补回之前，这些条目属于
> 「结论在、复现方式待更新」。

### U-L1：`load`/`store` 的 `p` 是**字节地址**，无对齐要求，窄类型写只写该宽度
- `p + 1` = 下一字节（不是下一个元素）；`p + Int k` → lowering 里直接 `add` `k`，**不乘元素宽度**。
- 非 8 对齐的 `U64` 读/写都**静默成功**：精确落在「该地址起的 8 个字节」，**不向下对齐**、**不越写邻居**、不 AV。
- `store(p, U8 v)` 只写 1 字节，邻居保留（不是提升成 8 字节写）。
- **访存宽度（2026-10-18 穷举 `N=1..64`，`U*` 与 `S*` 同规则）**：`N ≤ 32` → `ceil(N/8)` 字节；`N > 32` → 8 字节。
  分段零例外：`1-8→1`、`9-16→2`、`17-24→3`、`25-32→4`、`33-64→8`。
  `N < 8` 是**整字节访问**（预填 `0xFF` 后 `store` 0，读回 `0x00` —— 不是读-改-写），
  **`load` 与 `store` 对称**（`U16` 只读 2 字节、`U33` 读 8 字节再掩码；贴页尾能读出邻页字节）。
  邻居：`N ≤ 32` 保留；**`N > 32` 的写会清掉后 2–3 字节**（`U33` 实测 `55 55 55 55 01 00 00 00`）。
  详见 `dossier/ul_access_width.md` §10（含 64 种宽度穷举与 `load` 实测）。
- → lowering：`load`/`store` 的 `align` 只能声明 **1**，且**不得插入对齐检查/陷阱**（原版编译器没有这个假设）。
- 未测：`Arr[i]`/`Str[i]` 的单位（属 U-L2）、跨页边界的非对齐读写、`load` 的**读指令宽度**（掩码吞掉多读，测不出来）。
验证：`simplexc/dossier/ul1_load_store.md`（5 个片段 × 编译 + 宿主分页转储，片段正文抄在文件里，含三条命令）
      复跑观测：`PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul1/q2_store_unaligned.si --bin work/tmp/ul1/q2_store_unaligned.bin --ticks 1 --entry 0x212F --out <x>.jsonl --dump-pages <d>` → 页内字节 `aa 88 77 66 55 44 33 22 11 aa…`
      宽度分段：`simplexc/dossier/ul_access_width.md`（13 种宽度 × 写后转储，含 `U33` 越写 3 字节的反例）
最后验证：2026-10-18（按 `1..64` 穷举）；`load` 读宽度也对齐同一规则
最后验证：2026-10-17

### U-L7：位宽是**真语义** —— 转换与运算结果都按 `(1<<N)-1` 掩码
- `U8 (250 + 10)` = `0x04`；`U1 (1 + 1)` = `0`；运行期值同样（`U8 (3000)` = `0xB8`、`U5 (33)` = `1`）。
- 截断发生在**运算结果类型**上：`U5 31 + U5 1` = `0`，而 `U64 (U5 31) + U64 1` = `32`。
- 非 2 的幂宽度**全部掩码**，含 33 位：`U33 0xFFFFFFFFF` → `0x1FFFFFFFF`，`2^33` → `0`。
- 提升分两路（**不是一条规则**）：
  · **编译期常量**按**源类型的符号性**：`U64 (S32 -1)`、`S64 (S8 -1)` 符号扩展；`U64 (U32 0xFFFFFFFF)`、
    `S64 (U8 0xFF)` 零扩展。
  · **运行期窄值**（变量/`load`/运行期表达式）一律 `zext(trunc_W(v))`，**与符号性无关**：
    `S32` 变量 −8 → `U64` 得 `0x00000000FFFFFFF8`（不是 `0xFFFF…F8`）。
    排除过「读 8 字节、高 4 字节残留」：把槽预填 `0xFF` 与 `0x00`，整槽内容不同
    （`FFFF…F8` vs `0000…FFF8`）但转换结果**逐位相同**。
- `store(p, v)` 写入宽度：`N ≤ 32` → `ceil(N/8)`；`N > 32` → 8 字节零扩展写（含越写邻居，见 U-L1 与 `dossier/ul_access_width.md`）。
- 窄读掩码：`0xFF` 读成 `U5`→`0x1F`、`U3`→`0x07`、`U7`→`0x7F`、`U1`→`0x01`、`U8`→`0xFF`。
- → lowering：**不要**用 LLVM 原生 `iN`（N∉{8,16,32,64}）承载这些宽度（后端对非标准宽度的假设不可控），
  用「按 64/32 位算 + 显式掩码」，掩码点就两处：**转换**与**运算结果**。
验证：`simplexc/dossier/ul7_promote_truncate.md`（7 片段 × 3 tick，`evidence.txt` 21 行 ok，字节证据在 `bytes.txt`）
最后验证：2026-10-17

### U-L8：`>>` 是「扩到 64 位后逻辑右移再截断」，移位量按 W 取模，除零不 trap
- `x >> n` = `trunc_W( lshr( ext64(x), n & (W-1) ) )`。**永远是 lshr，不是 ashr**；
  `ext64` 的扩展方式与 U-L7 同一条规则：**常量按声明类型的符号性**，**运行期一律 `zext(trunc_W)`**。
  所以常量 `S32 -8 >> 1` = **-4**（sext 到 64 → lshr → 截回 32 得 `0xFFFFFFFC`），而常量
  `S64 -8 >> 1` = `0x7FFF…FFFC`（已在 64 位，lshr 不截断）；运行期窄值右移则先 zext 再 lshr
  （`S32` 变量 −8 右移 ≠ −4，见 U-L7 那条的运行期规则）。
- 移位量按位宽取模：`U32 1 << 32` = 1、`1 << 33` = 2、`1 >> 32` = 1；**不是**「≥ 位宽就 0」。
  `U8 1 << 8` = 1（`8 & 7 = 0`，结果再截断到 U8）。
- 除零**不 trap**：商 0、余数 = 被除数；无符号/有符号/常量/运行期一致，`exited_clean`、3 tick ok。
- `asr(x, n)` ≠ `x >> n`：W<64 且 n<W 时相同；n ≥ W 时 `asr` **饱和成符号填充**而 `>>` 回绕；
  **W=64 时根本不同**（`asr` 算术 −4，`>>` 逻辑 `0x7FFF…FFFC`）。
- 未测：`asr` 在 W=16 / W>64 的饱和；见下一条的 `/`、`%`。
验证：`simplexc/dossier/ul8_shift_div.md`（24 片段 × 3 tick，72 行 ok，逐条命令在文件里）
最后验证：2026-10-17

### `/`、`%`：opcode 由**被除数的声明类型**决定，除数完全不参与
- `S8/S16/S32/S64/Int` 被除数 → `sdiv`/`srem`；`U8/U16/U32/U64` 被除数 → `udiv`/`urem`。
  **除数**的类型、常量性都不参与（两个方向都实测）。改被除数类型会翻（同为位模式 `0xFFFFFFF9`：
  `S32` → −3，`U32` → `9223372036854775804`）。
- ⚠️ 原版编译器自身有**一处不自洽**：由两个**窄类型字面量折叠**出来的常量，被当成「W 位模式零扩展成 64 位」
  再送进**有符号**除法 —— 所以 `(S32 0 - S32 7) / (运行期 2)` = `sdiv(4294967289, 2)` = **2147483644**，
  而 `(S32 -7) / (运行期 2)` = **−3**。同一个值 −7、写法不同、结果不同。
  → 这不是语言语义，是原版编译器的实现细节；要保真就得复刻，**不要「修正」它**。
- 二维表（`/`，顺序 = 常量/常量、运行期/常量、常量/运行期、运行期/运行期）：`−3, −3, 2147483644, −3`；
  `%` 对应 `−1, −1, 1, −1`。
- 两个操作数**都是**常量 → 编译期折叠：用精确值 + 被除数符号性，结果**截断到 W**
  （`S8 0 - S8 7` ÷ `S8 2` 折出 `0xFD`=253）；**运行期**结果**不截断到 W**（`f8` 的 `+280` 是 64 位结果 −4294967289）。
- 宽度：S8/S16/S32 符号性一致；「折叠窄常量零扩展」那个坑三档都在（`S8 0 - S8 7` ÷ 运行期 2 = 124、`S16` = 32764）。
- 未定：零扩展的内部机制；U32 运行期值的 udiv 用 64 位表示（未见截到 W）；运行期×运行期的溢出没测。
验证：`simplexc/dossier/div_sign.md`（8 片段 × 2 遍，`check_claims.py` 断言报告里 100 个数字、0 失败）
最后验证：2026-10-17

### LLVM 目标码必须用 `RelocMode=PIC`（`Static` 与「拒绝内部绝对重定位」互斥）
产物要被整体 `memcpy` 到编译期未知的 JIT 基址执行 → 位置无关。实测 `RelocMode=Static` 下 LLVM 对
「往内部全局写常量」生成 `movq $imm32,[disp32]` 绝对寻址，产物里出现 6 处 `IMAGE_REL_AMD64_ADDR32`
（指向内部全局 `cycle`/`last_command_id`）→ 被装配器按契约 §4-2 拒（`status=3`）；
改 PIC 后 `.text` 只剩 `REL32`（+ `.pdata` 的 `ADDR32NB`，整节丢弃）→ 装配通过。
旁证：clang 对该三元组默认 `-mrelocation-model pic`（所以手写 C 链路从没踩到）。
验证：`docs/specs/2026-10-15-simplexc-interfaces.md` §7 附注；`simplexc/lower/ir_build.py` 的 RelocPIC
      + `docs/notes/2026-10-18-lowering-v0.md`
最后验证：2026-10-17

### M3 首个里程碑：`.si` → 我们的 lowering → 产物，006 与参考产物**逐 tick 全等**
`simplexc/lower/ir_build.py`（AST → LLVM-C → 目标码 → `tcblob`）吃 `simplexc/corpus/src/006.si`：
产物 3508 B、entry `0xA90`；宿主 10 tick 与参考产物（entry `0x212F`）**差异 0 条**
（页哈希 + `sim_cycle` + extern 序列全同）。**整条链没有手写 C 参与**：
`decls` → `expr` → `ir_build` → LLVM `default<O2>` → `tcblob`。
静态：我们的 708 条指令 vs 手写 C 版 508 条（差在 `alloca` + `align 1` + 全 i64），参考产物 3090 条。
验证：`python simplexc/lower/ir_build.py --src simplexc/corpus/src/006.si --out <bin>`，再 `host.py`
      两边各跑 `--ticks 10` 比 `pages`/`sim_cycle`/`extern`（复跑命令见 notes）
最后验证：2026-10-17

### rv32im 的 lowering 缺口只有**两类**根因（250 条里 208 条是连锁）
`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --list-unsupported`
→ 250 条 / 250 行（约占 M3.0 规模 5181 行的 4.8%）：**38 条三目** `cond ? a : b`（前端已解析、
lowering 未做）+ **2 条空 struct**（`type Input Struct { }`，U-L4 布局未定）+ 2 条顶层
`var level_input = Input {}` + **208 条连锁**（`let x = cond ? …` 失败 → 后续名字未定义）。
→ 补掉三目与空 struct，rv32im 就能装配起来；其余构造（`load`/`store`/`asr`/`min`/`max`/`Arr[i]`/
`Str.len`/`memory_copy`/`Ptr` 算术/非 2 的幂掩码）都没再报 unsupported。
验证：`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --list-unsupported`
最后验证：2026-10-17

### M3 达成（宿主面）：rv32im 的 ours vs 参考产物 10 tick 逐 tick 全等
`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --out <bin>` →
**58612 B / entry `0xE1F0`**（参考 219088 B / `0x215B`，**我们 1/3.7**）；宿主 10 tick 差异 **0 条**
（页哈希 + `sim_cycle` + extern 序列）。`--list-unsupported` 从 250 降到 **0**：补了
三目 `cond ? a : b`（`condbr` + 两臂各自求值 + `phi`，保证只求值一支）与空 struct（`packed { i8 }` 占位）。
006 基线**逐字节不变**（3508 B / entry `0xA90` / md5 `9d888a41…`）。
⚠️ **成立边界**（别读成「完全等价」）：① 判据只比「相对 pre-run 基线**变过的页**」，rv32im 每 tick 只变 3 页
（协议 2 + `#SIMULATION_STATE` 第 1 页），`#DATA_*` 的 128 MB RAM 镜像**没进比对**；
② 游戏内真环境（真 Sleep/时钟、游戏设的 target_cycle）未验。
验证：`python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --out <bin>`，再 `host.py`
      两边各跑 `--ticks 10`（ref entry `0x215B` / ours 看自己的 `.entry`）比 pages/sim_cycle/extern
最后验证：2026-10-17

### 宿主里那 272 MB RAM 镜像是**全零** —— 「逐 tick 全等」的强度边界
`#DATA_*` / `#RESET_DATA_*`（rv32im：128 MB×2 + 8 MB×2 等，合计 272.25 MB）在宿主里**从没被灌过内容**：
`regions` 段哈希实测等于「同长度全零缓冲的 sha256」。所以 rv32im 的「逐 tick 全等」只证明
**两边写出的内容一样**，**不证明复现了游戏里的 RAM 镜像**（游戏里那块是 ROM/程序映像，非零）。
→ 要真正演练这块，得给 `#RESET_DATA_*` 灌确定性图案（两边同图案）再跑 —— 宿主行为改动，未做。
验证：`simplexc/tools/host/host.py` 的 `_region_hashes`；`work/tmp/mirror/**`；两份 rv32im 快照的
      `regions` 字段逐段相同（6 段 / 10 tick）
最后验证：2026-10-17

### `memory_copy(src, dst, n)`：第一参数是**源**
我们的 lowering 曾把 `GetParam(0)` 当 dst —— 调用点传的是 `(源, 目的, n)`（源码
`memory_copy(#RESET_DATA_x - 2048, #DATA_x - 2048, N)`），于是 272 MB 镜像被**反向**拷贝并销毁。
这个 bug 在 RAM 全零时**完全看不见**（两边都读零）；灌确定性图案后第一 tick 就炸：
参考侧把图案拷进 `#DATA_*` 且 `#RESET_DATA_*` 完好，我们的两侧全零。
修法：`_body_memory_copy` 里 `GetParam(0)`→src、`GetParam(1)`→dst（`simplexc/lower/ir_build.py`）。
验证：`--seed-reset-data seq` 下 `work/tmp/fixmc/seq_{ref,ours}.jsonl` 的 `regions` 段哈希
最后验证：2026-10-17

### M3 强判据：灌图案后 rv32im 逐 tick 全等（含 272 MB 镜像真的被读写）
`--seed-reset-data seq|xorshift`（两边同图案；灌 `#RESET_DATA_*` 的 4 块 / 136.02 MB）
→ rv32im ours vs 参考产物 10 tick **差异 0 条**，6 段哈希逐段相同且**非零**
（DATA 段 `6165fb5d…` / `3e6dae7f…`；默认不灌时是 `cbcb79a5…` = 全零哈希）。
这条比不灌图案强得多：RAM 镜像被真写、真拷、真读，CPU 行为不再被简化。
验证：`selftest.py --src simplexc/corpus/rv32im/rv32im.si --bin <ours> --entry <e> --ticks 10 --seed-reset-data seq`
      + 我复跑的 `work/tmp/fixmc/{seq,xorshift}_{ref,ours}.jsonl`
最后验证：2026-10-17

### M4 结果（**Python 原型的 -O0 产物**）：游戏内 RV32IM 主频 —— ours **25.8 MHz** vs reference **5.37 MHz**（约 **4.8×**）
⚠️ 这里的 ours 是 Python 原型的 **-O0 机器码**成绩（`ir_build.py` 把 `CodeGenOptLevel` 写成 0）。
数字仍然真实，但**别拿它当产品基线** —— 产品实现的同题成绩见下一条（40.375 MHz）。
**判据入口一条命令可复现**：
`python tools/bench_rewrite.py --variant reference:off --variant ours:off --seconds 40 --json <out>.json`
→ `reference #009 5.369 MHz`、`ours #010 25.835 MHz`、**+381.2%**（各 39 样本；`opt.ini` 自动恢复、游戏不崩）。
手动的两轮（`simctl recompile` → `run` → `watch_freq --seconds 40`）给出同样的比值：
24.843/24.818 vs 5.204/5.169（4.78×）。产物：ours 58612 B / entry `0xE1F0`；reference 219088 B / entry `0x215B`。
reference 的 5.3–5.4 MHz 与设计 §2.5 的「改写前 5.4 MHz」吻合 → 靶子与量法没跑偏。
**中位数对切换过渡点不敏感**（drop 0/1/3/5：reference 5.369/5.372/5.369/5.368；ours 25.835/25.856/25.888/25.900），
而 mean 被第一个过渡点（3.30 MHz）拉低（5.309 vs 5.361）—— 这就是设计 §7 决策 3 选 median 的实测理由。
验证：`work/tmp/m4/ab3.json`（`schema=1`，含 raw samples）；`work/freq/bench_*.tsv`
最后验证：2026-10-18

### M4.5 游戏内验收：产品实现（C++ 前端 + 进程内编译）ours **40.375 MHz** vs reference **5.364 MHz**（**7.53×**）
同一次游戏会话内一条命令复现：`python tools/bench_rewrite.py --variant reference:off --variant ours:off --json <out>.json`
→ `reference #006 5.364 MHz`（7 样本）、`ours #007 40.375 MHz`（7 样本）。`hook.log` 原文：
`007  impl=ours  mode=ours  result=ok  budget_ms=5000  guard=veh` —— 无 error 行、无 `ours_failed`/`fallback`，游戏同 PID 存活。
同一份源码（sha `2461414c…`）两个实现的产物：ours **26404 B** / entry `0x63D0`；reference **219088 B** / entry `0x215B`。
相对 M4 的 -O0 产物（58612 B / 25.835 MHz）：体积小 2.22 倍、主频 +56.3%（7.53× vs 4.81×）。
**这条只证「快」，不证「对」** —— 正确性由宿主面的逐 tick 等价承担（`check.py` 的 lower_equiv 步）。
验证：`work/freq/m45_shell.json`、`work/freq/bench_20261018_133133.tsv`、`docs/notes/2026-10-18-m45-game-bench.md`
最后验证：2026-10-18

### campaign 的 `test.si` 是**测试片段**，不是完整编译单元
游戏自带的 `campaign/<关卡>/test.si` 只含 `const` + `def get_input` / `def check_output` + 顶层初始化代码，
**没有程序根**：`grep -lE "^label " simplexc/corpus/campaign/*/test.si | wc -l` = **0**；
而 hook 抓的生成源码 **98/98** 都含 `run_sim: while true { … }`。直接喂我们的 CLI 必然报「没有顶层语句块（程序根）」。
所以「campaign 92 份全装不出来」首先说明的是**语料形状**，不能读成 92 个编译缺口。
**游戏自己会把它们拼进完整程序**再交给 `compile.dll`：`def get_input` / `def check_output`（正是 `test.si` 的内容）
在 98 份捕获里 **98/98** 命中，`#CORRECT_OUTPUT` 命中 13 份且**值对得上**（`[U1 1,1]`、`[U1 1,0,0,0]` …）——
拿 `test.si` 当编译单元属于输入口径错误。
验证：`docs/notes/2026-10-18-campaign-survey.md`；`python simplexc/tools/m5/coverage.py --dir simplexc/corpus/campaign --no-diff`
最后验证：2026-10-18

### stdlib 前缀单独编不了：它没有程序根
`simplexc/corpus/stdlib_prefix.si` 里**没有顶层语句块**（没有 label），所以单独喂 CLI 必然报「没有顶层语句块（程序根）」——
它的正确输入形态是「**前缀 + 用户源码**整体」（原版编译器每次 `compile()` 就是这么拼的）。
它是前端的**两大块**（按单元算 121 个顶层单元：82 def + 39 个算子单元），其中：`$` 可变参数 23 处 / 13 单元 ·
`asm` 块 5 单元 · `kernel32.'Name'(…)` 外部调用 8 处 · `[T] expr` 方括号转换 5 单元 ·
7 个内建/宏缺失（`array` 38 处，其余六个同样零命中）· 16 个单元签名带 `@Type`/`@Any`/`@Size`。
验证：`docs/notes/2026-10-18-stdlib-prefix-survey.md`；`./build/simplexc_cli.exe --src simplexc/corpus/stdlib_prefix.si --out <blob>`
→ 先死在更早的一层（词法 `$`，第 172 行）
最后验证：2026-10-18

### U-L14：`#if in_scope(name)` 的可见性规则（2026-10-18 定案）
**真**：顶层 `def`（前/后）· **程序根语句树（`ast_.label` 体）里的嵌套 `def`**（前/后、引用点在别的 def 里也算）·
顶层 `var` / `const #X` / `type` / enum 成员名 · 自己的参数与同体局部 · **stdlib 前缀里的名字**。`#if` 与裸 `if` 同规则。
**假**：某个 `def` **体内**的嵌套 `def` · 别的函数的参数 · label 名 · 内建（`store`/`raw_cast`）·
`var` 声明在引用点**之后**（`def`/`type`/`const #` 后置仍真）· 无 `#` 的顶层 `const` · 未定义名。
名字表取自**未经 `in_scope` 裁剪的原 AST**（两遍，但不必不动点）。
同一条规则的另一面：**程序根体里的嵌套 `def` 像顶层 def 一样可被前向调用**（058 的调用点在第 269 行、声明在 350 行；
参考产物的存在就是证据）—— 把嵌套 def 只绑在它的语句位置上会让这些调用报「未定义的函数」。
验证：`docs/notes/2026-10-18-ul14-in-scope.md`（45 条片段 × 参考产物字节 + `objdump` 复核；
另用 `nest.py` 套回 98 份：95 份翻转 / 190 条 (文件,名字)，40 份 TickDiff 全在名单里）
最后验证：2026-10-18

### U-L4（部分）：复合字面量 `T { a: v, … }` 的字段初始化规则
**已验**（2026-10-18）：未列字段默认 **0** · 字段按**名字**匹配（与书写顺序无关）· 求值顺序 = **源码书写顺序** ·
字段值可以用**运行期表达式**（`load` / 函数调用）。
**未验**：字段偏移 / padding / 嵌套布局 —— 三次尝试都**不可观测**（只写不读的 internal 全局被参考 DCE；
全局初始化数据不在变化页；反汇编里被常量折叠 + SROA 拆栈槽）。现有 `packed`（`ceil(bits/8)`、声明序）**这轮没改也没验**。
反向的也可观测：参考**支持**顶层 `var x = T { a: U8 (f()) }` 这种运行期初始化（我们目前报「不是编译期常量」）。
验证：`docs/notes/2026-10-18-ul4-layout.md`；器材 `work/tmp/ul4/e2.py`（探针 + 参考编译 + 宿主观测 `set_text` 字节）
最后验证：2026-10-18

### 嵌套 def 的自由变量按「**调用点**」解析，不是按 def 的文本位置
- `def` 写在 `var x` **之前**、而调用点在 `var x` **之后** → 参考**编得过**（所以不是在 def 文本位置解析）；
- 调用点在 `var x` **之前**（同一块）→ 参考**拒绝**：`Variable x not in scope. (1516)`（所以解析在调用点、顺序敏感）；
- 调用点自己查不到时，链条**往外走到最外层调用点**（程序根语句树）才命中。
⇒ 程序根体里的 `var` **不是全局**（若是全局，第二条不该报错）。我们据此改成「在调用点摊平捕获」（`ebc97c2`）。
验证：`docs/notes/2026-10-18-closure-var.md`；片段与命令 `work/tmp/in_scope/rootvar_{E_def_then_var,K_guard_before}.si` + `work/tmp/ul14/refcc.py`
最后验证：2026-10-18

### U-L2：数组下标**不做任何检查**（2026-10-18 实测）
`a[i]` = `载荷 + i × 步长`，无检查、无夹取：`a[4]`（长度 4）读 = `load(p+4)`、`a[4]=99` 静默写到载荷外第 4 字节且不动已有元素、
`a[-1]` 读长度头的最高字节；`i = 1<<20` 写 → 宿主 VEH `0xC0000005`，故障地址 = 载荷 + 1 MB（**没有 trap 指令，只有硬件 AV**）。
编译期也不查（`a[4]` 能编过）。
验证：`docs/notes/2026-10-18-semantics-batch.md`（器材 `work/tmp/sem/run.py`）
最后验证：2026-10-18

### U-L5：enum 底层宽度**只有两档**（2026-10-18 实测）
≤256 个成员 → 1 字节（`size` 报 8 位、数组步长 1）；257…65535 → 2 字节（`size` 16 位、步长 2）。
**4/8 字节档不可达**：65536 个成员时参考自己断言崩溃（`enum_names.len < 1 shl 16`）。
取值 = 成员序号（从 0 起），转 `Int`/`U64` 是零扩展。
验证：`docs/notes/2026-10-18-semantics-batch.md`
最后验证：2026-10-18

### U-L10：字符串字面量 = 静态数据 + `len<<16` 头，**无 NUL 终止**
头是 `len << 16`（低 16 位 0；静态数据、不在堆上）；载荷是源码里的原始字节（UTF-8 直通）；
`p + len` 处不是 0（读到相邻数据）⇒ **不靠 NUL 终止**。
转义表有坑：`\0 \a \f \v \e \n \r` 标准，但 **`\t` → 0x08**（不是 0x09，参考自己的表如此）、
`\b` 与 `\x42` **不认**（原样保留反斜杠）、`\"` 保留反斜杠且不结束字面量、**`\\` 解析失败**。
`Str` 与 `Arr[Char]` 是**互不兼容的具名类型**（两个方向传参都被拒）。
验证：`docs/notes/2026-10-18-semantics-batch.md`
最后验证：2026-10-18

### U-L12：`min`/`max` 的结果类型 = **第一个实参的类型**（无提升）
`min(U8 200, U16 300)` = 200 且 `size` 8 位；`min(U16 300, U8 200)` = 200 且 16 位。
第二个实参按**它自己**的宽度与符号扩展（`min(3, U8 200)` = −56、`min(3, U16 60000)` = −5536）；混合宽度/符号都能编过（静默重解释）。
验证：`docs/notes/2026-10-18-semantics-batch.md`
最后验证：2026-10-18

### `$` 参数：引用（别名）语义，且调用点**强制**写 `$`
- 调用点漏 `$` 报错：`1st will get modified, so put '$' in front when calling it.`；
- 体内读它拿到调用者的当前值、赋值后立刻读到新值 ⇒ **引用/别名语义**（不是值拷贝 + 写回）；
- **写回时机仍未验**（全被内联 + SROA 成寄存器，观测不到）；
- `$`→`$` 转发规则不一致：标量 `inner8($s)` / `$s.inner8()` 被拒（后者是**无行号的裸 `COMPILER ERROR`**），
  但前缀自己的 `sort($arr) { quick_sort($arr) }`（`Arr[@Any]`）能过。
验证：`docs/notes/2026-10-18-routea-semantics.md` 与 `…-semantics-batch.md`
最后验证：2026-10-18

### 泛型 `@Type`/`@Size`/`@Any`：按实参具体类型实例化，`size()` 折成编译期常数（**单位 = 位**）
`gs(load(<U8>))` → 立即数 8111、`gs(load(<Int>))` → 64111（同名的两个实例化）。`@Type` 既能收 `<U8>` 也能收值（推型）。
**嵌套泛型 `<@Type>` 不透明传**：`ginner(<@Type>)` 里 `size()` 塔成 **-1**。
`@Size` 当**返回类型**时 = 该实例的绑定类型（实测 `gtr(<U8>/<U16>/<Int>)` → 参考产物立即数 `0x89` / `0x6789` / `0x123456789`）。
「同一 (函数, 实参) 只实例化一次」**未验**（参考全内联，看不到共享体）。
验证：`docs/notes/2026-10-18-routea-semantics.md`
最后验证：2026-10-18

### `asm` 块**不是原样内联**：参考会把块重新汇编
操作数名换成寄存器/立即数，周围插保存恢复（`mov rcx,32` → `mov rdx,rcx; mov ecx,0x20`），`$result` 解析到该变量所在寄存器；
前缀 `memory_copy` 里内联出 `rep movs BYTE PTR es:[rdi],…`，`mov al,0` 被规范成 `mov eax,0x0`。
⇒ 要支持它得写一个「认识名字操作数」的小汇编器。
验证：`docs/notes/2026-10-18-routea-semantics.md`
最后验证：2026-10-18

### 顶层算子单元接管运算符（`binary + (Int,Int)` 之类）
`binary + (Int,Int)` **接管**了 `Int` 上的 `+`（产物里没有 add，只有单元的 4242 存储 + 存 return 0）；
`dot foo` 按接收者静态类型解析；但 **`index_read` 没有接管 `Arr[Int]` 的 `a[0]`**（走内建 `[rcx+rax*8]`，单元没被调）。
⇒ 要支持它得建算子单元表 + 在调用点做重载解析。
验证：`docs/notes/2026-10-18-routea-semantics.md`
最后验证：2026-10-18

### 窄位宽 / 移位语义：真条件测清了一部分，但**对 98 份没有可测收益**（这条线收口）
参考实测（`docs/notes/2026-10-18-width-cond.md` · `…-width-180.md`）：
- 结果掩码按**左操作数（窄侧）的声明位宽**（`x1<<x4b(量1)` = 0、`x2<<x4b(量1)` = 2，`unify` 宽侧会给 2/6）；
- 运行期**移位量按存储宽度取模**（`x7<<x7b(量8)` = 0x7F、`x9<<x9b(量16)` = 0x1FF）；
- 旧代码用 `位宽-1` ⇒ 与参考在**非 8 倍数位宽**上不同（HEAD 的 `>>` probe：`x7>>8` 参考 0x7F、我们 0）。
**但对 98 份无收益**：把掩码改对后 176/177/180 的 IR **逐行相同**（那 6 处是 `U32<<U32`，W=32 时两种宽度恰好等价）⇒ 没有份因此改善；
「结果取左操作数」与 W≤2 的掩码（`mask_of(W)`，与存储宽度相冲）**没有任何语料验证**。
⚠️ 中间那一轮「只换掩码就让 180 掉 ⇒ 还有别的形状」的结论**作废**：那个 patch 多套了一层 `mask_of`，W=32 时把 `31` 变成 `0x7FFFFFFF`——
180 是被**坏 helper** 弄掉的，两个 patch 共用它才得到「哈希一字不差」。
验证：`docs/notes/2026-10-18-width-cond.md` · `docs/notes/2026-10-18-width-180.md`（patch 在 `work/tmp/width180/`）
最后验证：2026-10-18

### 参考的 def 是**非重入**的：同一个 def 的多次调用共用同一块结果缓冲
`rep(1,1) + rep(2,8) + rep(3,3)` 这样一个字符串里，三段插值**全部渲染成最后一次调用的结果**（`[F][T][T]`）；
`rep(2,8) + rep(1,1)` 连**长度**也来自最后一次（`[[T]|[T]]`）。
但不是「实参被覆盖」：两个**不同** def 不串（`[Ab|Cd]`），返回字面量/`Int` 的调用不串（`idn(5)+idn(9)` → `[5|9]`）；
也不在模板里：`let a=rep(1,1); let b=rep(2,8); set_text(a,…)` 里 a 读出 24B 的 `rep(2,8)`。
我们照此模拟（`template_expr` 把同名 Str 段换成最后一段的 SSA 指针，不重复调用；`5d4ff13`）—— 8 份语料因此归零。
验证：`docs/notes/2026-10-18-uibuffer.md`（19 例探针 `work/tmp/uibuf/probe_ui.py`）
最后验证：2026-10-18

### LLVM 的动态探栈调用（`___chkstk_ms`）**插在寄存器分配之后**，跨 call 的活值得由它自己保
动态 `alloca` 触发的 `___chkstk_ms` 是 LLVM 在**寄存器分配之后**插进函数中段的 → 它**插不出 spill** →
跨 call 的活值只能靠探栈函数自己保存。libgcc 形状的走查拿 `%r10` 当走查指针，于是会踩坏 LLVM 以为「跨调用有效」的值。
实测后果：9 份语料进了 `__simplexc_copy` 的 32 字节块拷贝循环（前有 `cmp $0x200000,%r10`，`%r10` 里是壳上次探栈残留的值 → 比较恒真；
循环计数 `真长度 & -32 == 0` 成 do-while 死循环）→ 32 字节 store 一路往上写穿线程栈顶那一页 → 宿主 AV。
**壳的 `___chkstk_ms` 因此改用已压栈保存的 `%rcx` 走查**（`002ca34`），函数现在除 flags 外不破坏任何寄存器。
旁支：LLVM 自己出的 COFF 里那块拷贝循环的掩码是 `0x0000FFFFFFFFFFE0`（高 16 位丢了；对 < 4 GiB 的尺寸与正确掩码等价，产物上限 64 MiB，碰不到）。
验证：`docs/notes/2026-10-18-host-av.md`；`python simplexc/tools/m5/coverage.py`（host_fail 9 → 0、equal 一份没掉）
最后验证：2026-10-18

### `array(<T>, n)` 的表示（2026-10-18 实测）
- 值 = **载荷指针**；载荷前 8 字节是 Nim 头形状，`头 >> 16 == n`（实测 `p-8 = 0x40005` → n=4）；
- 元素宽度 = U-L1 的字节步长（`<U8>` 1 字节 · `<Int>` 8 字节）；
- 载荷**零初始化**；`.len()` = `头 >> 16`；`size(arr)` = 64 bit（= `storage_bytes(Array)`）；
  **数组字面量同样要带这 8 字节头**（漏了会让 `.len()` 变成一条**越界的常量 load** → O2 里成 undef →
  依赖它的分支会被**整条折掉**；实测 9 份语料因此 `check_output` 退化成恒定 `ret 2`，详见 `…-test-result-chain.md`）；
- 参考在**堆**上每次新分配（首址固定 `0x4002540b…`）；我们实现成**每调用点一枚私有全局**（实测够用）。
- **未验**：越界（参考侧直接拒绝编译）、头的低 16 位（消费者只读 `>>16`；观察值 4→5 / 3→7）。
验证：`docs/notes/2026-10-18-array.md`（探针 `layout`/`zero`/`int8` + 两个片段与参考逐字节对照）
最后验证：2026-10-18

### `clz` 的范围 = 操作数的**声明位宽**（2026-10-18 实测）
`clz(U8 0) = 8` · `clz(U16 0) = 16` · `clz(U4 0) = 4` · `clz(Int 0) = clz(U64 0) = 64` · `clz(S8 -1) = 0`；
常量与运行期一致。
验证：`docs/notes/2026-10-18-small-classes.md`；探针 `work/tmp/small/clz_probe.py`
最后验证：2026-10-18

### `random` 的语义（实测；它定义在前缀里，但 98 份里 **32 份**在用）
- 递推是**顺序赋值**，不是展平式：`x ^= x<<12; x ^= x>>25; x ^= x>>27; x *= 0x2545F4914F6CDD1`（均按 u64 回绕）；
  常数是 **15 位**的 `0x2545F4914F6CDD1`（不是教科书那个 `…CDD1D`）。
  ⚠️ 展平式 `(x ^ (x<<12) ^ (x>>25) ^ (x>>27)) * C` **从第二次调用起就与参考不同** ——
  而**第一次调用两边相同**，所以写成展平式会「看起来像对」（实测：8 次连续调用逐位比，顺序式 8/8 相同，展平式从 state_2 起不同）。
- 初值 `state = 1`；`random()` 返回**新 state 的全 64 位**；`random(n) = (U64)state % (U64)n` → `[0, n)`；`n==1` → 0；
  **`n==0` 返回被除数、不 trap**；`n<0` 按 U64 重解释（后两条首轮测出，尚未重测）；
- 种子是**程序自有全局**（前缀的 `global_seed`，初值 1，与语料里 `global_seed = Seed(get_command(ctl_test)+1)` 是同一份），跳 tick 延续不重播种；
- **可以单点实现**（不需要 arena / `array` / `size` / `@Type`），已实现（`4c83440`），端到端 8 个槽与参考逐位相同。
验证：`docs/notes/2026-10-18-random.md`（探针 `work/tmp/randomtask/probe_seq.py`）+ `docs/notes/2026-10-18-stdlib-semantics.md`
最后验证：2026-10-18

### 我们的 lowering 产物在游戏内执行成功（LLVM 前端产物第一次真跑）
`manifest.tsv` seq 007 / 009：`impl=ours`、`out_len=58612`(0xE4F4)、`entry=0xE1F0`；
`hook.log`：`impl=ours mode=ours result=ok budget_ms=5000 guard=veh`，无 error 行；
游戏不崩、撤销历史可用、`sim_read` 能读到 RV32IM 的真实主频。
此前游戏内只跑过**手写 C** 的产物（M1/M1.5），这次是 `decls → expr → ir_build → LLVM → tcblob` 全链产物。
验证：`awk -F'	' '$1=="007"||$1=="009"' work/hook/manifest.tsv`；`grep -c error work/hook/hook.log` = 0
最后验证：2026-10-18

### M3 的靶子：RV32IM 电路的捕获已固定为仓内资产
`simplexc/corpus/rv32im/`：`rv32im.si`（10369 行 / 360772 B，sha256 `dba14f86…8d220e`）+
`rv32im.bin`（参考产物 219088 B，sha256 `baffee9f…60ca01`，`entry=0x215B`）+ 捕获时整份 manifest。
规模：`get_component_count() = 532`、`mode_refresh()` **6252 行**、43 个 def 名 / 89 条顶层声明；
**可达闭包 = 10 个名字 / 5090 代码行**（从 `run_sim` 顶层块起按调用图名字匹配，`decls.py` 的 strict 口径），
加顶层骨架 91 行 = **M3.0 规模 5181 行**。
⚠️ 口径坑：`work/tmp/dossier/reachability.py` 给出的「19 个可达」把 `def name(` **声明头**也当调用
（8 处单行/空体 def + 嵌套 `handle_test_result` 头），多算 9 个；以 `decls.py` 的 10 个为准。
`com_custom`(33) 与 `com_probe_wire_bit`(48) 本体是**空块、0 行代码**（代码已在 `mode_refresh` 里平铺），
所以「子元件层级」不构成额外 lowering 工作量。参考产物 ≈46600 条指令，与设计 §2.5 的 46591 条吻合。
为什么必须存仓里：`work/hook/` 每次游戏内编译都覆盖同名文件（实测 17:12 那批被 17:21 整批替换），
而且产物里的游戏内存地址按进程实例变 —— 判据不能依赖会被冲掉的路径或活着的进程。
验证：`sha256sum simplexc/corpus/rv32im/rv32im.{si,bin}`；`grep -c '' simplexc/corpus/rv32im/rv32im.si`；
      可达闭包：`python simplexc/frontend/decls.py --src simplexc/corpus/rv32im/rv32im.si`（看 stderr 的 strict 口径）
最后验证：2026-10-17

### 宿主该映射多大，由 `memory_copy`/`memory_clear` 的**实参**决定（不是 `c_alloc`）
参考产物 `+0x2391` 是 `rep movs`：`rsi = #RESET_DATA_7069020504170261741 - 0x800`、
`rdi = #DATA_7069020504170261741 - 0x800`、`ecx = 0x8001000`。源码里对应
`memory_copy(…, 134221824)` —— **134221824 = 128 MB + 4096**。
⚠️ 行尾注释里的 `c_alloc(134217728)` 只是数据大小，**照它映射会少 4096 字节**：
AV 地址 `0x18928150000` 正好落在「按 128 MB 算出的映射末尾」（实测，差一个页就崩）。
→ `simspec` 解析 `memory_copy/memory_clear` 实参算绝对区间，`host.merge_spans` 按它页对齐并合并；
RV32IM 合计 **278.5 MB / 6 段**（1 号组 128 MB×2 + 3 号组 8 MB×2 + 小的）。
验证：`PYTHONIOENCODING=utf-8 python simplexc/tools/host/selftest.py --src simplexc/corpus/rv32im/rv32im.si --bin simplexc/corpus/rv32im/rv32im.bin --entry 0x215B --ticks 10`
      → `tick 10/10 · 逐 tick 全等=是 · 仿真真在工作=是`（修复前是 AV，tick 0/0）
最后验证：2026-10-17

### 生成器给块地址的行尾都带注释，`Ptr` 解析必须容忍
`const #DATA_* / #RESET_DATA_* / #SETTINGS_*` 行尾是 `// """ & $(c_alloc(…) + 2048) & … """`。
`simspec.py` 的 `PTR_RE` 原来以 `\s*$` 收尾 → 这 12 个地址整批漏掉（地址表只有 9 个标量），
`reset_sim` 一跑就 AV。修成 `\s*(?://.*)?$` 后标量地址 9 → 21 个。
验证：`python -c "import sys;sys.path.insert(0,'simplexc/tools/host');import simspec;print(len(simspec.SimSpec.parse('simplexc/corpus/rv32im/rv32im.si').scalars))"` → 21
最后验证：2026-10-17

### simplexc/corpus：98 份唯一源码 + 含 entry 的 manifest
`work/hook/all/` 的 180 份捕获按源码字节去重 → 98 份（重复分布 1×91、2×3、3×2、4×1、73×1，
最多的是 004 出现 73 次）；离线重编译后 `entry` 全部非 0（53 个不同值，`0x212f` 出现 15 次）
且都 < `out_len`；98 份源码只产出 **97 个不同产物**（`020` 与 `058` 只差一个空行的位置）。
`corpus/manifest.tsv` 13 列：`id file src_len src_sha256 dups seen flags entry out_len out_sha256
ref_match ref_match_rebased rebased_delta`；`corpus/ref/*.bin` **不进 git**（能由源码重新生成）。
验证：`python simplexc/tools/recompile_corpus.py --check`；独立复算（不 import 该工具）行数/列数/
      `entry` 边界/`out_len` 与文件大小/`out_sha256` 与文件内容
最后验证：2026-10-16

### stdlib 前缀：27711 B 含 55 处 CRLF，归一化后 27656 B
`compile_real.dll` 偏移 `0x119F48` 起 27711 字节；CRLF→LF 归一化后 27656 B（1503 行）。
两者**等价** —— 不要去「修」这个差。
验证：`cmp simplexc/corpus/stdlib_prefix.si <工作副本>` → 相同（工作副本从 DLL 段重新提取后归一化）
最后验证：2026-10-16
