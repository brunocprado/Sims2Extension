#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// ============================================================================
// Self-contained 32-bit Win32, Winsock2, Direct3D 9 & MSVCRT ABI Declarations
// Allows compiling with clang++ --target=i686-pc-windows-msvc without external SDKs
// ============================================================================

#if !defined(_WIN32)
#define _WIN32 1
#endif

#define WINAPI      __stdcall
#define APIENTRY    __stdcall
#define CALLBACK    __stdcall
#define STDMETHODCALLTYPE __stdcall
#define __cdecl     __attribute__((__cdecl__))
#define __stdcall   __attribute__((__stdcall__))
#define __thiscall  __attribute__((__thiscall__))
#define __fastcall  __attribute__((__fastcall__))

extern "C" {

// Basic Win32 Types
using BYTE      = uint8_t;
using WORD      = uint16_t;
using DWORD     = uint32_t;
using BOOL      = int32_t;
using UINT      = uint32_t;
using INT       = int32_t;
using LONG      = int32_t;
using ULONG     = uint32_t;
using SHORT     = int16_t;
using USHORT    = uint16_t;
using LONGLONG  = int64_t;
using ULONGLONG = uint64_t;
using UINT_PTR  = uintptr_t;
using INT_PTR   = intptr_t;
using LONG_PTR  = intptr_t;
using ULONG_PTR = uintptr_t;
using SIZE_T    = size_t;
using WPARAM    = UINT_PTR;
using LPARAM    = LONG_PTR;
using LRESULT   = LONG_PTR;
using HRESULT   = int32_t;

using HANDLE    = void*;
using HINSTANCE = void*;
using HMODULE   = void*;
using HWND      = void*;
using HDC       = void*;
using FARPROC   = void*;
using LPVOID    = void*;
using LPCVOID   = const void*;
using LPSTR     = char*;
using LPCSTR    = const char*;

constexpr BOOL TRUE  = 1;
constexpr BOOL FALSE = 0;

constexpr DWORD DLL_PROCESS_DETACH = 0;
constexpr DWORD DLL_PROCESS_ATTACH = 1;
constexpr DWORD DLL_THREAD_ATTACH  = 2;
constexpr DWORD DLL_THREAD_DETACH  = 3;

constexpr DWORD PAGE_EXECUTE_READ      = 0x20;
constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;
constexpr DWORD PAGE_READWRITE         = 0x04;
constexpr DWORD MEM_COMMIT             = 0x1000;
constexpr DWORD MEM_RESERVE            = 0x2000;
constexpr DWORD MEM_RELEASE            = 0x8000;

constexpr int GWL_WNDPROC = -4;

// Window Messages
constexpr UINT WM_KEYDOWN     = 0x0100;
constexpr UINT WM_KEYUP       = 0x0101;
constexpr UINT WM_CHAR        = 0x0102;
constexpr UINT WM_SYSKEYDOWN  = 0x0104;
constexpr UINT WM_SYSKEYUP    = 0x0105;
constexpr UINT WM_MOUSEMOVE   = 0x0200;
constexpr UINT WM_LBUTTONDOWN = 0x0201;
constexpr UINT WM_LBUTTONUP   = 0x0202;
constexpr UINT WM_RBUTTONDOWN = 0x0204;
constexpr UINT WM_RBUTTONUP   = 0x0205;
constexpr UINT WM_MOUSEWHEEL  = 0x020A;

// Virtual Keys
constexpr int VK_LBUTTON = 0x01;
constexpr int VK_RBUTTON = 0x02;
constexpr int VK_BACK   = 0x08;
constexpr int VK_TAB    = 0x09;
constexpr int VK_RETURN = 0x0D;
constexpr int VK_SHIFT  = 0x10;
constexpr int VK_CONTROL= 0x11;
constexpr int VK_ESCAPE = 0x1B;
constexpr int VK_INSERT = 0x2D;
constexpr int VK_F5     = 0x74;
constexpr int VK_F9     = 0x78;
constexpr int VK_F10    = 0x79;
constexpr int VK_F11    = 0x7A;

struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};

struct POINT {
    LONG x;
    LONG y;
};

