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
int16_t   GetSimulatorState() { return 2; }   // pretend we are in Buy mode so purchases/sales replicate
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

namespace ts2::qol {
void RunPendingSave() {}
}

namespace ts2::hook {
void Trace(const char*, ...) {}
bool InstallDetour(Detour&, uintptr_t, void*, size_t) { return true; }
bool RemoveDetour(Detour&) { return true; }
bool HookPointerSlot(uintptr_t, void*, void**) { return true; }
void Log(const char*, ...) {}
}

// ---- fake object world: the guest's lot deliberately differs from the host's ----
static constexpr int kN = 16;
static constexpr int32_t T = 65536; // one tile
static ts2::coop::ObjPose g_objs[kN];
static bool    g_objPresent[kN];
static int16_t g_simNid[2] = {101, 202};
static int8_t  g_simMotives[2][16] = {};
static int32_t g_lastAppliedInterIdx = -1;
static uint16_t g_lastAppliedInterTarget = 0;

static uint32_t FCount() { return kN; }
static bool FRead(uint32_t id, ts2::coop::ObjPose* o) {
    if (id >= kN || !g_objPresent[id]) return false;
    *o = g_objs[id];
    return true;
}
static bool FIsSim(uint32_t id) { return id <= 1; } // objects 0 and 1 are Sims
static int32_t g_mySim = 0;
static int32_t FSelected() { return g_mySim; }
static bool FApply(uint32_t id, const ts2::coop::ObjPose& p) {
    if (id >= kN || !g_objPresent[id]) return false;
    g_objs[id] = p;
    return true;
}
static bool FCreate(uint32_t* pid, const ts2::coop::ObjPose& p) { // like the game: the engine picks the slot
    for (uint32_t i = 2; i < kN; ++i) {
        if (g_objPresent[i]) continue;
        g_objPresent[i] = true;
        g_objs[i] = p;
        *pid = i;
        return true;
    }
    return false;
}
static bool FDestroy(uint32_t id, const ts2::coop::ObjPose& old) {
    if (id >= kN || !g_objPresent[id]) return false;
    if (old.guid != 0 && g_objs[id].guid != old.guid) return false;
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
    for (int i = 0; i < 2; ++i) if (g_simNid[i] == nid) { memcpy(g_simMotives[i], mot, 16); return true; }
    (void)id;
    return false;
}
static bool FApplyInter(const ts2::coop::SimInteractionEvent& ev) {
    g_lastAppliedInterIdx    = ev.interactionIndex;
    g_lastAppliedInterTarget = ev.targetSlot;
    return true;
}
static int32_t FResolveSim(uint32_t, int16_t nid) {
    for (int i = 0; i < 2; ++i) if (g_simNid[i] == nid) return i;
    return -1;
}
static int16_t FSimNid(uint32_t id) { return id <= 1 ? g_simNid[id] : 0; }
static uint32_t FToken() { return 77; }
static const ts2::coop::ObjectBackend kFake = {
    &FCount, &FRead, &FApply, &FToken, &FIsSim, &FSelected,
    &FCreate, &FDestroy, &FReadSimState, &FApplySimState, &FApplyInter, &FResolveSim, &FSimNid
};

static int FindGuid(uint32_t guid) {
    for (int i = 2; i < kN; ++i) if (g_objPresent[i] && g_objs[i].guid == guid) return i;
    return -1;
}

