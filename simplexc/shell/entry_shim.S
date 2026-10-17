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
        leaq    24(%rsp), %r10
        jb      2f
1:      subq    $0x1000, %r10            /* 逐页踩过去，让守卫页按序触发 */
        testl   $0, (%r10)
        subq    $0x1000, %rax
        cmpq    $0x1000, %rax
        ja      1b
2:      popq    %rax
        popq    %rcx
        ret
