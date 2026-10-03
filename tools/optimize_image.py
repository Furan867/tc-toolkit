#!python3
"""产物改写器。

当前实现的第一条改写规则（**等长**，因此零重定位）：

    mov ecx, imm32            B9 <imm32>              5 字节
    shl/shr/sar r, cl         REX? D3 <modrm>         2~3 字节
    ------------------------------------------------------------
    shl/shr/sar r, imm8       REX? C1 <modrm> <imm8>   3~4 字节
    nop × 4                                            4 字节
    填充后长度与原来**逐字节相同**，后续指令偏移一个都不动。

为什么等价：
- 两个形式的移位量都只取低 5/6 位（32 位操作数 &0x1F、64 位 &0x3F），所以
  `imm8 = imm32 & 0x3F` 时两条指令的移位量必然相同；
- `modrm` 的 reg 字段（/4 shl、/5 shr、/7 sar）与 mod/rm 原样保留，只有操作码
  `D3`→`C1` 变了，并在末尾补一个立即数字节；
- 4 个 `nop` 吃掉省下的字节，执行流多走 4 条空指令，但**依赖链少了**：不再需要
  先等 `ecx` 写完才能移位。

什么时候**不能**改（保守判据，见 ecx_dead_after）：
`ecx` 的值后面还要用（还有别的移位读 cl、或别的指令读 ecx）、中途有跳转/调用、
或中途有 objdump 解不出的字节 —— 任何一条都放弃这一处，绝不冒险。

用法
----
    py tools/optimize_image.py --in N.raw.bin --out N.opt.bin

诊断
----
- work/opt/optimize.log：每次一行（时间 / 字节数 / sha256 / 改写处数 / 自校验）
  **这行是「子进程真的跑过」的唯一证据**。
- 明细写 stderr（转发层把它引到 work/opt/opt_stderr.log）。
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import time
from collections import Counter

import analyze_jit_code as A

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.normpath(os.path.join(HERE, os.pardir, "work", "opt", "optimize.log"))

SHIFT_MN = ("shl", "shr", "sar")
#: 保守判据：中途只要出现跳转/调用就放弃 —— 免得漏掉"别的路径也会读 ecx"
BRANCH_MN = ("jmp", "je", "jne", "jz", "jnz", "jl", "jg", "jle", "jge",
             "ja", "jb", "jae", "jbe", "call", "ret")
#: objdump 解不出的字节（含义不明）—— 改写必须绕开，扫描遇到就当不安全
UNDECODED = "(bad)"
#: 改写总开关。改成 False 就退回恒等改写（下次编译即生效，**不用重启游戏**）。
#: 万一游戏里发现行为异常，先把这里关掉，再查是不是改写的错。
ENABLE_REWRITE = True
#: REX 前缀（0x40~0x4F）
REX = range(0x40, 0x50)
NOP = b"\x90"


def split_ops(ops: str) -> list[str]:
    return [o.strip() for o in ops.split(",")] if ops else []


def ecx_dead_after(insns, i: int) -> str:
    """第 i 条 `mov ecx,...` 写的值，后面还有人用吗？返回 "" 表示没人用（可改写）。

    扫描到"下一次写 ecx"或"跳转/调用/无法解码的字节"为止：前者说明旧值已作废
    （安全），后者说明存在我们看不全的路径（放弃）。
    """
    for j in range(i + 2, min(i + 66, len(insns))):
        mn, o = insns[j][2], split_ops(insns[j][3])
        if mn == UNDECODED:
            return "中间有无法解码的字节"
        if mn in BRANCH_MN:
            return "中间有跳转/调用"
        if "cl" in o or "ecx" in o:
            if o[:1] == ["ecx"] and mn not in ("cmp", "test"):
                return ""            # 先被覆盖，旧值作废
            return f"{mn} 还要用 ecx/cl"
    return ""


def scan(insns) -> tuple[int, int, Counter]:
    """只统计、不改写：模式共多少处、其中多少处可安全改写、其余各为什么。"""
    total = safe = 0
    reasons: Counter = Counter()
    for i in range(len(insns) - 1):
        _off, raw, mn, ops = insns[i]
        o1 = split_ops(ops)
        if mn != "mov" or o1[:1] != ["ecx"] or len(o1) != 2 or not o1[1].startswith("0x"):
            continue
        if insns[i + 1][2] not in SHIFT_MN or "cl" not in split_ops(insns[i + 1][3]):
            continue
        total += 1
        if len(raw) != 5 or raw[0] != 0xB9:
            reasons["mov 不是 B9+imm32"] += 1
            continue
        why = ecx_dead_after(insns, i)
        if why:
            reasons[why] += 1
        else:
            safe += 1
    return total, safe, reasons


def rewrite(data: bytes, insns) -> tuple[bytes, int, Counter]:
    """就地等长改写，返回 (新字节, 改写处数, 放弃原因)。"""
    out = bytearray(data)
    done = 0
    skipped: Counter = Counter()
    for i in range(len(insns) - 1):
        off, raw1, mn1, ops1 = insns[i]
        raw2 = insns[i + 1][1]
        o1 = split_ops(ops1)
        if mn1 != "mov" or o1[:1] != ["ecx"] or len(o1) != 2 or not o1[1].startswith("0x"):
            continue
        if insns[i + 1][2] not in SHIFT_MN or "cl" not in split_ops(insns[i + 1][3]):
            continue
        if len(raw1) != 5 or raw1[0] != 0xB9:
            skipped["mov 不是 B9+imm32"] += 1
            continue
        why = ecx_dead_after(insns, i)
        if why:
            skipped[why] += 1
            continue

        rex, body = b"", raw2
        if body and body[0] in REX:
            rex, body = body[:1], body[1:]
        if len(body) != 2 or body[0] != 0xD3:
            skipped["移位不是 D3+modrm"] += 1
            continue

        imm8 = raw1[1] & 0x3F                       # 两条形式都只看低 6 位
        new = rex + bytes([0xC1, body[1], imm8])
        window = len(raw1) + len(raw2)
        filler = NOP * (window - len(new))           # 恒为 4 个
        if len(new) + len(filler) != window:
            skipped["窗口长度异常"] += 1
            continue
        out[off:off + window] = new + filler
        done += 1

        if done <= 3:                                # 留几处明细供人工抽查
            print(f"    改写 0x{off:X}: {raw1.hex(' ')} | {raw2.hex(' ')}"
                  f"  ->  {(new + filler).hex(' ')}", file=sys.stderr)
    return bytes(out), done, skipped


def run(src: str, dst: str) -> int:
    try:
        with open(src, "rb") as f:
            data = f.read()
    except OSError as exc:
        print(f"读不到输入：{exc}", file=sys.stderr)
        return 2
    size0 = len(data)

    parsed = A.load_insns(src)
    note = "未改写（解析失败）" if parsed is None else "未改写（开关关闭）"
    if parsed is not None and ENABLE_REWRITE:
        total, safe, _reasons = scan(parsed)
        data, done, skipped = rewrite(data, parsed)
        note = f"改写 {done} 处 / 模式 {total} 处（可改 {safe} 处）"
        for k, v in skipped.most_common(4):
            print(f"    放弃 {v:>5} 处：{k}", file=sys.stderr)

    try:
        with open(dst, "wb") as f:
            f.write(data)
    except OSError as exc:
        print(f"写不出输出：{exc}", file=sys.stderr)
        return 2

    # 自校验：改写后的产物必须仍能完整反汇编（零空洞），否则这次改写作废
    if parsed is not None and ENABLE_REWRITE:
        if len(data) != size0:
            print(f"❌ 长度变了（{size0} → {len(data)}）—— 这次不改写", file=sys.stderr)
            return 3
        if A.load_insns(dst) is None:
            print("❌ 改写后反汇编不自洽 —— 这次不改写", file=sys.stderr)
            return 4

    try:
        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}  "
                    f"{os.path.basename(src)} → {os.path.basename(dst)}  "
                    f"{len(data)} 字节  sha256 {hashlib.sha256(data).hexdigest()}  "
                    f"{note}\n")
    except OSError as exc:
        print(f"写日志失败：{exc}", file=sys.stderr)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="产物改写器")
    ap.add_argument("--in", dest="src", required=True, help="转发层落盘的原始产物")
    ap.add_argument("--out", dest="dst", required=True, help="要写回游戏的产物")
    args = ap.parse_args()
    return run(args.src, args.dst)


if __name__ == "__main__":
    sys.exit(main())
