# assemble

COFF 目标文件 → 游戏能 JIT 的 **blob**：重定位、extern 桩、8 字节头、entry 偏移、节布局。

现状：可用。`assemble.hh` 是契约（冻结版 1），`assemble.cc` 是实现，CLI 在 `simplexc/cli/tcblob.cc`。

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` §4；细则与理由写在
  `assemble.hh` 的注释里（**别在别处再写一份规则**）
- 产物形态的实测事实（哪种 COFF 形状会出现）：`docs/facts.md` §构建与 LLVM

两条最容易踩的：

- 产物里**只允许 PC 相对引用** —— blob 会被游戏拷到任意地址执行，节布局与桩的写法都为它让路；
- extern 桩通向真实地址：`__imp_` 前缀的符号要造**数据槽**而不是代码桩（调用方是
  `call *__imp_X(%rip)`，解引用拿到的是槽里的地址）。

怎么用：

    python simplexc/tools/build.py objs tcblob
    build/tcblob.exe --obj a.obj --obj b.obj --entry __tc_entry --out blob.bin [--run] [--verbose]

`--run` 按游戏的方式（VirtualAlloc RWX → memcpy(blob+8) → jmp base+entry）在进程内跑一遍，
不用进游戏就能自测。产物旁另写 `<blob>.entry`（十六进制），壳的自研路径靠它取入口偏移。
