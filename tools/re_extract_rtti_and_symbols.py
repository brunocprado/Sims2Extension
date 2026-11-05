#!/usr/bin/env python3
"""
Static PE32, IAT/EAT, MSVC RTTI, VTable, and Cross-Reference Extractor for The Sims 2 (Sims2EP9.exe & ijl15.dll).
Produces structured JSON and Markdown symbol tables for TS2SE & Co-op Multiplayer.
"""

import struct
import json
import re
import sys
import hashlib
from pathlib import Path
from collections import defaultdict


class PEImage:
    def __init__(self, path: str):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        self._parse_pe()

    def _parse_pe(self):
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        sig = self.data[e_lfanew:e_lfanew + 4]
        assert sig == b"PE\x00\x00", f"Invalid PE signature: {sig}"

        coff_off = e_lfanew + 4
        machine, num_sections, timedatestamp, _, _, opt_hdr_size, characteristics = struct.unpack_from(
            "<HHIIIHH", self.data, coff_off
        )
        self.machine = machine
        self.timedatestamp = timedatestamp

        opt_off = coff_off + 20
        magic = struct.unpack_from("<H", self.data, opt_off)[0]
        assert magic == 0x10B, f"Expected PE32 (0x10B), got {hex(magic)}"

        self.entry_rva = struct.unpack_from("<I", self.data, opt_off + 16)[0]
        self.image_base = struct.unpack_from("<I", self.data, opt_off + 28)[0]
        self.size_of_image = struct.unpack_from("<I", self.data, opt_off + 56)[0]
        num_rva_and_sizes = struct.unpack_from("<I", self.data, opt_off + 92)[0]

        self.data_dirs = []
        dd_off = opt_off + 96
        for i in range(num_rva_and_sizes):
            rva, size = struct.unpack_from("<II", self.data, dd_off + i * 8)
            self.data_dirs.append((rva, size))

        self.sections = []
        sec_off = opt_off + opt_hdr_size
        for i in range(num_sections):
            name_raw, vsize, vrva, raw_size, raw_ptr, _, _, _, _, flags = struct.unpack_from(
                "<8sIIIIIIHHI", self.data, sec_off + i * 40
            )
            name = name_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")
            self.sections.append({
                "name": name,
                "vsize": vsize,
                "vrva": vrva,
                "vma": self.image_base + vrva,
                "raw_size": raw_size,
                "raw_ptr": raw_ptr,
                "flags": flags,
            })

    def rva_to_offset(self, rva: int) -> int | None:
        for s in self.sections:
            max_sz = max(s["vsize"], s["raw_size"])
            if s["vrva"] <= rva < s["vrva"] + max_sz:
                off = rva - s["vrva"]
                if off < s["raw_size"]:
                    return s["raw_ptr"] + off
                return None
        return None

    def vma_to_offset(self, vma: int) -> int | None:
        if vma < self.image_base:
            return None
        return self.rva_to_offset(vma - self.image_base)

    def offset_to_vma(self, offset: int) -> int | None:
        for s in self.sections:
            if s["raw_ptr"] <= offset < s["raw_ptr"] + s["raw_size"]:
                return self.image_base + s["vrva"] + (offset - s["raw_ptr"])
        return None

    def read_u32_vma(self, vma: int) -> int | None:
        off = self.vma_to_offset(vma)
        if off is None or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]

    def read_cstring_off(self, off: int, max_len: int = 512) -> str:
        end = self.data.find(b"\x00", off, off + max_len)
        if end == -1:
            end = off + max_len
        return self.data[off:end].decode("latin1", errors="replace")

    def parse_exports(self) -> list[dict]:
        if not self.data_dirs or self.data_dirs[0][0] == 0:
            return []
        exp_rva, exp_size = self.data_dirs[0]
        off = self.rva_to_offset(exp_rva)
        if off is None:
            return []
        (
            _, timedatestamp, _, _, name_rva, ordinal_base,
            num_funcs, num_names, addr_funcs_rva, addr_names_rva, addr_ords_rva
        ) = struct.unpack_from("<IIHHIIIIIII", self.data, off)

        funcs_off = self.rva_to_offset(addr_funcs_rva)
        names_off = self.rva_to_offset(addr_names_rva)
        ords_off = self.rva_to_offset(addr_ords_rva)

        ord_to_name = {}
        for i in range(num_names):
            n_rva = struct.unpack_from("<I", self.data, names_off + i * 4)[0]
            ord_idx = struct.unpack_from("<H", self.data, ords_off + i * 2)[0]
            n_off = self.rva_to_offset(n_rva)
            ord_to_name[ord_idx] = self.read_cstring_off(n_off) if n_off else f"ord_{ord_idx}"

        exports = []
        for i in range(num_funcs):
            f_rva = struct.unpack_from("<I", self.data, funcs_off + i * 4)[0]
            if f_rva == 0:
                continue
            f_off = self.rva_to_offset(f_rva)
            # Check for stdcall ret imm16 in first 128 bytes
            ret_bytes = None
            if f_off is not None:
                chunk = self.data[f_off:f_off + 128]
                # Look for C2 XX 00 (ret imm16) or C3 (ret)
                for idx in range(len(chunk) - 2):
                    if chunk[idx] == 0xC2 and chunk[idx + 2] == 0x00 and chunk[idx + 1] in (4, 8, 12, 16, 20, 24):
                        ret_bytes = chunk[idx + 1]
                        break
                    elif chunk[idx] == 0xC3:
                        ret_bytes = 0
                        break
            exports.append({
                "ordinal": ordinal_base + i,
                "name": ord_to_name.get(i, f"ord_{ordinal_base + i}"),
                "rva": hex(f_rva),
                "vma": hex(self.image_base + f_rva),
                "inferred_stack_cleanup_bytes": ret_bytes,
            })
        return exports

    def parse_imports(self) -> list[dict]:
        if len(self.data_dirs) < 2 or self.data_dirs[1][0] == 0:
            return []
        imp_rva, _ = self.data_dirs[1]
        off = self.rva_to_offset(imp_rva)
        if off is None:
            return []

        imports = []
        while True:
            ilt_rva, ts, fwd, name_rva, iat_rva = struct.unpack_from("<IIIII", self.data, off)
            if ilt_rva == 0 and name_rva == 0 and iat_rva == 0:
                break
            off += 20
            name_off = self.rva_to_offset(name_rva)
            dll_name = self.read_cstring_off(name_off) if name_off else "unknown.dll"

            thunk_rva = ilt_rva if ilt_rva != 0 else iat_rva
            thunk_off = self.rva_to_offset(thunk_rva)
            funcs = []
            idx = 0
            while thunk_off is not None:
                val = struct.unpack_from("<I", self.data, thunk_off + idx * 4)[0]
                if val == 0:
                    break
                slot_vma = self.image_base + iat_rva + idx * 4
                if val & 0x80000000:
                    funcs.append({
                        "ordinal": val & 0xFFFF,
                        "name": f"ord_{val & 0xFFFF}",
                        "iat_vma": hex(slot_vma),
                    })
                else:
                    hint_off = self.rva_to_offset(val)
                    if hint_off is not None:
                        hint = struct.unpack_from("<H", self.data, hint_off)[0]
                        fname = self.read_cstring_off(hint_off + 2)
                    else:
                        hint = 0
                        fname = "unknown"
                    funcs.append({
                        "hint": hint,
                        "name": fname,
                        "iat_vma": hex(slot_vma),
                    })
                idx += 1
            imports.append({
                "dll": dll_name,
                "iat_rva": hex(iat_rva),
                "functions": funcs,
            })
        return imports


