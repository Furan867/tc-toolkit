/* codegen/asm_x64.hh —— stdlib `asm x86_64 { … }` 块的**编码器**（纯函数，不进编译流水线）。
 *
 * 边界：只做 `work/tmp/res-asm/report.md` §6 数出来的活代码白名单 —— 7 个助记符
 * （`mov` / `idiv` / `shl` / `or` / `rdtsc` / `rep movsb` / `rep stosb`），
 * 没有内存操作数（`[ … ]`）、没有标签、没有 `define`、没有 os 标签。其余一切报错，不猜。
 *
 * 为什么是「重新汇编」而不是原样粘字节：参考编译器把块里的**寄存器名当可变源变量**，
 * 进分配器后 SSA 化并插 version glue（`work/tmp/res-asm/report.md` §3.1 的实测：
 * `mov rcx,32` 前后多出 `mov rdx,rcx` / `mov rbx,rdx`）。所以能一次编完的只是**单条指令**，
 * glue 与寄存器分配是调用方的事 —— 本模块因此只对「一条源指令」负责。
 *
 * 编码口径全部来自参考产物里的字节（出处写在 `asm_x64.cc` 每张表旁边），不来自「x86 手册上
 * 应该怎么写」：参考对 `mov reg, imm` 一律发 32 位形式（`mov rcx,32` → `b9 20 00 00 00`，
 * 连 8 位的 `mov al,0` 也发 `b8 00 00 00 00`），照抄它才能逐字节对齐。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace simplexc {
namespace codegen {
namespace asm_x64 {

/* 源里的一行指令：助记符 + 操作数（都保持源里的字符串形态，不预先解释）。
 * `rep movsb` / `rep stosb` 这种两词形态整体作为助记符（`mnemonic = "rep movsb"`），
 * 因为参考的白名单表里它们就是一条（report §3.2）。 */
struct Inst {
    std::string mnemonic;
    std::vector<std::string> ops;
};

/* `$name` 操作数 = 源里的一个**局部变量**（`get_cycle_count` 的 `mov $result, rax`）。
 * 偏移由调用方给：帧布局不属于编码器，编码器只把它变成一次 rsp 相对的访存
 * （`mov [rsp+off], reg` / `mov reg, [rsp+off]`）。 */
struct Slot {
    std::string name;     /* 不带 `$` 的源码名 */
    std::int32_t offset;  /* 相对 rsp 的字节偏移（可负） */
};

/* 成功则 `bytes` 是这条指令的机器码、`error` 为空；失败则 `bytes` 为空、`error` 是一句人话。
 * 不用异常：失败是**预期结果**（白名单外/操作数不认识），调用方要能逐条收集而不是中途跳出。 */
struct Result {
    bool ok = false;
    std::vector<std::uint8_t> bytes;
    std::string error;
};

/* 编码一条指令。`slots` 只在该指令用到 `$name` 时被查（找不到同名槽 = 失败）。 */
Result encode(const Inst& inst, const std::vector<Slot>& slots);

/* 只判助记符在不在白名单里（不含操作数检查）。给「整块能不能过」的粗筛与单测用。 */
bool mnemonic_supported(const std::string& mnemonic);

/* 操作数是不是硬件寄存器名（`rax` / `eax` / `al` / `r8b` 三种拼写都算）。
 * 为什么要问编码器而不在调用方再拄一张表：能编的寄存器集合与「什么是寄存器」必须是同一份
 * 数据，两处各摆一份必然分叉（一边认 `r8`、一边不认）。 */
bool is_register(const std::string& name);

/* 寄存器名的**父名**（`al` / `eax` / `rax` 都 → `rax`，`cl` → `rcx`）。不是寄存器 → 空串。
 * 用途：inline asm 的 clobber 只能写父名。 */
std::string reg_parent(const std::string& name);

/* 寄存器名的 32 位拼写（`rdi` / `dil` / `edi` 都 → `edi`）。不是寄存器 → 空串。
 * 用途：参考对 `mov rdi, <常量>` 发的是 32 位形式（`bf imm32`），常量装载要逐字节对齐它的
 * 前提就是这里能拿到 32 位名。 */
std::string reg32_name(const std::string& name);

}  // namespace asm_x64
}  // namespace codegen
}  // namespace simplexc
