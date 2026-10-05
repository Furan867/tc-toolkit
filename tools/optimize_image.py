#!python3
"""产物改写器。

一条改写规则（`mov ecx,imm32` + `移位 r,cl` → `移位 r,imm8`）加一项删除规则，
由 opt.ini 的 `rewrite` 四态选择：

    mov ecx, imm32            B9 <imm32>              5 字节
    shl/shr/sar r, cl         REX? D3 <modrm>         2~3 字节
    ------------------------------------------------------------
    shl/shr/sar r, imm8       REX? C1 <modrm> <imm8>   3~4 字节

  rewrite = on    （等长）窗口剩余 4 字节用一条多字节 nop 填平 —— 零重定位。
                  实测 RV32IM 电路 5.4 → 6.9 MHz（+27.8%）
  rewrite = short （缩短）窗口只留新移位指令，其后全部前移（实测这份产物
                  -5670 条指令 / -22680 字节），**必须重定位**全部相对跳转与
                  rip 相对（探查：rel32 跳转/调用 949 条 + rip 相对 3 处）。
                  产物总长不变（尾部补 nop）→ 不动长度字段、不换缓冲区。
                  任一防御检查不过 → 子进程非 0 退出 → 转发层放弃写回
  rewrite = short_cse （缩短 + 删重复装载）在 short 之上再删「第二条重复装载」
                  `mov r,imm … mov r,imm`（六条件全过才删，实测 482 处 / 2568 字节）。
                  与 short 共用同一套缩短/重定位框架，收缩量可变：窗口 4 B、
                  CSE 整条 5/6 B。**short 仍是不带 CSE 的纯缩短**，好与已测的
                  8.05 MHz 对照
  rewrite = off   原样返回（对照实验）

为什么等价：
- 两个形式的移位量都只取低 5/6 位（32 位操作数 &0x1F、64 位 &0x3F），所以
  `imm8 = imm32 & 0x3F` 时两条指令的移位量必然相同；
- `modrm` 的 reg 字段（/4 shl、/5 shr、/7 sar）与 mod/rm 原样保留，只有操作码
  `D3`→`C1` 变了，并在末尾补一个立即数字节。

什么时候**不能**改（保守判据，见 ecx_dead_after）：
`ecx` 的值后面还要用（还有别的移位读 cl、或别的指令读 ecx）、中途有跳转/调用、
或中途有 objdump 解不出的字节 —— 任何一条都放弃这一处，绝不冒险。

short 模式的重定位面（探查 work/tmp/probe_jmp.py，2026-10-05 实测）：
- 相对跳转/调用**全部 rel32**：E9（jmp）/ E8（call）disp 在 +1，0F 8x（jcc）disp
  在 +2。没有 rel8（无 ±127 越界问题）；
- 寄存器间接 call（`41 ff d6` 一类，14 条）不是相对寻址，**不动**；
- rip 相对是 `lea r64,[rip+disp32]`（REX.W 8D <modrm: mod=00,rm=101> disp32，
  **reg 任意**，disp 在 +3），两电路实测各 1~3 处，全部指向产物内（函数指针）。
  ⚠ 曾经把 modrm 写死成 `0d`（rcx）—— RV32IM 里恰好只有这一种，换成 MIPS 的
  `lea rax`（`48 8d 05`）就整条 short **静默退回 on**（2026-10-05 实测踩到：
  MIPS 三轮 short/short_cse 全是 on，主频白测）；
- 跳转目标**没有任何一处落在改写窗口内**（5670 窗口全查过）；目标必须落在旧指令
  边界上，否则视为解析有误、整体放弃；rip 目标还必须在**产物内**——跳转有
  「是否指令边界」兜底，rip 没有，超界会让映射静默算出错值。

**产物里混着数据段**（实测 0x213F 起是字符串表；objdump 连续反汇编会把数据
误读成指令，零空洞自检发现不了；数据假跳转的特征是 target 不落任何指令边界，
实测只有 2 条、全在字符串表头部；实测 5670 窗口中落在数据段的为 0）。
产物是**多函数拼接**（单一入口的可达性分析只圈出 0.5%，已证伪），所以不做
全局可达性，防御改为两层：
- 假跳转（target 不落指令边界）单点跳过：不改、原样平移；
- check_reloc **全量**逐指令对照 mn/ops —— 改坏任何数据字节都会让该处反汇编
  文本变化而暴露，整体退回原样。

**入口协议（2026-10-05 实测确认，short 首次上线即崩后查实）**：compile() 的
40 字节输出里 qword[2] 是**入口偏移**（实测：空源码 0x212F、RV32IM 0x215B，
落在固定前缀末尾、组件区之前）—— 游戏每 tick 从这个偏移调用产物。
缩短平移后必须同步更新它（--entry 传入旧值，写 NNN.entry.txt 给转发层改回
Result[2]），否则游戏跳进指令中间：实测主频掉到 0.566（≈5.4/9.5，每 tick
扫完尾部 22680 字节 nop）后越界崩溃。等长模式不动任何偏移，无需更新。

用法
----
    py tools/optimize_image.py --in N.raw.bin --out N.opt.bin

诊断
----
- work/opt/optimize.log：每次一行（时间 / 字节数 / sha256 / 模式与改写处数 / 自校验）
  **这行是「子进程真的跑过」的唯一证据**。
- 明细写 stderr（转发层把它引到 work/opt/opt_stderr.log）。
"""
from __future__ import annotations

