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
- 停机时别用 `last_speed_check` 判断仿真是否在跑 —— 它照常每秒刷新，见 `facts/perf-tooling.md` §采样判据 ④。
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
- 别在没有前端的情况下为「验证收益」反复手写 C 等价物 —— 成本随电路复杂度爆炸，而且工作量小的电路
  （单与门、计数器）的比值会被协议轮询与 `Sleep` 粒度淹没，量不出东西。M1.5 就是因此于 2026-10-17 终止：
  收益结论停在「宿主内 2.95×、游戏内无证据」。要数字先有前端（M3.0 垂直切片）。
- 别把 `hook_src_NNN.si` 当成「我切到的那个电路」—— 它是**触发编译那一刻**游戏生成的源码。
  实测 008 是 `get_component_count() = 0`、`mode_refresh()` 空、`Input {}`/`Output {}` 的空电路
  （退出游戏时编的），每 tick 只 `cycle += 1`。拿到捕获先看 `get_component_count()` 与 `mode_refresh()` 再动手。
- 别把 `tools/try_compile_dll.py` 的输出接到 `| head` 这类会提前退出的管道 —— 进程可能在刷新
  `work/compile_out.bin` 之前就被杀，你拿到的是上一次/别人的产物（实测：9119 字节，头部 `0x239F`
  正好是**别人**的 out_len），表现为产物一跑就 `thread_exit`，看着像语义问题。
- `work/compile_out.bin` 是**共享路径**，多个任务同时编译会互相覆盖。判据必须「编译完先核对
  `out[0] == 文件大小`，再 `cp` 到自己的名字」，然后只认自己那份。
- 别在 `.si` 片段里用**含大写字母的变量名** —— 参考前端会拒（`a_C`/`aC`/`aS` 都报
  `let declarations use '='.`）；类型名（`S32`/`U32`）照旧大写，变量名必须全小写。
- 别用「全零内存」当等价性判据 —— 272 MB RAM 镜像默认全零时，两个产物都读零，
  「逐 tick 全等」是**平凡成立**的。实测因此藏住一个 `memory_copy` 方向反的 bug：
  灌确定性图案后第一 tick 就暴露（我们的产物把源当目的，图案被反向拷贝销毁）。
  判据的输入必须非平凡，否则绿灯没有意义。
- 别假设「每次编译都会起改写子进程」—— `compiler=ours` 分支**只写一行 `impl`**，没有
  `cfg`/`opt`/`writeback`。任何拿 `opt`/`writeback` 当「编译完成」信号的判据（历史归属编号、
  bench 的等产物逻辑）在 ours 轮都会误判：实测 bench 重试 3 次、每轮都编译成功
  （`manifest` seq 006/007/008 + `hook_out_*.bin` 都在），却报「等 60s 没等到产物」。
  「编译完成」= 该 seq 的**任一终态行**（`impl … result=ok` 或 `writeback`/`opt`）。
- 别把 `LLVMRunPasses` 当成「机器码优化过了」—— 它只优化 **IR**；机器码质量由 
  `LLVMCreateTargetMachine` 的 `CodeGenOptLevel` 决定（`Default`=2，`None`=0）。
  Python 原型把 `Default` 传成 0 一直没被发现，因为「IR 变小了」看起来像优化生效了；
  判据必须是 `.text` 体积或指令数，不是 IR 文本长度（实测差 2.2 倍，见 facts.md）。
- 别在 `bash -c "…"` 里用 heredoc（`python - <<'EOF'`）内联脚本 —— 尤其脚本含引号/反斜杠字面量时，定界符
  层层扣转义后不闭合，shell 就一直**等 stdin**：命令永不返回、`bash` 工具的 5 分钟硬超时也不生效
  （实测：子 agent 因此挂死 5m33s、零输出，且 watchdog 只报「bash 开了 240s」）。
  内联脚本一律用 `write` 工具落盘再 `python <file>.py` 跑（write 不走 shell，写什么是什么）。
