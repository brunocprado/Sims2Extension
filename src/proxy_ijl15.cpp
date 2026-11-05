#include "ts2/win32_abi.hpp"
#include "ts2/addresses.hpp"
#include "ts2/hook_engine.hpp"
#include "ts2/engine_qol.hpp"
#include "ts2/ts2se_engine.hpp"
#include "ts2/coop_net.hpp"
#include "ts2/dx9_overlay.hpp"

// ============================================================================
// Intel JPEG Library (ijl15.dll) Proxy & TS2SE/Co-op Bootstrapper
// Decompiled via REA + Ghidra 12.1.2:
//   ev_ecbd771ba3ef5bd05ef93e47149bdc9685dd4048560b06541247184d08a35b0f
//   ev_6cc767112786873dd7cc46278a859a5aa6533bfaf1fb177237a098ae141e7ff8
//   ev_264c1e4da6f68d8747f35bf1f4850266b7313be0b7aaa98924e63cd82dd1e84a
// ============================================================================

namespace {

HMODULE g_hOrigIjl = nullptr;

using Fn_ijlGetLibVersion = const void* (__stdcall*)();
using Fn_ijlInit          = int (__stdcall*)(void* jcprops);
using Fn_ijlFree          = int (__stdcall*)(void* jcprops);
using Fn_ijlRead          = int (__stdcall*)(void* jcprops, int iotype);
using Fn_ijlWrite         = int (__stdcall*)(void* jcprops, int iotype);
using Fn_ijlErrorStr      = const char* (__stdcall*)(int code);

Fn_ijlGetLibVersion g_origGetLibVersion = nullptr;
Fn_ijlInit          g_origInit          = nullptr;
Fn_ijlFree          g_origFree          = nullptr;
Fn_ijlRead          g_origRead          = nullptr;
Fn_ijlWrite         g_origWrite         = nullptr;
Fn_ijlErrorStr      g_origErrorStr      = nullptr;

void EnsureOriginalIjlLoaded() {
    if (g_hOrigIjl) return;
    g_hOrigIjl = LoadLibraryA("ijl15_orig.dll");
    if (!g_hOrigIjl) {
        g_hOrigIjl = LoadLibraryA("TSBin\\ijl15_orig.dll");
    }
    if (g_hOrigIjl) {
        g_origGetLibVersion = reinterpret_cast<Fn_ijlGetLibVersion>(GetProcAddress(g_hOrigIjl, "ijlGetLibVersion"));
        g_origInit          = reinterpret_cast<Fn_ijlInit>(GetProcAddress(g_hOrigIjl, "ijlInit"));
        g_origFree          = reinterpret_cast<Fn_ijlFree>(GetProcAddress(g_hOrigIjl, "ijlFree"));
        g_origRead          = reinterpret_cast<Fn_ijlRead>(GetProcAddress(g_hOrigIjl, "ijlRead"));
        g_origWrite         = reinterpret_cast<Fn_ijlWrite>(GetProcAddress(g_hOrigIjl, "ijlWrite"));
        g_origErrorStr      = reinterpret_cast<Fn_ijlErrorStr>(GetProcAddress(g_hOrigIjl, "ijlErrorStr"));
        ts2::hook::Log("[Proxy] Loaded original ijl15_orig.dll at 0x%08X", reinterpret_cast<unsigned>(g_hOrigIjl));
    } else {
        ts2::hook::Log("[Proxy] WARNING: ijl15_orig.dll not found! Using fallback stubs.");
    }
}

bool g_initialized = false;

void InitializeAllSubsystems() {
    if (g_initialized) return;
    g_initialized = true;

    HMODULE hExe = GetModuleHandleA(nullptr);
    ts2::addr::SetRuntimeBase(reinterpret_cast<uintptr_t>(hExe));
    ts2::hook::Log("================================================================");
    ts2::hook::Log(" TS2SE + Multiplayer Co-op + Engine QoL Suite v1.0 (EP9 1.0.0.153)");
    ts2::hook::Log(" PID: %u | Main EXE Module Base: 0x%08X (Static Base: 0xA0000000)",
                   static_cast<unsigned>(GetCurrentProcessId()),
                   reinterpret_cast<unsigned>(hExe));
    ts2::hook::Log("================================================================");

    ts2::hook::InstallCrashLogger();
    EnsureOriginalIjlLoaded();

    // 1. Apply Multi-Instance & Background Simulation patches
    ts2::qol::ApplyEarlyPatches();

    // 2. Initialize TS2SE (SimAntics VM hooks, Custom Primitives 0x0080+, Motives & Cheats)
    ts2::se::Initialize();

    // 3. Initialize Co-op v2 (host-authoritative TCP + neighborhood transfer)
    ts2::coop::Initialize();

    // 4. Initialize DirectX 9 Overlay (hooks Direct3DCreate9 -> EndScene/Reset + WndProc)
    ts2::ui::InitializeDx9Overlay();
}

} // namespace

extern "C" {

int _fltused = 0;

const void* __stdcall ijlGetLibVersion() {
    EnsureOriginalIjlLoaded();
    if (g_origGetLibVersion) return g_origGetLibVersion();
    static const int kDummyVer[8] = {1, 5, 4, 36, 0, 0, 0, 0};
    return kDummyVer;
}

int __stdcall ijlInit(void* jcprops) {
    EnsureOriginalIjlLoaded();
    return g_origInit ? g_origInit(jcprops) : 0;
}

int __stdcall ijlFree(void* jcprops) {
    EnsureOriginalIjlLoaded();
    return g_origFree ? g_origFree(jcprops) : 0;
}

int __stdcall ijlRead(void* jcprops, int iotype) {
    EnsureOriginalIjlLoaded();
    return g_origRead ? g_origRead(jcprops, iotype) : 0;
}

int __stdcall ijlWrite(void* jcprops, int iotype) {
    EnsureOriginalIjlLoaded();
    return g_origWrite ? g_origWrite(jcprops, iotype) : 0;
}

const char* __stdcall ijlErrorStr(int code) {
    EnsureOriginalIjlLoaded();
    return g_origErrorStr ? g_origErrorStr(code) : "IJL_OK";
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        // Pin our DLL in memory so the game's JPEG loader FreeLibrary() calls
        // never unload our hooks or reset our state!
        char selfPath[260] = {};
        if (GetModuleFileNameA(hinstDLL, selfPath, sizeof(selfPath)) > 0) {
            LoadLibraryA(selfPath);
        }
        InitializeAllSubsystems();
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        // Only tear down if the process is actually exiting (lpvReserved != nullptr)
        if (lpvReserved != nullptr) {
            ts2::ui::ShutdownDx9Overlay();
            ts2::coop::Shutdown();
            ts2::se::Shutdown();
        }
    }
    return TRUE;
}

// Entry point alias for lld-link /ENTRY:_DllMainCRTStartup@12
BOOL WINAPI _DllMainCRTStartup(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return DllMain(hinstDLL, fdwReason, lpvReserved);
}

} // extern "C"