static void Run(bool host, const char* profile) {
    SetEnvironmentVariableA("USERPROFILE", profile);
    ts2::coop::Initialize();
    using namespace ts2::coop;

    // Host lot: Sims in slots 0/1, furniture 2..9 (guid 0x1000+slot)
    for (int i = 0; i < 10; ++i) {
        g_objPresent[i] = true;
        g_objs[i] = ObjPose{i * 2 * T, i * 3 * T, 1, static_cast<uint16_t>(i & 7), static_cast<uint32_t>(0x1000 + i)};
    }
    for (int m = 0; m < 16; ++m) {
        g_simMotives[0][m] = host ? 85 : 10;   // slot 0 = NeighborID 101 on the host
        g_simMotives[1][m] = host ? 20 : 92;
    }
    if (!host) {
        // Guest save differs: Sims in swapped slots, furniture 6/7 swapped, #3 elsewhere, #8 missing, an extra object #12
        g_simNid[0] = 202; g_simNid[1] = 101;
        g_objs[0] = ObjPose{33 * T, 33 * T, 1, 0, 0x1001}; // Sim 202 (starts far away)
        g_objs[1] = ObjPose{44 * T, 44 * T, 1, 0, 0x1000}; // Sim 101
        g_objs[3] = ObjPose{99 * T, 99 * T, 1, 0, 0x1003};
        ObjPose a = g_objs[6], b = g_objs[7];
        g_objs[6] = b; g_objs[7] = a;
        g_objPresent[8] = false;
        g_objPresent[12] = true;
        g_objs[12] = ObjPose{5 * T, 5 * T, 1, 0, 0xEEEE0001u};
    }
    SetObjectBackendForTest(&kFake);
    g_mySim = host ? 0 : 0; // host controls NeighborID 101 (slot 0), guest controls NeighborID 202 (slot 0 on its side)
    SetSimSyncEnabled(true);
    bool movedA = false, movedB = false, bought = false, sold = false, gBought = false, gMoved = false, sentInter = false, sentChat = false;

    uint32_t t0 = GetTickCount();
    if (host) {
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

    int guestBoughtSlot = -1;
    for (;;) {
        PollNetwork();
        ObjectSyncTick();
        const CoopStats& st = GetStats();
        uint32_t el = GetTickCount() - t0;
        // host edits: moves 0x1005, buys a new object, sells 0x1004
        if (host && !movedA && el > 6000) { movedA = true; g_objs[5] = ObjPose{50 * T, 51 * T, 1, 3, 0x1005}; }
        if (host && !bought && el > 6500) {
            bought = true;
            g_objPresent[10] = true;
            g_objs[10] = ObjPose{110 * T, 120 * T, 1, 2, 0xABCD1234u};
        }
        if (host && !sold && el > 7000) { sold = true; g_objPresent[4] = false; }
        // guest edits: moves 0x1007 (slot 6 on the guest!), buys 0xBEEF0001 and later moves it
        if (!host && !movedB && el > 7000) {
            movedB = true;
            int k = FindGuid(0x1007);
            if (k >= 0) g_objs[k] = ObjPose{70 * T, 71 * T, 2, 5, 0x1007};
        }
        if (!host && !gBought && el > 8000) {
            gBought = true;
            for (int i = 2; i < kN; ++i) if (!g_objPresent[i]) { guestBoughtSlot = i; break; }
            if (guestBoughtSlot >= 0) { g_objPresent[guestBoughtSlot] = true; g_objs[guestBoughtSlot] = ObjPose{12 * T, 13 * T, 1, 1, 0xBEEF0001u}; }
        }
        if (!host && !gMoved && el > 10000 && guestBoughtSlot >= 0) { gMoved = true; g_objs[guestBoughtSlot].tx = 14 * T; g_objs[guestBoughtSlot].ty = 15 * T; }
        if (host && !sentChat && el > 6000) { sentChat = true; SendChat("ol\xC3\xA1 do host, tudo bem?"); }
        if (!host && !sentChat && el > 6500) { sentChat = true; SendChat("pong do convidado"); SendChat("/nick Zeca"); SendChat("com apelido"); }
        if (!host && !sentInter && el > 7500) {
            sentInter = true;
            SimInteractionEvent ev = {};
            ev.actorSlot = 0;
            ev.actorNeighborId = 202;
            int k = FindGuid(0x1005);
            ev.targetSlot = static_cast<uint16_t>(k < 0 ? 0 : k);
            ev.targetGuid = 0x1005;
            ev.interactionIndex = 7;
            ev.priority = 50;
            ev.actionTreeId = 4099;
            SendSimInteraction(ev);
        }
        // Sims walk far (the live updates only fire beyond the drift threshold)
        if (host && el > 8000 && el < 8100) g_objs[0] = ObjPose{20 * T, 21 * T, 1, 2, 0x1000};
        if (host && el > 9000 && el < 9100) g_objs[0] = ObjPose{22 * T, 23 * T, 1, 6, 0x1000};
        if (!host && el > 8500 && el < 8600) g_objs[0] = ObjPose{30 * T, 31 * T, 2, 4, 0x1001};
        if (el > 13000) break;
        Sleep(5);
    }
    const CoopStats& st = GetStats();
    uint16_t h = 0, m = 0, s = 0, d = 0;
    ts2::se::GetSimulatorClock(h, m, s, d);
    fprintf(g_log, "RESULT role=%s connected=%d tick=%u seed=%08X clock=%02u:%02u:%02u speed=%d funds=%d hostFunds=%d desync=%u\n",
            host ? "host" : "guest", st.peerConnected,
            ts2::se::GetSimulatorTickCount(), ts2::se::GetSimulatorRandomSeed(), h, m, s, ts2::se::GetSimulatorSpeed(), g_funds, st.hostFunds, st.desyncCount);
    fprintf(g_log, "OBJSTATS sent=%u applied=%u created=%u deleted=%u conflicts=%u tracked=%u simsSent=%u simsApplied=%u motives=%u inter=%u lastInter=%d:%u\n",
            st.objSent, st.objApplied, st.objCreated, st.objDeleted, st.objConflicts, st.objTracked,
            st.simsSent, st.simsApplied, st.motivesSynced, st.interactionsSynced, g_lastAppliedInterIdx, (unsigned)g_lastAppliedInterTarget);
    for (uint32_t i = 0; i < GetChatTotal(); ++i) {
        const ChatLine* c = GetChatLine(i);
        if (c) fprintf(g_log, "CHAT %d|%s|%s\n", (int)c->kind, c->name, c->text);
    }
    // slot independent dump: furniture by GUID, Sims by NeighborID
    for (int i = 0; i < 2; ++i)
        fprintf(g_log, "SIM %d: %d,%d,%d,%u motive=%d\n", (int)g_simNid[i], g_objs[i].tx / T, g_objs[i].ty / T, (int)g_objs[i].level, (unsigned)g_objs[i].rot, (int)g_simMotives[i][3]);
    for (int i = 2; i < kN; ++i)
        if (g_objPresent[i])
            fprintf(g_log, "OBJ %08X: %d,%d,%d,%u\n", g_objs[i].guid, g_objs[i].tx / T, g_objs[i].ty / T, (int)g_objs[i].level, (unsigned)g_objs[i].rot);
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
