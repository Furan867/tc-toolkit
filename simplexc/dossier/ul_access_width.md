# U-L1b：位宽 → 访存字节数（`load`/`store` 的宽度语义）— 实测冻结

**结论（一行）**：`store(p, U<N> v)` 写的字节数 = **`ceil(N/8)`（`N ≤ 32`）；`N > 32` 时一律写 8 字节**
（`U33` 实际写 `55 55 55 55 01 00 00 00` —— 多出的 3 个字节被**写成 0**，不是保留）；
`N ≤ 32` 时超出 `ceil(N/8)` 的邻居字节**保留原值**；`load(<U<N>, p)` 的返回值恒 = **所读字节按 `N` 位掩码、向上零扩展**
（高位恒 0，没有残留）；宽度只跟**类型**走，与对齐、与值是常量还是运行期值无关。

→ 落地到 lowering 的两条硬约束：① `N ≤ 32` 的 store 只能写 `ceil(N/8)` 字节（`U17`=3、`U23`=3、`U25`=4，**不是**最小宿主宽度 2/4/8）；
② **`N > 32`（`U33/U36/U40/U48` …）的 store 是 8 字节零扩展写**，会清掉后面 2–3 个邻居字节 —— 这是原版编译器的真实行为，不是「泄漏」。

## 0. 观测面（与 U-L1 同一套器材，换了块空地）

- 片段 = 自建 scratch，**不跑协议循环**：地址表 + 顶层 `store`/`load` 语句。程序 `main` 返回后线程自己退出
  → 宿主 `tick` 报 `reason=thread_exit`、**rc=2**，页转储照样落盘。**rc=2 是预期，不是失败判据。**
- 观测 = 页转储里的**绝对字节**（`t0001_0232a9f50000.bin`，64 KB 页）。两块空地都在 `#SIMULATION_STATE` 的 1 MB 区域、
  同一页内（与 U-L1 用的 `+0xA000`/`+0xC000` 不重叠）：

  | 名字 | 值 | 页内偏移 |
  | --- | --- | --- |
  | `#ULW_FILL` | `2416623083520` | `+0xE000`（预填区，64 字节） |
  | `#ULW_OUT`  | `2416623087616` | `+0xF000`（读回区，64 字节） |

- 预填一律 **`0xFF`**（8 条 `store(… , U64 0xFFFFFFFFFFFFFFFF)`）。选 `0xFF` 是为了**同时**看三件事：
  写进去的模式值字节（`0x55`/`0x01`）≠ `0xFF` ⇒ 看得见「写到哪个字节」；超出范围仍是 `0xFF` ⇒ 邻居有没有被改；
  被改写成 `0x00` ⇒ 是**被清掉**而不是保留。未写过的区外是 `0x00`（页初值），三种状态互不混淆。
- 编译产物路径 `work/compile_out.bin` 是**共享路径**（别的进程会覆盖，U-L1 §0 吃过一次）。本文件每个用例都
  「编译完**立刻** `cp` 到自己的名字，再核对 `out_len == 文件大小`」，全部对得上。

## 1. 共用片段头

```text
var commands = Ptr 2416573349888
var settings = Ptr 2416573415424

type SimCommand Enum[run, refresh, mode_reset, quit_simulation]
type CommandIndex Enum[ctl_command, ctl_command_id, ctl_cycle_speed_ms, ctl_exit, ctl_level_manual_input, ctl_level_manual_input_id, ctl_test]
type StateIndex Enum[sim_cycle, sim_target_cycle, sim_test_result, sim_last_command_id, sim_error_component, sim_short_circuit_component_id_1, sim_short_circuit_component_id_2, sim_short_circuit_pin_1, sim_short_circuit_pin_2, sim_short_circuit_top_level_permanent_id_1, sim_short_circuit_top_level_permanent_id_2, sim_short_circuit_value_1, sim_short_circuit_value_2, sim_short_circuit_any_top_level_wire_id, sim_running]

const #SIMULATION_STATE = Ptr 2416623026176
const #ULW_FILL = Ptr 2416623083520
const #ULW_OUT = Ptr 2416623087616
```

