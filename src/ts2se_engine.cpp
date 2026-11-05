#include "ts2/ts2se_engine.hpp"
#include "ts2/hook_engine.hpp"
#include "ts2/coop_net.hpp"
#include "ts2/engine_qol.hpp"
#include "ts2/dx9_postfx.hpp"
#include <stdarg.h>

namespace ts2::se {

namespace {

// ============================================================================
// Console Log Ring Buffer
// ============================================================================
constexpr size_t kMaxLogLines = 32;
static char   g_logLines[kMaxLogLines][144] = {};
static size_t g_logWriteIdx = 0;
static size_t g_logTotal    = 0;

// ============================================================================
// Shared 16-bit Registers (0..255) for BHAV Primitive 0x0081 & Lua/Console
// ============================================================================
static int16_t g_sharedRegs[256] = {};

// ============================================================================
// Inspected SimAntics Stack Frames & Active Sim Motives
// ============================================================================
static StackFrameSnapshot g_frames[kMaxInspectedFrames] = {};
static size_t             g_frameWriteIdx               = 0;
static size_t             g_frameCount                  = 0;
static uint32_t           g_totalBhavSteps              = 0;
static uintptr_t          g_lastMotivesPtr              = 0;

static const char* const kMotiveNames[16] = {
    "00 Mood",      "01 Energy",    "02 Comfort",   "03 Hunger",
    "04 Hygiene",   "05 Bladder",   "06 Room/Env",  "07 Social",
    "08 Fun",       "09 Amorous",   "10 Shopping",  "11 Stress",
    "12 Chew/Scratch","13 Public",  "14 PlantWater","15 PlantSun"
};

// ============================================================================
// Helper to read/write Locals & Params from cTSTreeStackElem* (vt = 0xA0F867AC)
// ============================================================================
static int16_t ReadStackLocal(void* pStackElem, uint8_t idx) {
    if (!pStackElem) return 0;
    auto base = reinterpret_cast<uintptr_t>(pStackElem);
    uint8_t numLocals = *reinterpret_cast<const uint8_t*>(base + addr::stack_off::kNumLocals);
    if (idx >= numLocals) return 0;
    int16_t* pLocals = *reinterpret_cast<int16_t* const*>(base + addr::stack_off::kLocalsPtr);
    return pLocals ? pLocals[idx] : 0;
}

static void WriteStackLocal(void* pStackElem, uint8_t idx, int16_t val) {
    if (!pStackElem) return;
    auto base = reinterpret_cast<uintptr_t>(pStackElem);
    uint8_t numLocals = *reinterpret_cast<const uint8_t*>(base + addr::stack_off::kNumLocals);
    if (idx >= numLocals) return;
    int16_t* pLocals = *reinterpret_cast<int16_t* const*>(base + addr::stack_off::kLocalsPtr);
    if (pLocals) pLocals[idx] = val;
}

static int16_t ReadStackParam(void* pStackElem, uint8_t idx) {
    if (!pStackElem) return 0;
    auto base = reinterpret_cast<uintptr_t>(pStackElem);
    uint8_t numParams = *reinterpret_cast<const uint8_t*>(base + addr::stack_off::kNumParams);
    if (idx >= numParams) return 0;
    int16_t* pParams = *reinterpret_cast<int16_t* const*>(base + addr::stack_off::kParamsPtr);
    return pParams ? pParams[idx] : 0;
}

// ============================================================================
// 8 Built-in TS2SE Custom SimAntics BHAV Primitives (0x0080 .. 0x0087)
// ============================================================================

// 0x0080: TS2SE_LogStackState
// Logs current StackObjectID, NodeID, Local[0..3], Param[0..1] to TS2SE log
static BhavResult Prim_0080_LogStackState(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)opcode;
    uintptr_t s = reinterpret_cast<uintptr_t>(pStack);
    uint16_t stackObjId = s ? (*reinterpret_cast<uint16_t*>(s + addr::stack_off::kStackObjectID) & 0x7FFFu) : 0;
    uint16_t nodeId     = s ? *reinterpret_cast<uint16_t*>(s + addr::stack_off::kCurrentNodeID) : 0;
    PushConsoleLog("[BHAV 0x0080] Obj=0x%08X StackID=0x%04X Node=%u L0=%d P0=%d Op0=%u",
                   reinterpret_cast<unsigned>(pObj), stackObjId, nodeId,
                   ReadStackLocal(pStack, 0), ReadStackParam(pStack, 0),
                   ops ? ops[0] : 0);
    return BhavResult::TrueBranch;
}

// 0x0081: TS2SE_GetSetGlobalReg
// ops[0] = mode (0=Get Reg[ops[1]] -> Local[ops[2]], 1=Set Reg[ops[1]] = Local[ops[2]], 2=Set Reg[ops[1]] = imm16(ops[2..3]))
static BhavResult Prim_0081_GetSetGlobalReg(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)pObj; (void)opcode;
    if (!ops) return BhavResult::FalseBranch;
    uint8_t mode   = ops[0];
    uint8_t regIdx = ops[1];
    uint8_t locIdx = ops[2];
    if (mode == 0) {
        WriteStackLocal(pStack, locIdx, GetSharedRegister(regIdx));
    } else if (mode == 1) {
        SetSharedRegister(regIdx, ReadStackLocal(pStack, locIdx), true);
    } else {
        int16_t imm = static_cast<int16_t>(uint16_t(ops[2]) | (uint16_t(ops[3]) << 8));
        SetSharedRegister(regIdx, imm, true);
    }
    return BhavResult::TrueBranch;
}

// 0x0082: TS2SE_CoopSendEvent
// Broadcasts custom SimAntics event (eventId = ops[0..1], arg0 = Local[ops[2]], arg1 = Local[ops[3]]) to Co-op peer
static BhavResult Prim_0082_CoopSendEvent(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)pObj; (void)opcode;
    if (!ops) return BhavResult::FalseBranch;
    uint16_t eventId = uint16_t(ops[0]) | (uint16_t(ops[1]) << 8);
    uintptr_t s = reinterpret_cast<uintptr_t>(pStack);
    uint16_t stackObjId = s ? (*reinterpret_cast<uint16_t*>(s + addr::stack_off::kStackObjectID) & 0x7FFFu) : 0;
    int16_t arg0 = ReadStackLocal(pStack, ops[2]);
    int16_t arg1 = ReadStackLocal(pStack, ops[3]);
    ts2::coop::SendBhavEvent(eventId, stackObjId, arg0, arg1);
    return BhavResult::TrueBranch;
}

// 0x0083: TS2SE_CoopIsHost
// Returns TrueBranch (0) if Solo or Host, FalseBranch (1) if Client
static BhavResult Prim_0083_CoopIsHost(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)pObj; (void)pStack; (void)opcode; (void)ops;
    const auto& st = ts2::coop::GetStats();
    return (st.role == ts2::coop::Role::Client) ? BhavResult::FalseBranch : BhavResult::TrueBranch;
}

