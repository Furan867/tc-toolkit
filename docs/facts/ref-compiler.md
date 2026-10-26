# 事实：原版编译器 `compile_real.dll` 的行为

**这个文件管什么**：原版编译器**自己的**行为 —— 它接受/拒绝什么源码、怎么诊断、产物怎么编码、布局与绑定。

- 语言**运行期**语义（位宽、掩码、求值顺序、`random` 递推…）→ `semantics.md`
- 转发层与游戏侧契约（调用契约、Result 字段、bit62、JIT 流程）→ `forwarding.md`
- 与我们的差异、待取证项 → `../bugs/`

纪律同 `docs/facts.md`：就地改写、不留旧值；每条带「验证：」与「最后验证：」。

## 源码接受 / 诊断

### 未调用的顶层 def 被丢弃（DCE），连语法错也不检查
- 把 031 里 31 个从未被调用的 `def` 整段删掉再编，参考产物 35000 B → 35000 B，`cmp -l` 只差 **4 字节**。
- `def unused() None { let x = ) }` + 一个正常根 → 参考**编过**（8604 B、无错误）；我们同样不报。
  所以「静默丢弃不可达 def」是参考既有行为；设计文档 §3.4 要消除的静默降级是我们自己的目标，不是偏差。
验证：`work/tmp/rc031/refc.py`（原始 `compile_real.dll` 离线编）+ `probe3_badsyntax.si` / `ref_031_nodefs.bin`
最后验证：2026-10-26

### 同名 def 压过内建
`def max(a: Int, b: Int) Int { return 999 }` + 根里 `max(1, 2)` → 参考产物里是 `mov $0x3e7`（999），
即**用户 def 优先**；我们一致。构造这种探针时形参类型必须与实参匹配，否则选内建是正确行为（踩过一次）。
验证：`work/tmp/rc031/refc.py probe4_shadow.si` + `objdump -D -b binary -m i386:x86-64`
最后验证：2026-10-26

### 裸顶层语句：接受
`store(...)` 直接写在顶层，参考产出 `+13` 字节；我们报「顶层这一行既不是声明也不是语句（未支持）」。
98 份语料里没有这种形态 —— 顶层行首 token 只有 `def` / `}` / `var` / `type` / `const` / `run_sim:`，
根的形态固定为 `run_sim: while true { … }`。所以不进 bugs（不是语料缺口），但**是我们的形态差异**。
验证：语料普查 `cd simplexc/corpus/src && grep -h '^[^[:space:]]' *.si | sed 's/[[:space:]].*//' | sort | uniq -c`；
      参考侧 `work/tmp/rc031/probe1_badsyntax.si`（8599 B）
最后验证：2026-10-26

## 产物体量与编码

### 空片段固定 8581 B 桩；体积差可以是编码密度，不是漏编
- 参考对空源码 / 只有顶层 `var` 的源码，产物固定 **8581 B**。
- 031：我们 3404 B、参考 35000 B，但**函数集合相同**。同一段 `mode_refresh`（282 次 `store()`）：
  参考占 22351 B（清空该体 35000 → 12649），我们占 1296 B（3404 → 2108）。
- ⇒ 小片段比体积是无效判据（`pitfalls.md` 同名条目）。
验证：`work/tmp/rc031/ref_empty.bin` / `ref_031.bin` / `ref_031_mr_empty.bin` / `A.bin` / `MR.bin`
最后验证：2026-10-26

## 产物布局与绑定
### 产物的绝对地址绑定 compile_real.dll 的加载基址
参考产物里的 `movabs r64,imm64` 全是 image 空间地址，落在 **compile_real.dll 自己的镜像**里：
会话里 DLL 加载在 `0x7FF9A0DE0000`，产物里的 `0x7FF9A0DE1310` = DLL 内 RVA `0x1310`，字节是
`48 FF 25 59 70 14 00` = `jmp QWORD PTR [rip+0x147059]`（目标 RVA `0x14836F`，在 `.idata`）→ **导入 thunk**；
另有一批指向 DLL `.bss` 槽位（如 base + `0x131280`）。语义上仍是调 kernel32，但**绑定的是 DLL 的加载基址**。
→ **产物跨 boot 不能逐字节相等**（基址随 boot 变、同一 boot 内跨进程恒定）；离线重编译只能做到
「两边都归一化到 DLL 镜像偏移后逐字节相同」。
验证：`python tools/classify_abs_addrs.py`（VirtualQuery/GetMappedFileNameW）、
      `objdump -D -b binary -m i386:x86-64 -M intel --start-address=0x910 compile_real.dll`、
      模块基址枚举（反例：本 boot 的 `KERNEL32.DLL` 在 `0x7FFA…` 段，产物里的 `0x7FF9…` 不在其中）
最后验证：2026-10-16

### 数组/字符串布局与 Nim seq 同构
`Arr[T]` 的载荷指针 `p`：`p[-8] = (len << 16) | cap_log2`（len 是 48 位，低 16 位是 log2 容量），
`len = u64(p-8) >> 16`，容量字节 = `(1 << u16(p-2)) >> 1`；空数组用 `empty_array_address()` 哨兵。
必须同构的理由：游戏把自己的 seq/字符串按硬编码绝对地址交给生成代码
（`raw_cast(<Arr[U64]>, 2416573480960)`、`#SIMULATION_STATE`）。
验证：读 stdlib 前缀的 `allocate_raw` / `get_length` / `set_length` / `get_capacity`
最后验证：2026-10-16

