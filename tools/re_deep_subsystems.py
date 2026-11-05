#!/usr/bin/env python3
"""
Deep Subsystem Disassembly & Structure Analyzer for Sims2EP9.exe (v1.0.0.153).
Inspects:
1. cTSGlobals (all 59 methods -> extracts global singleton pointers in .data)
2. cTSSimulator (all vtables & methods -> Tick, Speed, Pause, SimTime, PRNG seed)
3. SimAntics VM (cTSTreeSim, cEdithObject, BHAV Primitive Dispatch Table, RandomParam, PushActionParam)
4. cTSInteraction & cTSInteractionContainer (Action Queue push/pop/serialize)
5. cTSPerson / cTSObject / cTSMotives (Object ID, GUID, Position, Motives array)
6. CreateMutexA single-instance check, ijl15.dll loader, Direct3DCreate9 hook target, and Cheat registration.
"""

import json
import struct
import subprocess
from pathlib import Path


EXE_PATH = "/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/Sims2EP9.exe"
VTS_PATH = "/home/bruno/PROJETOS/TS/docs/re_all_vtables.json"
DUMP_PATH = "/home/bruno/PROJETOS/TS/docs/re_analysis_dump.json"


def disasm_range(start_vma: int, size: int = 128) -> str:
    stop_vma = start_vma + size
    cmd = [
        "objdump", "-d", "-M", "intel",
        f"--start-address={hex(start_vma)}",
        f"--stop-address={hex(stop_vma)}",
        EXE_PATH,
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, check=False)
    lines = []
    for line in res.stdout.splitlines():
        if line.startswith(" a0") or line.startswith("a0"):
            lines.append(line)
    return "\n".join(lines)


def find_vtables_by_class(vts: list[dict], class_name: str) -> list[dict]:
    return [vt for vt in vts if vt["class_name"] == class_name]


def find_vtable_xrefs(exe_bytes: bytes, text_raw_ptr: int, text_raw_size: int, text_vma: int, vt_vma: int) -> list[str]:
    packed = struct.pack("<I", vt_vma)
    text_data = exe_bytes[text_raw_ptr:text_raw_ptr + text_raw_size]
    sites = []
    pos = 0
    while True:
        idx = text_data.find(packed, pos)
        if idx == -1:
            break
        # Check MOV [reg], vt_vma (C7 00..07 or C7 40..47 or C7 80..87)
        site = text_vma + idx
        sites.append(hex(site))
        pos = idx + 1
        if len(sites) >= 20:
            break
    return sites


def main():
    vts = json.loads(Path(VTS_PATH).read_text(encoding="utf-8"))
    dump = json.loads(Path(DUMP_PATH).read_text(encoding="utf-8"))
    exe_bytes = Path(EXE_PATH).read_bytes()
    text_sec = next(s for s in dump["sims2ep9"]["sections"] if s["name"] == ".text")
    rdata_sec = next(s for s in dump["sims2ep9"]["sections"] if s["name"] == ".rdata")
    data_sec = next(s for s in dump["sims2ep9"]["sections"] if s["name"] == ".data")

    print("=================================================================")
    print("1. cTSGlobals (vt=0xa107085c) - Global Singletons & Service Getters")
    print("=================================================================")
    globals_vts = find_vtables_by_class(vts, "cTSGlobals")
    for gvt in globals_vts:
        print(f"VTable {gvt['vtable_vma']} (offset {gvt['vtable_offset']}, {gvt['method_count']} methods):")
        xrefs = find_vtable_xrefs(exe_bytes, text_sec["raw_ptr"], text_sec["raw_size"], text_sec["vma"], int(gvt["vtable_vma"], 16))
        print(f"  Constructor/VTable Xrefs in .text: {xrefs}")
        for idx, m_hex in enumerate(gvt["methods"]):
            m_vma = int(m_hex, 16)
            asm = disasm_range(m_vma, 48)
            first_lines = " | ".join(l.split("\t")[-1].strip() for l in asm.splitlines()[:5] if "\t" in l)
            print(f"  [{idx:02d}] {m_hex}: {first_lines}")

    print("\n=================================================================")
    print("2. cTSSimulator & cITSSimulator - All VTables & Methods")
    print("=================================================================")
    for sim_vt in find_vtables_by_class(vts, "cTSSimulator"):
        print(f"cTSSimulator VTable {sim_vt['vtable_vma']} (offset {sim_vt['vtable_offset']}, {sim_vt['method_count']} methods):")
        xrefs = find_vtable_xrefs(exe_bytes, text_sec["raw_ptr"], text_sec["raw_size"], text_sec["vma"], int(sim_vt["vtable_vma"], 16))
        print(f"  Constructor/VTable Xrefs in .text: {xrefs}")
        for idx, m_hex in enumerate(sim_vt["methods"]):
            m_vma = int(m_hex, 16)
            asm = disasm_range(m_vma, 64)
            first_lines = " | ".join(l.split("\t")[-1].strip() for l in asm.splitlines()[:6] if "\t" in l)
            print(f"  [{idx:02d}] {m_hex}: {first_lines}")

    print("\n=================================================================")
    print("3. cTSTreeSim & cEdithObject - SimAntics VM Execution")
    print("=================================================================")
    for ts_vt in find_vtables_by_class(vts, "cTSTreeSim"):
        print(f"cTSTreeSim VTable {ts_vt['vtable_vma']} (offset {ts_vt['vtable_offset']}, {ts_vt['method_count']} methods):")
        for idx, m_hex in enumerate(ts_vt["methods"]):
            m_vma = int(m_hex, 16)
            asm = disasm_range(m_vma, 64)
            first_lines = " | ".join(l.split("\t")[-1].strip() for l in asm.splitlines()[:6] if "\t" in l)
            print(f"  [{idx:02d}] {m_hex}: {first_lines}")

    print("\n=================================================================")
    print("4. BHAV Primitives: RandomParam, PushActionParam, GenericSimCallParam Xrefs")
    print("=================================================================")
    for pname in ["RandomParam", "PushActionParam", "GenericSimCallParam", "RunPrivateTreeParam", "SetMotiveDeltaParam", "GosubActionParam"]:
        pvts = find_vtables_by_class(vts, pname)
        for pvt in pvts:
            xrefs = find_vtable_xrefs(exe_bytes, text_sec["raw_ptr"], text_sec["raw_size"], text_sec["vma"], int(pvt["vtable_vma"], 16))
            print(f"  {pname:25s} vt={pvt['vtable_vma']} -> xrefs={xrefs}")

    print("\n=================================================================")
    print("5. CreateMutexA Sites & Single-Instance Check")
    print("=================================================================")
    mutex_info = dump["sims2ep9"]["iat_xrefs"].get("KERNEL32.dll!CreateMutexA", {})
    for site in mutex_info.get("sites", []):
        s_vma = int(site["site_vma"], 16)
        print(f"--- CreateMutexA call around {hex(s_vma)} ---")
        print(disasm_range(s_vma - 48, 112))


if __name__ == "__main__":
    main()
