# 设计：仿真控制工具 simctl

- 日期：2026-10-05
- 分支：`feat/sim-ctl`
- 状态：设计已定，待写实现计划

## 目标

让外部脚本能脚本化这一循环：**改 opt.ini → 重编译 → 运行 → 停止 → 再改**。
本工具只负责其中「重编译 / 运行 / 停止」三个动作，供其他脚本调用。

## 接口

核心模块 `tools/simctl.py`，暴露三个函数：

```python
simctl.run()         # 启动电路运行
simctl.stop()        # 停止电路运行
simctl.recompile()   # 停止 → 刷新 → 触发重编译
```

- 三者都返回结构化结果（成功/失败 + 关键状态读数），失败给原因
- 内部缓存进程句柄；PID 或模块基址变化时自动重查（游戏重启、Steam 更新后地址位移）
- 另附同名 CLI 薄壳，供手工敲或其他语言调用（`import` 比每次起解释器快，这是留函数接口的原因）

## 行为契约

### `run`

写四个字段：

| 字段 | 值 |
| --- | --- |
| `settings[1]` sim_target_cycle | `INT64_MAX` |
| `commands[0]` ctl_command | `0` (run) |
| `commands[1]` ctl_command_id | 新 id |
| `simulation_next_command_id` | 新 id + 1 |

判据：`set[0]` (sim_cycle) 递增。**已实测** —— 1 秒后电路跑起来，读到 10.75 MHz。

### `stop`

写 `commands[0] = 3` (quit_simulation) + 新 id。

判据：`set[14]` (sim_running) == 0 且 `set[0]` 为哨兵。

> 说明：`3` 是「停止」按钮与「编辑电路时自动停止」共用的命令（实测日志 40.1s）。
> `2` (mode_reset) 是「暂停」，语义不同，本工具不用。

### `recompile`

固定序列，无分支：

```text
1. stop（写 3 + 新 id）
2. 等 set[14] == 0
3. 写 commands[0] = 1 (refresh) + 新 id
4. 经通道请求转发层执行 await_build_result(board, scratch)
5. 等：通道回报 ok + work/hook/ 出现新编号 + next_sim_version 增长
```

第 3 步照抄游戏「编辑电路」路径（实测 40.1s `quit_simulation` → 41.9s `refresh`）。
我们做的正是那条路径的等价物 —— 换掉电路内容，直接让它重编译一次。

三个判据必须同时成立才算成功；任一步超时即返回失败，不回滚。

### 通道（内部实现，仅约定行为）

`stop`/`run` 走写内存，不经通道。只有 `recompile` 需要请求游戏进程内执行，走文件通道：

- **不丢失、不覆盖**：连续两次 `recompile()` 必须排成两次执行，前者结果不能被后者冲掉
- **有完成回报**：调用方能拿到「已执行 / 失败 + 原因」，不能只发不问
- 工具侧不假设游戏何时处理，一律带超时轮询

## 并发约定

`simctl` 的每个动作（`recompile` 指整条序列）同一时刻只允许一个在执行：
内部用一把文件锁串行化，跨进程有效。并发调用排队等待，而不是交错执行 ——
否则两次 `recompile` 的 stop / refresh 命令会互相插入，状态不可预期。

## 转发层扩展

在现有 `compile_hook.dll` 上加两个点：

1. **hook `await_build_result` 入口** —— 把 `rcx`（board 指针）记下来，然后原样转发。
   游戏加载电路时必然经过一次，之后 `recompile` 就用这个指针。
2. **主线程 hook 点**（候选 `sim_do`）—— 每帧检查通道，有请求就调用
   `await_build_result(board, scratch)`，其中第二参数只是 200 字节输出结构
   （由 `wait_for_sim_sync` 的代码形态确证：局部清零 25×8 字节后传入）。

转发层继续只做「转发 + 记录」，不修改游戏状态。

## 实现前必须实测的两件

1. **`3` → `1` 之后能否直接 `run`** —— 若不能，序列缺了什么动作
2. **board 指针在切换电路后是否失效** —— 失效则每次 hook 到就刷新

## 风险

- 写内存与进程内调用都可能让游戏崩溃；用户已接受此风险
- 所有地址依赖进程实例，游戏重启即失效（工具自动重查）
- Steam 更新会让函数地址位移，hook 点需重新确认
