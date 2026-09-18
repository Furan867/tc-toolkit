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
| `sample_threads.py` | 采样各线程 RIP（被一键脚本调用） | ⚠️ 线程探测判据仍靠瞬时 RIP；一键脚本用 `--all-threads` 绕过 |
| `build_component_map.py` | 建组件↔机器码偏移映射（被一键脚本调用） | ✅ 坐标偏差恒等，38/38 锚点全命中 |
| `analyze_sim_hotspot.py` | 出组件级耗时报告（被一键脚本调用） | ✅ |

### 目录约定

```
work/hook/     捕获的源码(.si)与产物(.bin)   ← 不可再生，别删
work/symbols/  符号表          work/maps/   组件映射表与建映射中间产物
work/reports/  分析报告        work/tmp/    一次性中间产物
build/         compile_hook.dll（构建产物）
```

### 用法

```
python tools/profile_by_image.py                  # 默认取最新的一对
python tools/profile_by_image.py --watch          # 监听新出现的捕获
```

**注意**：`hook_src_NNN` 与 `hook_out_NNN` 是**配对**的（同一次 `compile()` 调用），
但编号**不连续** —— 两者共用一个递增计数器，所以源码是奇数、产物是偶数
（`src_069` ↔ `out_070`）。

### 三个容易踩的坑

1. **产物文件前 8 字节是长度头，不是机器码**。而且头字段的值等于**文件总长（含头
   自己）**，不是数据长度 —— 按 `head == len - 8` 判断会判错，带着头去搜会让特征
   偏移整体错 8 字节。`load_body()` 里已封装。
2. **同一份机器码在内存里可能有多个副本**（每编译一次占一个新 JIT 区，旧区不释放）。
   实测同一电路编译 31 次 → 31 个副本。**必须靠采样筛出「正在跑的那个」**，
   不能拿 `locate()` 返回的第一个。
3. **转发层必须用自身模块路径加载 `compile_real.dll`**，不能写相对路径 —— 相对路径
   按宿主进程的当前目录解析，离线调用时会失败（返回 NULL，表现为"编译失败"）。

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

1. `sample_threads.py` 的线程探测判据换成 `QueryThreadCycleTime`（不需要挂起线程）
2. `hw_breakpoint_trace.py` 补上 DR 清理，或直接删除
3. ~~`profile_by_image.py` 的调用方式封装~~ ✅ 已完成（默认取最新一对，另有 `--watch`）
