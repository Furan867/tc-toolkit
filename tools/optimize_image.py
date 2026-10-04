#!python3
"""产物改写器。

一条改写规则（`mov ecx,imm32` + `移位 r,cl` → `移位 r,imm8`），两种落地方式，
由 opt.ini 的 `rewrite` 三态选择：

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
- rip 相对只有一种形态：`48 8d 0d <rel32>`（lea rcx,[rip+disp]，有符号，disp 在 +3），
  3 处全部指向产物内（函数指针，随代码平移）；
- 跳转目标**没有任何一处落在改写窗口内**（5670 窗口全查过）；目标必须落在旧指令
  边界上，否则视为解析有误、整体放弃。

**产物里混着数据段**（实测 0x213F 起是字符串表；objdump 连续反汇编会把数据
误读成指令，零空洞自检发现不了；数据假跳转的特征是 target 不落任何指令边界，
实测只有 2 条、全在字符串表头部；实测 5670 窗口中落在数据段的为 0）。
产物是**多函数拼接**（单一入口的可达性分析只圈出 0.5%，已证伪），所以不做
全局可达性，防御改为两层：
- 假跳转（target 不落指令边界）单点跳过：不改、原样平移；
- check_reloc **全量**逐指令对照 mn/ops —— 改坏任何数据字节都会让该处反汇编
  文本变化而暴露，整体退回原样。

已知残留风险（接受）：假设游戏从产物**开头**调用（第一个窗口 0xF3 > 头部 8 字节，
入口前的指令原样未动）。若实测发现入口在中部固定偏移，此模式作废。

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


def _map_maker(wins):
    """旧偏移 → 新偏移。窗口整体消失（新指令就放在窗口起点）：
    窗口头与移位起点都映到新窗口起点；落在窗口中段视为错误（None）。"""
    starts = [w[1] for w in wins]
    ends = [w[1] + w[3] for w in wins]

    def map_off(x: int) -> int | None:
        i = bisect.bisect_right(starts, x) - 1
        if i >= 0 and x < ends[i]:                  # 落在窗口 i 里
            if x in (starts[i], starts[i] + 5):     # 窗口头 / 移位起点（mov 恒 5 字节）
                return starts[i] - 4 * i
            return None
        return x - 4 * (i + 1)                      # 前面 i+1 个窗口各缩 4 字节
    return map_off


def rewrite_short(data: bytes, insns):
    """缩短改写 + 重定位（rewrite = short）。

    返回 (新字节, 改写处数, 重定位条数, wins, 放弃原因)；
    任一防御检查不过 → 打印原因并返回 None（调用方退回等长改写，收益保底）。
    数据假跳转（target 不落指令边界）单点跳过：不改、原样平移。
    """
    wins, skipped = find_windows(insns)
    if not wins:
        return None
    map_off = _map_maker(wins)
    starts = {insns[i][0] for i in range(len(insns))} | {w[1] + 5 for w in wins}
    win_head = {w[0]: k for k, w in enumerate(wins)}
    win_tail = {w[0] + 1 for w in wins}

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
            # 探查（2026-10-05）：全部是 48 8d 0d <rel32>。只认这个形态，其余放弃。
            if len(raw) != 7 or raw[0] != 0x48 or raw[1] != 0x8D or raw[2] != 0x0D:
                print(f"❌ 不认识的 rip 形态 0x{off:X}: {raw.hex(' ')}", file=sys.stderr)
                return None
            d = int.from_bytes(raw[3:7], "little", signed=True)
            nt = map_off(off + len(raw) + d)
            if nt is None:
                print(f"❌ rip 目标落在窗口中段（0x{off:X} 处）", file=sys.stderr)
                return None
            nd = nt - (new_off + len(raw))
            b[3:7] = nd.to_bytes(4, "little", signed=True)
            reloc += 1
        out += b
    if len(out) < len(data):                         # 尾部补 nop：总长不变（不在执行路径上）
        out += nops(len(data) - len(out))
    return bytes(out), len(wins), reloc, wins, skipped


def check_reloc(old, new, wins) -> list[str]:
    """独立通道对照：新旧两份反汇编逐指令比对，验证重定位语义。

    不依赖拼装过程的任何内部状态 —— 从两份反汇编直接读：
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
    starts = [w[1] for w in wins]
    ends = [w[1] + w[3] for w in wins]

    def map_off(x):
        i = bisect.bisect_right(starts, x) - 1
        if i >= 0 and x < ends[i]:
            return starts[i] - 4 * i if x in (starts[i], starts[i] + 5) else None
        return x - 4 * (i + 1)

    bad: list[str] = []
    di = 0
    i = j = 0
    while i < len(old):
        if j >= len(new):
            bad.append(f"新序列在旧 0x{old[i][0]:X} 之前就结束了")
            break
        off, raw, mn, ops = old[i]
        noff, nraw, nmn, nops = new[j]
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
            if map_off(t) != nt:
                bad.append(f"0x{off:X}: rip 旧 target 0x{t:X} 应映到"
                           f" 0x{map_off(t):X}，实际是 0x{nt:X}")
        elif ops != nops:
            bad.append(f"0x{off:X}: {ops} → 0x{noff:X}: {nops}")
        i += 1
        j += 1
    for noff, _nraw, nmn, _nops in new[j:]:
        if nmn != "nop":
            bad.append(f"尾部 0x{noff:X} 不是 nop：{nmn}")
    return bad


def rewrite_mode() -> str:
    """三态：'on'（等长）/ 'short'（缩短+重定位）/ 'off'。读不到配置默认 'on'。"""
    cp = configparser.ConfigParser()
    try:
        if not cp.read(CONF, encoding="utf-8"):
            return "on"                       # 没有配置文件：默认等长改写
        v = cp.get("opt", "rewrite", fallback="on").strip().lower()
        return v if v in ("on", "short", "off") else "on"
    except (OSError, configparser.Error) as exc:
        print(f"读配置失败，按 on 处理：{exc}", file=sys.stderr)
        return "on"


def run(src: str, dst: str) -> int:
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
    wins = None
    note = "未改写（解析失败）" if parsed is None else f"未改写（rewrite = {mode}）"

    if parsed is not None and mode == "short":
        r = rewrite_short(data, parsed)
        if r is None:
            print("❌ 缩短改写防御未过 → 退回等长改写（收益保底）", file=sys.stderr)
            mode = "on"
        else:
            data, done, reloc, wins, skipped = r
            note = f"缩短改写 {done} 处 / 重定位 {reloc} 条"
            for k, v in skipped.most_common(4):
                print(f"    放弃 {v:>5} 处：{k}", file=sys.stderr)
            short_used = True

    if parsed is not None and mode == "on" and not short_used:
        data, done, skipped = rewrite(data, parsed)
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
        bad = check_reloc(parsed, ins2, wins)
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
    args = ap.parse_args()
    return run(args.src, args.dst)


if __name__ == "__main__":
    sys.exit(main())
