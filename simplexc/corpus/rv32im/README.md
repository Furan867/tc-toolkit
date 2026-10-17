# RV32IM 电路：M3 的靶子（游戏内捕获快照）

设计文件 `docs/specs/2026-10-15-simplex-llvm-compiler-design.md` 的里程碑表写明
**M3 = 生成代码子集编译 + 逐 tick 状态等价（RV32IM 电路）**，§主频测量也说 benchmark 固定 RV32IM。
所以这份捕获是 M3 的**唯一判据来源**。

## 为什么放在这里而不是 `work/hook/`

`work/hook/` 是转发层的滚动捕获目录：**每一次游戏内编译都会覆盖同名文件**（实测 17:12 那批被
17:21 那批整批替换）。M3 的判据不能依赖一个会被下一次编译冲掉的路径，也不能依赖活着的游戏进程
（产物里的游戏内存地址按进程实例变，见 `docs/facts.md`）。所以把源码与参考产物各存一份到这里。

## 文件

| 文件 | 说明 |
| --- | --- |
| `rv32im.si` | 生成源码，**10369 行 / 360772 B**，sha256 `dba14f86b2a947b27cb2d7b8034090205d4debc367b9c7603297aa8af88d220e` |
| `rv32im.bin` | 同一份源码的**参考产物**，**219088 B**，sha256 `baffee9f8fbe3c2c32af27fcaff5d6bf7488983d328936d9171894851260ca01` |
| `manifest-capture.tsv` | 捕获时整份 `work/hook/manifest.tsv`（含各 seq 的 entry / sha / 时间，用于回溯来源） |

来源：2026-10-17 17:21:33 游戏内编译，`seq 007`（`seq 002…007` 六份逐字节相同，是同一份源码被连续编译）。
参考产物的 `entry = 0x215B`。载体：`impl=reference`。

## 规模（写前端前先看这组数字）

- `get_component_count()` = **532**
- `mode_refresh()` = **6252 行**（组件运算全在这里）
- 源码 = 10369 行；参考产物 219088 B ≈ 46600 条指令（与设计文件 §2.5 的 46591 条吻合）
- 组件种类频次（前几名）：`com_static_indexer` 113、`com_cc_input_buffer` 80、`com_probe_wire_bit` 48、
  `com_switch_bit` 42、`com_switch_word` 40、**`com_custom` 33**、`com_constant` 28、`com_equal` 20、
  `com_not_bit` 17、`com_mux` 17 …
- ⚠️ `com_custom` 33 个 → 这份源码含**子元件层级**；前端不能只做平铺，否则过不了。

## 为什么它是收益的靶心

设计文件 §2.5 已实测参考编译器的产物质量：`mov*` 占 65.2%（其中 reg-reg 拷贝 27.7%）、
碰 `rsp/rbp` 19.4%、置零 6327 次，典型片段反复重算地址。同一份电路做纯窥孔改写已实测
**5.4 → 8.0 MHz（+48%）** —— 说明这个靶子的主频落在可测量、且有余量的区间。

## 怎么用

宿主（离线，不需要游戏）：

```bash
python simplexc/tools/host/host.py --src simplexc/corpus/rv32im/rv32im.si \
    --bin simplexc/corpus/rv32im/rv32im.bin --entry 0x215B --ticks 10 --out <jsonl>
```

生成地址头（给手写/生成的 C 或 IR 用；**必须**从这份 `.si` 现取，不能用别的电路的头）：

```bash
python simplexc/tools/gen_addrs.py --src simplexc/corpus/rv32im/rv32im.si -o <workdir>/addrs.h
```

⚠️ 这份源码里的游戏内存地址属于**捕获它的那个进程实例**（`commands = Ptr 1686665494528`，本文件生成时）。
宿主不受影响（它按源码里的地址自己 `VirtualAlloc` 假内存）；游戏内验证必须重新捕获并现取地址。
