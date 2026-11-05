// Test harness: runs the REAL src/coop_net.cpp (host/guest) outside the game under Wine.
// Usage: harness.exe host|guest <userprofile-dir>
#include "ts2/win32_abi.hpp"
#include "ts2/coop_net.hpp"
#include "ts2/ts2se_engine.hpp"
#include "ts2/hook_engine.hpp"

extern "C" {
__declspec(dllimport) BOOL WINAPI SetEnvironmentVariableA(LPCSTR, LPCSTR);
__declspec(dllimport) void WINAPI ExitProcess(UINT);
__declspec(dllimport) LPSTR WINAPI GetCommandLineA(void);
int _fltused = 0;
}

static FILE* g_log = nullptr;
static uint8_t g_fakeSim[0x400];
static int32_t g_funds = 1000;

namespace ts2::se {
void PushConsoleLog(const char* fmt, ...) {
    char buf[400];
    va_list ap; va_start(ap, fmt); _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    fprintf(g_log, "%s\n", buf); fflush(g_log);
}
uintptr_t GetSelectedSimPtr() { return 0; }
uintptr_t GetSimulatorPtr() { return reinterpret_cast<uintptr_t>(g_fakeSim); }
uint32_t  GetSimulatorTickCount() { return *reinterpret_cast<uint32_t*>(g_fakeSim + addr::sim_off::kLotTickCount); }
uint32_t  GetSimulatorRandomSeed() { return *reinterpret_cast<uint32_t*>(g_fakeSim + addr::sim_off::kRandomSeed); }
void      SetSimulatorRandomSeed(uint32_t s) { *reinterpret_cast<uint32_t*>(g_fakeSim + addr::sim_off::kRandomSeed) = s; }
int16_t   GetSimulatorState() { return 0; }
int16_t   GetSimulatorPauseCount() { return *reinterpret_cast<int16_t*>(g_fakeSim + addr::sim_off::kPauseCount); }
int16_t   GetSimulatorSpeed() { return *reinterpret_cast<int16_t*>(g_fakeSim + addr::sim_off::kSpeedMultiplier); }
void      SetSimulatorSpeed(int16_t v) { *reinterpret_cast<int16_t*>(g_fakeSim + addr::sim_off::kSpeedMultiplier) = v; }
void      SetSimulatorPaused(bool p) { *reinterpret_cast<int16_t*>(g_fakeSim + addr::sim_off::kPauseCount) = p ? 1 : 0; }
void GetSimulatorClock(uint16_t& h, uint16_t& m, uint16_t& s, uint16_t& d) {
    h = *reinterpret_cast<uint16_t*>(g_fakeSim + addr::sim_off::kHour);
    m = *reinterpret_cast<uint16_t*>(g_fakeSim + addr::sim_off::kMinute);
    s = *reinterpret_cast<uint16_t*>(g_fakeSim + addr::sim_off::kSubTick);
    d = 1;
}
int32_t GetFamilyFunds() { return g_funds; }
bool ModifyFamilyFunds(int32_t delta, int32_t* out) { g_funds += delta; if (out) *out = g_funds; return true; }
void SetSharedRegister(uint8_t, int16_t, bool) {}
} // namespace ts2::se

namespace ts2::hook {
bool InstallDetour(Detour&, uintptr_t, void*, size_t) { return true; }
bool RemoveDetour(Detour&) { return true; }
bool HookPointerSlot(uintptr_t, void*, void**) { return true; }
void Log(const char*, ...) {}
}

// ---- fake object world for the furniture-sync, Buy/Build create/destroy, Sim motives & interaction sync ----
static ts2::coop::ObjPose g_objs[12];
static bool    g_objPresent[12];
static int16_t g_simNid[2] = {101, 202};
static int8_t  g_simMotives[2][16] = {};
static int32_t g_lastAppliedInterIdx = -1;
static uint16_t g_lastAppliedInterTarget = 0;

