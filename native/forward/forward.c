/* forward.c —— 转发机制：路径表、原实现符号、编号、捕获、日志、指纹、配置、加载时清理。
 *
 * 这一层只认「一组路径」和一个「dll 名 + 符号名」，不认识任何具体业务（转发了谁、日志里的键
 * 各是什么意思）。路径全由壳注入，见 forward.h。
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "forward.h"

/* 生效的路径表。字段的初值指向空串而不是 NULL：任何一个函数在 forward_init 之前被调到都不会
   解引用空指针（结果是写不出文件，而不是崩进程）。 */
static const char k_empty[] = "";
static forward_paths g_paths = { k_empty, k_empty, k_empty, k_empty, k_empty };

/* 调用编号。定义在这里而不是调用方：捕获文件名里的编号必须跨「哪一层在记」唯一，
   所以本层自己持有它、只暴露一个取号函数。 */
static LONG g_seq = 0;

void forward_init(const forward_paths *p) {
    if (p) g_paths = *p;
}

const forward_paths *forward_get_paths(void) {
    return &g_paths;
}

LONG forward_next_seq(void) {
    return InterlockedIncrement(&g_seq);
}

void *forward_real_symbol(const char *dll, const char *name) {
    /* 必须用**自身模块路径**推导 dll 的位置，不能只写文件名：相对路径按宿主进程的**当前目录**
       解析，而宿主未必与我们同目录 —— 离线调用这个 dll 时就踩到了：加载失败 -> 返回 NULL ->
       看上去像「原实现不出活」，查了半天才发现是路径问题。
       多线程首次并发时可能重复加载：无害，只是引用计数 +1。 */
    HMODULE self = NULL;
    HMODULE real = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)&forward_real_symbol, &self)) {
        DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
        if (n > 0) {
            char *slash = strrchr(path, SEP);
            if (slash) {
                slash[1] = 0;
                lstrcatA(path, dll);
                real = LoadLibraryA(path);
            }
        }
    }
    if (!real) real = LoadLibraryA(dll);            /* 兜底：当前目录 */
    if (!real) return NULL;                         /* 最后错误码 = 126 / 193，留给调用方 */
    return (void *)GetProcAddress(real, name);      /* 没有该导出 = 127，同上 */
}

int forward_dump(const char *tag, const char *ext, const void *p, long long n, LONG seq) {
    if (!p || n <= 0 || n > (64LL << 20)) return 0;
    CreateDirectoryA(g_paths.out_dir, NULL);       /* 已存在会失败，无所谓 */
    char path[MAX_PATH];
    sprintf(path, "%s%chook_%s_%03ld%s", g_paths.out_dir, SEP, tag, seq, ext);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, p, (DWORD)n, &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)n;
}

int forward_write_file(const char *path, const void *p, long long n) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, p, (DWORD)n, &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)n;
}

/* 读一个「一行十六进制」的记录项。文件不存在返回 0，调用方就不动它的东西。 */
int forward_read_entry(const char *path, long long *val) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[32];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    if (!ok || got == 0) return 0;
    buf[got] = 0;
    return sscanf(buf, "%llx", (unsigned long long *)val) == 1;
}

/* 追加一行日志。
 *
 * 格式（制表符分隔：时间 / 编号 / 事件 / key=value…）与扩容方式（只加事件名或键，消费者忽略
 * 不认识的）以 Python 侧的解析模块为准 —— 那是这份契约的权威描述，本函数只是它在 C 侧的实现，
 * 改一处必须改另一处。
 *
 * 追加写（FILE_APPEND_DATA）：两个进程可能同时往这个文件里写，追加语义由系统保证；
 * 日志的价值也全在历史。
 */