import argparse
import bisect
import configparser
import hashlib
import os
import re
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

#: 寄存器别名族：同族任一处写都算「写了该寄存器」（CSE 判定的条件 1）
FAMILY: dict[str, str] = {}
for _name, _aliases in {
    "rax": ("rax", "eax", "ax", "al", "ah"),
    "rcx": ("rcx", "ecx", "cx", "cl", "ch"),
    "rdx": ("rdx", "edx", "dx", "dl", "dh"),
    "rbx": ("rbx", "ebx", "bx", "bl", "bh"),
    "rsp": ("rsp", "esp", "sp", "spl"),
    "rbp": ("rbp", "ebp", "bp", "bpl"),
    "rsi": ("rsi", "esi", "si", "sil"),
    "rdi": ("rdi", "edi", "di", "dil"),
}.items():
    for _a in _aliases:
        FAMILY[_a] = _name
for _k in range(8, 16):
    for _suf in ("", "d", "w", "b"):
        FAMILY[f"r{_k}{_suf}"] = f"r{_k}"

#: 第一操作数不是写入目标的助记符（intel 语法）。call 单列 —— 它虽然也算流打断，
#: 但隐式写 rax/rcx/rdx/rsi/rdi/r8-r11，不能当“不写”。
NO_DST_MN = (frozenset(("cmp", "test", "push", "pop", "nop", "jmp", "ret"))
             | (set(BRANCH_MN) - {"call"}))
#: 顺序流会被打断的助记符（CSE 的条件 2）：跳转/调用/返回 + loop/jrcxz 族。
#: 比 BRANCH_MN 多 loop 族（它也递减 rcx）。
FLOW_BREAK_MN = frozenset(BRANCH_MN) | {"loop", "loope", "loopne", "jrcxz", "jecxz"}
#: 隐式写（宁滥勿缺）。注意 mul/imul/div/idiv 的单操作数形态在这里，
#: 双/三操作数形态的显式目标由操作数解析负责 —— 两者不能混。
IMPLICIT_WRITES: dict[str, tuple[str, ...]] = {
    "call": ("rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"),
    "cdq": ("rdx",), "cqo": ("rdx",), "cwd": ("rdx",), "cdqe": ("rax",),
    "cpuid": ("rax", "rbx", "rcx", "rdx"),
}
#: 单操作数形态隐含写 rax/rdx 的助记符
IMPLICIT_MULDIV = ("mul", "imul", "div", "idiv")
#: objdump 文本里的寄存器 token（`mov r10d,0x1` / `shl r12,cl`）
REG_TOKEN = re.compile(r"\b(?:r(?:ax|bx|cx|dx|sp|bp|si|di)|e(?:ax|bx|cx|dx|sp|bp|si|di)|"
                       r"[abcd][lhx]|[sd]il|[sb]pl|r(?:[89]|1[0-5]))[dwb]?\b")
#: `mov <reg>,<立即数>` 的文本形态（已去掉空格）
MOV_IMM_RE = re.compile(r"^(\w+),(0x[0-9a-f]+|\d+)$")
#: CSE：两条同值装载之间最多隔几条指令。间隔越大越容易踩到别的路径（且收益越小），
#: 实测 ≤5 与 ≤30 的安全子集几乎相同（484 vs 489），所以取小的。
CSE_GAP = 5
#: 配置文件：reversing/opt.ini 的 [opt] 段。**每次编译重新读**，
#: 所以改完下次编译就生效，不用重启游戏。
CONF = os.path.normpath(os.path.join(HERE, os.pardir, "opt.ini"))
#: REX 前缀（0x40~0x4F）
REX = range(0x40, 0x50)
#: 相对跳转/调用助记符全家（含防御性列出产物里未必出现的）
JCC_ALL = ("jmp", "call", "loop", "loope", "loopne", "jrcxz", "jo", "jno", "js", "jns",
           "je", "jz", "jne", "jnz", "jb", "jc", "jnae", "jae", "jnb", "jnc", "jbe",
           "jna", "ja", "jnbe", "jl", "jnge", "jge", "jnl", "jle", "jng", "jg", "jnle",
           "jp", "jpe", "jnp", "jpo")
