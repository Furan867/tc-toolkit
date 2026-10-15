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
 *   manifest.tsv      —— 一次 compile() 一行：seq / 时间 / flags / src 与产物的
 *                        文件名·长度·sha256 / Result 全 5 字段（十六进制）。
 *                        为什么另开一份而不写进 hook.log：hook.log 只记事件与
 *                        rewrite 配置，且它的行格式已被 bench_rewrite（数新行）
 *                        和 watch_freq（提 rewrite= 行）解析；flags 与 Result
 *                        全字段是「每次调用的结构化事实」，塞进去会搅混语义。
 *
 * 编号：**一次 compile() 调用取一个号，src 与 out 共用同一个号** ——
 *       hook_src_005.si 与 hook_out_005.bin 就是一对。
 *       计数器是本 DLL 的全局变量，**每次加载（游戏启动）都从 1 重新编号**，
 *       因此加载时会清空目录里的 .si/.bin/.tsv（不含子目录），并把 OPT_DIR 里的
 *       中间产物（NNN.raw.bin / NNN.opt.bin / *.log）一并清掉 —— 两边的编号
 *       是同一套，只清一边就会新旧同号混在一起。
 *
 * 契约（已由反汇编独立验证，2026-09-15 / 09-17 两次复核）
 * ------------------------------------------------------
 *   Result* compile(Result* out [rcx], NimStr* src [rdx], int flags [r8d])
 *   NimStr = {long long len; void* ptr}      字符数据在 ptr + 8（不是内联！）
 *   Result = {long long out_len; void* out_ptr; ...}   共 40 字节
 *   注意 out_len 的值等于**整个缓冲的长度**（含 8 字节头），不是机器码长度。
 *   qword[2]（out+16）= 产物内的**锚点偏移**（空源码 0x212F、RV32IM 0x215B、
 *   MIPS 0x2142）。它随产物平移，但**不保证落在指令边界上**（MIPS 那个就在
 *   "break point" 字符串中间），所以按「能映射」处理即可，别要求它在指令起点。
 *   缩短改写会平移它，必须同步更新，否则游戏拿到错位置（实测主频掉到 0.566
 *   后越界崩溃）。改写器把新偏移写在 NNN.entry.txt，由本层读回改 qword[2]。
 *
 * 编译
 * ----
 *   D:\bin\w64devkit\bin\gcc.exe -shared -O2 -static -o compile.dll compile_hook.c
 *
 *   无第三方链接依赖：manifest.tsv 的 sha256 走系统 bcrypt.dll 的按需绑定
 *   （见 cng_bind / sha256_hex），所以不用 -lbcrypt、也不 include <bcrypt.h>。
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

/* 返回是否真的写出了文件 —— manifest 要按它决定写文件名还是 "-"，
   免得记录里指着一个因超限/失败而根本没落盘的捕获。 */
static int dump(const char* tag, const char* ext, const void* p, long long n, LONG seq) {
    if (!p || n <= 0 || n > (64LL << 20)) return 0;
    CreateDirectoryA(OUT_DIR, NULL);       /* 已存在会失败，无所谓 */
    char path[MAX_PATH];
    sprintf(path, "%s%chook_%s_%03ld%s", OUT_DIR, SEP, tag, seq, ext);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, p, (DWORD)n, &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)n;
}

/* ---- 系统 CNG（bcrypt.dll）的按需绑定 ------------------------------------
 *
 * sha256 用 Windows 自带的 CNG，但**不链接 -lbcrypt** —— 那要依赖 w64devkit
 * 的导入库 libbcrypt.a（只是声明，不含实现）。这里只取 7 个函数：首次用到时
 * LoadLibrary 一次、GetProcAddress 一次，之后每次调用只查一个静态标志，
 * 与 ensure_real() 是同一套路。
 *
 * 连 <bcrypt.h> 也不 include：需要的符号一共就下面这几个，自己声明即可，
 * 免得依赖 w64devkit 的头文件。
 *
 * 绑不上（或任一环节失败）就让 sha256 退化成写 "-"，不影响编译与返回值。
 * 多线程首次并发可能重复 LoadLibrary —— 与 ensure_real 同样宽容处理，无害。
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
 * 用系统 CNG 而不是自带实现：`hook.log` 里 `opt` 行的指纹是 Python hashlib 算的，
 * 两边必须逐字符一致才谈得上交叉核对，所以用同一个标准实现。
 *
 * **失败时不碰 out**（保留调用方给的 "-"）—— 记录失败要静默，不能因为指纹
 * 算不出来就影响编译，或写出一条看似有效的错指纹。
 */
