#pragma once
#include "ts2/win32_abi.hpp"
#include "ts2/addresses.hpp"
#include <stdint.h>

namespace ts2::se {

// ============================================================================
// TS2 Script Extender (TS2SE) & Custom SimAntics BHAV Primitives (0x0080+)
// ============================================================================

enum class BhavResult : int32_t {
    TrueBranch  = 0,
    FalseBranch = 1,
    YieldWait   = 2,
    ErrorReset  = 3
};

// Snapshot of the most recent active SimAntics stack frames for the real-time Inspector
struct StackFrameSnapshot {
    uintptr_t stackElemPtr   = 0;
    uintptr_t objectPtr      = 0;
    uint16_t  stackObjectID  = 0;
    uint16_t  targetObjectID = 0;
    uint16_t  currentNodeID  = 0;
    uint16_t  lastOpcode     = 0;
    uint8_t   numLocals      = 0;
    uint8_t   numParams      = 0;
    int16_t   locals[8]      = {};
    int16_t   params[4]      = {};
    uint32_t  captureTick    = 0;
};

// Custom BHAV Primitive Handler Signature (Opcodes 0x0080 .. 0x00FF)
using CustomPrimitiveFn = BhavResult (*)(
    void*          pObject,     // cTSObject* or cTSPerson*
    void*          pStackElem,  // cTSTreeStackElem*
    uint16_t       opcode,      // 0x0080 .. 0x00FF
    const uint8_t* operands16   // 16-byte instruction operand block
);

struct CustomPrimitiveInfo {
    uint16_t          opcode    = 0;
    const char*       name      = nullptr;
    const char*       summary   = nullptr;
    CustomPrimitiveFn handler   = nullptr;
    uint32_t          callCount = 0;
};

// Initializes TS2SE hooks (cTSObject::ExecuteObjectPrimitive, cTSTreeSim::ExecuteStep, cTSMotives)
bool Initialize();
void Shutdown();

// Registers a custom SimAntics BHAV primitive in range [0x0080, 0x00FF]
bool RegisterPrimitive(uint16_t opcode, const char* name, const char* summary, CustomPrimitiveFn fn);

// Access the 8 built-in TS2SE custom primitives (0x0080..0x0087)
const CustomPrimitiveInfo* GetRegisteredPrimitives(size_t* outCount);

// Shared 16-bit registers accessible by BHAV Primitive 0x0081 & Lua/Console (replicated in Co-op)
int16_t GetSharedRegister(uint8_t regIndex);
void    SetSharedRegister(uint8_t regIndex, int16_t value, bool broadcastToCoop = true);

// Executes either a native Sims 2 cheat (via 0xA0C9F770) or a TS2SE/Lua command
bool ExecuteCommand(const char* cmdLine, char* outResponse, size_t maxResponseLen);

// Console log ring buffer for the DX9 Overlay
size_t      GetConsoleLogCount();
const char* GetConsoleLogLine(size_t index);
void        PushConsoleLog(const char* fmt, ...);

// SimAntics VM & Motive Inspector Accessors
constexpr size_t kMaxInspectedFrames = 16;
const StackFrameSnapshot* GetInspectedFrames(size_t* outCount);
uint32_t                  GetTotalBhavStepsExecuted();

// Live Motive Inspector (tracks the most recently active Sim's cTSMotives* at +0x48)
uintptr_t   GetActiveSimMotivesPtr();
float       GetSimMotive(int motiveIndex); // 0..15 (-100.0f .. +100.0f)
void        SetSimMotive(int motiveIndex, float value);
void        FillAllActiveSimMotives();
const char* GetMotiveName(int motiveIndex);

// Reads live cITSSimulator* fields safely
uintptr_t GetSimulatorPtr();
uint32_t  GetSimulatorTickCount();
uint32_t  GetSimulatorRandomSeed();
void      SetSimulatorRandomSeed(uint32_t seed);
int16_t   GetSimulatorState();
int16_t   GetSimulatorPauseCount();
int16_t   GetSimulatorSpeed();
void      SetSimulatorSpeed(int16_t speed);
void      SetSimulatorPaused(bool paused);
void      GetSimulatorClock(uint16_t& outHour, uint16_t& outMin, uint16_t& outSubTick, uint16_t& outDay);
bool      SetSimulatorHour(uint32_t hour24);

// Direct Native Gameplay / Sim / Build Unlocker Helpers (verified via REA in Sims2EP9.exe)
int32_t   GetFamilyFunds();
bool      ModifyFamilyFunds(int32_t delta, int32_t* outNewFunds = nullptr);
size_t    GetLotSimCount();
bool      IsMotiveDecayLocked();
size_t    SetMotiveDecayLocked(bool locked);
bool      IsAspirationLocked();
size_t    SetAspirationLocked(bool locked);
bool      SetSelectedSimAspirationLevel(int level0to5); // 0=Failure..5=Platinum
bool      AddSelectedSimAspirationPoints(int32_t pointsDelta);
size_t    UnlockAllCareerRewardsOnLot();
bool      TriggerForceTwinsOnSelectedSim();
uintptr_t GetSelectedSimPtr(); // cITSPerson* of the Sim this player is currently controlling (0 if none)
bool      CycleSelectedLotSim(); // Switches control to the next playable Sim on the lot
bool      TogglePlumbBobVisibility();
bool      SetShowHeadlinesEnabled(bool enabled);
bool      SetNativeBoolProperty(const char* propName, bool enabled);

} // namespace ts2::se
