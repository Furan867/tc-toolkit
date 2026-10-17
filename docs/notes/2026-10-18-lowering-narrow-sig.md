# lowering：窄整数函数签名（表示层 vs ABI 层），006/rv32im 逐字节不变

- 日期：2026-10-18
- 代码：`simplexc/lower/ir_build.py`（本刀只动 `ensure_def_fn` / `ensure_nested_fn` /
  `ensure_prefix_fn` / `finish_function` 的签名类型，加一个 `val_ll`；再顺手把 `e_CondExpr`
  的 phi 类型换成同一个 `val_ll`）
- 判据：006 与 rv32im 的 10 tick 差分仍 0 条、产物 **md5 与改前逐字节相同**；`check.py` 4 PASS；
  窄参数/窄返回的复现件能装配、宿主 3 tick 干净退出（见 §4）
- 状态：`lowering-v0` §7 与 `lowering-tern` §「没做」第 1 条那个缺口**已修**，无回归

## 1. 分界：一个「值」有**三种**宽度，别混

| 层 | 谁用 | 窄整数怎么给 | 代码 |
|---|---|---|---|
| **值表示**（SSA） | 寄存器里的中间值、phi 结果、内部函数签名 | 一律 `i64` 位模式，宽度靠掩码 | `val_ll`（新） |
| **内存宽度** | `alloca` / 全局 / `load`/`store` 的存取宽度 | `i8/i16/i32/i64`（`ceil(bits/8)`） | `ll_type` / `storage_ll` / `storage_bytes` |
| **目标 ABI 宽度** | `get_extern` 声明的外部符号 | 按调用约定（`Sleep` 是 `i32`） | `get_extern` 的 `ll_type` |

改前的 bug 就是把**值表示层**当成了**内存/ABI 层**：`ensure_*_fn` 用 `ll_type(t)` 声明参数与返回，
窄整数于是落到 `i8/i16/i32`，而函数体内所有 SSA 值与 `mask_ll` 的掩码常量都是 `i64` ⇒
`Call parameter type does not match function signature!` / `and i8 …, i64 …` / `ret i64` 对 `i32`
（`VerifyModule` 直接拒）。

**这一刀不加任何掩码指令、不改任何可观测语义** —— 只把三处签名从「内存宽度」换到「值表示宽度」。

## 2. 改了什么（4 处签名 + 1 个 helper）

```python
def val_ll(self, ty):          # 新增，紧挨 ll_type
    return self.ll_type(ty) if ty.kind in ('struct', 'str', 'array') else self.i64
```

`val_ll` 就是 `e_CondExpr` 里 phi 那段写的「值的类型」（`ll_type` if struct/str/array else i64），
现在只有一处定义，三处签名与 phi 共用它：

| 位置 | 改前 | 改后 |
|---|---|---|
| `ensure_def_fn` | 参数/返回 `ll_type(t)` | `val_ll(t)` |
| `ensure_nested_fn` | 参数 `ll_type(t)`、返回 `ll_type`；捕获参数**已经是 `ptr`**，不动 | `val_ll(t)`（捕获仍 `ptr`） |
| `ensure_prefix_fn` | 参数/返回 `ll_type(t)` | `val_ll(t)` |
| `finish_function`（控制流落到末尾补 ret） | `ConstNull(ll_type(ret_ty))` | `ConstNull(val_ll(ret_ty))` |
| `e_CondExpr` 的 phi 结果类型 | 内联同一表达式 | `val_ll(ty)`（语义等价，纯去重） |

**没有动 `get_extern`** —— 它按目标 ABI 给宽度（`Sleep(i32)`、`GetSystemTimePreciseAsFileTime(ptr)`），
是唯一合法的窄签名；转换放在调用点（`_body_sleep` 的 `udiv i64` 之后 `trunc i64 … to i32`）。

映射结果（`val_ll` 的取值）：

| Simplex | 旧签名 | 新签名 |
|---|---|---|
| `U1…U64` / `S1…S64`（含非 2 的幂） | `i8/i16/i32/i64` | **`i64`** |
| `Int`/`U64`/`Time`/`Seed`/`Ptr`/`Enum` | `i64` | `i64`（不变） |
| `Str`/`Arr[T]` | `ptr` | `ptr`（不变） |
| `Struct` | 聚合 | 聚合（不变） |
| `None` | `void` | `void`（不变） |

## 3. 掩码点在哪 —— **全是既有的，本刀没新加**

签名变成 `i64` 之后，进/出函数的掩码依旧走既有路径：

- **参数入口**：`ensure_def_fn`/`ensure_nested_fn` 的
  `store_addr(a, V(GetParam(fnref, i), ty), ty)` —— `store_addr` 内部 `convert(v, ty)` 按
  `ty.bits` 掩码，再 `trunc` 到 `storage_ll(ty)` 落进槽。改前这段就在，只是拿到的 `GetParam`
  是 `i8` ⇒ `and i8 …, i64 255` 直接非法；现在 `GetParam` 是 `i64`，同一段代码自然正确。
- **返回出口**：`s_ReturnStmt` 的 `convert(值, ret_ty)`（U-L7 指示 1 的两处掩码之一在这里）。
- **落到末尾**：`finish_function` 补 `ConstNull(val_ll(ret_ty))` = `i64 0`（任何宽度的位模式零都是 0）。
- **调用点**：`call_user`/`call_nested` 对每个实参 `convert(值, 形参类型)` 后传入，与签名同形。

举例（`wide(a: U8, b: U32) U32`，`work/tmp/narrow/wide_edge/wide_edge.si.ll`）：