### 内建/宏名表就在 DLL 的 .rdata 字面量池里
`INTRINSIC_{STANDARD,UNARY,BINARY,BINARY_MUTATING}_NAMES` 是 DLL 里的全局（符号与地址见
`recovered/compiler-src/OO/types.nim.stub:714-717,769-772`），名字本身作为 Nim 字符串字面量按**声明顺序**
紧邻排列在 `.rdata`；`mac_*` 组共 17 个（`mac_load`/`mac_store`/`mac_raw_cast`/`mac_size`/…）。
→ 内建清单（主设计 §11 U5）的权威来源是这里，不是从前缀反推。
验证：`python tools/extract_nim_strings.py <dll> lits --out FILE`，看文件偏移 `0x1104A0`–`0x1106A0` 段
最后验证：2026-10-16

### stdlib 里 `kernel32.'Name'` 的真实绑定：三类，只有两类要名字映射

参考产物里这些调用编成 `movabs reg, imm64` + `call reg`。**算法**：拿 `imm64` 减去该产物里
`compile_real.dll` 的加载基址得 RVA。98/98 份参考产物五个 RVA 完全相同；另一次 boot 的捕获
（基址不同、`imm64` 全变）RVA 一个不差 —— 所以下面的表不是噪声。

| stdlib 里的名字 | RVA | 落在 | 最终绑定 | 该怎么办 |
| --- | --- | --- | --- | --- |
| `_virtual_alloc` | `0x1310` | `.text` 导入 thunk（`48 FF 25 …` → IAT `0x148370`） | `KERNEL32!VirtualAlloc` | 名字映射 |
| `_virtual_free` | `0x1320` | 同类 thunk（IAT `0x148378`） | `KERNEL32!VirtualFree` | 名字映射 |
| `_get_system_time_precise_as_file_time` | `0x1350` | **DLL 自己的代码**：无参 → 64 位 FILETIME 的包装（`sub rsp,0x38; lea rcx,[rsp+0x28]; call [IAT 0x148340]; mov rax,[rsp+0x28]; ret`） | 包装二跳 → `KERNEL32!GetSystemTimePreciseAsFileTime` | **不要映射**，要语义垫片 |

阳性对照（证明「thunk vs DLL 代码」这个区分不是猜的）：`Sleep` → RVA `0x1370`、thunk；
`ExitThread` → RVA `0x1380`、DLL 包装。两种形态都能识别出来。

**推论**：`sema/analyze.cc` 的 `extern_sigs()` + `HExternShape::OutParam64`（垫片在
`codegen.cc`）已经就是这个形态 —— 前两个走名字映射，第三个走「无参 → 输出 64 位」的垫片，
不必改。唯一可议：`_virtual_free` 我们现在声明成 `void`，而参考那个槽是裸 thunk、`eax` 带 BOOL；
目前没人读它的返回值，所以不改。

⇒ 「这三个名字在 kernel32 / ucrtbase / msvcrt 的**导出表**里都查不到」不等于「缺功能」：
它们本来就是 DLL 内部的转发层，查导出表当然查不到。
验证：探针与原始输出在 `work/tmp/k32probe/`（临时目录）；方法就是本节的「`imm64` 减加载基址」；
`ctypes` 查导出表那版见 `work/tmp/res-k32/report.md`
最后验证：2026-10-25
## 运行期不可复现（产物侧）

### 084/096/098：产物拿未初始化的栈值当随机种子
- **084（`campaign/overture_1_registers`，本地译名「寄存器序章」）**：参考的种子与源码无关 —— 源码写 `Seed 42`、
  读回全局确实是 `0x2a`，链却仍按 `xorshift(7)` 走；那个 `7` 是同一 tick 更早的无关代码（时间/节流那段）
  写进栈槽的垃圾值。我们的产物按源码的种子走（同一 seed 两侧逐位相同），抽取序号也相同。
- **096/098（`overture_2_alu` / `overture_3_immediates`，本地译名「ALU 序章」/「立即数序章」）**：
  参考把这枚种子降成**1 字节**栈槽（反汇编里链的输入是 `movzx rcx, byte ptr [rsp-0x8b]`、
  写回是 `mov byte ptr [rsp-0x8b], cl`），tick 1 是未初始化的 `0x07`，之后每 tick 只留低字节 →
  周期 16 的纯乘链；两侧 `random` 调用次数**完全相同**（101 次、实参逐条一致），不是抽多抽少。
- 结论：这三份的「逐 tick 等价」不可达（参考换一个栈初值就换一条序列），单列标 `known-ref-unreproducible`。
  细节、复现命令与关卡定名依据见 `../bugs/2026-10-26-084-ref-uninit-stack-seed.md`
  与 `../bugs/2026-10-25-random-stream-offset.md`。
验证：`build/difftrace.exe diff --state … --src … --tick-mark 0x232A7010000`（084）；`work/tmp/rc096/repro.sh`（096/098）
最后验证：2026-10-26
