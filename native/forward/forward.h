/* 转发层的**机制** —— 与「劫持了哪个 dll、日志里那些键是什么意思」无关的那一半。
 *
 * 为什么单独一个静态库：将来还要转发别的 dll，那一份壳与这一份共用同一套「在自身模块旁边找
 * 原实现 / 调用编号 / 捕获落盘 / 追加日志 / 读配置 / 加载时清理」，各自的业务差异只留在各自的壳里。
 *
 * 路径全部由壳经 `forward_init` 注入：本层不知道配置文件叫什么、目录按谁的规矩排。壳读到运行时
 * 配置（例如仓库根）以后自己重算一份 `forward_paths` 再交给本层 —— 本层只存不算。
 */
#ifndef FORWARD_H
#define FORWARD_H
#include <windows.h>
#include <stddef.h>

#define SEP 92      /* 反斜杠的 ASCII —— 写成字符字面量在多轮转义里会失真 */

typedef struct {
    const char *out_dir;    /* 捕获产物 */
    const char *tmp_dir;    /* 改写中间产物 */
    const char *log_dir;    /* hook.log */
    const char *ini_path;   /* 配置 */
    const char *script;     /* 改写器脚本，壳提供 */
} forward_paths;

/* 指针**不做拷贝**：传进来的字符串必须活到进程结束（壳传的是静态缓冲）。 */
void forward_init(const forward_paths *p);
const forward_paths *forward_get_paths(void);

/* 在**本模块所在目录**旁边加载 dll 并取符号（相对路径会按宿主的当前目录解析，不能指望）；
   返回 NULL = 加载失败或没有这个符号。失败时 Win32 的最后错误码原样留给调用方
   （126 找不到文件 / 193 位数不对 / 127 没有该导出）。 */
void *forward_real_symbol(const char *dll, const char *name);

/* 一次调用一个号：从 1 开始递增，本进程内唯一。 */
LONG forward_next_seq(void);

/* 返回是否真的写出了文件（调用方要按它决定簿记里写文件名还是 "-"）。 */
int forward_dump(const char *tag, const char *ext, const void *p, long long n, LONG seq);
void forward_log(LONG seq, const char *event, const char *kv);
int forward_write_file(const char *path, const void *p, long long n);
int forward_read_entry(const char *path, long long *val);
void forward_sha256_hex(const void *p, long long n, char *out);

/* 读 [opt] 段的 `key`：返回 1 = 键存在（值已写进 buf，**可能是空串**）；0 = 键不存在（buf 置空）。
   所以缺省值要由调用方先填 —— 等价于 GetPrivateProfileString 的那个默认值参数，只是把
   「键不存在」与「键存在但值是空的」这两件事分开（后者 Win32 也写成空串，分不开）。 */
int forward_config_get(const char *key, char *buf, size_t cap);

/* 加载时清掉上一次会话的残留（捕获目录 / 中间产物目录各一份，只删本层的文件）。 */
void forward_purge_captures(void);
void forward_purge_tmp(void);
#endif