#: 跳转指令的操作数是 objdump 算好的绝对目标：`0x1234`
RE_ABS_TGT = re.compile(r"^0x[0-9a-f]+$")

#: 单条就能填满 N 字节的 nop（Intel 推荐形态）。**必须用它，而不是一串 `90`**：
#: 填充只是为了"不移动后面的指令"，每多一条 nop 就多占一份前端取指/解码带宽，
#: 本来是为了不拖慢执行才做改写，不该反过来把执行拖慢。
NOPS = {
    1: b"\x90",
    2: b"\x66\x90",
    3: b"\x0f\x1f\x00",
    4: b"\x0f\x1f\x40\x00",
    5: b"\x0f\x1f\x44\x00\x00",
    6: b"\x66\x0f\x1f\x44\x00\x00",
    7: b"\x0f\x1f\x80\x00\x00\x00\x00",
    8: b"\x0f\x1f\x84\x00\x00\x00\x00\x00",
    9: b"\x66\x0f\x1f\x84\x00\x00\x00\x00\x00",
}


def nops(n: int) -> bytes:
    """用尽量少的指令填充 n 字节。"""
    out = b""
    while n > 9:
        out += NOPS[9]
        n -= 9
    return out + (NOPS[n] if n else b"")


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
        if mn.startswith(("rep", "loop")) or mn in ("jrcxz", "jecxz"):
            # rep 族与 loop 把 rcx 当**完整计数**消耗掉，高 24 位不允许是残留
            return f"{mn} 要用完整 ecx"
        if "cl" in o or "ecx" in o:
            if o[:1] == ["ecx"] and mn not in ("cmp", "test"):
                return ""            # 先被覆盖，旧值作废
            return f"{mn} 还要用 ecx/cl"
    return ""


def find_windows(insns):
    """找出可安全改写的窗口。

    返回 ([(mov 在 insns 的下标, 窗口起点, 新字节, 窗口长), ...], 放弃原因 Counter)。
    等长与缩短两种模式共用这一份判定。数据段的假窗口靠 check_reloc 的
    全量 mn/ops 对照兜底（改坏任何字节都会暴露、整体退回原样）。
    """
    wins, skipped = [], Counter()
    for i in range(len(insns) - 1):
        off, raw1, mn1, ops1 = insns[i]
        o1 = split_ops(ops1)
        if mn1 != "mov" or o1[:1] != ["ecx"] or len(o1) != 2 or not o1[1].startswith("0x"):
            continue
        raw2, mn2, ops2 = insns[i + 1][1], insns[i + 1][2], insns[i + 1][3]
        if mn2 not in SHIFT_MN or "cl" not in split_ops(ops2):
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
        wins.append((i, off, rex + bytes([0xC1, body[1], imm8]),
                     len(raw1) + len(raw2)))
    return wins, skipped


def _mov_imm(insn):
    """`mov <reg>,<立即数>` → (寄存器, 立即数)，否则 None。"""
    if insn[2] != "mov":
        return None
    m = MOV_IMM_RE.match((insn[3] or "").replace(" ", ""))
    if not m or m.group(1) not in FAMILY:
        return None
    return m.group(1), m.group(2)


def written_families(insn) -> set[str]:
    """该指令写到的寄存器族（显式目标 + 隐式写）。宁滥勿缺 —— CSE 的条件 1。

    ⚠️ `rep` 前缀指令（`rep stos` 等）会**消耗 rcx**（计数递减到 0），objdump 把
    前缀拼进助记符（`rep stos`），所以必须用 startswith 判，不能靠隐式写表查键 ——
    漏了它就会把「rep 之后重装同一计数」误删（实测踩过 3 处）。
    """
    out: set[str] = set()
    mn, ops = insn[2], (insn[3] or "").replace(" ", "")
    if mn.startswith(("rep", "loop")):
        out |= {"rcx", "rdi", "rsi"}                 # 计数 + 字符串指令的双指针
    if mn in ("push", "pop"):
        out.add("rsp")
    if mn == "call":
        return out | {FAMILY[r] for r in IMPLICIT_WRITES["call"] if r in FAMILY}
    if mn in NO_DST_MN:
        return out
    if mn in IMPLICIT_MULDIV and "," not in ops:     # 单操作数：隐式 rax/rdx
        return {"rax", "rdx"}
    if mn in IMPLICIT_WRITES:
        return {FAMILY[r] for r in IMPLICIT_WRITES[mn] if r in FAMILY}
    dst = ops if mn == "xchg" else (ops.split(",", 1)[0] if "," in ops else ops)
    for r in REG_TOKEN.findall(dst):
        if r in FAMILY:
            out.add(FAMILY[r])
    return out


