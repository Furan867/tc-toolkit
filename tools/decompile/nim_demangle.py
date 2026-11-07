"""Rebuild the Turing Complete project tree from Nim-mangled symbols.

Data sources:
- strings in the COFF string table (authoritative module names, full length)
- nm output (authoritative symbol names + addresses, parsed by GNU BFD)

Nim 2.x symbol format: [TypeName_]procname[_opsuffix]__moduleZpath[_uID]
  - module path segments separated by 'Z'
  - '_' in path encoded as '95' (decimal ord), other chars as 'u' + decimal ord
  - trailing '_u<digits>' is a compiler disambiguation id
  - operators in proc names as hex ord (e.g. '5D' = ']', '3D' = '='); 'X' + hex
    escapes also occur (e.g. X5BX5D = '[]')
  - methods are prefixed with their type: renderer_destroy = Renderer.destroy
"""
import re
import os
import struct
import sys
from collections import defaultdict

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # 仓库根（tools/decompile/ 的上两级）
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from libs import tc_common  # noqa: E402

EXE = tc_common.game_exe()
WORK = os.path.join(_ROOT, "work")
OUTROOT = os.path.join(_ROOT, "recovered", "src")

# ---------- 1. module list from string table ----------
with open(EXE, "rb") as f:
    data = f.read()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
coff = e_lfanew + 4
symtab_off, num_syms = struct.unpack_from("<II", data, coff + 8)
strtab_off = symtab_off + num_syms * 18
strtab_len = struct.unpack_from("<I", data, strtab_off)[0]
strtab = data[strtab_off + 4 : strtab_off + strtab_len]

modules_raw = set()
for m in re.finditer(rb"[\x00-\x7f]+", strtab):
    s = m.group(0).decode("ascii", "replace")
    if s.endswith(".nim.c"):
        modules_raw.add(s)

print(f".nim.c entries in string table: {len(modules_raw)}")

# ---------- 2. demangle helpers ----------

def decode_proc_name(p: str) -> str:
    """Decode operator escapes in proc part: X<hex> and <hex-only tokens like 5Deq."""
    # X + hex escapes first
    p = re.sub(r"X([0-9A-Fa-f]{2})", lambda mm: chr(int(mm.group(1), 16)), p)
    # bare hex pairs that decode to printable punctuation (5D -> ])
    def bare(m):
        c = chr(int(m.group(0), 16))
        return c if 0x21 <= ord(c) <= 0x7E and c not in "0123456789" else m.group(0)
    p = re.sub(r"\b([0-9A-F]{2})\b", bare, p)
    return p

def demangle_module(m: str) -> str:
    """presenterZboard95ui -> presenter/board_ui"""
    m = re.sub(r"_u\d+$", "", m)  # strip disambiguation id
    m = re.sub(r"\.part\.\d+$", "", m)  # strip GCC function-split suffix
    m = re.sub(r"\.cold$", "", m)       # strip GCC cold-path suffix
    segs = []
    for seg in m.split("Z"):
        seg = re.sub(r"95", "_", seg)  # '_' encoded as 95
        seg = re.sub(r"u(\d{1,3})", lambda mm: chr(int(mm.group(1))) if 32 <= int(mm.group(1)) < 127 else mm.group(0), seg)
        segs.append(seg)
    return "/".join(segs)

MODULE_RE = re.compile(r"^[a-z_][a-z0-9_]*(\/[a-z_][a-z0-9_]*)*$")

def valid_module(m: str) -> bool:
    return bool(MODULE_RE.match(m))

def demangle_symbol(sym: str):
    """Return (procname, module) or None."""
    if "__" not in sym:
        return None
    proc, _, mod = sym.partition("__")
    if not mod:
        return None
    proc = decode_proc_name(proc)
    if not proc:
        proc = "<module-init>"
    module = demangle_module(mod).lstrip("_")
    if not valid_module(module):
        return None
    return proc, module

# ---------- 3. parse nm output ----------
functions = defaultdict(set)   # module -> set of (proc, addr)
vars_by_module = defaultdict(set)
unmatched = []

with open(os.path.join(WORK, "symbols_raw.txt"), encoding="utf-8", errors="replace") as f:
    for line in f:
        parts = line.split()
        if len(parts) < 3:
            continue
        addr, typ, name = parts[0], parts[1], parts[2]
        if name.startswith(("NTI", "__imp", "__mingw", ".refptr")):
            continue
        if typ in "Tt":
            d = demangle_symbol(name)
            if d:
                functions[d[1]].add((d[0], addr))
            else:
                unmatched.append((name, addr))
        elif typ in "DdRrBb":
            d = demangle_symbol(name)
            if d:
                vars_by_module[d[1]].add((d[0], addr))

print(f"Function symbols demangled: {sum(len(v) for v in functions.values())}")
print(f"Variable symbols demangled: {sum(len(v) for v in vars_by_module.values())}")
print(f"Modules (from symbols): {len(functions) | len(vars_by_module)}")

# ---------- 4. write project tree ----------
os.makedirs(OUTROOT, exist_ok=True)

def module_rel(module: str) -> str:
    rel = module.replace("res://", "").strip("/")
    return rel if rel.endswith(".nim") else rel + ".nim"

all_modules = set(functions) | set(vars_by_module)
# add string-table modules that have no symbols (empty stubs)
for raw in modules_raw:
    m = raw[2:-6] if raw.startswith("@m") else raw[:-6]
    m = m.replace("@s", "/").replace("@p", "_")
    all_modules.add(m + ".nim")

for module in sorted(all_modules):
    rel = module_rel(module)
    outpath = os.path.join(OUTROOT, rel + ".stub")
    os.makedirs(os.path.dirname(outpath), exist_ok=True)
    with open(outpath, "w", encoding="utf-8") as f:
        f.write(f"# Recovered symbol stub for: {module}\n")
        f.write(f"# (from Turing Complete.exe COFF symbol table, Nim 2.x mangling)\n\n")
        for proc, addr in sorted(functions.get(module, set())):
            f.write(f"proc {proc}()  # {addr}\n")
        for var, addr in sorted(vars_by_module.get(module, set())):
            f.write(f"var {var}    # {addr}\n")

with open(os.path.join(OUTROOT, "_TREE.txt"), "w", encoding="utf-8") as f:
    for module in sorted(all_modules):
        nprocs = len(functions.get(module, set()))
        nvars = len(vars_by_module.get(module, set()))
        f.write(f"{module}\t{nprocs} procs, {nvars} vars\n")

with open(os.path.join(WORK, "unmatched_symbols.txt"), "w", encoding="utf-8") as f:
    for name, addr in unmatched:
        f.write(f"{name}\t{addr}\n")

print(f"\nTop game modules:")
game_mods = [(m, len(v)) for m, v in functions.items() if not m.startswith("std") and "system" not in m.split("/")[0]]
for m, cnt in sorted(game_mods, key=lambda x: -x[1])[:15]:
    print(f"  {cnt:5d} procs  {m}")
print(f"\nStub tree written to {OUTROOT}")
