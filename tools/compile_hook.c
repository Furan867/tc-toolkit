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
 *       hook_src_005.si 与 hook_out_005.bin 就是一对。计数器是本 DLL 的全局变量，**每次加载（游戏启动）都从 1 重新编号**，
 *       因此加载时会清空目录里的 .si/.bin/.tsv 与 hook.log（不含子目录），并把 OPT_DIR 里的
 *       中间产物（NNN.raw.bin / NNN.opt.bin / *.log）一并清掉 —— 两边的编号
 *       是同一套，只清一边就会新旧同号混在一起。
 *
 * 入口
 * ----
 * `opt.ini` 的 `[opt] compiler=` 选实现：`ref`（默认）/ `ours` / `ours_with_fallback`，
 * 语义见 `docs/specs/2026-10-15-simplexc-interfaces.md` §5；**每次** compile() 都写一条
 * `impl` 事件到 `hook.log`（含 ref —— 回退率需要分母），键 `impl` / `mode` / `result`，
 * 词表 `ref | ours | fallback | ours_failed` 与 manifest 的 `impl` 列同源。
 *
 * 失败路径：ours 段的失败点归到 §8 的分类串（`error` 事件 `stage=ours` + `kind=`；
 * 超时 `stage=ours_timeout`、熔断 `stage=ours_fault`），**并照常写 manifest**（`impl=ours_failed`
 * 或 `fallback`）；`ours` 段还有两个兜底：耗时超预算（默认 5 s）与 VEH 熔断（本进程内
 * 永久不再进 ours 段）。任何失败都返回形状合法的 Result，绝不把异常传给游戏。
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
 *   不再是「一个 gcc 单文件命令」：本文件仍是 **C**（不要改成 C++），但它链接我们的
 *   C++ 编译器核心（`simplexc/shell/simplexc_core.h` 是两者之间唯一的 C 边界）。
 *   构建入口：`python simplexc/tools/build.py simplexc_dll`（或部署入口
 *   `python tools/deploy_compile_hook.py build-deploy`，它转调同一个 target）。
 *   链接用 PATH 上的 g++（要 C++ 运行库），LLVM-C 的头与导入库由 CMake 的 `LLVMC::LLVMC`
 *   目标给（`simplexc/.deps/llvm`）；部署时**必须**把 `LLVM-C.dll` 一起拷进游戏目录（接口 §7）——
 *   CMake 已从 `simplexc/.deps/llvm/bin/` 拷到 `build/`，部署拿 `build/LLVM-C.dll`。
 *
 *   无第三方链接依赖：manifest.tsv 的 sha256 走系统 bcrypt.dll 的按需绑定
 *   （见 cng_bind / sha256_hex），所以不用 -lbcrypt、也不 include <bcrypt.h>。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REAL_DLL "compile_real.dll"
#define SEP      92        /* 反斜杠的 ASCII —— 写成字符字面量在多轮转义里会失真 */

/* 路径：三层优先级 —— `opt.ini` 的 `[opt] toolkit_root`（有且非空）> 编译期宏 > `"."`。
 *
 * 为什么不再写死绝对路径：转发层住在**游戏目录**里，它无从知道仓库在哪
 * （`compile_real.dll` 能用自身模块路径推导，是因为它与我们同在游戏目录 —— 见
 * ensure_real）；而仓库位置一台机器一个值。现在由 CMake 传
 * `-DTC_TOOLKIT_ROOT=<仓库根>` 注入，源码里不再有机器相关的路径。
 *
 * 运行时那一层（`opt.ini` 的 `toolkit_root`）是给「库已经部署进游戏目录、还想换工作目录」
 * 用的：不必重编。
 *
 * clangd 不走 CMake、拿不到 `-D`，所以下面每个宏都必须有**能静态解析的默认值** ——
 * 占位 `"."`（进程当前目录），不能 `#error`。
 *
 * `OUT_DIR` / `OPT_DIR` / `OPT_INI` / `OPT_SCRIPT` / `HOOK_LOG_DIR` 仍然可以被 `-D` 覆盖：
 * 离线判据（simplexc/tools/hook_offline.py）靠它把工作目录隔离到 work/tmp/hook_offline/，
 * 不能碰活数据 —— 转发层加载时会清空 OUT_DIR / OPT_DIR（见 purge_*），测试构建若共用真路径
 * 就会把游戏抓到的捕获和 work/hook/hook.log 一起洗掉。 */