// 0x0084: TS2SE_MathExt
// Extended fixed-point math on Locals: ops[0]=subOp (0=Sqrt, 1=Sin1000, 2=Cos1000, 3=Min, 4=Max, 5=Clamp), ops[1]=dstLocal, ops[2]=srcLocalA, ops[3]=srcLocalB
static BhavResult Prim_0084_MathExt(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)pObj; (void)opcode;
    if (!ops) return BhavResult::FalseBranch;
    uint8_t subOp = ops[0];
    int16_t a = ReadStackLocal(pStack, ops[2]);
    int16_t b = ReadStackLocal(pStack, ops[3]);
    int16_t res = 0;
    switch (subOp) {
        case 0: res = (a > 0) ? static_cast<int16_t>(sqrt(static_cast<double>(a))) : 0; break;
        case 1: res = static_cast<int16_t>(sin(static_cast<double>(a) * 0.0174532925) * 1000.0); break;
        case 2: res = static_cast<int16_t>(cos(static_cast<double>(a) * 0.0174532925) * 1000.0); break;
        case 3: res = (a < b) ? a : b; break;
        case 4: res = (a > b) ? a : b; break;
        default: res = a; break;
    }
    WriteStackLocal(pStack, ops[1], res);
    return BhavResult::TrueBranch;
}

// 0x0085: TS2SE_GetSetMotiveDirect
// ops[0] = mode (0=Get Motive[ops[1]] -> Local[ops[2]], 1=Set Motive[ops[1]] = Local[ops[2]])
static BhavResult Prim_0085_GetSetMotiveDirect(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)pObj; (void)opcode;
    if (!ops) return BhavResult::FalseBranch;
    int motiveIdx = ops[1] & 0x0F;
    if (ops[0] == 0) {
        int16_t val = static_cast<int16_t>(GetSimMotive(motiveIdx));
        WriteStackLocal(pStack, ops[2], val);
    } else {
        float val = static_cast<float>(ReadStackLocal(pStack, ops[2]));
        SetSimMotive(motiveIdx, val);
    }
    return BhavResult::TrueBranch;
}

// 0x0086: TS2SE_ExecCommand
// Executes a preset cheat/script command based on ops[0]
static BhavResult Prim_0086_ExecCommand(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)pObj; (void)pStack; (void)opcode;
    static const char* const kPresetCmds[4] = {
        "kaching",
        "motherlode",
        "maxMotives",
        "boolProp testingCheatsEnabled true"
    };
    uint8_t idx = ops ? (ops[0] & 3u) : 0;
    char out[128];
    ExecuteCommand(kPresetCmds[idx], out, sizeof(out));
    return BhavResult::TrueBranch;
}

// 0x0087: TS2SE_DeterministicRand
// Draws a deterministic 16-bit value from cTSSimulator::m_uRandomSeed (+0xA4) modulo ops[1..2] into Local[ops[0]]
static BhavResult Prim_0087_DeterministicRand(void* pObj, void* pStack, uint16_t opcode, const uint8_t* ops) {
    (void)pObj; (void)opcode;
    if (!ops) return BhavResult::FalseBranch;
    uint32_t seed = GetSimulatorRandomSeed();
    seed = seed * 0x0019660Du + 0x3C6EF35Fu;
    SetSimulatorRandomSeed(seed);
    uint16_t range = uint16_t(ops[1]) | (uint16_t(ops[2]) << 8);
    if (range == 0) range = 100;
    int16_t val = static_cast<int16_t>((seed >> 16) % range);
    WriteStackLocal(pStack, ops[0], val);
    return BhavResult::TrueBranch;
}

static CustomPrimitiveInfo g_customPrimitives[128] = {};
static size_t              g_customPrimCount       = 0;

static CustomPrimitiveInfo* FindCustomPrimitive(uint16_t opcode) {
    for (size_t i = 0; i < g_customPrimCount; ++i) {
        if (g_customPrimitives[i].opcode == opcode) {
            return &g_customPrimitives[i];
        }
    }
    return nullptr;
}

// ============================================================================
// SimAntics VM Hooks:
//   1. cTSObject::ExecutePrimitive (0xA053E780) — intercepts opcodes 0x0080..0x00FF
//   2. cTSTreeSim::ExecuteStep (0xA0576500) — captures live stack frames for Inspector
//   3. cTSMotives::GetMotive (vtable[10] @ 0xA0F73B3C + 0x28) — tracks active Sim motives
// ============================================================================

static ts2::hook::Detour g_detourExecutePrimitive;
static ts2::hook::Detour g_detourExecuteStep;

// 0xA0534880: bool __thiscall cTSObject::ExecuteObjectPrimitive(cTSTreeStackElem* pStackElem, cITSTreeNode* pNode, int32_t* pOutResult)
using Fn_ExecuteObjectPrimitive = uint8_t (__thiscall*)(void* pObject, void* pStackElem, void* pNode, int32_t* pOutResult);
// 0xA0576500: int __thiscall cTSTreeSim::ExecuteStep(cTSObject* pObj, int a2)  [ret 0x8!]
using Fn_ExecuteStep            = int (__thiscall*)(void* pTreeSim, void* pObj, int a2);
using Fn_GetMotive              = float (__thiscall*)(void* pMotives, int index);

static Fn_GetMotive g_origGetMotive = nullptr;
static bool         g_motiveDecayLocked = false;

// Helper to call pNode->GetOpcode() (vtable + 0x0C) and pNode->GetOperands() (vtable + 0x40)
static uint16_t GetNodeOpcode(void* pNode) {
    if (!pNode) return 0;
    void** vt = *reinterpret_cast<void***>(pNode);
    if (!vt) return 0;
    using Fn = uint16_t (__thiscall*)(void*);
    return reinterpret_cast<Fn>(vt[0x0C / 4])(pNode);
}

static const uint8_t* GetNodeOperands(void* pNode) {
    if (!pNode) return nullptr;
    void** vt = *reinterpret_cast<void***>(pNode);
    if (!vt) return nullptr;
    using Fn = const uint8_t* (__thiscall*)(void*);
    return reinterpret_cast<Fn>(vt[0x40 / 4])(pNode);
}

static void RecordStackFrame(void* pObject, void* pStackElem, uint16_t opcode) {
    if (!pStackElem) return;
    uintptr_t s = reinterpret_cast<uintptr_t>(pStackElem);
    StackFrameSnapshot& snap = g_frames[g_frameWriteIdx % kMaxInspectedFrames];
    snap.stackElemPtr   = s;
    snap.objectPtr      = reinterpret_cast<uintptr_t>(pObject);
    snap.stackObjectID  = *reinterpret_cast<uint16_t*>(s + addr::stack_off::kStackObjectID) & 0x7FFFu;
    snap.targetObjectID = *reinterpret_cast<uint16_t*>(s + addr::stack_off::kTargetObjectID);
    snap.currentNodeID  = *reinterpret_cast<uint16_t*>(s + addr::stack_off::kCurrentNodeID);
    snap.lastOpcode     = opcode;
    snap.numLocals      = *reinterpret_cast<uint8_t*>(s + addr::stack_off::kNumLocals);
    snap.numParams      = *reinterpret_cast<uint8_t*>(s + addr::stack_off::kNumParams);
    snap.captureTick    = GetSimulatorTickCount();

    for (uint8_t i = 0; i < 8; ++i) {
        snap.locals[i] = (i < snap.numLocals) ? ReadStackLocal(pStackElem, i) : 0;
    }
    for (uint8_t i = 0; i < 4; ++i) {
        snap.params[i] = (i < snap.numParams) ? ReadStackParam(pStackElem, i) : 0;
    }

    ++g_frameWriteIdx;
    if (g_frameCount < kMaxInspectedFrames) ++g_frameCount;
}

