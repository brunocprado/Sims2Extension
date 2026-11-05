#pragma once
#include <stdint.h>

namespace ts2::addr {

// ============================================================================
// Image Base & Dynamic Rebase (ASLR / Wine / Proton Relocation Support)
// Static PE ImageBase of Sims2EP9.exe (v1.0.0.153) is 0xA0000000,
// but Wine/Proton relocates it (e.g. to 0x00120000 or 0x00400000) via .reloc!
// Always convert static VMAs with RVA(staticVma) at runtime!
// ============================================================================
constexpr uintptr_t kDefaultImageBase = 0xA0000000u;
constexpr uintptr_t kTextBase         = 0xA0001000u;
constexpr uintptr_t kTextSize         = 0x00F471EEu;
constexpr uintptr_t kRdataBase        = 0xA0F49000u;
constexpr uintptr_t kRdataSize        = 0x0029C9A8u;
constexpr uintptr_t kDataBase         = 0xA11E6000u;
constexpr uintptr_t kDataSize         = 0x000FCE00u;

inline uintptr_t& MutableRuntimeBase() {
    static uintptr_t s_base = kDefaultImageBase;
    return s_base;
}

inline uintptr_t RuntimeBase() {
    return MutableRuntimeBase();
}

inline void SetRuntimeBase(uintptr_t base) {
    if (base != 0) {
        MutableRuntimeBase() = base;
    }
}

// Translates a static VMA (0xA0xxxxxx) to the actual relocated runtime address
inline uintptr_t Rebase(uintptr_t staticVma) {
    return (staticVma - kDefaultImageBase) + MutableRuntimeBase();
}

// ============================================================================
// Import Address Table (IAT) Entries (.rdata) & nGZGraphic4::cDeviceDX
// ============================================================================
constexpr uintptr_t kIAT_CreateMutexA     = 0xA0F493ACu;
constexpr uintptr_t kIAT_WaitMessage      = 0xA0F49620u;
constexpr uintptr_t kIAT_PeekMessageA     = 0xA0F4961Cu;
constexpr uintptr_t kIAT_CreateWindowExA  = 0xA0F496B0u;
constexpr uintptr_t kIAT_Direct3DCreate9  = 0xA0F4974Cu;
constexpr uintptr_t kThunk_Direct3DCreate9 = 0xA09F5758u;

// nGZGraphic4::cDeviceDX (vtable = 0xA0F4D518):
//   this + 0x38 = IDirect3DDevice9*
//   this + 0x6C = int32_t m_sceneDepth (incremented on BeginScene, decremented on EndScene)
//   0xA0167B00  = cDeviceDX::BeginScene()
//   0xA0169920  = cDeviceDX::EndScene()
//   0xA0169C70  = cDeviceDX::OnTickMessage(cGZMessage*) -> calls Present() every frame
constexpr uintptr_t kVTable_cDeviceDX              = 0xA0F4D518u;
constexpr uintptr_t kFunc_cDeviceDX_BeginScene     = 0xA0167B00u;
constexpr uintptr_t kFunc_cDeviceDX_EndScene       = 0xA0169920u;
constexpr uintptr_t kFunc_cDeviceDX_OnTickPresent  = 0xA0169C70u;
constexpr uintptr_t kOffset_cDeviceDX_D3DDevice    = 0x38u;
constexpr uintptr_t kOffset_cDeviceDX_SceneDepth   = 0x6Cu;

// ============================================================================
// Multi-Instance & Background Simulation Patch Sites
// ============================================================================
constexpr uintptr_t kPatch_BgSimSkipWaitMessage = 0xA01438A3u;
constexpr uintptr_t kPatch_BgSimAlwaysOnIdle    = 0xA01438E0u;
constexpr uintptr_t kPatch_MultiInstanceBypass  = 0xA0DC1CB0u;
constexpr uintptr_t kFunc_cRZSysMutex_Ctor      = 0xA0E06DC8u;

// ============================================================================
// Gonzo / Rizzo Framework & Global Singletons
// ============================================================================
constexpr uintptr_t kFunc_GZFramework           = 0xA014350Eu; // cIGZCOM* __cdecl GZFramework()
constexpr uintptr_t kFunc_TSGetGlobals          = 0xA020E0CBu; // cITSGlobals* __cdecl TSGetGlobals()
constexpr uintptr_t kFunc_TSGetSimulator        = 0xA020E26Au; // cITSSimulator* __cdecl TSGetSimulator()
constexpr uintptr_t kFunc_TSGetLot              = 0xA020E0B0u; // cITSLot* __cdecl TSGetLot()
constexpr uintptr_t kFunc_TSGetNeighborhoodMgr  = 0xA020E0F6u; // cITSNeighborhoodManager* __cdecl TSGetNeighborhoodMgr()
constexpr uintptr_t kFunc_TSGetManipulatorSys   = 0xA020E121u; // void* __cdecl TSGetManipulatorSystem()
constexpr uintptr_t kFunc_TSGetPersistSystem    = 0xA020E064u; // cITSPersistSystem* __cdecl TSGetPersistSystem()
constexpr uintptr_t kFunc_TSGetSimSystem        = 0xA020E23Fu; // void* __cdecl TSGetSimSystem() (+0xDC = cRZRandom*)
constexpr uintptr_t kGlobal_pTSGlobals          = 0xA12E5D70u; // Cached cTSGlobals*
constexpr uintptr_t kGlobal_pITSGlobals         = 0xA12E5BA0u; // Cached cITSGlobals*
constexpr uintptr_t kGlobal_pTSSimulator        = 0xA12E5BC4u; // Cached cITSSimulator*

// Native Property & Cheat System Helpers
constexpr uintptr_t kFunc_SetTestingCheats      = 0xA0DC8199u; // void __stdcall SetTestingCheatsEnabled(uint8_t enabled)
constexpr uintptr_t kFunc_ConstructPropKey      = 0xA01BEEDEu; // void* __thiscall ConstructPropKey(void* key24, const char* name)
constexpr uintptr_t kFunc_DestructPropKey       = 0xA01BFA23u; // void __thiscall DestructPropKey(void* key24)
constexpr uintptr_t kVTable_BoolPropertyVariant = 0xA0F4B710u; // vtable for stack bool property variant
constexpr uint32_t  kCLSID_CommandParser        = 0x2A4915EAu;
constexpr uint32_t  kIID_cIGZCommandParser      = 0x4A490E00u;

// GZCOM Service GUIDs
constexpr uint32_t kCLSID_GlobalPropertySet     = 0x0AC1887Eu;
constexpr uint32_t kIID_cIGZPropertySet         = 0x4BB0D269u;
constexpr uint32_t kCLSID_cTSGlobals            = 0x9A353CE8u;
constexpr uint32_t kIID_cITSGlobals             = 0xFA353CDBu;
constexpr uint32_t kCLSID_cTSInteraction        = 0xFC5ECC3Du;
constexpr uint32_t kIID_cITSInteraction         = 0x5ACA6692u;

// ============================================================================
// cTSSimulator & Deterministic PRNG (cRZRandom)
// ============================================================================
constexpr uintptr_t kVTable_cTSSimulator_Msg    = 0xA0F79A0Cu;
constexpr uintptr_t kVTable_cITSSimulator       = 0xA0F79A24u;

constexpr uintptr_t kFunc_cTSSimulator_OnIdle            = 0xA04829B0u; // vtable[10] (+0x28)
constexpr uintptr_t kFunc_cTSSimulator_SimulateAllObjects = 0xA0483390u; // vtable[11] (+0x2C)
constexpr uintptr_t kFunc_cTSSimulator_SimulateTick      = 0xA0482F90u; // vtable[12] (+0x30)
constexpr uintptr_t kFunc_cTSSimulator_GetTickCount      = 0xA024C050u; // vtable[18] (+0x48)
constexpr uintptr_t kFunc_cTSSimulator_SetState          = 0xA0482730u; // vtable[20] (+0x50)
constexpr uintptr_t kFunc_cTSSimulator_GetState          = 0xA0480F20u; // vtable[21] (+0x54)
constexpr uintptr_t kFunc_cTSSimulator_Pause             = 0xA0481490u; // vtable[24] (+0x60)
constexpr uintptr_t kFunc_cTSSimulator_Unpause           = 0xA0482180u; // vtable[25] (+0x64)
constexpr uintptr_t kFunc_cTSSimulator_IsPaused          = 0xA0481220u; // vtable[26] (+0x68)
constexpr uintptr_t kFunc_cTSSimulator_GetSpeed          = 0xA0480D90u; // vtable[27] (+0x6C)
constexpr uintptr_t kFunc_cTSSimulator_SetSpeed          = 0xA04826C0u; // vtable[28] (+0x70)

constexpr uintptr_t kFunc_cRZRandom_NextUint32  = 0xA01B919Bu;
constexpr uintptr_t kFunc_cRZRandom_NextRange   = 0xA01B91B4u;

// Offsets inside cITSSimulator* (cTSSimulator + 0x04)
namespace sim_off {
    constexpr uintptr_t kHour            = 0x10; // uint16_t (0..23)
    constexpr uintptr_t kDay             = 0x12; // uint16_t (1..30)
    constexpr uintptr_t kMinute          = 0x1A; // uint16_t (0..59)
    constexpr uintptr_t kSubTick         = 0x1C; // uint16_t (0..58, +2 per tick)
    constexpr uintptr_t kMonth           = 0x1E; // uint16_t (1..12)
    constexpr uintptr_t kYear            = 0x20; // uint16_t
    constexpr uintptr_t kSimulatorState  = 0x30; // int16_t (1=Live, 2=Buy, 3=Build)
    constexpr uintptr_t kPauseCount      = 0x32; // int16_t (>0 = paused)
    constexpr uintptr_t kSpeedMultiplier = 0x36; // int16_t
    constexpr uintptr_t kTotalTicks64    = 0x88; // uint64_t
    constexpr uintptr_t kLotTickCount    = 0x90; // uint32_t
    constexpr uintptr_t kRandomSeed      = 0xA4; // uint32_t (Deterministic PRNG seed!)
    constexpr uintptr_t kCurrentObjectID = 0x114; // uint32_t
}

// ============================================================================
// SimAntics VM (cTSTreeSim, cTSObject, cTSPerson, cTSTreeStackElem)
// ============================================================================
constexpr uintptr_t kVTable_cTSTreeSim          = 0xA0F868A4u;
constexpr uintptr_t kVTable_cEdithObject        = 0xA0F869C0u;
constexpr uintptr_t kVTable_cTSObject           = 0xA0F81F9Cu;
constexpr uintptr_t kVTable_cTSObject_TreeSim   = 0xA0F8253Cu;
constexpr uintptr_t kVTable_cTSPerson           = 0xA0F7DDD8u;
constexpr uintptr_t kVTable_cTSPerson_TreeSim   = 0xA0F7E39Cu;
constexpr uintptr_t kVTable_cTSTreeStackElem    = 0xA0F867ACu;

constexpr uintptr_t kFunc_cTSTreeSim_ExecuteStep            = 0xA0576500u;
constexpr uintptr_t kFunc_cTSObject_ExecutePrimitive        = 0xA053E780u;
constexpr uintptr_t kFunc_cTSObject_ExecuteObjectPrimitive  = 0xA0534880u;
constexpr uintptr_t kFunc_cTSPerson_ExecutePersonPrimitive  = 0xA051E700u;
constexpr uintptr_t kGlobal_MaxTreeIterations               = 0xA12E0000u - 0x000CE6ECu; // 0xA1211914u

// ---- Object placement, Buy/Build creation/deletion & Sim sync (verified via REA in Sims2EP9.exe) ----
// Object table = cEdithObjectModule (the object returned by TSGetGlobals()->vtable[0x68]; vtable 0xA0F73188).
//   vtable slot 16 (+0x40): int16_t CreateObjectInstance(cITSObjectSelector* sel, int, int, int) `ret 0x10`
//   vtable slot 18 (+0x48): void    DeleteObject(int16_t id) `ret 4`
//   vtable slot 30 (+0x78): cTSObject* GetObject(int id)  thiscall `ret 4`; ids are 1-based and objects flagged
//   0x10000/0x20000 (deleted/ghost) are filtered out. Pointer vector: begin at +0x1A24, end at +0x1A28.
//   vtable slot 39 (+0x9C): cITSPerson* GetSelectedPerson()
//   vtable slot 40 (+0xA0): void SelectPerson(cITSPerson* pPerson) `ret 4`
constexpr uintptr_t kVTable_cEdithObjectModule  = 0xA0F73188u;
constexpr uintptr_t kVTable_cTSObjectFolder     = 0xA0F7C004u;
constexpr uintptr_t kVTable_cTSObjectSelector   = 0xA0F7C22Cu;
constexpr uintptr_t kVTable_cITSPerson          = 0xA0F7E4C0u;
constexpr uintptr_t kObjTable_VecBegin          = 0x1A24;
constexpr uintptr_t kObjTable_VecEnd            = 0x1A28;
constexpr uintptr_t kObjTable_PersonBegin       = 0x1A70;
constexpr uintptr_t kObjTable_PersonEnd         = 0x1A74;
constexpr uintptr_t kObjTable_CreateObjectSlot  = 16; // +0x40
// vtable[18] (0xA0423410) is the raw delete and takes TWO stack args (id, flag); calling it with one corrupts the stack.
// vtable[19] (0xA0423790, ret 4) is the checked wrapper the game itself uses: Delete(id).
constexpr uintptr_t kObjTable_DeleteObjectSlot  = 19; // +0x4C
constexpr uintptr_t kObjTable_GetObjectSlot     = 30; // +0x78
constexpr uintptr_t kObjTable_GetSelectedSlot   = 39; // +0x9C
constexpr uintptr_t kObjTable_SelectPersonSlot  = 40; // +0xA0
constexpr uintptr_t kFunc_TilePosToWorld        = 0xA07DCE80u; // void __cdecl(float out[3], const int32 tile[2], int level)
constexpr uintptr_t kFunc_GZGetAllocatorCOM     = 0xA01AE45Du; // cIGZCOM* __cdecl() -> vt[3 (+0x0C)] creates cTSInteraction
constexpr uint32_t  kGUID_GoHereObject          = 0x000007C4u; // "Walk Here / Go Here" temporary target selector GUID

namespace obj_off {
    constexpr uintptr_t kEditNest    = 0x084; // int16 Begin/End nesting counter
    constexpr uintptr_t kInWorld     = 0x086; // uint8 (0 = out-of-world, !=0 = placed on lot)
    constexpr uintptr_t kRotation    = 0x0A8; // uint16 (0..7). Setter takes B=(10-rot)&7 by pointer.
    constexpr uintptr_t kContainerID = 0x0AA; // int16 (1-based Object ID of container/holder, 0 = none)
    constexpr uintptr_t kObjectID    = 0x1CC; // int16 (1-based Object ID in cEdithObjectModule)
    constexpr uintptr_t kTileX       = 0x1D0; // int32 (16.16 fixed-point tile X; -65536 = out-of-world)
    constexpr uintptr_t kTileY       = 0x1D4; // int32 (16.16 fixed-point tile Y; -65536 = out-of-world)
    constexpr uintptr_t kLevel       = 0x1D8; // int16 (floor level 1..16)
    constexpr uintptr_t kFlags       = 0x230; // uint32 (0x10000 / 0x20000 = deleted/ghost)
    constexpr uintptr_t kContainerPtr= 0x23C; // cTSObject* parent/container
    constexpr uintptr_t kSelectorPtr = 0x240; // cITSObjectSelector* catalog definition
}
namespace obj_vt {            // byte offsets into the cTSObject vtable
    constexpr uintptr_t kGetLeadObject  = 0x17C; // cTSObject* __thiscall() (vt[95]: returns lead object for multi-tile)
    constexpr uintptr_t kGetObjectType  = 0x188; // int __thiscall() (vt[98]: 2=Person, 4=Normal, 7=Portal, 8=Door, 9=Window, 11=Jig)
    constexpr uintptr_t kSetOrientation = 0x2A8; // void __thiscall(int32* B, int)      `ret 8`
    constexpr uintptr_t kSetLocation    = 0x2AC; // void __thiscall(float* pos3, float, int) `ret 0xC`
    constexpr uintptr_t kBeginEdit      = 0x35C; // void __thiscall()
    constexpr uintptr_t kEndEdit        = 0x360; // void __thiscall()
}
namespace person_off {        // offsets relative to cITSPerson* (cTSPerson + 0x2F0)
    constexpr uintptr_t kSubobjectOffset = 0x2F0; // cITSPerson* = cTSPerson* + 0x2F0
    constexpr uintptr_t kPersonDataBase  = 0x356; // int16_t[] PersonData array
    constexpr uintptr_t kNeighborID      = 0x394; // int16_t PersonData[0x1F] (unique Sim NeighborID)
    constexpr uintptr_t kMotivesPtr      = 0x524; // cTSMotives* (+0x48 = float[16] motives)
}

// Offsets inside cTSTreeStackElem (vt = 0xA0F867AC)
namespace stack_off {
    constexpr uintptr_t kStackObjectID   = 0x08; // uint16_t (& 0x7FFF)
    constexpr uintptr_t kTargetObjectID  = 0x0A; // uint16_t
    constexpr uintptr_t kCurrentNodeID   = 0x0C; // uint16_t
    constexpr uintptr_t kBehaviorTree    = 0x10; // cITSBehaviorTree*
    constexpr uintptr_t kNumLocals       = 0x14; // uint8_t
    constexpr uintptr_t kNumParams       = 0x15; // uint8_t
    constexpr uintptr_t kParamsPtr       = 0x24; // int16_t*
    constexpr uintptr_t kLocalsPtr       = 0x30; // int16_t*
}

// ============================================================================
// Interactions (Pie Menu Queue), Motives & Cheats
// ============================================================================
constexpr uintptr_t kVTable_cTSInteractionContainer     = 0xA0F7751Cu;
constexpr uintptr_t kVTable_cTSInteraction              = 0xA0F773D4u;
constexpr uintptr_t kFunc_InteractionContainer_Push     = 0xA044F9B0u;
constexpr uintptr_t kFunc_InteractionContainer_GetAt    = 0xA0451780u;
constexpr uintptr_t kFunc_InteractionContainer_GetCount = 0xA0153DA0u;
constexpr uintptr_t kFunc_InteractionContainer_Enqueue  = 0xA044FA30u;
constexpr uintptr_t kFunc_cTSInteraction_Init           = 0xA04515A0u; // vt[8 (+0x20)]: bool __thiscall(cITSPerson*, cTSObject*, int idx, int prio, int flags) `ret 0x14`
constexpr uintptr_t kFunc_cTSPerson_EnqueueInteraction  = 0xA0512360u; // cITSPerson::vt[122 (+0x1E8)]: bool __thiscall(cITSInteraction*) `ret 4` (5-byte prologue: 55 8B EC 6A FF)

namespace inter_off {
    constexpr uintptr_t kActorObjectID    = 0x0C; // int16_t (1-based Sim Object ID)
    constexpr uintptr_t kTargetObjectID   = 0x0E; // int16_t (1-based Target Object ID)
    constexpr uintptr_t kIconObjectID     = 0x10; // int16_t
    constexpr uintptr_t kInteractionIndex = 0x14; // int32_t (Pie Menu / TTAB index)
    constexpr uintptr_t kParams           = 0x18; // int16_t[4] (0x18, 0x1A, 0x1C, 0x1E)
    constexpr uintptr_t kPriority         = 0x20; // int32_t (0x32=50 user, 0x14=20 autonomous)
    constexpr uintptr_t kActionTreeID     = 0x24; // int16_t (BHAV tree ID)
    constexpr uintptr_t kAttenuation      = 0x28; // float
    constexpr uintptr_t kFlags            = 0x68; // uint32_t (bit 0 = autonomous, bit 1 = continuation)
}

constexpr uintptr_t kVTable_cTSMotives                  = 0xA0F73B3Cu;
constexpr uintptr_t kFunc_cTSMotives_GetMotive          = 0xA0436B70u;
constexpr uintptr_t kFunc_cTSMotives_SetMotive          = 0xA0437000u;
constexpr uintptr_t kOffset_cTSMotives_Array            = 0x48u;

constexpr uintptr_t kVTable_cTSCheatSystem              = 0xA108F264u;
constexpr uintptr_t kFunc_CheatSystem_Register          = 0xA0CA1C20u;
constexpr uintptr_t kFunc_CheatSystem_ExecuteString     = 0xA0C9F770u;

} // namespace ts2::addr
