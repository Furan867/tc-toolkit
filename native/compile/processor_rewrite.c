/* 编译后处理器 `rewrite`：把产物交给外部改写器（`tools/optimize_image.py`）走一趟，
 * 成功则**就地**写回原缓冲区。
 *
 * 为什么单独一个翻译单元：编译后优化是**可以不要**的一层，接口见 processor.h。
 * 「删掉编译后优化」= 移走本文件 + 删掉壳里注册表的那一项（`compile_hook.c` 的 `g_processors[]`），
 * 壳的正文一个字都不用改 —— 判据就是「删掉后壳照样编得过」。
 *
 * 配置值（`opt.ini` 的 `[opt] rewrite=`）由壳读**一次**后经 `init()` 交进来：本文件不自己读文件，
 * 于是「这趟用了什么模式」只有一处来源，与 impl 行的 `rewrite=` 必然同值。
 */
#include <windows.h>
#include <stdio.h>

#include "forward.h"            /* 日志 / 写文件 / 读记录项 / 路径表（机制层）*/
#include "paths.h"               /* 子进程的 cwd（壳自己那两个路径）*/
#include "native_internal.h"     /* 启动器与超时 */
#include "processor.h"

static char rewrite_mode[32];

/* 长度按 `rewrite_mode` 截断：进来的是 `opt.ini` 里人写的串，长度不受我们控制。 */
static void rewrite_init(const char *mode) {
    lstrcpynA(rewrite_mode, mode, (int)sizeof rewrite_mode);
}

/* 把产物交给外部改写器走一趟，成功则**就地**写回原缓冲区。
 *
 * MVP 的三条约束都落在这个函数里：
 *   · 长度必须与原件相 —— 不等就放弃（因此不必动 out_len、不必动 8 字节头）
 *   · 不换缓冲区 —— 直接覆盖原缓冲，于是没有「新内存谁分配、谁释放」的问题
 *   · 任何一步失败都静默放弃 —— 游戏绝不该因为优化器出问题而受影响
 */
static void rewrite_process(void* buf, long long len, LONG seq, void* out) {
    if (!buf || len <= 0 || len > (64LL << 20)) return;
    const char* tmp_dir = forward_get_paths()->tmp_dir;
    const char* script = forward_get_paths()->script;
    CreateDirectoryA(tmp_dir, NULL);

    char raw[MAX_PATH], opt[MAX_PATH], cmd[4 * MAX_PATH], err[MAX_PATH];
    char entry[MAX_PATH];
    sprintf(raw, "%s%c%03ld.raw.bin", tmp_dir, SEP, seq);
    sprintf(opt, "%s%c%03ld.opt.bin", tmp_dir, SEP, seq);
    sprintf(err, "%s%copt_stderr.log", tmp_dir, SEP);
    sprintf(entry, "%s%c%03ld.entry.txt", tmp_dir, SEP, seq);
    DeleteFileA(entry);   /* 防残留：上一轮同号的入口文件不能影响本轮 */

    if (!forward_write_file(raw, buf, len)) { forward_log(seq, "error", "stage=raw"); return; }

    /* 把 Result[2]（旧锚点偏移）与模式一起传给改写器 —— 缩短模式必须同步更新
       锚点；模式由壳经 init() 交进来（文件级的 `rewrite_mode`），是为了让配置**只被读一次、
       只被判定一次** —— 与 impl 行的 `rewrite=` 同源。 */
    if (rewrite_mode[0])
        sprintf(cmd, "\"%s\" \"%s\" --in \"%s\" --out \"%s\" --entry %llX --mode %s",
                PY_EXE, script, raw, opt,
                out ? *(long long*)((char*)out + 16) : 0LL, rewrite_mode);
    else
        sprintf(cmd, "\"%s\" \"%s\" --in \"%s\" --out \"%s\" --entry %llX",
                PY_EXE, script, raw, opt,
                out ? *(long long*)((char*)out + 16) : 0LL);

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
                                  CREATE_NO_WINDOW, NULL, g_paths.proj, &si, &pi);
    if (sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
    if (!started) { forward_log(seq, "error", "stage=spawn"); return; }

    DWORD wait = WaitForSingleObject(pi.hProcess, OPT_TIMEOUT);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (wait != WAIT_OBJECT_0 || code != 0) {
        char kv[64];
        if (wait != WAIT_OBJECT_0) sprintf(kv, "stage=child\ttimeout=1");
        else sprintf(kv, "stage=child\tcode=%lu", (unsigned long)code);
        forward_log(seq, "error", kv);
        return;
    }

    HANDLE h = CreateFileA(opt, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { forward_log(seq, "error", "stage=optbin"); return; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart != len) {
        CloseHandle(h);
        forward_log(seq, "error", "stage=length");
        return;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)len, &got, NULL);
    CloseHandle(h);
    if (!ok || got != (DWORD)len) { forward_log(seq, "error", "stage=read"); return; }
    char kvw[64];
    sprintf(kvw, "len=%lld", len);
    forward_log(seq, "writeback", kvw);

    /* 缩短模式：改写器把平移后的入口偏移写在 NNN.entry.txt，
       必须同步改回 Result[2]，否则游戏按旧偏移调进指令中间（实测必崩） */
    long long v = 0;
    if (forward_read_entry(entry, &v) && v > 0 && v < len) {
        *(long long*)((char*)out + 16) = v;
        char kve[48];
        sprintf(kve, "offset=0x%llX", v);
        forward_log(seq, "entry", kve);
    }
}

const forward_processor processor_rewrite = { "rewrite", rewrite_init, rewrite_process };