#ifndef TC_TOOLKIT_ROOT
#define TC_TOOLKIT_ROOT "."
#endif
#ifndef OUT_DIR
#define OUT_DIR  TC_TOOLKIT_ROOT "\\work\\hook"
#endif

/* hook.log 的目录：默认就落在捕获目录（OUT_DIR = work/hook）里。
 *
 * 为什么单列一个宏：离线判据（simplexc/tools/hook_offline.py）从 case 的 opt/ 下读
 * 日志，得把它 -D 到那里的同一个位置；正式构建一个 -D 都不给，于是取默认值。 */
#ifndef HOOK_LOG_DIR
#define HOOK_LOG_DIR OUT_DIR
#endif

/* 产物改写器（MVP：把产物交给外部 Python 走一趟，再**就地**写回原缓冲） */
#ifndef OPT_DIR
#define OPT_DIR     TC_TOOLKIT_ROOT "\\work\\opt"
#endif
#ifndef OPT_PROJ
#define OPT_PROJ    TC_TOOLKIT_ROOT
#endif
#ifndef OPT_SCRIPT
#define OPT_SCRIPT  OPT_PROJ "\\tools\\optimize_image.py"
#endif
#ifndef OPT_INI
#define OPT_INI     OPT_PROJ "\\opt.ini"
#endif
/* stdlib 前缀：相对项目根（`OPT_PROJ`）的路径。它不在游戏目录里，所以必须靠这个宏推出来；
   读不到时核心报错（见 `simplexc_core.h`），不会静默退硬编码。 */
#define STDLIB_PREFIX OPT_PROJ "\\simplexc\\corpus\\stdlib_prefix.si"
#define PY_EXE      "C:\\Windows\\py.exe"    /* 系统自带的启动器，不依赖 PATH */
#define OPT_TIMEOUT 8000                      /* 子进程最多等 8 秒，超时就放弃 */

/* 生效的路径：上面那些宏是**默认值**，运行时还要被 `opt.ini` 的 `toolkit_root` 顶一次，
 * 所以调用点一律读这里的字段，不再直接读宏。
 * `ini` 例外 —— 它只能来自编译期：要读 `toolkit_root` 得先知道 opt.ini 在哪，自举不能绕回去。 */
typedef struct {
    char out_dir[MAX_PATH];    /* 捕获（hook_src_NNN.si / hook_out_NNN.bin / manifest.tsv）*/
    char opt_dir[MAX_PATH];    /* 改写器中间产物 */
    char proj[MAX_PATH];       /* 改写子进程的 cwd */
    char script[MAX_PATH];     /* tools/optimize_image.py */
    char ini[MAX_PATH];        /* opt.ini */
    char stdlib[MAX_PATH];     /* simplexc/corpus/stdlib_prefix.si */
    char log_dir[MAX_PATH];    /* hook.log 所在目录 */
} toolkit_paths;

static toolkit_paths g_paths;
static int g_paths_ready = 0;

/* `dst = <dir>\<name>`，`dir` 末尾已带分隔符就不重复加。
 * 不直接用 `sprintf`：这条拼接会拿到 `opt.ini` 里人写的 root（长度不受我们控制）。 */
static void path_join(char* dst, size_t cap, const char* dir, const char* name) {
    size_t n = 0;
    while (dir[n] && n + 1 < cap) { dst[n] = dir[n]; n++; }
    if (n && dst[n - 1] != SEP && dst[n - 1] != '/' && n + 1 < cap) dst[n++] = (char)SEP;
    while (*name && n + 1 < cap) dst[n++] = *name++;
    dst[n] = 0;
}

/* 解析一次，之后只读。
 *
 * `toolkit_root` 非空时顶掉三者（OUT_DIR / OPT_DIR / OPT_PROJ），派生的四份跟着重算 ——
 * 其中 hook.log 跟着**新的**捕获目录走：root 换到别处以后，日志留在旧目录里等于两边都对不上号。
 */
