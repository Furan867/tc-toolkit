/* 生效的路径表 —— 壳（compile_hook.c）与编译后处理器（processor_*.c）共用。
 *
 * 为什么单列一份头：`g_paths` 的**定义**只在壳里（`toolkit_paths_init` 在加载时解析一次），
 * 处理器只读；结构体若两边各写一遍，改字段时漏一处就是两边看见不同的布局。
 *
 * 字段只增不改名：处理器按名读字段，壳按名填。
 */
#ifndef PATHS_H
#define PATHS_H
#include <windows.h>

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

extern toolkit_paths g_paths;
#endif
