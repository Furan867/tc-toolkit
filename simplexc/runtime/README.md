# runtime

路线 B 的运行时：生成代码真正用到的内建与运行时。用 C++17 写、编成 COFF，**随 blob 装配进产物**
（不是外挂 DLL）。

现状：只有 `rt_min.cc` —— 最小一块（`thread_exit`，加一个 spike 用的可观测痕迹），够跑通空程序，
不是终态。终态由 stdlib 源码取代（路线 A，主设计 §6）。

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` §3（runtime ABI）
- 组件职责：主设计 §3.1

写新文件时照这两条（否则产物装配不上）：

- 全部 `extern "C"`，符号名要裸名 —— 装配器按 `ExitThread` / `thread_exit` 这种名字匹配，
  编成 C++ 修饰名（`_Z…`）就再也对不上；
- 只用 kernel32 + 纯算数 —— 任何 CRT 符号（`sprintf` 之类）都是 `ExternResolver` 解析不了的
  未定义符号，装配会失败（`rt_min.cc` 里手写十进制转换就是这个原因）。

怎么用：`python simplexc/tools/build.py objs` 编成 `build/objs/rt_min.o`，再由 `tcblob` 装进 blob。
