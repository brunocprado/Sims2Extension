#!/usr/bin/env python3
"""
Standalone 32-bit Windows PE DLL Builder for TS2SE + Multiplayer Co-op + Engine QoL (ijl15.dll)
Uses clang++ (--target=i686-pc-windows-msvc), llvm-dlltool, and lld-link.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
os.makedirs(BUILD, exist_ok=True)

DEFS = {
    "kernel32": """LIBRARY KERNEL32.dll
EXPORTS
    LoadLibraryA@4
    GetModuleHandleA@4
    GetProcAddress@8
    FreeLibrary@4
    DisableThreadLibraryCalls@4
    VirtualProtect@16
    VirtualAlloc@16
    VirtualFree@12
    GetCurrentProcessId@0
    GetTickCount@0
    GetUserDefaultUILanguage@0
    SetLastError@4
    GetLastError@0
    CreateMutexA@12
    GetModuleFileNameA@12
    OutputDebugStringA@4
    Sleep@4
    CreateFileA@28
    ReadFile@20
    WriteFile@20
    CloseHandle@4
    FindFirstFileA@8
    FindNextFileA@8
    FindClose@4
    CreateDirectoryA@8
    MoveFileA@8
    GetFileAttributesA@4
    GetEnvironmentVariableA@12
""",
    "user32": """LIBRARY USER32.dll
EXPORTS
    SetWindowLongA@12
    GetWindowLongA@8
    CallWindowProcA@20
    GetClientRect@8
    GetWindowRect@8
    SetWindowPos@28
    GetSystemMetrics@4
    GetAsyncKeyState@4
    SetWindowTextA@8
    GetCursorPos@4
    ScreenToClient@8
    GetForegroundWindow@0
    GetWindowThreadProcessId@8
""",
    "gdi32": """LIBRARY GDI32.dll
EXPORTS
    CreateCompatibleDC@4
    DeleteDC@4
    CreateDIBSection@24
    SelectObject@8
    DeleteObject@4
    CreateFontW@56
    SetTextColor@8
    SetBkMode@8
    TextOutW@20
    GetTextExtentPoint32W@16
    GetTextMetricsW@8
    GdiFlush@0
""",
    "ws2_32": """LIBRARY WS2_32.dll
EXPORTS
    WSAStartup@8
    WSACleanup@0
    WSAGetLastError@0
    socket@12
    closesocket@4
    bind@12
    sendto@24
    recvfrom@24
    ioctlsocket@12
    listen@8
    accept@12
    connect@12
    send@16
    recv@16
    select@20
    setsockopt@20
    getsockopt@20
    htons@4
    ntohs@4
    inet_addr@4
""",
    "msvcrt": """LIBRARY msvcrt.dll
EXPORTS
    fopen
    fclose
    fflush
    fprintf
    _snprintf
    _vsnprintf
    malloc
    free
    memcpy
    memset
    memmove
    strlen
    strcmp
    strncmp
    _stricmp
    strstr
    strncpy
    strtol
    strtod
    sin
    cos
    sqrt
    atan2
"""
}

def run(cmd):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd)

def main():
    libs = []
    for name, content in DEFS.items():
        def_path = os.path.join(BUILD, f"{name}.def")
        lib_path = os.path.join(BUILD, f"{name}.lib")
        with open(def_path, "w") as f:
            f.write(content)
        run(["llvm-dlltool", "-m", "i386", "-k", "-d", def_path, "-l", lib_path])
        libs.append(lib_path)

    sources = [
        "src/proxy_ijl15.cpp",
        "src/hook_engine.cpp",
        "src/dx9_overlay.cpp",
        "src/dx9_postfx.cpp",
        "src/ui_kit.cpp",
        "src/i18n.cpp",
        "src/ui_graphics_tab.cpp",
        "src/ts2se_engine.cpp",
        "src/coop_net.cpp",
        "src/engine_qol.cpp",
    ]

    objs = []
    for src in sources:
        src_path = os.path.join(ROOT, src)
        obj_name = os.path.splitext(os.path.basename(src))[0] + ".obj"
        obj_path = os.path.join(BUILD, obj_name)
        run([
            "clang++",
            "--target=i686-pc-windows-msvc",
            "-ffreestanding",
            "-std=c++20",
            "-O2",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-threadsafe-statics",
            "-mno-sse2",
            "-I", os.path.join(ROOT, "include"),
            "-c", src_path,
            "-o", obj_path,
        ])
        objs.append(obj_path)

    out_dll = os.path.join(BUILD, "ijl15.dll")
    def_export = os.path.join(ROOT, "src/ijl15.def")
    run([
        "lld-link",
        "/DLL",
        "/MACHINE:X86",
        "/NODEFAULTLIB",
        "/ENTRY:DllMain",
        f"/DEF:{def_export}",
        f"/OUT:{out_dll}",
        *objs,
        *libs,
    ])
    print(f"\n[SUCCESS] Built 32-bit Windows PE DLL: {out_dll} ({os.path.getsize(out_dll)} bytes)")

if __name__ == "__main__":
    main()
