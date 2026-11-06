/* 编译器后端接口 —— `native/compile/` 的 compile() 只认这张表，不认具体实现。
 *
 * 为什么分两层：换编译器实现（我们的核心 / 原实现 / 将来的第三种）只该换一个文件 + 一行 extern，
 * 壳里的产品逻辑（选哪个、回退策略、hook.log、manifest、导出）与实现无关，不该跟着动
 * （`docs/specs/2026-11-05-toolkit-architecture.md` §5.2）。
 *
 * 产物怎么写：**结果一律落在 `out` 里** —— 那是游戏持有的 40 字节 Result（字段语义见
 * `native/compile/compile_hook.c` 的头注释），游戏只看它。实现可以自己 malloc 一块产物再填进去
 * （simplexc 那条路的 bit62 + 8 字节头就是这么来的），也可以直接把原实现的返回值转交（ref）。
 * 所以接口不规定 blob 的形状，只要求「成功以后 out 里是一个形状合法的 Result」。
 *
 * 失败分类是 §8 的封闭词表：源码侧那六个（`UserError` / `UnsupportedConstruct` /
 * `BlobTooLarge` / `RelocUnsupported` / `SymbolUndefined` / `InternalError`）由核心在 C 边界那头
 * 定义并返回（全仓只有 `native/backend/simplexc.c` include 那份头），这里不重复定义；
 * 下面四个是**壳与后端自己**产生的，
 * 壳要按同一个值分支（hook.log 的 `stage=`、回退决策），所以定义在这一处。值不能改 ——
 * 已经写进 hook.log 的历史行与回退率统计。
 */
#ifndef BACKEND_H
#define BACKEND_H

#define KIND_INTERNAL   "InternalError"
#define KIND_TIMEOUT    "Timeout"
#define KIND_FAULT      "OursFault"
#define KIND_USER_ERROR "UserError"

/* 被转发的原实现。壳（`ensure_real` / `NimMain` / `NimDestroyGlobals`）与 ref 后端都要定位
 * 同一份模块，所以只能有一处拼写：编译期改名成 `compile_real.dll`、`compile.dll` 名字留给我们的壳。 */
#define REAL_DLL "compile_real.dll"

/* 一趟编译里后端回填给壳的账。壳用它写 hook.log 的 `impl` 行（`budget_ms=` / `guard=veh`
   只在 `attempted` 为真时写），并据此判断这一趟实现自己走得怎么样。
   产物本身不在这里读 —— 壳从 `out` 读（见文件头）。 */
typedef struct {
    void              *blob;             /* 本趟产物缓冲；谁是它的主人由实现定义 */
    long long          blob_len;
    long long          entry;            /* 产物内的锚点偏移，相对机器码起点 */
    void              *ret;              /* 游戏要拿到的那次调用返回值；simplexc 恒 NULL */
    int                attempted;        /* 实现真的跑了自己那段（ref 恒 0）*/
    int                guard_installed;  /* 本趟装了实现自己的崩溃保护（VEH）*/
    unsigned long long budget_ms;        /* 实现自己那段的时间预算；0 = 没有预算 */
} backend_run;

typedef struct {
    const char *name;                 /* "simplexc" / "ref" */
    /* 跑一趟。成功返回 NULL；失败返回 §8 的分类串（静态存储期，不要 free）。
     *
     *   src/slen      游戏给的源码，**已经拆好**的字符数据（NimStr 的 ptr+8 与 len）；可能
     *                 NULL / 0 —— 算不算失败由实现判（simplexc 那条路判 `InternalError`）
     *   src_raw       游戏给的 NimStr 指针**原件**：ref 把它原样转给原实现（原实现只收这个）；
     *                 `src/slen` 是壳解读出来的副本，simplexc 用那一份。两样都给，是因为
     *                 NimStr 的布局只能在一处（壳）解读，后端不该再拆一次
     *   stdlib_path   NULL = 不拼 stdlib 前缀（非 NULL = 读这个文件、把内容拼在源码前面）；
     *                 ref 忽略它 —— 原实现自己内部就拼
     *   flags         游戏传进来的原值；只有要转交原实现的实现会用到
     *   r             必填、由实现填满（壳每次调用前清零，实现只管写）
     *
     * 成功时 `r->ret` 必须是**游戏该拿到的那次调用返回值**（ours 恒 NULL）：它是行为不变量的一部分，
     * 离线判据只把它打印出来、不断言，丢了也不会变红。
     *
     * 失败时也得让 out 保持**上一步走过的合法状态**：要么什么都没写（壳随后自己写失败 Result），
     * 要么写到实现自己承诺的形状上 —— 别留半截指针。
     */
    const char *(*run)(void *out, const char *src, long long slen, void *src_raw,
                       const char *stdlib_path, int flags, backend_run *r);
    /* 放掉 run 交回的 `r->blob`；传 NULL 安全。壳在成功路径上不调它（那时缓冲已经交给游戏，
       归还规则由游戏按 bit62 定），我们自己的上一块也由实现在下一次调用时自己放 —— 留这个钩子是
       给「拿到了产物又不交给游戏」的路径用。ref 没有可放的（空实现）。 */
    void        (*free_blob)(void *blob);
    /* 最近一次失败的**三轴诊断串**（`sema/用户源码错 第 12 行：…`），没有失败时是空串；
       与 `run` 同一次调用里读完即弃，静态存储期不要 free。没有诊断的实现返回 NULL。 */
    const char *(*last_error)(void);
    /* 最近一次的**非致命观察**串；约定同 `last_error`。没有的实现返回 NULL。 */
    const char *(*last_warnings)(void);
} compiler_backend;

extern const compiler_backend backend_simplexc;
extern const compiler_backend backend_ref;

#endif /* BACKEND_H */