using WNDPROC = LRESULT (CALLBACK*)(HWND, UINT, WPARAM, LPARAM);

// ============================================================================
// KERNEL32.DLL Imports
// ============================================================================
__declspec(dllimport) HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName);
__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
__declspec(dllimport) FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
__declspec(dllimport) BOOL    WINAPI FreeLibrary(HMODULE hLibModule);
__declspec(dllimport) BOOL    WINAPI DisableThreadLibraryCalls(HMODULE hLibModule);
__declspec(dllimport) BOOL    WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect);
__declspec(dllimport) LPVOID  WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
__declspec(dllimport) BOOL    WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
__declspec(dllimport) DWORD   WINAPI GetCurrentProcessId(void);
__declspec(dllimport) DWORD   WINAPI GetTickCount(void);
__declspec(dllimport) void    WINAPI SetLastError(DWORD dwErrCode);
__declspec(dllimport) DWORD   WINAPI GetLastError(void);
__declspec(dllimport) HANDLE  WINAPI CreateMutexA(LPVOID lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
__declspec(dllimport) DWORD   WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
__declspec(dllimport) void    WINAPI OutputDebugStringA(LPCSTR lpOutputString);
__declspec(dllimport) void    WINAPI Sleep(DWORD dwMilliseconds);

// File system (used by Co-op neighborhood transfer)
struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
struct WIN32_FIND_DATAA {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    char     cFileName[260];
    char     cAlternateFileName[14];
};
constexpr DWORD GENERIC_READ             = 0x80000000u;
constexpr DWORD GENERIC_WRITE            = 0x40000000u;
constexpr DWORD FILE_SHARE_READ          = 1;
constexpr DWORD FILE_SHARE_WRITE         = 2;
constexpr DWORD CREATE_ALWAYS            = 2;
constexpr DWORD OPEN_EXISTING            = 3;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr DWORD FILE_ATTRIBUTE_NORMAL    = 0x80;
constexpr DWORD INVALID_FILE_ATTRIBUTES  = 0xFFFFFFFFu;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

__declspec(dllimport) HANDLE  WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPVOID lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
__declspec(dllimport) BOOL    WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, DWORD* lpNumberOfBytesRead, LPVOID lpOverlapped);
__declspec(dllimport) BOOL    WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, DWORD* lpNumberOfBytesWritten, LPVOID lpOverlapped);
__declspec(dllimport) BOOL    WINAPI CloseHandle(HANDLE hObject);
__declspec(dllimport) HANDLE  WINAPI FindFirstFileA(LPCSTR lpFileName, WIN32_FIND_DATAA* lpFindFileData);
__declspec(dllimport) BOOL    WINAPI FindNextFileA(HANDLE hFindFile, WIN32_FIND_DATAA* lpFindFileData);
__declspec(dllimport) BOOL    WINAPI FindClose(HANDLE hFindFile);
__declspec(dllimport) BOOL    WINAPI CreateDirectoryA(LPCSTR lpPathName, LPVOID lpSecurityAttributes);
__declspec(dllimport) BOOL    WINAPI MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
__declspec(dllimport) DWORD   WINAPI GetFileAttributesA(LPCSTR lpFileName);
__declspec(dllimport) DWORD   WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);

