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

### 容忍 `return <expr>)` 里多余的右括号（我们比它严）
实测（2×2 单因子）：`store(...))` 多一个 `)` → 参考 `sysFatal front_end.nim(2688,28) Missing open paren`；
`return …)` 多一个 → **编译成功**，且产物 sha256 与删掉它的同一份源码相同（`f41b2839b25cf6a9…`）。
17 份语料全落在这个被容忍的形状（每份唯一不平衡行都含 `return`、每行净 −1）。
验证：`docs/notes/2026-10-18-m5-gap-diagnosis.md`
最后验证：2026-10-18

## 语义实现选择（参考侧）

### 重载候选：按实参类型挑具体度，与声明序无关
- `random(128)` 选 `random(max: Int)`，**不**选泛型 `random(type: @Type)` —— 即使后者在声明序上靠前。
- 与「人工 `binary +` 多版本时后声明者胜」**不是同一条规则**：那条讲**同签名的人工单元**互相覆盖；
  这条讲**具体类型 / 泛型 / 内建**三类候选之间按具体度比。混成一谈会做错。
验证：`work/tmp/res-type/report.md`（m7/m7b，98/98 都有参考产物）
最后验证：2026-10-25

### `asm` 块**不是原样内联**：参考会把块重新汇编
操作数名换成寄存器/立即数，周围插保存恢复（`mov rcx,32` → `mov rdx,rcx; mov ecx,0x20`），`$result` 解析到该变量所在寄存器；
前缀 `memory_copy` 里内联出 `rep movs BYTE PTR es:[rdi],…`，`mov al,0` 被规范成 `mov eax,0x0`。
⇒ 要支持它得写一个「认识名字操作数」的小汇编器。
**助记符是白名单**（`nop` 被拒：`Unsupported instruction nop.`）；活路径只用得上 7 条：
`mov` / `idiv` / `shl` / `or` / `rdtsc` / `rep movsb` / `rep stosb`，无内存操作数、无标签、无 `define`。
`asm aarch64 { … }` 这类**非目标架构的块整块丢弃且不验证内容**（塞垃圾也照样编过，产物与没有该块相同）。
验证：`docs/notes/2026-10-25-stdlib-survey.md`（探针在 `work/tmp/res-asm/`；编码器见 `codegen/asm_x64.*`）
最后验证：2026-10-25

### def 是**非重入**的：同一个 def 的多次调用共用同一块结果缓冲
`rep(1,1) + rep(2,8) + rep(3,3)` 这样一个字符串里，三段插值**全部渲染成最后一次调用的结果**（`[F][T][T]`）；
`rep(2,8) + rep(1,1)` 连**长度**也来自最后一次（`[[T]|[T]]`）。
但不是「实参被覆盖」：两个**不同** def 不串（`[Ab|Cd]`），返回字面量/`Int` 的调用不串（`idn(5)+idn(9)` → `[5|9]`）；
也不在模板里：`let a=rep(1,1); let b=rep(2,8); set_text(a,…)` 里 a 读出 24B 的 `rep(2,8)`。
我们照此模拟（`template_expr` 把同名 Str 段换成最后一段的 SSA 指针，不重复调用）。
验证：`docs/notes/2026-10-18-uibuffer.md`（19 例探针 `work/tmp/uibuf/probe_ui.py`）
最后验证：2026-10-18

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
验证：`python decompile/extract_nim_strings.py <dll> lits --out FILE`，看文件偏移 `0x1104A0`–`0x1106A0` 段
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

### extern 调用：5 个参数被静默错译；声明里的 DLL 名既不校验也不烘焙
- 产物形态是 `movabs reg, imm64 ; call reg`；`extern` / `call_conv` 声明**不调用时不产任何字节**。
- **声明里的 DLL 名不校验也不烘焙**：9 个名字里 4 个不是 kernel32 导出（`_write` 实际在 ucrtbase），照样编过。
- **5 个参数的调用被静默错译**：第 5 个参数进 `edx`，没有栈传参。
  ⚠️ 判据提醒：我们若按 Win64 ABI 正确地走栈传参，产物行为会与参考**不同** —— 这里「正确」反而是差异。
- `shadow_space` 改 `sub rsp,0x70` → `0x48`；`save_restore` 改 `movabs` 的目标寄存器。
验证：`docs/notes/2026-10-25-stdlib-survey.md` §kernel32（探针 `work/tmp/res-k32/`）
最后验证：2026-10-25

## 运行期不可复现（产物侧）
### 084/096/098：两侧随机序列不同（差异存在，原因待查）
- **084（`campaign/overture_1_registers`，本地译名「Overture 1 寄存器序章」）**：参考的种子与源码无关 —— 源码写 `Seed 42`、
  读回全局确实是 `0x2a`，链却仍按 `xorshift(7)` 走；那个 `7` 是同一 tick 更早的无关代码（时间/节流那段）
  写进栈槽的垃圾值。我们的产物按源码的种子走（同一 seed 两侧逐位相同），抽取序号也相同。
- **096/098（`overture_2_alu` / `overture_3_immediates`，本地译名「Overture 2 ALU 序章」/「Overture 3 立即数序章」）**：
  参考把这枚种子降成**1 字节**栈槽（反汇编里链的输入是 `movzx rcx, byte ptr [rsp-0x8b]`、
  写回是 `mov byte ptr [rsp-0x8b], cl`），tick 1 是未初始化的 `0x07`，之后每 tick 只留低字节 →
  周期 16 的纯乘链；两侧 `random` 调用次数**完全相同**（101 次、实参逐条一致），不是抽多抽少。
- 结论：这三份目前都是**差异存在、原因待查、差分测试暂不可复现**；三份的机制不同（084 是同一个 64 位链、
  种子来源不同；096/098 是种子被降成 1 字节栈槽）。不归因于任何一方的 bug，也不从分母剔除 ——
  未查清前只记为差异。
  细节、复现命令与关卡定名依据见 `../bugs/2026-10-26-084-ref-uninit-stack-seed.md`
  与 `../bugs/2026-10-25-random-stream-offset.md`。
验证：`build/difftrace.exe diff --state … --src … --tick-mark 0x232A7010000`（084）；`work/tmp/rc096/repro.sh`（096/098）
最后验证：2026-10-26
