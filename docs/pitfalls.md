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
- 别把 ctypes 回调当普通局部变量 —— 注册给系统（VEH / 窗口过程 / 线程过程）后必须留引用；
  Python 对象被回收后系统会跳到已释放内存，表现为**没有任何信息的静默崩溃**
  （实测：VEH 装了却从不打印、进程直接以 `0xC0000005` 退出）。
- 别在可选头里找 `SizeOfOptionalHeader` —— 它在 COFF 头（`e_lfanew+20`）。读错会让整张节表错位，
  报出「缓冲区不够」这类看不出根因的怪错（实测：19 个节全乱、import 目录指到 674 MB 处）。
- 别用 `sim_cycle == target` 判一次 run 完成 —— 被 `check_output`/短路 halt 的 run **提前结束**
  （实测 120：`sim_test_result=win` → halt → `sim_cycle` 停在 0），会把正常完成当成超时。
- 别拿 extern 调用条数当判据 —— `Sleep`/读时钟是节奏类，条数随真实时间变化（实测两次跑差 650 万条），
  而仿真状态可以逐字节全等。
- 别拿 `VirtualQuery` 返回非空当「已映射」—— `MEM_RESERVE`-only 的区间也返回非空且 `Protect=0`，
  能解引用的判据是 `Protect != 0`（宿主判「表外 movabs」硬错误时踩过）。
- 别以为 COFF 里 `PointerToRawData == 0` 是「没有这个节」—— 它是「节没有原始数据」（`.bss` 的标准形态），
  按 rawptr 读 rawsize 字节读到的是**文件开头的 COFF 头**（含编译时间戳）→ 产物不可复现、`.bss` 初值成垃圾。
- 别用 `SOURCE_DATE_EPOCH` 去固定 clang 写的 COFF 时间戳 —— 实测**无效**（要产物确定只能在装配后的 blob 上看）。
- 别以为「同一份源码 → 同一份 `.obj`」—— clang 每次写的 COFF header 时间戳都不同，比 sha 要拿**装配后的 blob**。
- 别在宿主里指望我们的产物「没有 extern 跳板」—— 它把 kernel32 绝对地址写死在 `.text` 尾部的桩里，
  不读 `<blob>.stubs` sidecar 就会把它当未知地址拒跑（实测就是这么被挡住的）。
- 别把「含绝对地址的产物」跨游戏进程复用 —— 生成源码里的游戏内存地址（`commands`/`settings`/
  `#SIMULATION_STATE`…）按**游戏进程实例**变（实测同一进程内关卡与元件工坊都是 `2270676779008` 段，
  而 12:24 那次会话的捕获是 `2416573349888` 段）；拿旧 blob 喂新进程 = 往未映射地址写。
  游戏内验证必须「同一次会话内：抓源码 → 生成产物 → 部署」。
- 别把「部署」当成单独一步、拿上一次会话生成的产物去装 —— 2026-10-17 16:50 就这么让游戏崩了：
  产物里嵌的是进程 A 的地址，游戏 16:47 崩过并重启（进程 B），写未映射地址 → `0xc0000005`、
  WER `Faulting module: unknown`。部署前必须用**最新**的 `work/hook/hook_src_NNN.si` 现取地址重编，
  并校验产物里的地址 == 该源码的 `Ptr` 值（同一次部署里做完，别跨消息）。
- 别把 `hook_src_NNN.si` 当成「我切到的那个电路」—— 它是**触发编译那一刻**游戏生成的源码。
  实测 008（用户以为切到了计数器）是 `get_component_count() = 0`、`mode_refresh()` 空、`Input {}`/`Output {}`
  的空电路：每 tick 只 `cycle += 1`，测不出任何收益。拿到捕获先看这两个位置再动手写等价物。
