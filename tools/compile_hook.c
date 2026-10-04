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
 *       计数器是本 DLL 的全局变量，**每次加载（游戏启动）都从 1 重新编号**，
 *       因此加载时会清空目录里的 .si/.bin（不含子目录），并把 OPT_DIR 里的
 *       中间产物（NNN.raw.bin / NNN.opt.bin / *.log）一并清掉 —— 两边的编号
 *       是同一套，只清一边就会新旧同号混在一起。
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

/* 产物改写器（MVP：把产物交给外部 Python 走一趟，再**就地**写回原缓冲）
 *
 * 这两个路径与 OUT_DIR 一样只能写死：转发层住在游戏目录里，它无从知道项目在哪
 * （`compile_real.dll` 能用自身模块路径推导，是因为它与我们同在游戏目录 ——
 * 见 ensure_real。改机器上的路径要改这三行）。
 */
#define OPT_DIR     "D:\\home\\code-proj\\Agent\\proj\\tc\\reversing\\work\\opt"
#define OPT_PROJ    "D:\\home\\code-proj\\Agent\\proj\\tc\\reversing"
#define OPT_SCRIPT  OPT_PROJ "\\tools\\optimize_image.py"
#define PY_EXE      "C:\\Windows\\py.exe"    /* 系统自带的启动器，不依赖 PATH */
#define OPT_TIMEOUT 8000                      /* 子进程最多等 8 秒，超时就放弃 */

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

static int write_file(const char* path, const void* p, long long n) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, p, (DWORD)n, &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)n;
}

/* 优化器的运行日志。恒等改写时 opt 与 raw 逐字节相同，光看产物分不出
   「跑了但没改」和「压根没跑」—— 这行日志就是唯一的证据。 */
