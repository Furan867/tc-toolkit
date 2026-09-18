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
 *   · 不占用 CPU 的 DR0-DR3（那是**全局**资源，游戏或 Steam overlay 可能自己也在用
 *     —— 实测因此崩了两次游戏）
 *   · 不拖慢游戏（调试器附加会让每个异常、每次线程创建都绕一趟调试器）
 *   · 失败模式可控：本 DLL 加载失败 = 电路编译失败，**不会崩进程**
 *   · 恢复只需改回文件名
 *
 * 记录的内容（写到 OUT_DIR）
 * -------------------------
 *   hook_src_NNN.si   —— compile() 收到的 Simplex 源码（.si = Simplex）
 *   hook_out_NNN.bin  —— compile() 产出的机器码
 *
 * 编号：**一次 compile() 调用取一个号，src 与 out 共用同一个号** ——
 *       hook_src_005.si 与 hook_out_005.bin 就是一对。
 *       （计数器是进程内变量，游戏重启会归零，所以文件名的新旧要看 mtime。）
 *
 * 契约（已由反汇编独立验证，2026-09-15 / 09-17 两次复核）
 * ------------------------------------------------------
 *   Result* compile(Result* out [rcx], NimStr* src [rdx], int flags [r8d])
 *   NimStr = {long long len; void* ptr}      字符数据在 ptr + 8（不是内联！）
 *   Result = {long long out_len; void* out_ptr; ...}   共 40 字节
 *   注意 out_len 的值等于**整个缓冲的长度**（含 8 字节头），不是机器码长度。
 *
 * 编译
 * ----
 *   D:\bin\w64devkit\bin\gcc.exe -shared -O2 -static -o compile.dll compile_hook.c
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define REAL_DLL "compile_real.dll"
#define OUT_DIR  "D:\\home\\code-proj\\Agent\\proj\\tc\\reversing\\work\\hook"
#define SEP      92        /* 反斜杠的 ASCII —— 写成字符字面量在多轮转义里会失真 */

typedef void* (*compile_fn)(void* out, void* src, int flags);
typedef void  (*void_fn)(void);

static HMODULE    g_real = NULL;
static compile_fn g_compile = NULL;
static LONG       g_seq = 0;

static void ensure_real(void) {
    /* 延迟加载：绝不能在 DllMain 里 LoadLibrary（loader lock 会死锁）。
       多线程首次并发时可能重复 LoadLibrary —— 无害，只是引用计数 +1。

       ⚠️ 必须用**自身模块路径**推导 compile_real.dll 的位置，不能写相对路径
       "compile_real.dll" —— 相对路径按宿主进程的**当前目录**解析，而宿主未必在
       游戏目录。离线调用这个 dll 做验证时就踩到了：LoadLibrary 失败 -> 返回 NULL
       -> 表现为「编译失败」，查了半天才发现是路径问题。 */
    if (g_real) return;
    HMODULE self = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)&ensure_real, &self)) {
        DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
        if (n > 0) {
            char *slash = strrchr(path, SEP);
            if (slash) {
                *(slash + 1) = 0;
                lstrcatA(path, REAL_DLL);
                g_real = LoadLibraryA(path);
            }
        }
    }
    if (!g_real) g_real = LoadLibraryA(REAL_DLL);      /* 兜底：当前目录 */
    if (g_real) g_compile = (compile_fn)GetProcAddress(g_real, "compile");
}

static void dump(const char* tag, const char* ext, const void* p, long long n, LONG seq) {
    if (!p || n <= 0 || n > (64LL << 20)) return;
    CreateDirectoryA(OUT_DIR, NULL);       /* 已存在会失败，无所谓 */
    char path[MAX_PATH];
    sprintf(path, "%s%chook_%s_%03ld%s", OUT_DIR, SEP, tag, seq, ext);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, p, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

__declspec(dllexport) void* compile(void* out, void* src, int flags) {
    ensure_real();
    /* **一次调用取一个号**，src 与 out 共用 —— 这样 hook_src_NNN.si 与
       hook_out_NNN.bin 编号相同，一眼看出一对。早先的实现是在 dump() 里各自
       取号，于是编号成了 src_001/out_002、src_003/out_004…，配对要看「-1」。 */
    LONG seq = InterlockedIncrement(&g_seq);
    if (src) {
        long long len = *(long long*)src;
        void* ptr = *(void**)((char*)src + 8);
        if (ptr) dump("src", ".si", (const char*)ptr + 8, len, seq);   /* 数据在 ptr + 8 */
    }
    if (!g_compile) return NULL;
    void* ret = g_compile(out, src, flags);
    if (out) {
        long long olen = *(long long*)out;
        void* optr = *(void**)((char*)out + 8);
        if (optr) dump("out", ".bin", optr, olen, seq);
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
