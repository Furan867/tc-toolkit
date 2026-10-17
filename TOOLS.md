# 工具

每个工具的用途与状态。**用法细节看各自的 docstring**（`python tools/<x>.py --help`）；
踩过的坑在 [`pitfalls.md`](pitfalls.md)，结论在 [`facts.md`](facts.md)，项目结构在 [`map.md`](map.md)。

图例：✅ 可用 ｜ ⚠️ 有前提 ｜ ❌ 已废弃

## 性能剖析链（主线）

| 工具 | 作用 | 状态 |
|---|---|---|
| `profile_by_image.py` | **一键入口**：定位产物 → 采样 → 按字节范围过滤 → 建映射 → 出报告 | ✅ |
| `build_component_map.py` | 建组件 ↔ 机器码偏移映射（被上一行调用） | ✅ |
| `analyze_sim_hotspot.py` | 组件级耗时报告（按 `comp_index` 聚合） | ⚠️ ⑥-B `com_none` 未滤除，待修 |
| `sample_threads.py` | RIP 采样 | ✅ |
| `deploy_compile_hook.py` | 构建 / 部署 / 查询转发层（子命令 `build-deploy`、`status`…） | ✅ |

## 源码与产物

| 工具 | 作用 | 状态 |
|---|---|---|
| `compile_hook.c` → `build/compile_hook.dll` | **转发层**：劫持 `compile()`，拿源码与产物，并记 `work/hook/manifest.tsv`（flags / Result 5 字段 / sha256） | ✅ |
| `optimize_image.py` | 机器码改写器：四态 + 两条规则 + 锚点协议 | ✅ |
| `check_shift_equiv.py` | 把改写前后的字节真跑一遍，验证语义等价 | ✅ |
| `try_compile_dll.py` | 离线加载 `compile.dll` 调 `compile()` | ✅ ⚠️ 须从游戏目录运行 |
| `analyze_jit_code.py` | 量化 JIT 机器码的冗余（带解析自检） | ✅ |
| `scan_game_mem.py` | 扫内存 / `--dump-exec` dump JIT 区 | ⚠️ 抓源码部分已废弃 |

## 动态分析（对运行中的游戏）

| 工具 | 作用 | 状态 |
|---|---|---|
| `winmem.py` | 公共基础设施：进程 / 内存只读访问 | ✅ |
| `term.py` | 公共基础设施：终端着色 | ✅ |
| `read_freq.py` | 读当前实测主频（UI 上那个 MHz） | ✅ |
| `watch_freq.py` | 被动主频采样器，按编译区间出总结 | ✅ |
| `simctl.py` | 重编译 / 运行 / 停止（供实验脚本 import） | ✅ |

**simctl 的操作约定**（踩过才写在这里，别散到 `facts.md` 里）：

- `run` 注入 F6，是**运行/暂停切换**，不是「确保在运行」；连发两次等于没发。
- `recompile` 注入 Ctrl+Z / Ctrl+Shift+Z（按净消耗交替），会让游戏重建场景 →
  **仿真停在停止状态**；要采样必须再 `run()`。
- 改 `opt.ini`（切 `compiler` / `rewrite`）后必须先 `recompile()` 才生效，再 `run()` 恢复运行，然后才采。
- 三者都要求**游戏窗口在前台**；脚本化时会抢焦点。

## 静态逆向（对 exe / dll）

| 工具 | 作用 | 状态 |
|---|---|---|
| `pdata_bounds.py` | 函数精确边界（解析 PE `.pdata`） | ✅ 唯一可信 |
| `find_sym.py` | 按模块 / 名字 / 地址查符号 | ⚠️ `--name` 只匹配进程名 |
| `analyze_gen_code.py` | 统计生成源码的组件构成与代码代价 | ⚠️ 需源码输入 |
| `nm`（w64devkit） | **符号表的正确来源**（未 strip） | ✅ |
| `nim_demangle.py`、`demangle_dll.py`、`coff_symbols.py`、`organize_strings.py`、`pe_overlay.py`、`_deprecated_*` | 反编译辅助 / 旧流程 | ❌ 已失效、路径写死或会崩游戏，别再依赖 |

## 目录约定

```
work/hook/     捕获的源码(.si) 与产物(.bin)   ← 同号即一对；manifest.tsv 为每次调用的结构化记录
work/opt/      转发层工作区：NNN.raw.bin / NNN.opt.bin / hook.log（唯一记录型日志）+ opt_stderr.log
work/freq/     主频采样：freq_log.tsv（事件）与 freq_summary.txt（给人读）
work/symbols/  符号表        work/maps/  组件映射表与建映射中间产物
work/reports/  分析报告      work/tmp/   一次性中间产物
build/         compile_hook.dll（构建产物）
```

清空语义见 [`facts.md`](facts.md)。**取主频数只读 `work/freq/freq_summary.txt`**。

## 技术债

1. `hw_breakpoint_trace.py`：**重启这条线前必须先补 DR 清理**（否则崩游戏），或直接删
2. `analyze_sim_hotspot.py`：⑥-B（`com_none` 未滤除、区间归属错）待修
3. 上表标 ❌ 的废弃工具应择机清理
