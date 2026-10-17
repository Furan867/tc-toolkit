/* rt_min.cpp —— 路线 B 运行时的最小一块（够跑通「空程序」这个 spike，不是终态）。
 *
 * 只做两件事：
 *   · `thread_exit()` —— 程序根的收尾（参考实现里是 `kernel32.'ExitThread'(0)`）；
 *   · 一个能被汇编器用 `ExternResolver` 解析的运行期依赖示范。
 *
 * 约定
 * ----
 * 全部 `extern "C"`：装配器按**裸符号名**匹配（`ExitThread`、`thread_exit`…），
 * 一旦编成 C++ 修饰名（`_Z…`）就再也对不上。
 * 未定义的符号（这里是 `ExitThread`）由外部解析 → 汇编器造 `movabs rax,imm64 ; jmp rax` 桩。
 */
#include <windows.h>

extern "C" void thread_exit(void) {
    ExitThread(0);            /* 不返回 */
}
