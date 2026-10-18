/* entry_shim.S —— 游戏 jmp 进来的最外层入口（主设计 §5 第 8 条）。
 *
 * 为什么必须自己手写
 * ------------------
 * 游戏侧是 `lea (%rsi,%rdi,1),%rax; jmp *%rax` —— **不是 call**，所以：
 *   · 没有任何寄存器被清零（进来时 rdi=entry、rsi=dest、rbx=Result*）；
 *   · 栈**不保证** 16 字节对齐，而 LLVM 生成的代码按 ABI 假设对齐（对齐的 SSE 会 fault）。
 *
 * 这里只做两件事：把栈对齐到 16、给被调者留下影子空间，然后 call 程序根。
 * **不保存寄存器**：本入口永不返回（程序根末尾是 thread_exit），游戏也不会再来读这些寄存器；
 * 真需要保存时再加，别为想象中的需求写代码。
 *
 * `___chkstk_ms` 也放这里：LLVM 给 >4 KiB 栈帧的自动探栈调用它（实测见 facts.md §构建与 LLVM），
 * 它是 CRT 符号、我们不给就链接不上。语义：rax = 要分配的字节数，逐页 touch 一遍、
 * **不许改 rsp、不许破坏任何寄存器**。
 *
 * 这个「不许破坏任何寄存器」不是客套 —— 动态 alloca（运行期尺寸）的那次 `___chkstk_ms` 是 LLVM 在
 * **寄存器分配之后**才插进函数中段的，它插不出 spill 来兜底，所以跨这次 call 的活值只能指望
 * 探栈函数自己保住。原先的走查用 %r10（libgcc 的实现就是这个形状），实测**真崩**：LLVM 把长度
 * 留在 %r10 里跨 call 用（`movabs $-32,%r15` 前后的 `cmp $0x200000,%r10`），%r10 被我们改成
 * 调用者的 rsp 之后那个比较恒真 → 走大块 memcpy 路径而循环计数是 0 → 32 字节 store 一路往上写，
 * 撞到线程栈顶那页 AV（001/047…054/124 九份语料的 host_fail 都是这一条）。
 * 所以走查指针改用**已经压栈保存过的 %rcx**，函数只动 flags。
 */
        .text

        .globl  __tc_entry
        .p2align 4
__tc_entry:
        andq    $-16, %rsp              /* 对齐到 16：进来时是 jmp，不保证 ABI 对齐 */
        subq    $32, %rsp               /* 影子空间；call 时 rsp ≡ 0 (mod 16) */
        call    tc_main
        xorl    %ecx, %ecx              /* 兜底：程序根不该返回 */
        call    thread_exit
        hlt

        .globl  ___chkstk_ms
        .p2align 4
___chkstk_ms:
        pushq   %rcx
        pushq   %rax
        cmpq    $0x1000, %rax
        leaq    24(%rsp), %rcx            /* 两次 push 后 rsp+24 = 调用者的 rsp */
        jb      2f
1:      subq    $0x1000, %rcx            /* 逐页踩过去，让守卫页按序触发 */
        testl   $0, (%rcx)
        subq    $0x1000, %rax
        cmpq    $0x1000, %rax
        ja      1b
2:      popq    %rax
        popq    %rcx
        ret