def demangle_msvc_type_name(mangled: str) -> str:
    # Convert .?AVcTSSimulator@@ or .?AV?$cGZ... into readable class name
    s = mangled
    if s.startswith(".?AV") or s.startswith(".?AU"):
        s = s[4:]
    if s.endswith("@@"):
        s = s[:-2]
    parts = [p for p in s.split("@") if p]
    if len(parts) > 1 and not s.startswith("?$"):
        return "::".join(reversed(parts))
    return s


def extract_msvc_rtti(pe: PEImage) -> dict:
    """
    Scans .data and .rdata for MSVC 32-bit TypeDescriptors, CompleteObjectLocators,
    ClassHierarchyDescriptors, and VTables.
    """
    text_sec = next(s for s in pe.sections if s["name"] == ".text")
    rdata_sec = next(s for s in pe.sections if s["name"] == ".rdata")
    data_sec = next(s for s in pe.sections if s["name"] == ".data")

    text_start, text_end = text_sec["vma"], text_sec["vma"] + text_sec["vsize"]
    rdata_start, rdata_end = rdata_sec["vma"], rdata_sec["vma"] + rdata_sec["vsize"]
    data_start, data_end = data_sec["vma"], data_sec["vma"] + data_sec["vsize"]

    # 1. Find all TypeDescriptors by scanning for ".?AV" or ".?AU"
    # In 32-bit MSVC, TypeDescriptor layout:
    # +0x00: uint32 pVFTable (points to type_info::`vftable` in .rdata)
    # +0x04: uint32 spare (0)
    # +0x08: char name[] (".?AV..." or ".?AU...")
    type_descriptors = {}  # td_vma -> {mangled, name, vftable}
    for match in re.finditer(rb"\.\?A[VU][A-Za-z0-9_@?$]+\x00", pe.data):
        name_off = match.start()
        td_off = name_off - 8
        if td_off < 0:
            continue
        td_vma = pe.offset_to_vma(td_off)
        if td_vma is None:
            continue
        pvftable, spare = struct.unpack_from("<II", pe.data, td_off)
        if not (rdata_start <= pvftable < rdata_end) or spare != 0:
            continue
        mangled = match.group(0)[:-1].decode("ascii", errors="replace")
        type_descriptors[td_vma] = {
            "td_vma": hex(td_vma),
            "mangled": mangled,
            "class_name": demangle_msvc_type_name(mangled),
        }

    # 2. Build fast lookup of all 4-byte aligned pointers to TypeDescriptors inside .rdata
    # CompleteObjectLocator (COL) layout in 32-bit x86:
    # +0x00: uint32 signature (0)
    # +0x04: uint32 offset (vtable offset in class)
    # +0x08: uint32 cdOffset
    # +0x0C: uint32 pTypeDescriptor (in type_descriptors)
    # +0x10: uint32 pClassDescriptor (in .rdata)
    rdata_bytes = pe.data[rdata_sec["raw_ptr"]:rdata_sec["raw_ptr"] + rdata_sec["raw_size"]]
    cols = {}  # col_vma -> {td_vma, offset, chd_vma, base_classes}

    for off in range(12, len(rdata_bytes) - 8, 4):
        ptd = struct.unpack_from("<I", rdata_bytes, off)[0]
        if ptd in type_descriptors:
            col_rel = off - 12
            sig, voff, cdoff, _, pchd = struct.unpack_from("<IIIII", rdata_bytes, col_rel)
            if sig == 0 and voff < 0x100000 and cdoff < 0x100000 and (rdata_start <= pchd < rdata_end):
                col_vma = rdata_start + col_rel
                # Parse ClassHierarchyDescriptor at pchd
                chd_off = pe.vma_to_offset(pchd)
                bases = []
                if chd_off is not None and chd_off + 16 <= len(pe.data):
                    chd_sig, chd_attr, num_bases, pbca = struct.unpack_from("<IIII", pe.data, chd_off)
                    if chd_sig == 0 and 0 < num_bases <= 64 and (rdata_start <= pbca < rdata_end):
                        bca_off = pe.vma_to_offset(pbca)
                        if bca_off is not None:
                            for b_i in range(num_bases):
                                bcd_ptr = struct.unpack_from("<I", pe.data, bca_off + b_i * 4)[0]
                                bcd_off = pe.vma_to_offset(bcd_ptr)
                                if bcd_off is not None and bcd_off + 24 <= len(pe.data):
                                    b_td = struct.unpack_from("<I", pe.data, bcd_off)[0]
                                    if b_td in type_descriptors:
                                        b_name = type_descriptors[b_td]["class_name"]
                                        if b_name not in bases:
                                            bases.append(b_name)
                cols[col_vma] = {
                    "col_vma": hex(col_vma),
                    "td_vma": ptd,
                    "class_name": type_descriptors[ptd]["class_name"],
                    "mangled": type_descriptors[ptd]["mangled"],
                    "vtable_offset": voff,
                    "chd_vma": hex(pchd),
                    "base_classes": bases,
                }

    # 3. Scan .rdata for VTables!
    # A VTable has vtable[-1] == col_vma (in cols), and vtable[0] in .text!
    vtables = []
    for off in range(0, len(rdata_bytes) - 8, 4):
        val = struct.unpack_from("<I", rdata_bytes, off)[0]
        if val in cols:
            first_fn = struct.unpack_from("<I", rdata_bytes, off + 4)[0]
            if text_start <= first_fn < text_end:
                vt_vma = rdata_start + off + 4
                methods = []
                m_off = off + 4
                while m_off + 4 <= len(rdata_bytes) and len(methods) < 512:
                    fn_ptr = struct.unpack_from("<I", rdata_bytes, m_off)[0]
                    if fn_ptr in cols or not (text_start <= fn_ptr < text_end):
                        break
                    methods.append(hex(fn_ptr))
                    m_off += 4
                col_info = cols[val]
                vtables.append({
                    "class_name": col_info["class_name"],
                    "mangled": col_info["mangled"],
                    "vtable_vma": hex(vt_vma),
                    "vtable_offset": col_info["vtable_offset"],
                    "method_count": len(methods),
                    "methods": methods,
                    "base_classes": col_info["base_classes"],
                })

    return {
        "type_descriptor_count": len(type_descriptors),
        "col_count": len(cols),
        "vtable_count": len(vtables),
        "type_descriptors": list(type_descriptors.values()),
        "vtables": vtables,
    }


