/* 后端之一：进程内调我们的 C++ 编译器核心（`simplexc_compile`）。
 *
 * 本文件是**唯一** include `simplexc/shell/simplexc_core.h` 的地方（接口 §5.2）：换编译器实现
 * = 换这个文件 + `backend.h` 里换一行 extern，`native/compile/` 的壳一行不动。
 *
 * 产物（`ours_compile_ours`）的三条不变量，写错了会崩游戏，别动：
 *   · 缓冲是核心 **malloc** 的，头 word 的 **bit62 置位** → 游戏不拿 Nim 的分配器去 free 我们的指针；
 *   · `out[0]` = 缓冲总长（含 8 字节头），缓冲实际分配 **out[0] + 8** 字节（游戏会多读 8 字节）；
 *   · 上一次的缓冲在下一次编译开始时释放，而且必须用 `simplexc_free` 放（跨 C 边界的所有权）。
 *
 * 看门狗（超时预算 + VEH 熔断）也在这里：它包住的是**本实现自己那段**，参考实现的路径上没有它
 * —— 壳不再知道这些（`docs/notes/2026-10-17-shell-failure-paths.md` 的穿透试验）。
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "forward.h"                    /* forward_config_get：超时预算的第二个来源 */
#include "simplexc_core.h"              /* C 边界（纯 C 头）：核心实现它，本文件只调它 */

/* stdlib 前缀的路径由壳算好传进来（`run` 的 `stdlib_path`）：本文件不推自己的安装位置。 */

static void* g_ours_prev = NULL;    /* 我们上次给游戏的产物缓冲（bit62 置位 → 游戏不 free） */

/* 核心给的失败还带着三轴诊断串（`sema/用户源码错 第 12 行：…`，`simplexc_last_error()`）。
 * 为什么存到文件级静态缓冲：拿到 kind 的地方与壳读 `last_error()` 的地方隔着一次 `run` 返回，
 * 而诊断串只在**同一次调用**里有效 —— 本文件在 `ours_compile_ours` 入口清它，不存在跨调用串值。
 * 本实现自己判的失败（没源码 / 形状不合法）没有诊断，那时它是空串。 */
static char g_kind_detail[512];

/* ours 段的超时预算（毫秒）：`TC_OURS_TIMEOUT_MS` 环境变量 > `opt.ini` 的
 * `[opt] ours_timeout_ms` > 默认 5 秒；调试期调大到 60000。
 * 与 `OPT_TIMEOUT` 是两同事：那个只管改写子进程，这个只量 ours 段自身。
 * 特殊值 0 = 任何耗时都算超预算（离线判据用它对真实产物走一遍超时分支）。
 */
#define OURS_TIMEOUT_MS        5000
#define OURS_TIMEOUT_ENV       "TC_OURS_TIMEOUT_MS"
#define OURS_TIMEOUT_INI_KEY   "ours_timeout_ms"

static unsigned long long parse_ms(const char* s) {
    /* 不合法（含空串）返回 -1 表示"这个来源没给值"。写错的值不会报错，但它总会被
       `budget_ms` 记进 hook.log 的 impl 行 —— 静默改值至少留得下痕迹。 */
    if (!s || !s[0]) return (unsigned long long)-1;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end) return (unsigned long long)-1;
    return v;
}

static unsigned long long ours_budget_ms(void) {
    char buf[32];
    DWORD n = GetEnvironmentVariableA(OURS_TIMEOUT_ENV, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        unsigned long long v = parse_ms(buf);
        if (v != (unsigned long long)-1) return v;
    }
    buf[0] = 0;
    forward_config_get(OURS_TIMEOUT_INI_KEY, buf, sizeof(buf));
    unsigned long long v = parse_ms(buf);
    return v == (unsigned long long)-1 ? OURS_TIMEOUT_MS : v;
}

/* 自研路径的产物：**进程内**调我们的 C++ 编译器核心（`simplexc_compile`），拿回一块内存里的产物，
 * 按游戏那个 Result 的形状写进 `out`。
 *
 * 返回 NULL = 成功，否则是 §8 分类串。分类串由核心给（UnsupportedConstruct / InternalError /
 * BlobTooLarge / RelocUnsupported / SymbolUndefined），只有「没源码」与「形状不合法」是本实现自己判的。
 */
