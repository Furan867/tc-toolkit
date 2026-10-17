# lowering：三目 + 空 struct（rv32im 缺口清零，装配 + 10 tick 差分 0）

- 日期：2026-10-18
- 代码：`simplexc/lower/ir_build.py`（本刀只动 `e_CondExpr`（新增）、`_scan_type`、`_make_types`）
- 判据：见 §5（四条 + 一条额外的差分判据）
- 状态：**rv32im 的 unsupported 250 → 0，blob 装出来了，而且与参考产物 10 tick 差异 0 处**

## 0. 一句话

`lowering v0` 的 §8「下一步」第 1、2 条（三目、空 struct）做完，rv32im 的 250 条 unsupported
**全是这两条的连锁**，清零；顺带发现 rv32im 能装配且逐 tick 等价的判据**已经能跑**。

## 1. 三目的 IR 形状：`br` + `phi`

**不是 `select`。** 判据是语义：三目两臂里都可能有 `load(…)`（rv32im 的 `com_mux` 两臂就是
两支 `load`），`select` 两个操作数都会求值 —— 那与短路语义（只有被选中那支求值）不一致。
所以形状跟 `if` 语句同构：

```text
  %c668 = icmp ne i64 …, 0
  br i1 %c668, label %t_then, label %t_else

t_then:                       ; 只有这一支被执行
  %ld669 = load i32, ptr inttoptr (i64 1686710845924 to ptr), align 1
  %m675  = and i64 %m671, 4294967295
  br label %t_end

t_else:
  %ld672 = load i32, ptr inttoptr (i64 1686710845916 to ptr), align 1
  %m676  = and i64 %m674, 4294967295
  br label %t_end

t_end:
  %cnd = phi i64 [ %m675, %t_then ], [ %m676, %t_else ]
```

三个实现要点（都是踩过才知道的）：

1. **转换必须在各自的块里发**。类型统一要先拿到两臂的类型，而 `convert` 会把掩码 /
   `inttoptr` 发进**当前块**。所以顺序是「两臂各自求值 → `unify` → 回到各臂块尾补 `convert`
   → 两支 `br` 到 `t_end`」；先 `br` 再转换会把掩码指令发到错误（甚至是别人的）块里。
2. **phi 的结果类型是「值的类型」不是 `ll_type`**。`ll_type` 给的是**内存宽度**（U32 → i32），
   而本 lowering 的 SSA 值一律 i64 位模式 —— 第一版用 `ll_type(ty)` 建 phi，整个 rv32im 报
   38 处 `PHI node operands are not the same type as the result` / `and i32 …, i64 …`。
   现在是 `ll_type(ty) if ty.kind in ('struct','str','array') else i64`。
3. 类型与掩码**没有新规则**：`unify(两臂类型)` → `convert(臂, 统一类型)`，与 if/`binop` 同一套
   （U-L7 指示 1 的两处掩码之一就在 `convert` 里）。

## 2. 空 struct 的表示：**1 字节占位**

`type X Struct { }` → `Ty('struct', bits=8, fields=())`，LLVM 类型 `packed { i8 }`，
顶层 `var level_input = Input {}` 落成 `@level_input = internal global <{ i8 }> zeroinitializer`。

选 1 字节而不是零宽度聚合（`{}`）的理由：零宽度类型会让 `alloca` / 全局的大小是 0，
这条路在本 lowering 里**没验证过**（`alloca` 全插在 entry、全局走 `.bss`），而 1 字节只是白占
1 字节却保住「对象有地址」；`storage_bytes` / 字段偏移 / `ConstNull` 三条既有路径都不用特判。
**代价**：U-L4（struct 布局）本来就没定，这等于替 0 字节这一档先选了一个值；将来若拿到
参考实现的布局证据，改的是一个数字（`_scan_type` 里那一处）。

连带解锁的东西（**不用额外改动**，原代码已经具备）：

- `composite_literal`（`Input {}`）原本就返回 `ConstNull(结构类型)` → 空 struct 直接走通；
- `_make_globals` 的「初始化式是编译期常量就直接写 initializer」路径接住了顶层 `var`。

