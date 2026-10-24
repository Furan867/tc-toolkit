# U-L7：提升 / 截断语义（原版编译器实测，已定）

**结论**：位宽是**真语义**，不是注解。

1. 转换与运算结果按声明宽度**掩码** `(1<<N)-1` —— `U8 (250+10)=0x04`、`U1 (1+1)=0`、`U5 (33)=1`、`U33 0xFFFFFFFFF=0x1FFFFFFFF`。
2. 截断发生在**运算结果类型**上，不是只在赋值/store 处 —— `U5 31 + U5 1 = 0`，而 `U64 (U5 31) + U64 1 = 32`。
3. 提升方向跟**源类型的符号性**走 —— 无符号源零扩展（`U64 (U32 0xFFFFFFFF)=0xFFFFFFFF`），有符号源符号扩展（`U64 (S32 -1)`=全 FF，`S64 (S8 -1)`=全 FF，`U64 (S8 -1)`=全 FF）。
4. `store` 的写宽度 = **值类型的字节宽度** —— `store(p, U32 …)` 只写 4 字节（高 4 字节保持原值）。
5. 窄读 `load(<UN>, p)` **掩码**到 N 位 —— 字节 `0xFF` 读成 `U5`→`0x1F`、`U3`→`0x07`、`U7`→`0x7F`、`U1`→`0x01`、`U8`→`0xFF`。

## 实验基座（所有片段共用）

- 基座源码 `work/tmp/ul7/base.si` = `work/tmp/sample.si`（004 家族里 2 元件的那份捕获，`#SIMULATION_STATE = Ptr 2416623026176 = 0x232A9F50000`）。
  未改一行时 3 tick 全绿，`+256/+257 = 01 01` —— 观测通路活着。
- 观测点：把表达式结果写进 `#SIMULATION_STATE + 264 … + 368`（基座自己只用 256/257）。
  `#SIMULATION_STATE` 正好落在页基址上 → **页内偏移 = +k**。
- 观测面只有 `--dump-pages` 的页字节：`work/tmp/ul7/<frag>.pages/t0003_0232a9f50000.bin`（tick 3，页 64 KB）。
- 编译产物里的 `entry = 0x212F`（`try_compile_dll.py` 输出结构的 `[2]`）。
- 每条都重跑过两遍，字节一致。

## Q1 算术是否按声明宽度自动截断

片段 `work/tmp/ul7/q1.si`（插进 `mode_refresh()`，下同）：

```text
store(#SIMULATION_STATE + 264, U64 (U8 (250 + 10)))
store(#SIMULATION_STATE + 272, U64 (U1 (1 + 1)))
store(#SIMULATION_STATE + 280, U64 0xFFFFFFFFFFFFFFFF)          // 先填满 FF
store(#SIMULATION_STATE + 280, U32 (U64 4294967297))            // 再看写几个字节
```

`q1.pages/t0003_0232a9f50000.bin`：

```text
+264  04 00 00 00 00 00 00 00      U8 (250+10) → 0x04（截断，不是 0x104）
+272  00 00 00 00 00 00 00 00      U1 (1+1)    → 0（截断，不是 2）
+280  01 00 00 00 ff ff ff ff      U32 值只覆盖 4 字节，284..287 保持预填的 FF
```

**运行时值同款**（排除「只在常量折叠时截断」）：片段 `work/tmp/ul7/q1b.si` 用全局 `cycle`（tick 3 时为 3）：

```text
store(#SIMULATION_STATE + 288, U64 cycle)
store(#SIMULATION_STATE + 296, U64 (U5 (cycle * 11)))     // 33
store(#SIMULATION_STATE + 304, U64 (U1 (cycle)))          // 3
store(#SIMULATION_STATE + 312, U64 (U8 (cycle * 1000)))   // 3000
```

```text
+288  03 …                     cycle = 3
+296  01 …                     U5 (33)   → 1    （33 & 0x1F）
+304  01 …                     U1 (3)    → 1
+312  b8 …                     U8 (3000) → 0xB8 （3000 & 0xFF）
```

验证：

