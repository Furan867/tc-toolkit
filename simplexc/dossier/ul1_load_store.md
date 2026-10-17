# U-L1：`load`/`store` 的地址单位、对齐、窄类型写 —— 实测冻结

**结论（一行）**：`p` 是**字节地址**（`p + 1` = 下一字节，不是下一个元素）；**没有对齐要求**
（非 8 对齐的 `U64` 读写都静默成功、精确读写「该地址起的 8 个字节」、邻居字节保留）；**窄类型写只写该宽度的字节数**
（`store(p, U8 v)` 写 1 字节，邻居不变）。

→ 落地到 lowering：`Ptr` 算术直接用**字节数**做 `add i64`（不做元素宽度缩放）；
`load`/`store` 的对齐只能按 **1** 声明，且**不得插入对齐检查/陷阱**（参考实现在非对齐时不报错、不崩、不向下对齐）。

## 0. 观测面（怎么看见的）

- 片段 = 自建 scratch，**不跑协议循环**：地址表 + 直接顶层的 `store`/`load` 语句。
  程序 `main` 返回后线程自己退出 → 宿主 `tick` 报 `reason=thread_exit`、**rc=2**，但页转储照样落盘
  （`tick()` 里 `_snapshot` 无条件 `--dump-pages`）。**rc=2 是预期，不是失败判据。**
- 观测 = 页转储里的**绝对字节**。`#SIMULATION_STATE`（0x232A9F50000）这个块一个 1 MB 区域，
  取它内部两块空地：`#UL1_FILL = 2416623067136`（页内 +0xA000）、`#UL1_OUT = 2416623075328`（页内 +0xC000）。
- 编译产物路径 `work/compile_out.bin` 是**共享路径**：本机有别的进程在同一时刻编译并覆盖它
  （实测：编完到 `cp` 之间被换成一份 12801 字节的外来产物，跑出来「页在变但我的写入全空」）。
  → 判据一律「编译完**立刻** `cp` 到自己的名字，再核对 `out_len`/指纹」。本文件所有数字都来自核对过的产物。
- 片段源码在 `work/tmp/ul1/*.si`（`work/` 被 .gitignore 忽略，所以下面把片段正文抄全）。

### 共用片段头（所有片段一致）

```text
var commands = Ptr 2416573349888
var settings = Ptr 2416573415424

type SimCommand Enum[run, refresh, mode_reset, quit_simulation]
type CommandIndex Enum[ctl_command, ctl_command_id, ctl_cycle_speed_ms, ctl_exit, ctl_level_manual_input, ctl_level_manual_input_id, ctl_test]
type StateIndex Enum[sim_cycle, sim_target_cycle, sim_test_result, sim_last_command_id, sim_error_component, sim_short_circuit_component_id_1, sim_short_circuit_component_id_2, sim_short_circuit_pin_1, sim_short_circuit_pin_2, sim_short_circuit_top_level_permanent_id_1, sim_short_circuit_top_level_permanent_id_2, sim_short_circuit_value_1, sim_short_circuit_value_2, sim_short_circuit_any_top_level_wire_id, sim_running]

const #SIMULATION_STATE = Ptr 2416623026176
const #UL1_FILL = Ptr 2416623067136
const #UL1_OUT = Ptr 2416623075328
```

`commands` / `settings` / 三个枚举是宿主 `simspec` 的硬要求（`--src` 只是给宿主读环境规格，不是给编译器用的）。

### 三条命令（每个片段都是这三步）

```bash
# ① 编译（必须子进程：解析期错误会让进程 fatal）
python tools/try_compile_dll.py --compile --src work/tmp/ul1/<frag>.si
# ② 立刻存一份并核对：out_len = 输出的 [0]，entry = 输出的 [2]
cp work/compile_out.bin work/tmp/ul1/<frag>.bin
# ③ 差分宿主（--entry 用 ② 的 [2]）
PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul1/<frag>.si \
    --bin work/tmp/ul1/<frag>.bin --ticks 1 --entry 0x212F \
    --out work/tmp/ul1/<frag>.jsonl --dump-pages work/tmp/ul1/<frag>.pages
```

页转储文件名 `t0001_<页基址>.bin`，读法：`python work/tmp/ul1/read_page.py <页文件> <页内偏移> <qword 数>`。

## 1. 单位：`p` 是字节地址

**片段** `work/tmp/ul1/q1_unit.si`（头 + 下面这段）：先按 `+8k` 铺 20 个**互不相同**的 8 字节槽，
再读 5 个 8 对齐地址，把结果写到 `#UL1_OUT`。

```text
store(#UL1_FILL + 0, U64 0x0101010101010101)      // v0
store(#UL1_FILL + 8, U64 0x0202020202020202)      // v1
store(#UL1_FILL + 16, U64 0x0303030303030303)     // v2
   …（每 8 递增一字节值，直到）
store(#UL1_FILL + 152, U64 0x1414141414141414)    // v19

store(#UL1_OUT + 0,  load(<U64>, #UL1_FILL + 0))
store(#UL1_OUT + 8,  load(<U64>, #UL1_FILL + 8))
store(#UL1_OUT + 16, load(<U64>, #UL1_FILL + 24))
store(#UL1_OUT + 24, load(<U64>, #UL1_FILL + 64))
store(#UL1_OUT + 32, load(<U64>, #UL1_FILL + 152))
```

