# 逐 tick 快照加「每个映射段的内容哈希」（让 `#DATA_*` 的 128 MB RAM 镜像进比对）

**一句话**：`host.py` 的快照多一个字段 `regions`（每个映射段的**绝对内容** sha256），
`selftest.compare` 把它纳入比较；rv32im 的 ours vs 参考产物 10 tick 差异仍 **0 条**，
段哈希覆盖 **6 段 / 278.5 MB**（含两个 128 MB `#DATA_*` 镜像）。判据只加强，没有放宽任何既有条款。

## 1. 字段格式

快照 JSONL 的每一行（`host.py --out …`）新增一个键，位置在 `pages` 之后：

```json
{"tick": 1, "reason": "ok", "stabilized": true, "alive": true, "sim_cycle": 1,
 "pages": {"0x188b5190000": "a2c962b168b6458f", …},
 "regions": {"0x188b5190000": "1302fb703b6a500b…（64 位小写十六进制）",
             "0x188b7cd0000": "2544179080603279…", …},
 "extern": [], "pacing": {…}}
```

- 键 = 段基址（与 `pages` 的键同一个写法：`hex(lo)` 小写）；值 = 整段内容的 **sha256 全 64 位**。
- `--region-hash-every N`（默认 **1** = 每个 tick 都算）跳过的那一 tick 里 `regions` 是 **`null`**；
  `N = 0` 被拒（rc=1）—— 关掉段哈希等于放宽判据。
- 哈希只当**内容校验和**用，不是安全用途。选 sha256 而不是 md5 的原因：与 `pages`
  （`sha256(page)[:16]`）同一口径、一处只留一种哈希，且实测更快（见 §5）；pi-lens 也会把
  `hashlib.md5` 报成弱哈希 blocker。
- 分块读（`HASH_CHUNK = 1 MiB`，`ctypes.string_at` 逐块喂 `hashlib`）：一次 `string_at(lo, size)`
  会把 128 MB 白拷到 Python 堆上。实测「拷 + 哈希」0.345s/278 MB（md5）→ 现在 sha256 0.131s。

## 2. 段范围怎么定 —— 用**源码算的那一套**，不是 `self.regions`

`Host._map_game_memory` 现在多存一份 `self.spec_regions = merge_spans(self.spec_regions())`，
段哈希走的是它。两者都要，因为它们的几何**不一样**：

| 集合 | 来源 | rv32im 的几何 | 能不能拿来比 |
| --- | --- | --- | --- |
| `self.regions`（既有，`pages` 用的） | `spec.regions()` **＋ 产物 movabs 里的游戏地址**（每个再给 1 MB） | 6 段、**278.6 MB（参考）/ 278.7 MB（ours）** | ✗ 随产物变 |
| `self.spec_regions`（新，段哈希用的） | 只由 `.si` 算：地址表（scalars/arrays）＋ `memory_copy`/`memory_clear` 的实参 | 6 段、**278.5 MB（两个产物逐字节相同）** | ✓ |

这是**实测撞出来的**，不是设计洁癖：第一版按 `self.regions` 哈希，ours vs 参考产物 10 tick
里 tick 1..8 全报 `段 0x188b5190000 哈希不同` —— 同一份内容、不同的字节数（多 0.1 MB 的
`game_like` 默认段），两个产物永远比不出「相等」。改口径后差异 0 条。

rv32im 的 6 段（`--src simplexc/corpus/rv32im/rv32im.si`）：

| 段基址 | 大小 | 里面是什么 | tick 1 内容 |
| --- | --- | --- | --- |
| `0x188B5190000` | 1.250 MB | `commands`/`settings`/`error_buffer`/`input_replay[]`/`output_history_pins` | 有内容 |
| `0x188B7CD0000` | 1.000 MB | `#SIMULATION_STATE` | 有内容 |
| `0x188B86A0000` | 1.000 MB | `ui_buffer` | 全零 |
| `0x188B88A0000` | 1.562 MB | 键盘/按钮/`ctl_input_replay_reset[]` | 有内容 |
| `0x189111A0000` | 1.438 MB | 小的 `#DATA_*`/`#SETTINGS_*` | 全零 |
| `0x18918140000` | 272.250 MB | `#DATA_7069020504170261741` `#DATA_7585293127307814633`（两个 128 MB RAM 镜像）＋ 两个 8 MB `#SETTINGS_*` | **全零**（见 §6） |

## 3. 为什么这个哈希不包含产物/JIT 区与 DLL 区

`spec_regions` 的每一项都来自 `.si` 文本里的**游戏内存地址**（地址表 + `memory_copy`/
`memory_clear` 实参），产物的机器码在宿主的哪一块跟它无关：

- 产物的机器码由 `mem_alloc(0, …)` 拿一段 OS 自选地址（rv32im 实测 `0x1B6BC400000` 之类），
  与游戏地址带（`0x188B…`、`0x1891…`，1.6 TB 量级）相距极远，也不在任何一段的跨度里；
- `compile_real.dll` 加载在自己的基址（`0x7FF…`），同样在带外；
- 产物自己的 `VirtualAlloc` 走的是 `ARENA_BASE = 0x100000000` 的确定性 bump 分配器，也不在这 6 段里
  （rv32im 的 extern 序列是空的，两个产物都没走到它）；
- 反过来，产物 movabs 里那些「像游戏地址」的 movabs（`game_like`）**只进 `self.regions`
  （`pages` 的覆盖面），不进段哈希** —— 上表第 1 行的原因。

## 4. 负控：某段差一个字节 → 报出具体段