```bash
cd reversing
python tools/try_compile_dll.py --compile --src work/tmp/ul7/q1.si          # q1 的 [0]=0x31DF（=12767）；先核对 out_len == 文件大小，见下「坑」
cp work/compile_out.bin work/tmp/ul7/q1.bin
PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul7/q1.si --bin work/tmp/ul7/q1.bin --entry 0x212F --ticks 3 --out work/tmp/ul7/q1.jsonl --dump-pages work/tmp/ul7/q1.pages
python -c "d=open('work/tmp/ul7/q1.pages/t0003_0232a9f50000.bin','rb').read();print(' '.join(f'{b:02x}' for b in d[264:288]))"
```
→ `04 00 00 00 00 00 00 00 00 …`（+264=04、+272=00、+280 起 `01 00 00 00 ff ff ff ff`）
（q1b 同法，把 `q1` 换成 `q1b`、偏移看 288/296/304/312）

## Q2 非 2 的幂宽度

片段 `work/tmp/ul7/q2.si`：

```text
store(#SIMULATION_STATE + 264, U64 (U5 (31 + 1)))
store(#SIMULATION_STATE + 272, U64 (U3 (7 + 1)))
store(#SIMULATION_STATE + 280, U64 (U5 (33)))
store(#SIMULATION_STATE + 288, U64 (U5 31 + U5 1))
store(#SIMULATION_STATE + 296, U64 (U5 (20 + 8)))
```

```text
+264  00 …     U5 (31+1=32) → 0     （32 & 0x1F）
+272  00 …     U3 (7+1=8)   → 0     （8 & 0x07）
+280  01 …     U5 (33)      → 1     （33 & 0x1F，字面量超宽也掩码，不报错）
+288  00 …     U5 31 + U5 1 → 0     （结果按 5 位截断）
+296  1c …     U5 (20+8)    → 28    （不溢出的对照点）
```

**截断在运算结果上，不在赋值处**：片段 `work/tmp/ul7/q2b.si`

```text
let t = U5 31 + U5 1                 // 变宽后再掩码就不是 0 了
store(#SIMULATION_STATE + 320, U64 t)
let u = U64 (U5 31) + U64 1          // 先变宽再相加 → 不截断
store(#SIMULATION_STATE + 328, u)
```

```text
+320  00 …     t = 0
+328  20 …     u = 32
```

**大于 32 位的非 2 的幂也成立**（靶子里 `U33` 出现 1023 次，必须钉）：片段 `work/tmp/ul7/q2c.si`

```text
store(#SIMULATION_STATE + 336, U64 (U33 0x1FFFFFFFF))        // 33 位全 1
store(#SIMULATION_STATE + 344, U64 (U33 0xFFFFFFFFF))        // 36 位，超宽
store(#SIMULATION_STATE + 352, U64 (U33 (0x1FFFFFFFF + 1)))  // 2^33
store(#SIMULATION_STATE + 360, U64 (U20 0x1FFFFF))           // 21 位，超宽
store(#SIMULATION_STATE + 368, U64 (U10 0x7FF))              // 11 位，超宽
```

```text
+336  ff ff ff ff 01 00 00 00      = 0x1FFFFFFFF（33 位原样）
+344  ff ff ff ff 01 00 00 00      = 0x1FFFFFFFF（36 位掩到 33 位）
+352  00 …                         = 0（2^33 掩掉）
+360  ff ff 0f 00 00 00 00 00      = 0xFFFFF（21 位掩到 20 位）
+368  ff 03 00 00 00 00 00 00      = 0x3FF（11 位掩到 10 位）
```

验证（q2 / q2b / q2c 同法，只换文件名与偏移）：

```bash
cd reversing
python tools/try_compile_dll.py --compile --src work/tmp/ul7/q2c.si ; cp work/compile_out.bin work/tmp/ul7/q2c.bin
PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul7/q2c.si --bin work/tmp/ul7/q2c.bin --entry 0x212F --ticks 3 --out work/tmp/ul7/q2c.jsonl --dump-pages work/tmp/ul7/q2c.pages
python -c "d=open('work/tmp/ul7/q2c.pages/t0003_0232a9f50000.bin','rb').read();print(' '.join(f'{b:02x}' for b in d[336:376]))"
```

## Q3 提升方向

片段 `work/tmp/ul7/q3.si`：

```text
store(#SIMULATION_STATE + 264, U64 (U32 0xFFFFFFFF))
store(#SIMULATION_STATE + 272, U64 (S32 -1))
store(#SIMULATION_STATE + 280, S64 (S8 -1))
store(#SIMULATION_STATE + 288, U64 (S8 -1))
store(#SIMULATION_STATE + 296, S64 (U8 0xFF))
store(#SIMULATION_STATE + 304, U64 (U32 0x80000000))
```

