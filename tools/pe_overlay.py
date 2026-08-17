"""Parse PE structure and dump overlay data of Turing Complete.exe"""
import struct
import sys
import os

EXE = r"F:\opt\SteamLibrary\steamapps\common\Turing Complete\Turing Complete.exe"

with open(EXE, "rb") as f:
    data = f.read()

file_size = len(data)
print(f"File size: {file_size} (0x{file_size:X})")

# DOS header
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
print(f"PE header offset: 0x{e_lfanew:X}")

# PE header
sig = data[e_lfanew:e_lfanew+4]
print(f"PE signature: {sig}")
coff = e_lfanew + 4
machine, num_sections, timestamp, symtab_off, num_syms, opt_size, characteristics = struct.unpack_from("<HHIIIHH", data, coff)
print(f"Sections: {num_sections}, Symbol table off: 0x{symtab_off:X}, Num symbols: {num_syms}")

opt = coff + 20
magic = struct.unpack_from("<H", data, opt)[0]
print(f"Optional header magic: 0x{magic:X} {'(PE32+)' if magic == 0x20b else '(PE32)'}")

sectab = opt + opt_size
print("\n--- Sections ---")
end_of_sections = 0
for i in range(num_sections):
    off = sectab + i * 40
    name = data[off:off+8].rstrip(b"\x00").decode("ascii", "replace")
    vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
    end = rawptr + max(rawsize, 0)
    end_of_sections = max(end_of_sections, end)
    print(f"  {name:12s} vaddr=0x{vaddr:08X} vsize=0x{vsize:08X} rawptr=0x{rawptr:08X} rawsize=0x{rawsize:08X} end=0x{end:X}")

print(f"\nEnd of last section: 0x{end_of_sections:X}")
overlay_off = end_of_sections
overlay_size = file_size - overlay_off
print(f"Overlay: offset 0x{overlay_off:X}, size 0x{overlay_size:X} ({overlay_size} bytes)")

if overlay_size > 0:
    overlay = data[overlay_off:]
    print(f"\n--- Overlay head (first 128 bytes) ---")
    print(overlay[:128].hex(" "))
    print(f"\n--- Overlay as ASCII (first 256) ---")
    print(overlay[:256])
    # save overlay
    out = r"D:\home\code-proj\Python\turing-complete\work\overlay.bin"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(overlay)
    print(f"\nOverlay saved to {out}")
