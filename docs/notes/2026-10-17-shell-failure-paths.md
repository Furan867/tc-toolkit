# 壳的失败路径与熔断（`tools/compile_hook.c`）

- 日期：2026-10-17 ｜ 写者：壳/熔断子 agent ｜ 锁：`work/tmp/locks/compile-hook.lock`（做完置回空闲）
- 接口依据：`docs/specs/2026-10-15-simplexc-interfaces.md` §5（三模式 + `impl` 词表）、§8（错误分类）
- 判据代码/输出：`work/tmp/hook_test/`（harness.py、tools/rewrite_stub.py、bin/、evidence/*.txt）

## 结论

| 要求 | 落地 | 判据 |
|---|---|---|
| ours 失败也记账 | 失败不再早返回：`error` 行 + `impl` 行 + **manifest 行**（`impl=ours_failed`，无产物写 `-`） | 情形 1 |
| §8 分类落地 | `ours` 路径每个失败点返回分类串；`Kind.*` 四串一处定义 | 情形 1（InternalError）、超时（Timeout） |
| ours 超时预算 | `GetTickCount64` 量 ours 段；默认 5 s，`TC_OURS_TIMEOUT_MS` > `opt.ini [opt] ours_timeout_ms`；超预算丢产物 → 按模式失败/回退 | 超时两情形 + 反证（`big-ours-ok`） |
| VEH 只包 ours 段 | 惰性装一次，作用域靠 `g_in_ours`；触发 → 永久熔断 + 上下文搬回恢复点 | 已装（`guard=veh`）；**触发未实测**，见下 |
| impl 行 per-call | **每次** compile() 都写，键 `impl`/`mode`/`result`（另附 `budget_ms`、首次加 `guard=veh`） | 情形 1/2/3/4 |
| reference 不回归 | 同一进程内新旧两份壳的参考产物逐字节相同 | 判据 3（`refcmp`） |

## 改了什么

`tools/compile_hook.c`（独占写，本次唯一改的源码文件）：

1. **路径宏包 `#ifndef`**：任务点名的 `OURS_BLOB` / `OUT_DIR` / `OPT_DIR`，**另加 `OPT_INI`、`OPT_SCRIPT`**。
   只加这两处的原因：离线判据**必须换掉** `compiler=` 的 ini 才能选模式，也**必须换掉改写器**
   —— 真 `optimize_image.py` 会 `import hook_log`，而那份模块的 PATH 写死 `work/opt/hook.log`，
   一次参考编译就会往主程的活日志里掺一行（bench_rewrite 靠数它的新行）。默认值逐字符未动，
   正式构建（`py tools/deploy_compile_hook.py build`）不给任何 `-D`，行为与改动前一致。
2. **`ours_compile_spike` 返回值改成 §8 分类串**（NULL = 成功）：blob 缺失/长度非法/无 bit62/entry 文件或越界 → `InternalError`；
   > 64 MB → `BlobTooLarge`；超预算 → `Timeout`；VEH 拦下的异常 → `OursFault`。
   `UnsupportedConstruct` / `RelocUnsupported` / `SymbolUndefined` 属于编译器核心（前端/装配器），还没接进来。
3. **`ours_segment()`**：VEH 安装 + 恢复点 + 计时；超预算时丢弃产物并清干净 `out` 里指向它的字段（不留悬垂指针）。
4. **VEH（`ours_veh`）**：只认 8 个真异常码（AV/非法指令/特权/除零/溢出/越界/页错误/栈溢出），
   断点、单步、C++ 抛出等一律 `CONTINUE_SEARCH` 穿透；命中后置 `g_ours_fault` + `InterlockedExchange(&g_breaker,1)`
   （本进程永久），再把 `g_resume` 搬回 `ContextRecord` → 控制流回到 `ours_segment()`。
   处理器里不分配内存、不 LoadLibrary、不写文件。加了线程号校验（壳本来就是单线程假设）。
5. **`compile()` 重排**：选实现 → ours 段 → 失败分类记账（`error` 行）→ `impl` 行 → 参考调用（若需要）→ dump/manifest。
   `ours` 模式失败写自造失败 Result（`out[3]≠0`，内容 `ours: <分类串>`）；`ours_with_fallback` 整单转交；
   熔断后按模式当"ours 失败"处理（`ours` 给失败 Result、`ours_with_fallback` 转交）。
   顺带修了两处老问题：`out == NULL` 时 `ours` 模式原来会写空指针，现在跳过；失败路径不再漏 manifest。
6. 文件头 docstring 的「入口」段补了 impl 词表与失败路径说明。

**`impl` 行的键**：`impl` / `mode` / `result`（§5 冻结的三个）+ `budget_ms`（本次用的超时预算，证明环境变量/ini 生效）
+ 首次安装 VEH 的那一次多一个 `guard=veh`。hook.log 的"只加不改"约定照旧成立；
`tools/hook_log.py` 里 `impl` 事件那张键表需要补 `mode`（**我没动那个文件**，超出本次允许改动范围）。

失败串前缀从 `ours: no product available` 改成 `ours: <分类串>`（§5 要求），全仓没有别的消费者匹配旧串（已 grep）。

## 判据

命令（都在 `D:/home/code-proj/Agent/proj/tc/tc-toolkit` 下）：

| 命令 | 结果 |
|---|---|
| `py tools/deploy_compile_hook.py build` | rc=0，`build/compile_hook.dll` 111693 B |
| `py work/tmp/hook_test/harness.py build` | rc=0（两个 dll：新壳 + 基线壳） |
| `py work/tmp/hook_test/harness.py ours-missing` | 情形 1 通过 |
| `py work/tmp/hook_test/harness.py fallback-missing` | 情形 2 通过（参考产物 == 回退产物） |
| `py work/tmp/hook_test/harness.py ref` | 情形 3 通过（`impl=reference` 行 + manifest 分母） |
| `py work/tmp/hook_test/harness.py refcmp` | 判据 3 通过（逐字节相同） |
| `py work/tmp/hook_test/harness.py ours-ok` | 情形 4 通过（out_len=728、无 error、与基线一致） |
| `py work/tmp/hook_test/harness.py timeout-ours` / `timeout-fallback` | 超时分支两模式通过 |
| `py work/tmp/hook_test/harness.py big-ours-ok` | 反证：同一 64 MB blob、默认预算 → ours 成功 |
| `D:\bin\w64devkit\bin\gcc.exe -O2 -o work/tmp/veh_probe.exe work/tmp/veh_probe.c && ./work/tmp/veh_probe.exe [pass]` | VEH 恢复原语与穿透性 |

关键输出（逐条见 `work/tmp/hook_test/evidence/*.txt`）：

- **情形 1**（`ours` + blob 缺失）：`Result [0]=0 [1]=0 [2]=0 [3]=0x13 [4]=…`（`out[3]≠0`，形状合法）；
  `hook.log` = `error stage=ours kind=InternalError` + `impl=ours_failed mode=ours result=fail budget_ms=5000 guard=veh`；
  manifest 新行 `… - 0 - 0x0 0x0 0x0 0x13 0x… ours_failed`。
- **情形 2**（`ours_with_fallback` + blob 缺失）：返回参考产物（`out[0]=12019`、`r2=0x212F`、sha256 与同进程的 reference 调用**相同**）；
  manifest `impl=fallback`；`hook.log` 有 `error`（同上）与 `impl=fallback mode=ours_with_fallback result=reference`。
- **情形 3**（`reference`）：manifest `impl=reference`；`hook.log` `impl=reference mode=reference result=reference`（分母有了）。
- **情形 4**（`ours` + 真 `build/blob_empty.bin`）：`out[0]=0x2D8(728)`、`out[2]=0x200`、`out[3]=0`、无 `error` 行；
  与改动前的壳（HEAD 源码 + 仅把路径宏 `#ifndef` 化的机械副本）产物字节相同、`out[0]/[2]/[3]` 相同。
- **判据 3**：同一进程、同一 `compile_real.dll` 实例，新旧两份壳对 `simplexc/corpus/src/058.si` 的参考产物
  sha256 均为 `0e08fe004f7464f2914cf713bc0f80176f1bb7f401e1daa22dece29b8cfbe422`，逐字节相同。
  ⚠️ **参考产物的 sha256 只在同一进程内可比**：它烘焙了 `compile_real.dll` 镜像里的绝对地址。
  实测同一个 `compile_real.dll` 基址（0x7FF9DA050000）跨 4 个进程稳定、产物也稳定，但 14:24:03 那一次跑出过
  不同的 sha（`8dd3ff08…`，未复现、原因未查清）—— 所以逐字节比较一律放在同进程里做（`refcmp` 就是这么写的）。
- **VEH 原语探针**（`work/tmp/veh_probe.c`，与壳里同一套 `RtlCaptureContext` + 搬上下文 + `CONTINUE_EXECUTION`）：
  守卫窗口内踩 AV → 回到恢复点、`printf`/`malloc`/`strlen` 照常工作、进程 rc=0；
  窗口外的 AV → 进程被系统打死（rc=139）⇒ 作用域确实是"只包自己那一段"。

## 未验证 / 存疑

1. **VEH 触发路径未实测**（要求里允许如实报）。构造触发点只能在 ours 段里人为踩一次异常
   —— 那就是给产品代码开后门，代价大于收益。已做的替代验证：① 上面那个原语探针证明了恢复机制本身成立
   （含窗口外穿透）；② 壳里 `guard=veh` 键证明 `AddVectoredExceptionHandler` 返回非 NULL 且只装一次。
   **未验证的正是"ours 段真的踩异常时"这一跳**（含熔断后的行为）。游戏内验证也做不到（spike 只读文件，踩不出异常）。
2. **熔断后的行为未实测**：`g_breaker` 置位后按模式失败/回退这条路径要等上面那条跳通才会跑到。
3. 恢复 = 从 VEH 里跳过 `KiUserExceptionDispatcher` 的若干帧（等同 longjmp 出异常派发）。
   实测在独立进程里正常，但**没有在游戏进程里验证过**；第一次在游戏内触发时应当盯着有没有后续怪象
   （若异常派发持有 loader lock，理论上可能留下死锁 —— 这是"宁可熔断"之外的残余风险）。
4. 壳是单线程假设（`g_seq` / `g_ours_prev` / `g_resume` 都是全局）；加了线程号校验防止跨线程误恢复，
   但没有加锁、也没打算加（不在本次范围内）。
5. 跨进程参考产物 sha 那次差异（见上）未查清。
6. 游戏进程里 VEH 是**进程级**的：若将来有人 `FreeLibrary` 这个 dll，必须先摘掉处理器（当前部署方式不会卸载）。

### 游戏内待验清单（要 `game.lock`，由主程做；本 agent 不部署）

1. `py tools/deploy_compile_hook.py build-deploy`（游戏不在跑）→ 以 `compiler=reference` 启动游戏 → 热切：
   - `ours` + 把 `build/blob_empty.bin` 改名（模拟失败）：游戏应走**编译失败分支**（电路/地图报错），
     不是 index error、不是崩溃；`hook.log` 有 `error stage=ours kind=InternalError`、`impl=ours_failed`。
   - `ours_with_fallback` + 同样改名：游戏应正常跑参考产物；`impl=fallback`。
   - `ours` + 正常 blob：`impl=ours`，游戏跑我们的 blob（这条 M1 已验证过一次，回归即可）。
2. 每次编译（含 `reference`）都要有一条 `impl` 行，`manifest.tsv` 的 `impl` 列每行都有值 —— 分母是否齐。
3. VEH 装了之后游戏本身有无变化（帧率、异常行为）：VEH 是 first-chance、进程级，值得盯一轮。
4. 超时：`opt.ini` 加 `ours_timeout_ms = 1` 或启动游戏前设 `TC_OURS_TIMEOUT_MS=1`，看是否 `stage=ours_timeout` 并回退
   —— 注意 spike 太快，**很可能测不出来**（要真编译器接进来才有意义）。
5. 我们的失败 Result 交给游戏后，游戏自己打印的报错文本（应当能看到 `ours: InternalError`）。

## 交接给主程

- 已重建 `build/compile_hook.dll`（14:27，md5 `7ff5a8a1d3e15aaaa02d1027bcf70cfc`）；**没有部署**（游戏目录里的
  `compile.dll` 仍是 12:07 那份）。另一个 agent 正在用 `simplexc/tools/build.py` 复现同一个 dll（其副本
  `work/tmp/flagcheck/hook_by_buildpy2.dll` 与本文件 md5 相同）—— 我若再改源码会破坏它的比对，故到此封版。
- 没碰 `work/hook/`、`work/opt/`（改动前后 mtime 都是 12:24）、没碰 `docs/specs/`、没 `git add/commit`。
- 建议折进 `docs/facts.md` 的候选：① `impl` 词表与 per-call 行格式（含 `budget_ms`/`guard`）；
  ② 参考产物 sha256 只在同进程内可比（烘焙了 `compile_real.dll` 的绝对地址）；
  ③ VEH 恢复原语（`RtlCaptureContext` + 搬 `ContextRecord` + `CONTINUE_EXECUTION`）在 w64devkit gcc 下可用，
  窗口外异常穿透（探针在 `work/tmp/veh_probe.c`）。
- 待办（不属本次）：`tools/hook_log.py` 的 `impl` 键表补 `mode`（freq 层重写时一起做）；
  freq 层要适配"参考模式也有 impl 行"（bench_rewrite 数行数的阈值逻辑要重新看一遍）。