## 3. rv32im 的 250 → 0

| 阶段 | `--list-unsupported` | 说明 |
| --- | --- | --- |
| 改前（基线） | **250** | 38 三目 + 2 空 struct + 2 顶层 `var`（因空 struct 建不出类型）+ 208 连锁（`let x = cond ? …` 失败 → 后面用 `x` 全报未定义） |
| 加 `e_CondExpr` + 空 struct 后 | **0** | 350 万条里的连锁全自然消失，**没有放宽任何判据**（没开 page 白名单、没把 unsupported 改成静默跳过） |

剩下 3 条 `WARNING`（`get_command` / `get_setting` / `mode_run$get_target_cycle` 控制流落到
末尾补 ret）**改前就在**，不是本刀引入的。

## 4. 装配结果

```text
[lower] IR 1379416 B（函数 14 个）→ work\tmp\tern\rv32im\rv32im.si.ll
[lower] COFF 59047 B → work\tmp\tern\rv32im.bin.o
[lower] blob 58612 B → work\tmp\tern\rv32im.bin（entry=0xe1f0）
```

（参考产物 `corpus/rv32im/rv32im.bin` 是 219088 B / entry 0x215B。我们小是预期的：参考产物没优化。）

14 个可达函数（`grep '^define\|^declare'`）：`tc_main` / `reset_sim` / `set_error` /
`get_command` / `set_setting` / `get_setting` / `memory_clear` / `memory_copy` / `mode_refresh` /
`mode_run` / `sleep` / `get_time` / `mode_run$halt#L6523` / `mode_run$get_target_cycle#L10262`。

## 5. 判据（四条 + 一条额外的）

1. **不回归**：`006.si` 重编 → 10 tick 差异 **0 处**；`blob` 3508 B / entry `0xa90` / md5
   `9d888a415cc7881de2110311485fbc1f` —— **与改动前逐字节相同**（IR 25561 B、COFF 3617 B 也没变）。
2. **缺口清零**：`--list-unsupported` = **0 条**（退出码 0）。
3. **能装配**：exit 0，blob 58612 B / entry `0xe1f0`（见 §4）。
4. **不回归 2**：`python simplexc/tools/check.py` → `corpus` / `host_selftest` / `hook_offline` /
   `probe_006` 4 步 **全 PASS**。
5. **额外（本刀没要求，但拿到了）**：rv32im 与参考产物做逐 tick 差分 →
   `10/10 tick · 逐 tick 全等=是 · 差异 0 处`（`--check --ticks 10 --ref corpus/rv32im/rv32im.bin
   --ref-entry 0x215B`）。宿主 rc 两端都是 0、`alive=True`、`sim_cycle` 1..10、`extern` 0 条。

### 这条差分判据的**效力边界**（别当成「全等」的全面证明）

- 宿主比的是「**相对 pre-run 基线变过的页**」（`host._changed_pages`，整页 sha256）。
  rv32im 每 tick 变 **3 页**：`commands`/`settings` 那两页（协议）+ `#SIMULATION_STATE` 的
  **第 1 页**（532 个组件的状态值，~2 KB 恰好一页）。`#DATA_*`（128 MB 那几段）在这 10 tick 里
  没被写过 —— `reset_sim` 那 5120 字节拷贝写进去的值与种子值相同，所以不进「变过的页」。
  ⇒ 全等约束的是**组件状态页 + 协议字段**；RAM 镜像的等价性**没有**被这条判据覆盖。
- 比较器不是空跑：负控 —— 把参考 jsonl 里一个页哈希改成别的 → 立刻报 1 处差异；
  改 `sim_cycle` → 报 1 处字段差异（`compare_ticks` 的白名单恒为空）。

## 6. 离「rv32im 逐 tick 全等」还差什么

**主判据这 10 tick 已经过了**（见 §5.5）。剩下的是这条判据**照不到**的地方：

