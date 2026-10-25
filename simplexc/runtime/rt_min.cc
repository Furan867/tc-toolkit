/* rt_min.cc —— 路线 B 运行时的最小一块（够跑通「空程序」这个 spike，不是终态）。
 *
 * 只做三件事：
 *   · `thread_exit()` —— 程序根的收尾（原版编译器里是 `kernel32.'ExitThread'(0)`）；
 *   · `tc_array_alloc()` —— `array(<T>, n)` 的运行期分配；
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

/* `array(<T>, n)`：载荷前 8 字节的头是 `n << 16`（`.len()` 读的就是它），载荷零初始化、
 * 元素按 U-L1 字节步长紧密排列。**每次求值都新分配**：098 的 `array(<U8>, 256)` 在 `run_sim`
 * 的 while 循环体里，做成调用点的私有全局会让第二轮拿到上一轮写过的残留。
 *
 * 为什么走 `VirtualAlloc` 而不是 CRT 的 `malloc`：CRT 符号是 `ExternResolver` 解析不了的未定义
 * 符号（见 README）；`VirtualAlloc` 是 kernel32 的，宿主对它已有确定性桩（按请求顺序递增，
 * 提交的页由系统保证是零）。地址确定性是判据的一部分 —— 见 `tools/difftrace/stubs.cc`。 */
extern "C" void* tc_array_alloc(unsigned long long count, unsigned long long elem_bytes) {
    const unsigned long long payload = count * elem_bytes;
    auto* p = static_cast<unsigned char*>(
        VirtualAlloc(NULL, 8 + payload, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (p == NULL) return NULL;

    const unsigned long long header = count << 16;
    for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>((header >> (8 * i)) & 0xFFu);
    /* 显式清零：不把「新提交的页是零」当成接口的一部分（同一个分配器将来若复用块就不成立了）。
     * 写 volatile：不写的话 GCC 把这个循环折成 `call memset`，而 `memset` 是 `ExternResolver`
     * 解析不了的 CRT 符号 —— 整个产物装配不上（`--out` 报 `memset` 未定义）。 */
    volatile unsigned char* z = p + 8;
    for (unsigned long long i = 0; i < payload; ++i) z[i] = 0;
    return p + 8;
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