static uint32_t FCount() { return 12; }
static bool FRead(uint32_t id, ts2::coop::ObjPose* o) {
    if (id >= 12 || !g_objPresent[id]) return false;
    *o = g_objs[id];
    return true;
}
static bool FIsSim(uint32_t id) { return id <= 1; } // objects 0 and 1 are Sims
static int32_t g_mySim = 0;
static int32_t FSelected() { return g_mySim; }
static bool FApply(uint32_t id, const ts2::coop::ObjPose& p) {
    if (id >= 12 || !g_objPresent[id]) return false;
    g_objs[id] = p;
    return true;
}
static bool FCreate(uint32_t id, const ts2::coop::ObjPose& p) {
    if (id >= 12) return false;
    g_objPresent[id] = true;
    g_objs[id] = p;
    return true;
}
static bool FDestroy(uint32_t id, const ts2::coop::ObjPose&) {
    if (id >= 12 || !g_objPresent[id]) return false;
    g_objPresent[id] = false;
    return true;
}
static bool FReadSimState(uint32_t id, int16_t* outNid, int8_t outMot[16]) {
    if (id > 1) return false;
    if (outNid) *outNid = g_simNid[id];
    if (outMot) memcpy(outMot, g_simMotives[id], 16);
    return true;
}
static bool FApplySimState(uint32_t id, int16_t nid, const int8_t mot[16]) {
    if (id > 1) return false;
    g_simNid[id] = nid;
    memcpy(g_simMotives[id], mot, 16);
    return true;
}
static bool FApplyInter(const ts2::coop::SimInteractionEvent& ev) {
    g_lastAppliedInterIdx    = ev.interactionIndex;
    g_lastAppliedInterTarget = ev.targetSlot;
    return true;
}
static uint32_t FToken() { return 77; }
static const ts2::coop::ObjectBackend kFake = {
    &FCount, &FRead, &FApply, &FToken, &FIsSim, &FSelected,
    &FCreate, &FDestroy, &FReadSimState, &FApplySimState, &FApplyInter
};