// ============================================================================
// USER32.DLL Imports
// ============================================================================
__declspec(dllimport) LONG    WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong);
__declspec(dllimport) LONG    WINAPI GetWindowLongA(HWND hWnd, int nIndex);
__declspec(dllimport) LRESULT WINAPI CallWindowProcA(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
__declspec(dllimport) BOOL    WINAPI GetClientRect(HWND hWnd, RECT* lpRect);
__declspec(dllimport) BOOL    WINAPI GetWindowRect(HWND hWnd, RECT* lpRect);
__declspec(dllimport) BOOL    WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
__declspec(dllimport) int     WINAPI GetSystemMetrics(int nIndex);
__declspec(dllimport) SHORT   WINAPI GetAsyncKeyState(int vKey);
__declspec(dllimport) BOOL    WINAPI SetWindowTextA(HWND hWnd, LPCSTR lpString);
__declspec(dllimport) BOOL    WINAPI GetCursorPos(POINT* lpPoint);
__declspec(dllimport) BOOL    WINAPI ScreenToClient(HWND hWnd, POINT* lpPoint);
__declspec(dllimport) HWND    WINAPI GetForegroundWindow(void);
__declspec(dllimport) DWORD   WINAPI GetWindowThreadProcessId(HWND hWnd, DWORD* lpdwProcessId);

// ============================================================================
// WS2_32.DLL (Winsock2) Imports
// ============================================================================
using SOCKET = UINT_PTR;
constexpr SOCKET INVALID_SOCKET = (SOCKET)(~0);
constexpr int SOCKET_ERROR      = -1;
constexpr int AF_INET           = 2;
constexpr int SOCK_DGRAM        = 2;
constexpr int IPPROTO_UDP       = 17;
constexpr long FIONBIO          = (long)0x8004667E;
constexpr int SOCK_STREAM       = 1;
constexpr int IPPROTO_TCP       = 6;
constexpr int TCP_NODELAY       = 1;
constexpr int SOL_SOCKET        = 0xFFFF;
constexpr int SO_REUSEADDR      = 0x0004;
constexpr int SO_ERROR          = 0x1007;
constexpr int WSAEWOULDBLOCK    = 10035;
constexpr int WSAEINPROGRESS    = 10036;
constexpr int WSAEALREADY       = 10037;
constexpr int WSAEISCONN        = 10056;

struct fd_set {
    UINT   fd_count;
    SOCKET fd_array[64];
};
struct timeval {
    LONG tv_sec;
    LONG tv_usec;
};

struct WSADATA {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
};

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    int16_t  sin_family;
    uint16_t sin_port;
    in_addr  sin_addr;
    char     sin_zero[8];
};

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

__declspec(dllimport) int      WINAPI WSAStartup(WORD wVersionRequested, WSADATA* lpWSAData);
__declspec(dllimport) int      WINAPI WSACleanup(void);
__declspec(dllimport) int      WINAPI WSAGetLastError(void);
__declspec(dllimport) SOCKET   WINAPI socket(int af, int type, int protocol);
__declspec(dllimport) int      WINAPI closesocket(SOCKET s);
__declspec(dllimport) int      WINAPI bind(SOCKET s, const sockaddr* name, int namelen);
__declspec(dllimport) int      WINAPI sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen);
__declspec(dllimport) int      WINAPI recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen);
__declspec(dllimport) int      WINAPI ioctlsocket(SOCKET s, long cmd, ULONG* argp);
__declspec(dllimport) int      WINAPI listen(SOCKET s, int backlog);
__declspec(dllimport) SOCKET   WINAPI accept(SOCKET s, sockaddr* addr, int* addrlen);
__declspec(dllimport) int      WINAPI connect(SOCKET s, const sockaddr* name, int namelen);
__declspec(dllimport) int      WINAPI send(SOCKET s, const char* buf, int len, int flags);
__declspec(dllimport) int      WINAPI recv(SOCKET s, char* buf, int len, int flags);
__declspec(dllimport) int      WINAPI select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const timeval* timeout);
__declspec(dllimport) int      WINAPI setsockopt(SOCKET s, int level, int optname, const char* optval, int optlen);
__declspec(dllimport) int      WINAPI getsockopt(SOCKET s, int level, int optname, char* optval, int* optlen);
__declspec(dllimport) uint16_t WINAPI htons(uint16_t hostshort);
__declspec(dllimport) uint16_t WINAPI ntohs(uint16_t netshort);
__declspec(dllimport) uint32_t WINAPI inet_addr(const char* cp);

