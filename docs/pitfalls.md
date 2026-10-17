<!-- 一行一条：别做什么 —— 后果。只增条目，不堆解释。 -->
# 坑

- 别用「电路名」去定位源码 —— CE 法找到的是 UI 字符串池，不指向源码（该线已暂停）。
- 喂给编译器的源码先查 `{` 与 `}` 是否配平 —— 截断的源会让 `compile.dll` 直接 abort。
- `compile.dll` 遇编译错误会 **abort 整个进程**（Nim `sysFatal`），不返回错误码 —— 一律放子进程跑。
- 不要调 `NimMain()` —— DllMain 已经初始化过，重复调用触发断言。
- 游戏没开、或电路没在跑时什么都抓不到 —— 表现像「方法失效」，实为空跑。
- 别拿跨运行的映射表复用 —— `component_map.tsv` 不记自己的出处，会**静默**给出错误归属。
- 用调试寄存器前必须清 DR 并**循环复验** —— 线程会创建/销毁，只清一遍必然漏，漏了就崩游戏。
- 断点工具别把地址靠命令行传入 —— 必须在**同一次运行内**完成「搜地址 → 设断点」。
- 合法值集合只能有一处定义 —— 「静默改值」永远比报错难查（`#043` 那次多绕了很远）。
- 别想从进程内触发 recompile —— 8 条路全败，触发链依赖活的游戏状态且只能在主线程执行。
- 别用窗口消息投递按键 —— 游戏不通过窗口消息接收键盘（连同步 `SendMessage` 都不动），
  它轮询键盘状态，`PostMessage` 不更新键盘状态。
- 别用 `ShowWindow(SW_RESTORE)` / `BringWindowToTop` 抢前台 —— 会触发 `game_engine.dll`
  的 ImGui 断言 `scale > 0.0f` 搞崩游戏。
- 跨虚拟桌面抢前台会把用户**从当前桌面拽回游戏桌面** —— 要用就由调用者自己决定时机。
- 写文档时不要记「文件多大 / 有多少符号 / 多少字节」这类**物理属性数值** —— 重建就变，
  没人会跟着改（`TOOLS.md` 里的转发层字节数就这么过时了）。
- 停机时别用 `last_speed_check` 判断仿真是否在跑 —— 它照常每秒刷新，见 `facts.md` §采样判据 ④。
- 改文件的时候别并行去跑读它的命令（grep/tail 都算）—— 会读到写了一半的内容，
  我因此漏检过一处待改的旧说法。
- 脚本首行写 `#!/usr/bin/env python3` 会让 PyLauncher 去 **PATH** 里找 `python3.exe`
  （本机命中 MSYS2 那个），同一个 DLL 编译慢 **16 倍**且**毫无报错**。工具脚本一律写
  `#!python3`；`build_component_map` 会打印每次编译耗时，十几秒就是选错解释器了。
- 别拿空程序/桩程序去替代真实电路的产物 —— 游戏下一次编译请求会 **abort 整个进程**（先
  `send_command(3)+joinThreads` 再 `compile()`，半途的仿真协议会让游戏自己 fatal），而且**不留 WER、不留
  dump**，查不到现场；连 `work/hook/hook_src_NNN.si` 都不会新增（死在 `compile()` 之前）。
  进游戏验证自研产物必须拿「真能跑完仿真协议」的程序，见 `docs/facts.md` 同名条目。
- 别带着 `compiler = ours` 启动游戏 —— 启动会编译地图电路，拿到什么都不同的程序后地图构建不出来，
  表现为「游戏起不来」（不是崩，是卡）。要验证就先用 `reference` 起来再热切。
- 别用 `objdump -g`（不加 `--dwarf=`）判断 DWARF 里有没有 Nim 符号 —— 它打印的
  `…@spasses@sfront_end@stokenizer.nim.c:` 来自 **COFF `.file` 记录**，不是 `.debug_info` 的内容。
- 别把 pi-lens 的 C/C++ 诊断当编译错误 —— 报「找不到系统头」的是它的 **lsp runner（自带 clangd 22.1.0，
  windows-msvc 版）**，不是 `cpp-check`（后者落到 `g++`，从没报过）。真判据只有 `g++` 编译。缓解已就位：
  机器级 `%LOCALAPPDATA%\clangd\config.yaml` + 源文件旁 `compile_flags.txt`（`--target` + `--gcc-toolchain` + `-I`，
  见 `simplexc/compile_flags.txt`）。**别用逐行 `-isystem` 拼 mingw 头目录** —— libstdc++ 用 `#include_next`
  取 `stdlib.h`，重复且顺序不对的 `-isystem` 会让它找不到（要用 `-idirafter`）。
- `.h` 会被 pi-lens 按 **C** 检查，`compile_flags.txt` 里的 `-std=c++17` 于是非法 —— 按项目约定，
  C++ 头一律 `.hh`（C 才用 `.h`，见 `AGENTS.md`）。另一个陷阱：同一文件重复 write 时 harness 不重复报
  —— 「第二次写没报错」不能当修好的证据。
- 别让 git 的行尾归一化碰语料 —— `core.autocrlf=true` 会把 CRLF 折成 LF 存（`src/001.si` 少 74 字节）、
  检出时再折回来；只要语料路径在 `.gitattributes` 里标了 `-text` 才安全。
  自检：`git show :<file> | wc -c` 必须等于工作区字节数。