static const char* ours_compile_ours(void* out, const char* src, long long slen,
                                     const char* stdlib_path) {
    if (g_ours_prev) { simplexc_free(g_ours_prev); g_ours_prev = NULL; }
    g_kind_detail[0] = 0;                           /* 诊断串只跟**本次**失败走 */
    if (!src || slen <= 0) return KIND_INTERNAL;     /* 没有源码可编（src 缺失或空）*/

    void* blob = NULL;
    long long len = 0, entry = 0;
    const char* kind = simplexc_compile(src, slen, stdlib_path, &blob, &len, &entry);
    if (kind) {                                     /* 失败：三个出参一个都没动 */
        lstrcpynA(g_kind_detail, simplexc_last_error(), (int)sizeof g_kind_detail);
        return kind;
    }

    /* 核心的返回值已经过自己的校验，这里再守一道：形状不合法就不能交给游戏。
       长度必须装得下 8 字节头、且不超转发层自己的上限（dump() 也是 64 MB）。 */
    /* `entry > len - 8` 而不是 `entry + 8 > len`：后者的加法在 entry 接近 LLONG_MAX 时是有符号溢出（UB），
       而这条判断的整个意义就是「不信核心给的数」——判据本身不能先 UB。len >= 16 已在前面卡住。 */
    if (!blob || len < 16 || len > (64LL << 20) || entry < 0 || entry > len - 8) {
        simplexc_free(blob);
        return KIND_INTERNAL;
    }

    g_ours_prev = blob;
    *(long long*)((char*)out + 0) = len;
    *(void**)((char*)out + 8) = blob;
    *(long long*)((char*)out + 16) = entry;     /* 8 字节写全（游戏按 32 位读、按 64 位用） */
    *(long long*)((char*)out + 24) = 0;         /* err_len = 0 */
    *(long long*)((char*)out + 32) = 0;         /* err_ptr = 0 */
    return NULL;
}

/* ---- ours 段的看门狗：超时预算 + VEH 熔断 -------------------------------
 *
 * VEH **只包 ours 段**：处理器装一次（首次进 ours 段时），作用域由 `g_in_ours`
 * 卡住 —— 参考实现路径上它是 0，异常照原样穿透给游戏自己的处理器（实测见
 * `docs/notes/2026-10-17-shell-failure-paths.md` 的穿透试验）。
 *
 * 触发后：置 `g_ours_fault`、**永久**置熔断标志（本进程内之后所有调用直接按
 * 失败处理，不再进 ours 段），并把上下文搬回 `g_resume` 恢复点 —— 于是控制流
 * 回到 `ours_segment()` 里，异常不往游戏那边传。
 *
 * 处理器里只做三件事：置标志、搬上下文、返回；**不分配内存、不 LoadLibrary、
 * 不写文件**（真正落盘留给调用方，理由与 `hook_log` 的静默风格相反：这里没有
 * 安全的 I/O 手段，硬写可能死在写一半上）。
 */
static volatile LONG g_veh_present  = 0;    /* VEH 已注册（惰性装一次） */
static volatile LONG g_ours_fault  = 0;    /* 本次调用里 ours 段出过异常 */
static volatile LONG g_in_ours     = 0;    /* ours 段正在跑（VEH 只认这个窗口） */
static volatile LONG g_have_resume = 0;    /* g_resume 里有一个有效的恢复点 */
static volatile LONG g_resume_tid  = 0;    /* 抓恢复点时的线程 —— 别线程的异常绝不用它 */
static volatile LONG g_breaker     = 0;    /* 熔断：本进程内永久不再进 ours 段 */
static CONTEXT       g_resume;

