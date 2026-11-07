# hotspot/consumer

采样会话（W2）：suspend 后端产出 `samples.tsv v1`，加常驻 REPL 与 `save`。
设计与键表见 `docs/specs/2026-11-01-hotspot-profiler-design.md` §4.0–4.6、§10。

## 构建与自测

```bash
cmake -S hotspot/consumer -B hotspot/consumer/build -G Ninja   # 本机没有 MSVC，默认生成器 NMake 不可用
cmake --build hotspot/consumer/build
ctest --test-dir hotspot/consumer/build --output-on-failure
# 或：cmake --preset mingw -S hotspot/consumer && cmake --build --preset mingw
```

`-Wall -Werror` 挂在 `hotspot_flags` 上，全部自有源码（src/ 与 tests/）都吃这一套。

## 用法

```text
hotspot-consumer [--config hotspot/config.ini]
> open work/hook/all/hook_out_001.bin [--pid N] [--interval-ms 1]
> window 10 save          # 采 10 s 并落 run 目录（samples.tsv + run.ini）
> status / locate / save [dir] / quit
```

`open` 之后定位/枚举只做一次，多窗口复用；`window` 开头轻量校验线程集合。
`save` 建 `runs/<ts>_<产物名>_t<秒>/`，写 `samples.tsv` + `run.ini`，读两次 freq
（`freq.command`，超时 `freq.timeout_sec`）填入 `freq.start_hz/end_hz`。
无游戏、无 PID、产物不存在都只报错，REPL 不退出（`quit`/EOF 才退）。

后端：`suspend`（免管理员；SuspendThread + GetThreadContext，粗采
`suspend_probe_rounds` 轮锁定仿真线程，未命中则退回全线程并告警）和自写 `ETW`
（`StartTrace` / `ProcessTrace` 实时消费，不落 ETL；仅 ETW 路径需要提权）。
两路的真机结果与已知差异见 `docs/status.md`。

## 已验证 / 后续真机覆盖

已验证（本机，2026-11-05）：

- 构建 0 warning（`-Wall -Werror`）；`ctest` 2/2 通过（聚合/定位/配置/落盘/会话用例）；
- 事件注入：多副本合并、bucket 0/1/2 判定、blob_offset 换算、samples.tsv 头与行序；
- 特征窗偏移（0x400 / 过短退 len/3）、候选基址换算、产物头 `(head & ~(1<<62)) == 文件长`；
- 无游戏/无 PID/坏产物的 REPL 错误路径与 `game.lock` 的 RAII 清理。

后续真机结果（ETW / suspend 双后端、暂停上界、端到端样本量）见 `docs/status.md`。
