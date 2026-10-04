# 工具状态

> 2026-09-18 建立。**项目指令文件（现名 `AGENTS.md`，2026-10-03 前叫 `CLAUDE.md`）
> 不再内联工具清单，改为引用本文件** ——
> 工具的增删改比主线变化频繁，集中在一处维护，避免两处说法打架。

## 图例

| 标记 | 含义 |
|---|---|
| ✅ | 活跃，已验证可用 |
| ⚠️ | 可用，但有已知限制（见备注） |
| ❌ | 已废弃。文件名以 `_deprecated_` 开头的，是**保留作参考但不要用** |

---

## 一、性能剖析链条（当前主线的核心）

整条链的目标：**回答「这个电路里，时间花在哪个组件上」**。
一键入口是 `profile_by_image.py`，其余是它调用的环节。

| 工具 | 作用 | 状态 |
|---|---|---|
| `compile_hook.c` | **转发层本体**。把游戏的 `compile.dll` 换掉，记录 `compile()` 的入参（源码）与产物，再转发给真实现 | ✅ |
| `deploy_compile_hook.py` | 部署 / 恢复转发层。带状态检查与防误操作 | ✅ |
| `locate_jit_image.py` | 在游戏进程里定位某份机器码的加载地址。字节级特征 + **整段校验** + **多副本枚举** | ✅ |
| `profile_by_image.py` | **一键**：定位 → 采样 → 按字节范围过滤 → 建映射 → 出报告 | ✅ |
| `sample_threads.py` | 采样各线程 RIP（被一键脚本调用）。给 `--target BASE:LEN` 就**先用字节范围探测、锁定真正在跑这份代码的线程，再只采它们**；带进度条（写 stderr，非终端时自动静默） | ✅ |
| `build_component_map.py` | 建组件↔机器码偏移映射（被一键脚本调用）。产物**一行是一段机器码**，同一组件的多段共享 `comp_index` | ✅ |
| `analyze_sim_hotspot.py` | 出组件级耗时报告（被一键脚本调用）。按 `comp_index` 聚合成**组件级**数字，默认列出**全部**类型与组件 | ✅ |

### 目录约定

```
work/hook/     捕获的源码(.si)与产物(.bin)   ← 同号即一对
work/opt/      转发层工作区：NNN.raw.bin / NNN.opt.bin / hook.log 等三个 log
work/freq/     主频采样输出：freq_log.tsv（事件）与 freq_summary.txt（给人读）
work/symbols/  符号表          work/maps/   组件映射表与建映射中间产物
work/reports/  分析报告        work/tmp/    一次性中间产物
build/         compile_hook.dll（构建产物）
```

**清空语义**（★ 2026-10-05 统一）：`work/hook/` 与 `work/opt/` 都是**转发层的工作区**，
两边的编号同出一源（DLL 的 `g_seq`），所以**每次加载（游戏启动）一起清空** —— 只清
一边就会新旧同号混在一起（实测 `optimize.log` 里同一编号攒到 5 条，而对应的
`hook_out_001.bin` 只剩本轮那份，回查指纹就会张冠李戴）。
`work/freq/` 是**采样数据**，跨会话保留 —— 它早先住在 `work/opt/` 里，会被一起清掉。

### 用法

```
python tools/profile_by_image.py                  # 默认取最新的一对
python tools/profile_by_image.py --watch          # 监听新出现的捕获
```

**编号**：**同号即一对** —— 一次 `compile()` 调用取一个号，`hook_src_005.si`
与 `hook_out_005.bin` 就是同一次调用。
（★ 2026-09-19 改的：早先是 src/out 各取一次号，编号差 1，配对要看 `src_069 ↔ out_070`，
已统一。）

### 六个容易踩的坑

1. **产物文件前 8 字节是长度头，不是机器码**。而且头字段的值等于**文件总长（含头
   自己）**，不是数据长度 —— 按 `head == len - 8` 判断会判错，带着头去搜会让特征
   偏移整体错 8 字节。`load_body()` 里已封装。
