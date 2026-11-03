<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**热点分析工具** —— 组件级时间归因（偏移 → 行 → 元件）。
设计 `docs/specs/2026-11-01-hotspot-profiler-design.md`（v8；2026-11-03 按实测补开工契约）。

- **W1–W4 已落地**，当前只启用了 suspend 后端；etw 后端未实现
- W1：DIBuilder + DebugLoc 行号 + `--debug-info` / `--elements-out` / `--offsets-out` + G1
- W2：聚合层 + suspend 后端 + 常驻 REPL
- W3 等 W1 的 `el.tsv` 与 `<blob>.offsets`；W4 等 W2 的 run 目录 + W3 的 sidecar

## 开工前实测改掉的 spec 前提

- 元件表：`mask_source` 把注释抹成空格，`raw_lines` **不含注释** → 改读 `--src` 原文
- `.debug_line` 地址是对象 `.text` 节内偏移（装配器丢弃 `.debug*`）→ 必须靠 `<blob>.offsets`
- blob 头 = `文件总长 | (1<<62)`，长度判据先掩 bit62

## 下一步（W1/W2 验收判据）

- W1：G1 绿（同源开/关 `.text` objdump diff 空）+ `el.tsv` 行数/行区间抽查 + offsets 不变式
- W2：`-Wall -Werror` 零警告 + 事件注入单测（合成 IP/TID → 聚合/分桶）+ REPL 无游戏错误路径
- 真样本阶段（待环境）：etw 要管理员终端；采样要游戏已启动并载入电路

## 其它线

- **simplexc**（自研编译器）：暂停，等热点工具做完；进度见
  `docs/notes/2026-11-02-simplexc-progress.md`
- 判据入口 `python simplexc/tools/check.py`（6 步 + W1 新增 G1）
