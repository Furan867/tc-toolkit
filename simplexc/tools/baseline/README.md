# 产物基线回归（重构前后每一份语料的产物必须等价）

编译器重构（`simplexc/lower/` → `simplexc/sema/` + `simplexc/codegen/`）的硬判据：
**98 份语料各自的装配产物，重构前后逐字段相同**。执行体是 `simplexc/tools/blob_baseline.py`，
入口是 `python simplexc/tools/check.py`（步骤 `blob_baseline`）。

## 判什么

不是比 `.obj` —— COFF 头里带编译时间戳，同一份源码两次编译天然不同。比的是 CLI 写出的
**装配后 blob** `<id>.bin` 及 sidecar `<id>.bin.stubs` / `<id>.bin.entry`。

每份语料一行清单（`corpus-baseline_<日期>.tsv`），判等只比前 6 列：

| 列 | 比不比 | 是什么 |
| --- | --- | --- |
| `status` | 比 | `ok` / `fail:rc=<n>`：**失败也是基线的一部分**，见下 |
| `blob_len` | 比 | 产物长度；长度不同直接判不等，不用比内容 |
| `entry` | 比 | `<id>.bin.entry` 的内容（相对机器码起点的入口偏移） |
| `stub_count` | 比 | `.stubs` sidecar 的桩/槽条数 |
| `norm_sha256` | 比 | **掩掉 extern 绝对地址字节后**产物的 sha256 |
| `stubs_sha256` | 比 | `.stubs` sidecar 的**文本** sha256（桩/槽的偏移、种类、符号名） |
| `raw_sha256` | **不比** | 含 kernel32 绝对地址，重启机器就变；留着只为对照 |

`stubs_sha256` 单独比一次是因为「桩块整体前移」这类错误只让偏移变、归一化后产物仍然相等 ——
直接看这列比看字节差好查。

## 怎么 record

产物不吃 git（98 份 blob 合计约 10 MB），清单吃 git（十几 KB）：

```bash
cd tc-toolkit/

# 1. 先有产物（build/ 不进 git）
python simplexc/tools/build.py

# 2. 落基线：清单进 simplexc/tools/baseline/，blob 与 sidecar 落 work/tmp/baseline-blobs/
python simplexc/tools/blob_baseline.py --record simplexc/tools/baseline
```

产物目录用 `--blobs <dir>` 换（两台机器/两个 worktree 各存各的）。文件名的日期是
record 当天，`--verify` 取目录里**唯一**那份 `corpus-baseline_*.tsv`；有多份时要 `--manifest` 指定。

判据（`check.py` 的 `blob_baseline` 步骤就是这条）：

```bash
python simplexc/tools/blob_baseline.py --verify simplexc/tools/baseline
```

退出码：`0` = 98 份全一致；`1` = 有差异（或基线外新增 / 本轮缺失）；`2` = 参数或环境
（缺 CLI / 缺清单 / 缺产物目录 / 清单有多份）。

**产物目录不存在时 `check.py` 记 SKIP 而不是 FAIL**：缺 blob 说明这台机器还没 record，
不是重构把产物改坏了。

## 为什么比 `norm_sha256` 而不是 `raw_sha256`

产物尾部桩块里嵌着 `GetModuleHandleA("kernel32.dll")` + `GetProcAddress` 拿到的**绝对地址**，
基址随 boot / 机器变（同一 boot 内跨进程共用，所以同一 boot 里连 raw 都相同 —— 判据不能靠这个）。
归一化只掩这几段（`masked_ranges()`，唯一定义处）：

- `kind=stub`（代码桩，`48 B8 <imm64> FF E0`）→ 掩 imm64 那 8 字节，前后两字节是确定的；
- `kind=slot`（`__imp_X` 数据槽）→ 整 8 字节都掩。

sidecar 的 `offset` 坐标系是**机器码起点**（`blob + 8`，`assemble.hh` §StubRef），所以落盘偏移要 +8。
**掩多了等于放弃判据**：除这两处之外，产物其余字节（8 字节头 word、entry、机器码、`.rdata`/`.data`）
都必须是确定的。

## 跨 boot 会变的是什么

- 变：`raw_sha256`，以及 `work/tmp/baseline-blobs/` 里那份 blob 在掩掉区间内的字节
  （基线条目录里存的 blob 是**未归一化**的原始产物；判等时两边各自归一化再逐字节比）。
- 不变：清单里的 `norm_sha256` / `stubs_sha256` / `blob_len` / `entry` / `stub_count` / `status`。

所以换机器、换 boot 之后 `--verify` 照样 rc=0；差异报出来的字节区间只在归一化后的产物上算。

## 报出来的差异长什么样

```text
[001 src/001.si] 字段不同：norm_sha256
  norm_sha256   基线 d923ba24…   本轮 00000000…
  字节差区间     首个 0x40..0x40；归一化后共 1 处（0x40..0x40）
```

「文件 / 字段 / 期望 vs 实际 / 首个差异字节区间」四样齐了才判得下去 —— 只有一句 sha 不等，
人还得自己去找第几字节不同。

`--verify` 拿到的期望值是**清单 + 产物目录**：产物目录里的 blob 存在时以它为准（否则改一个字节
判据看不见），两边的 `CMP_FIELDS` 不一致时另报一行「基线产物与清单不符」—— 那是**基线自己坏了**
（产物目录过期或被改过），重新 `--record`，不是重构的锅。`raw_sha256` 不参与这个交叉核对（跨 boot 必变）。

## 什么时候该重建基线，而不是判据失败

- 语料本身变了（`simplexc/corpus/manifest.tsv` 的 `src_sha256` 变）→ 先更新语料，再 record；
- `build/objs/{entry_shim,rt_min}.o` 变了（或换 LLVM / 工具链）→ 产物整体变，record 重建；
- 语料里有 23 份**本来就报错**（未实现 / 外部符号解析器缺符号），它们的 `status` 也在基线里：
  重构后如果它们变成出产物，判据会报差异 —— 那是**有意变更**，人判后重建基线。

## 重建记录

- 2026-10-24：因修复 `arith_ty`（字面量多态铺给所有算术运算符）与移位量掩码（按存储宽度向上取到
  2 的幂再减 1），重建基线，旧基线作废；同一轮把 `size(Int)` 改回声明位宽。
- 2026-10-24（同日第二次）：因修复运算符优先级（`+ - | ^ &` 同级左结合、`* / % << >>` 同级），
  12 份产物变了（100/102/104/106/108/110/112/114/116/157/178/179），旧基线作废。
  这次重建的依据是 coverage 的 `equal` 由 30 升到 39、`diff` 由 14 降到 2 —— 不是「产物变了就重建」。