// ============================================================================
// MSVCRT.DLL Imports
// ============================================================================
struct FILE;
__declspec(dllimport) FILE*  __cdecl fopen(const char* filename, const char* mode);
__declspec(dllimport) int    __cdecl fclose(FILE* stream);
__declspec(dllimport) int    __cdecl fflush(FILE* stream);
__declspec(dllimport) int    __cdecl fprintf(FILE* stream, const char* format, ...);
__declspec(dllimport) int    __cdecl _snprintf(char* buffer, size_t count, const char* format, ...);
__declspec(dllimport) int    __cdecl _vsnprintf(char* buffer, size_t count, const char* format, va_list argptr);
__declspec(dllimport) void*  __cdecl malloc(size_t size);
__declspec(dllimport) void   __cdecl free(void* ptr);
__declspec(dllimport) void*  __cdecl memcpy(void* dst, const void* src, size_t count);
__declspec(dllimport) void*  __cdecl memset(void* dst, int val, size_t count);
__declspec(dllimport) void*  __cdecl memmove(void* dst, const void* src, size_t count);
__declspec(dllimport) size_t __cdecl strlen(const char* str);
__declspec(dllimport) int    __cdecl strcmp(const char* s1, const char* s2);
__declspec(dllimport) int    __cdecl strncmp(const char* s1, const char* s2, size_t n);
__declspec(dllimport) int    __cdecl _stricmp(const char* s1, const char* s2);
__declspec(dllimport) char*  __cdecl strstr(const char* haystack, const char* needle);
__declspec(dllimport) char*  __cdecl strncpy(char* dst, const char* src, size_t n);
__declspec(dllimport) long   __cdecl strtol(const char* str, char** endptr, int base);
__declspec(dllimport) double __cdecl strtod(const char* str, char** endptr);
__declspec(dllimport) double __cdecl sin(double x);
__declspec(dllimport) double __cdecl cos(double x);
__declspec(dllimport) double __cdecl sqrt(double x);
__declspec(dllimport) double __cdecl atan2(double y, double x);

} // extern "C"

// ============================================================================
// Direct3D 9 Minimal ABI (VTable indices & RenderStates for Immediate-Mode UI)
// ============================================================================
namespace d3d9 {

constexpr DWORD D3DFVF_XYZRHW  = 0x004;
constexpr DWORD D3DFVF_DIFFUSE = 0x040;
constexpr DWORD D3DFVF_UI_VERT = (D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

constexpr DWORD D3DPT_POINTLIST     = 1;
constexpr DWORD D3DPT_LINELIST      = 2;
constexpr DWORD D3DPT_LINESTRIP     = 3;
constexpr DWORD D3DPT_TRIANGLELIST  = 4;
constexpr DWORD D3DPT_TRIANGLESTRIP = 5;

// D3DRENDERSTATETYPE
constexpr DWORD D3DRS_ZENABLE          = 7;
constexpr DWORD D3DRS_FILLMODE         = 8;
constexpr DWORD D3DRS_SHADEMODE        = 9;
constexpr DWORD D3DRS_ZWRITEENABLE     = 14;
constexpr DWORD D3DRS_ALPHATESTENABLE  = 15;
constexpr DWORD D3DRS_SRCBLEND         = 19;
constexpr DWORD D3DRS_DESTBLEND        = 20;
constexpr DWORD D3DRS_CULLMODE         = 22;
constexpr DWORD D3DRS_ALPHABLENDENABLE = 27;
constexpr DWORD D3DRS_FOGENABLE        = 28;
constexpr DWORD D3DRS_STENCILENABLE    = 52;
constexpr DWORD D3DRS_LIGHTING         = 137;
constexpr DWORD D3DRS_CLIPPLANEENABLE  = 152;
constexpr DWORD D3DRS_COLORWRITEENABLE = 168;
constexpr DWORD D3DRS_SCISSORTESTENABLE= 174;

constexpr DWORD D3DBLEND_SRCALPHA      = 5;
constexpr DWORD D3DBLEND_INVSRCALPHA   = 6;
constexpr DWORD D3DCULL_NONE           = 1;
constexpr DWORD D3DFILL_SOLID          = 3;

// D3DTEXTURESTAGESTATETYPE
constexpr DWORD D3DTSS_COLOROP   = 1;
constexpr DWORD D3DTSS_COLORARG1 = 2;
constexpr DWORD D3DTSS_ALPHAOP   = 4;
constexpr DWORD D3DTSS_ALPHAARG1 = 5;
constexpr DWORD D3DTOP_SELECTARG1= 2;
constexpr DWORD D3DTA_DIFFUSE    = 0;

struct D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType;
    DWORD MultiSampleQuality;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
};

struct UIVertex {
    float x, y, z, rhw;
    DWORD color; // 0xAARRGGBB
};