- 派活时别漏掉「pi-lens 清零」这条验收判据 —— 新写的工具脚本会自带一堆
  `unchecked-throwing-call-python`（**error 级**，命中 `open`/`json.load(s)`/`os.*`/`shutil.*`）。
  项目的既定处置是**清零**（`docs/notes/2026-10-18-ir-build-lint.md`：真会抛的加 `try/except` +
  「路径+原因」+ 非零退出码；规则不适配的假报也改成显式写法、行为等价），**不是判「误报」放过**。
  实测：一次派活漏了这条，3 个文件欠 31 条债，其中 17 条来自新派活写出来的 `m5/coverage.py`。
- 给负责人汇报时别堆技术细节（文件:行、分类计数、字段名、内部术语）—— 他要判断的是「**哪个好做、哪个有用**」，
  堆细节等于没给他判断依据（实测：一次汇报里塞了 8 类缺口 + 行号 + 段地址，他的反馈是「我看不懂」）。
  细节进 `docs/notes` 与 `status.md`；**汇报只给三样**：做了什么 / 结论 / 代价与收益（+ 需要他定的那一件）。
- 别在同一个文件上同时放两个写者（哪怕派活时说「你只改这里」，前一个写者只要还在跑、改动还在工作区，就是两个写者）：
  agent 各自 read-modify-write 会**互相覆盖**。实测：一个 worker 的实现未提交、我又给同一个 `lower.cc` 派了第二个写者，
  只能靠「叫停一个 + 催先完成那个提交」救回来。**派写者前先看 `git status` + 确认没有活着的写者占着那个目录。**
- 别把子 agent 报的公式/规格自己「简化」后再写进 `facts.md` —— 规格要**原样**抄，或者标清「未原样核实」。
  实测：`random` 的递推是**顺序赋值**（`x^=x<<12; x^=x>>25; x^=x>>27; x*=C`），我折文档时简化成了展平式；
  两者**第一次调用结果相同、从第二次起分道** —— 会静默把后续实现引到错的路上（而且看上去像对的）。
- 别从「改函数体不产生字节差」推出「那个体被丢了」：先看它的 IR 是不是完整。实测：数组字面量漏了 Nim 头（8 字节）→
  `.len()` 变成一条**越界的常量 load** → O2 里成 undef → 依赖它的分支被**整条折掉**，
  表现就是「函数体像被整个丢掉」（改体、换字段全都没字节差）；真正的原因是**数据表示**，不是语句丢失。
- 别把「N 个观测点无反例」的规则当**完整规格**去铺开 —— 先用**已经全等的份**当反例试一遍。
  实测：窄位宽规则在 20 个观测点上无反例，但铺到其余算符后 176/177/180 从全等掉成 diff
  （它们里面有「宽侧是常量但参考**不**窄化」的形状）→ 铺开前先拿「当前全等集合」做回归，宁可少铺。
- 查「元凶是什么」时，先确认**你的对照组本身没 bug**：两个 patch 共用同一个写错的 helper（`mask_of` 多套了一层）时，
  它们的输出哈希一字不差 —— 那看起来像「同一原因」的强证据，实际是**同一个 bug**。实测：为此追了四轮，最后发现中间那轮的
  「反例」正是 patch 自己造的，且修对了对覆盖率**零收益**。
- 别拿默认参数跑 `simplexc/tools/m5/coverage.py` 的子集验证 —— `--report` 默认就指向仓库里的
  `docs/notes/2026-10-18-m5-coverage.md`，一次 `--ids 100,…` 就把 **98 份的全量报告覆盖成 17 份子集**
  （实测：154 行差异）。子集跑必须显式 `--report work/tmp/…`（`--tsv`/`--summary-json` 同理）。