static LONG CALLBACK ours_veh(EXCEPTION_POINTERS* ep) {
    if (!g_in_ours || !g_have_resume) return EXCEPTION_CONTINUE_SEARCH;
    /* 整个壳本来就是单线程假设（g_seq / g_ours_prev 也是全局的）；万一有别的线程
       在 ours 窗口里踩了异常，把 A 线程的上下文往 B 线程身上搬会跳到别人的栈帧上
       —— 宁可不接，也不要把"不崩"变成"换个死法"。 */
    if ((LONG)GetCurrentThreadId() != g_resume_tid) return EXCEPTION_CONTINUE_SEARCH;
    switch (ep->ExceptionRecord->ExceptionCode) {
    /* 本体代码会踩的"真"异常；断点 / 单步 / C++ 抛出等一律穿透，别抢 */
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_ours_fault = 1;
    InterlockedExchange(&g_breaker, 1);
    g_in_ours = 0;
    g_have_resume = 0;
    memcpy(ep->ContextRecord, &g_resume, sizeof(CONTEXT));
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* 装一次；返回 1 表示"这次调用是安装者"（只用于在日志里留一条安装证据）。 */
static int ours_guard_install(void) {
    if (g_veh_present) return 0;
    if (AddVectoredExceptionHandler(1, ours_veh)) { g_veh_present = 1; return 1; }
    return 0;
}

/* ours 段（VEH 包住的整段，含编译）。返回 NULL = 成功，否则是 §8 分类串。
 *
 * 计时用 GetTickCount64（毫秒粒度，别拿它量微秒级的差异 —— 这里要判的是"秒级预算"）。
 * `src`/`slen` 是游戏给的那份源码（壳已经从 NimStr 里取出来，与写 `hook_src_NNN.si` 的是同一块）。
 */
static const char* ours_segment(void* out, const char* src, long long slen,
                                const char* stdlib_path, int* installed,
                                unsigned long long budget_ms) {
    *installed = ours_guard_install();
    g_ours_fault = 0;

    /* 恢复点：下面这行之后就是"异常被拦下"回来的入口。
       RtlCaptureContext 存下的就是这一刻的寄存器组（含非易失寄存器与 Rsp），
       处理器把它搬回 ContextRecord 再 CONTINUE_EXECUTION，等于一次 longjmp。 */
    RtlCaptureContext(&g_resume);
    if (g_ours_fault) return KIND_FAULT;
    g_resume_tid = (LONG)GetCurrentThreadId();
    g_have_resume = 1;
    g_in_ours = 1;
    ULONGLONG t0 = GetTickCount64();
    const char* err = ours_compile_ours(out, src, slen, stdlib_path);
    ULONGLONG elapsed = GetTickCount64() - t0;
    g_in_ours = 0;
    g_have_resume = 0;
    if (err) return err;
    if (elapsed > budget_ms) {
        /* 超预算：产物作废。缓冲区由我们持有，直接放掉，并把 out 里指向它的字段
           清干净 —— 后面任何路径都不该读到一个悬垂指针。 */
        if (g_ours_prev) { simplexc_free(g_ours_prev); g_ours_prev = NULL; }
        *(long long*)((char*)out + 0) = 0;
        *(void**)((char*)out + 8) = NULL;
        *(long long*)((char*)out + 16) = 0;
        return KIND_TIMEOUT;
    }
    return NULL;
}

static const char* backend_simplexc_run(void* out, const char* src, long long slen,
                                        void* src_raw, const char* stdlib_path, int flags,
                                        backend_run* r) {
    (void)src_raw;                      /* 我们的核心只收拆好的 `src`/`slen` */
    (void)flags;                        /* 我们的核心不收 flags */
    if (g_breaker) return KIND_FAULT;   /* 熔断：本进程内不再进 ours 段，也不碰 out */
    r->attempted = 1;
    r->budget_ms = ours_budget_ms();
    const char* kind = ours_segment(out, src, slen, stdlib_path, &r->guard_installed, r->budget_ms);
    if (!kind) {
        r->blob     = *(void**)((char*)out + 8);
        r->blob_len = *(long long*)((char*)out + 0);
        r->entry    = *(long long*)((char*)out + 16);
    }
    return kind;                        /* `r->ret` 从此不填 = NULL：ours 成功时壳原本就返回 NULL */
}

static void backend_simplexc_free_blob(void* blob) { simplexc_free(blob); }

static const char* backend_simplexc_last_error(void) { return g_kind_detail; }

static const char* backend_simplexc_last_warnings(void) { return simplexc_last_warnings(); }

const compiler_backend backend_simplexc = {
    "simplexc",
    backend_simplexc_run,
    backend_simplexc_free_blob,
    backend_simplexc_last_error,
    backend_simplexc_last_warnings,
};