def jump_targets(insns) -> set[int]:
    """全部 rel32 跳转/调用的目标偏移（产物内边界过滤，挡掉数据段的假跳转）。"""
    lo, hi = insns[0][0], insns[-1][0] + len(insns[-1][1])
    tgts: set[int] = set()
    for off, raw, _mn, _ops in insns:
        disp = None
        if len(raw) == 5 and raw[0] in (0xE9, 0xE8):
            disp = int.from_bytes(raw[1:5], "little", signed=True)
        elif len(raw) == 6 and raw[0] == 0x0F and 0x80 <= raw[1] <= 0x8F:
            disp = int.from_bytes(raw[2:6], "little", signed=True)
        if disp is not None:
            t = off + len(raw) + disp
            if lo <= t < hi:
                tgts.add(t)
    return tgts


def find_cse(insns, exclude=()) -> list[tuple[int, int, int]]:
    """找可安全删除的「重复装载」：`mov <reg>,<imm> ... mov <reg>,<imm>`（同 reg 同值）。

    删第二条要六个条件全过（前五个与 `work/tmp/check_cse.py` 一致，第六条是本实现补的）：
      1) 中间没写该寄存器族（写同族任一处都算）—— 写了就得重装；
      2) 中间没有跳转/调用/返回 —— **顺序流必须连续**，否则区间可能来自别的路径，
         那时寄存器值不保证还等于第一条装载的值（`check_cse.py` 漏了这条）；
      3) 区间内没有跳转目标 —— 外部跳进区间要靠第二条装载；
      4) 第二条自己不是跳转目标（实测块接缝处的 mov 往往正是入口）；
      5) 第二条不是 `exclude`（移位窗口头）—— 那里交给窗口改写，收益更大；
      6) 中间没有无法解码的字节。

    返回 [(insns 下标, 偏移, 字节长), ...]，按偏移升序。
    """
    tgts = jump_targets(insns)
    exclude = set(exclude)
    out: list[tuple[int, int, int]] = []
    last: dict[tuple[str, str], int] = {}
    for idx, insn in enumerate(insns):
        key = _mov_imm(insn)
        if key is None:
            continue
        j = last.get(key)
        if j is not None and idx - j <= CSE_GAP:
            fam = FAMILY[key[0]]
            ok = True
            for k in range(j + 1, idx):
                mn = insns[k][2]
                if mn == UNDECODED or mn in FLOW_BREAK_MN or insns[k][0] in tgts \
                        or fam in written_families(insns[k]):
                    ok = False
                    break
            if ok and insns[idx][0] not in tgts and idx not in exclude:
                out.append((idx, insn[0], len(insn[1])))
        last[key] = idx
    return out


def rewrite(data: bytes, insns) -> tuple[bytes, int, Counter]:
    """就地等长改写（rewrite = on），返回 (新字节, 改写处数, 放弃原因)。"""
    wins, skipped = find_windows(insns)
    out = bytearray(data)
    for k, (i, off, new, window) in enumerate(wins):
        filler = nops(window - len(new))             # 恒 4 字节，但只占 1 条指令
        out[off:off + window] = new + filler
        if k < 3:                                   # 留几处明细供人工抽查
            print(f"    改写 0x{off:X}: {insns[i][1].hex(' ')} | {insns[i + 1][1].hex(' ')}"
                  f"  ->  {(new + filler).hex(' ')}", file=sys.stderr)
    return bytes(out), len(wins), skipped


def cuts_of(wins, cse) -> list[tuple[int, int]]:
    """收缩点列表，升序：`(旧偏移, 收缩字节数)`。

    窗口收缩恒 4 字节（旧 `mov ecx,imm32`+移位 = 7/8 B → 新移位 = 3/4 B）；
    CSE 删的是**整条**装载（5 B 或 6 B）。两者相加就是总缩短量。
    """
    cuts = [(w[1], 4) for w in wins] + [(c[1], c[2]) for c in cse]
    cuts.sort()
    return cuts