static uint8_t __fastcall Hooked_cTSObject_ExecuteObjectPrimitive(
    void* pObject, void* /*edx*/, void* pStackElem, void* pNode, int32_t* pOutResult)
{
    uint16_t opcode = GetNodeOpcode(pNode);
    if ((g_totalBhavSteps & 0x1Fu) == 0 || (opcode >= 0x0080 && opcode <= 0x00FF)) {
        RecordStackFrame(pObject, pStackElem, opcode);
    }

    // Custom TS2SE Primitives live in [0x0080, 0x00FF] (since native primitives end at 0x007E in 0xA05348C0!)
    if (opcode >= 0x0080 && opcode <= 0x00FF) {
        CustomPrimitiveInfo* info = FindCustomPrimitive(opcode);
        if (info && info->handler) {
            ++info->callCount;
            const uint8_t* operands = GetNodeOperands(pNode);
            BhavResult res = info->handler(pObject, pStackElem, opcode, operands);
            if (pOutResult) {
                *pOutResult = static_cast<int32_t>(res);
            }
            return 1;
        }
    }

    return g_detourExecutePrimitive.Original<Fn_ExecuteObjectPrimitive>()(pObject, pStackElem, pNode, pOutResult);
}

static int __fastcall Hooked_cTSTreeSim_ExecuteStep(
    void* pTreeSim, void* /*edx*/, void* pObj, int a2)
{
    ++g_totalBhavSteps;
    return g_detourExecuteStep.Original<Fn_ExecuteStep>()(pTreeSim, pObj, a2);
}

static float __fastcall Hooked_cTSMotives_GetMotive(void* pMotives, void* /*edx*/, int index) {
    if (pMotives) {
        g_lastMotivesPtr = reinterpret_cast<uintptr_t>(pMotives);
    }
    if (g_motiveDecayLocked && index >= 1 && index <= 9 && index != 2 && index != 6) {
        return 100.0f;
    }
    return g_origGetMotive ? g_origGetMotive(pMotives, index) : 0.0f;
}

// ============================================================================
// Native Sims2EP9.exe Helpers (Reverse Engineered via REA)
// ============================================================================

// Iterates all cTSPerson* on the current lot via TSGetGlobals()->GetObjectFolder()->GetPersonList() (0xA0418700)
template <typename Callback>
static size_t ForEachLotPerson(Callback&& cb) {
    uintptr_t pITSGlobals = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pITSGlobals));
    if (!pITSGlobals) {
        using FnGetGlobals = uintptr_t (__cdecl*)();
        pITSGlobals = reinterpret_cast<FnGetGlobals>(addr::Rebase(addr::kFunc_TSGetGlobals))();
    }
    if (!pITSGlobals) return 0;

    void** globalsVt = *reinterpret_cast<void***>(pITSGlobals);
    if (!globalsVt) return 0;
    using FnGetObjFolder = void* (__thiscall*)(void*);
    void* pObjFolder = reinterpret_cast<FnGetObjFolder>(globalsVt[0x68 / 4])(reinterpret_cast<void*>(pITSGlobals));
    if (!pObjFolder) return 0;

    void** folderVt = *reinterpret_cast<void***>(pObjFolder);
    if (!folderVt) return 0;
    using FnGetPersonVec = uintptr_t* (__thiscall*)(void*);
    uintptr_t* pVec = reinterpret_cast<FnGetPersonVec>(folderVt[0xB4 / 4])(pObjFolder);
    if (!pVec) return 0;

    uintptr_t* begin = reinterpret_cast<uintptr_t*>(pVec[0]);
    uintptr_t* end   = reinterpret_cast<uintptr_t*>(pVec[1]);
    if (!begin || !end || end < begin || (end - begin) > 512) return 0;

    size_t count = 0;
    for (uintptr_t* it = begin; it < end; ++it) {
        void* pPerson = reinterpret_cast<void*>(*it);
        if (pPerson) {
            cb(pPerson);
            ++count;
        }
    }
    return count;
}

// Native cIGZCommandParser execution (0xA0E44C51: CLSID 0x2A4915EA, IID 0x4A490E00, vtable[7] +0x1C)
static bool ExecuteNativeCommandParser(const char* cmdLine) {
    if (!cmdLine || !*cmdLine) return false;
    using FnGZFramework = void* (__cdecl*)();
    void* pFramework = reinterpret_cast<FnGZFramework>(addr::Rebase(addr::kFunc_GZFramework))();
    if (!pFramework) return false;

    void** fwVt = *reinterpret_cast<void***>(pFramework);
    if (!fwVt) return false;

    void* pParser = nullptr;
    using FnGetService = bool (__thiscall*)(void*, uint32_t, uint32_t, void**);
    reinterpret_cast<FnGetService>(fwVt[0x14 / 4])(
        pFramework, addr::kCLSID_CommandParser, addr::kIID_cIGZCommandParser, &pParser);
    if (!pParser) return false;

    void** parserVt = *reinterpret_cast<void***>(pParser);
    using FnExecCmd = bool (__thiscall*)(void*, const char*);
    using FnRelease = int  (__thiscall*)(void*);
    bool ok = reinterpret_cast<FnExecCmd>(parserVt[0x1C / 4])(pParser, cmdLine);
    reinterpret_cast<FnRelease>(parserVt[0x08 / 4])(pParser);
    return ok;
}

// Native cIGZPropertySet boolProp setter (0xA01D0D60 / 0xA0771CEE)
static bool SetNativeBoolProp(const char* propName, bool enabled) {
    if (!propName || !*propName) return false;
    using FnGZFramework = void* (__cdecl*)();
    void* pFramework = reinterpret_cast<FnGZFramework>(addr::Rebase(addr::kFunc_GZFramework))();
    if (!pFramework) return false;

    void** fwVt = *reinterpret_cast<void***>(pFramework);
    if (!fwVt) return false;

    void* pPropSys = nullptr;
    using FnGetService = bool (__thiscall*)(void*, uint32_t, uint32_t, void**);
    reinterpret_cast<FnGetService>(fwVt[0x14 / 4])(
        pFramework, addr::kCLSID_GlobalPropertySet, addr::kIID_cIGZPropertySet, &pPropSys);
    if (!pPropSys) return false;

    uint8_t key24[32] = {};
    using FnConstructKey = void* (__thiscall*)(void*, const char*);
    using FnDestructKey  = void  (__thiscall*)(void*);
    reinterpret_cast<FnConstructKey>(addr::Rebase(addr::kFunc_ConstructPropKey))(key24, propName);

    struct BoolVariant {
        uintptr_t vtable;
        uint8_t   val;
        uint8_t   pad[3];
    } var = { addr::Rebase(addr::kVTable_BoolPropertyVariant), static_cast<uint8_t>(enabled ? 1 : 0), {0, 0, 0} };

    void** propVt = *reinterpret_cast<void***>(pPropSys);
    using FnSetProp = void (__thiscall*)(void*, void*, void*);
    using FnRelease = int  (__thiscall*)(void*);
    reinterpret_cast<FnSetProp>(propVt[0x0C / 4])(pPropSys, key24, &var);
    reinterpret_cast<FnDestructKey>(addr::Rebase(addr::kFunc_DestructPropKey))(key24);
    reinterpret_cast<FnRelease>(propVt[0x08 / 4])(pPropSys);
    return true;
}