static void toolkit_paths_init(void) {
    if (g_paths_ready) return;
    lstrcpynA(g_paths.out_dir, OUT_DIR, MAX_PATH);
    lstrcpynA(g_paths.opt_dir, OPT_DIR, MAX_PATH);
    lstrcpynA(g_paths.proj, OPT_PROJ, MAX_PATH);
    lstrcpynA(g_paths.script, OPT_SCRIPT, MAX_PATH);
    lstrcpynA(g_paths.ini, OPT_INI, MAX_PATH);
    lstrcpynA(g_paths.stdlib, STDLIB_PREFIX, MAX_PATH);
    lstrcpynA(g_paths.log_dir, HOOK_LOG_DIR, MAX_PATH);

    char root[MAX_PATH];
    root[0] = 0;
    GetPrivateProfileStringA("opt", "toolkit_root", "", root, sizeof(root), OPT_INI);
    if (root[0]) {
        lstrcpynA(g_paths.proj, root, MAX_PATH);
        path_join(g_paths.out_dir, MAX_PATH, root, "work\\hook");
        path_join(g_paths.opt_dir, MAX_PATH, root, "work\\opt");
        path_join(g_paths.script, MAX_PATH, root, "tools\\optimize_image.py");
        path_join(g_paths.stdlib, MAX_PATH, root, "simplexc\\corpus\\stdlib_prefix.si");
        lstrcpynA(g_paths.log_dir, g_paths.out_dir, MAX_PATH);
    }
    g_paths_ready = 1;
}

/* 同目录的 C 边界（纯 C 头，冻结版）：核心（C++17）实现它，本文件只调它。
 * 用相对路径 include：转发层的编译命令就没有额外的 -I 要维护（它两个入口共用）。 */
#include "../simplexc/shell/simplexc_core.h"

typedef void* (*compile_fn)(void* out, void* src, int flags);
typedef void  (*void_fn)(void);

static HMODULE    g_real = NULL;
static compile_fn g_compile = NULL;
static LONG       g_seq = 0;
static void*      g_ours_prev = NULL;    /* 我们上次给游戏的产物缓冲（bit62 置位 → 游戏不 free） */

/* ---- 选实现：`opt.ini` 的 `[opt] compiler=`（见 docs/specs/…interfaces.md §5）
 *
 * 合法值只在 `resolve_compiler()` 里定义一次（“静默改值”远比报错难查，见 pitfalls）。
 * 读不到 / 非法 → **ref**：绝不能把「没配」当成「我们的实现」。
 */
static const char* resolve_compiler(void) {
    static char mode[32] = "";
    GetPrivateProfileStringA("opt", "compiler", "ref", mode, sizeof(mode), g_paths.ini);
    if (lstrcmpA(mode, "ours") == 0) return "ours";
    if (lstrcmpA(mode, "ours_with_fallback") == 0) return "ours_with_fallback";
    return "ref";
}

/* ---- stdlib 前缀开关：`opt.ini` 的 `[opt] stdlib`（默认 **on**）
 *
 * on  = 编译时把 `simplexc/corpus/stdlib_prefix.si` 拼在源码前面 —— 参考编译器的 `compile()`
 *       内部就是自动这么做的，所以这才是真路径；
 * off = 走我们硬编码的 runtime（`sema/analyze.cc` 的 `prefix_funcs()` + `global_seed` + `#U64_HASH`）。
 *
 * **`compiler = ref` 时这个键无作用，而且这不是「碰巧没作用」**：`ref` 根本不进 ours 段，
 * 也就不调 `simplexc_compile()`（见 `compile()` 的 `is_ref` 分支）；参考编译器的前缀由它自己
 * 在内部拼，与我们这个键无关 —— 两边产物逐字节相同（离线判据里量过）。
 *
 * 读不到 / 非法 → on（真路径）；配的原样值由 `raw` 带回，整行入选 `impl` 行（`stdlib_raw=`）——
 * 写错（`of`）会看起来与 on 一样能编，日志里必须留下痕迹。 */
static int resolve_stdlib(char* raw, DWORD raw_cap) {
    GetPrivateProfileStringA("opt", "stdlib", "on", raw, raw_cap, g_paths.ini);
    return lstrcmpA(raw, "off") != 0;   /* 只有明确的 off 才关 */
}

