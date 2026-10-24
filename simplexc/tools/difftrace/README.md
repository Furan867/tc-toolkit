# difftrace —— Unicorn 上的访存序列差分

判据是**内存状态**，不是执行轨迹：记录所有 store，按 tick 折叠成「每个 tick 结束时的内存」，
两边比这个。所以 DCE 掉的死代码、写几次、写的顺序都不影响结论——只要状态一样。

设计：`docs/specs/2026-10-25-difftrace-design.md`。

## 为什么不用 `host/host.py`

它把机器码跑在**真进程**里（VirtualAlloc 假内存 + detour 系统调用），判据靠每个 tick 对整块游戏内存
做全量扫描与 sha256（rv32im 是 278 MB）。成本与内存规模成正比，与「这一 tick 改了多少」无关：

| | 每 tick | rv32im 100 tick |
| --- | --- | --- |
| `host.py` | ~200 ms（sha256 148 + 双遍全扫 54） | ~20 s |
| difftrace | 几十微秒（只记被写的字节） | **5.4 s**（其中 5 s 是一次性启动的 128 MB `rep movsb`） |

`host.py` 保留作 golden：difftrace 的结论与它一致，才算 difftrace 可信。

## 依赖

两个都是「PyPI wheel 里带 C SDK」，不进 git：

```bash
# Unicorn 2.1.4：wheel 里有 include/unicorn/*.h、lib/unicorn.dll、lib/unicorn.lib
python -m pip download unicorn --no-deps -d <work>/uniwheel
unzip -oq <work>/uniwheel/*.whl -d <sdk>/unicorn_sdk
cd <sdk>/unicorn_sdk/unicorn/lib && gendef unicorn.dll && dlltool -d unicorn.def -l libunicorn.a

# Capstone 5：装好的 site-packages 里就有
#   D:/bin/Python313/Lib/site-packages/capstone/{include,lib}
cd "D:/bin/Python313/Lib/site-packages/capstone/lib" && gendef capstone.dll && dlltool -d capstone.def -l libcapstone.a
```

`gendef`/`dlltool` 来自 w64devkit（`/d/bin/w64devkit/bin`）。

## 编译

```bash
g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic \
  -I <sdk>/unicorn_sdk/unicorn/include \
  -I "D:/bin/Python313/Lib/site-packages/capstone/include" \
  simplexc/tools/difftrace/*.cc \
  -L <sdk>/unicorn_sdk/unicorn/lib -lunicorn \
  -L "D:/bin/Python313/Lib/site-packages/capstone/lib" -lcapstone \
  -o difftrace.exe
```

运行前把 `unicorn.dll`、`capstone.dll` 放在 exe 旁。

## 用法

跑一份产物、落盘序列：

```bash
difftrace --src simplexc/corpus/rv32im/rv32im.si \
          --bin simplexc/corpus/rv32im/rv32im.bin --entry 0x215B \
          --target 100 --seed seq --save ref.seq
```

比两份序列：

```bash
difftrace --compare-state a.seq b.seq \
          --sim-cycle-addr 0x188b7cd0a07 --src simplexc/corpus/rv32im/rv32im.si
```

- **主判据**：最终状态是否等价（整段折叠、按字节、缺失当 0）。不依赖任何界标。
- **线索**：第一个不同的 tick 与字节明细（依赖 `--sim-cycle-addr`）。
- **排除项**：`spec.regions()` 之外的一切（栈区）、以及被改写成 magic 的导入槽。

`--compare a.seq b.seq` 是逐条轨迹比较——只用于「同一编译器前后两版」，判「与参考等价」必须用
`--compare-state`（两边产物结构不同，轨迹本来就不一样）。

`--where <addr> --seq <file>` 查谁写了某地址（给出 pc + 反汇编 + 源码行）。

## 停机判定

run 完成的判据是 **`sim_running` 被写 0 的那一刻**（`set_setting(sim_running, U64 0)`，
地址 = `settings + StateIndex[sim_running]*8`，从 spec 算，不硬编码）。这条必须精确——
早期按「每 1000 条指令采样」判定，导致两边切点深度差 8 倍、凭空造出 688 字节假差异。

## 已知口径

- `--sim-cycle-addr` 用的 `#SIMULATION_STATE+2567` 是**输出缓存**（`rv32im.si:7169` 注释），
  不是 cycle 计数器。它每 tick 写一次，只配当界标；「tick 数」不能读成「跑了多少 cycle」。
- 启动阶段 1.34 亿条指令是 `reset_sim` 的 128 MB `rep movsb`（Unicorn 按字节计一条指令）。
- `--seed none` 全零、`seq` 递增图案、`xorshift` 随机。**rv32im 用 `xorshift` 会在 `cyc=13`
  报「非法访存」**——`#RESET_DATA_*` 是游戏里的真实内存镜像，宿主没有它，随机字节填进去
  CPU 就会跳到垃圾上。`none`/`seq` 都能跑满 target。
