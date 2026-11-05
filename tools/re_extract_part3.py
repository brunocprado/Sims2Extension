#!/usr/bin/env python3
import struct
import subprocess
import json

EXE = "/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/Sims2EP9.exe"
with open(EXE, "rb") as f:
    data = f.read()

IMAGE_BASE = 0xA0000000
pe_off = struct.unpack_from("<I", data, 0x3C)[0]
num_sections = struct.unpack_from("<H", data, pe_off + 6)[0]
opt_hdr_size = struct.unpack_from("<H", data, pe_off + 20)[0]
sec_table = pe_off + 24 + opt_hdr_size

sections = []
for i in range(num_sections):
    s = sec_table + i * 40
    name = data[s:s+8].rstrip(b"\x00").decode("ascii", errors="ignore")
    vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
    sections.append((name, IMAGE_BASE + vaddr, vsize, rawptr, rawsize))

def va_to_off(va):
    for name, vstart, vsize, rawptr, rawsize in sections:
        if vstart <= va < vstart + max(vsize, rawsize):
            return rawptr + (va - vstart)
    return None

def read_u32(va):
    off = va_to_off(va)
    return struct.unpack_from("<I", data, off)[0]

def disasm(start_va, stop_va):
    cmd = ["objdump", "-d", "-M", "intel", f"--start-address={ hex(start_va) }", f"--stop-address={ hex(stop_va) }", EXE]
    return subprocess.check_output(cmd, text=True)

with open("/home/bruno/PROJETOS/TS/docs/re_all_vtables.json") as f:
    vts = json.load(f)

out = []

def dump_class_vt(cls_name, va, max_methods=120):
    out.append(f"=== {cls_name} VTable {hex(va)} ===")
    for i in range(max_methods):
        m_va = read_u32(va + i * 4)
        if not (0xA0001000 <= m_va < 0xA0F48000):
            break
        d = disasm(m_va, m_va + 48)
        insns = []
        for line in d.splitlines():
            if line.startswith(" a0"):
                parts = line.split("\t")
                if len(parts) >= 3:
                    insns.append(parts[2].strip())
        out.append(f"  [{i:03d}] (+{hex(i*4)}) {hex(m_va)}: {' | '.join(insns[:6])}")

dump_class_vt("cTSObject", 0xa0f81f9c, 260)
dump_class_vt("cTSPerson", 0xa0f83aa0, 260)
dump_class_vt("cTSInteraction", 0xa0f76aa4, 90)

# Check cTSObject vtable[6] (ExecutePrimitive!)
cTSObject_vt = 0xa0f81f9c
exec_prim_va = read_u32(cTSObject_vt + 6 * 4)
out.append(f"\n=== cTSObject::vtable[6] (ExecutePrimitive) @ {hex(exec_prim_va)} ===")
out.append(disasm(exec_prim_va, exec_prim_va + 0x450))

# Check cTSInteractionContainer::PushInteraction / AddInteraction (methods [03] 0xa044f9b0 and [06] 0xa0452070)
out.append("\n=== cTSInteractionContainer::[03] 0xa044f9b0 & [06] 0xa0452070 ===")
out.append(disasm(0xa044f9b0, 0xa044fd00))
out.append(disasm(0xa0452070, 0xa0452400))

# Check cTSCheatSystem::ParseCommand / RegisterCheat (0xa0ca1c20, 0xa0ca1450, 0xa0e587a0, 0xa0e589b0)
out.append("\n=== cTSCheatSystem methods (0xa0ca1c20, 0xa0ca1450, 0xa0e587a0, 0xa0e589b0) ===")
out.append(disasm(0xa0ca1c20, 0xa0ca1eb0))
out.append(disasm(0xa0ca1450, 0xa0ca1670))
out.append(disasm(0xa0e587a0, 0xa0e58b70))

# Find all xrefs to CreateMutexA (0xa0f493ac) and FindWindowA in .text
out.append("\n=== Xrefs to CreateMutexA (0xa0f493ac) and Single-Instance Check ===")
text_off = va_to_off(0xA0001000)
text_data = data[text_off:text_off+0x00f471ee]
# Search for b7 00 00 00 (ERROR_ALREADY_EXISTS = 183 = 0xB7) after GetLastError (0xa0f4924c)
get_last_error_iat = struct.pack("<I", 0xa0f4924c)
idx = 0
while True:
    p = text_data.find(b"\xff\x15" + get_last_error_iat, idx)
    if p == -1:
        break
    va = 0xA0001000 + p
    # check if 0xb7 is nearby
    chunk = text_data[p:p+64]
    if b"\xb7\x00\x00\x00" in chunk or b"\x3d\xb7" in chunk:
        out.append(f"Found GetLastError + ERROR_ALREADY_EXISTS at {hex(va)}:")
        out.append(disasm(va - 0x60, va + 0x80))
    idx = p + 1

# Also check callers of 0xa0e06dc8 (cRZSysMutex ctor)
target = 0xa0e06dc8
idx = 0
while True:
    p = text_data.find(b"\xe8", idx)
    if p == -1:
        break
    rel = struct.unpack_from("<i", text_data, p + 1)[0]
    call_va = 0xA0001000 + p
    if call_va + 5 + rel == target:
        out.append(f"Caller of cRZSysMutex ctor (0xa0e06dc8) at {hex(call_va)}:")
        out.append(disasm(call_va - 0x40, call_va + 0x60))
    idx = p + 1

with open("/home/bruno/PROJETOS/TS/docs/re_deep_subsystems_part3.txt", "w") as f:
    f.write("\n".join(out))
print("Wrote part3, lines:", len(out))
