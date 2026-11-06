/* 壳（compile_hook.c）与编译后处理器（processor_rewrite.c）之间的**内部**边界。
 *
 * 这里只放两样东西：
 *   · 处理器也要用的那几个常量（改写子进程的启动器、超时、路径分隔符）——
 *     定义**只在本头**一处；壳与处理器各存一份就会在改的时候只改一边。
 *   · 壳提供给处理器的工具函数（日志、写文件），声明在这里、定义仍在壳里 ——
 *     只给这几个符号去掉 `static`，不把壳整个摊开。
 *
 * 不是对外接口（对外只有 `simplexc_core.h` 与 `processor.h`）。
 */
#ifndef NATIVE_INTERNAL_H
#define NATIVE_INTERNAL_H
#include <windows.h>

#define SEP      92        /* 反斜杠的 ASCII —— 写成字符字面量在多轮转义里会失真 */
#define PY_EXE      "C:\\Windows\\py.exe"    /* 系统自带的启动器，不依赖 PATH */
#define OPT_TIMEOUT 8000                      /* 子进程最多等 8 秒，超时就放弃 */

/* 往 hook.log 追一行（格式由壳定义，见 compile_hook.c 的 hook_log）。 */
void hook_log(LONG seq, const char *event, const char *kv);
/* 整块写文件；返回 0 = 失败。 */
int write_file(const char *path, const void *p, long long n);
#endif
