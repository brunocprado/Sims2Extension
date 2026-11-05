#pragma once
#include "ts2/win32_abi.hpp"
#include <stdint.h>

namespace ts2::qol {

// ============================================================================
// Modern Engine QoL & Unlocker Suite for Sims2EP9.exe (v1.0.0.153)
// ============================================================================

struct QoLConfig {
    bool     multiInstanceUnlocked       = true;
    bool     backgroundSimEnabled        = true;
    bool     borderlessFullscreen        = false;
    bool     vsyncEnabled                = true;
    bool     testingCheatsEnabled        = false;
    bool     rotate45DegreeEnabled       = false;
    bool     quarterTileEnabled          = false;
    bool     moveObjectsEnabled          = false;
    bool     snapObjectsToGrid           = true;
    bool     constrainFloorElevation     = true;
    bool     unlockDormAndAptTools       = false;
    bool     showHeadlinesEnabled        = true;
    bool     plumbBobVisible             = true;
    bool     agingEnabled                = true;
    bool     smartAutosaveEnabled        = true;
    uint32_t autosaveIntervalMin         = 15;
    uint32_t maxTreeIterations           = 25000; // Default in game is 10000 (0xA1211914)
    uint32_t maxTotalSims                = 16;    // Default is 8
    uint32_t maxTotalHumans              = 12;
    uint32_t maxTotalPets                = 8;
    uint32_t highestAllowedLevel         = 5;
    char     catalogSearchFilter[64]     = "";
};

// Applies early startup patches (Multi-Instance Mutex bypass + Background Simulation)
bool ApplyEarlyPatches();

// Called each frame from DX9 EndScene to update Smart Autosave timer & MaxTreeIterations
void TickFrame();

// Toggles Background Simulation live at runtime (patches/restores 0xA01438A3 & 0xA01438E0)
void SetBackgroundSimEnabled(bool enabled);

// Sets the SimAntics VM Max Iterations per tick at 0xA1211914
void SetMaxTreeIterations(uint32_t maxIter);
uint32_t GetMaxTreeIterations();

// Applies Lot Sim/Pet limits and build cheats via cTSCheatSystem (0xA0C9F770)
void ApplyLotAndBuildQoL(bool testingCheats, bool rotate45, bool moveObjects, uint32_t maxSims);

// Toggles Borderless Fullscreen Windowed mode on the active game HWND
bool ToggleBorderlessFullscreen(HWND hWnd);

// Triggers a safe Lot Save when in Live mode
bool TriggerSmartSaveNow();   // only requests the save (safe from any thread / EndScene)
void RunPendingSave();        // executes a requested save; call from the simulator's OnIdle hook
uint32_t GetSecondsUntilNextAutosave();

QoLConfig& GetConfig();

} // namespace ts2::qol