    // IDirect3DDevice9 VTable indices:
    //   [4]  GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS*)
    //   [16] Reset(D3DPRESENT_PARAMETERS*)
    //   [17] Present(const RECT*, const RECT*, HWND, const RGNDATA*)
    //   [41] BeginScene()
    //   [42] EndScene()
    //   [57] SetRenderState(D3DRENDERSTATETYPE, DWORD)
    //   [59] CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**)
    //   [65] SetTexture(DWORD Stage, void* pTexture)
    //   [67] SetTextureStageState(DWORD Stage, DWORD Type, DWORD Value)
    //   [83] DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
    //   [89] SetFVF(DWORD FVF)
    //   [92] SetVertexShader(void* pShader)
    //   [107] SetPixelShader(void* pShader)
struct D3DDEVICE_CREATION_PARAMETERS {
    UINT  AdapterOrdinal;
    DWORD DeviceType;
    HWND  hFocusWindow;
    DWORD BehaviorFlags;
};

struct D3DVIEWPORT9 {
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
};

struct IDirect3DStateBlock9 {
    void** vtable;
    ULONG Release() {
        using Fn = ULONG(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
        return reinterpret_cast<Fn>(vtable[2])(this);
    }
    HRESULT Capture() {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
        return reinterpret_cast<Fn>(vtable[4])(this);
    }
    HRESULT Apply() {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
        return reinterpret_cast<Fn>(vtable[5])(this);
    }
};

struct IDirect3DDevice9 {
    void** vtable;

    HRESULT GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParams) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DDEVICE_CREATION_PARAMETERS*);
        return reinterpret_cast<Fn>(vtable[9])(this, pParams);
    }
    HRESULT BeginScene() {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
        return reinterpret_cast<Fn>(vtable[41])(this);
    }
    HRESULT EndScene() {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
        return reinterpret_cast<Fn>(vtable[42])(this);
    }
    HRESULT SetViewport(const D3DVIEWPORT9* pViewport) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
        return reinterpret_cast<Fn>(vtable[47])(this, pViewport);
    }
    HRESULT GetViewport(D3DVIEWPORT9* pViewport) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DVIEWPORT9*);
        return reinterpret_cast<Fn>(vtable[48])(this, pViewport);
    }
    HRESULT CreateStateBlock(DWORD type, IDirect3DStateBlock9** ppSB) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DStateBlock9**);
        return reinterpret_cast<Fn>(vtable[59])(this, type, ppSB);
    }
    HRESULT SetRenderState(DWORD state, DWORD value) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, DWORD);
        return reinterpret_cast<Fn>(vtable[57])(this, state, value);
    }
    HRESULT SetTexture(DWORD stage, void* tex) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, void*);
        return reinterpret_cast<Fn>(vtable[65])(this, stage, tex);
    }
    HRESULT SetTextureStageState(DWORD stage, DWORD type, DWORD value) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, DWORD, DWORD);
        return reinterpret_cast<Fn>(vtable[67])(this, stage, type, value);
    }
    HRESULT DrawPrimitiveUP(DWORD primType, UINT primCount, const void* verts, UINT stride) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, UINT, const void*, UINT);
        return reinterpret_cast<Fn>(vtable[83])(this, primType, primCount, verts, stride);
    }
    HRESULT SetFVF(DWORD fvf) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);
        return reinterpret_cast<Fn>(vtable[89])(this, fvf);
    }
    HRESULT SetVertexShader(void* shader) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*);
        return reinterpret_cast<Fn>(vtable[92])(this, shader);
    }
    HRESULT SetPixelShader(void* shader) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*);
        return reinterpret_cast<Fn>(vtable[107])(this, shader);
    }
};

// IDirect3D9 VTable index:
//   [16] CreateDevice(UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**)
struct IDirect3D9 {
    void** vtable;
};

constexpr DWORD ColorARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (DWORD(a) << 24) | (DWORD(r) << 16) | (DWORD(g) << 8) | DWORD(b);
}

} // namespace d3d9