2. **同一份机器码在内存里可能有多个副本**（每编译一次占一个新 JIT 区，旧区不释放）。
   实测同一电路编译 31 次 → 31 个副本。**必须靠采样筛出「正在跑的那个」**，
   不能拿 `locate()` 返回的第一个。
3. **转发层必须用自身模块路径加载 `compile_real.dll`**，不能写相对路径 —— 相对路径
   按宿主进程的当前目录解析，离线调用时会失败（返回 NULL，表现为"编译失败"）。
4. **编号计数器是 DLL 的全局变量，每次加载（游戏启动）都从 1 重编号** ——
   所以转发层**加载时会清空目录里的 `.si`/`.bin`**（★ 2026-09-19 加的；
   **不含子目录**，想长期保留的捕获自己挪进子目录）。
   本条的历史：早先不清空，于是新旧同号混在一起、互相覆盖（实测 09-19 00:23 那批
   001–006 被 00:35 那批 001–004 覆盖掉）。**判断新旧一律看 mtime，不看编号** ——
   按编号取最大会选中旧进程的产物，它的 JIT 区已随进程消失 → 定位 0 候选，
   表现得像"工具坏了"。
5. **`py` 启动器按脚本的 shebang 选解释器**（★ 2026-09-19 踩到，极隐蔽）。
   首行写 `#!/usr/bin/env python3` 时，Windows 的 PyLauncher 把 `python3` 当
   **版本说明符**，去 **PATH** 里找 `python3.exe` —— 本机命中 MSYS2 的
   `D:/bin/msys2/ucrt64/bin/python3.exe`（Python 3.14.6）。同一个
   `compile_real.dll` 在**那个**解释器里编译慢 **16 倍**（12.7s/次 vs 0.78s/次），
   整条流程 12s 变 36s，**没有任何报错**，只是慢。

   | 脚本首行 | `py` 选中 | `build_component_map` 耗时 |
   |---|---|---|
   | `#!/usr/bin/env python3` | MSYS2 python3.exe | **26s** |
   | 无 shebang | 官方 3.13 | 2s |
   | `#!python3` | 官方 3.13 | 2s |

   **工具脚本一律写 `#!python3`**（PyLauncher 解析出「3.x」，走注册表而不是 PATH）。
   已全量改过 `tools/` 下 16 个脚本。
   缓解手段：`build_component_map` 现在会打印每次编译的耗时 —— 正常约 0.8s，
   若看到十几秒，先查解释器（`py -c "import sys;print(sys.executable)"`）。
   ⚠️ 未查明的一层：两个解释器加载的依赖 DLL 来源相同（都是游戏目录的版本），
   所以「MSYS2 的 Python 为何慢 16 倍」只知其然。
6. **转发层部署期间，游戏目录的 `compile.dll` 不是编译器**（★ 2026-10-03 踩到）。
   那时它就是转发层本身：**67375 字节**，与 `build/compile_hook.dll` 同源（mtime 一致）。
   真编译器是 **`compile_real.dll`**（1800374 字节；符号 7447 行、代码符号 2012 个、
   含 `passes` 的 1052 个）。**查静态信息（符号 / 导入表 / 反汇编）一律认它。**
   踩法：照 `compile.dll` 查符号 —— 只剩 113 个代码符号、`rawNewString` 零命中，
   于是误判「编译器不带 Nim 运行时」；而 `compile_real.dll` 内部**定义**着
   `rawNewString`（T）、`newSeq`×60、`dealloc`×5，导入表只有 libgcc + KERNEL32 + UCRT，
   **不碰游戏运行时**；游戏 exe 里另有一份独立的 `rawNewString`，两份运行时两个堆。
   部署状态用 `python tools/deploy_compile_hook.py --status` 确认。

---

## 一·B、源码处理

| 工具 | 作用 | 状态 |
|---|---|---|
| `format_si.py` | 格式化 `.si`：**压连续空行 / 去行尾空白 / 去文末空行**，不动任何非空行（实测 332 行逐字节一致） | ✅ |