`commands` / `settings` / 三个枚举是宿主 `simspec` 的硬要求（`--src` 只给宿主读环境规格，不是给编译器用的）。

## 2. 共用片段体（每个宽度只换 `N`、`V`、`W`）

```text
// ① 预填 64 字节 0xFF（+0 / +8 / … / +56 共 8 条）
store(#ULW_FILL + 0, U64 0xFFFFFFFFFFFFFFFF)
   …

// ② 模式值 V = 0x5555555555555555 & (2^N - 1)（写进去的字节是 0x55 / 0x01，不是 0xFF）
store(#ULW_FILL + 8, U<N> V)

// ③ 全 1 值 W = 2^N - 1
store(#ULW_FILL + 16, U<N> W)

// ④ 读回：模式值槽 / 全 1 槽 / 未写过的 0xFF 槽 / 原始 8 字节
store(#ULW_OUT + 0,  U64 load(<U<N>, #ULW_FILL + 8))
store(#ULW_OUT + 8,  U64 load(<U<N>, #ULW_FILL + 16))
store(#ULW_OUT + 16, U64 load(<U<N>, #ULW_FILL + 0))
store(#ULW_OUT + 24, load(<U64>, #ULW_FILL + 8))
store(#ULW_OUT + 32, load(<U64>, #ULW_FILL + 16))
```

一个完整的实例（`U33`，可直接存盘跑）：

```text
var commands = Ptr 2416573349888
var settings = Ptr 2416573415424

type SimCommand Enum[run, refresh, mode_reset, quit_simulation]
type CommandIndex Enum[ctl_command, ctl_command_id, ctl_cycle_speed_ms, ctl_exit, ctl_level_manual_input, ctl_level_manual_input_id, ctl_test]
type StateIndex Enum[sim_cycle, sim_target_cycle, sim_test_result, sim_last_command_id, sim_error_component, sim_short_circuit_component_id_1, sim_short_circuit_component_id_2, sim_short_circuit_pin_1, sim_short_circuit_pin_2, sim_short_circuit_top_level_permanent_id_1, sim_short_circuit_top_level_permanent_id_2, sim_short_circuit_value_1, sim_short_circuit_value_2, sim_short_circuit_any_top_level_wire_id, sim_running]

const #SIMULATION_STATE = Ptr 2416623026176
const #ULW_FILL = Ptr 2416623083520
const #ULW_OUT = Ptr 2416623087616

store(#ULW_FILL + 0, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 8, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 16, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 24, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 32, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 40, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 48, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 56, U64 0xFFFFFFFFFFFFFFFF)
store(#ULW_FILL + 8, U33 0x155555555)
store(#ULW_FILL + 16, U33 0x1FFFFFFFF)
store(#ULW_OUT + 0, U64 load(<U33>, #ULW_FILL + 8))
store(#ULW_OUT + 8, U64 load(<U33>, #ULW_FILL + 16))
store(#ULW_OUT + 16, U64 load(<U33>, #ULW_FILL + 0))
store(#ULW_OUT + 24, load(<U64>, #ULW_FILL + 8))
store(#ULW_OUT + 32, load(<U64>, #ULW_FILL + 16))
```

## 3. 主表：`store` 写几个字节 / 邻居怎么样 / `load` 读回什么

`FILL+8..15` 那列是**页转储里的真实字节**（未写的字节还是预填的 `0xFF`）。
「清掉的邻居」= 超出 `ceil(N/8)` 却被写成 `0x00` 的字节位置（相对 `FILL`）。