/* ---- 失败分类（接口冻结版 1 §8）-----------------------------------------
 * 这里的三个串是**壳自己**会产生的失败：内部错误（没有源码 / 核心给的形状不合法）、
 * 超预算、熔断。源码侧的六个分类（`UserError` / `UnsupportedConstruct` / `BlobTooLarge` /
 * `RelocUnsupported` / `SymbolUndefined` / `InternalError`）由核心返回，
 * 它们的唯一一处定义在 `simplexc/shell/simplexc_core.h`（那边是 C++，与本文件无法共享宏）。
 * `UserError` 只在这里多一个用法：回退决策要认出它（用户错不转交），但仍没在这里定义。 */
#define KIND_INTERNAL   "InternalError"
#define KIND_TIMEOUT    "Timeout"
#define KIND_FAULT      "OursFault"
#define KIND_USER_ERROR "UserError"

/* 核心给的失败还带着三轴诊断串（`sema/用户源码错 第 12 行：…`，`simplexc_last_error()`）。
 * 为什么存到文件级静态缓冲：`ours_compile_ours` 拿到 kind 的地方与 `ours_fail_result` /
 * `kind=` 日志行用到它的地方隔着一个段（`ours_segment`），而诊断串只在**同一次调用**里有效
 * —— 壳在 `ours_compile_ours` 入口清它，不存在跨调用串值。
 * 壳自己判的失败（没源码 / 形状不合法）没有诊断，那时它是空串。 */
static char g_kind_detail[512];

/* 把 `s` 接到 `buf` 尾（显式截断，绝不越界）；返回新的已用长度。
 * 为什么不用 `sprintf`/`strcat`：拼进去的是用户源码里的名字，长度不受我们控制；
 * 而 `strcat` 还要先手算 `strlen`，那正是最容易算错的地方。 */
static size_t append_str(char* buf, size_t cap, size_t used, const char* s) {
    while (*s && used + 1 < cap) buf[used++] = *s++;
    buf[used] = 0;
    return used;
}

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
    GetPrivateProfileStringA("opt", OURS_TIMEOUT_INI_KEY, "", buf, sizeof(buf), g_paths.ini);
    unsigned long long v = parse_ms(buf);
    return v == (unsigned long long)-1 ? OURS_TIMEOUT_MS : v;
}

/* 自研路径的产物：**进程内**调我们的 C++ 编译器核心（`simplexc_compile`），拿回一块内存里的产物。
 *
 * 三条不变量（写错了会崩游戏，别动）：
 *   · 缓冲是核心 **malloc** 的，头 word 的 **bit62 置位** → 游戏不拿 Nim 的分配器去 free 我们的指针；
 *   · `out[0]` = 缓冲总长（含 8 字节头），缓冲实际分配 **out[0] + 8** 字节（游戏会多读 8 字节）；
 *   · 上一次的缓冲在下一次编译开始时释放，而且必须用 `simplexc_free` 放（跨 C 边界的所有权）。
 *
 * 返回 NULL = 成功，否则是 §8 分类串。分类串由核心给（UnsupportedConstruct / InternalError /
 * BlobTooLarge / RelocUnsupported / SymbolUndefined），只有「没源码」与「形状不合法」是壳自己判的。
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
        append_str(g_kind_detail, sizeof g_kind_detail, 0, simplexc_last_error());
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
                                unsigned long long* budget_ms) {
    *installed = ours_guard_install();
    *budget_ms = ours_budget_ms();
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
    if (elapsed > *budget_ms) {
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

/* `ours` 模式的失败 Result（以及回退模式下**故意不转交**的那一类）：**必须给非 0 的 out[3]**，
 * 否则游戏会走成功路径、在 jit 里因 len<=0 抛 index error（见 docs/facts.md）。返回值的形状由
 * 游戏自己拼，这里返回 NULL 即可（真正决定走向的是 out[3]）。
 *
 * 内容 = `ours: ` + §8 分类串（接口冻结版 1 §5）+ ` @ ` + 三轴诊断（有的话）—— 回退率报告按
 * 前缀分类；诊断串是给**人**看的（游戏是 GUI 进程，看不到我们的 stderr，只有这条路能出去）。 */
