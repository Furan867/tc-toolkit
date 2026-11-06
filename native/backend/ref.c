/* 后端之一：**原实现** —— `compile_real.dll` 的 `compile`，reshipped 成同一个 `compiler_backend`。
 *
 * 为什么它算一个后端而不是壳里的一个分支：`opt.ini` 的 `compiler=ref` 与「ours 失败后回退」走的
 * 是同一条路（调原实现、把它的返回值原样交回游戏），壳不该为它留第二处实现判断
 * （`docs/specs/2026-11-05-toolkit-architecture.md` §5.2 / §10）。
 *
 * 原实现的三个入参都是**游戏给的原件**：`out`（那个 40 字节 Result）、`src`（NimStr 指针）、
 * `flags`。所以本后端用 `src_raw` 而不是拆好的 `src/slen` —— NimStr 的布局只有壳解读一次，
 * 这里不复制第二份。
 */
#include <windows.h>

#include "backend.h"
#include "forward.h"                    /* forward_real_symbol：按自身模块路径找原实现 */

typedef void* (*compile_fn)(void* out, void* src, int flags);

/* 惰性加载一次就缓存。`forward_real_symbol` **不带缓存**（每次调用都会 LoadLibrary 一遍）——
 * 所以这里必须存一份，否则每次 compile() 都要重新加载 + 查导出表。
 * 加载失败不缓存：原实现缺失是部署问题，下一次调用还要再试（与壳的 `ensure_real` 同一条规矩）。 */
static compile_fn ref_compile(void) {
    static compile_fn sym = NULL;
    if (!sym) sym = (compile_fn)forward_real_symbol(REAL_DLL, "compile");
    return sym;
}

static const char* backend_ref_run(void* out, const char* src, long long slen, void* src_raw,
                                   const char* stdlib_path, int flags, backend_run* r) {
    (void)src; (void)slen;              /* 原实现收 NimStr 原件（src_raw），不收拆好的数据 */
    (void)stdlib_path;                  /* 前缀是原实现自己在内部拼的，开关对它无作用 */
    compile_fn f = ref_compile();
    if (!f) return KIND_INTERNAL;       /* dll 缺失 / 没有 compile 导出；错误码本来就在 GetLastError */
    r->ret = f(out, src_raw, flags);    /* 原实现的返回值**原样**回给游戏 */
    return NULL;
}

static void backend_ref_free_blob(void* blob) { (void)blob; }   /* 产物由原实现分配，我们不持有 */

static const char* backend_ref_last_error(void) { return NULL; }    /* 没有诊断串这条通道 */

static const char* backend_ref_last_warnings(void) { return NULL; }

const compiler_backend backend_ref = {
    "ref",
    backend_ref_run,
    backend_ref_free_blob,
    backend_ref_last_error,
    backend_ref_last_warnings,
};
