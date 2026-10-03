#!python3
"""真执行验证第一条改写规则的等价性：

    mov ecx, imm32 ; 移位 r, cl      ≡      移位 r, imm8 ; nop×4

做法是把两段字节各自拷进一块可执行内存**真的跑一遍**，比较返回值 ——
不靠推理，也不靠游戏。覆盖 64/32 位两种操作数宽度 × shl/shr/sar 三种移位，
移位量穷举 0~79（含超出操作数位宽、只能取低位的那些值），并挑一批边界输入值。

用法
----
    py tools/check_shift_equiv.py
"""
from __future__ import annotations

import ctypes
import sys

#: modrm 的 reg 字段：shl=/4、shr=/5、sar=/7（Intel 手册的 opcode 扩展）
SHIFT_REG = {"shl": 4, "shr": 5, "sar": 7}
NOP = b"\x90"

VALUES = [0, 1, 2, 3, 0xFF, 0x100, 0xDEADBEEF, 0x123456789ABCDEF,
          0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF]
SHIFT_IMMS = list(range(80))


def build(value: int, imm32: int, mnemonic: str, width: int) -> tuple[bytes, bytes]:
    """返回 (原序列, 新序列)；两段等长，差别只在中间那两条指令。

    序列：把 value 装进 r12 → 设移位量 → 移位 r12 → 结果搬到 rax → ret
    """
    modrm = 0xC0 | (SHIFT_REG[mnemonic] << 3) | 0x04      # rm=100 + REX.B = r12
    imm8 = imm32 & 0x3F                                   # 两条形式都只看低 5/6 位
    if width == 64:
        prologue = bytes([0x49, 0xBC]) + value.to_bytes(8, "little")
        old_mov = b"\xB9" + imm32.to_bytes(4, "little")
        old_shift = bytes([0x49, 0xD3, modrm])
        new_shift = bytes([0x49, 0xC1, modrm, imm8])
        epilogue = bytes([0x4C, 0x89, 0xE0, 0xC3])        # mov rax,r12 ; ret
        trim = 0
    else:
        prologue = bytes([0x41, 0xBC]) + (value & 0xFFFFFFFF).to_bytes(4, "little")
        old_mov = b"\xB9" + imm32.to_bytes(4, "little")
        old_shift = bytes([0x41, 0xD3, modrm])
        new_shift = bytes([0x41, 0xC1, modrm, imm8])
        epilogue = bytes([0x44, 0x89, 0xE0, 0xC3])        # mov eax,r12d ; ret
        trim = 32

    old = prologue + old_mov + old_shift + epilogue
    window = len(old_mov) + len(old_shift)            # 原窗口：mov + 移位
    filler = NOP * (window - len(new_shift))          # 新序列短多少就补多少 nop
    new = prologue + new_shift + filler + epilogue
    if len(old) != len(new):
        raise AssertionError(f"两段不等长：{len(old)} vs {len(new)}")
    return old, new, trim


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.VirtualAlloc.restype = ctypes.c_void_p
_k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                              ctypes.c_ulong, ctypes.c_ulong]
FN = ctypes.CFUNCTYPE(ctypes.c_uint64)


def run(code: bytes) -> int:
    """把字节拷进 RWX 内存执行，返回 rax。"""
    addr = _k32.VirtualAlloc(None, len(code), 0x3000, 0x40)   # COMMIT|RESERVE, RWX
    if not addr:
        raise OSError("VirtualAlloc 失败")
    ctypes.memmove(addr, code, len(code))
    try:
        return FN(addr)()
    finally:
        _k32.VirtualFree(ctypes.c_void_p(addr), 0, 0x8000)


def main() -> int:
    total = bad = 0
    for width in (64, 32):
        for mnemonic in SHIFT_REG:
            for value in VALUES:
                for imm32 in SHIFT_IMMS:
                    old, new, trim = build(value, imm32, mnemonic, width)
                    a, b = run(old), run(new)
                    if trim:
                        a, b = a & 0xFFFFFFFF, b & 0xFFFFFFFF
                    total += 1
                    if a != b:
                        bad += 1
                        if bad <= 5:
                            print(f"❌ {width} 位 {mnemonic} 值=0x{value:X} 移位量={imm32}"
                                  f"：原 0x{a:X} vs 新 0x{b:X}", file=sys.stderr)
    print(f"执行对比 {total} 组：一致 {total - bad}，不一致 {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
