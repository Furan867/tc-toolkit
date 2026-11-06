/* 壳**自己**的那两个路径 —— 机制用的那组（捕获目录 / 日志目录 / 配置 / 改写器脚本）在
 * `forward.h` 的 `forward_paths` 里，由 `forward_init` 注入。
 *
 * 为什么单列一份头：定义的**唯一一处**在壳里（加载时算一次），编译后处理器只读；结构体若两边
 * 各写一遍，改字段时漏一处就是两边看见不同的布局。
 *
 * 字段只增不改名：处理器按名读字段，壳按名填。
 */
#ifndef PATHS_H
#define PATHS_H
#include <windows.h>

typedef struct {
    char proj[MAX_PATH];       /* 改写子进程的 cwd */
    char stdlib[MAX_PATH];     /* 编译时拼在源码前面的前缀文件 */
} toolkit_paths;

extern toolkit_paths g_paths;
#endif
