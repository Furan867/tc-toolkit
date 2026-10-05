# 重编译触发机制调查（收尾记录）

- 日期：2026-10-06
- 结论：**脚本无法触发重编译** —— 8 条路全部失败，根因已钉死
- 后续走向：注入 UI 事件（键盘/鼠标）

## 一句话结论

游戏内部的「编译触发」**依赖活的游戏状态**（board 对象 + 内存管理器 + UI 上下文），
而且那些触发函数**只能在主线程里执行**。因此：

- 从外部写内存 ✗（没有任何数据开关）
- 从外部线程调用游戏函数 ✗（并发访问 → 崩）
- 捕获参数快照再重放 ✗（快照里的指针几秒后即失效）

**唯一可行的执行环境是游戏主线程** —— 也就是必须让游戏自己发起这次编译，即注入 UI 事件。

## 已定位的触发链

```text
main 主循环（事件码 sil > 0x20 的分支，RVA 0x48306a 处的 call）
  └→ preorder_from_frontend(rcx=frontend, rdx=108KB 电路快照)   0x140316750, 329 B
       ├─ sim_stop_and_refresh()
       ├─ reset_ui(frontend)
       ├─ memcpy(栈上, rdx, 0x1a630)
       └→ preorder(rcx=frontend, rdx=栈上快照, r8d=bool)        0x140261a20, 2382 B
            └→ compile_thread_send(rdx=preorder)                u3000 @ 0x140261790
```

「加/删一根线」走的就是这条；编辑后游戏「无响应」数十秒，是它在重建场景。
`preorder_from_frontend` 只被 6 个 `load_*` 和 `main` 调用 —— 编辑电路走的是 **main 的事件分发**。

## 八次尝试与各自死因

| # | 路径 | 死因 |
| --- | --- | --- |
| 1 | 外部写 `commands[]` 触发编译 | 编译是纯事件驱动，没有任何数据开关 |
| 2 | 调 `await_build_result(board, out)` | 开头 `if compile_thread_synced() return` —— 已同步时什么都不做 |
| 3 | 调 `try_update_preorder_result(board, out)` | 先 `peek(response_channel)`，空则直接返回（它是「消费响应」的） |
| 4 | 重放发往 response_channel 的响应 | `r8` 指向的数据已释放（全 0）+ `sendImpl` 无消费者时阻塞，把游戏拖死 |
| 5 | 读 channel data 缓冲残留 | 有残留但格式不足以重发 |
| 6 | 转发层 hook + 裸拷 preorder 1024 B | 跨页越界（崩溃） |
| 7 | `eqdup` 深拷贝 preorder + 重放 | 深拷贝里的指针在重放时已失效（崩溃） |
| 8 | 捕获 108KB 电路快照 + 重放 `preorder_from_frontend` | 同上：快照里的指针不活（崩溃）；且远程线程调用与主线程并发 |

## 顺带钉死的关键事实

- `compile_thread_send(rcx 未用, rdx = preorder)` —— **u3000**（`preorder` / `try_update_preorder_result` / `main` 用）
- `compile_thread_send(rcx = preorder, ...)` —— **u3021**（`asm_assemble` / `asm_isa_compile` / `build_schematic` 用）—— 两个版本参数寄存器不同
- 编译请求的**唯一出口**是 `compile_thread_send`；`next_sim_version` 是每次请求分配的新版本号
- `preorder` 是**每次调用现场生成的临时对象**，函数一返回内容就被冲掉（事后读全 0）
- 转发的 `compile()`（即 opt.ini 改写生效点）**只在游戏自己发起编译时被调用**

## 下一步方向：注入 UI 事件

优先级从高到低：

1. **键盘快捷键 + `PostMessage`** —— 首选。不需要鼠标坐标，也不需要窗口在前台。
   例如给游戏发 Ctrl+Z / Ctrl+Y（撤销/重做）来回切换，制造「电路变化」。
   **待验证**：游戏是否接受 `PostMessage` 的按键（有些引擎只认 Raw Input / `GetAsyncKeyState`）。
2. **鼠标模拟**（`SendInput`）—— 需要精确坐标（元件位置），且在重建场景的数十秒里点击可能丢。

两条路都避开"进程内调用"这个死结：动作由游戏主线程自己发起，状态天然有效。

## 相关文件

- `work/tmp/control_map.md` —— 完整控制机制地图（含实验 A 的实测数据、本调查的过程记录）
- `work/tmp/probe_csend.py` —— 就地探针（记录参数与调用者），本次调查的主力工具
- `docs/specs/2026-10-05-sim-ctl-design.md` —— simctl 工具的设计文档