`work/tmp/mirror/negctl_poke.py`（临时脚本，不属于工具链）。做法：两个**独立进程**各跑一遍同一个
产物（ours，`entry 0xE1F0`），B 侧在 `tick(1)` 之后、取快照之前把目标地址那 1 字节取反，
再用 `Host.tick` 里**同一套** `_settled_pages()` / `_region_hashes()` / `_snapshot()` 造快照 ——
差异只可能来自「内存内容真的不一样」，不是拼出来的快照。

```text
$ python work/tmp/mirror/negctl_poke.py --bin work/tmp/tern/rv32im.bin --entry 0xE1F0
[negctl] 目标段 0x18918140000（272.25 MB，含 `#DATA_*`），戳 0x18918240040（页 0x18918240000）
[negctl] b 侧完成：[negctl] 戳 0x18918240040：0x00 → 0xFF
[negctl] A 段 0x18918140000 哈希 cbcb79a53e082209…dfa59780
[negctl] B 段 0x18918140000 哈希 658f8ed9ce9d8db0…cd0da0cc
[negctl] 原样比：差异 2 条
   tick 1: 段 0x18918140000 哈希不同 A=cbcb79a5… / B=658f8ed9…
   tick 1: 页不同 A 独有 {} / B 独有 {'0x18918240000': '7771e509f895d0ca'}
[negctl] 把页 0x18918240000 从两边 pages 里摘掉（页白名单那条路）再比：差异 1 条
   tick 1: 段 0x18918140000 哈希不同 A=cbcb79a5… / B=658f8ed9…
[negctl] PASS：原样比报出段 0x18918140000；摘掉被戳的页后段哈希仍报 是
```

两件事同时被证：① 判据**不是空跑**（1 字节差就报，且指到具体段）；② 摘掉被戳的页之后
（`equiv_build`/`probe_006` 里 `PAGE_WHITELIST` 走的那条路）页判据**看不见**了，段哈希照样报 ——
这是新字段相对 `pages` 的独有覆盖面（白名单放行的页，段哈希仍然盯着）。

## 5. 性能

同一台机器、同一对文件（`--src simplexc/corpus/rv32im/rv32im.si`，10 tick）：

| | 改前 | 改后（默认每 tick 哈希） | 改后 `--region-hash-every 2` |
| --- | --- | --- | --- |
| 参考产物一次 10 tick | 1.25 s | **2.76 s** | 2.03 s |
| ours 一次 10 tick | 1.25 s | **2.76 s** | — |
| `selftest.py`（两次宿主、各 10 tick） | — | 5.59 s | — |

每 tick +0.15 s ≈ sha256 过 278.5 MB（278 MB 分块实测 0.131 s）＋ 分块拷贝的 Python 开销。
远在「10 tick 10 分钟」的线内，**所以默认保持每 tick 都算**，`--region-hash-every` 只是给更慢的
靶子留的旋钮（`N>1` 时只在 `tick % N == 0` 比，跳过态两边都是 `null`；一边有一边没有算差异）。

## 6. 还剩什么没覆盖

1. **宿主里那 272.25 MB 的 RAM 镜像全是零**：段 `0x18918140000` 的哈希 == 同样长度的全零缓冲的
   sha256（`0x188B86A0000`、`0x189111A0000` 也是全零）。原因是 `reset_sim` 的
   `rep movs` 从 `#RESET_DATA_*` 拷到 `#DATA_*`，而宿主把两者都按零页摆出来 —— 拷贝等于没拷。
   所以这条判据现在钉住的是「两边都**没往 RAM 写东西/写出的内容一样**」，
   **不是**「两边都复现了游戏里那份 RAM 镜像」。要真演练这一段，得让宿主把 `#RESET_DATA_*`
   用确定性图案 seed 起来（那是宿主行为改动，本次没做 —— 交给主程定）。
   顺带：`pages` 的覆盖面在「两边基线相同」时逻辑上蕴含段哈希，段哈希补的是
   **基线不进快照**这个洞（基线不同、页白名单放行时 `pages` 什么都说不出来）。
2. **跨进程的时序**：两次宿主运行是两个独立进程，快照取在「稳定判定」那一刻（连续两次采样一致），
   但稳定判定与哈希之间的微秒级窗口、以及两个进程各自的采样时刻，都不在判据里。
3. **`regions` 的覆盖面 = 源码算出的 6 段**；产物 movabs 补出来的 `game_like` 段里的字节
   （rv32im 上多出约 0.1 MB）只在 `pages` 里，不在段哈希里。
4. **带 `--dump-pages` 时**快照里的 `pages` 是落盘那一路**又采一次**的结果，而 `regions` 是稳定判定
   那一刻的结果（既有行为；本次没动 `pages` 的采样）。
5. **游戏内**：真 Sleep/时钟、游戏设的 `target_cycle`、游戏往 `#RESET_DATA` 里放的真数据，一律照旧没验。

## 7. 判据入口与其它消费者

- 标准入口 `selftest.py` 多了一条防伪检查 `region_hash_present`（字段缺了就「静默全等」，
  所以要求「有哈希的 tick 数 == 预期数」），报告里多 `segments`（段基址清单）与 `region_hashed_ticks`；
  打印行变成 `段哈希 6 段/10 tick=有`。
- `equiv_build.py` 也走 `selftest.compare`（同一个比较器，两边同一份 `--src` → 几何一致），
  所以它顺带也被加强；`probe_006.py` 有自己的比较器（本次不在允许改动范围内，未动）。
- 临时物：`work/tmp/mirror/{compare_runs.py,negctl_poke.py,*.jsonl}`（ours vs 参考产物那条差分
  仓里没有现成入口，所以用临时脚本 import `selftest.compare`，判据口径不另写一份）。
