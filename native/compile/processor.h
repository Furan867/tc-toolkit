/* 编译后处理器的接口 —— 壳只认这张表，不认具体处理器。
 *
 * 为什么分两层：编译后优化是**可以不要**的一层（它调外部 Python 改写产物）。
 * 壳里若直接调它，想「关掉这层」就得改壳的正文；现在壳只遍历注册表，
 * 「删掉编译后优化」= 删一个 .cc + 删注册表里那一项，没有第三处引用。
 */
#ifndef PROCESSOR_H
#define PROCESSOR_H
#include <windows.h>

typedef struct {
    const char *name;                 /* "rewrite" */
    void (*init)(const char *mode);   /* 每趟编译前由 compile() 传入配置值，处理器不读文件 */
    void (*process)(void *buf, long long len, LONG seq, void *out);
} forward_processor;

extern const forward_processor processor_rewrite;
#endif
