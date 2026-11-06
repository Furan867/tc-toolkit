"""Parse COFF symbol table from Turing Complete.exe and recover compile-unit -> function mapping."""
import struct
import os
import sys
from collections import defaultdict

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # 仓库根（decompile/ 的父）
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from libs import tc_common  # noqa: E402

EXE = tc_common.game_exe()
OUTDIR = os.path.join(_ROOT, "work")

with open(EXE, "rb") as f:
    data = f.read()

e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
coff = e_lfanew + 4
symtab_off, num_syms = struct.unpack_from("<II", data, coff + 8)
print(f"Symbol table: offset=0x{symtab_off:X}, entries={num_syms}")

strtab_off = symtab_off + num_syms * 18
strtab_len = struct.unpack_from("<I", data, strtab_off)[0]
strtab = data[strtab_off + 4 : strtab_off + strtab_len]
print(f"String table: offset=0x{strtab_off:X}, len={strtab_len}")

def get_name(name_bytes, off):
    if name_bytes[:4] == b"\x00\x00\x00\x00":
        o = struct.unpack_from("<I", name_bytes, 4)[0]
        end = strtab.find(b"\x00", o)
        return strtab[o:end].decode("utf-8", "replace")
    return name_bytes.rstrip(b"\x00").decode("utf-8", "replace")

IMAGE_SYM_CLASS_FILE = 103
IMAGE_SYM_CLASS_EXTERNAL = 2
IMAGE_SYM_CLASS_STATIC = 3

files = []          # list of (index, path)
functions = []      # list of (unit_path, name, value, storage_class)
current_file = None
i = 0
while i < num_syms:
    off = symtab_off + i * 18
    entry = data[off:off + 18]
    name_bytes, value, sec, typ, scl, naux = struct.unpack_from("<8sIHHBB", entry)
    name = get_name(name_bytes, off)

    if name == ".file" and scl == IMAGE_SYM_CLASS_FILE and naux == 1:
        aux = data[off + 18: off + 36]
        # aux contains the filename (may use string table via first 4 bytes == 0)
        fname = get_name(aux, off + 18)
        current_file = fname
        files.append(fname)
    elif scl in (IMAGE_SYM_CLASS_EXTERNAL, IMAGE_SYM_CLASS_STATIC) and typ in (0x20, 0x21) and sec > 0:
        functions.append((current_file, name, value))

    i += 1 + naux

print(f"Compile units (.file records): {len(files)}")
print(f"Function symbols: {len(functions)}")

os.makedirs(OUTDIR, exist_ok=True)

with open(os.path.join(OUTDIR, "compile_units.txt"), "w", encoding="utf-8") as f:
    for p in sorted(set(files)):
        f.write(p + "\n")

with open(os.path.join(OUTDIR, "functions_by_unit.tsv"), "w", encoding="utf-8") as f:
    for unit, name, value in sorted(functions):
        f.write(f"{unit}\t{name}\t0x{value:X}\n")

# Summary: per-unit function counts
per_unit = defaultdict(int)
for unit, name, value in functions:
    per_unit[unit] += 1
print("\nTop compile units by function count:")
for unit, cnt in sorted(per_unit.items(), key=lambda x: -x[1])[:25]:
    print(f"  {cnt:5d}  {unit}")
