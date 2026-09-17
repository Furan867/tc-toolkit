/* compile_hook.c —— 转发 compile.dll 的三个导出，顺手记录 compile() 的输入与产物。
 *
 * 为什么用这个办法
 * ----------------
 * 游戏 exe 是用 Nim 的 dynlib **按名字**取编译器的（符号 loadLib__pureZdynlib_u3 /
 * symAddr__pureZdynlib_u74 = LoadLibrary + GetProcAddress）。既然是按名字取，
 * 那只要让 "compile.dll" 这个名字指向我们就够了 —— 真正的实现改名成
 * compile_real.dll，我们只做转发。
 *
 * 比起之前"调试器 + 硬件断点"的做法：
 *   · 不占用 CPU 的 DR0–DR3（那是**全局**资源，游戏或 Steam overlay 可能自己也在用
 *     —— 实测因此崩了两次游戏）
 *   · 不拖慢游戏（调试器附加会让每个异常、每次线程创建都绕一趟调试器）
 *   · 失败模式可控：本 DLL 加载失败 = 电路编译失败，**不会崩进程**
 *   · 恢复只需改回文件名
 *
 * 记录的内容（写到 OUT_DIR）
 * ----------
 *   hook_src_NNN.bin  —— compile() 收到的 Simplex 源码
 *   hook_out_NNN.bin  —— compile() 产出的机器码
 *
 * 契约（已由反汇编独立验证，2026-09-15 / 09-17 两次复核）
 * ------------------------------------------------------
 *   Result* compile(Result* out [rcx], NimStr* src [rdx], int flags [r8d]
 *   NimStr = {long long len; void* ptr}      字符数据在 ptr + 8（不是内联！）
 *   Result = {long long out_len; void* out_ptr; ...}   共 40 字节
 *   compile 入口机器码实测：push r15..rbx / sub rsp,0x118 / mov rbp,[rdx] /
 *                          mov r14,[rdx+8] / lea rdx,[rbp+0x6C3F](27711)
 *
 * 编译
 * ----
 *   D:\bin\w64devkit\bin\gcc.exe -shared -O2 -static -o compile.dll compile_hook.c
 */
#include <windows.h>
#include <stdio.h>

#define REAL_DLL "compile_real.dll"
#define OUT_DIR  "D:\\home\\code-proj\\Agent\\proj\\tc\\reversing\\work"

typedef void* (*compile_fn)(void* out, void* src, int flags);
typedef void  (*void_fn)(void);

static HMODULE    g_real = NULL;
static compile_fn g_compile = NULL;
static LONG       g_seq = 0;

/* 延迟加载：绝不能在 DllMain 里 LoadLibrary（loader lock 会死锁）。
   多线程首次并发时可能重复 LoadLibrary —— 无害，只是引用计数 +1。 */
static void ensure_real(void) {
    if (g_real) return;
    HMODULE m = LoadLibraryA(REAL_DLL);
    if (!m) return;
    g_real = m;
    g_compile = (compile_fn)GetProcAddress(m, "compile");
}

static void dump(const char* tag, const void* p, long long n) {
    if (!p || n <= 0 || n > (64LL << 20)) return;
    char path[MAX_PATH];
    LONG i = InterlockedIncrement(&g_seq);
    sprintf(path, "%s\\hook_%s_%03ld.bin", OUT_DIR, tag, i);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, p, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

__declspec(dllexport) void* compile(void* out, void* src, int flags) {
    ensure_real();
    if (src) {
        long long len = *(long long*)src;
        void* ptr = *(void**)((char*)src + 8);
        if (ptr) dump("src", (const char*)ptr + 8, len);   /* 数据在 ptr + 8 */
    }
    if (!g_compile) return NULL;
    void* ret = g_compile(out, src, flags);
    if (out) {
        long long olen = *(long long*)out;
        void* optr = *(void**)((char*)out + 8);
        if (optr) dump("out", optr, olen);
    }
    return ret;
}

__declspec(dllexport) void NimMain(void) {
    ensure_real();
    if (!g_real) return;
    void_fn f = (void_fn)GetProcAddress(g_real, "NimMain");
    if (f) f();
}

__declspec(dllexport) void NimDestroyGlobals(void) {
    ensure_real();
    if (!g_real) return;
    void_fn f = (void_fn)GetProcAddress(g_real, "NimDestroyGlobals");
    if (f) f();
}