void forward_log(LONG seq, const char *event, const char *kv) {
    CreateDirectoryA(g_paths.log_dir, NULL);
    char path[MAX_PATH];
    sprintf(path, "%s%chook.log", g_paths.log_dir, SEP);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[320];
    int n = sprintf(line, "%04d-%02d-%02d %02d:%02d:%02d\t%03ld\t%s",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    seq, event);
    if (kv && kv[0]) n += sprintf(line + n, "\t%s", kv);
    n += sprintf(line + n, "\r\n");
    DWORD w = 0;
    WriteFile(h, line, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

/* ---- 系统 CNG（bcrypt.dll）的按需绑定 ------------------------------------
 *
 * 指纹用 Windows 自带的 CNG，但**不链接 -lbcrypt** —— 那要依赖工具链的导入库 libbcrypt.a
 * （只是声明，不含实现）。这里只取 7 个函数：首次用到时 LoadLibrary 一次、GetProcAddress 一次，
 * 之后每次调用只查一个静态标志，与 forward_real_symbol 是同一套路。
 *
 * 连 <bcrypt.h> 也不 include：需要的符号一共就下面这几个，自己声明即可。
 *
 * 绑不上（或任一环节失败）就让指纹退化成调用方给的 "-"，不影响别的事。
 * 多线程首次并发可能重复 LoadLibrary —— 与 forward_real_symbol 同样宽容处理，无害。
 */
#define CNG_DLL           "bcrypt.dll"
#define CNG_SHA256        L"SHA256"
#define CNG_OBJECT_LENGTH L"ObjectLength"

typedef LONG (WINAPI *cng_open_t)(void** alg, LPCWSTR id, LPCWSTR impl, ULONG flags);
typedef LONG (WINAPI *cng_prop_t)(void* obj, LPCWSTR prop, PUCHAR out, ULONG outlen,
                                  ULONG* got, ULONG flags);
typedef LONG (WINAPI *cng_create_t)(void* alg, void** hash, PUCHAR buf, ULONG buflen,
                                    PUCHAR secret, ULONG secretlen, ULONG flags);
typedef LONG (WINAPI *cng_data_t)(void* hash, PUCHAR in, ULONG inlen, ULONG flags);
typedef LONG (WINAPI *cng_finish_t)(void* hash, PUCHAR out, ULONG outlen, ULONG flags);
typedef LONG (WINAPI *cng_destroy_t)(void* hash);
typedef LONG (WINAPI *cng_close_t)(void* alg, ULONG flags);

static struct {
    int           state;          /* 0 = 还没试过，1 = 可用，-1 = 不可用 */
    cng_open_t    open_alg;
    cng_prop_t    get_prop;
    cng_create_t  create_hash;
    cng_data_t    hash_data;
    cng_finish_t  finish_hash;
    cng_destroy_t destroy_hash;
    cng_close_t   close_alg;
} g_cng;

static int cng_bind(void) {
    if (g_cng.state) return g_cng.state > 0;
    HMODULE m = LoadLibraryA(CNG_DLL);
    if (m) {
        g_cng.open_alg     = (cng_open_t)   GetProcAddress(m, "BCryptOpenAlgorithmProvider");
        g_cng.get_prop     = (cng_prop_t)   GetProcAddress(m, "BCryptGetProperty");
        g_cng.create_hash  = (cng_create_t) GetProcAddress(m, "BCryptCreateHash");
        g_cng.hash_data    = (cng_data_t)   GetProcAddress(m, "BCryptHashData");
        g_cng.finish_hash  = (cng_finish_t) GetProcAddress(m, "BCryptFinishHash");
        g_cng.destroy_hash = (cng_destroy_t)GetProcAddress(m, "BCryptDestroyHash");
        g_cng.close_alg    = (cng_close_t)  GetProcAddress(m, "BCryptCloseAlgorithmProvider");
    }
    if (!m || !g_cng.open_alg || !g_cng.get_prop || !g_cng.create_hash ||
        !g_cng.hash_data || !g_cng.finish_hash || !g_cng.destroy_hash ||
        !g_cng.close_alg) {
        g_cng.state = -1;
        if (m) FreeLibrary(m);
        return 0;
    }
    g_cng.state = 1;
    return 1;
}

/* SHA-256 十六进制串（小写，64 字符 + 终止符）。
 *
 * 用系统 CNG 而不是自带实现：日志里另一侧（Python hashlib）算的指纹必须与本层逐字符一致才谈得
 * 上交叉核对，所以用同一个标准实现。
 *
 * **失败时不碰 out**（保留调用方给的 "-"）—— 记录失败要静默，不能因为指纹算不出来就影响正事，
 * 或写出一条看似有效的错指纹。
 */
void forward_sha256_hex(const void *p, long long n, char *out) {
    if (!p || n <= 0 || n > (64LL << 20)) return;
    if (!cng_bind()) return;
    void *alg = NULL;
    void *hash = NULL;
    PUCHAR obj = NULL;
    ULONG objlen = 0, got = 0;
    UCHAR digest[32];
    if (g_cng.open_alg(&alg, CNG_SHA256, NULL, 0) != 0) return;
    if (g_cng.get_prop(alg, CNG_OBJECT_LENGTH, (PUCHAR)&objlen,
                       sizeof(objlen), &got, 0) != 0) goto done;
    obj = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, objlen);
    if (!obj) goto done;
    if (g_cng.create_hash(alg, &hash, obj, objlen, NULL, 0, 0) != 0) goto done;
    if (g_cng.hash_data(hash, (PUCHAR)p, (ULONG)n, 0) != 0) goto done;
    if (g_cng.finish_hash(hash, digest, (ULONG)sizeof(digest), 0) != 0) goto done;
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", digest[i]);
done:
    if (hash) g_cng.destroy_hash(hash);
    if (obj) HeapFree(GetProcessHeap(), 0, obj);
    g_cng.close_alg(alg, 0);
}

/* 一个配置值里不会出现的字节：Win32 把「键不存在」与「键存在但值是空的」都写成空串，靠它分开。 */
#define CONFIG_ABSENT "\x01"

int forward_config_get(const char *key, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
    GetPrivateProfileStringA("opt", key, CONFIG_ABSENT, buf, (DWORD)cap, g_paths.ini_path);
    if (lstrcmpA(buf, CONFIG_ABSENT) == 0) {
        buf[0] = 0;
        return 0;
    }
    return 1;
}

/* 加载时清掉上一次会话的捕获。
 *
 * 为什么：编号是**本 dll 的全局变量**，每次加载都从 1 重新编号。不清空的话，新捕获会覆盖同号的
 * 旧文件，而且新旧混在同一目录里分不出哪些是本次会话的。
 *
 * **只删这一层的文件，不进入子目录** —— 想长期保留的，自己挪到子目录里。
 * 在 DllMain 里只做 FindFirstFile / DeleteFile 这类不触发 loader 的操作，是安全的。
 */
static int ends_with_ci(const char *name, const char *ext) {
    int ln = lstrlenA(name), le = lstrlenA(ext);
    return ln >= le && lstrcmpiA(name + ln - le, ext) == 0;
}

void forward_purge_captures(void) {
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    lstrcpyA(pat, g_paths.out_dir);
    lstrcatA(pat, "\\*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;         /* 目录还不存在，没什么可清的 */
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;   /* 不碰子目录 */
        /* 日志与捕获同批清：它是**一次会话**的记录（编号随加载从 1 重来），留着上一会话的同号行，
           按编号回查归属时会摸到上一会话的值。 */
        if (lstrcmpiA(fd.cFileName, "hook.log") != 0 &&
            !ends_with_ci(fd.cFileName, ".si") &&
            !ends_with_ci(fd.cFileName, ".bin") &&
            !ends_with_ci(fd.cFileName, ".tsv")) continue;   /* 清单文件同批清 */
        lstrcpyA(path, g_paths.out_dir);
        lstrcatA(path, "\\");
        lstrcatA(path, fd.cFileName);
        DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* 加载时也清掉中间产物目录。
 *
 * 为什么：那里的 NNN.raw.bin / NNN.opt.bin / *.log 与捕获文件共用同一个编号（都出自
 * forward_next_seq），却**从未跟着一起清** —— 实测同一个编号攒到 5 条（五轮会话都编到过 001），
 * 而对应的捕获只剩本轮那一份。后果是按编号回查时会摸到上一轮会话的记录，指纹张冠李戴。
 *
 * 只删这四类（`.raw.bin` / `.opt.bin` / `.entry.txt` / `.log`），不整目录删 —— 免得误伤
 * 将来往这里放的东西。
 */
void forward_purge_tmp(void) {
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    lstrcpyA(pat, g_paths.tmp_dir);
    lstrcatA(pat, "\\*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;         /* 目录还不存在 */
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!ends_with_ci(fd.cFileName, ".raw.bin") &&
            !ends_with_ci(fd.cFileName, ".opt.bin") &&
            !ends_with_ci(fd.cFileName, ".entry.txt") &&
            !ends_with_ci(fd.cFileName, ".log")) continue;
        lstrcpyA(path, g_paths.tmp_dir);
        lstrcatA(path, "\\");
        lstrcatA(path, fd.cFileName);
        DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
