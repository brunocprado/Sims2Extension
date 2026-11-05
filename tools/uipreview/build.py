#!/usr/bin/env python3
"""Builds build/uipreview.exe: a D3D9 window that renders the panel with the real ui_kit (run via preview.sh)."""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build", "uipreview")
os.makedirs(BUILD, exist_ok=True)

DEFS = {
    "kernel32": "LIBRARY KERNEL32.dll\nEXPORTS\n" + "\n".join(f"    {n}" for n in [
        "LoadLibraryA@4", "GetModuleHandleA@4", "GetProcAddress@8", "CreateFileA@28", "ReadFile@20", "CloseHandle@4",
        "Sleep@4", "OutputDebugStringA@4", "GetTickCount@0", "ExitProcess@4"]) + "\n",
    "user32": "LIBRARY USER32.dll\nEXPORTS\n" + "\n".join(f"    {n}" for n in [
        "RegisterClassA@4", "CreateWindowExA@48", "ShowWindow@8", "PeekMessageA@20", "TranslateMessage@4",
        "DispatchMessageA@4", "DefWindowProcA@16", "AdjustWindowRect@12"]) + "\n",
    "gdi32": "LIBRARY GDI32.dll\nEXPORTS\n" + "\n".join(f"    {n}" for n in [
        "CreateCompatibleDC@4", "DeleteDC@4", "CreateDIBSection@24", "SelectObject@8", "DeleteObject@4", "CreateFontW@56",
        "SetTextColor@8", "SetBkMode@8", "TextOutW@20", "GetTextExtentPoint32W@16", "GetTextMetricsW@8", "GdiFlush@0"]) + "\n",
    "msvcrt": "LIBRARY msvcrt.dll\nEXPORTS\n" + "\n".join(f"    {n}" for n in [
        "_snprintf", "memcpy", "memset", "strlen", "strncmp", "strtol", "sin", "cos", "sqrt"]) + "\n",
}


def run(cmd):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd)


libs = []
for name, text in DEFS.items():
    d = os.path.join(BUILD, name + ".def")
    l = os.path.join(BUILD, name + ".lib")
    open(d, "w").write(text)
    run(["llvm-dlltool", "-m", "i386", "-k", "-d", d, "-l", l])
    libs.append(l)

objs = []
for src in ["tools/uipreview/preview.cpp", "src/ui_kit.cpp", "src/ui_graphics_tab.cpp"]:
    obj = os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".obj")
    run(["clang++", "--target=i686-pc-windows-msvc", "-ffreestanding", "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti",
         "-fno-threadsafe-statics", "-mno-sse2", "-I", os.path.join(ROOT, "include"), "-c", os.path.join(ROOT, src), "-o", obj])
    objs.append(obj)

out = os.path.join(ROOT, "build", "uipreview.exe")
run(["lld-link", "/MACHINE:X86", "/NODEFAULTLIB", "/SUBSYSTEM:WINDOWS", "/ENTRY:WinMainCRTStartup", f"/OUT:{out}", *objs, *libs])
print("built", out)