// Native moveObjects on/off handler (0xA07746AE)
static bool SetNativeMoveObjects(bool enabled) {
    using FnGetSys = void* (__cdecl*)();
    void* pSimSys   = reinterpret_cast<FnGetSys>(addr::Rebase(addr::kFunc_TSGetSimSystem))();
    void* pManipSys = reinterpret_cast<FnGetSys>(addr::Rebase(addr::kFunc_TSGetManipulatorSys))();
    uint8_t flag    = enabled ? 1 : 0;
    if (pSimSys) {
        void** vt = *reinterpret_cast<void***>(pSimSys);
        using FnSet = void (__thiscall*)(void*, uint8_t);
        if (vt) reinterpret_cast<FnSet>(vt[0xA8 / 4])(pSimSys, flag);
    }
    if (pManipSys) {
        void** vt = *reinterpret_cast<void***>(pManipSys);
        using FnSet = void (__thiscall*)(void*, uint8_t);
        if (vt) reinterpret_cast<FnSet>(vt[0xCC / 4])(pManipSys, flag);
    }
    return (pSimSys != nullptr || pManipSys != nullptr);
}

// Native family funds setter for kaching (+1000) / motherlode (+50000) (0xA041847E)
static bool AddFamilyFunds(int32_t delta, int32_t* outNewFunds = nullptr) {
    uintptr_t sim = GetSimulatorPtr();
    if (!sim) return false;
    void** simVt = *reinterpret_cast<void***>(sim);
    if (!simVt) return false;

    using FnGetFunds = int32_t (__thiscall*)(void*);
    using FnSetFunds = void    (__thiscall*)(void*, int32_t);
    int32_t cur = reinterpret_cast<FnGetFunds>(simVt[0x74 / 4])(reinterpret_cast<void*>(sim));
    int32_t next = cur + delta;
    reinterpret_cast<FnSetFunds>(simVt[0x78 / 4])(reinterpret_cast<void*>(sim), next);

    // Refresh UI Simoleon counter via TSGetManipulatorSystem()->vtable[4]()
    using FnGetSys = void* (__cdecl*)();
    void* pManipSys = reinterpret_cast<FnGetSys>(addr::Rebase(addr::kFunc_TSGetManipulatorSys))();
    if (pManipSys) {
        void** vt = *reinterpret_cast<void***>(pManipSys);
        using FnRefresh = void (__thiscall*)(void*);
        if (vt) reinterpret_cast<FnRefresh>(vt[0x10 / 4])(pManipSys);
    }
    if (outNewFunds) *outNewFunds = next;
    return true;
}

// Native motiveDecay on/off handler (0xA0418B5E)
static size_t SetNativeMotiveDecayLocked(bool locked) {
    g_motiveDecayLocked = locked;
    return ForEachLotPerson([locked](void* pPerson) {
        void** vt = *reinterpret_cast<void***>(pPerson);
        if (!vt) return;
        using FnSetPersonData = void (__thiscall*)(void*, int, int, int);
        reinterpret_cast<FnSetPersonData>(vt[0x2C / 4])(pPerson, 0x1D, locked ? 2 : 0, 1);
    });
}

static bool g_aspirationLocked = false;

// Native lockAspiration on/off handler (0xA0418600: PersonData 0xC1 = 1 or 0)
static size_t SetNativeAspirationLocked(bool locked) {
    g_aspirationLocked = locked;
    return ForEachLotPerson([locked](void* pPerson) {
        void** vt = *reinterpret_cast<void***>(pPerson);
        if (!vt) return;
        using FnSetPersonData = void (__thiscall*)(void*, int, int, int);
        reinterpret_cast<FnSetPersonData>(vt[0x2C / 4])(pPerson, 0xC1, locked ? 1 : 0, 1);
    });
}

// Helper to get currently selected cTSPerson* on the lot (0xA048C4D3: GetObjectFolder()->vtable[39] +0x9C)
static void* GetSelectedLotPerson() {
    uintptr_t pITSGlobals = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pITSGlobals));
    if (!pITSGlobals) {
        using FnGetGlobals = uintptr_t (__cdecl*)();
        pITSGlobals = reinterpret_cast<FnGetGlobals>(addr::Rebase(addr::kFunc_TSGetGlobals))();
    }
    if (!pITSGlobals) return nullptr;

    void** globalsVt = *reinterpret_cast<void***>(pITSGlobals);
    if (!globalsVt) return nullptr;
    using FnGetObjFolder = void* (__thiscall*)(void*);
    void* pObjFolder = reinterpret_cast<FnGetObjFolder>(globalsVt[0x68 / 4])(reinterpret_cast<void*>(pITSGlobals));
    if (!pObjFolder) return nullptr;

    void** folderVt = *reinterpret_cast<void***>(pObjFolder);
    if (!folderVt) return nullptr;
    using FnGetSelectedPerson = void* (__thiscall*)(void*);
    return reinterpret_cast<FnGetSelectedPerson>(folderVt[0x9C / 4])(pObjFolder);
}

} // namespace

// ============================================================================
// Public TS2SE API Implementation
// ============================================================================

uintptr_t GetSelectedSimPtr() {
    return reinterpret_cast<uintptr_t>(GetSelectedLotPerson());
}

bool CycleSelectedLotSim() {
    uintptr_t pITSGlobals = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pITSGlobals));
    if (!pITSGlobals) {
        using FnGetGlobals = uintptr_t (__cdecl*)();
        pITSGlobals = reinterpret_cast<FnGetGlobals>(addr::Rebase(addr::kFunc_TSGetGlobals))();
    }
    if (!pITSGlobals) return false;

    void** globalsVt = *reinterpret_cast<void***>(pITSGlobals);
    if (!globalsVt) return false;
    using FnGetObjFolder = void* (__thiscall*)(void*);
    void* pObjFolder = reinterpret_cast<FnGetObjFolder>(globalsVt[0x68 / 4])(reinterpret_cast<void*>(pITSGlobals));
    if (!pObjFolder) return false;

    void** folderVt = *reinterpret_cast<void***>(pObjFolder);
    if (!folderVt) return false;
    uintptr_t mod = reinterpret_cast<uintptr_t>(pObjFolder);
    if (*reinterpret_cast<const uintptr_t*>(mod) != addr::Rebase(addr::kVTable_cEdithObjectModule)) return false;

    uintptr_t pb = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_PersonBegin);
    uintptr_t pe = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_PersonEnd);
    if (pb < 0x10000u || pe <= pb) return false;
    uint32_t np = static_cast<uint32_t>((pe - pb) >> 2);
    if (np == 0 || np > 256) return false;

    void* curSel = GetSelectedLotPerson();
    uint32_t startIdx = 0;
    for (uint32_t i = 0; i < np; ++i) {
        if (reinterpret_cast<void*>(*reinterpret_cast<const uintptr_t*>(pb + i * 4u)) == curSel) {
            startIdx = i + 1;
            break;
        }
    }

    using FnBool = uint8_t (__thiscall*)(void*);
    using FnSelectPerson = void (__thiscall*)(void*, void*);
    for (uint32_t step = 0; step < np; ++step) {
        uint32_t idx = (startIdx + step) % np;
        void* cand = reinterpret_cast<void*>(*reinterpret_cast<const uintptr_t*>(pb + idx * 4u));
        if (!cand || cand == curSel) continue;
        void** pvt = *reinterpret_cast<void***>(cand);
        if (!pvt) continue;
        // Verify candidate is selectable (cITSPerson::vt[31] +0x7C == 1 && vt[32] +0x80 == 0)
        if (!reinterpret_cast<FnBool>(pvt[0x7C / 4])(cand)) continue;
        if (reinterpret_cast<FnBool>(pvt[0x80 / 4])(cand)) continue;
        reinterpret_cast<FnSelectPerson>(folderVt[addr::kObjTable_SelectPersonSlot])(pObjFolder, cand);
        if (GetSelectedLotPerson() == cand) {
            PushConsoleLog("[Sim] Controle alternado para o Sim #%u (0x%08X)!", idx, reinterpret_cast<unsigned>(cand));
            return true;
        }
    }
    return false;
}

