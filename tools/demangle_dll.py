"""Demangle symbols from compile.dll (Nim-written compiler backend)."""
import re
import os
import subprocess
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))

import tc_common  # noqa: E402
from nim_demangle import decode_proc_name, demangle_module  # noqa: E402

# compile.dll contains: compiler modules under OO/, plus Nim stdlib.
# Allow only those; reject linker/CRT junk symbols.
STD_ROOTS = ("system", "system/", "std/", "pure/", "windows/", "io/", "posix/", "fenv/")

def demangle_symbol_loose(sym: str):
    if "__" not in sym:
        return None
    proc, _, mod = sym.partition("__")
    if not mod:
        return None
    proc = decode_proc_name(proc)
    if not proc:
        proc = "<module-init>"
    module = demangle_module(mod).lstrip("_")
    if module.startswith("OO/") or module == "OO":
        pass  # compiler project root
    elif any(module.startswith(r) for r in STD_ROOTS):
        pass  # Nim stdlib
    else:
        return None
    return proc, module

DLL = tc_common.game_dll()
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # 仓库根（tools/ 的父）
OUTROOT = os.path.join(_ROOT, "recovered", "compiler-src")
WORK = os.path.join(_ROOT, "work")

out = subprocess.run(
    [tc_common.tool("nm"), DLL],
    capture_output=True, text=True, errors="replace",
).stdout

functions = defaultdict(set)
vars_by_module = defaultdict(set)
unmatched = []

for line in out.splitlines():
    parts = line.split()
    if len(parts) < 3:
        continue
    addr, typ, name = parts[0], parts[1], parts[2]
    if name.startswith(("NTI", "__imp", "__mingw", ".refptr")):
        continue
    if typ in "Tt":
        d = demangle_symbol_loose(name)
        if d:
            functions[d[1]].add((d[0], addr))
        else:
            unmatched.append((name, addr))
    elif typ in "DdRrBb":
        d = demangle_symbol_loose(name)
        if d:
            vars_by_module[d[1]].add((d[0], addr))

print(f"compile.dll: {sum(len(v) for v in functions.values())} procs, "
      f"{len(functions)} modules")

os.makedirs(OUTROOT, exist_ok=True)
for module in sorted(set(functions) | set(vars_by_module)):
    rel = module.replace("res://", "").strip("/")
    if not rel.endswith(".nim"):
        rel += ".nim"
    outpath = os.path.join(OUTROOT, rel + ".stub")
    os.makedirs(os.path.dirname(outpath), exist_ok=True)
    with open(outpath, "w", encoding="utf-8") as f:
        f.write(f"# Recovered symbol stub for: {module}\n")
        f.write(f"# (from compile.dll COFF symbol table, Nim mangling)\n\n")
        for proc, addr in sorted(functions.get(module, set())):
            f.write(f"proc {proc}()  # {addr}\n")
        for var, addr in sorted(vars_by_module.get(module, set())):
            f.write(f"var {var}    # {addr}\n")

with open(os.path.join(OUTROOT, "_TREE.txt"), "w", encoding="utf-8") as f:
    for module in sorted(set(functions) | set(vars_by_module)):
        f.write(f"{module}\t{len(functions.get(module, set()))} procs\n")

print("\ncompiler modules:")
for m, procs in sorted(functions.items(), key=lambda x: -len(x[1]))[:20]:
    print(f"  {len(procs):5d} procs  {m}")
