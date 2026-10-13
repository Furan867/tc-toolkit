<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts.md。 -->
# 状态

在追：

## Simplex → LLVM 编译器（主线，2026-10-13 起）

目标：把游戏目录的 `compile.dll` 换成自研实现 —— Simplex → LLVM IR → 优化 → 装配成游戏可 JIT 的 blob，
提升游戏内电路仿真主频。正确性判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**。
主频测量用 `watch_freq.py`，benchmark 用 RV32IM 电路（很远，暂不做）。

### 刚完成（2026-10-13）

转发层补齐记录：`work/hook/manifest.tsv`，一次 `compile()` 一行 —— `flags` + Result 全 5 字段
（`r0/r1` 长度与指针、`r2` 改写后的最终锚点、`r3/r4` 首次落盘）+ src 与产物的 sha256、长度。
清空时机与 `.si`/`.bin` 一致；记录失败静默跳过。

离线验证通过（不动游戏目录）：flags 如实入库，sha256 与 Python `hashlib` 对拍一致，
`r3/r4` 实测被写成 0（调用方缓冲预填 0xCC 哨兵）。详见 `notes/2026-10-13-转发层记录manifest.md`。

### 下一步

1. **采一次游戏内真值**：`py tools/deploy_compile_hook.py --build --deploy` → 启动游戏 → 编译一次电路 →
   看 manifest 里 `flags` 的真值与 `r3/r4` 是否仍为 0（离线那次是我们自己传的 flags，游戏传什么还不知道）。
2. 回到主线：Simplex 前端，先做「同一份 `.si` → 逐 tick 等价」的最小通路。

### 验收标准

- 记录：manifest 行能与 `optimize.log` 的 sha256 互相印证；记录失败绝不影响编译返回值。
- 主线（远期）：自研产物的逐 tick 状态等价，且主频不低于参考产物。
