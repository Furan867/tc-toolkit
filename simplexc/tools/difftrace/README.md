# difftrace —— Unicorn 上的访存序列差分

判据是**内存状态**，不是执行轨迹：记录所有 store，折叠成「每个 tick 结束时的内存」，两边比这个。
所以 DCE 掉的死代码、写几次、写的顺序都不影响结论——只要状态一样。

设计：`docs/specs/2026-10-25-difftrace-design.md`。

## 为什么不用 `host/host.py`

它把机器码跑在**真进程**里（VirtualAlloc 假内存 + detour 系统调用），判据靠每个 tick 对整块游戏内存
做全量扫描与 sha256。成本与内存规模成正比，与「这一 tick 改了多少」无关。

| | 每 tick | rv32im 100 tick |
| --- | --- | --- |
| `host.py` | ~200 ms（sha256 148 + 双遍全扫 54） | ~20 s |
| difftrace | 几十 µs（只记被写的字节） | **0.42 s** |

`host.py` 保留作 golden：difftrace 的结论与它一致，才算 difftrace 可信。

那 0.42 s 里：进程创建 + 加载 17 MB DLL 6 ms · **VM 镜像装配 390 ms（93%）** · 真跑 100 tick 29 ms。
装配是「291 MB guest 映射 + 142 MB `RESET_DATA` 填充 + 8.4 M 条启动指令」，已经是页错误的物理成本。

## 依赖与编译

```bash
python simplexc/tools/difftrace/fetch_deps.py     # 备 Unicorn 2.1.4 + Capstone 5 的 C SDK（幂等）
python simplexc/tools/build.py difftrace          # → build/difftrace.exe（顺带拷两个 dll）
python simplexc/tools/build.py difftrace_selftest # 编并跑 16 条边界用例
```

SDK 不进 git，落在 `simplexc/.deps/difftrace-deps/`。`build.py` 的默认目标 `all` **不含** difftrace，
所以没装 SDK 的机器上 `build.py` 照样成功。

## 用法

```text
difftrace run   --src <si> --bin <blob> [--entry <hex>] [--target N]
                [--seed none|seq|xorshift] [--save <seq>] [--json]
difftrace diff  --state a.seq b.seq [--src <si>] [--tick-mark <hex>] [--json]
difftrace diff  --trace a.seq b.seq [--json]        # 同一编译器前后两版（比轨迹）
difftrace where --seq <seq> --addr <hex> [--src <si> --bin <blob>] [--json]
```

- **`entry` 可省**：先找 `<blob>.entry`（我们产物），再从 blob 目录向上找 `manifest.tsv` /
  `manifest-capture.tsv` 按 `out_sha256` 匹配（列名 `entry` 或 `r2`）。
- **`--tick-mark` 不传就不做逐 tick**（只比终态）。传了但该地址在序列里一次都没出现 → **退出码 2**，
  不会静默退化成终态比。
- **DLL 定位**：`--dll-dir` > 环境变量 `DIFFTRACE_DLL_DIR` > 内置的游戏路径。
- **退出码**：`run` 0=成功/2=错误；`diff` 0=equal/1=不等/2=错误；`where` 0=命中/1=未命中/2=错误。
- `--json` 在三个子命令里都可用，错误时也输出 `{"ok":false,...}`。

## 判据口径

- **主判据**：终态等价（整段折叠、按字节、缺失当 0）。不依赖任何界标。
- **逐 tick**：可选，依赖 `--tick-mark`。**没有真正的 tick 界标**：tick 边界是 `cycle += 1`，
  而 `cycle` 是模块级 `var`（VM 全局槽，不是内存写），store 序列里看不见。rv32im 上
  `#SIMULATION_STATE + 2908`（`10171`，体内最后一条写）切出来的段末才对齐边界；
  `+2567` 是体内第 9/27 条写，段末停在 tick 中途。
- **排除项**：`spec.regions()` 之外的写（栈区）、被改写成 magic 的导入槽。
- **`size` 只接受 1/2/4/8**：其它值（含 16）抛 `std::runtime_error`。`Store::value` 只有 64 位，
  16 字节写的高 8 字节本来就不存在，展开等于造假数据。

## 并发

多进程即可（每进程一个 `uc_engine`，无共享状态）。16 路实测 176 份序列 md5 全同。

| 并发 | 总墙钟（同产物） | 加速比 | 效率 | 峰值内存 |
| --- | --- | --- | --- | --- |
| 1 | 419 ms | — | — | 0.44 GB |
| 4 | 461 ms | 3.63× | 91% | 1.75 GB |
| 8 | 554 ms | 6.05× | 76% | 3.50 GB |
| 12 | 716 ms | 7.02× | 59% | 5.25 GB |
| 16 | 898 ms | 7.47× | 47% | 7.00 GB |

**建议 12 路**（16 路的 94% 吞吐，省 1.75 GB）。

## 已知口径与坑

- 启动期 128 MB `rep movsb` 已特判成宿主 `memmove`（一条 `rep movsb` 在 Unicorn 里会触发 64 次 code hook，
  1.34 亿条指令就是这么来的）。**特判只在启动期开**：`uc_mem_map_ptr` 的区里写仍会触发 mem write hook，
  而宿主侧 `memmove` 不会——抓迹窗口里开它会让 store 从序列里静默消失。
- `--seed xorshift` 会让 rv32im 在 `cyc=13` 报「非法访存」：`#RESET_DATA_*` 是游戏里的真实内存镜像，
  宿主没有它，随机字节填进去 CPU 就跳到垃圾上。`none`/`seq` 都能跑满 target。
- 停机判定是 `sim_running` 被写 0 的那一刻（`settings + StateIndex[sim_running]*8`，从 spec 算）。
  早期按「每 1000 条指令采样」判定，两边切点深度差 8 倍，凭空造出 688 字节假差异。
