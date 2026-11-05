#include "ts2/engine_qol.hpp"
#include "ts2/addresses.hpp"
#include "ts2/hook_engine.hpp"
#include "ts2/ts2se_engine.hpp"

namespace ts2::qol {

namespace {

static QoLConfig g_config = {};
static DWORD     g_lastAutosaveMs = 0;

using Fn_CreateMutexA = HANDLE (WINAPI*)(LPVOID lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
static Fn_CreateMutexA g_origCreateMutexA = nullptr;

// ============================================================================
// Hooked CreateMutexA (IAT 0xA0F493AC)
// Appends the current Process ID to "Global\The Sims 2" and "Global\The Sims 2 Body Shop"
// so multiple instances of Sims2EP9.exe never collide on Win32 named mutexes!
// ============================================================================
static HANDLE WINAPI HookedCreateMutexA(LPVOID lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName) {
    char uniqueName[160];
    LPCSTR effectiveName = lpName;
    if (lpName && (strstr(lpName, "The Sims 2") != nullptr || strstr(lpName, "SIMS2") != nullptr)) {
        _snprintf(uniqueName, sizeof(uniqueName), "%s_PID_%u",
                  lpName, static_cast<unsigned>(GetCurrentProcessId()));
        effectiveName = uniqueName;
        ts2::hook::Log("[MultiInstance] Redirected mutex '%s' -> '%s'", lpName, effectiveName);
    }
    HANDLE h = g_origCreateMutexA
               ? g_origCreateMutexA(lpMutexAttributes, bInitialOwner, effectiveName)
               : nullptr;
    SetLastError(0); // Clear ERROR_ALREADY_EXISTS (183)
    return h;
}

// Representative indexed catalog items for Fast Buy/Build Search Filter
static const CatalogEntry kIndexedCatalog[] = {
    {0x07A01B12, "NuMica AllinOne Kitchen Counter", "Surfaces / Counters", 800},
    {0x4B82D301, "Sleep-All-Night Double Bed",      "Comfort / Beds",      1250},
    {0x9C1104E2, "Luxuriant Leather Sofa",          "Comfort / Sofas",     950},
    {0x3E74A088, "BrandName Econo-Fridge",          "Appliances / Kitchen",600},
    {0x8D42E119, "Dialectric FreeStanding Stove",   "Appliances / Kitchen",900},
    {0x1F55C230, "Ciao Time Espresso Machine",      "Appliances / Small",  450},
    {0x6A09B441, "FlushForce 5000 Toilet",          "Plumbing / Toilets",  350},
    {0x5E12980C, "HydroSwirl Shower-Tub Combo",     "Plumbing / Showers",  1100},
    {0x2B9901A4, "Trottco 27-inch Color TV",        "Electronics / TVs",   500},
    {0x7F31C8D2, "Moneywell Computer XL",           "Electronics / PCs",   1800},
    {0xC08812FA, "SimAntics Pinball Machine",       "Entertainment / Rec", 1650},
    {0xD419002B, "Grand Parlour Chess Table",       "Hobbies / Logic",     500},
    {0xE8214490, "bacterial Bookshelf Oak",         "Hobbies / Knowledge", 400},
    {0xA1058833, "2-Story Colonial Column",         "Build / Columns",     320},
    {0xB740219E, "Le Magnifique Bay Window",        "Build / Windows",     420},
    {0xF0923411, "Solid Mahogany Double Door",      "Build / Doors",       650}
};

static bool ContainsCaseInsensitive(const char* haystack, const char* needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    size_t nLen = strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        if (_stricmp(p, needle) == 0) return true;
        bool match = true;
        for (size_t i = 0; i < nLen; ++i) {
            char c1 = p[i];
            char c2 = needle[i];
            if (!c1) { match = false; break; }
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

bool ApplyEarlyPatches() {
    g_lastAutosaveMs = GetTickCount();

    // 1. Hook CreateMutexA in IAT (0xA0F493AC) to isolate named mutexes per PID
    bool okMutexIat = ts2::hook::HookPointerSlot(
        addr::Rebase(addr::kIAT_CreateMutexA),
        reinterpret_cast<void*>(&HookedCreateMutexA),
        reinterpret_cast<void**>(&g_origCreateMutexA)
    );

    // 2. Patch cTSApp::Init @ 0xA0DC1CB0 (74 08 -> 90 90) so single-instance flags [edi+0x6d..0x6e] are always 0
    bool okAppCheck = ts2::hook::NopRange(addr::Rebase(addr::kPatch_MultiInstanceBypass), 2);

    // 3. Enable Background Simulation in cGZFrameworkW32::Run (0xA01438A3 & 0xA01438E0)
    SetBackgroundSimEnabled(true);

    // 4. Raise default SimAntics Max Tree Iterations (0xA1211914) from 10,000 to 25,000
    SetMaxTreeIterations(g_config.maxTreeIterations);

    ts2::hook::Log("[QoL] Early patches applied: MutexIAT=%d, AppCheckBypass=%d, BgSim=1, MaxIter=%u",
                   okMutexIat ? 1 : 0, okAppCheck ? 1 : 0, g_config.maxTreeIterations);
    return okMutexIat && okAppCheck;
}

void SetBackgroundSimEnabled(bool enabled) {
    g_config.backgroundSimEnabled = enabled;
    if (enabled) {
        // 0xA01438A3: 75 22 (jne +0x22) -> EB 22 (jmp +0x22) so WaitMessage() is never called when unfocused
        const uint8_t jmpSkipWait[2] = {0xEB, 0x22};
        ts2::hook::PatchBytes(addr::Rebase(addr::kPatch_BgSimSkipWaitMessage), jmpSkipWait, 2);
        // 0xA01438E0: 74 07 (je +0x07) -> 90 90 (nop; nop) so cGZFramework::OnIdle() always runs when unfocused
        ts2::hook::NopRange(addr::Rebase(addr::kPatch_BgSimAlwaysOnIdle), 2);
        ts2::se::PushConsoleLog("[QoL] Simulacao em Background ATIVADA (0x%08X=EB 22, 0x%08X=90 90).",
                                static_cast<unsigned>(addr::Rebase(addr::kPatch_BgSimSkipWaitMessage)),
                                static_cast<unsigned>(addr::Rebase(addr::kPatch_BgSimAlwaysOnIdle)));
    } else {
        // Restore original bytes
        const uint8_t origJne[2] = {0x75, 0x22};
        const uint8_t origJe[2]  = {0x74, 0x07};
        ts2::hook::PatchBytes(addr::Rebase(addr::kPatch_BgSimSkipWaitMessage), origJne, 2);
        ts2::hook::PatchBytes(addr::Rebase(addr::kPatch_BgSimAlwaysOnIdle), origJe, 2);
        ts2::se::PushConsoleLog("[QoL] Simulacao em Background DESATIVADA (bytes originais restaurados).");
    }
}

void SetMaxTreeIterations(uint32_t maxIter) {
    g_config.maxTreeIterations = maxIter;
    ts2::hook::PatchBytes(addr::Rebase(addr::kGlobal_MaxTreeIterations), &maxIter, sizeof(maxIter));
    ts2::se::PushConsoleLog("[QoL] SimAntics MaxTreeIterations (0x%08X) ajustado para %u.",
                            static_cast<unsigned>(addr::Rebase(addr::kGlobal_MaxTreeIterations)), maxIter);
}

uint32_t GetMaxTreeIterations() {
    return *reinterpret_cast<const uint32_t*>(addr::Rebase(addr::kGlobal_MaxTreeIterations));
}

void ApplyLotAndBuildQoL(bool testingCheats, bool rotate45, bool moveObjects, uint32_t maxSims) {
    char out[128];
    ts2::se::ExecuteCommand(testingCheats ? "boolProp testingCheatsEnabled true"
                                          : "boolProp testingCheatsEnabled false", out, sizeof(out));
    ts2::se::ExecuteCommand(rotate45 ? "boolProp allow45DegreeAngleOfRotation true"
                                     : "boolProp allow45DegreeAngleOfRotation false", out, sizeof(out));
    ts2::se::ExecuteCommand(moveObjects ? "moveObjects on" : "moveObjects off", out, sizeof(out));

    char limitCmd[64];
    _snprintf(limitCmd, sizeof(limitCmd), "uintProp maxTotalSims %u", maxSims);
    ts2::se::ExecuteCommand(limitCmd, out, sizeof(out));
}

static LONG g_savedStyle    = 0;
static RECT g_savedWndRect  = {100, 100, 1380, 820};

bool ToggleBorderlessFullscreen(HWND hWnd) {
    if (!hWnd) return false;
    constexpr int  GWL_STYLE          = -16;
    constexpr LONG WS_CAPTION         = 0x00C00000L;
    constexpr LONG WS_THICKFRAME      = 0x00040000L;
    constexpr LONG WS_MINIMIZEBOX     = 0x00020000L;
    constexpr LONG WS_MAXIMIZEBOX     = 0x00010000L;
    constexpr LONG WS_SYSMENU         = 0x00080000L;
    constexpr LONG kWindowedFrameMask = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    constexpr UINT SWP_FRAMECHANGED   = 0x0020;
    constexpr UINT SWP_NOZORDER       = 0x0004;
    constexpr UINT SWP_SHOWWINDOW     = 0x0040;

    if (!g_config.borderlessFullscreen) {
        g_savedStyle = GetWindowLongA(hWnd, GWL_STYLE);
        GetWindowRect(hWnd, &g_savedWndRect);
        int screenW = GetSystemMetrics(0 /* SM_CXSCREEN */);
        int screenH = GetSystemMetrics(1 /* SM_CYSCREEN */);
        if (screenW <= 0) screenW = 1920;
        if (screenH <= 0) screenH = 1080;

        SetWindowLongA(hWnd, GWL_STYLE, g_savedStyle & ~kWindowedFrameMask);
        SetWindowPos(hWnd, nullptr, 0, 0, screenW, screenH,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        g_config.borderlessFullscreen = true;
        ts2::se::PushConsoleLog("[QoL] Janela Sem Bordas (Borderless Fullscreen %dx%d) ATIVADA!", screenW, screenH);
    } else {
        LONG restoreStyle = g_savedStyle ? g_savedStyle : (GetWindowLongA(hWnd, GWL_STYLE) | kWindowedFrameMask);
        int w = g_savedWndRect.right - g_savedWndRect.left;
        int h = g_savedWndRect.bottom - g_savedWndRect.top;
        if (w < 640) w = 1280;
        if (h < 480) h = 720;

        SetWindowLongA(hWnd, GWL_STYLE, restoreStyle);
        SetWindowPos(hWnd, nullptr, g_savedWndRect.left, g_savedWndRect.top, w, h,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        g_config.borderlessFullscreen = false;
        ts2::se::PushConsoleLog("[QoL] Modo Janela com Bordas restaurado (%dx%d).", w, h);
    }
    return g_config.borderlessFullscreen;
}

bool TriggerSmartSaveNow() {
    g_lastAutosaveMs = GetTickCount();
    uintptr_t sim = ts2::se::GetSimulatorPtr();
    if (!sim) {
        ts2::se::PushConsoleLog("[Autosave] Aviso: Nenhum lote carregado no momento. Save adiado.");
        return false;
    }
    uintptr_t pPersist = reinterpret_cast<uintptr_t(*)()>(addr::Rebase(addr::kFunc_TSGetPersistSystem))();
    char out[128] = {};
    ts2::se::ExecuteCommand("save", out, sizeof(out));
    ts2::se::PushConsoleLog("[Autosave] Checkpoint de lote disparado (PersistSys=0x%08X, Tick=%u)!",
                            static_cast<unsigned>(pPersist), ts2::se::GetSimulatorTickCount());
    return true;
}

uint32_t GetSecondsUntilNextAutosave() {
    if (!g_config.smartAutosaveEnabled) return 0;
    DWORD elapsedSec = (GetTickCount() - g_lastAutosaveMs) / 1000u;
    uint32_t targetSec = g_config.autosaveIntervalMin * 60u;
    return (elapsedSec < targetSec) ? (targetSec - elapsedSec) : 0;
}

void TickFrame() {
    if (!g_config.smartAutosaveEnabled) return;
    if (GetSecondsUntilNextAutosave() == 0) {
        TriggerSmartSaveNow();
    }
}

QoLConfig& GetConfig() {
    return g_config;
}

size_t FilterCatalogEntries(const char* query, const CatalogEntry** outMatches, size_t maxMatches) {
    size_t count = 0;
    for (size_t i = 0; i < sizeof(kIndexedCatalog) / sizeof(kIndexedCatalog[0]); ++i) {
        if (ContainsCaseInsensitive(kIndexedCatalog[i].name, query) ||
            ContainsCaseInsensitive(kIndexedCatalog[i].category, query)) {
            if (count < maxMatches) {
                outMatches[count++] = &kIndexedCatalog[i];
            }
        }
    }
    return count;
}

} // namespace ts2::qol