def find_string_xrefs(pe: PEImage, patterns: list[str]) -> list[dict]:
    """
    Finds matching strings in .rdata/.data and searches .text for immediate 32-bit VMA references
    (e.g., PUSH imm32, MOV reg/mem, imm32).
    """
    text_sec = next(s for s in pe.sections if s["name"] == ".text")
    text_bytes = pe.data[text_sec["raw_ptr"]:text_sec["raw_ptr"] + text_sec["raw_size"]]
    text_vma_base = text_sec["vma"]

    regex = re.compile("|".join(patterns).encode("latin1"), re.IGNORECASE)
    results = []

    # Scan null-terminated ASCII strings >= 4 chars
    for m in re.finditer(rb"[\x20-\x7e]{4,300}\x00", pe.data):
        s_bytes = m.group(0)[:-1]
        if not regex.search(s_bytes):
            continue
        s_off = m.start()
        s_vma = pe.offset_to_vma(s_off)
        if s_vma is None:
            continue
        # Avoid matching inside .text unless needed
        if text_vma_base <= s_vma < text_vma_base + text_sec["vsize"]:
            continue

        packed_vma = struct.pack("<I", s_vma)
        xrefs = []
        pos = 0
        while True:
            idx = text_bytes.find(packed_vma, pos)
            if idx == -1:
                break
            # Check instruction prefix byte before the 4-byte immediate
            if idx >= 1:
                op1 = text_bytes[idx - 1]
                # 0x68 = PUSH imm32, 0xB8..0xBF = MOV r32, imm32, 0xA1/A3 = MOV EAX, [imm32]
                if op1 in (0x68, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xA1, 0xA3):
                    xrefs.append(hex(text_vma_base + idx - 1))
                elif idx >= 2 and text_bytes[idx - 2] in (0x8B, 0x8D, 0xC7, 0xFF, 0x3B, 0x83):
                    xrefs.append(hex(text_vma_base + idx - 2))
                else:
                    xrefs.append(hex(text_vma_base + idx))
            pos = idx + 1
            if len(xrefs) >= 25:
                break

        results.append({
            "string": s_bytes.decode("latin1", errors="replace"),
            "vma": hex(s_vma),
            "xref_count": len(xrefs),
            "xrefs": xrefs,
        })

    return results