void PushConsoleLog(const char* fmt, ...) {
    char* dst = g_logLines[g_logWriteIdx % kMaxLogLines];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf(dst, 142, fmt, args);
    va_end(args);
    if (n < 0) n = 142;
    dst[n] = '\0';
    ++g_logWriteIdx;
    if (g_logTotal < kMaxLogLines) ++g_logTotal;
    ts2::hook::Log("%s", dst);
}

size_t GetConsoleLogCount() {
    return g_logTotal;
}

const char* GetConsoleLogLine(size_t index) {
    if (index >= g_logTotal) return "";
    size_t start = (g_logWriteIdx > kMaxLogLines) ? (g_logWriteIdx - kMaxLogLines) : 0;
    return g_logLines[(start + index) % kMaxLogLines];
}

bool RegisterPrimitive(uint16_t opcode, const char* name, const char* summary, CustomPrimitiveFn fn) {
    if (opcode < 0x007F || opcode > 0x00FF || !fn) return false;
    if (CustomPrimitiveInfo* existing = FindCustomPrimitive(opcode)) {
        existing->name    = name;
        existing->summary = summary;
        existing->handler = fn;
        return true;
    }
    if (g_customPrimCount >= 128) return false;
    g_customPrimitives[g_customPrimCount++] = {opcode, name, summary, fn, 0};
    return true;
}

const CustomPrimitiveInfo* GetRegisteredPrimitives(size_t* outCount) {
    if (outCount) *outCount = g_customPrimCount;
    return g_customPrimitives;
}

int16_t GetSharedRegister(uint8_t regIndex) {
    return g_sharedRegs[regIndex];
}

void SetSharedRegister(uint8_t regIndex, int16_t value, bool broadcastToCoop) {
    g_sharedRegs[regIndex] = value;
    if (broadcastToCoop) {
        ts2::coop::SendBhavEvent(0x0081, regIndex, value, 0);
    }
}

bool Initialize() {
    // Register the 8 built-in Custom SimAntics Primitives (0x0080 .. 0x0087)
    RegisterPrimitive(0x0080, "TS2SE_LogStackState",     "Dump StackElem/Locals/Args no console",   &Prim_0080_LogStackState);
    RegisterPrimitive(0x0081, "TS2SE_GetSetGlobalReg",   "Le/Escreve registrador global 16-bit",    &Prim_0081_GetSetGlobalReg);
    RegisterPrimitive(0x0082, "TS2SE_CoopSendEvent",     "Envia evento BHAV para o peer Co-op",     &Prim_0082_CoopSendEvent);
    RegisterPrimitive(0x0083, "TS2SE_CoopIsHost",        "Retorna True no Host/Solo, False Client", &Prim_0083_CoopIsHost);
    RegisterPrimitive(0x0084, "TS2SE_MathExt",           "Sqrt/Sin/Cos/Min/Max em Locals BHAV",     &Prim_0084_MathExt);
    RegisterPrimitive(0x0085, "TS2SE_GetSetMotiveDirect","Le/Altera cTSMotives (+0x48) via BHAV",   &Prim_0085_GetSetMotiveDirect);
    RegisterPrimitive(0x0086, "TS2SE_ExecCommand",       "Executa cheat/script TS2SE via BHAV",     &Prim_0086_ExecCommand);
    RegisterPrimitive(0x0087, "TS2SE_DeterministicRand", "PRNG deterministico (+0xA4) via BHAV",    &Prim_0087_DeterministicRand);

    // 1. Hook cTSObject::ExecuteObjectPrimitive @ 0xA0534880 (5-byte prologue: 55 8B EC 6A FF)
    bool okPrim = ts2::hook::InstallDetour(
        g_detourExecutePrimitive,
        addr::Rebase(addr::kFunc_cTSObject_ExecuteObjectPrimitive),
        reinterpret_cast<void*>(&Hooked_cTSObject_ExecuteObjectPrimitive),
        5
    );

    // 2. Hook cTSTreeSim::ExecuteStep @ 0xA0576500 (9-byte prologue: 55 8B EC 64 A1 00 00 00 00)
    bool okStep = ts2::hook::InstallDetour(
        g_detourExecuteStep,
        addr::Rebase(addr::kFunc_cTSTreeSim_ExecuteStep),
        reinterpret_cast<void*>(&Hooked_cTSTreeSim_ExecuteStep),
        9
    );

    // 3. Hook cTSMotives::GetMotive at VTable[10] (0xA0F73B3C + 0x28 = 0xA0436B70)
    bool okMot = ts2::hook::HookPointerSlot(
        addr::Rebase(addr::kVTable_cTSMotives) + 10 * sizeof(void*),
        reinterpret_cast<void*>(&Hooked_cTSMotives_GetMotive),
        reinterpret_cast<void**>(&g_origGetMotive)
    );

    PushConsoleLog("[TS2SE] Inicializado: ExecutePrimitive=%d ExecuteStep=%d Motives=%d (8 opcodes 0x0080..0x0087)",
                   okPrim ? 1 : 0, okStep ? 1 : 0, okMot ? 1 : 0);
    return okPrim && okStep;
}

void Shutdown() {
    ts2::hook::RemoveDetour(g_detourExecutePrimitive);
    ts2::hook::RemoveDetour(g_detourExecuteStep);
}

