# U-L7 反例：运行期窄变量怎么加宽（复核）

**结论（一行）**：加宽分两路 —— **编译期常量**按源类型的符号性做 sext/zext；
**运行期窄值**（变量、`load`、运行期表达式）一律**截到声明宽度后零扩展**（`zext(trunc(v))`），
与声明符号性无关。所以 U-L8 报的那条反例**不是**「读了 8 字节、高 4 字节残留 0」——
预填 `0xFF` 与预填 `0x00` 结果逐位相同；U-L7「提升按源类型的符号性」那句话只对常量成立，要改写。

- 器材与原始输出：`reversing/work/tmp/ul7x/`（12 片段 + `.bin` + `.pages` + `evidence.txt`）
- 观测面：`work/tmp/ul7x/<frag>.pages/t0003_0232a9f50000.bin`（tick 3）；
  `#SIMULATION_STATE = Ptr 2416623026176 = 0x232A9F50000` 落在页基址 → **页内偏移 = `+k`**；
  `entry = 0x212F`。片段头逐字节抄自 `work/tmp/ul8/base.si`。

## Q1：S32 窄变量（值 −8）—— 预填 0xFF / 0x00 两组对照

窄变量放在 `p = #SIMULATION_STATE + 384`（8 字节槽）：先预填整槽，再 `store(p, S32 -8)`（窄写只覆盖 4 字节），
然后 `rn = load(<S32>, p)`。`+264` 是把整槽按 `U64` 原样读回（证明预填与窄写都生效）。

`fA`（预填 0xFF）：

```
    let p = #SIMULATION_STATE + 384
    store(p, U64 0xFFFFFFFFFFFFFFFF)                 // 预填 8 字节 FF
    store(p, S32 -8)                                  // 窄写：只写 4 字节，高 4 保留
    store(#SIMULATION_STATE + 264, load(<U64>, p))    // 原始 8 字节
    let rn = load(<S32>, p)                           // 窄变量（S32，值 -8）
    store(#SIMULATION_STATE + 272, U64 rn)
    store(#SIMULATION_STATE + 280, U64 (S64 rn))
```

| 片段 | 预填 | `+264` 整槽（`load(<U64>,p)`） | `+272` `U64 rn` | `+280` `U64 (S64 rn)` |
|---|---|---|---|---|
| `fA` | `FF FF FF FF FF FF FF FF` | `f8 ff ff ff ff ff ff ff` = `0xFFFFFFFFFFFFFFF8` | `f8 ff ff ff 00 00 00 00` = `0x00000000FFFFFFF8` | `0x00000000FFFFFFF8` |
| `fB` | `00 00 00 00 00 00 00 00` | `f8 ff ff ff 00 00 00 00` = `0x00000000FFFFFFF8` | `0x00000000FFFFFFF8` | `0x00000000FFFFFFF8` |

`fB` 与 `fA` 只差预填（`store(p, U64 0)`）。

**裁决**：整槽在两组里明显不同（`FF..F8` vs `00..00FFFFFFF8`），但 `U64 rn` / `U64 (S64 rn)` **逐位相同**
（都是 `0x00000000FFFFFFF8`）→ 转换**没有**把槽的高 4 字节读进来 →
**不是「读 8 字节、高 4 字节残留 0」**；是**按声明宽度（4 字节）取值后零扩展**。

## Q2：S8 窄变量（值 −8）—— 同两组对照

`fC`/`fD` 与 `fA`/`fB` 同形，只是把 `S32` 换成 `S8`（`store(p, S8 -8)` 写 1 字节）。

| 片段 | 预填 | `+264` 整槽 | `+272` `U64 r8` | `+280` `U64 (S64 r8)` |
|---|---|---|---|---|
| `fC` | `FF … FF` | `f8 ff ff ff ff ff ff ff` | `f8 00 00 00 00 00 00 00` = `0x00000000000000F8` | `0x00000000000000F8` |
| `fD` | `00 … 00` | `f8 00 00 00 00 00 00 00` | `0x00000000000000F8` | `0x00000000000000F8` |

**裁决**：与 S32 同规律 —— 预填无关，按 1 字节取值后零扩展（`0xF8`，不是 `0xFFFFFFFFFFFFFFF8`）。

## 常量 vs 运行期：`U64 (S32 -8)` 为什么是符号扩展

`fE`/`fI`（同一运行期值 −8，不同构造；tick 3）：

| 片段 偏移 | 表达式 | 结果 | 分类 |
|---|---|---|---|
| `fI` +264 | `U64 (S32 (cycle - 11))` | `0x00000000FFFFFFF8` | 真运行期 → 零扩展 |
| `fI` +272 | `U64 (S32 (cycle - 1 - 10))` | `0x00000000FFFFFFF8` | 真运行期 → 零扩展 |
| `fI` +280 | `U64 (S32 (cycle - cycle - 8))` | `0xFFFFFFFFFFFFFFF8` | **含恒等式，被折成常量** → 符号扩展 |
| `fI` +288 | `U64 (S32 (cycle * 0 - 8))` | `0xFFFFFFFFFFFFFFF8` | 同上，折成常量 |
| `fI` +296 | `U64 (S32 (0 - 8))` | `0xFFFFFFFFFFFFFFF8` | 纯常量 → 符号扩展 |
| `fE` +272 | `U64 w`，`w = S32 (cycle - 11)` | `0x00000000FFFFFFF8` | 运行期变量 → 零扩展 |
| `fE` +304 | `U64 k`，`k = S32 -8` | `0xFFFFFFFFFFFFFFF8` | 常量变量 → 符号扩展 |