**观测（`work/tmp/ul1/q1_unit.pages/t0001_0232a9f50000.bin` 的原始字节）**

| 地址 | 字节 | 说明 |
| --- | --- | --- |
| 0x232A9F5A000..+15 | `01×8 02×8` | `+8k` 落在**字节 8k**：160 个字节里 20 个槽**稠密**排布 |
| 0x232A9F5A098..+9F | `14×8` | 第 20 个槽在字节 152..159 = `+152` ⇒ 单位 1 字节 |
| 0x232A9F5A0A0 起（第 161 字节） | `00…` | 没有「每个槽占 64 字节」的稀疏形态（若是那样，字节 8..63 会是 0） |

`#UL1_OUT`（0x232A9F5C000）：

| 读的地址 | 得到 | 结论 |
| --- | --- | --- |
| `FILL + 0` | `0x0101…01` = v0 | `+0` = 字节 0 |
| `FILL + 8` | `0x0202…02` = v1 | `+8` = **字节 8**（不是 v8 所在的字节 64） |
| `FILL + 24` | `0x0404…04` = v3 | `+24` = 字节 24 |
| `FILL + 64` | `0x0909…09` = v8 | `+64` = 字节 64 |
| `FILL + 152` | `0x1414…14` = v19 | `+152` = 字节 152 |

**运行期偏移也是字节**：`work/tmp/ul1/q1b_var_offset.si` 用循环变量
`store(#UL1_FILL + k * 8, U64 0x0F0F0F0F0F0F0F0F)`（k = 0…19）铺，再用
`store(#UL1_OUT + j * 8, load(<U64>, #UL1_FILL + j * 8))` 镜像 → 两块都从基址起
**连续 160 字节全 0x0F**、第 161 字节起为 0（地址 `p + k*8` 里 `k*8` 是字节数，`Ptr + Int` 不缩放）。

**排除的假说**：①「`p + 1` = 下一个元素（+8 字节）」——`FILL + 8` 读到的是字节 8 处的 v1，且槽是稠密的；
②「`Ptr + Int` 按语言层的元素宽度缩放」——稠密排布 + 运行期循环都否掉；
③「常量偏移和运行期偏移规则不同」——q1 vs q1b 两块都是稠密 160 字节。

**验证**：
`python tools/try_compile_dll.py --compile --src work/tmp/ul1/q1_unit.si`（`[0]=0x23D3=9171`、`[2]=0x212F`、`[3]=0`）
→ `PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul1/q1_unit.si --bin work/tmp/ul1/q1_unit.bin --ticks 1 --entry 0x212F --out work/tmp/ul1/q1_unit.jsonl --dump-pages work/tmp/ul1/q1_unit.pages`
→ 页 `0x232A9F5C000` 的字节 = `0101010101010101 0202020202020202 0404040404040404 0909090909090909 1414141414141414`。
`q1b` 同理（`[0]=0x2270=8816`），页 `0x232A9F5A000`/`0x232A9F5C000` 各 160 字节 `0f`。

## 2. 对齐：**没有**要求（非对齐静默按字节读写，不报错、不 AV、不向下对齐）

### 2a. 非对齐 load

**片段** `work/tmp/ul1/q2_load_unaligned.si`：同一铺法（`FILL + 8k` = 重复字节 k+1），
只把探针改成非 8 对齐：

```text
store(#UL1_OUT + 0,  load(<U64>, #UL1_FILL + 1))
store(#UL1_OUT + 8,  load(<U64>, #UL1_FILL + 4))
store(#UL1_OUT + 16, load(<U64>, #UL1_FILL + 2))
```

**观测**（页 `0x232A9F5C000`）：`0101010101010102 | 0101010102020202 | 0101010101010202`

| 探针 | 得到 | 应为「从该字节起的连续 8 字节」 |
| --- | --- | --- |
| `FILL + 1` | `0x0201010101010101` | 字节 1..7 = 01（v0 高 7 字节）+ 字节 8 = 02（v1 低字节）✓ |
| `FILL + 4` | `0x0202020201010101` | 字节 4..7 = 01 + 字节 8..11 = 02 ✓ |
| `FILL + 2` | `0x0202010101010101` | 字节 2..7 = 01×6 + 字节 8..9 = 02×2 ✓ |

宿主没有报 `[host] 异常 …`，程序跑到正常退出（`exited_clean`）：**非对齐 load 只是静默的字节级读**。

### 2b. 非对齐 store

**片段** `work/tmp/ul1/q2_store_unaligned.si`：先把 `FILL + 8k`（k = 0…19）全铺成
`U64 0xAAAAAAAAAAAAAAAA`（共 160 字节 0xAA），再

```text
store(#UL1_FILL + 1, U64 0x1122334455667788)
```