bool ExecuteCommand(const char* cmdLine, char* outResponse, size_t maxResponseLen) {
    if (!cmdLine || !*cmdLine) return false;

    if (strncmp(cmdLine, "fx.", 3) == 0) {
        const bool handled = ts2::fx::ExecuteCommand(cmdLine, outResponse, maxResponseLen);
        if (handled) PushConsoleLog("[Console] %s -> %s", cmdLine, outResponse);
        return handled;
    }

    if (_stricmp(cmdLine, "ts2se.help") == 0) {
        _snprintf(outResponse, maxResponseLen,
                  "Comandos: motherlode | kaching | maxMotives | motiveDecay on/off | moveObjects on/off | boolProp ...");
        PushConsoleLog("[Console] %s -> %s", cmdLine, outResponse);
        return true;
    }

    if (_stricmp(cmdLine, "ts2se.status") == 0) {
        _snprintf(outResponse, maxResponseLen,
                  "GZCOM=0x%08X | TSGlobals=0x%08X | Sim=0x%08X | Tick=%u | Seed=0x%08X | BHAVSteps=%u",
                  reinterpret_cast<unsigned>(reinterpret_cast<void*(*)()>(addr::Rebase(addr::kFunc_GZFramework))()),
                  reinterpret_cast<unsigned>(*reinterpret_cast<void**>(addr::Rebase(addr::kGlobal_pITSGlobals))),
                  static_cast<unsigned>(GetSimulatorPtr()),
                  GetSimulatorTickCount(),
                  GetSimulatorRandomSeed(),
                  g_totalBhavSteps);
        PushConsoleLog("[Console] %s", outResponse);
        return true;
    }

    if (_stricmp(cmdLine, "ts2se.prim.test") == 0) {
        uint8_t ops[16] = {2, 0, 0x39, 0x05}; // Set Reg[0] = 1337 (0x0539)
        if (CustomPrimitiveInfo* p80 = FindCustomPrimitive(0x0080)) {
            ++p80->callCount;
            p80->handler(nullptr, nullptr, 0x0080, ops);
        }
        if (CustomPrimitiveInfo* p81 = FindCustomPrimitive(0x0081)) {
            ++p81->callCount;
            p81->handler(nullptr, nullptr, 0x0081, ops);
        }
        _snprintf(outResponse, maxResponseLen,
                  "Primitivas 0x0080 e 0x0081 executadas com sucesso! SharedReg[0] = %d",
                  GetSharedRegister(0));
        return true;
    }

    if (_stricmp(cmdLine, "ts2se.coop.host") == 0) {
        ts2::coop::StartHost(27025);
        _snprintf(outResponse, maxResponseLen, "Host Co-op escutando na porta TCP 27025.");
        return true;
    }

    if (_stricmp(cmdLine, "ts2se.coop.join") == 0) {
        ts2::coop::JoinHost("127.0.0.1", 27025);
        _snprintf(outResponse, maxResponseLen, "Convidado Co-op conectando a 127.0.0.1:27025 (TCP).");
        return true;
    }

    // Native money cheats: motherlode (+50,000) and kaching (+1,000) (0xA041847E)
    if (_stricmp(cmdLine, "motherlode") == 0) {
        int32_t newFunds = 0;
        if (AddFamilyFunds(50000, &newFunds)) {
            _snprintf(outResponse, maxResponseLen, "motherlode aplicado! +$50.000 (Fundos atuais: $%d)", newFunds);
            PushConsoleLog("[Cheat] %s", outResponse);
            return true;
        }
    }
    if (_stricmp(cmdLine, "kaching") == 0) {
        int32_t newFunds = 0;
        if (AddFamilyFunds(1000, &newFunds)) {
            _snprintf(outResponse, maxResponseLen, "kaching aplicado! +$1.000 (Fundos atuais: $%d)", newFunds);
            PushConsoleLog("[Cheat] %s", outResponse);
            return true;
        }
    }

    // Native maxMotives (0xA0418700)
    if (_stricmp(cmdLine, "maxMotives") == 0) {
        FillAllActiveSimMotives();
        _snprintf(outResponse, maxResponseLen, "maxMotives aplicado em todos os Sims do lote!");
        return true;
    }

    // Native motiveDecay on/off (0xA0418B5E)
    if (_stricmp(cmdLine, "motiveDecay off") == 0 || _stricmp(cmdLine, "motivedecay off") == 0) {
        size_t n = SetNativeMotiveDecayLocked(true);
        _snprintf(outResponse, maxResponseLen, "motiveDecay OFF (Necessidades congeladas em %u Sims).", static_cast<unsigned>(n));
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }
    if (_stricmp(cmdLine, "motiveDecay on") == 0 || _stricmp(cmdLine, "motivedecay on") == 0) {
        size_t n = SetNativeMotiveDecayLocked(false);
        _snprintf(outResponse, maxResponseLen, "motiveDecay ON (Decaimento normal em %u Sims).", static_cast<unsigned>(n));
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }

    // Native moveObjects on/off (0xA07746AE)
    if (_stricmp(cmdLine, "moveObjects on") == 0 || _stricmp(cmdLine, "moveobjects on") == 0) {
        SetNativeMoveObjects(true);
        ExecuteNativeCommandParser(cmdLine);
        _snprintf(outResponse, maxResponseLen, "moveObjects ON ativado no SimSystem e ManipulatorSystem!");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }
    if (_stricmp(cmdLine, "moveObjects off") == 0 || _stricmp(cmdLine, "moveobjects off") == 0) {
        SetNativeMoveObjects(false);
        ExecuteNativeCommandParser(cmdLine);
        _snprintf(outResponse, maxResponseLen, "moveObjects OFF aplicado.");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }

    // Native boolProp testingCheatsEnabled & allow45DegreeAngleOfRotation & quarterTilePlacementEnabled
    if (_stricmp(cmdLine, "boolProp testingCheatsEnabled true") == 0) {
        using FnSetTesting = void (__stdcall*)(uint8_t);
        reinterpret_cast<FnSetTesting>(addr::Rebase(addr::kFunc_SetTestingCheats))(1);
        SetNativeBoolProp("testingCheatsEnabled", true);
        ExecuteNativeCommandParser(cmdLine);
        _snprintf(outResponse, maxResponseLen, "testingCheatsEnabled = TRUE aplicado nativamente!");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }
    if (_stricmp(cmdLine, "boolProp testingCheatsEnabled false") == 0) {
        using FnSetTesting = void (__stdcall*)(uint8_t);
        reinterpret_cast<FnSetTesting>(addr::Rebase(addr::kFunc_SetTestingCheats))(0);
        SetNativeBoolProp("testingCheatsEnabled", false);
        ExecuteNativeCommandParser(cmdLine);
        _snprintf(outResponse, maxResponseLen, "testingCheatsEnabled = FALSE aplicado nativamente!");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }
    if (_stricmp(cmdLine, "boolProp allow45DegreeAngleOfRotation true") == 0) {
        SetNativeBoolProp("allow45DegreeAngleOfRotation", true);
        ExecuteNativeCommandParser(cmdLine);
        _snprintf(outResponse, maxResponseLen, "allow45DegreeAngleOfRotation = TRUE aplicado!");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }
    if (_stricmp(cmdLine, "boolProp allow45DegreeAngleOfRotation false") == 0) {
        SetNativeBoolProp("allow45DegreeAngleOfRotation", false);
        ExecuteNativeCommandParser(cmdLine);
        _snprintf(outResponse, maxResponseLen, "allow45DegreeAngleOfRotation = FALSE aplicado!");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }
    if (_stricmp(cmdLine, "setQuarterTilePlacement on") == 0 ||
        _stricmp(cmdLine, "boolProp quarterTilePlacementEnabled true") == 0) {
        SetNativeBoolProp("quarterTilePlacementEnabled", true);
        ExecuteNativeCommandParser("setQuarterTilePlacement on");
        _snprintf(outResponse, maxResponseLen, "quarterTilePlacementEnabled = TRUE aplicado!");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }
    if (_stricmp(cmdLine, "setQuarterTilePlacement off") == 0 ||
        _stricmp(cmdLine, "boolProp quarterTilePlacementEnabled false") == 0) {
        SetNativeBoolProp("quarterTilePlacementEnabled", false);
        ExecuteNativeCommandParser("setQuarterTilePlacement off");
        _snprintf(outResponse, maxResponseLen, "quarterTilePlacementEnabled = FALSE aplicado!");
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }

    // Forward any other command to native cIGZCommandParser (0xA0E44C51)
    if (ExecuteNativeCommandParser(cmdLine)) {
        _snprintf(outResponse, maxResponseLen, "Comando '%s' executado pelo cIGZCommandParser nativo!", cmdLine);
        PushConsoleLog("[Cheat] %s", outResponse);
        return true;
    }

    _snprintf(outResponse, maxResponseLen, "Comando '%s' enviado (cIGZCommandParser retornou false).", cmdLine);
    PushConsoleLog("[Console] %s", outResponse);
    return false;
}

const StackFrameSnapshot* GetInspectedFrames(size_t* outCount) {
    if (outCount) *outCount = g_frameCount;
    return g_frames;
}