def find_iat_xrefs(pe: PEImage, target_apis: set[str]) -> dict[str, list[str]]:
    """
    Finds CALL [iat_vma] (FF 15 <vma>) and JMP [iat_vma] (FF 25 <vma>) or MOV reg, [iat_vma]
    in .text for selected Win32 / DirectX / Socket APIs.
    """
    text_sec = next(s for s in pe.sections if s["name"] == ".text")
    text_bytes = pe.data[text_sec["raw_ptr"]:text_sec["raw_ptr"] + text_sec["raw_size"]]
    text_vma_base = text_sec["vma"]

    api_xrefs = {}
    for dll_entry in pe.parse_imports():
        for fn in dll_entry["functions"]:
            if fn["name"] in target_apis:
                iat_vma = int(fn["iat_vma"], 16)
                packed = struct.pack("<I", iat_vma)
                sites = []
                pos = 0
                while True:
                    idx = text_bytes.find(packed, pos)
                    if idx == -1:
                        break
                    if idx >= 2 and text_bytes[idx - 2] == 0xFF and text_bytes[idx - 1] in (0x15, 0x25):
                        sites.append({
                            "site_vma": hex(text_vma_base + idx - 2),
                            "kind": "CALL" if text_bytes[idx - 1] == 0x15 else "JMP",
                        })
                    elif idx >= 2 and text_bytes[idx - 2] == 0x8B:
                        sites.append({
                            "site_vma": hex(text_vma_base + idx - 2),
                            "kind": "MOV_REG",
                        })
                    pos = idx + 1
                api_xrefs[f"{dll_entry['dll']}!{fn['name']}"] = {
                    "iat_vma": fn["iat_vma"],
                    "sites": sites,
                }
    return api_xrefs