→ U-L8 反例里那条「运行期表达式却符号扩展」的 `U64 (S32 (cycle - cycle - 8))` 是**常量折叠**
（`cycle - cycle` 折成 0，整式变常量 `-8`），不是运行期规则。运行期值一律零扩展。

更多运行期窄值（`fJ`）：`U64 (U32 (cycle - 11))` = `0x00000000FFFFFFF8`、
`U64 (S16 (cycle - 11))` = `0x000000000000FFF8`、`U64 (S8 (cycle - 11))` = `0x00000000000000F8`、
`U64 (S32 rn)` = `U64 (U32 rn)` = `0x00000000FFFFFFF8`（`rn = load(<S32>,p)`）——
**声明符号性在运行期完全不影响加宽结果**。

加固（排除「因为刚刚 store 过所以被折叠成 4 字节读」）：`fG`/`fH` 把地址与值都弄成不透明
（`p = #SIMULATION_STATE + 384 + (cycle - cycle)`、`store(p, S32 (cycle - 11))`），
结果与 `fA`/`fB` 逐位相同（`fG`/`fA`/`x1` 的 tick3 页 md5 相同）。

## 裁决：U-L7 那条怎么改写

`docs/facts.md` §U-L7 现在的句子（`U64 (U32 0xFFFFFFFF)` 零扩展；`U64 (S32 -1)`、`S64 (S8 -1)`、
`U64 (S8 -1)` 符号扩展；`S64 (U8 0xFF)` 零扩展）**全部是常量**，本身没错，但不完整。

建议改写成两条：

1. 「**编译期常量**加宽按源类型符号性：常量折叠后用 64 位带符号表示 —— `U64 (S32 -1)` → `0xFFFFFFFFFFFFFFFF`、
   `U64 (U32 0xFFFFFFFF)` → `0x00000000FFFFFFFF`。可折叠成常量的表达式同此（`U64 (S32 (cycle - cycle - 8))`）。」
2. 「**运行期**窄值（变量、`load`、运行期表达式）加宽 = **截到声明宽度后零扩展** `zext(trunc_W(v))`，
   与声明符号性无关：运行期 `S32 -8` → `0x00000000FFFFFFF8`、`S8 -8` → `0xF8`、`S16 -8` → `0xFFF8`；
   8 字节槽的高 4 字节（预填 `0xFF` 也一样）不参与转换。」

→ lowering 指示：**运行期**整数加宽按 `trunc_W` + `zext` 落（不要 sext），只有编译期常量按符号性 sext。

## 验证命令

每个片段一条（换 `fA` → `fB`/`fC`/`fD`/`fG`/`fH`/`fE`/`fI`/`fJ`/`x0` 即可复跑；首次编译会核对 `out_len == 文件大小`）：

```
cd reversing
python tools/try_compile_dll.py --compile --src work/tmp/ul7x/fA.si          # [0]=0x3288=12936=文件大小, [2]=0x212F
cp work/compile_out.bin work/tmp/ul7x/fA.bin
PYTHONIOENCODING=utf-8 python simplexc/tools/host/host.py --src work/tmp/ul7x/fA.si \
    --bin work/tmp/ul7x/fA.bin --ticks 3 --entry 0x212F \
    --out work/tmp/ul7x/fA.jsonl --dump-pages work/tmp/ul7x/fA.pages
python -c "d=open('work/tmp/ul7x/fA.pages/t0003_0232a9f50000.bin','rb').read(); print(' '.join('%02x'%b for b in d[264:288]))"
# 期望：f8 ff ff ff ff ff ff ff f8 ff ff ff 00 00 00 00 f8 ff ff ff 00 00 00 00
```

一键脚本（在 `reversing/` 下）：`work/tmp/ul7x/run.sh <frag>`（编译+核对+cp+跑+打印 `+256..+400`）、
`work/tmp/ul7x/rerun.sh <frag>`（用已存 `.bin` 重跑并比 tick3 页 md5）。

`fA` 与 `fB` 只差一句 `store(p, U64 0xFFFFFFFFFFFFFFFF)` / `store(p, U64 0)` ——
两组对照必须一起看（只看 `fA` 会误判成「按 8 字节读」）。

## 未解

1. **机制未定**（不影响规则）：运行期零扩展可能来自 ① 32/16/8 位寄存器写入自动清零高位、
   ② 转换代码显式 `zext(trunc_W)`、③ 窄 `load` 自己做了 `trunc+zext`。三者在本观测下不可分。
   对 lowering 无差别（按 `zext(trunc_W)` 落即可）。
2. **真正的栈上局部变量无法预填它的 8 字节槽**：本语言没有取地址一元运算符
   （实测 `let pp = &rn` 报 `No unary operator matched & S32` 且 `out_len = 0`）。
   所以「预填」是用同声明类型（`S32`/`S8`）的**内存变量**实现的；
   局部变量只复现了零扩展（`x0` +264 `let rn = S32 (cycle - 11)` → `U64 rn` = `0x00000000FFFFFFF8`），
   没给它预填非零高位。两者结论一致，但「局部变量是否有自己的 8 字节槽」未证。
3. **常量折叠的边界未穷尽**：只证了 `cycle - cycle`、`cycle * 0` 被折叠；哪些恒等式会被折、折叠发生在什么阶段，未测。
4. **W>32 的运行期宽值**（如运行期 `S64` 负值经 `U64`）未单独测 —— `fE` +280/+288 只是显示 `S64` 不截断，
   没有「加宽」这一步。