static void sha256_hex(const void* p, long long n, char* out) {
    if (!p || n <= 0 || n > (64LL << 20)) return;
    if (!cng_bind()) return;
    void* alg = NULL;
    void* hash = NULL;
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

/* 追加一行 manifest.tsv：把一次 compile() 的四样东西串起来 ——
 * flags（游戏传入的真值，之前从未落盘）、src 与产物的指纹、Result 全 5 字段。
 *
 * 列（制表符分隔，首行表头）：
 *   seq time flags src src_len src_sha256 out out_len out_sha256 r0 r1 r2 r3 r4
 * r0..r4 是 Result 的 qword[0..4]（40 字节），r0 = 缓冲总长、r1 = 缓冲指针、
 * r2 = 锚点偏移；r3/r4 到 2026-10 为止没人读过，记下来就是为了看编译器是否初始化它们
 * —— 离线实测被写成 0（调用方缓冲预先填 0xCC 哨兵，读回来仍是 0，不是残留）。
 *
 * 必须在 optimize_inplace 之后调用：那时 r2 才是交给游戏的**最终**锚点。
 * 任何一步失败都静默跳过（与 dump / opt_log 同一风格）。
 */
static void manifest_line(LONG seq, int flags,
                          const char* src_name, long long src_len, const char* src_sha,
                          const char* out_name, long long out_len, const char* out_sha,
                          const void* out) {
    if (!out) return;                      /* 没有 out 缓冲就没什么可记 */
    CreateDirectoryA(OUT_DIR, NULL);
    char path[MAX_PATH];
    sprintf(path, "%s%cmanifest.tsv", OUT_DIR, SEP);
    int fresh = GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    const long long* r = (const long long*)out;
    char line[512];
    int n = 0;
    if (fresh)
        n += sprintf(line + n, "seq\ttime\tflags\tsrc\tsrc_len\tsrc_sha256"
                               "\tout\tout_len\tout_sha256\tr0\tr1\tr2\tr3\tr4\r\n");
    n += sprintf(line + n,
                 "%03ld\t%04d-%02d-%02d %02d:%02d:%02d\t0x%X\t%s\t%lld\t%s\t%s\t%lld\t%s"
                 "\t0x%016llX\t0x%016llX\t0x%016llX\t0x%016llX\t0x%016llX\r\n",
                 seq, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 (unsigned)flags, src_name, src_len, src_sha, out_name, out_len, out_sha,
                 (unsigned long long)r[0], (unsigned long long)r[1],
                 (unsigned long long)r[2], (unsigned long long)r[3],
                 (unsigned long long)r[4]);
    DWORD w = 0;
    WriteFile(h, line, (DWORD)n, &w, NULL);
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

/* 读 NNN.entry.txt（一行十六进制）—— 改写器在缩短模式下写的新入口偏移。
   文件不存在（等长/off/防御未过）返回 FALSE，本层就不动 Result[2]。 */
static BOOL read_entry(const char* path, long long* val) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char buf[32];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    if (!ok || got == 0) return FALSE;
    buf[got] = 0;
    return sscanf(buf, "%llx", (unsigned long long*)val) == 1;
}

/* 转发层的记录项 —— 与改写器写的 `opt` 行同处一个 hook.log。
 *
 * 格式（制表符分隔：时间 / 编号 / 事件 / key=value…）与扩容方式（只加事件名或
 * 键，消费者忽略不认识的）见 tools/hook_log.py 的模块 docstring —— 那是这份契约
 * 的权威描述，本函数只是它在 C 侧的实现，改一处必须改另一处。
 *
 * 追加写（FILE_APPEND_DATA）：转发层与改写子进程同时往这个文件里写，追加语义由
 * 系统保证；日志的价值也全在历史。
 */
static void hook_log(LONG seq, const char* event, const char* kv) {
    CreateDirectoryA(OPT_DIR, NULL);
    char path[MAX_PATH];
    sprintf(path, "%s%chook.log", OPT_DIR, SEP);
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

/* 把产物交给外部改写器走一趟，成功则**就地**写回原缓冲区。
 *
 * MVP 的三条约束都落在这个函数里：
 *   · 长度必须与原件相 —— 不等就放弃（因此不必动 out_len、不必动 8 字节头）
 *   · 不换缓冲区 —— 直接覆盖原缓冲，于是没有「新内存谁分配、谁释放」的问题
 *   · 任何一步失败都静默放弃 —— 游戏绝不该因为优化器出问题而受影响
 */
static void optimize_inplace(void* buf, long long len, LONG seq, void* out) {
    if (!buf || len <= 0 || len > (64LL << 20)) return;
    CreateDirectoryA(OPT_DIR, NULL);

    char raw[MAX_PATH], opt[MAX_PATH], cmd[4 * MAX_PATH], err[MAX_PATH];
    char entry[MAX_PATH];
    sprintf(raw, "%s%c%03ld.raw.bin", OPT_DIR, SEP, seq);
    sprintf(opt, "%s%c%03ld.opt.bin", OPT_DIR, SEP, seq);
    sprintf(err, "%s%copt_stderr.log", OPT_DIR, SEP);
    sprintf(entry, "%s%c%03ld.entry.txt", OPT_DIR, SEP, seq);
    DeleteFileA(entry);   /* 防残留：上一轮同号的入口文件不能影响本轮 */

    /* 读 rewrite 配置：**这里不判断合法性，连字符集也不查** —— 合法值集合只在
       改写器的 VALID_MODES 定义一次。这边要是也维护一份，漏一个值就会像
       2026-10-05 的 short_cse 那样被静默改写成 on（hook.log 标 on、改写器实跑
       short_cse，两份日志对不上）。值为空就不传 --mode，让改写器自己读 opt.ini。 */
    char mode[32] = "";
    GetPrivateProfileStringA("opt", "rewrite", "", mode, sizeof(mode),
                              OPT_PROJ "\\opt.ini");
    /* 写进 hook.log（起子进程前）：监听主频的程序要拿「编译时刻」的值 ——
       这一行与下面传给改写器的值同源，必然一致。 */
    char tag[64];
    sprintf(tag, "rewrite=%s", mode[0] ? mode : "(空)");
    hook_log(seq, "cfg", tag);
    if (!write_file(raw, buf, len)) { hook_log(seq, "error", "stage=raw"); return; }

    /* 把 Result[2]（旧锚点偏移）与模式一起传给改写器 —— 缩短模式必须同步更新
       锚点；模式由转发层传过去，是为了让配置**只被读一次、只被判定一次**。 */
    if (mode[0])
        sprintf(cmd, "\"%s\" \"%s\" --in \"%s\" --out \"%s\" --entry %llX --mode %s",
                PY_EXE, OPT_SCRIPT, raw, opt,
                out ? *(long long*)((char*)out + 16) : 0LL, mode);
    else
        sprintf(cmd, "\"%s\" \"%s\" --in \"%s\" --out \"%s\" --entry %llX",
                PY_EXE, OPT_SCRIPT, raw, opt,
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
                                  CREATE_NO_WINDOW, NULL, OPT_PROJ, &si, &pi);
    if (sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
    if (!started) { hook_log(seq, "error", "stage=spawn"); return; }

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
        hook_log(seq, "error", kv);
        return;
    }

    HANDLE h = CreateFileA(opt, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { hook_log(seq, "error", "stage=optbin"); return; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart != len) {
        CloseHandle(h);
        hook_log(seq, "error", "stage=length");
        return;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)len, &got, NULL);
    CloseHandle(h);
    if (!ok || got != (DWORD)len) { hook_log(seq, "error", "stage=read"); return; }
    char kvw[64];
    sprintf(kvw, "len=%lld", len);
    hook_log(seq, "writeback", kvw);

    /* 缩短模式：改写器把平移后的入口偏移写在 NNN.entry.txt，
       必须同步改回 Result[2]，否则游戏按旧偏移调进指令中间（实测必崩） */
    long long v = 0;
    if (read_entry(entry, &v) && v > 0 && v < len) {
        *(long long*)((char*)out + 16) = v;
        char kve[48];
        sprintf(kve, "offset=0x%llX", v);
        hook_log(seq, "entry", kve);
    }
}

/* 加载时清空 OUT_DIR 下的 `.si` / `.bin` / `.tsv`（不含子目录）。
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
            !ends_with_ci(fd.cFileName, ".bin") &&
            !ends_with_ci(fd.cFileName, ".tsv")) continue;   /* manifest.tsv 同批清 */
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
 * 只删这四类（`.raw.bin` / `.opt.bin` / `.entry.txt` / `.log`），不整目录删 —— 免得误伤
 * 将来往这里放的东西。
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
            !ends_with_ci(fd.cFileName, ".entry.txt") &&
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
    const void* sp = NULL;                 /* 源码数据（ptr + 8）与长度，留给 manifest */
    long long slen = 0;
    int s_wrote = 0;
    if (src) {
        long long len = *(long long*)src;
        void* ptr = *(void**)((char*)src + 8);
        if (ptr) {
            sp = (const char*)ptr + 8;     /* 数据在 ptr + 8 */
            slen = len;
            s_wrote = dump("src", ".si", sp, len, seq);
        }
    }
    if (!g_compile) return NULL;
    void* ret = g_compile(out, src, flags);
    if (out) {
        long long olen = *(long long*)out;
        void* optr = *(void**)((char*)out + 8);
        int o_wrote = 0;
        if (optr) {
            o_wrote = dump("out", ".bin", optr, olen, seq);
            /* MVP：让产物去 Python 里转一圈，原样写回。任何一步失败都不影响返回。 */
            optimize_inplace(optr, olen, seq, out);
        }
        /* 放在写回之后：r2 记录的是改写器更新后的最终锚点。
           sha 失败留 "-"，文件名按 dump 是否真的落了盘来写。 */
        char ssha[65] = "-", osha[65] = "-", sname[32] = "-", oname[32] = "-";
        if (s_wrote) {
            sprintf(sname, "hook_src_%03ld.si", seq);
            sha256_hex(sp, slen, ssha);
        }
        if (o_wrote) {
            sprintf(oname, "hook_out_%03ld.bin", seq);
            sha256_hex(optr, olen, osha);
        }
        manifest_line(seq, flags, sname, slen, ssha, oname, olen, osha, out);
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