| `N` | 写的模式值 `V` | `FILL+8..15` | 写了几字节 | 清掉的邻居 | `load(<UN>)`@`+8` | @`+16`(全 1 槽) | @`+0`(未写过的 `0xFF`) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `0x1` | `01 ff ff ff ff ff ff ff` | 1 | — | `0x1` | `0x1` | `0x1` |
| 3 | `0x5` | `05 ff ff ff ff ff ff ff` | 1 | — | `0x5` | `0x7` | `0x7` |
| 4 | `0x5` | `05 ff ff ff ff ff ff ff` | 1 | — | `0x5` | `0xF` | `0xF` |
| 5 | `0x15` | `15 ff ff ff ff ff ff ff` | 1 | — | `0x15` | `0x1F` | `0x1F` |
| 9 | `0x155` | `55 01 ff ff ff ff ff ff` | 2 | — | `0x155` | `0x1FF` | `0x1FF` |
| 15 | `0x5555` | `55 55 ff ff ff ff ff ff` | 2 | — | `0x5555` | `0x7FFF` | `0x7FFF` |
| 16 | `0x5555` | `55 55 ff ff ff ff ff ff` | 2 | — | `0x5555` | `0xFFFF` | `0xFFFF` |
| 17 | `0x15555` | `55 55 01 ff ff ff ff ff` | **3** | — | `0x15555` | `0x1FFFF` | `0x1FFFF` |
| 23 | `0x555555` | `55 55 55 ff ff ff ff ff` | **3** | — | `0x555555` | `0x7FFFFF` | `0x7FFFFF` |
| 24 | `0x555555` | `55 55 55 ff ff ff ff ff` | **3** | — | `0x555555` | `0xFFFFFF` | `0xFFFFFF` |
| 25 | `0x1555555` | `55 55 55 01 ff ff ff ff` | 4 | — | `0x1555555` | `0x1FFFFFF` | `0x1FFFFFF` |
| 31 | `0x55555555` | `55 55 55 55 ff ff ff ff` | 4 | — | `0x55555555` | `0x7FFFFFFF` | `0x7FFFFFFF` |
| 32 | `0x55555555` | `55 55 55 55 ff ff ff ff` | 4 | — | `0x55555555` | `0xFFFFFFFF` | `0xFFFFFFFF` |
| 33 | `0x155555555` | `55 55 55 55 01 00 00 00` | **8** | `13 14 15` | `0x155555555` | `0x1FFFFFFFF` | `0x1FFFFFFFF` |
| 36 | `0x555555555` | `55 55 55 55 05 00 00 00` | **8** | `13 14 15` | `0x555555555` | `0xFFFFFFFFF` | `0xFFFFFFFFF` |
| 40 | `0x5555555555` | `55 55 55 55 55 00 00 00` | **8** | `13 14 15` | `0x5555555555` | `0xFFFFFFFFFF` | `0xFFFFFFFFFF` |
| 48 | `0x555555555555` | `55 55 55 55 55 55 00 00` | **8** | `14 15` | `0x555555555555` | `0xFFFFFFFFFFFF` | `0xFFFFFFFFFFFF` |
| 64 | `0x5555555555555555` | `55 55 55 55 55 55 55 55` | 8 | — | `0x5555555555555555` | `0xFFFFFFFFFFFFFFFF` | `0xFFFFFFFFFFFFFFFF` |

读三行结论：

1. **写宽度 `= ceil(N/8)`，但只在 `N ≤ 32` 成立**。`U17` 只写 3 字节（`55 55 01`，第 4 字节仍是 `0xFF`）、`U23`=3、`U25`=4
   —— 所以不是「最小宿主宽度（2/4/8）」：那样 `U17/U23/U24` 都该是 4 字节。
2. **`N > 32` 一律 8 字节零扩展写**：`U33` 写完 `FILL+13..15` 变成 `00`（不是 `FF`）。
   `U33/U36/U40` 清 3 个邻居字节、`U48` 清 2 个。`U64` 正好 8 字节，无越界。
3. **`load` 的返回值 = 掩码到 `N` 位、高位零扩展**：全 `0xFF` 的槽读成 `U1`→`0x1`、`U3`→`0x7`、`U5`→`0x1F`、
   `U9`→`0x1FF`、`U17`→`0x1FFFF`、`U33`→`0x1FFFFFFFF`（`0x00` 高位为 0，不是残留的 `0xFF`）。

**排除的假说**：