```text
+264  ff ff ff ff 00 00 00 00   U64 (U32 0xFFFFFFFF)    → 零扩展
+272  ff ff ff ff ff ff ff ff   U64 (S32 -1)            → 符号扩展
+280  ff ff ff ff ff ff ff ff   S64 (S8 -1)             → 符号扩展
+288  ff ff ff ff ff ff ff ff   U64 (S8 -1)             → 符号扩展（有符号源伸到无符号目标也符号扩展）
+296  ff 00 00 00 00 00 00 00   S64 (U8 0xFF)           → 零扩展（无符号源伸到有符号目标也零扩展）
+304  00 00 00 80 00 00 00 00   U64 (U32 0x80000000)    → 零扩展
```

判定规则：**看源类型有符号与否，不看目标类型**。

验证：`q3` 同 Q1 的命令（偏移 264 起 48 字节）。

## Q4 窄读是否掩码

片段 `work/tmp/ul7/q4.si`：

```text
store(#SIMULATION_STATE + 320, U8 0xFF)                                   // 源字节
store(#SIMULATION_STATE + 264, U64 (load(<U5>, #SIMULATION_STATE + 320)))
store(#SIMULATION_STATE + 272, U64 (load(<U3>, #SIMULATION_STATE + 320)))
store(#SIMULATION_STATE + 280, U64 (load(<U7>, #SIMULATION_STATE + 320)))
store(#SIMULATION_STATE + 288, U64 (load(<U1>, #SIMULATION_STATE + 320)))
store(#SIMULATION_STATE + 296, U64 (load(<U8>, #SIMULATION_STATE + 320)))
```

```text
+320  ff …   源字节
+264  1f …   load(<U5>) → 0x1F（掩码）
+272  07 …   load(<U3>) → 0x07
+280  7f …   load(<U7>) → 0x7F
+288  01 …   load(<U1>) → 0x01
+296  ff …   load(<U8>) → 0xFF（8 位，本来就整字节）
```

验证：`q4` 同 Q1 的命令（偏移 320 与 264/272/280/288/296）。

## 未解 / 边界

- **store 一个非字节宽度值写几个字节**（`store(p, U5 31)` / `U33 …` 的写宽度与对齐）**没测** —— 那是 §9 **U-L1** 的 `uN_width` 配方，本任务不碰 U-L1。已知的只有「字节整数倍宽度按宽度写」（U32 → 4 字节）。
- `U5 31 + U5 1 = 0` 无法区分「运算后掩码」与「按 5 位做模算术」—— 两者在模 2^N 下同值，对 lowering 无差别，不算风险。
- `load(<U5>, p)` 的掩码值同值于「读 1 字节再掩码」和「读 8 字节再掩码」（掩码只留低 5 位）→ **读宽度/对齐归 U-L1**，此处不结论。
- 位宽 > 64 未测（靶子里没有）。
- 观测点只落在 `#SIMULATION_STATE` 一页；同页内 256/257 由基座自己写，互不干扰。

## 对 lowering 的直接指示

1. **掩码插在「每次类型转换 + 每个运算结果」上**，不是只在赋值/store 处：
   `and (2^N - 1)`（等价于 trunc 到 iN 再 zext），证据 `U5 31 + U5 1 → 0` vs `U64 (U5 31) + U64 1 → 32`。
2. **用 i64 运算 + 掩码表达宽度**，不要交给 `i33`/`i5` 让后端猜（§4b 的建议成立，且掩码在 N=33 上实测有效）。
3. **提升按源类型符号性**：无符号源 `zext`、有符号源 `sext`（`U64 (S8 -1)` 是全 FF，别按目标类型选）。
4. **`store` 写宽度 = 值的字节宽度**（U32 只写 4 字节）—— 不要一律写 8 字节。
5. **`load(<UN>)` 之后必须掩码到 N 位**（`0xFF` 读成 `U5` 是 `0x1F`）。

## 坑（这次踩到的，写下来别再踩）

`work/compile_out.bin` 是工具默认的共享输出路径，**别的线（U-L1）也在写它**；而且把
`try_compile_dll.py` 的输出接进会提前退出的管道（`| head`）会让进程半途死掉、文件不被刷新。
我第一次跑 q4 就是这么拿到**别的产物**（9119 字节，头 0x239F 与自身大小一致，不是我任何片段的 out_len）——
宿主表现成 `thread_exit`、tick 1 就死，看着像「窄读会让产物跑不起来」，其实是产物拿错了。
判据：编译后先核对 `[0] out_len == work/compile_out.bin 的文件大小`，再 `cp`；别把编译器输出接进 `head`。