def main():
    exe_path = "/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/Sims2EP9.exe"
    ijl_path = "/mnt/NVME-JOGOS/The Sims 2/EP9/TSBin/ijl15.dll"
    out_dir = Path("/home/bruno/PROJETOS/TS/docs")
    out_dir.mkdir(parents=True, exist_ok=True)

    print("[*] Parsing ijl15.dll...")
    ijl_pe = PEImage(ijl_path)
    ijl_exports = ijl_pe.parse_exports()

    print("[*] Parsing Sims2EP9.exe...")
    exe_pe = PEImage(exe_path)
    exe_imports = exe_pe.parse_imports()
    exe_exports = exe_pe.parse_exports()

    print("[*] Extracting MSVC C++ RTTI and VTables from Sims2EP9.exe...")
    rtti = extract_msvc_rtti(exe_pe)
    print(f"    -> TypeDescriptors: {rtti['type_descriptor_count']}")
    print(f"    -> CompleteObjectLocators: {rtti['col_count']}")
    print(f"    -> VTables recovered: {rtti['vtable_count']}")

    target_apis = {
        "CreateMutexA", "CreateMutexW", "OpenMutexA",
        "Direct3DCreate9",
        "CreateWindowExA", "CreateWindowExW",
        "PeekMessageA", "GetMessageA", "DispatchMessageA",
        "QueryPerformanceCounter", "timeGetTime", "Sleep",
        "ijlInit", "ijlFree", "ijlRead", "ijlWrite", "ijlGetLibVersion", "ijlErrorStr",
        "WSAStartup", "socket", "connect", "send", "recv",
    }
    print("[*] Scanning IAT cross-references in .text...")
    iat_xrefs = find_iat_xrefs(exe_pe, target_apis)

    patterns = [
        r"simInBackground",
        r"sleepInBackground",
        r"maxTotalSims",
        r"maxTotalHumans",
        r"maxTotalPets",
        r"freeWill",
        r"loadLot",
        r"loadNeighborhood",
        r"cTSSimulator",
        r"cEdith",
        r"cGZCommandParser",
        r"cGZFramework",
        r"Primitive",
        r"Random",
        r"Interaction",
        r"PieMenu",
        r"SaveLot",
        r"boolProp",
        r"intProp",
        r"floatProp",
        r"uintProp",
        r"The Sims 2",
    ]
    print("[*] Scanning string cross-references in .text...")
    str_xrefs = find_string_xrefs(exe_pe, patterns)
    print(f"    -> String matches with xrefs: {len(str_xrefs)}")

    report = {
        "sims2ep9": {
            "path": exe_path,
            "sha256": exe_pe.sha256,
            "image_base": hex(exe_pe.image_base),
            "entry_vma": hex(exe_pe.image_base + exe_pe.entry_rva),
            "sections": exe_pe.sections,
            "imports": exe_imports,
            "exports": exe_exports,
            "iat_xrefs": iat_xrefs,
            "rtti_summary": {
                "type_descriptor_count": rtti["type_descriptor_count"],
                "col_count": rtti["col_count"],
                "vtable_count": rtti["vtable_count"],
            },
        },
        "ijl15": {
            "path": ijl_path,
            "sha256": ijl_pe.sha256,
            "image_base": hex(ijl_pe.image_base),
            "exports": ijl_exports,
        },
        "key_vtables": [
            vt for vt in rtti["vtables"]
            if any(k in vt["class_name"].lower() for k in (
                "simulator", "edith", "commandparser", "messageserver", "framework",
                "person", "sim", "lot", "neighborhood", "interaction", "piemenu",
                "persist", "propertyset", "catalog", "object", "camera", "app", "cheat"
            ))
        ],
        "string_xrefs": str_xrefs,
    }

    (out_dir / "re_analysis_dump.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (out_dir / "re_all_vtables.json").write_text(json.dumps(rtti["vtables"], indent=2), encoding="utf-8")
    print(f"[+] Wrote {out_dir / 're_analysis_dump.json'} and {out_dir / 're_all_vtables.json'}")


if __name__ == "__main__":
    main()
