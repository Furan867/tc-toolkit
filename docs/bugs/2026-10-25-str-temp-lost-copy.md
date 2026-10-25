- 状态：未修
- 影响：`corpus/src/069.si`、`080.si`（两份都跑不起来）；构造上是「同一表达式里做第二次字符串拼接」，069 是
  `get_padded_number` 里 `s = " " + s`，080 是 `get_input` 里 `` `[left]`+`…`+`[/left]` `` 的插值拼接
- 现象：跑到 `0x40800000`（= `kStackBase + kStackSize`，栈区上界外一页）就死。069 报
  `未映射读=0x40800000`（Unicorn 挂了 `UC_HOOK_MEM_READ_UNMAPPED` 后的返回码是 `UC_ERR_MAP`），
  080 报 `stub_error: memcpy 读源 … UC_ERR_READ_UNMAPPED`。我们的产物在 **tick 0 内**就死
  （`sim_cycle` 仍是 -1）
- 判据：`python simplexc/tools/m5/batch.py --with-prefix --jobs 12 --ticks 100` 的 `host_fail` 归 0
  （现在是 069 / 080 两份）
- 已确认：
  - 069 出错指令 `pc=0x100015b5`：`movups 0x10(%rcx,%r15,1),%xmm1`（LLVM 把 `__simplexc_copy`
    向量化成 32 B/轮）。当时 `rsp=0x407fbde0 rcx=0x407ffde8 r15=0x200` → 读 `[0x407ffff8, +16)`，
    后 8 字节出界命中 `0x40800000`。
  - 长度从哪来：`0x10001245`：`mov -0x8(%rax),%r8; shr $0x10` —— **Str 头字右移 16 位**。
    080 的 `0x10001905` 是同一算法（`mov -0x8(%rax),%rsi; shr $0x10`）。
  - 头字的实际值：069 是 `[0x407ffde0]=0x407ffdf9` → `shr 16` = `0x407f` = **16511**；
    080 是 `[src-8]=0x407ffcd8` → 同样 `0x407f`。**都是栈指针，不是长度**。
  - 为什么头字成了栈指针（069 实证）：第二次拼接（`s + " "`）在产物里**只剩 alloca**
    （`0x1000176a..0x10001785`：`shr; add 0x18; and -16; call ___chkstk_ms; sub rax,rsp;
    lea 8(rsp),%rdx; ret`）—— 两段 copy 和头字 store **都没了**；而 `069.ll` 的 `then19` 块里
    它们**在**。窗口写日志显示 `pc=0x100017c0 addr=0x407ffde0 value=0x407ffdf9`
    （`___chkstk_ms` 的 `push %rcx`，`rcx` = 上次 memcpy 的 dst），此后 `0x407ffde0` 再没被写过。
    第一次拼接因为头字 store 在 alloca 之后，正好把同样的污染盖掉了。
  - **是我们的 bug**：`corpus/ref/069.bin`、`080.bin` 在 `--target 3`（同 tick 数）与 `--target 100`
    都 `running_zero/ok`；我们的产物 tick 0 内就死。
- 已排除：
  - 不是栈溢出：069 出错时 `rsp` 只在栈顶下 16 KB。
  - 不是栈映射范围（`kStackSize`）：加大栈只把崩溃点推后，参考产物同 tick 数下正常。
  - 不是「重复映射」：080 报的是 `UC_ERR_READ_UNMAPPED`，069 的 `UC_ERR_MAP` 是 Unicorn 挂了
    未映射读的 hook 之后的返回码，两者都不是 re-map。
  - 不是 `difftrace` 的问题：两份产物在 `--target 3` 与 `--target 100` 下都在 tick 0 内死。
- 下一步：先定位 O2 里**哪个 pass** 把第二次拼接的 copy + 头字 store 删掉的（`.ll` 里有、产物里没有）。
  同一处还要一并看设计层面的问题：`Str` 返回「指向自己 dead frame 的指针」这件事本身靠侥幸成立
  （第一次拼接能对，只是因为头字 store 恰好排在 alloca 之后把污染盖住了）。
- 复现：`python simplexc/tools/build.py simplexc_cli && python simplexc/tools/m5/batch.py --with-prefix --jobs 2 --ticks 3 --only 069,080`
  （`--only` 若不存在，跑全量再看 069/080 两行）