static void ours_fail_result(void* out, long long seq, const char* kind) {
    char why[768];
    size_t n = append_str(why, sizeof why, 0, "ours: ");
    n = append_str(why, sizeof why, n, kind ? kind : KIND_INTERNAL);
    if (g_kind_detail[0]) {
        n = append_str(why, sizeof why, n, " @ ");
        n = append_str(why, sizeof why, n, g_kind_detail);
    }
    unsigned long long* desc = (unsigned long long*)malloc(n + 9);
    if (!desc) return;
    desc[0] = ((unsigned long long)n) | (1ULL << 62);   /* bit62 → 游戏不 free */
    memcpy((char*)desc + 8, why, n + 1);
    *(long long*)((char*)out + 0) = 0;
    *(void**)((char*)out + 8) = NULL;
    *(long long*)((char*)out + 16) = 0;
    *(long long*)((char*)out + 24) = (long long)n;
    *(void**)((char*)out + 32) = (char*)desc + 8;
    (void)seq;
}

/* `hook_log` 在下面定义（中间隔着 dump/manifest 一段）：先声明，因为「加载失败」
   是必须留痕的事件，而痕迹只能写在 ensure_real 里 —— 只有那里拿得到 GetLastError
   （126 = 找不到文件 / 193 = 位数不对 / 127 = 没有 compile 导出）。 */
static void hook_log(LONG seq, const char* event, const char* kv);

static void ensure_real(void) {
    static int fail_logged = 0;
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
    /* 加载失败以前**零记录**：`compile()` 里 `if (!g_compile) return NULL` 不给 out 写任何字段，
       hook.log 也没有行 —— 游戏按 out[3]==0 判成功，于是「我们的部署错了」被记成「什么都没发生」
       （P3）。这里补一行，带 Win32 错误码；只记一次 —— NimMain / NimDestroyGlobals 也会走到本函数，
       否则每次调用都添一行。 */
    if (!g_compile && !fail_logged) {
        DWORD err = GetLastError();
        char kv[160];
        fail_logged = 1;
        snprintf(kv, sizeof kv, "stage=ensure_real\tkind=%s\tdetail=%s err=%lu",
                 KIND_INTERNAL, REAL_DLL, (unsigned long)err);
        hook_log(0, "error", kv);
    }
}

/* 返回是否真的写出了文件 —— manifest 要按它决定写文件名还是 "-"，
   免得记录里指着一个因超限/失败而根本没落盘的捕获。 */
static int dump(const char* tag, const char* ext, const void* p, long long n, LONG seq) {
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
                          const void* out, const char* impl, const char* stdlib) {
    if (!out) return;                      /* 没有 out 缓冲就没什么可记 */
    CreateDirectoryA(g_paths.out_dir, NULL);
    char path[MAX_PATH];
    sprintf(path, "%s%cmanifest.tsv", g_paths.out_dir, SEP);
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
                               "\tout\tout_len\tout_sha256\tr0\tr1\tr2\tr3\tr4\timpl\tstdlib\r\n");
    n += sprintf(line + n,
                 "%03ld\t%04d-%02d-%02d %02d:%02d:%02d\t0x%X\t%s\t%lld\t%s\t%s\t%lld\t%s"
                 "\t0x%016llX\t0x%016llX\t0x%016llX\t0x%016llX\t0x%016llX\t%s\t%s\r\n",
                 seq, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 (unsigned)flags, src_name, src_len, src_sha, out_name, out_len, out_sha,
                 (unsigned long long)r[0], (unsigned long long)r[1],
                 (unsigned long long)r[2], (unsigned long long)r[3],
                 (unsigned long long)r[4], impl ? impl : "-", stdlib ? stdlib : "-");
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

/* 把产物交给外部改写器走一趟，成功则**就地**写回原缓冲区。
 *
 * MVP 的三条约束都落在这个函数里：
 *   · 长度必须与原件相 —— 不等就放弃（因此不必动 out_len、不必动 8 字节头）
 *   · 不换缓冲区 —— 直接覆盖原缓冲，于是没有「新内存谁分配、谁释放」的问题
 *   · 任何一步失败都静默放弃 —— 游戏绝不该因为优化器出问题而受影响
 */