| 假说 | 否掉的证据 |
| --- | --- |
| 窄写统一提升成 8 字节写（清邻居） | `U1…U32` 的邻居仍是预填 `0xFF` |
| 窄写按「最小宿主宽度」2/4/8 字节 | `U17`→3、`U23`→3（按宿主宽度该是 4） |
| 窄写一律 `ceil(N/8)` | `U33`→8、`U40`→8、`U48`→8（`ceil` 是 5/5/6） |
| `N>32` 多写的那几字节只是「保留原值」 | 预填 `0xFF`，多出的字节变成 `0x00` |
| 宽度规则只在常量折叠里成立 | 运行期值（`load(<U64>,…)` 转 `U33`/`U17`）逐字节同结果（见 §5） |
| `load` 高位残留 | 18 个宽度的读回值逐位 = 掩码结果 |
| 非字节对齐地址会向下对齐 | 见 §4：4 个非对齐地址都精确落在 `p` 起 |

> 关于 §3 里 `+16` 那两列：`U16/U32/U64` 的「全 1 槽」写的就是 `0xFF...`，与预填不可区分（表里 `@+16` 读回正确，
> 但「写了几个字节」这件事在 `+16` 上看不出来）—— 这三个宽度的字节数由 `+8` 的模式值行给出，非 2 的幂宽度由
> `U9/U15/U17/U23/U25/U31` 这些边界值补齐。

## 4. 非对齐**地址**（宽度规则与地址对齐无关）

片段 `mis`：预填 64 字节 `0xFF`，然后 `store(FILL+9, U9 0x155)`、`store(FILL+17, U33 0x155555555)`、
`store(FILL+33, U32 0x55555555)`、`store(FILL+41, U16 0x5555)`。

页转储（`FILL+0..47`，每行 8 字节 = 一个 `store` 槽的对齐窗）：

```text
+0x00: FF FF FF FF FF FF FF FF
+0x08: FF 55 01 FF FF FF FF FF     ← store(FILL+9,  U9  0x155)：字节 9..10
+0x10: FF 55 55 55 55 01 00 00     ← store(FILL+17, U33 0x155555555)：字节 17..24（8 个）
+0x18: 00 FF FF FF FF FF FF FF        └ 其中 22/23/24 是被清掉的邻居
+0x20: FF 55 55 55 55 FF FF FF     ← store(FILL+33, U32 0x55555555)：字节 33..36
+0x28: FF 55 55 FF FF FF FF FF     ← store(FILL+41, U16 0x5555)：字节 41..42
```

| 目标 | 前后各一字节 | 结论 |
| --- | --- | --- |
| `U9 @ +9` | `FF` \| `55 01` \| `FF` | 2 字节，精确从 `p` 起，不向下/向上对齐 |
| `U33 @ +17` | `FF` \| `55 55 55 55 01 00 00 00` \| `FF` | 8 字节零扩展写，清 `+22..+24` |
| `U32 @ +33` | `FF` \| `55 55 55 55` \| `FF` | 4 字节 |
| `U16 @ +41` | `FF` \| `55 55` \| `FF` | 2 字节 |

读回同地址：`load(<U9>,+9)=0x155`、`load(<U33>,+17)=0x155555555`、`load(<U32>,+33)=0x55555555`、`load(<U16>,+41)=0x5555`
—— 非对齐读也不丢字节、不向下对齐。

## 5. 宽度只看类型（常量 vs 运行期值同一规则）

片段 `rt33` / `rt17`：`store(FILL+40, U64 <种子>)` 造种子（`rt33` = `0x0000000155555555`、`rt17` = `0x0000000000015555`），再
`store(FILL+8, U<N> load(<U64>, FILL+40))`（值是运行期算出来的，类型仍是 `U<N>`）。

| 片段 | `FILL+8..15` | 写了几字节 | 读回 |
| --- | --- | --- | --- |
| `rt33` | `55 55 55 55 01 00 00 00` | 8（清 13/14/15） | `load(<U33>)=0x155555555` |
| `rt17` | `55 55 01 ff ff ff ff ff` | 3 | `load(<U17>)=0x15555` |

