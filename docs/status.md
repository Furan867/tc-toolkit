<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**热点分析工具** —— 组件级时间归因（偏移 → 行 → 元件）。
设计 `docs/specs/2026-11-01-hotspot-profiler-design.md`（v9：etw 自写，不用第三方库）。

## 已落地

- W1：DIBuilder/DebugLoc、`--debug-info` / `--elements-out` / `--offsets-out`；
  `--stdlib` 逐字节复现 hook 产物（007：sha `ab8fe261…` 一致）
- W2：聚合层 + suspend 后端 + 常驻 REPL；定位忽略运行时 `.bss`、窗口只采锁定线程后，
  真机 10 s 窗口 bucket0 1279/1279（100%），已归属 1115（87.2% 全样本）
- W3：sidecar join 与 G2 抽查（20/20、80/80）
- W4：四表 + `report.md` / `report.html` + G3′/G4 + baseline；7 条 analysis 单测通过
- 一键入口 `python hotspot/run.py --src … [--stdlib …]`：编译 → sidecar → 采样 → 报表；
  默认采样后停电路（`--keep-running` 保留）
- 配置：`config.toml` 移出仓库，`config.example.toml` 入库

## 当前

- **etw 自写**：spec v9 已改；只解析 SampledProfile，实时收，不落 ETL；失败 assert
- 探针 `work/tmp/etw_probe/etw_probe.exe` 已编译，等管理员终端验证
  Provider GUID / Event ID / 负载布局

## 下一步

- 跑探针，钉死 GUID/ID/布局与事件率
- 重写 `backend_etw`，接聚合层；真机 ETW vs suspend 对比
- 验收阈值由负责人现场定（spec §8/§12 已不留默认数字）

## 其它线

- **simplexc**（自研编译器）：暂停，等热点工具做完；进度见
  `docs/notes/2026-11-02-simplexc-progress.md`
- 判据入口 `python simplexc/tools/check.py`（6 步 + G1）；当前 3 步红：游戏目录缺
  `compile_real.dll`，非回归
