# 工具状态

> 2026-09-18 建立。**CLAUDE.md 不再内联工具清单，改为引用本文件** ——
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
work/hook/     捕获的源码(.si)与产物(.bin)   ← 同号即一对；**转发层加载时清空**
work/symbols/  符号表          work/maps/   组件映射表与建映射中间产物
work/reports/  分析报告        work/tmp/    一次性中间产物
build/         compile_hook.dll（构建产物）
```

### 用法

```
python tools/profile_by_image.py                  # 默认取最新的一对
python tools/profile_by_image.py --watch          # 监听新出现的捕获
```

**编号**：**同号即一对** —— 一次 `compile()` 调用取一个号，`hook_src_005.si`
与 `hook_out_005.bin` 就是同一次调用。
（★ 2026-09-19 改的：早先是 src/out 各取一次号，编号差 1，配对要看 `src_069 ↔ out_070`，
已统一。）

### 五个容易踩的坑

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

---

## 一·B、源码处理

| 工具 | 作用 | 状态 |
|---|---|---|
| `format_si.py` | 格式化 `.si`：**压连续空行 / 去行尾空白 / 去文末空行**，不动任何非空行（实测 332 行逐字节一致） | ✅ |

借不到 VSCode 插件的能力：`michai.tc-si` 只提供 TextMate 正则高亮，**没有 formatter 贡献点**。

---

## 二、动态分析（对运行中的游戏）

| 工具 | 作用 | 状态 |
|---|---|---|
| `winmem.py` | **公共基础设施**：只读的进程/内存访问（`find_pid` / `find_module` / `rpm` / `iter_exec_regions` / `MBI`） | ✅ 被多个脚本 import |
| `scan_game_mem.py` | 扫内存、dump JIT 可执行区 | ⚠️ **抓源码部分已废弃**（改用 hook `compile()`）；`--dump-exec` dump JIT 区仍有用 |
| `try_compile_dll.py` | 独立加载 `compile.dll` 调用它（脱离游戏进程） | ✅ ⚠️ 转发层部署期间，**必须从游戏目录运行**（否则转发层找不到真 DLL） |
| `analyze_gen_code.py` | 统计生成源码的组件构成与代码代价 | ⚠️ 需要源码文件输入 |
| `analyze_jit_code.py` | 量化 JIT 机器码的冗余指令 | ⚠️ |
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