借不到 VSCode 插件的能力：`michai.tc-si` 只提供 TextMate 正则高亮，**没有 formatter 贡献点**。

---

## 一·C、产物改写（当前主线）

转发层把产物交给 `optimize_image.py` 走一趟、再原样写回（见 `compile_hook.c` 的
`optimize_inplace()`）。配置在 `opt.ini`，**每次编译重新读**，改完下次编译生效。

| 工具 | 作用 | 状态 |
|---|---|---|
| `optimize_image.py` | **改写器**。当前规则：`mov ecx,imm32` + `移位 r,cl` → `移位 r,imm8` + 单条 4 字节 nop（等长，零重定位）。实测该规则把 219088 字节的电路从 **5.4 提到 6.9 MHz** | ✅ |
| `check_shift_equiv.py` | 把改写前后的两段字节各拷进一块 RWX 内存**真的跑一遍**对比结果，验证规则语义等价（当前 5280 组） | ✅ |
| `try_compile_dll.py --hook` | 强制加载转发层调 `compile()`，用于**离线**验证这条管路（注意转发层加载时会清空 `work/hook/`） | ✅ |

运行痕迹都在 `work/opt/`（**每次游戏启动清空**）：`hook.log`（每次编译是「已就地写回」
还是「放弃」）、`optimize.log`（改写处数 / 是否被配置关掉 / 产物 sha256）、
`opt_stderr.log`（自检与抽查明细）。

**`opt.ini` 不在 git 里**（★ 2026-10-05）：它每做一次「开 / 关」对照实验都要改，
留着会让工作区长期显示为脏。模板是 `opt.ini.example`，首次
`cp opt.ini.example opt.ini`。切换方式就是搬注释符（`;` 与 `#` 都行，
`configparser` 两种都认）：

```ini
rewrite = on
; rewrite = off
```

同一时刻**只能有一行生效** —— 两行都放开时 configparser 会因重复 key 报错，
`rewrite_on()` 便按「开启」兜底。

---

## 二、动态分析（对运行中的游戏）

| 工具 | 作用 | 状态 |
|---|---|---|
| `winmem.py` | **公共基础设施**：只读的进程/内存访问（`find_pid` / `find_module` / `rpm` / `iter_exec_regions` / `MBI`） | ✅ 被多个脚本 import |
| `term.py` | **公共基础设施**：终端着色。`NO_COLOR` / `FORCE_COLOR` / `isatty()` 三级判断；`paint` / `heat`（按占比）/ `duration`（按耗时）。重定向到文件时自动禁用 | ✅ |
| `read_freq.py` | **读游戏当前实测主频**（UI 显示的那个 MHz）。`sim_speed` = float32 Hz，模块内偏移 `0x624D618`；同时用 `last_cycle_count` / `last_speed_check` 自算一条对照。对照实验不必再靠人眼读数。⚠️ Steam 更新会让偏移位移 | ✅ |
| `watch_freq.py` | **被动主频采样器**：终端实时显示（停机心跳用 `\r` 原地刷新，不刷屏），退出时按「编译区间」给一份总结 —— 两次编译之间必是同一次运行，所以**一个电路一行**（取该区间最大的有效段中位数，过渡段与停机段不参与，没跑起来的区间省略）。**取数只读 `work/freq/freq_summary.txt`**（几十行，**每次运行覆盖**，只留最近一份；历史靠同目录 `freq_log.tsv` 的 `summary` 行回溯，`grep summary` 即可，不必读那几百行事件流）；`work/freq/freq_log.tsv` 只记事件与段摘要，别手动读。编译点带产物 sha256 前 8 位 = **电路指纹**（同电路重复编译指纹相同）。启动时也会补报当前在跑的产物 | ✅ |
| `scan_game_mem.py` | 扫内存、dump JIT 可执行区 | ⚠️ **抓源码部分已废弃**（改用 hook `compile()`）；`--dump-exec` dump JIT 区仍有用 |
| `try_compile_dll.py` | 独立加载 `compile.dll` 调用它（脱离游戏进程） | ✅ ⚠️ 转发层部署期间，**必须从游戏目录运行**（否则转发层找不到真 DLL） |
| `analyze_gen_code.py` | 统计生成源码的组件构成与代码代价 | ⚠️ 需要源码文件输入 |
| `analyze_jit_code.py` | 量化 JIT 机器码的冗余指令 | ✅ **解析已修（2026-10-03）**：改严格正则 + **把续行拼回上一条指令**（objdump 字节列上限 7 字节，超出的另起一行且无助记符 —— 旧 `LINE_RE` 用 `(\S+)` 回溯会把行尾字节吃成「助记符 `00`」）。实测 `plain.bin`：虚高 4851 条（55254 → **50403**，已跳过 8 字节长度头）；**绝对条数不变**（4913 对 mov / 6223 处 `cl`），只有分母错 → 冗余 **16049 条 = 31.8%**（旧报 29.0%）。新增自检「偏移连续 / 零空洞 / 字节铺满」，不自洽即报错返回 2。另：`movabs` 的 10 字节原先只拿到 7 字节，重定位会错位。⚠️ `HANDOFF-2026-10-03.md` 里的 50783 / 31.6% 是修正前旧值，**以本行为准** |
| `_deprecated_catch_compile_source.py` | 调试器 + 硬件断点抓 `compile()` 参数 | ❌ **会崩游戏**（DR 残留），已被转发层方案取代 |
| `_deprecated_profile_current.py` | 旧的一键流程 | ❌ 第一步是扫内存抓源码，那个环节已被淘汰，必然报"没有语法完整的候选源码" |
| `hw_breakpoint_trace.py` | 硬件断点追踪「谁写了某地址」 | ❌ **未清理调试寄存器**，就是 2026-09-17 崩游戏两次的元凶。**重启这条线前必须先修** |

