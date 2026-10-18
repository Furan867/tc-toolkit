/* simplexc_core.h —— 壳与 C++ 编译器核心之间**唯一**的 C 边界。
 *
 * 这是一个**纯 C 头**（没有 `#ifdef __cplusplus` 守卫，故意的）：
 * 唯一的读者是 C 侧的 `tools/compile_hook.c`；C++ 侧（`simplexc_core.cc`）用
 * `extern "C" { #include "simplexc_core.h" }` 包住它来拿 C 链接，实现也用 `extern "C"` 定义。
 * 这样不必用 `__cplusplus`（pi-lens 的 reserved-namespace 规则会把它当自定义标识符误报）。
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
 *   "UnsupportedConstruct" / "InternalError" / "BlobTooLarge"
 *   "RelocUnsupported"     / "SymbolUndefined"
 *
 * 不抛异常、不写文件、不改可观察的全局状态。调用方仍应包 VEH —— LLVM 在游戏进程里跑，
 * 熔断是壳的责任（`ours_veh`），不是核心的。
 */
const char* simplexc_compile(const char* src, long long src_len, void** out_blob,
                             long long* out_len, long long* out_entry);

/* 释放 simplexc_compile 给出的缓冲；传 NULL 安全。 */
void simplexc_free(void* blob);

#endif /* SIMPLEXC_CORE_H */