static void opt_log(LONG seq, const char* tag) {
    CreateDirectoryA(OPT_DIR, NULL);
    char path[MAX_PATH];
    sprintf(path, "%s%chook.log", OPT_DIR, SEP);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[160];
    int n = sprintf(line, "%04d-%02d-%02d %02d:%02d:%02d  %03ld  %s\r\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    seq, tag);
    DWORD w = 0;
    WriteFile(h, line, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

/* 把产物交给外部改写器走一趟，成功则**就地**写回原缓冲区。
 *
 * MVP 的三条约束都落在这个函数里：
 *   · 长度必须与原件相 —— 不等就放弃（因此不必动 out_len、不必动 8 字节头）
 *   · 不换缓冲区 —— 直接覆盖原缓冲，于是没有「新内存谁分配、谁释放」的问题
 *   · 任何一步失败都静默放弃 —— 游戏绝不该因为优化器出问题而受影响
 */
static void optimize_inplace(void* buf, long long len, LONG seq) {
    if (!buf || len <= 0 || len > (64LL << 20)) return;
    CreateDirectoryA(OPT_DIR, NULL);

    char raw[MAX_PATH], opt[MAX_PATH], cmd[4 * MAX_PATH], err[MAX_PATH];
    sprintf(raw, "%s%c%03ld.raw.bin", OPT_DIR, SEP, seq);
    sprintf(opt, "%s%c%03ld.opt.bin", OPT_DIR, SEP, seq);
    sprintf(err, "%s%copt_stderr.log", OPT_DIR, SEP);
    if (!write_file(raw, buf, len)) { opt_log(seq, "写 raw 失败，放弃"); return; }

    sprintf(cmd, "\"%s\" \"%s\" --in \"%s\" --out \"%s\"",
            PY_EXE, OPT_SCRIPT, raw, opt);

    /* 把子进程的输出/错误引到文件：游戏是 GUI 进程，标准句柄未必可用，
       直接继承会让 Python 在写 stderr 时出怪事，且现场也留不下来。 */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE sink = CreateFileA(err, FILE_APPEND_DATA, FILE_SHARE_READ, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (sink != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = sink;
        si.hStdError = sink;
    }
    ZeroMemory(&pi, sizeof(pi));
    BOOL started = CreateProcessA(NULL, cmd, NULL, NULL,
                                  sink != INVALID_HANDLE_VALUE,
                                  CREATE_NO_WINDOW, NULL, OPT_PROJ, &si, &pi);
    if (sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
    if (!started) { opt_log(seq, "起子进程失败，放弃"); return; }

    DWORD wait = WaitForSingleObject(pi.hProcess, OPT_TIMEOUT);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (wait != WAIT_OBJECT_0 || code != 0) { opt_log(seq, "子进程失败或超时，放弃"); return; }

    HANDLE h = CreateFileA(opt, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { opt_log(seq, "读不到 opt，放弃"); return; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart != len) {
        CloseHandle(h);
        opt_log(seq, "长度与原件不符，放弃（MVP 不改长度）");
        return;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)len, &got, NULL);
    CloseHandle(h);
    opt_log(seq, ok && got == (DWORD)len ? "已就地写回（长度未变）" : "写回失败");
}

/* 加载时清空 OUT_DIR 下的 .si / .bin。
 *
 * 为什么：编号计数器 g_seq 是**本 DLL 的全局变量**，每次加载都从 1 重新编号。
 * 不清空的话，新捕获会覆盖同号的旧文件，而且新旧混在同一目录里分不出哪些是
 * 本次会话的 —— 实测就发生过「09-19 00:23 的 001-006 被 00:35 的 001-004 覆盖」。
 *
 * **只删这一层的文件，不进入子目录** —— 想长期保留的捕获，自己挪到子目录里。
 * DllMain 里只做 FindFirstFile / DeleteFile 这类不触发 loader 的操作，是安全的。
 */
static int ends_with_ci(const char* name, const char* ext) {
    int ln = lstrlenA(name), le = lstrlenA(ext);
    return ln >= le && lstrcmpiA(name + ln - le, ext) == 0;
}

static void purge_captures(void) {
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    lstrcpyA(pat, OUT_DIR);
    lstrcatA(pat, "\\*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;         /* 目录还不存在，没什么可清的 */
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;   /* 不碰子目录 */
        if (!ends_with_ci(fd.cFileName, ".si") &&
            !ends_with_ci(fd.cFileName, ".bin")) continue;
        lstrcpyA(path, OUT_DIR);
        lstrcatA(path, "\\");
        lstrcatA(path, fd.cFileName);
        DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* 加载时也清掉 OPT_DIR 里的中间产物。
 *
 * 为什么：OPT_DIR 里的 NNN.raw.bin / NNN.opt.bin / *.log 与 OUT_DIR 的捕获文件
 * 共用同一个编号（都出自本 DLL 的 g_seq），却**从未跟着一起清** —— 实测
 * optimize.log 里同一编号攒到 5 条（五轮游戏会话都编到过 001），而对应的
 * hook_out_001.bin 只剩本轮那一份。后果是按编号回查产物信息时会摸到上一轮
 * 会话的记录，指纹张冠李戴。
 *
 * 只删这三类，不整目录删 —— 免得误伤将来往这里放的东西。
 */
static void purge_workdir(void) {
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    lstrcpyA(pat, OPT_DIR);
    lstrcatA(pat, "\\*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;         /* 目录还不存在 */
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!ends_with_ci(fd.cFileName, ".raw.bin") &&
            !ends_with_ci(fd.cFileName, ".opt.bin") &&
            !ends_with_ci(fd.cFileName, ".log")) continue;
        lstrcpyA(path, OPT_DIR);
        lstrcatA(path, "\\");
        lstrcatA(path, fd.cFileName);
        DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        purge_captures();
        purge_workdir();
    }
    return TRUE;
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
        if (optr) {
            dump("out", ".bin", optr, olen, seq);
            /* MVP：让产物去 Python 里转一圈，原样写回。任何一步失败都不影响返回。 */
            optimize_inplace(optr, olen, seq);
        }
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