def _map_maker(wins, cse):
    """旧偏移 → 新偏移：减去「位于它之前的所有收缩量」。

    窗口头与移位起点都映到新窗口起点（窗口内两条并成一条）；落在窗口中段或
    CSE 删除的指令内部（非起点）视为错误（None）——调用方据此放弃本轮缩短。
    """
    starts = [w[1] for w in wins]
    ends = [w[1] + w[3] for w in wins]
    cuts = cuts_of(wins, cse)
    coffs = [c[0] for c in cuts]
    csize = {c[1]: c[2] for c in cse}               # 键必须是**偏移**，不是 insns 下标
    prefix = [0]
    for _o, d in cuts:
        prefix.append(prefix[-1] + d)

    def map_off(x: int) -> int | None:
        i = bisect.bisect_right(starts, x) - 1
        if i >= 0 and x < ends[i] and x not in (starts[i], starts[i] + 5):
            return None                             # 窗口中段（那条空隙不存在）
        m = bisect.bisect_left(coffs, x)
        if m > 0 and coffs[m - 1] < x < coffs[m - 1] + csize.get(coffs[m - 1], 0):
            return None                             # CSE 删除的指令内部（非起点）
        return x - prefix[m]
    return map_off


def rewrite_short(data: bytes, insns, wins, cse, skipped):
    """缩短改写 + 重定位（rewrite = short）：移位窗口与 CSE 删除一次平移到位。

    返回 (新字节, 重定位条数)；任一防御检查不过 → 打印原因并返回 None
    （调用方退回等长改写，收益保底）。
    数据假跳转（target 不落指令边界）单点跳过：不改、原样平移。
    """
    if not wins and not cse:
        return None
    map_off = _map_maker(wins, cse)
    starts = {insns[i][0] for i in range(len(insns))} | {w[1] + 5 for w in wins}
    win_head = {w[0]: k for k, w in enumerate(wins)}
    win_tail = {w[0] + 1 for w in wins}
    cut_idx = {c[0] for c in cse}

    out = bytearray(data[:8])                       # 长度头原样（总长不变，值也不用动）
    di = 0                                          # 已缩短的字节数
    reloc = 0
    for idx, (off, raw, mn, ops) in enumerate(insns):
        if idx in win_head:
            _i, _soff, new, window = wins[win_head[idx]]
            out += new
            di += window - len(new)                 # 恒 4
            continue
        if idx in win_tail:                         # 旧移位指令不拷贝
            continue
        if idx in cut_idx:                          # CSE：整条重复装载消失
            di += len(raw)
            continue
        b = bytearray(raw)
        new_off = off - di
        if mn in JCC_ALL and RE_ABS_TGT.match(ops.strip()):
            t = int(ops, 16)
            if t not in starts:                  # 数据假跳转：不改，原样平移
                out += b
                continue
            nt = map_off(t)
            if nt is None:
                print(f"❌ 跳转目标 0x{t:X} 落在窗口中段", file=sys.stderr)
                return None
            nd = nt - (new_off + len(raw))
            if len(raw) == 5 and raw[0] in (0xE9, 0xE8):
                b[1:5] = nd.to_bytes(4, "little", signed=True)
            elif len(raw) == 6 and raw[0] == 0x0F and 0x80 <= raw[1] <= 0x8F:
                b[2:6] = nd.to_bytes(4, "little", signed=True)
            elif len(raw) == 2 and (raw[0] == 0xEB or 0x70 <= raw[0] <= 0x7F):
                if not -128 <= nd <= 127:
                    print(f"❌ rel8 重定位越界（0x{off:X} 处）", file=sys.stderr)
                    return None
                b[1] = nd & 0xFF
            else:
                print(f"❌ 不认识的跳转编码 0x{off:X}: {raw.hex(' ')}", file=sys.stderr)
                return None
            reloc += 1
        elif "[rip" in ops:
            # lea r64,[rip+disp32] = REX.W 8D <modrm: mod=00, rm=101> disp32，**reg 任意**
            # （0d=rcx、05=rax、15=rdx…；
            #  高 REX.R 位（0x4c 前缀）能括到 r8–r15）。
            # ⚠ 曾经把 reg 写死成 rcx（0d）：RV32IM 里恰好只有这一种形态，换成 MIPS
            # （lea rax）就整条 short 静默退回 on（2026-10-05 实测踩到）。
            if (len(raw) != 7 or (raw[0] & 0xF8) != 0x48 or raw[1] != 0x8D
                    or (raw[2] & 0xC7) != 0x05):
                print(f"❌ 不认识的 rip 形态 0x{off:X}: {raw.hex(' ')}", file=sys.stderr)
                return None
            d = int.from_bytes(raw[3:7], "little", signed=True)
            t = off + len(raw) + d
            if not 0 <= t < len(data):               # 目标跑出产物 → 平移映射没意义
                print(f"❌ rip 目标 0x{t:X} 在产物外（0x{off:X} 处）", file=sys.stderr)
                return None
            nt = map_off(t)
            if nt is None:
                print(f"❌ rip 目标落在窗口中段（0x{off:X} 处）", file=sys.stderr)
                return None
            nd = nt - (new_off + len(raw))
            b[3:7] = nd.to_bytes(4, "little", signed=True)
            reloc += 1
        out += b
    if len(out) < len(data):                         # 尾部补 nop：总长不变（不在执行路径上）
        out += nops(len(data) - len(out))
    return bytes(out), reloc