⇒ 8 字节写不是常量折叠的特例，是 **`N>32` 的 store 路径本身**。

## 6. 验证（每个宽度三条命令，`<d>` = 案例目录，`<frag>` = `w<N>` / `mis` / `rt<N>`）

```bash
# ① 编译（必须子进程：解析期错误会让进程 fatal）
python tools/try_compile_dll.py --compile --src <d>\<frag>.si
# ② 立刻拷走并核对 out_len（= 输出的 [0]）与文件大小
cp work/compile_out.bin <d>\<frag>.bin
# ③ 差分宿主（entry 固定 0x212F，rc=2 是 thread_exit，见 §0）
PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src <d>\<frag>.si \
    --bin <d>\<frag>.bin --ticks 1 --entry 0x212F \
    --out <d>\<frag>.jsonl --dump-pages <d>\<frag>.pages
# ④ 看字节：FILL 在页内 +0xE000、OUT 在 +0xF000
python -c "d=open(r'<d>\<frag>.pages\t0001_0232a9f50000.bin','rb').read();print(' '.join(f'{b:02X}' for b in d[0xE000:0xE040]));print(' '.join(f'{b:02X}' for b in d[0xF000:0xF040]))"
```

观测到的 `out_len`（`[0]`，`[2]` 全部 `0x212F`）：

| 片段 | `out_len` | 片段 | `out_len` | 片段 | `out_len` |
| --- | --- | --- | --- | --- | --- |
| `w1/w3/w4/w5` | `0x2317` (8983) | `w9/w15` | `0x2319` (8985) | `w16` | `0x22CE` (8910) |
| `w17/w23/w24` | `0x2337` (9015) | `w25/w31/w32` | `0x2311` (8977) | `w33/w36/w40/w48` | `0x2320` (8992) |
| `w64` | `0x22D2` (8914) | `mis` | `0x238D` (9101) | `rt17`/`rt33` | `0x22D1` / `0x22BC` |

