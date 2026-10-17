# cli

离线 CLI：源码 → blob，与壳共用核心。给 agent、差分宿主、回归用，**不碰游戏**。

现状（三个入口文件）：

- `tcblob.cc` → `build/tcblob.exe`：装配器 CLI（`--obj … --entry … --out … [--run] [--verbose]`）
- `prog_empty.cc`：spike 用的「空程序」程序根（留一条可观测痕迹就返回；**不能进游戏**）
- `llvm_prog.cc` → `build/llvm_prog.exe`：用 LLVM-C 生成程序根 → COFF，M1 那条链的样板

- 对外接口（冻结版 1）：`docs/specs/2026-10-15-simplexc-interfaces.md` §7（构建与链接）；
  那里记的产物名 `build/simplexc-cli.exe` 是**接上前端之后**的形态，现在这几个都还不是它
- 组件职责：主设计 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` §3.1
- 用法细节：各文件的头部注释（`tcblob.cc` 的 `--run`、`llvm_prog.cc` 的模块形状）

怎么用：

    python simplexc/tools/build.py all            # 三个都编出来
    build/tcblob.exe --obj … --entry __tc_entry --out blob.bin --run
    build/llvm_prog.exe out.obj                   # PATH 里要有 build/llvm/bin 才找得到 LLVM-C.dll

`python simplexc/tools/build.py spike` 用它们装出 `build/blob_empty.bin` + `.entry`。
