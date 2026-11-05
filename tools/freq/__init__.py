"""主频测量层的公共包（设计：`docs/specs/2026-10-15-freq-measurement-design.md` §3）。

分层（上层只依赖下层）：

    probe     偏移常量 + Snapshot + wait_for_game + 快照读取（底层，只读内存）
    variants  Variant(compiler × rewrite) 与 opt.ini 读写（合法值集合的唯一一处）
    products  产物编号 / 指纹 / 归属（只认 hook.log 的 cfg、opt、impl 三行）
    sampler   采样循环 + Segmenter + 事件 Sink（口径唯一的一处：median + 丢首个 +
              相位对齐 + 停机/低压判据）
    report    人读表（区间表 / 对照表）+ JSON 报告（Report.schema = 1）
    driver    驱动游戏的薄封装（simctl + 前置断言）
    cli_*     薄 CLI：cli_read / cli_watch / cli_bench（`tools/*.py` 的外壳转调它们）

`libs` 的 `winmem` / `term` / `hook_log` 与 `services` 的 `simctl` 对外接口保持不变，本包只在其上分层。

包内模块一律 `from libs import winmem` 这样的包名导入，因此**两个入口都能用**：

    py tools/watch_freq.py           # 人用的薄壳
    python -m tools.freq.cli_watch   # 等价（在 tc-toolkit/ 下跑）
"""