1. **窄整数函数签名**（新发现，**本刀没改**）。`ensure_def_fn` / `ensure_nested_fn` /
   `ensure_prefix_fn` 用 `ll_type(t)` 声明参数与返回类型 → 窄整数给 i8/i16/i32，而 SSA 值与
   `mask_ll` 一律 i64 ⇒ 只要某个**可达 def 带窄整数参数或窄整数返回**，`VerifyModule` 立刻报
   `Call parameter type does not match function signature!` / `ret i64` 对 `i32`。
   rv32im 的 14 个可达函数参数/返回**全是 i64 或 ptr**，所以踩不到（实测装配通过、10 tick 全等）。
   复现：`work/tmp/tern/tern_edge.si`（`def wide(a: U8, b: U32) U32`）→ rc=1，7 类验错。
   修法方向：内部函数的签名用「值的类型」（int → i64，struct/str/array 走 `ll_type`），
   与 `get_extern` 的**目标 ABI 宽度**分开（后者本来就该是 i32，例如 `Sleep(DWORD)`）。
2. **RAM 镜像 / 输入激励没有覆盖**。上面 §5 的边界：`#DATA_*` 没变过页，等于没验。
   要证 RAM 就不能只信「变过的页」，得让宿主导出整段镜像比对，或换一个会真的跑指令的激励。
3. `static_type` 仍然不认 `CondExpr`（本刀试过加，rv32im 的 IR / blob **逐字节不变** ⇒ 对
   rv32im 是死代码，按「别加投机脚手架」删了）。后果：**三目当实参传给重载 def / `min`/`max`
   时**会落到 `static_type` 返回 None → 报「重载解析拿不到参数类型」。rv32im 里没有这种用法。
4. 其余未定项照旧（`v0` 笔记 §7）：U-L4 布局、U-L5 enum 宽度、U-L10 字符串逃逸、
   闭包捕获值/引用语义、`.ms()` 等）。

## 7. 代码改动清单（3 处，都在 `simplexc/lower/ir_build.py`）

| 位置 | 改动 |
| --- | --- |
| `Lower.e_CondExpr`（新增，`e_BinaryExpr` 之后） | 三目：condbr + 两臂各自求值/转换 + phi |
| `Program._scan_type`（struct 分支） | 空 struct 不再 `Unsupported`，给 `bits=8` / 无字段 |
| `Program._make_types` | 字段为空时用 `Ty.bits` 那个占位宽度建 `{ i8 }` |

另外**验证过但没留**的一处：`static_type` 加 `CondExpr` 分支 —— 对 rv32im 的 IR / blob
逐字节无影响，删掉了（见 §6.3）。

## 8. 建议折叠进账本（留给主程裁决，本文件不改 `facts.md` / `status.md`）

- `facts.md`（可验证的长期结论）：
  1. rv32im 的 250 条 unsupported 根因只有「三目 + 空 struct」，清掉后**连锁全消**（判据命令同 §5.2）。
  2. 空 struct 我们取 **1 字节占位** `packed { i8 }`（U-L4 未定，本刀的选择）。
  3. 我们的 rv32im 产物与参考产物 **10 tick 逐 tick 全等（差异 0 处）**；`blob` 58612 B /
     entry `0xe1f0`，参考 219088 B / entry `0x215B`。**验证：** `ir_build.py --check --ticks 10
     --ref simplexc/corpus/rv32im/rv32im.bin --ref-entry 0x215B`。
  4. 差分判据的覆盖面 = 「相对 pre-run 基线变过的页」；rv32im 每 tick 只有 3 页（协议 2 + 状态 1）。
  5. **窄整数函数签名是未修的缺口**（rv32im 踩不到，见 §6.1）。
- `pitfalls.md`（一行一条）：
  - 别用 `ll_type` 给 SSA 值/phi 定类型 —— 它是**内存宽度**，窄整数会给 i8/i16/i32，而值一律 i64。
  - 别在发出 `br` 之后才做两臂的 `convert` —— `convert` 往**当前块**发指令，会发到别的分支里。
  - 别把「变过的页相同」当成「内存全等」—— 只覆盖相对基线变过的页，RAM 镜像照不到。