- 别让子 agent 在代码或回答里**手写大段期望值字面量**（dump golden、期望输出、长基准文本）——
  生成这类长文本时被 provider 的 content_filter 掐断过两次，且每次都是「文件已落盘、未接线、未验证」
  的半成品。期望值一律用**重定向从程序输出生成**（给程序加 `--dump-only` 之类的开关，
  `./exe --dump-only > testdata/x.txt`），再人工核对 golden 的每一行是否合规格。
- 别拿**报错文案**给缺口分类 —— 我以为剩下的 14 份是「`const #X = "<串>"` 字符串常量」，因为
  `analyze.cc` 的报错就是这么写的；实测 `grep 'const #* *= *"' simplexc/corpus/src/*.si` 是 **0 处**，
  真正的缺口是**聚合常量**（`const #C = [U1 1,0]`）。归因必须查语料，不能读报错。
- 别用「归一化差异」解决产物不等 —— 子 agent 遇到「新侧少 2296 道 `and`（掩码）」时提议在对照工具里把
  冗余 `and` 抹掉再比；每条 `and` 是 4–7 字节机器码，所以那是**产物差异**，不是 IR 格式差异。
  抹掉之后真正的掩码 bug 也会被一起吞掉，而掩码正是窄位宽语义（U-L7）的判据载体。
  改判据器材去让红变绿，只有在「器材本身错了」时才成立。
- 别只改「已知的那两个调用点」就宣布产品路径已切换 —— 零引用证明要全仓 `grep`，包括判据入口。
  实测：切 HIR 入口那次改了 `cli/` 与 `shell/simplexc_core.cc`，漏了 `shell/core_selftest.cc`；
  而它的头注释写着「用与 `simplexc_core.cc` 相同的链」，于是「两条路逐字节相同」那条断言**静默**变成了
  跨新旧路径比较 —— 98/98 照样过，所以没有任何信号暴露这个错位。
- 别把 `host.py` 的 `detours.records` 为 0 读成「程序没调用系统 API」—— `Sleep`/`GetSystemTimePreciseAsFileTime` 归
  `PACING` 集合，只进 `pacing` 计数、永不进 `records`。实测据此误判「一次 Sleep 都没调」，实际 35 万次，
  顺着错误结论去查「程序卡在哪个非 Sleep 自旋」白花一轮。
- 别在同一个 `uc_engine` 里换 blob 或改写已翻译过的地址 —— `uc_mem_write` **不清翻译缓存**，下一趟
  `uc_emu_start` 会跑旧 TB。实测：写 sleep 段后换成 VirtualAlloc 段，结果 `clock` 又 +100 ms、`prec`
  落在旧代码边界，最后 `UC_ERR_READ_UNMAPPED`。要么 `uc_ctl_flush_tb`，要么换新地址/新引擎。
- 别拿「标量地址」当逐 tick 界标 —— `keep_regions` 用 `s.addr < hi` 判归属，标量的 region 是零长 `(addr,addr)`，
  恒不匹配，于是界标被**静默过滤**，逐 tick 那条判据悄悄退化成「只比终态」而输出照旧。实测传
  `settings+sim_cycle*8` 得到 0 个界标就是这个原因。
- 别假设「每 tick 写一次的地址」就是 tick 边界 —— `rv32im.si` 的 tick 边界是 `cycle += 1`（10229），
  而 `cycle` 是模块级 `var`（VM 全局槽，不是内存写），store 序列里看不见它。`+2567`（Output cache）
  只是体内第 9/27 条写，段末停在 tick 中途；`+2908`（10171，延迟线 LATE）才是体内最后一条写。
- 别把同一张名字表在实现分支（`e_call`）与静态类型推导（`static_type`）各写一遍 —— 后者是手抄的，
  漏一个名字就变成「推不出实参类型 → 连**单候选**的重载都判成『拿不到参数类型』」这种看不出根因的怪错。
  已两次命中：`static_type` 的 Ident 档漏「程序根体的 `const #`」（084 的 `#MOV`），Call 档漏 `random`/`ror`。
