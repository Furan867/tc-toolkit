/* simplexc_core.h —— 壳与 C++ 编译器核心之间**唯一**的 C 边界。
 *
 * C 与 C++ 双用头：标准的 `#ifdef __cplusplus` 守卫 + `extern "C"`。
 * （pi-lens 的 `reserved-namespace` 规则会把 `__cplusplus` 报成「保留标识符」—— 那是误报：
 *  它是编译器预定义宏，名字由标准固定、无法改名也没有替代写法。代码仍按标准写，
 *  误报已向负责人汇报，不在代码里绕。）
 *
 * 为什么边界是 C：壳是 C 且继续演进、不做第二份壳；C++ 名字改编会让链接找不到符号。
 * 异常也不许穿过这条边界 —— 穿过 Nim 帧的展开没有保证（接口冻结版 §5），
 * 所以核心内一律 catch-all，折成下面的返回码。
 *
 * 语言分工（接口 §0）：`shell/`、`frontend/`、`lower/`、`assemble/` 都是 C++17；
 * 只有这一个头是 C（`.h`），因为它的另一半读者是 C。
 */
#ifndef SIMPLEXC_CORE_H
#define SIMPLEXC_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 编译一段 Simplex 源码（**内存里的**源码，不落盘 —— 游戏进程里没有临时文件可言）。
 *
 * 成功：返回 NULL
 *   out_blob  → malloc 的产物缓冲：前 8 字节 = `out_len | (1<<62)`（bit62 让游戏不 free），
 *               机器码从 +8 起，**尾部多 8 字节 padding**（游戏 memcpy 会读到末尾之外，主设计 §4-1）。
 *   out_len   → 缓冲总长（含 8 字节头）
 *   out_entry → entry 偏移，相对机器码起点
 *   缓冲由 `simplexc_free()` 释放；壳负责在**下次编译开始时**释放上一块。
 *
 * 失败：返回 §8 的分类串（静态存储期，不要 free），三个出参不动：
 *   "UserError"（用户源码错，**不转交参考实现**）/ "UnsupportedConstruct" / "InternalError"
 *   "BlobTooLarge"          / "RelocUnsupported"       / "SymbolUndefined"
 *
 * 分类串只说「哪一档」，三轴（责任/阶段/位置）在 `simplexc_last_error()` 里。
 *
 * 不抛异常、不写文件、不改可观察的全局状态。调用方仍应包 VEH —— LLVM 在游戏进程里跑，
 * 熔断是壳的责任（`ours_veh`），不是核心的。
 */
const char* simplexc_compile(const char* src, long long src_len, void** out_blob,
                             long long* out_len, long long* out_entry);

/* 最近一次 `simplexc_compile` 失败的**三轴诊断串**（`sema/用户源码错 第 12 行：…`）。
 *
 * 为什么不在返回值里：返回值是**封闭词表**的分类串（壳与 manifest/hook.log 都按它分支），
 * 而诊断串里有 detail —— 长度由用户源码决定，不能让词的边界跟着用户输入变。
 * 调用约定：同一次调用里读完即弃；没有失败（或失败在那之前就被壳自己判掉）时是空串。
 * 静态存储期，不要 free；下一次失败会覆盖它。
 */
const char* simplexc_last_error(void);

/* 最近一次 `simplexc_compile` 的**非致命观察**串（`; ` 分隔的人话条目：解析期 notes、
 * 「不可达 def 被跳过」这类不改行为的记录）。
 *
 * 为什么要有它：这两类信息不改产物、不该报错（改了会与参考实现的差分判据分叉），
 * 以前它们只活在 C++ 侧的局部变量里，编译路径上一个消费者都没 —— 游戏是 GUI 进程，
 * 也看不到 stderr。壳把它写进 `hook.log` 的 `warn` 行，事后才能查到「这里曾经有话」。
 * 调用约定与 `simplexc_last_error()` 相同：同一次调用里读完即弃，静态存储期不要 free；
 * 每次 `simplexc_compile` 开头清空，没有观察时是空串。
 */
const char* simplexc_last_warnings(void);

/* 释放 simplexc_compile 给出的缓冲；传 NULL 安全。 */
void simplexc_free(void* blob);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLEXC_CORE_H */
