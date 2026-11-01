# shell

壳里**新增的那部分**：选实现（reference / ours…）、失败转交、核心入口。既有壳
`tools/compile_hook.c` 继续演进，**不做第二份壳**（所有权见接口 §0）。

现状：

- `entry_shim.S` —— 游戏 `lea (%rsi,%rdi,1),%rax; jmp *%rax` 跳进来的最外层入口（不是 `call`，
  所以进来时寄存器脏、栈不保证 16 对齐）；顺带提供 `___chkstk_ms`（CRT 符号，LLVM 给
  >4 KiB 栈帧的探栈会调它）。为什么必须手写写在文件的注释里。
- `llvm_link_probe.cc` —— 最小 LLVM-C 链接探针（对应 `docs/facts.md` §构建与 LLVM 的验证行）。
- **还没有**：选实现、失败路径、熔断 —— 这些现在都在 `tools/compile_hook.c` 里。

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` §5（DLL 壳）、§8（错误与回退契约）
- 主设计 §5 第 8 条：entry 桩为什么不能交给编译器生成

怎么用：

    python simplexc/tools/build.py objs                  # → build/objs/entry_shim.o（随 blob 装配）
    python tools/deploy_compile_hook.py build-deploy     # 编译转发层并部署（游戏必须先关）

⚠️ 动 `tools/compile_hook.c` 前先持 `work/tmp/locks/compile-hook.lock`（另一个 agent 可能正在改它）。
