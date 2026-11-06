/* 编译后处理器（processor_rewrite.c）要用的两个常量：改写子进程的启动器与超时。
 *
 * 定义只在这一处：壳不认识它们，处理器也不该把值抄回自己的 .c 里。
 *
 * 日志 / 写文件 / 读记录项 / 路径表那些是**机制**，在 `native/forward/forward.h` 与 `paths.h`。
 * 不是对外接口。
 */
#ifndef NATIVE_INTERNAL_H
#define NATIVE_INTERNAL_H
#include <windows.h>

#define PY_EXE      "C:\\Windows\\py.exe"    /* 系统自带的启动器，不依赖 PATH */
#define OPT_TIMEOUT 8000                      /* 子进程最多等 8 秒，超时就放弃 */
#endif