def check_reloc(old, new, wins, cse) -> list[str]:
    """独立通道对照：新旧两份反汇编逐指令比对，验证重定位语义。

    不依赖拼装过程的任何内部状态（连 map_off 都在这里重写一遍，与生产路径互为对照）
    —— 从两份反汇编直接读：
    - CSE 删除的指令：旧序列有、新序列无，且不得是跳转目标（否则那里就少了一次装载）；
    - 窗口头：旧 (mov,移位) 两条 → 新一条移位，起点必须等于旧起点减累计缩短量；
    - 其余指令 mn/ops 逐字相同 —— **这条同时是完备的数据防御**：改坏任何数据
      字节（假窗口/假跳转被误动）都会让该处反汇编文本变化而暴露；
    - 跳转按 target 是否落在指令边界分两类：真跳转（被重定位）新 target 必须等于
      旧 target 过映射；假跳转（数据，未动）新 target 必须等于旧 target 平移；
    - rip 相对把 disp 换算成 target 再比；
    - 逐条指令的新偏移必须等于旧偏移减累计缩短量；
    - 对照循环结束后，新序列剩余必须全是 nop（尾部填充）。
    """
    istarts = {ins[0] for ins in old} | {w[1] + 5 for w in wins}
    win_no = {w[0]: k for k, w in enumerate(wins)}
    cut_idx = {c[0] for c in cse}
    # 独立重算跳转目标（不复用 jump_targets，好让两套实现对照着抓错）：
    # CSE 删掉的指令绝不能是任何跳转的目标 —— 否则那条路径来了就少一次装载。
    tgts: set[int] = set()
    for _off, _raw, _mn, _ops in old:
        _disp = None
        if len(_raw) == 5 and _raw[0] in (0xE9, 0xE8):
            _disp = int.from_bytes(_raw[1:5], "little", signed=True)
        elif len(_raw) == 6 and _raw[0] == 0x0F and 0x80 <= _raw[1] <= 0x8F:
            _disp = int.from_bytes(_raw[2:6], "little", signed=True)
        if _disp is not None:
            tgts.add(_off + len(_raw) + _disp)
    starts = [w[1] for w in wins]
    ends = [w[1] + w[3] for w in wins]
    cuts = sorted([(w[1], 4) for w in wins] + [(c[1], c[2]) for c in cse])
    coffs = [c[0] for c in cuts]
    csize = {c[1]: c[2] for c in cse}               # 键必须是**偏移**，不是 insns 下标
    prefix = [0]
    for _o, d in cuts:
        prefix.append(prefix[-1] + d)

    def map_off(x):
        i = bisect.bisect_right(starts, x) - 1
        if i >= 0 and x < ends[i] and x not in (starts[i], starts[i] + 5):
            return None
        m = bisect.bisect_left(coffs, x)
        if m > 0 and coffs[m - 1] < x < coffs[m - 1] + csize.get(coffs[m - 1], 0):
            return None
        return x - prefix[m]

    hi = old[-1][0] + len(old[-1][1])
    bad: list[str] = []
    di = 0
    i = j = 0
    while i < len(old):
        if j >= len(new):
            bad.append(f"新序列在旧 0x{old[i][0]:X} 之前就结束了")
            break
        off, raw, mn, ops = old[i]
        noff, nraw, nmn, nops = new[j]
        if i in cut_idx:                        # 旧序列这条被 CSE 删：新序列无对应
            if off in tgts:
                bad.append(f"CSE 删掉的 0x{off:X} 是跳转目标（那条路径会少一次装载）")
            di += len(raw)
            i += 1
            continue
        if i in win_no:
            k = win_no[i]
            _ii, _soff, newb, window = wins[k]
            if noff != off - di or nraw != newb:
                bad.append(f"窗口 0x{off:X} → 0x{noff:X}: {nraw.hex(' ')}"
                           f"（期望 0x{off - di:X}: {newb.hex(' ')}）")
            di += window - len(newb)
            i += 2
            j += 1
            continue
        if noff != off - di:
            bad.append(f"偏移 0x{off:X} → 0x{noff:X}（期望 0x{off - di:X}）")
        if nmn != mn:
            bad.append(f"0x{off:X}: {mn} {ops} → 0x{noff:X}: {nmn} {nops}")
        elif mn in JCC_ALL and RE_ABS_TGT.match(ops.strip()):
            t, nt = int(ops, 16), int(nops, 16)
            if t in istarts:                    # 真跳转：必须按映射重定位
                want = map_off(t)
                if want != nt:
                    bad.append(f"0x{off:X}: {mn} 旧 target 0x{t:X} 应映到"
                               f" {hex(want) if want is not None else '窗口中段'}，"
                               f"实际是 0x{nt:X}")
            elif nt != t - di:                   # 数据假跳转：未动，整条平移
                bad.append(f"0x{off:X}: 假跳转 {mn} 0x{t:X} 应平移到 0x{t - di:X}，"
                           f"实际是 0x{nt:X}")
        elif "[rip" in ops:
            t = off + len(raw) + int.from_bytes(raw[3:7], "little", signed=True)
            nt = noff + len(nraw) + int.from_bytes(nraw[3:7], "little", signed=True)
            # 目标必须在产物内（生产路径也是这么判的，这里独立重算一遍）
            want = map_off(t) if 0 <= t < hi else None
            if want != nt:
                bad.append(f"0x{off:X}: rip 旧 target 0x{t:X} 应映到"
                           f" {hex(want) if want is not None else '产物外/窗口中段'}，"
                           f"实际是 0x{nt:X}")
        elif ops != nops:
            bad.append(f"0x{off:X}: {ops} → 0x{noff:X}: {nops}")
        i += 1
        j += 1
    for noff, _nraw, nmn, _nops in new[j:]:
        if nmn != "nop":
            bad.append(f"尾部 0x{noff:X} 不是 nop：{nmn}")
    return bad