**观测**（页 `0x232A9F5A000` 前 24 字节）：
`aa 88 77 66 55 44 33 22 11 aa aa aa aa aa aa aa | aa aa aa aa aa aa aa aa`

→ 8 个字节落在**绝对字节 1..8**（小端 `88 77 66 55 44 33 22 11`），**字节 0 的 0xAA 与字节 9 起的 0xAA 都在**。
即：非对齐 store 静默写入、**不向下对齐**、**不越写邻居**、不 AV。

**排除的假说**：①「非 8 对齐会报错/陷阱」——两次都没报错、进程正常退出；
②「会向下取整到 8 的对齐地址」——`FILL+1` 的写入从字节 1 起（字节 0 仍是 0xAA）；
③「非对齐退化成一个更大范围的读写」——2b 里字节 0/9 起都没被碰。

**验证**：
`python tools/try_compile_dll.py --compile --src work/tmp/ul1/q2_load_unaligned.si`（`[0]=0x239F=9119`）→
`PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul1/q2_load_unaligned.si --bin work/tmp/ul1/q2_load_unaligned.bin --ticks 1 --entry 0x212F --out work/tmp/ul1/q2_load_unaligned.jsonl --dump-pages work/tmp/ul1/q2_load_unaligned.pages`。
store 侧同样是一条命令（`q2_store_unaligned.si`，`[0]=0x2368=9064`）；两次 rc 都是 **2**（= `reason=thread_exit`，见 §0）。

## 3. 窄类型写：只写该宽度的字节数，邻居保留

**片段** `work/tmp/ul1/q3_narrow_store.si`：先 `FILL + 8k`（k=0…19）铺满 0xAA，再

```text
store(#UL1_FILL + 0, U8 0x55)
store(#UL1_FILL + 8, U8 0x66)
store(#UL1_FILL + 2, U8 0x77)
```

**观测**（页 `0x232A9F5A000` 前 18 字节）：
`55 aa 77 aa aa aa aa aa | 66 aa aa aa aa aa aa aa | aa aa`

→ 三处都**只写 1 个字节**（0x55→字节 0、0x77→字节 2、0x66→字节 8），其余 0xAA 原样保留。
**不是**「写整个 8 字节把邻居清零」（那样会看到 `55 00 00 00 00 00 00 00`）。

**排除的假说**：①「窄类型 store 提升成 8 字节写」——邻居字节没被清零；
②「窄类型 store 读到-改-写整字但有 bug」——`FILL+2` 的 0x77 与 `FILL+0` 的 0x55 同时存在，
说明三次写各自只动一个字节，没有互相覆盖。

**验证**：`python tools/try_compile_dll.py --compile --src work/tmp/ul1/q3_narrow_store.si`（`[0]=0x2384=9092`）→
`PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul1/q3_narrow_store.si --bin work/tmp/ul1/q3_narrow_store.bin --ticks 1 --entry 0x212F --out work/tmp/ul1/q3_narrow_store.jsonl --dump-pages work/tmp/ul1/q3_narrow_store.pages`。

## 4. 给 lowering 的落点（U-L1 定死）

| 问题 | 定死的答案 | IR 上的后果 |
| --- | --- | --- |
| 单位 | 字节地址 | `#SIMULATION_STATE + 264` → `add i64 <const>, 264`；`p + Int k` → `add` 直接用 `k`，**不乘元素宽度** |
| 对齐 | 无要求、无检查 | `LLVMBuildLoad2`/`LLVMBuildStore` 后 `LLVMSetAlignment(1)`；**不要**设 8（那是参考实现没有的假设），**不要**生成对齐检查 |
| 窄类型写 | 只写该宽度字节 | `store(p, U8 v)` → `store i8`；不能提升成 i64 写（会清邻居） |

## 5. 未解 / 没测（不许当已定）

1. **只测了 U64（宽）与 U8（窄）**。U32/U16/U1/U5/U3… 的读写**宽度**语义（是先 `load i8` 再 `trunc/and`
   还是直接 `i5*`）属于 **U-L7**，本次没测；U-L1 只回答「地址是什么单位、要不要对齐」。
2. **只测了 `Ptr` 常量 + 常量偏移、以及 `Ptr + Int`（运行期变量）**。
   `Str[i]` / `Arr[i]` 下标的单位与越界属于 U-L2/U-L3，没测。
3. **没测跨页边界的非对齐读写**（本实验全部落在同一页内）。理论上只要相邻页都映射就照读，
   但没有实测；别把「跨页也一定不 AV」写进结论。
4. **没有验证语义**：`load(<U64>, p)` 是否只读 8 字节（内存观测看不出「多读」）。
   store 侧的「不多写」有证据（2b/§3 的邻居保留）；load 侧只有「读到的是那 8 个字节」。
5. **没有反汇编**：结论是「可观测行为」，不是「产物里没有对齐检查指令」。存在恒不触发的检查也
   与语义无关；反正非对齐时必须放行。
6. 三个片段的宿主 **rc 都是 2**（`reason=thread_exit`，程序跑完自己的语句后线程退出）。
   这是本观测法的正常收尾，**不是**判据失败；页转储已经落盘。
