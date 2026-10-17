/* rt_min.cc —— 路线 B 运行时的最小一块（够跑通「空程序」这个 spike，不是终态）。
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

/* spike 专用的可观测副作用：让我们能确认「自己的 blob 在游戏进程里真的跑到了这里」。
 *
 * 为什么要自己拼字符串：这个目标文件会被链进 blob，任何 CRT 符号（`sprintf` 之类）
 * 都是 `ExternResolver` 解析不了的未定义符号 —— 只能用 kernel32 + 纯算数。
 * 正式实现里不会有它。
 */
#define SPIKE_LOG "D:\\home\\code-proj\\Agent\\proj\\tc\\reversing\\work\\tmp\\blob_ran.log"

extern "C" unsigned long long tc_spike_mark(unsigned long long tag) {
    char line[48];
    int n = 0;
    const char* prefix = "blob ran, tag=";
    while (*prefix) line[n++] = *prefix++;
    char digits[24];
    int d = 0;
    unsigned long long t = tag;
    if (t == 0) digits[d++] = '0';
    while (t) { digits[d++] = char('0' + (t % 10)); t /= 10; }
    while (d) line[n++] = digits[--d];
    line[n++] = '\r';
    line[n++] = '\n';

    HANDLE h = CreateFileA(SPIKE_LOG, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, line, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
    return ok ? (unsigned long long)wrote : 0;
}