static void optimize_inplace(void* buf, long long len, LONG seq, void* out, const char* mode) {
    if (!buf || len <= 0 || len > (64LL << 20)) return;
    CreateDirectoryA(g_paths.opt_dir, NULL);

    char raw[MAX_PATH], opt[MAX_PATH], cmd[4 * MAX_PATH], err[MAX_PATH];
    char entry[MAX_PATH];
    sprintf(raw, "%s%c%03ld.raw.bin", g_paths.opt_dir, SEP, seq);
    sprintf(opt, "%s%c%03ld.opt.bin", g_paths.opt_dir, SEP, seq);
    sprintf(err, "%s%copt_stderr.log", g_paths.opt_dir, SEP);
    sprintf(entry, "%s%c%03ld.entry.txt", g_paths.opt_dir, SEP, seq);
    DeleteFileA(entry);   /* 防残留：上一轮同号的入口文件不能影响本轮 */

    if (!write_file(raw, buf, len)) { hook_log(seq, "error", "stage=raw"); return; }

    /* 把 Result[2]（旧锚点偏移）与模式一起传给改写器 —— 缩短模式必须同步更新
       锚点；模式由调用方传进来，是为了让配置**只被读一次、只被判定一次**。 */
    if (mode[0])
        sprintf(cmd, "\"%s\" \"%s\" --in \"%s\" --out \"%s\" --entry %llX --mode %s",
                PY_EXE, g_paths.script, raw, opt,
                out ? *(long long*)((char*)out + 16) : 0LL, mode);
    else
        sprintf(cmd, "\"%s\" \"%s\" --in \"%s\" --out \"%s\" --entry %llX",
                PY_EXE, g_paths.script, raw, opt,
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
    lstrcpyA(pat, g_paths.out_dir);
    lstrcatA(pat, "\\*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;         /* 目录还不存在，没什么可清的 */
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;   /* 不碰子目录 */
        /* hook.log 与捕获同批清：它是**一次游戏会话**的记录（编号随加载从 1 重来），
           留着上一会话的同号行，按编号回查归属时（ownership 同号取最后一条）会摸到
           上一会话的值 —— 与 purge_workdir 挡的是同一个坑。 */
        if (lstrcmpiA(fd.cFileName, "hook.log") != 0 &&
            !ends_with_ci(fd.cFileName, ".si") &&
            !ends_with_ci(fd.cFileName, ".bin") &&
            !ends_with_ci(fd.cFileName, ".tsv")) continue;   /* manifest.tsv 同批清 */
        lstrcpyA(path, g_paths.out_dir);
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
    lstrcpyA(pat, g_paths.opt_dir);
    lstrcatA(pat, "\\*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;         /* 目录还不存在 */
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!ends_with_ci(fd.cFileName, ".raw.bin") &&
            !ends_with_ci(fd.cFileName, ".opt.bin") &&
            !ends_with_ci(fd.cFileName, ".entry.txt") &&
            !ends_with_ci(fd.cFileName, ".log")) continue;
        lstrcpyA(path, g_paths.opt_dir);
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
        /* 路径先解析：下面两个 purge 清哪个目录全看它（`opt.ini` 的 `toolkit_root` 只在这里
           读一次，之后每次 compile() 用同一组值）。 */
        toolkit_paths_init();
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

    /* 选实现 + 跑 ours 段。§5 的词表与三种模式一一对应（左列与 `impl=` 这次**同词**：两者是两
       个轴（`mode=` 是配了什么，`impl=` 是实际走了哪个），但词表变更时得一起改，见接口 §5）：
     *   ref             → impl=ref        result=ref（**每次调用都记**，回退率要分母）
     *   ours 成功       → impl=ours       result=ok
     *   fallback 成功   → impl=fallback   result=ref
     *   ours 失败       → impl=ours_failed result=fail
     * 熔断（g_breaker）后不再进 ours 段，按模式当"ours 失败"处理。 */
    const char* mode = resolve_compiler();
    int is_ref = lstrcmpA(mode, "ref") == 0;
    /* stdlib 开关：`compiler = ref` 时它仍然被读、也被记，但对产物**无作用** —— 参考那条路
       不调 `simplexc_compile()`，前缀是参考编译器自己内部拼的（见 `resolve_stdlib` 的注释）。 */
    char stdlib_raw[16];
    const int stdlib_on = resolve_stdlib(stdlib_raw, sizeof stdlib_raw);
    const char* stdlib = stdlib_on ? "on" : "off";
    const char* impl = NULL;
    const char* result = NULL;
    const char* kind = NULL;               /* §8 分类串；NULL = 没失败 */
    int ours_ok = 0, ours_attempted = 0, guard_installed = 0, need_reference = 0;
    unsigned long long budget_ms = 0;

    if (is_ref) {
        impl = "ref"; result = "ref"; need_reference = 1;
    } else {
        if (out && !g_breaker) {
            ours_attempted = 1;
            kind = ours_segment(out, (const char*)sp, slen, stdlib_on ? g_paths.stdlib : NULL,
                                &guard_installed, &budget_ms);
            ours_ok = kind == NULL;
        } else {
            /* 没有 out 缓冲（无处写产物）/ 已熔断：不试 ours */
            kind = out ? KIND_FAULT : KIND_INTERNAL;
        }
        if (ours_ok) { impl = "ours"; result = "ok"; }
        else if (lstrcmpA(mode, "ours") == 0) { impl = "ours_failed"; result = "fail"; }
        /* 回退模式下还有一类**故意不转交**的失败：`UserError` = 用户能自己改对的源码错。
           为什么：转交只会白跑一次参考编译，而且往回退率的分母里偷一个「我们的 bug」——
           回退率是关掉回退的唯一量化依据。其余分类（未实现 / `OurBug` / 超时 / 熔断）照旧转交：
           那几档确实是「我们不行」，让原版顶上是对的。 */
        else if (lstrcmpA(kind, KIND_USER_ERROR) == 0) { impl = "ours_failed"; result = "fail"; }
        else { impl = "fallback"; result = "ref"; need_reference = 1; }
    }

    /* 编译期的**非致命观察**（解析期 notes、「不可达 def 被跳过」）以前在编译路径上没有任何
       消费者：C++ 侧只往一个没人读的 `stats.warnings` 里写，而游戏是 GUI 进程、看不到我们的
       stderr —— 于是「这里被略过了什么」查不到（P2/P6）。这里落一行：事件名用 `warn`（不是
       `error`），回退率按 `error` 行统计，不会被这些条目污染。`detail=` 与 `error` 行同形，
       消费者忽略不认识的事件名。 */
    if (ours_attempted) {
        const char* warns = simplexc_last_warnings();
        if (warns && warns[0]) {
            char kvw[260]; /* `hook_log` 行缓冲 320 减固定前缀后的额度；超了由 snprintf 截断 */
            snprintf(kvw, sizeof kvw, "detail=%s", warns);
            hook_log(seq, "warn", kvw);
        }
    }

    /* 失败先记账。为什么放在这里而不是各自的 return 点上：早先版本 ours 失败直接
       return，manifest 写在下面的 `if (out)` 块里够不到 —— 按 manifest 统计回退率会
       漏掉**全部** ours 失败。
       超时/熔断用各自的 stage 名（§5），其余分类用 stage=ours + kind=分类串。 */
    if (kind) {
        const char* stage = "ours";
        if (lstrcmpA(kind, KIND_TIMEOUT) == 0)      stage = "ours_timeout";
        else if (lstrcmpA(kind, KIND_FAULT) == 0)   stage = "ours_fault";
        /* `detail=` 是三轴诊断（`sema/用户源码错 第 12 行：…`）：stderr 只有开发机看得到，
           游戏在别人的机器上跑，而「哪一行、哪个阶段」全在这一串里。缓冲 200 是 `hook_log`
           行缓冲（320）的上限减去固定前缀后剩下的额度，超了就在这里截断，不让它冲爆那边。 */
        char kve[200];
        size_t used = append_str(kve, sizeof kve, 0, "stage=");
        used = append_str(kve, sizeof kve, used, stage);
        used = append_str(kve, sizeof kve, used, "\tkind=");
        used = append_str(kve, sizeof kve, used, kind);
        if (g_kind_detail[0]) {
            used = append_str(kve, sizeof kve, used, "\tdetail=");
            used = append_str(kve, sizeof kve, used, g_kind_detail);
        }
        hook_log(seq, "error", kve);
    }

    /* 失败的两种走向：`ours` 模式、以及回退模式下**故意不转交**的（`UserError`）——
       两者都要给游戏一个形状合法的失败 Result，差別只在 impl 行里的 mode。 */
    void* ret = NULL;
    if (!is_ref && !ours_ok && !need_reference) {
        if (out) ours_fail_result(out, seq, kind);
        ret = NULL;
    } else if (need_reference) {
        ret = g_compile(out, src, flags);
    }

    long long olen = 0;
    void* optr = NULL;
    /* 这趟过不过改写器。改写器是给**参考产物**准备的（它按参考实现的指令形状做等长改写）：
       我们自己的产物不过它 —— 否则就是拿别人尺子改自己的衣服。产物要等参考实现返回才存在，
       所以判定只能放在这里；`impl` 行的 `rewrite=` 依赖它。 */
    int rewrote = 0;
    if (out) {
        olen = *(long long*)out;
        optr = *(void**)((char*)out + 8);
        rewrote = optr != NULL && !ours_ok;
    }
    char rewrite_mode[32] = "";
    if (rewrote) {
        /* 读 rewrite 配置：**这里不判断合法性，连字符集也不查** —— 合法值集合只在
           改写器的 VALID_MODES 定义一次。这边要是也维护一份，漏一个值就会像
           2026-10-05 的 short_cse 那样被静默改写成 on（hook.log 标 on、改写器实跑
           short_cse，两份日志对不上）。值为空就不传 --mode，让改写器自己读 opt.ini。 */
        GetPrivateProfileStringA("opt", "rewrite", "", rewrite_mode,
                                 sizeof(rewrite_mode), g_paths.ini);
    }

    /* impl 行落在这里 —— 编译之后、改写之前。为什么不能更早：`rewrite=` 必须是**事实**
       （有产物且没走自研实现才会起改写器），而产物要等参考实现返回才存在，写早了只能靠猜。
       猜错就会给没起改写器的那趟也标上 `rewrite=`，而「有 `rewrite=` = 起了改写器」是
       `tools/freq/products.py` 的归因判据。代价：参考实现真 abort 掉这一趟就没有 `impl`
       行（hook.log 只加不改，写出去的行补不了），那一趟同样没有 dump 与 manifest。 */
    {
        char kvi[224];
        int n = sprintf(kvi, "impl=%s\tmode=%s\tresult=%s\tstdlib=%s", impl, mode, result, stdlib);
        /* 原样值只在它不被认出来时多记一个键：`stdlib = of` 会静默当 on 用（能编、产物一样），
           而开/关的产物**字节与符号表都不同**，事后归因只靠这一行。 */
        if (lstrcmpA(stdlib_raw, stdlib) != 0) n += sprintf(kvi + n, "\tstdlib_raw=%s", stdlib_raw);
        if (ours_attempted) n += sprintf(kvi + n, "\tbudget_ms=%llu", budget_ms);
        if (guard_installed) n += sprintf(kvi + n, "\tguard=veh");
        /* `rewrite=` 只在起了改写器的这趟带 —— 「有 `rewrite=` = 起了改写器」是归因判据。
           值是**配置值**，与传给 `--mode` 的同源；空值写 `(空)`。 */
        if (rewrote) n += sprintf(kvi + n, "\trewrite=%s", rewrite_mode[0] ? rewrite_mode : "(空)");
        hook_log(seq, "impl", kvi);
    }

    if (out) {
        int o_wrote = 0;
        if (optr) {
            o_wrote = dump("out", ".bin", optr, olen, seq);
            if (rewrote) optimize_inplace(optr, olen, seq, out, rewrite_mode);
        }
        char ssha[65] = "-", osha[65] = "-", sname[32] = "-", oname[32] = "-";
        if (s_wrote) {
            sprintf(sname, "hook_src_%03ld.si", seq);
            sha256_hex(sp, slen, ssha);
        }
        if (o_wrote) {
            sprintf(oname, "hook_out_%03ld.bin", seq);
            sha256_hex(optr, olen, osha);
        }
        manifest_line(seq, flags, sname, slen, ssha, oname, olen, osha, out, impl, stdlib);
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