---

## 三、静态逆向（对 exe / dll 文件）

| 工具 | 作用 | 状态 |
|---|---|---|
| `pdata_bounds.py` | 函数精确边界（解析 PE `.pdata`） | ✅ **唯一可信的静态工具** —— 数据源独立于符号表，自带 staleness 警告 |
| `find_sym.py` | 按模块 / 函数名 / 地址查符号 | ⚠️ `--name` 只匹配 demangle 后的**进程名**，模块段不参与，所以 `--name code95gen` 查不到东西 |
| `nim_demangle.py` | 符号 demangle + 模块树 | ⚠️ 正则含 `\x00` 导致整表匹配成 1 条，**字符串表取模块的功能已失效** |
| `demangle_dll.py` | compile.dll 符号 demangle | ⚠️ 路径写死旧目录 |
| `coff_symbols.py` | COFF 符号表 → 编译单元/函数 | ❌ **截断 bug**（长名少前 4 字符：`contains`→`ains`），产物已弃用。**改用 `nm` 输出** |
| `organize_strings.py` | exe 字符串分类 | ❌ 输入文件不存在，已失效（旧产物已删除） |
| `pe_overlay.py` | PE 结构解析 | ⚠️ **会写文件到硬编码路径**，别在游戏目录跑 |

**符号表的正确来源**：直接跑 `nm`

```
D:/bin/w64devkit/bin/nm.exe "<游戏目录>/Turing Complete.exe" > work/symbols/exe_nm.txt
```

42881 行，未 strip，符号名完整。**别用 `coff_symbols.py` 的产物**。

---

## 四、待办：工具层面的技术债

1. ~~`sample_threads.py` 的线程探测判据换成 `QueryThreadCycleTime`~~ ✅ 已完成
   （2026-09-19），**但没用 `QueryThreadCycleTime`** —— 那东西给的是线程累计
   **周期数**，只能回答"哪个线程在烧 CPU"，**给不出 RIP**，替代不了采样。
   实际改成了**用产物字节范围探测**（`--target`），与过滤判据同源。
2. `hw_breakpoint_trace.py` 补上 DR 清理，或直接删除
3. ~~`profile_by_image.py` 的调用方式封装~~ ✅ 已完成（默认取最新一对，另有 `--watch`）
