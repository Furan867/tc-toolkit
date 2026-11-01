<!-- 整体重写，禁止追加。≤80 行。
     一次性结论 → notes/；被复用过的结论 → facts/。具体事实不在这里复制。 -->
# 状态

在追：**开源发布准备**。目标是让仓库从「本机开发工作区」变成「别人 clone 下来能编译、能跑判据」。
M5 功能线暂停（见末节），不再往新功能推进。

- 发布范围：**只发 `tc-toolkit/`**。根仓 `tc/` 只有治理文档，不对外。
- 许可证 MIT（`LICENSE`）；第三方声明在 `THIRD_PARTY_NOTICES.md`（LLVM / Capstone / Unicorn，均不随仓库分发）。
- 接口权威 = `docs/specs/2026-10-15-simplexc-interfaces.md`（冻结版 1.4）；主设计 = `…-simplex-llvm-compiler-design.md`。

## 本轮：路径可移植化（2026-10-26）

别人 clone 后不该依赖本机目录结构。已落地：

| 原状 | 现状 |
| --- | --- |
| 20+ 处脚本写死 `F:\opt\…\Turing Complete\…` | `tools/tc_common.py`：`game_dir()` / `game_exe()` / `game_dll()` / `game_build_id()` / `game_fingerprint()` / `llvm_root()` / `tool()` |
| 工具链写死 w64devkit 的绝对路径（另有一个环境变量覆盖） | `g++` / `gcc` / `cmake` / `ninja` / `objdump` / `nm` 一律从 **PATH** 找（需要 GCC 14+） |
| LLVM 资产在 `build/llvm` | `simplexc/.deps/llvm`（`fetch_llvm.py` 默认落点，`TC_LLVM_DIR` 可覆盖，旧路径仍认） |
| 转发层三个硬编码绝对路径宏 | 编译期 `-DTC_TOOLKIT_ROOT`（缺省 `"."`）+ 运行时 `opt.ini` 的 `[opt] toolkit_root` 覆盖 |
| `simplexc/probe/results/` 入库 | 已取消跟踪（M1.5 探针已停，一次性数据） |

判据（2026-10-26 实测）：构建 **rc=0、0 warning**（`-O2 -Wall -Werror`）；`check.py` **6 PASS**。

⚠️ `check.py` 的 `corpus` / `host_selftest` / `lower_equiv` / `hook_offline` 都要求游戏目录里有
`compile_real.dll`（原版编译器）。**未部署转发层时这几步必然失败，不是回归**。要跑全量判据：
`python tools/deploy_compile_hook.py` 部署，跑完 `--restore` 还原。

## M5（暂停）

目标没变：把 `compile.dll` 换成自研实现（Simplex → LLVM IR → 游戏可 JIT 的 blob），判据是同一份
`.si` 逐 tick 状态等价。最近一次全量：98 份 **95 equal / 3 diff / 0 fail**（`batch.py --with-prefix --jobs 12 --ticks 100`，38.6 s）。

3 份 diff 目前都是**差异存在、原因待查、差分测试暂不可复现** —— 三份的机制并不相同（084 是同一个 64 位链、种子来源不同；
096/098 是种子被降成 1 字节栈槽），不归因于任何一方的 bug：

- 084「Overture 1 寄存器序章」→ `docs/bugs/2026-10-26-084-ref-uninit-stack-seed.md`
- 096「Overture 2 ALU 序章」/ 098「Overture 3 立即数序章」→ `docs/bugs/2026-10-25-random-stream-offset.md`

（中文名是本地译名，定关依据在各自 bug 文档里。）

⚠️ **分母口径**：原因查清前这 3 份**不从分母剔除**、也不定性为谁的 bug，对外只说「95/98 逐 tick 全等，3 份待查」。

## 未解

- **031-032「逻辑试炼」的「游戏内崩溃」**：原结论（产物缺 20 个接口函数）已被推翻 —— 参考同样丢弃未调用的
  def（删掉那 31 个 def，参考产物只差 4 字节）。缺现场重新取证，见 `docs/bugs/2026-10-26-031-unreproducible-crash.md`。
- **游戏内「连续编译两次拿到的 `.si` 不同」未查**（031 vs 032：`get_gate_score` 14276 vs 722、组件 51 vs 128）。
  需要游戏内实验。
- `thread_exit.1` 符号碰撞（`docs/bugs/2026-10-25-thread-exit-collision.md`）。
- `lower_equiv` 在 ref 侧加载失败时**仍判 PASS**（判据退化，见 `docs/pitfalls.md`）。
- runtime 整块链接：不用 `array()` 的语料也带 `tc_array_alloc` 的 200 B + 一个 `__imp_VirtualAlloc` 槽。

## 环境

- `build/` 是共享的：多写者按目录切分，同一时刻只让一个写者跑判据入口。
- `python simplexc/tools/build.py` 默认目标**不含** `simplexc_cli` / `difftrace`；改了编译器要另跑。
- 判据入口：`check.py`（6 步约 40 s，需部署状态）· `m5/batch.py`（`--with-prefix` 跑 stdlib 全量）· `difftrace`（run / diff / where）。
- clangd：`simplexc/**/compile_flags.txt` 只给 LLVM 头与 `-std`，不写 `--gcc-toolchain`（clangd 自己从 PATH 推）。