uint32_t GetTotalBhavStepsExecuted() {
    return g_totalBhavSteps;
}

uintptr_t GetActiveSimMotivesPtr() {
    if (void* pSel = GetSelectedLotPerson()) {
        uintptr_t mot = *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<uintptr_t>(pSel) + addr::person_off::kMotivesPtr);
        if (mot > 0x10000u) {
            g_lastMotivesPtr = mot;
            return mot;
        }
    }
    return g_lastMotivesPtr;
}

float GetSimMotive(int motiveIndex) {
    uintptr_t mot = GetActiveSimMotivesPtr();
    if (motiveIndex < 0 || motiveIndex >= 16 || !mot) return 0.0f;
    return *reinterpret_cast<const float*>(mot + addr::kOffset_cTSMotives_Array + motiveIndex * 4);
}

void SetSimMotive(int motiveIndex, float value) {
    if (motiveIndex < 0 || motiveIndex >= 16) return;
    if (value < -100.0f) value = -100.0f;
    if (value > 100.0f)  value = 100.0f;

    if (uintptr_t mot = GetActiveSimMotivesPtr()) {
        *reinterpret_cast<float*>(mot + addr::kOffset_cTSMotives_Array + motiveIndex * 4) = value;
    }
    if (void* pSel = GetSelectedLotPerson()) {
        void** vt = *reinterpret_cast<void***>(pSel);
        if (vt) {
            using FnSetPersonMotive = void (__thiscall*)(void*, int, float);
            reinterpret_cast<FnSetPersonMotive>(vt[0x38 / 4])(pSel, motiveIndex, value);
            return;
        }
    }
    // Fallback: update via cITSPerson::SetMotiveValue (vtable[14] +0x38) on lot Sims
    ForEachLotPerson([motiveIndex, value](void* pPerson) {
        void** vt = *reinterpret_cast<void***>(pPerson);
        if (!vt) return;
        using FnSetPersonMotive = void (__thiscall*)(void*, int, float);
        reinterpret_cast<FnSetPersonMotive>(vt[0x38 / 4])(pPerson, motiveIndex, value);
    });
}

void FillAllActiveSimMotives() {
    // 1. Native maxMotives implementation (0xA0418700): iterate all Sims on lot and call vtable[14] (+0x38)
    size_t simCount = ForEachLotPerson([](void* pPerson) {
        void** vt = *reinterpret_cast<void***>(pPerson);
        if (!vt) return;
        using FnSetPersonMotive = void (__thiscall*)(void*, int, float);
        auto fn = reinterpret_cast<FnSetPersonMotive>(vt[0x38 / 4]);
        for (int m = 0; m < 16; ++m) {
            if (m == 2 || (m >= 10 && m <= 13)) continue; // Match native 0xA0418700 filter
            fn(pPerson, m, 100.0f);
        }
    });

    // 2. Also update cached g_lastMotivesPtr array directly if available
    if (g_lastMotivesPtr) {
        for (int i = 0; i < 16; ++i) {
            *reinterpret_cast<float*>(g_lastMotivesPtr + addr::kOffset_cTSMotives_Array + i * 4) = 100.0f;
        }
    }

    PushConsoleLog("[Motives] Motivos maximizados (+100.0) em %u Sims do lote (MotivesPtr=0x%08X)!",
                   static_cast<unsigned>(simCount), static_cast<unsigned>(g_lastMotivesPtr));
}

const char* GetMotiveName(int motiveIndex) {
    if (motiveIndex < 0 || motiveIndex >= 16) return "Unknown";
    return kMotiveNames[motiveIndex];
}

// ============================================================================
// Safe cITSSimulator* Accessors (via cached g_pTSSimulator @ 0xA12E5BC4 / 0xA020E26A)
// ============================================================================
uintptr_t GetSimulatorPtr() {
    uintptr_t sim = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pTSSimulator));
    if (sim > 0x10000u) return sim;

    uintptr_t pITSGlobals = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pITSGlobals));
    if (pITSGlobals > 0x10000u) {
        using FnGetSim = uintptr_t (__cdecl*)();
        sim = reinterpret_cast<FnGetSim>(addr::Rebase(addr::kFunc_TSGetSimulator))();
        if (sim > 0x10000u) return sim;
    }
    return 0;
}

uint32_t GetSimulatorTickCount() {
    uintptr_t sim = GetSimulatorPtr();
    return sim ? *reinterpret_cast<const uint32_t*>(sim + addr::sim_off::kLotTickCount) : 0;
}

uint32_t GetSimulatorRandomSeed() {
    uintptr_t sim = GetSimulatorPtr();
    return sim ? *reinterpret_cast<const uint32_t*>(sim + addr::sim_off::kRandomSeed) : 0;
}

void SetSimulatorRandomSeed(uint32_t seed) {
    uintptr_t sim = GetSimulatorPtr();
    if (sim) {
        *reinterpret_cast<uint32_t*>(sim + addr::sim_off::kRandomSeed) = seed;
    }
}

int16_t GetSimulatorState() {
    uintptr_t sim = GetSimulatorPtr();
    return sim ? *reinterpret_cast<const int16_t*>(sim + addr::sim_off::kSimulatorState) : 0;
}

int16_t GetSimulatorPauseCount() {
    uintptr_t sim = GetSimulatorPtr();
    return sim ? *reinterpret_cast<const int16_t*>(sim + addr::sim_off::kPauseCount) : 0;
}

int16_t GetSimulatorSpeed() {
    uintptr_t sim = GetSimulatorPtr();
    return sim ? *reinterpret_cast<const int16_t*>(sim + addr::sim_off::kSpeedMultiplier) : 1;
}

void SetSimulatorSpeed(int16_t speed) {
    uintptr_t sim = GetSimulatorPtr();
    if (sim) {
        if (speed < 1) speed = 1;
        if (speed > 3) speed = 3; // the game only defines speeds 1..3
        // cTSSimulator::SetSpeed @ 0xA04826C0 is `ret 8`: TWO stack args (speed, speed again) which it
        // stores in +0x36 and +0x7E. Passing a single argument unbalanced the stack and crashed the HUD.
        using FnSetSpeed = void (__thiscall*)(void*, int, int);
        reinterpret_cast<FnSetSpeed>(addr::Rebase(addr::kFunc_cTSSimulator_SetSpeed))(
            reinterpret_cast<void*>(sim), static_cast<int>(speed), static_cast<int>(speed));
    }
}

void SetSimulatorPaused(bool paused) {
    uintptr_t sim = GetSimulatorPtr();
    if (sim) {
        using FnPause = void (__thiscall*)(void*, int);
        if (paused) {
            reinterpret_cast<FnPause>(addr::Rebase(addr::kFunc_cTSSimulator_Pause))(
                reinterpret_cast<void*>(sim), 1);
            *reinterpret_cast<int16_t*>(sim + addr::sim_off::kPauseCount) = 1;
        } else {
            reinterpret_cast<FnPause>(addr::Rebase(addr::kFunc_cTSSimulator_Unpause))(
                reinterpret_cast<void*>(sim), 1);
            *reinterpret_cast<int16_t*>(sim + addr::sim_off::kPauseCount) = 0;
        }
    }
}