`w33` 与 `w9` 各复跑一次，页转储 `md5` 逐字节相同（`w33.pages/t0001_0232a9f50000.bin`、`w9.…`）。
案例与原始输出在 `%TEMP%\ul-width\`（不进 git：`run.py` 生成片段、`analyze.py` 机械读表、`rt.py` 运行期对照）。

## 7. 给 lowering 的落点（这部分只陈述后果，怎么落由主线定）

| 问题 | 原版编译器的答案 | IR 上的后果 |
| --- | --- | --- |
| `store(p, U<N> v)`，`N ≤ 32` | 写 `ceil(N/8)` 字节，其余邻居**保留** | `store iN`（LLVM 的 i1..i32 内存操作 store size = `ceil(N/8)`）或拆 `i16/i8`；**不能**提升成 `i32`/`i64` 写 |
| `store(p, U<N> v)`，`N > 32` | 写 **8** 字节，值为掩码后零扩展 → 清掉 `ceil(N/8)` 之后的 2–3 字节 | 等价 `store i64 zext(...)`；若改成 `store iN`（5 字节）就与参考**不一致** |
| `load(<U<N>, p)` | 值 = 掩码到 `N` 位（高位 0） | 读 → 掩码 `(1<<N)-1`；对齐仍按 1 声明（U-L1） |
| 对齐 | 不影响宽度规则（§4） | 不要为窄类型声明自然对齐、不要做对齐检查 |

## 8. 未解 / 没测（不许当已定）

> 2026-10-18 补测：原第 1、2、5、6 项已在 §10 测掉（`load` 读宽度、跨页读、位内高位、`S*`）。
> 仍开放：第 4、7 项；另新增两项 —— `load` 读到**未映射页**的行为（§10.3 只证明"读了 8 字节"，
> 没测读进未映射页是否 fault）、以及 `U64` 之外宽度的**非对齐跨页写**。

4. **打包布局的后果没测**：如果 `Arr[U33]` 的步长是 5 字节、或结构体里 `U33` 字段紧邻，那么「`U33` 写 8 字节」
   会清掉下一个元素的前 3 个字节。步长/字段偏移属 U-L2/U-L4，本轮没验。
7. 每个片段宿主 **rc 都是 2**（`reason=thread_exit`）—— 本观测法的正常收尾，页转储已落盘。

## 9. 与 U-L1 / U-L7 现有记载的关系

- **U-L1 §3「窄类型写只写该宽度的字节数」**：在 `U8` 上成立（=1 字节），本文件把它推广到 `1..32` 位
  （= `ceil(N/8)`），并**否掉**它往 `33..64` 位的自然推广（`U33` 写 8 字节、清 3 个邻居字节）。**不算冲突**，是补了例外。
- **U-L1「不越写邻居」**：对 `U64`/`U8` 成立；`N > 32` 的非整字节宽度（`U33/U36/U40/U48`）**会**清掉后面 2–3 字节 → 需要加一条例外。
- **U-L1「无对齐要求、不向下对齐」**：一致（§4 四个非对齐地址）。
- **U-L7 第 4 条「`store` 的写宽度 = 值类型的字节宽度」**：在 `U32` 上成立（4 字节）。本文件显示它不是一句话能概括的：
  `N ≤ 32` → `ceil(N/8)`；`N > 32` → 8 字节。建议把 U-L7 那句收紧成这两条（U-L7 只测过 `U32`，没测过 `>32` 的非整字节宽度）。
- **U-L7「窄读掩码到 `N` 位」**：完全一致，本文件在 18 个宽度上复现（含 `U33/U40/U48` 这类 `>32` 的）。

## 10. 补测（2026-10-18）：位内高位 / 全量穷举 / `load` 读宽度

器材与地址同 §0/§6；补测脚本在 `%TEMP%/ul-width/`（`bits.py`、`exhaust.py`、`loadw.py`）。

### 10.1 `N < 8` 时同一字节内的高位：**整字节写**

预填 `0xFF`，对 `N = 1..8` 各 `store` 值 `0` 到相邻字节，再用 `U8` 读回整字节：

```
FILL[0:8] = 00 00 00 00 00 00 00 00
```

若高位保留（读-改-写），`N=1..7` 应得 `FE FC F8 F0 E0 C0 80`；实测全 `00`
⇒ **`N < 8` 也是整字节完整写**（值掩码到 `N` 位后写满 1 字节），**不是读-改-写**。
→ codegen 可直接发 `store i8`，不需要 load-modify-store。

### 10.2 `N = 1..64` 穷举：零例外

一页内 64 个 16 字节槽，每宽度写一个模式值（`U*` 预填 `0xFF` 写 `0x55…` 掩码后；`S*` 预填 `0x00` 写 `-1`），
按槽统计"被改动的连续字节数"：

| 写入字节数 | 覆盖的 `N` |
|---|---|
| 1 | 1–8 |
| 2 | 9–16 |
| 3 | 17–24 |
| 4 | 25–32 |
| 8 | **33–64**（32 个宽度全部 8 字节） |

`U*` 与 `S*` **两组结果完全相同**（写宽度与符号性无关；`S*` 只是值按符号规则扩展）。

### 10.3 `load` 的读宽度：与 `store` 对称

把被测地址放在 `FILL` 页最后一字节（`+0xFFF`，值 `0xAA`），紧邻的 `OUT` 页首 8 字节预填 `0xBB`：
若 `load` 真读 8 字节，就会读到邻页的 `BB`。

| `load(<T>)` | 读回 | 实际读 |
|---|---|---|
| `U64` | `0xBBBBBBBBBBBBBBAA` | 8 字节 |
| `U33` | `0x00000001BBBBBBAA` | 8 字节（读 8 再掩码 33 位） |
| `U16` | `0x000000000000BBAA` | 2 |
| `U9` | `0x00000000000001AA` | 2 |
| `U8` | `0x00000000000000AA` | 1 |
| `U1` | `0x0000000000000000` | 1（`0xAA & 1 = 0`，顺带复核掩码） |

⇒ **`load` 与 `store` 的访问宽度规则完全相同**、读写对称；`N < 8` 为整字节读。
