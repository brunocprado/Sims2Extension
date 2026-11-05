#!/usr/bin/env python3
"""
Part 2 of Deep Subsystem Disassembly for Sims2EP9.exe (v1.0.0.153):
- cTSGlobals::Init (0xa0dc7c80..0xa0dc8360) -> Global pointer & all service offsets
- cTSSimulator core methods (0xa0480770..0xa0483640) -> Tick, PRNG, Speed, SimTime
- SimAntics BHAV Primitive Table & cEdithObject / cTSTreeSim (0xa046a800..0xa046d400, 0xa0576400..0xa0579600)
- cTSInteractionContainer & cTSInteraction (vt=0xa0f7751c, vt=0xa0f773d4)
- cTSMotives (vt=0xa0f73b3c) & cTSPerson / cTSObject ID/GUID/Position getters
- cTSCheatSystem (vt=0xa108f264) & Cheat registration
- Single-instance check (0xa0e06d50..0xa0e06e50 & FindWindowA xrefs)
"""

import json
import struct
import subprocess
from pathlib import Path


EXE_PATH = "/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/Sims2EP9.exe"
VTS_PATH = "/home/bruno/PROJETOS/TS/docs/re_all_vtables.json"
DUMP_PATH = "/home/bruno/PROJETOS/TS/docs/re_analysis_dump.json"


def disasm_range(start_vma: int, stop_vma: int) -> str:
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


def main():
    vts = json.loads(Path(VTS_PATH).read_text(encoding="utf-8"))
    dump = json.loads(Path(DUMP_PATH).read_text(encoding="utf-8"))
    exe_bytes = Path(EXE_PATH).read_bytes()

    out = []

    out.append("=================================================================")
    out.append("1. cTSGlobals Constructor & Init (0xa0dc7c60 .. 0xa0dc8360)")
    out.append("=================================================================")
    out.append(disasm_range(0xa0dc7c60, 0xa0dc8360))

    out.append("\n=================================================================")
    out.append("2. cTSSimulator Methods (0xa0480770 .. 0xa0483660)")
    out.append("=================================================================")
    out.append(disasm_range(0xa0480770, 0xa0483660))

    out.append("\n=================================================================")
    out.append("3. cEdithObject VTables & Methods (vt=0xa0f869c0)")
    out.append("=================================================================")
    for eo_vt in find_vtables_by_class(vts, "cEdithObject"):
        out.append(f"cEdithObject VTable {eo_vt['vtable_vma']} (offset {eo_vt['vtable_offset']}, {eo_vt['method_count']} methods):")
        for idx, m_hex in enumerate(eo_vt["methods"]):
            m_vma = int(m_hex, 16)
            asm = disasm_range(m_vma, m_vma + 64)
            first_lines = " | ".join(l.split("\t")[-1].strip() for l in asm.splitlines()[:6] if "\t" in l)
            out.append(f"  [{idx:02d}] {m_hex}: {first_lines}")

    out.append("\n=================================================================")
    out.append("4. SimAntics BHAV Primitive Registration & Dispatch (0xa046a800 .. 0xa046c000)")
    out.append("=================================================================")
    out.append(disasm_range(0xa046a800, 0xa046c000))

    out.append("\n=================================================================")
    out.append("5. cTSTreeSim Execution Loop (0xa0576400 .. 0xa0578b00)")
    out.append("=================================================================")
    out.append(disasm_range(0xa0576400, 0xa0578b00))

    out.append("\n=================================================================")
    out.append("6. cTSInteractionContainer (vt=0xa0f7751c) & cTSMotives (vt=0xa0f73b3c) & cTSCheatSystem (vt=0xa108f264)")
    out.append("=================================================================")
    for cname in ["cTSInteractionContainer", "cTSMotives", "cTSCheatSystem", "cTSWinCheatConsole", "cLoadLotCheat", "cTSSaveLotController", "cTSLoadLotController"]:
        for cvt in find_vtables_by_class(vts, cname):
            out.append(f"{cname} VTable {cvt['vtable_vma']} (offset {cvt['vtable_offset']}, {cvt['method_count']} methods):")
            for idx, m_hex in enumerate(cvt["methods"]):
                m_vma = int(m_hex, 16)
                asm = disasm_range(m_vma, m_vma + 80)
                first_lines = " | ".join(l.split("\t")[-1].strip() for l in asm.splitlines()[:8] if "\t" in l)
                out.append(f"  [{idx:02d}] {m_hex}: {first_lines}")

    out.append("\n=================================================================")
    out.append("7. Single-Instance / App Mutex (0xa0e06d40 .. 0xa0e06e60 & 0xa0143480 .. 0xa0143b00)")
    out.append("=================================================================")
    out.append(disasm_range(0xa0e06d40, 0xa0e06e60))
    out.append("\n--- cGZFramework Main Loop & Init (0xa0143480 .. 0xa0143b00) ---")
    out.append(disasm_range(0xa0143480, 0xa0143b00))

    Path("/home/bruno/PROJETOS/TS/docs/re_deep_subsystems_part2.txt").write_text("\n".join(out), encoding="utf-8")
    print("Wrote /home/bruno/PROJETOS/TS/docs/re_deep_subsystems_part2.txt")


if __name__ == "__main__":
    main()