void GetSimulatorClock(uint16_t& outHour, uint16_t& outMin, uint16_t& outSubTick, uint16_t& outDay) {
    uintptr_t sim = GetSimulatorPtr();
    if (!sim) {
        outHour = outMin = outSubTick = outDay = 0;
        return;
    }
    outHour    = *reinterpret_cast<const uint16_t*>(sim + addr::sim_off::kHour);
    outDay     = *reinterpret_cast<const uint16_t*>(sim + addr::sim_off::kDay);
    outMin     = *reinterpret_cast<const uint16_t*>(sim + addr::sim_off::kMinute);
    outSubTick = *reinterpret_cast<const uint16_t*>(sim + addr::sim_off::kSubTick);
}

// Native setHour (0xA0418C10): calls cITSSimulator::vtable[41] (+0xA4)(hour, 0, -1, -1, -1, -1)
bool SetSimulatorHour(uint32_t hour24) {
    if (hour24 > 23) hour24 %= 24;
    uintptr_t sim = GetSimulatorPtr();
    if (!sim) return false;
    void** simVt = *reinterpret_cast<void***>(sim);
    if (!simVt) return false;

    using FnSetSimTime = void (__thiscall*)(void*, uint32_t, int, int, int, int, int);
    reinterpret_cast<FnSetSimTime>(simVt[0xA4 / 4])(
        reinterpret_cast<void*>(sim), hour24, 0, -1, -1, -1, -1);
    PushConsoleLog("[Time] Relogio do lote ajustado para %02u:00!", hour24);
    return true;
}

int32_t GetFamilyFunds() {
    uintptr_t sim = GetSimulatorPtr();
    if (!sim) return 0;
    void** simVt = *reinterpret_cast<void***>(sim);
    if (!simVt) return 0;
    using FnGetFunds = int32_t (__thiscall*)(void*);
    return reinterpret_cast<FnGetFunds>(simVt[0x74 / 4])(reinterpret_cast<void*>(sim));
}

bool ModifyFamilyFunds(int32_t delta, int32_t* outNewFunds) {
    int32_t next = 0;
    bool ok = AddFamilyFunds(delta, &next);
    if (ok) {
        PushConsoleLog("[Funds] Fundos da familia ajustados em %+d -> $%d", delta, next);
    }
    if (outNewFunds) *outNewFunds = next;
    return ok;
}

size_t GetLotSimCount() {
    return ForEachLotPerson([](void*) {});
}

bool IsMotiveDecayLocked() {
    return g_motiveDecayLocked;
}

size_t SetMotiveDecayLocked(bool locked) {
    size_t n = SetNativeMotiveDecayLocked(locked);
    PushConsoleLog("[Motives] Decaimento de motivos %s em %u Sims.", locked ? "CONGELADO" : "ATIVADO", static_cast<unsigned>(n));
    return n;
}

bool IsAspirationLocked() {
    return g_aspirationLocked;
}

size_t SetAspirationLocked(bool locked) {
    size_t n = SetNativeAspirationLocked(locked);
    PushConsoleLog("[Aspiration] Decaimento de aspiracao %s em %u Sims.", locked ? "CONGELADO" : "ATIVADO", static_cast<unsigned>(n));
    return n;
}

// Native aspirationLevel (0xA048C4A0): TSGetGlobals()->vtable[4] (+0x10)->vtable[41] (+0xA4)(pPerson, level)
bool SetSelectedSimAspirationLevel(int level0to5) {
    if (level0to5 < 0) level0to5 = 0;
    if (level0to5 > 5) level0to5 = 5;
    void* pPerson = GetSelectedLotPerson();
    if (!pPerson) return false;

    uintptr_t pITSGlobals = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pITSGlobals));
    if (!pITSGlobals) return false;
    void** globalsVt = *reinterpret_cast<void***>(pITSGlobals);
    if (!globalsVt) return false;

    using FnGetAspMgr = void* (__thiscall*)(void*);
    void* pAspMgr = reinterpret_cast<FnGetAspMgr>(globalsVt[0x10 / 4])(reinterpret_cast<void*>(pITSGlobals));
    if (!pAspMgr) return false;

    void** aspVt = *reinterpret_cast<void***>(pAspMgr);
    if (!aspVt) return false;
    using FnSetAspLevel = void (__thiscall*)(void*, void*, int);
    reinterpret_cast<FnSetAspLevel>(aspVt[0xA4 / 4])(pAspMgr, pPerson, level0to5);

    static const char* kNames[] = {"Failure", "Bad", "Neutral", "Green", "Gold", "Platinum"};
    PushConsoleLog("[Aspiration] Nivel de aspiracao do Sim ativo definido para %s (%d)!", kNames[level0to5], level0to5);
    return true;
}

// Native aspirationPoints (0xA048C640): PersonData 0xA1 stores (points / 10)
bool AddSelectedSimAspirationPoints(int32_t pointsDelta) {
    void* pPerson = GetSelectedLotPerson();
    if (!pPerson) return false;
    void** vt = *reinterpret_cast<void***>(pPerson);
    if (!vt) return false;

    using FnGetPersonData = int16_t (__thiscall*)(void*, int, int);
    using FnSetPersonData = void    (__thiscall*)(void*, int, int, int);
    int16_t curUnits = reinterpret_cast<FnGetPersonData>(vt[0x28 / 4])(pPerson, 0xA1, 1);
    int32_t newPts   = static_cast<int32_t>(curUnits) * 10 + pointsDelta;
    if (newPts < 0) newPts = 0;
    reinterpret_cast<FnSetPersonData>(vt[0x2C / 4])(pPerson, 0xA1, newPts / 10, 1);

    // Refresh UI notification via native command parser as well
    char cmd[64];
    _snprintf(cmd, sizeof(cmd), "aspirationPoints %d", pointsDelta);
    ExecuteNativeCommandParser(cmd);
    PushConsoleLog("[Aspiration] +%d Pontos de Aspiracao adicionados (Total: %d pts)!", pointsDelta, newPts);
    return true;
}

// Native unlockCareerRewards (0xA0771D7B)
size_t UnlockAllCareerRewardsOnLot() {
    ExecuteNativeCommandParser("unlockCareerRewards");
    size_t count = ForEachLotPerson([](void*) {});
    PushConsoleLog("[Rewards] Todas as Recompensas de Carreira desbloqueadas no lote (%u Sims)!", static_cast<unsigned>(count));
    return count;
}

// Native forcetwins (0xA0418590)
bool TriggerForceTwinsOnSelectedSim() {
    bool ok = ExecuteNativeCommandParser("forcetwins");
    PushConsoleLog("[Sim] Cheat nativo 'forcetwins' executado no Sim selecionado!");
    return ok;
}

// Native PlumbBobToggle (0xA0418BD0)
bool TogglePlumbBobVisibility() {
    bool ok = ExecuteNativeCommandParser("PlumbBobToggle");
    PushConsoleLog("[Cinema] PlumbBob (diamante verde) alternado no lote!");
    return ok;
}

// Native showHeadlines (0xA077151C)
bool SetShowHeadlinesEnabled(bool enabled) {
    SetNativeBoolProp("ShowHeadlines", enabled);
    ExecuteNativeCommandParser(enabled ? "showHeadlines on" : "showHeadlines off");
    PushConsoleLog("[Cinema] Baloes de fala/pensamento (showHeadlines): %s", enabled ? "ON" : "OFF");
    return true;
}

bool SetNativeBoolProperty(const char* propName, bool enabled) {
    bool ok = SetNativeBoolProp(propName, enabled);
    PushConsoleLog("[BoolProp] %s = %s", propName, enabled ? "true" : "false");
    return ok;
}

} // namespace ts2::se