def rewrite_mode() -> str:
    """四态：'on'（等长）/ 'short'（缩短+重定位）/ 'short_cse'（缩短 + 删重复装载）
    / 'off'。读不到配置默认 'on'。

    环境变量 `OPT_REWRITE` 优先于 opt.ini —— 离线验证用（opt.ini 归游戏内实验用，
    验证时不应该去动它）：`OPT_REWRITE=short py tools/optimize_image.py --in ...  `
    """
    v = os.environ.get("OPT_REWRITE", "").strip().lower()
    if v in ("on", "short", "short_cse", "off"):
        return v
    cp = configparser.ConfigParser()
    try:
        if not cp.read(CONF, encoding="utf-8"):
            return "on"                       # 没有配置文件：默认等长改写
        v = cp.get("opt", "rewrite", fallback="on").strip().lower()
        return v if v in ("on", "short", "short_cse", "off") else "on"
    except (OSError, configparser.Error) as exc:
        print(f"读配置失败，按 on 处理：{exc}", file=sys.stderr)
        return "on"


def run(src: str, dst: str, entry: int | None = None) -> int:
    try:
        with open(src, "rb") as f:
            data0 = f.read()                        # 原样，自校验失败时回写它
    except OSError as exc:
        print(f"读不到输入：{exc}", file=sys.stderr)
        return 2
    size0 = len(data0)
    data = data0

    parsed = A.load_insns(src)
    mode = rewrite_mode()
    short_used = False
    wins: list[tuple[int, int, bytes, int]] = []
    cse: list[tuple[int, int, int]] = []
    new_entry = None
    note = "未改写（解析失败）" if parsed is None else f"未改写（rewrite = {mode}）"

    if parsed is not None and mode in ("short", "short_cse") and entry is None:
        # 防呆：老转发层不传 --entry，short 会把游戏跳崩（见 docstring 入口协议）
        print("⚠ 转发层没传入口偏移（老版本？）→ 退回等长改写", file=sys.stderr)
        mode = "on"

    if parsed is not None and mode in ("short", "short_cse"):
        wins, skipped = find_windows(parsed)
        cse = (find_cse(parsed, exclude={w[0] for w in wins})
               if mode == "short_cse" else [])
        r = rewrite_short(data0, parsed, wins, cse, skipped)
        if r is None:
            print("❌ 缩短改写防御未过 → 退回等长改写（收益保底）", file=sys.stderr)
            mode = "on"
        else:
            data, reloc = r
            note = f"缩短改写 {len(wins)} 处"
            if cse:
                note += f" / CSE 删 {len(cse)} 处（{sum(c[2] for c in cse)} B）"
            note += f" / 重定位 {reloc} 条"
            for k, v in skipped.most_common(4):
                print(f"    放弃 {v:>5} 处：{k}", file=sys.stderr)
            for i, off, size in cse[:3]:                 # 留几处明细供人工抽查
                print(f"    CSE 删 0x{off:X}: {parsed[i][2]} {parsed[i][3]}（{size} B）",
                      file=sys.stderr)
            map_off = _map_maker(wins, cse)
            starts = ({parsed[i][0] for i in range(len(parsed))}
                      | {w[1] + 5 for w in wins})
            cut_off = {c[1] for c in cse}
            if (entry is None or entry not in starts or entry in cut_off
                    or map_off(entry) is None):
                print(f"❌ 入口偏移 0x{entry:X} 不在指令边界 → 退回等长改写", file=sys.stderr)
                mode, data, wins, cse = "on", data0, [], []
            else:
                new_entry = map_off(entry)
                note += f" / 入口 0x{entry:X}→0x{new_entry:X}"
                short_used = True

    if parsed is not None and mode == "on" and not short_used:
        data, done, skipped = rewrite(data0, parsed)
        note = f"改写 {done} 处 / 模式 {done + sum(skipped.values())} 处（等长）"
        for k, v in skipped.most_common(4):
            print(f"    放弃 {v:>5} 处：{k}", file=sys.stderr)

    def write_out(payload: bytes) -> bool:
        try:
            with open(dst, "wb") as f:
                f.write(payload)
            return True
        except OSError as exc:
            print(f"写不出输出：{exc}", file=sys.stderr)
            return False

    if not write_out(data):
        return 2

    # --- 自校验（任何一条不过：回写原样 + 非 0 退出 → 转发层放弃，游戏拿到原样） ---
    if short_used:
        if len(data) != size0:
            print(f"❌ 长度变了（{size0} → {len(data)}）", file=sys.stderr)
            write_out(data0)
            return 3
        ins2 = A.load_insns(dst)
        if ins2 is None:
            print("❌ 缩短产物反汇编不自洽", file=sys.stderr)
            write_out(data0)
            return 4
        bad = check_reloc(parsed, ins2, wins, cse)
        if bad:
            for line in bad[:6]:
                print(f"❌ {line}", file=sys.stderr)
            if len(bad) > 6:
                print(f"❌ ……共 {len(bad)} 条", file=sys.stderr)
            write_out(data0)
            return 5
        print(f"    对照自校验通过：{len(ins2)} 条指令、重定位语义逐条一致", file=sys.stderr)
    elif parsed is not None and mode == "on":
        if len(data) != size0:
            print(f"❌ 长度变了（{size0} → {len(data)}）—— 这次不改写", file=sys.stderr)
            write_out(data0)
            return 3
        if A.load_insns(dst) is None:
            print("❌ 改写后反汇编不自洽 —— 这次不改写", file=sys.stderr)
            write_out(data0)
            return 4

    # 入口文件最后才写（一切自检通过后）：任何失败路径都没写过它，
    # 转发层读不到就不会改 Result[2]，游戏拿到的仍是原样产物与原样入口。
    if short_used and new_entry is not None:
        entry_path = os.path.join(os.path.dirname(dst),
                                   os.path.basename(dst).split(".")[0] + ".entry.txt")
        try:
            with open(entry_path, "w", encoding="ascii") as f:
                f.write(hex(new_entry))
        except OSError as exc:
            print(f"❌ 写不出入口文件：{exc} → 整体退回原样", file=sys.stderr)
            write_out(data0)
            return 6

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
    ap = argparse.ArgumentParser(description="产物改写器（等长 / 缩短两模式）")
    ap.add_argument("--in", dest="src", required=True, help="转发层落盘的原始产物")
    ap.add_argument("--out", dest="dst", required=True, help="要写回游戏的产物")
    ap.add_argument("--entry", help="产物入口偏移（转发层从 Result[2] 取来，十六进制）——"
                    "缩短模式必须同步更新它，否则游戏跳进指令中间（见模块 docstring）")
    args = ap.parse_args()
    entry = int(args.entry, 16) if args.entry else None
    return run(args.src, args.dst, entry)


if __name__ == "__main__":
    sys.exit(main())