static void Run(bool host, const char* profile) {
    SetEnvironmentVariableA("USERPROFILE", profile);
    SetEnvironmentVariableA("TS2_COOP_TEST", "1");
    ts2::coop::Initialize();
    using namespace ts2::coop;

    for (int i = 0; i < 10; ++i) {
        g_objPresent[i] = true;
        g_objs[i] = ObjPose{i * 2, i * 3, 1, static_cast<uint16_t>(i & 7), static_cast<uint32_t>(0x1000 + i)};
    }
    g_objPresent[10] = false;
    g_objPresent[11] = false;
    for (int m = 0; m < 16; ++m) {
        g_simMotives[0][m] = host ? 85 : 10;
        g_simMotives[1][m] = host ? 20 : 92;
    }
    if (!host) g_objs[3] = ObjPose{99, 99, 1, 0, 0x1003}; // guest starts with a different save -> snapshot must fix it
    SetObjectBackendForTest(&kFake);
    g_mySim = host ? 0 : 1;
    SetSimSyncEnabled(true);
    bool movedA = false, movedB = false, boughtAndSold = false, sentInter = false;

    uint32_t t0 = GetTickCount();
    bool sentRequest = false;
    if (host) {
        // authoritative state on the host
        *reinterpret_cast<uint32_t*>(g_fakeSim + ts2::addr::sim_off::kLotTickCount) = 4200;
        *reinterpret_cast<uint32_t*>(g_fakeSim + ts2::addr::sim_off::kRandomSeed) = 0xDEADBEEF;
        *reinterpret_cast<uint16_t*>(g_fakeSim + ts2::addr::sim_off::kHour) = 17;
        *reinterpret_cast<uint16_t*>(g_fakeSim + ts2::addr::sim_off::kMinute) = 42;
        *reinterpret_cast<uint16_t*>(g_fakeSim + ts2::addr::sim_off::kSubTick) = 24;
        *reinterpret_cast<int16_t*>(g_fakeSim + ts2::addr::sim_off::kSpeedMultiplier) = 3;
        g_funds = 123456;
        StartHost(27931);
    } else {
        g_funds = 5;
        JoinHost("127.0.0.1", 27931);
    }

    for (;;) {
        PollNetwork();
        ObjectSyncTick();
        const CoopStats& st = GetStats();
        uint32_t el = GetTickCount() - t0;
        if (host && st.peerConnected && !sentRequest && el > 1500) {
            sentRequest = true;
            SendNeighborhoodToGuest(500);
        }
        // furniture edits during the session (host moves #5, guest moves #7)
        if (host && !movedA && el > 6000) { movedA = true; g_objs[5] = ObjPose{50, 51, 1, 3, 0x1005}; }
        if (host && !boughtAndSold && el > 6500) {
            boughtAndSold = true;
            // Host buys new object #10 and sells object #4
            g_objPresent[10] = true;
            g_objs[10] = ObjPose{110, 120, 1, 2, 0xABCD1234u};
            g_objPresent[4] = false;
        }
        if (!host && !sentInter && el > 7500) {
            sentInter = true;
            SimInteractionEvent ev = {};
            ev.actorSlot = 1;
            ev.actorNeighborId = 202;
            ev.targetSlot = 5;
            ev.targetGuid = 0x1005;
            ev.interactionIndex = 7;
            ev.priority = 50;
            ev.actionTreeId = 4099;
            SendSimInteraction(ev);
        }
        if (host && el > 8000 && el < 8100) g_objs[0] = ObjPose{3, 4, 1, 2, 0x1000};   // the Host's Sim walks
        if (host && el > 9000 && el < 9100) g_objs[0] = ObjPose{7, 8, 1, 6, 0x1000};
        if (!host && el > 8500 && el < 8600) g_objs[1] = ObjPose{20, 21, 2, 4, 0x1001}; // the Guest's Sim walks
        if (!host && !movedB && el > 7000) { movedB = true; g_objs[7] = ObjPose{70, 71, 2, 5, 0x1007}; }
        if (el > 12000) break;
        if (st.xferState == XferState::Error) { Sleep(500); PollNetwork(); break; }
        Sleep(5);
    }
    const CoopStats& st = GetStats();
    uint16_t h = 0, m = 0, s = 0, d = 0;
    ts2::se::GetSimulatorClock(h, m, s, d);
    fprintf(g_log, "RESULT role=%s connected=%d xfer=%d files=%u/%u bytes=%u/%u msg='%s' tick=%u seed=%08X clock=%02u:%02u:%02u speed=%d funds=%d hostFunds=%d desync=%u\n",
            host ? "host" : "guest", st.peerConnected, (int)st.xferState, st.xferFilesDone, st.xferFilesTotal,
            st.xferBytesDone, st.xferBytesTotal, st.xferMessage,
            ts2::se::GetSimulatorTickCount(), ts2::se::GetSimulatorRandomSeed(), h, m, s, ts2::se::GetSimulatorSpeed(), g_funds, st.hostFunds, st.desyncCount);
    fprintf(g_log, "OBJSTATS sent=%u applied=%u created=%u deleted=%u conflicts=%u tracked=%u simsSent=%u simsApplied=%u motives=%u inter=%u lastInter=%d:%u\n",
            st.objSent, st.objApplied, st.objCreated, st.objDeleted, st.objConflicts, st.objTracked,
            st.simsSent, st.simsApplied, st.motivesSynced, st.interactionsSynced, g_lastAppliedInterIdx, (unsigned)g_lastAppliedInterTarget);
    fprintf(g_log, "MOTIVES sim0=%d sim1=%d\n", (int)g_simMotives[0][3], (int)g_simMotives[1][3]);
    for (int i = 0; i < 12; ++i) {
        if (g_objPresent[i])
            fprintf(g_log, "OBJ %d: %d,%d,%d,%u,%08X\n", i, g_objs[i].tx, g_objs[i].ty, (int)g_objs[i].level, (unsigned)g_objs[i].rot, g_objs[i].guid);
        else
            fprintf(g_log, "OBJ %d: DELETED\n", i);
    }
    fflush(g_log);
}

extern "C" void HarnessMain() {
    const char* cmd = GetCommandLineA();
    bool host = strstr(cmd, " host") != nullptr;
    const char* p = nullptr;  // last "Z:" token = profile dir (first one is the exe path)
    for (const char* c = cmd; *c; ++c) if (c[0] == 'Z' && c[1] == ':') p = c;
    char profile[260] = {};
    if (p) {
        size_t n = 0;
        while (p[n] && p[n] != ' ' && p[n] != '"' && n < 255) { profile[n] = p[n]; ++n; }
    }
    char logPath[300];
    _snprintf(logPath, sizeof(logPath), "%s\\harness.log", profile);
    g_log = fopen(logPath, "w");
    if (!g_log) ExitProcess(2);
    Run(host, profile);
    fclose(g_log);
    ExitProcess(0);
}
