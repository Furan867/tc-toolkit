# simplexc 进度快照（2026-11-02）

- 状态：**暂停**，等热点分析工具先做完；随时可恢复
- 判据：同一份 `.si`，我们的产物与参考产物**逐 tick 状态等价**

## 现在

- 98 份语料：**95 逐 tick 全等、3 份差异待查**（不归因、不从分母剔除）
  最近一次全量：`batch.py --with-prefix --jobs 12 --ticks 100` → `95 equal / 3 diff / 0 fail`，39.7 s
- 3 份 diff 的机制并不相同（084 是同一条 64 位链但种子来源不同；096/098 的种子被降成 1 字节栈槽），
  细节在 `docs/bugs/2026-10-26-084-ref-uninit-stack-seed.md` 与 `2026-10-25-random-stream-offset.md`
- 判据入口 `check.py`（6 步）；单份对照见 `difftrace run / diff / where`

## 未解

- **031-032「逻辑试炼」的游戏内崩溃未复现**：原根因判断已推翻（参考同样丢弃未调用的 def），缺现场取证
  → `docs/bugs/2026-10-26-031-unreproducible-crash.md`
- **`lower_equiv` 在 ref 侧加载失败时仍判 PASS**（判据退化）→ `docs/pitfalls.md` 同名条目
- **runtime 整块链接**：不用 `array()` 的语料也带 `tc_array_alloc` 的 200 B 与一个 `__imp_VirtualAlloc` 槽

## 环境

- `simplexc/build/` 是共享产物目录：多写者按目录切分，同一时刻只有一个写者跑判据入口
- `python simplexc/tools/build.py` 默认目标**不含** `simplexc_cli` / `difftrace`；改了编译器要另跑
- `check.py` 的 `corpus` / `host_selftest` / `lower_equiv` / `hook_offline` 四步要求游戏目录里有
  `compile_real.dll`（原版编译器）—— 未部署转发层时这四步失败，不是回归
