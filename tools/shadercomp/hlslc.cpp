// Minimal HLSL -> D3D9 shader bytecode compiler front-end.
// Loads d3dcompiler_47.dll (the one shipped with Wine/Windows) and calls D3DCompile, so shaders can be
// compiled on Linux without the DirectX SDK.
//
//   hlslc.exe <input.hlsl> <output.bin> <entry> <profile>      e.g.  hlslc.exe ssao.hlsl ssao.bin PSMain ps_3_0
//
// Built by tools/build_shaders.py with clang (--target=i686-pc-windows-msvc), freestanding, kernel32 only.
#include <stdint.h>

extern "C" {
typedef void*          HANDLE;
typedef unsigned long  DWORD;
typedef int            BOOL;
typedef const char*    LPCSTR;
typedef void*          HMODULE;
typedef void*          FARPROC_T;

__declspec(dllimport) HMODULE  __stdcall LoadLibraryA(LPCSTR);
__declspec(dllimport) FARPROC_T __stdcall GetProcAddress(HMODULE, LPCSTR);
__declspec(dllimport) HANDLE   __stdcall CreateFileA(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL     __stdcall ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
__declspec(dllimport) BOOL     __stdcall WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) BOOL     __stdcall CloseHandle(HANDLE);
__declspec(dllimport) DWORD    __stdcall GetFileSize(HANDLE, DWORD*);
__declspec(dllimport) void*    __stdcall VirtualAlloc(void*, DWORD, DWORD, DWORD);
__declspec(dllimport) const char* __stdcall GetCommandLineA(void);
__declspec(dllimport) HANDLE   __stdcall GetStdHandle(DWORD);
__declspec(dllimport) void     __stdcall ExitProcess(unsigned);
}

static void Print(const char* s) {
    DWORD n = 0, w = 0;
    while (s[n]) ++n;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, nullptr);
}

struct Blob {
    void** vtable;
};
static void* BlobPtr(Blob* b)  { return reinterpret_cast<void*(__stdcall*)(Blob*)>(b->vtable[3])(b); }
static DWORD BlobSize(Blob* b) { return (DWORD)reinterpret_cast<unsigned(__stdcall*)(Blob*)>(b->vtable[4])(b); }

typedef long (__stdcall* PFN_D3DCompile)(const void* src, unsigned srcLen, LPCSTR name, const void* defines,
                                         void* include, LPCSTR entry, LPCSTR target, unsigned flags1,
                                         unsigned flags2, Blob** code, Blob** errors);

// Splits GetCommandLineA() into at most 8 arguments (quotes supported).
static int SplitArgs(char* cmd, char** argv, int maxArgs) {
    int argc = 0;
    char* p = cmd;
    while (*p && argc < maxArgs) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p == '"') {
            ++p;
            argv[argc++] = p;
            while (*p && *p != '"') ++p;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') ++p;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

extern "C" void __stdcall mainCRTStartup() {
    static char cmdline[2048];
    const char* src = GetCommandLineA();
    int i = 0;
    while (src[i] && i < 2047) { cmdline[i] = src[i]; ++i; }
    cmdline[i] = 0;
    char* argv[9] = {};
    int argc = SplitArgs(cmdline, argv, 9);
    if (argc < 5) {
        Print("usage: hlslc <in.hlsl> <out.bin> <entry> <profile>\n");
        ExitProcess(2);
    }
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    if (!dll) { Print("cannot load d3dcompiler_47.dll\n"); ExitProcess(3); }
    auto compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(dll, "D3DCompile"));
    if (!compile) { Print("D3DCompile not found\n"); ExitProcess(3); }

    HANDLE f = CreateFileA(argv[1], 0x80000000u, 1, nullptr, 3, 0x80, nullptr);
    if (f == (HANDLE)(intptr_t)-1) { Print("cannot open input\n"); ExitProcess(4); }
    DWORD size = GetFileSize(f, nullptr), got = 0;
    char* text = static_cast<char*>(VirtualAlloc(nullptr, size + 1, 0x3000, 4));
    ReadFile(f, text, size, &got, nullptr);
    CloseHandle(f);

    Blob* code = nullptr;
    Blob* errs = nullptr;
    // D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1<<15, D3DCOMPILE_PACK_MATRIX_ROW_MAJOR = 1<<3
    long hr = compile(text, got, argv[1], nullptr, nullptr, argv[3], argv[4], (1u << 15) | (1u << 3), 0, &code, &errs);
    if (errs) {
        DWORD w = 0;
        WriteFile(GetStdHandle((DWORD)-11), BlobPtr(errs), BlobSize(errs), &w, nullptr);
    }
    if (hr < 0 || !code) { Print("COMPILE FAILED\n"); ExitProcess(5); }

    HANDLE o = CreateFileA(argv[2], 0x40000000u, 0, nullptr, 2, 0x80, nullptr);
    if (o == (HANDLE)(intptr_t)-1) { Print("cannot create output\n"); ExitProcess(6); }
    DWORD w = 0;
    WriteFile(o, BlobPtr(code), BlobSize(code), &w, nullptr);
    CloseHandle(o);
    ExitProcess(0);
}
