# 地图

项目由哪些东西组成、东西在哪。**找不到东西时读这里**。靠自觉维护——路径指错了，一用就撞上。

## 是什么

理解游戏的**仿真逻辑**，用于优化游戏内电路的仿真速度。搞不清现状时读 `status.md`；
需要某条结论读 `facts.md`；动手改某块代码前读 `pitfalls.md`。

## 环境

- 游戏：`F:\opt\SteamLibrary\steamapps\common\Turing Complete\`
- 架构：Nim 2.2.6 编译的 exe + Godot 3.4.2 宿主 + 自研编译器 `compile.dll`
- ⚠️ **Steam 更新会让全部函数地址位移** —— 动手前先比对 exe 的 mtime 与产物
- 工具链：`D:\bin\w64devkit\bin\{nm,objdump}.exe`、Python 3.13、`D:\bin\7z.exe`

## 目录

```
tools/        分析脚本与工具（已固化的）
docs/         本套文档 + 专题记录（specs/ 下是设计文档）
work/hook/    转发层捕获：hook_src_NNN.si ↔ hook_out_NNN.bin（同号即一对）
work/opt/     转发层工作区：NNN.raw.bin / NNN.opt.bin / hook.log / optimize.log / opt_stderr.log
work/freq/    主频采样输出：freq_log.tsv（事件）与 freq_summary.txt（给人读）
work/symbols/ 符号表        work/maps/   组件映射表与建映射中间产物
work/reports/ 分析报告      work/tmp/    一次性中间产物
build/        compile_hook.dll（构建产物）
docs/agent…   无
```

`work/hook/` 与 `work/opt/` 每次游戏启动一起清空（见 `facts.md` 的「清空语义」）；
`work/freq/` 跨会话保留。

## 六条工作线

按时间顺序（新的在前）。它们不是并列的，是**下层给上层供货**：

```
sim_ctl ──→ 机器码改写 ──→ 主频监听 ──→ 热点分析 ──→ compile.dll 转发 ──→ 游戏反编译
（让上面能自动跑）  （改产物）    （测效果）    （用产物做组件级分析）（供源码+产物）（已降级）
```

| 线 | 做什么 | 状态 | 结论在哪 |
|---|---|---|---|
| **sim_ctl** | 脚本化「重编译 / 运行 / 停止」，供实验脚本 import | 已建成、在用 | `facts.md` §simctl |
| **机器码改写** | `optimize_image.py` 改写产物：四态、两条规则、锚点协议 | 已建成、在用 | `facts.md` §机器码改写 |
| **主频监听** | `read_freq` / `watch_freq` 被动记录实测主频 | 已建成、在用 | `facts.md` §主频读数 |
| **热点分析** | 五层管道：抓源码 → 识别 → 建映射 → 采样 → 出组件报告 | 已建成；**下一个要改进的目标** | `facts.md` §组件映射、§采样 |
| **compile.dll 转发** | 转发层劫持 `compile()`，拿源码与产物 | 已建成、在用 | `facts.md` §compile.dll 转发层 |
| **游戏反编译** | 反编译 exe/dll、符号表、字符串 | **已降级，非主线** | `facts.md` §静态逆向 |

「热点分析」这条线的下一步是**改进它本身**（现成的最老派一环是手写 `CONTEXT` 布局的采样）。

## 工具清单

工具的用途、状态与技术债见 `TOOLS.md`（工具文档，按主题组织）。