```llvm
define internal i64 @wide(i64 %0, i64 %1) #0 {
entry:
  %m  = and i64 %0, 255            ; 参数入口按 U8 掩码
  %t  = trunc i64 %m to i8
  store i8 %t, ptr %a, align 1
  %m1 = and i64 %1, 4294967295     ; 参数入口按 U32 掩码
  ...
t_end:
  %cnd = phi i64 [ %m11, %t_then ], [ %m12, %t_else ]   ; phi 也是 i64
  %m13 = and i64 %cnd, 4294967295  ; 返回出口按 U32 掩码
  ret i64 %m13
}
```

## 4. 判据

| # | 判据 | 命令 | 结果 |
|---|---|---|---|
| 1a | 复现件修好 | `ir_build.py --src work/tmp/tern/tern_edge.si --out work/tmp/narrow/tern_edge.bin` | 改前 `IR 校验失败`（`i8 … call i32` 等 7 类、rc=1）；改后 `blob 952 B / entry 0xc0`、`UNSUPPORTED 0 条` |
| 1b | 窄参数+窄返回片段宿主跑通 | 见下 | `wide_edge.si`（U8→U32 定义 + 三目窄臂）与 `narrow_def.si`（`narrow_mix(a:U8,b:S16) S16`）各 3 tick `ok`、`exited_clean` |
| 2 | 006 不回归 | `ir_build.py --src …006.si --out …006.bin --check --ticks 10` | 10 tick 差异 **0 处**；blob **3508 B / entry 0xa90**，**md5 `9d888a415cc7881de2110311485fbc1f`（与改前逐字节相同）**，`.o md5 dbe0d549ee3bb555…` 也不变 |
| 3 | rv32im 不回归 | `ir_build.py --src …rv32im.si --out …rv32im.bin --check --ticks 10 --ref …rv32im.bin --ref-entry 0x215B` | 10 tick 差异 **0 处**；blob **58612 B / entry 0xe1f0**、md5 `9a79dc6902ef670cea92c44bcd19b933`（与改前逐字节相同） |
| 4 | 不回归 3 | `python simplexc/tools/check.py` | 4 步全 **PASS**（corpus / host_selftest / hook_offline / probe_006） |
| 5 | ABI 边界 | `grep Sleep work/tmp/narrow/006/006.si.ll` | `declare void @Sleep(i32)`；调用点 `%m = trunc i64 %ms to i32` + `call void @Sleep(i32 %m)`，我们的 `sleep` 本体签名是 `define internal void @sleep(i64 %0)` |

判据 1b 的片段（`work/tmp/narrow/`；都是 006 的完整协议骨架 + 注入的窄整数定义/调用点，
因为 `tern_edge.si` 那种纯 lowering 片段缺 `CommandIndex`/`StateIndex`/`settings`，宿主没法驱动 3 tick）：

```text
python simplexc/lower/ir_build.py --src work/tmp/narrow/wide_edge.si   --out work/tmp/narrow/wide_edge.bin   --workdir work/tmp/narrow/wide_edge
python simplexc/tools/host/host.py --src work/tmp/narrow/wide_edge.si  --bin work/tmp/narrow/wide_edge.bin  --entry 0xAE0 --ticks 3 --out …/wide_edge.jsonl
# → 3 行 ok / sim_cycle=1..3 / 收尾 exited_clean
```

## 5. 覆盖 / 没覆盖

**被本刀的判据覆盖到的窄路径**：

- 顶层 `def` 的窄参数（`U8`/`U32`）与窄返回（`U32`/`S16`） —— 判据 1；
- 嵌套 `def` 的窄参数（`U16`）与窄返回（`U8`）、以及控制流落到末尾的窄返回
  （`work/tmp/narrow/nested_narrow.si`，仅装配，未跑宿主）—— 见该文件 IR；
- 三目两臂窄化/加宽（rv32im 差分 0 覆盖）；
- 窄整数传给**内部**函数后在调用点被掩码（`call_user` 的 `convert`）。

**仍未覆盖的「窄整数」路径**（都不在本刀范围内，均为既有/未来面）：

1. `ensure_prefix_fn` 现在**没有**窄参数/窄返回的实例（`get_time`/`sleep`/`memory_clear`/
   `memory_copy`/`thread_exit` 全是 `i64`/`void`/`ptr`）—— 改动对 prefix 函数是**行为恒等**，
   没有窄实例来实测；要造一个得往 `PREFIX_FUNCS` 里加窄签名函数。
2. `get_extern` 的窄 ABI 参数只接了 `Sleep(i32)` 与 `GetSystemTimePreciseAsFileTime(ptr)`；
   将来若有窄整数 extern 参数，调用点必须自己做 i64↔ABI 宽度转换（目前没有通用转换点）。
3. 窄 `enum` 参数：`ll_type(enum)` 本来就是 `i64`，与 `val_ll` 一致，因此本就没踩到，也没单独测。
4. `Struct` 里嵌窄字段再当参数传 —— 走聚合路径，未测。
5. 差分判据仍只比「相对 pre-run 基线**变过的页**」（`lowering-tern` §成立边界那条），
   本刀没有放宽它。

## 6. 没动 / 别误会

- 没放宽任何判据（白名单恒空 `{}`、地址表校验照旧）。
- 没动 `get_extern`、没动 `tools/**`、没动游戏目录。
- 工作区里 `simplexc/tools/host/host.py` / `selftest.py` 有**别人**的未提交改动（git status 可见），
  不是本刀产物；本刀的 diff 只有 `simplexc/lower/ir_build.py`。
