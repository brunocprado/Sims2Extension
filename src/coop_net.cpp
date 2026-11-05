#include "ts2/coop_net.hpp"
#include "ts2/addresses.hpp"
#include "ts2/hook_engine.hpp"
#include "ts2/engine_qol.hpp"
#include "ts2/ts2se_engine.hpp"

namespace ts2::coop {

namespace {

constexpr uint32_t kProtoVersion = 5;
constexpr uint32_t kMaxFrame     = 65536;
constexpr uint32_t kTxCap        = 1u << 20;
constexpr uint32_t kRxCap        = 1u << 18;
constexpr uint32_t kChunkSize    = 32768;
constexpr uint32_t kMaxFiles     = 16384;
constexpr uint32_t kStateEveryMs = 250;

enum class Msg : uint8_t {
    Hello      = 1,
    State      = 2,  // Host -> Guest authoritative state
    SpeedPause = 3,  // either direction
    Bhav       = 4,  // either direction
    ResyncReq  = 5,  // Guest -> Host
    XferBegin  = 10,
    XferFile   = 11,
    XferChunk  = 12,
    XferFileEnd= 13,
    XferEnd    = 14,
    XferAbort  = 15,
    XferAck    = 16,
    ObjUpdate      = 20, // either direction: batch of object pose changes (or a full snapshot)
    ObjSnapReq     = 21, // Guest -> Host: please send a full snapshot of object poses
    SimState       = 22, // either direction: Sim NeighborID + 16 motives sync
    SimInteraction = 23  // either direction: live SimAntics interaction queue replication
};

#pragma pack(push, 1)
struct FrameHdr {
    uint32_t len;   // payload bytes (excludes this header)
    uint8_t  type;
};
struct HelloPayload {
    uint32_t version;
    uint8_t  role;
};
struct StatePayload {
    uint32_t tick;
    uint32_t seed;
    int16_t  simState;
    int16_t  pause;
    int16_t  speed;
    uint16_t hour;
    uint16_t minute;
    uint16_t sub;
    int32_t  funds;
    uint32_t crc;
};
struct SpeedPausePayload {
    int16_t speed;
    int16_t pause;
};
struct BhavPayload {
    uint16_t eventId;
    uint16_t stackObjId;
    int16_t  arg0;
    int16_t  arg1;
};
struct XferBeginPayload {
    uint32_t fileCount;
    uint32_t totalBytes;
    char     hood[8];
};
struct XferFileHdr {
    uint32_t size;
    uint16_t pathLen;
    // followed by pathLen chars
};
struct XferAbortPayload {
    char msg[96];
};
struct ObjBatchHdr {
    uint16_t count;
    uint8_t  snapshot; // 0: move, 1: snapshot, 2: Sim pose, 3: Buy/Build create, 4: Buy/Build destroy
};
struct ObjRec {
    uint16_t id;       // index in the lot object folder
    ObjPose  oldPose;  // pose the sender last agreed on (compare-and-swap guard)
    ObjPose  newPose;
};
struct SimStatePayload {
    uint16_t slot;
    int16_t  neighborId;
    int8_t   motives[16];
    uint8_t  isControlled;
};
#pragma pack(pop)

struct XferEntry {
    char     rel[192];
    uint32_t size;
};

CoopStats g_stats = {};

SOCKET g_listen = INVALID_SOCKET;
SOCKET g_conn   = INVALID_SOCKET;
bool   g_wsaInitialized = false;
bool   g_inPoll         = false;

// Guest connect state machine
bool  g_connecting       = false;
DWORD g_connectStartTick = 0;
DWORD g_nextConnectTick  = 0;

DWORD g_lastStateTx = 0;

// TX ring buffer
uint8_t  g_tx[kTxCap];
uint32_t g_txHead = 0;
uint32_t g_txUsed = 0;

// RX linear buffer
uint8_t  g_rx[kRxCap];
uint32_t g_rxLen = 0;

// Host-side transfer
XferEntry* g_xferList      = nullptr;
uint32_t   g_xferCount     = 0;
uint32_t   g_xferIdx       = 0;
HANDLE     g_xferFile      = INVALID_HANDLE_VALUE;
uint32_t   g_xferRemain    = 0;
bool       g_xferSending   = false;
char       g_xferRoot[300] = {};
DWORD      g_sendAtTick    = 0;
bool       g_sendPending   = false;

// Guest-side transfer
bool     g_rxActive     = false;
HANDLE   g_rxFile       = INVALID_HANDLE_VALUE;
uint32_t g_rxRemain     = 0;
char     g_rxDestDir[300] = {};

ts2::hook::Detour g_detourSimulateTick;
ts2::hook::Detour g_detourOnIdle;
ts2::hook::Detour g_detourEnqueueInteraction;
using Fn_SimulateTick       = uint8_t (__thiscall*)(void* pSimulator);
using Fn_OnIdle             = void (__thiscall*)(void* pSimulator);
using Fn_EnqueueInteraction = uint8_t (__thiscall*)(void* pITSPerson, void* pITSInteraction);

bool g_applyingRemoteInteraction = false;

// ----------------------------------------------------------------------------
// Utilities
// ----------------------------------------------------------------------------
void SetXferMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_stats.xferMessage, sizeof(g_stats.xferMessage) - 1, fmt, ap);
    va_end(ap);
    g_stats.xferMessage[sizeof(g_stats.xferMessage) - 1] = 0;
}

void SetConnectMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_stats.connectMessage, sizeof(g_stats.connectMessage) - 1, fmt, ap);
    va_end(ap);
    g_stats.connectMessage[sizeof(g_stats.connectMessage) - 1] = 0;
}

bool IsDots(const char* n) {
    return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

bool IsHoodName(const char* n) {
    return n && n[0] == 'N' && n[1] >= '0' && n[1] <= '9' && n[2] >= '0' && n[2] <= '9' &&
           n[3] >= '0' && n[3] <= '9' && n[4] == 0;
}

bool IsSkippedDir(const char* n) {
    return _stricmp(n, "Thumbnails") == 0 || _stricmp(n, "Storytelling") == 0;
}

uint64_t FtToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool DirExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Relative path from the peer must stay inside the destination folder.
bool IsSafeRelPath(const char* p) {
    if (!p || !*p) return false;
    if (p[0] == '\\' || p[0] == '/') return false;
    for (const char* c = p; *c; ++c) {
        if (*c == ':' || static_cast<unsigned char>(*c) < 32) return false;
        if (c[0] == '.' && c[1] == '.') return false;
    }
    return true;
}

// Creates every directory component of `fullPath` that lies after `rootLen`.
void EnsureParentDirs(char* fullPath, size_t rootLen) {
    for (size_t i = rootLen; fullPath[i]; ++i) {
        if (fullPath[i] == '\\') {
            fullPath[i] = 0;
            CreateDirectoryA(fullPath, nullptr);
            fullPath[i] = '\\';
        }
    }
}

// <Documents>\EA Games\<The Sims 2 ...>  (the directory that contains "Neighborhoods")
bool FindUserDataRoot(char* out, size_t cap) {
    char prof[260] = {};
    if (!GetEnvironmentVariableA("USERPROFILE", prof, sizeof(prof)) || !prof[0]) return false;

    static const char* kDocs[] = {"Documents", "My Documents"};
    uint64_t bestTime = 0;
    bool found = false;

    for (const char* docs : kDocs) {
        char pattern[400];
        _snprintf(pattern, sizeof(pattern), "%s\\%s\\EA Games\\*", prof, docs);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (IsDots(fd.cFileName) || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!strstr(fd.cFileName, "Sims")) continue;
            char cand[400], probe[420];
            _snprintf(cand, sizeof(cand), "%s\\%s\\EA Games\\%s", prof, docs, fd.cFileName);
            _snprintf(probe, sizeof(probe), "%s\\Neighborhoods", cand);
            if (!DirExists(probe)) continue;
            uint64_t t = FtToU64(fd.ftLastWriteTime);
            if (!found || t > bestTime) {
                bestTime = t;
                found = true;
                strncpy(out, cand, cap - 1);
                out[cap - 1] = 0;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return found;
}

uint64_t NewestWriteTime(const char* dir, int depth) {
    uint64_t best = 0;
    char pattern[400];
    _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (IsDots(fd.cFileName)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !IsSkippedDir(fd.cFileName)) {
                char sub[400];
                _snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
                uint64_t t = NewestWriteTime(sub, depth - 1);
                if (t > best) best = t;
            }
        } else {
            uint64_t t = FtToU64(fd.ftLastWriteTime);
            if (t > best) best = t;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return best;
}

// Neighborhood folder (NXXX) with the most recently modified content.
bool FindActiveNeighborhood(const char* root, char* outHood) {
    char pattern[400];
    _snprintf(pattern, sizeof(pattern), "%s\\Neighborhoods\\N*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint64_t best = 0;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsHoodName(fd.cFileName)) continue;
        char path[400];
        _snprintf(path, sizeof(path), "%s\\Neighborhoods\\%s", root, fd.cFileName);
        uint64_t t = NewestWriteTime(path, 3);
        if (!found || t > best) {
            best = t;
            found = true;
            strncpy(outHood, fd.cFileName, 7);
            outHood[7] = 0;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

void CollectFiles(const char* absDir, const char* relDir, int depth, uint64_t& total) {
    if (depth > 8) return;
    char pattern[420];
    _snprintf(pattern, sizeof(pattern), "%s\\*", absDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (IsDots(fd.cFileName)) continue;
        char abs[420], rel[220];
        _snprintf(abs, sizeof(abs), "%s\\%s", absDir, fd.cFileName);
        if (relDir[0]) _snprintf(rel, sizeof(rel), "%s\\%s", relDir, fd.cFileName);
        else           _snprintf(rel, sizeof(rel), "%s", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!IsSkippedDir(fd.cFileName)) CollectFiles(abs, rel, depth + 1, total);
        } else if (g_xferCount < kMaxFiles && strlen(rel) < sizeof(g_xferList[0].rel) &&
                   fd.nFileSizeHigh == 0) {
            XferEntry& e = g_xferList[g_xferCount++];
            strncpy(e.rel, rel, sizeof(e.rel) - 1);
            e.rel[sizeof(e.rel) - 1] = 0;
            e.size = fd.nFileSizeLow;
            total += fd.nFileSizeLow;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ----------------------------------------------------------------------------
// Reliable framed transport
// ----------------------------------------------------------------------------
void CloseConnection(const char* reason);

bool QueueFrame(Msg type, const void* payload, uint32_t len) {
    if (g_conn == INVALID_SOCKET) return false;
    if (g_txUsed + sizeof(FrameHdr) + len > kTxCap) return false;
    FrameHdr hdr = {len, static_cast<uint8_t>(type)};
    auto push = [](const uint8_t* src, uint32_t n) {
        uint32_t tail = (g_txHead + g_txUsed) % kTxCap;
        uint32_t first = kTxCap - tail;
        if (first > n) first = n;
        memcpy(g_tx + tail, src, first);
        if (first < n) memcpy(g_tx, src + first, n - first);
        g_txUsed += n;
    };
    push(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
    if (len) push(static_cast<const uint8_t*>(payload), len);
    ++g_stats.packetsSent;
    return true;
}

void FlushTx() {
    while (g_conn != INVALID_SOCKET && g_txUsed > 0) {
        uint32_t contiguous = kTxCap - g_txHead;
        if (contiguous > g_txUsed) contiguous = g_txUsed;
        int n = send(g_conn, reinterpret_cast<const char*>(g_tx + g_txHead), static_cast<int>(contiguous), 0);
        if (n > 0) {
            g_txHead = (g_txHead + static_cast<uint32_t>(n)) % kTxCap;
            g_txUsed -= static_cast<uint32_t>(n);
        } else {
            if (WSAGetLastError() != WSAEWOULDBLOCK) CloseConnection("falha ao enviar");
            return;
        }
    }
}

void SendHello() {
    HelloPayload p = {kProtoVersion, static_cast<uint8_t>(g_stats.role)};
    QueueFrame(Msg::Hello, &p, sizeof(p));
}

uint32_t ComputeLotCrc32(uintptr_t simPtr) {
    if (!simPtr) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    auto mixWord = [&](uint32_t v) {
        crc ^= v;
        for (int b = 0; b < 32; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
    };
    mixWord(*reinterpret_cast<const uint32_t*>(simPtr + addr::sim_off::kLotTickCount));
    mixWord(*reinterpret_cast<const uint32_t*>(simPtr + addr::sim_off::kRandomSeed));
    mixWord(*reinterpret_cast<const uint16_t*>(simPtr + addr::sim_off::kHour));
    mixWord(*reinterpret_cast<const uint16_t*>(simPtr + addr::sim_off::kMinute));
    return ~crc;
}

bool LocalLotReady(); // a lot is loaded and has been running for a moment (defined with the object backend)

void SendStateNow() {
    uintptr_t sim = ts2::se::GetSimulatorPtr();
    StatePayload s = {};
    if (sim > 0x10000u && LocalLotReady()) {
        s.tick     = ts2::se::GetSimulatorTickCount();
        s.seed     = ts2::se::GetSimulatorRandomSeed();
        s.simState = ts2::se::GetSimulatorState();
        s.pause    = ts2::se::GetSimulatorPauseCount();
        s.speed    = ts2::se::GetSimulatorSpeed();
        uint16_t d = 0;
        ts2::se::GetSimulatorClock(s.hour, s.minute, s.sub, d);
        s.funds    = ts2::se::GetFamilyFunds();
        s.crc      = ComputeLotCrc32(sim);
        g_stats.lastLocalCrc = s.crc;
    } else {
        s.simState = -1; // not in a lot: Guest must ignore
    }
    if (QueueFrame(Msg::State, &s, sizeof(s))) ++g_stats.ticksSynced;
}

int MinutesOfDay(uint16_t h, uint16_t m) { return static_cast<int>(h) * 60 + static_cast<int>(m); }

void ApplyHostState(const StatePayload& s) {
    g_stats.lastPeerTick = s.tick;
    g_stats.lastPeerCrc  = s.crc;
    g_stats.hostFunds    = s.funds;
    ++g_stats.ticksSynced;
    if (s.simState < 1) return; // the Host is not inside a lot

    // The simulator object exists in the neighborhood view and while a lot is still loading, but poking its clock,
    // pause count, PRNG seed and funds then corrupts the load. Only touch it once our own lot is up and running.
    if (!LocalLotReady()) return;
    uintptr_t sim = ts2::se::GetSimulatorPtr();
    if (sim <= 0x10000u) return;

    *reinterpret_cast<uint32_t*>(sim + addr::sim_off::kLotTickCount)   = s.tick;
    *reinterpret_cast<uint32_t*>(sim + addr::sim_off::kRandomSeed)     = s.seed;
    *reinterpret_cast<int16_t*>(sim + addr::sim_off::kPauseCount)      = s.pause;
    if (s.speed >= 1 && s.speed <= 3 && ts2::se::GetSimulatorSpeed() != s.speed) {
        ts2::se::SetSimulatorSpeed(s.speed); // proper setter: also updates the HUD
    }

    // Align clock & subtick so both instances compute the exact same deterministic lot CRC
    uint16_t lh = 0, lm = 0, ls = 0, ld = 0;
    ts2::se::GetSimulatorClock(lh, lm, ls, ld);
    int subDiff = static_cast<int>(s.sub) - static_cast<int>(ls);
    if (subDiff < 0) subDiff = -subDiff;
    if (lh != s.hour || lm != s.minute || subDiff > 10) {
        *reinterpret_cast<uint16_t*>(sim + addr::sim_off::kHour)    = s.hour;
        *reinterpret_cast<uint16_t*>(sim + addr::sim_off::kMinute)  = s.minute;
        *reinterpret_cast<uint16_t*>(sim + addr::sim_off::kSubTick) = s.sub;
    }

    int32_t localFunds = ts2::se::GetFamilyFunds();
    if (localFunds != s.funds) ts2::se::ModifyFamilyFunds(s.funds - localFunds);

    g_stats.lastLocalCrc = ComputeLotCrc32(sim);
    if (g_stats.lastLocalCrc != s.crc) ++g_stats.desyncCount;
}

// ----------------------------------------------------------------------------
// Neighborhood transfer: Host (sender)
// ----------------------------------------------------------------------------
void AbortSend(const char* why) {
    if (g_xferFile != INVALID_HANDLE_VALUE) { CloseHandle(g_xferFile); g_xferFile = INVALID_HANDLE_VALUE; }
    g_xferSending = false;
    g_stats.xferState = XferState::Error;
    SetXferMessage("%s", why);
    ts2::se::PushConsoleLog("[Co-op] Transferencia abortada: %s", why);
}

void StartSendNow() {
    g_sendPending = false;
    if (g_stats.role != Role::Host || !g_stats.peerConnected) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Nenhum convidado conectado.");
        return;
    }
    char root[300] = {};
    if (!FindUserDataRoot(root, sizeof(root))) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Pasta 'EA Games\\The Sims 2' nao encontrada em Documentos.");
        return;
    }
    char hood[8] = {};
    if (!FindActiveNeighborhood(root, hood)) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Nenhuma vizinhanca NXXX encontrada em %s", root);
        return;
    }
    if (!g_xferList) {
        g_xferList = static_cast<XferEntry*>(malloc(sizeof(XferEntry) * kMaxFiles));
        if (!g_xferList) { g_stats.xferState = XferState::Error; SetXferMessage("Sem memoria."); return; }
    }
    _snprintf(g_xferRoot, sizeof(g_xferRoot), "%s\\Neighborhoods\\%s", root, hood);
    g_xferCount = 0;
    uint64_t total = 0;
    CollectFiles(g_xferRoot, "", 0, total);
    if (g_xferCount == 0 || total > 0xFFFFFFF0ull) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Vizinhanca %s vazia ou grande demais (%u arquivos).", hood, g_xferCount);
        return;
    }

    XferBeginPayload b = {};
    b.fileCount  = g_xferCount;
    b.totalBytes = static_cast<uint32_t>(total);
    strncpy(b.hood, hood, sizeof(b.hood) - 1);
    QueueFrame(Msg::XferBegin, &b, sizeof(b));

    g_xferIdx = 0;
    g_xferFile = INVALID_HANDLE_VALUE;
    g_xferRemain = 0;
    g_xferSending = true;
    g_stats.xferState      = XferState::Sending;
    g_stats.xferBytesDone  = 0;
    g_stats.xferBytesTotal = b.totalBytes;
    g_stats.xferFilesDone  = 0;
    g_stats.xferFilesTotal = g_xferCount;
    strncpy(g_stats.xferHood, hood, sizeof(g_stats.xferHood) - 1);
    SetXferMessage("Enviando %s: %u arquivos, %u KB", hood, g_xferCount, b.totalBytes / 1024);
    ts2::se::PushConsoleLog("[Co-op] Enviando vizinhanca %s (%u arquivos, %u KB)", hood, g_xferCount, b.totalBytes / 1024);
}

void PumpSend() {
    if (!g_xferSending) return;
    static uint8_t buf[kChunkSize + 512];
    for (int iter = 0; iter < 16; ++iter) {
        if (kTxCap - g_txUsed < kChunkSize + 1024) return; // wait for the socket to drain

        if (g_xferFile == INVALID_HANDLE_VALUE) {
            if (g_xferIdx >= g_xferCount) {
                QueueFrame(Msg::XferEnd, nullptr, 0);
                g_xferSending = false;
                g_stats.xferState = XferState::Done;
                SetXferMessage("Vizinhanca %s enviada! Aguardando o convidado carregar.", g_stats.xferHood);
                ts2::se::PushConsoleLog("[Co-op] Vizinhanca enviada.");
                return;
            }
            const XferEntry& e = g_xferList[g_xferIdx];
            char abs[520];
            _snprintf(abs, sizeof(abs), "%s\\%s", g_xferRoot, e.rel);
            HANDLE f = CreateFileA(abs, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                AbortSend("Falha ao abrir arquivo para envio (em uso?).");
                return;
            }
            uint16_t pathLen = static_cast<uint16_t>(strlen(e.rel));
            XferFileHdr fh = {e.size, pathLen};
            memcpy(buf, &fh, sizeof(fh));
            memcpy(buf + sizeof(fh), e.rel, pathLen);
            QueueFrame(Msg::XferFile, buf, sizeof(fh) + pathLen);
            g_xferFile = f;
            g_xferRemain = e.size;
        }

        uint32_t want = g_xferRemain < kChunkSize ? g_xferRemain : kChunkSize;
        if (want > 0) {
            DWORD got = 0;
            if (!ReadFile(g_xferFile, buf, want, &got, nullptr) || got == 0) {
                AbortSend("Falha ao ler arquivo durante o envio.");
                return;
            }
            QueueFrame(Msg::XferChunk, buf, got);
            g_xferRemain -= got;
            g_stats.xferBytesDone += got;
        }
        if (g_xferRemain == 0) {
            CloseHandle(g_xferFile);
            g_xferFile = INVALID_HANDLE_VALUE;
            QueueFrame(Msg::XferFileEnd, nullptr, 0);
            ++g_xferIdx;
            ++g_stats.xferFilesDone;
        }
    }
}

// ----------------------------------------------------------------------------
// Neighborhood transfer: Guest (receiver)
// ----------------------------------------------------------------------------
void GuestRxFail(const char* why) {
    if (g_rxFile != INVALID_HANDLE_VALUE) { CloseHandle(g_rxFile); g_rxFile = INVALID_HANDLE_VALUE; }
    g_rxActive = false;
    g_stats.xferState = XferState::Error;
    SetXferMessage("%s", why);
    ts2::se::PushConsoleLog("[Co-op] Recebimento falhou: %s", why);
    XferAbortPayload a = {};
    strncpy(a.msg, why, sizeof(a.msg) - 1);
    QueueFrame(Msg::XferAbort, &a, sizeof(a));
}

void GuestBegin(const XferBeginPayload& b) {
    if (g_rxFile != INVALID_HANDLE_VALUE) { CloseHandle(g_rxFile); g_rxFile = INVALID_HANDLE_VALUE; }
    g_rxActive = false;

    char hood[8] = {};
    memcpy(hood, b.hood, 7);
    if (!IsHoodName(hood)) { GuestRxFail("Nome de vizinhanca invalido recebido."); return; }

    if (g_stats.peerLoopback) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Host no mesmo PC: transferencia ignorada (as pastas sao as mesmas).");
        XferAbortPayload a = {};
        strncpy(a.msg, "mesmo PC", sizeof(a.msg) - 1);
        QueueFrame(Msg::XferAbort, &a, sizeof(a));
        return;
    }

    char root[300] = {};
    if (!FindUserDataRoot(root, sizeof(root))) { GuestRxFail("Pasta 'EA Games\\The Sims 2' nao encontrada no convidado."); return; }

    char dest[400];
    _snprintf(dest, sizeof(dest), "%s\\Neighborhoods\\%s", root, hood);
    if (DirExists(dest)) {
        char backupRoot[400], backup[460];
        _snprintf(backupRoot, sizeof(backupRoot), "%s\\CoopBackup", root);
        CreateDirectoryA(backupRoot, nullptr);
        _snprintf(backup, sizeof(backup), "%s\\%s_%u", backupRoot, hood, static_cast<unsigned>(GetTickCount()));
        if (!MoveFileA(dest, backup)) {
            GuestRxFail("Nao consegui mover sua vizinhanca atual para CoopBackup (feche o jogo no menu principal?).");
            return;
        }
        ts2::se::PushConsoleLog("[Co-op] Backup do seu %s -> %s", hood, backup);
    }
    CreateDirectoryA(dest, nullptr);
    if (!DirExists(dest)) { GuestRxFail("Nao consegui criar a pasta da vizinhanca."); return; }

    strncpy(g_rxDestDir, dest, sizeof(g_rxDestDir) - 1);
    g_rxActive = true;
    g_stats.xferState      = XferState::Receiving;
    g_stats.xferBytesDone  = 0;
    g_stats.xferBytesTotal = b.totalBytes;
    g_stats.xferFilesDone  = 0;
    g_stats.xferFilesTotal = b.fileCount;
    strncpy(g_stats.xferHood, hood, sizeof(g_stats.xferHood) - 1);
    SetXferMessage("Recebendo %s: %u arquivos, %u KB", hood, b.fileCount, b.totalBytes / 1024);
    ts2::se::PushConsoleLog("[Co-op] Recebendo vizinhanca %s do Host...", hood);
}

void GuestFile(const uint8_t* p, uint32_t len) {
    if (!g_rxActive) return;
    if (g_rxFile != INVALID_HANDLE_VALUE) { CloseHandle(g_rxFile); g_rxFile = INVALID_HANDLE_VALUE; }
    if (len < sizeof(XferFileHdr)) { GuestRxFail("Quadro de arquivo invalido."); return; }
    XferFileHdr fh;
    memcpy(&fh, p, sizeof(fh));
    if (fh.pathLen == 0 || fh.pathLen > 190 || sizeof(fh) + fh.pathLen > len) { GuestRxFail("Caminho de arquivo invalido."); return; }

    char rel[200] = {};
    memcpy(rel, p + sizeof(fh), fh.pathLen);
    for (char* c = rel; *c; ++c) if (*c == '/') *c = '\\';
    if (!IsSafeRelPath(rel)) { GuestRxFail("Caminho inseguro recusado."); return; }

    char abs[520];
    _snprintf(abs, sizeof(abs), "%s\\%s", g_rxDestDir, rel);
    EnsureParentDirs(abs, strlen(g_rxDestDir) + 1);
    HANDLE f = CreateFileA(abs, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { GuestRxFail("Falha ao criar arquivo no convidado."); return; }
    g_rxFile = f;
    g_rxRemain = fh.size;
}

void GuestChunk(const uint8_t* p, uint32_t len) {
    if (!g_rxActive || g_rxFile == INVALID_HANDLE_VALUE) return;
    if (len > g_rxRemain) { GuestRxFail("Dados alem do tamanho do arquivo."); return; }
    DWORD written = 0;
    if (!WriteFile(g_rxFile, p, len, &written, nullptr) || written != len) {
        GuestRxFail("Falha ao gravar arquivo (disco cheio?).");
        return;
    }
    g_rxRemain -= len;
    g_stats.xferBytesDone += len;
}

void GuestFileEnd() {
    if (g_rxFile != INVALID_HANDLE_VALUE) { CloseHandle(g_rxFile); g_rxFile = INVALID_HANDLE_VALUE; }
    if (g_rxActive) ++g_stats.xferFilesDone;
}

void GuestEnd() {
    if (!g_rxActive) return;
    g_rxActive = false;
    g_stats.xferState = XferState::Done;
    SetXferMessage("Vizinhanca %s instalada! Volte ao menu principal e carregue-a.", g_stats.xferHood);
    ts2::se::PushConsoleLog("[Co-op] Vizinhanca %s instalada. Carregue-a pelo menu principal.", g_stats.xferHood);
    QueueFrame(Msg::XferAck, nullptr, 0);
}

// ----------------------------------------------------------------------------
// Object (furniture) placement sync
//
// Both machines watch every object of the lot (poll every 250 ms, game thread). A pose that changed and
// stayed stable for two polls is sent as {id, oldPose, newPose}. The receiver applies it only if its own
// object is still at oldPose (or already at newPose) - this keeps us safe if object ids ever diverge
// between the two simulations. The Host answers a snapshot request with the pose of every object, which
// is applied unconditionally (the Host is authoritative).
// Not synchronised: creating or deleting objects (only moving/rotating existing ones).
// ----------------------------------------------------------------------------
constexpr uint32_t kMaxObj          = 12000;
constexpr uint32_t kObjScanEveryMs  = 250;
constexpr uint32_t kObjApplyPerTick = 200;
constexpr uint32_t kObjBatch        = 1000;

bool PoseEq(const ObjPose& a, const ObjPose& b) {
    return a.tx == b.tx && a.ty == b.ty && a.level == b.level && a.rot == b.rot;
}

// ---- Real backend (talks to Sims2EP9.exe) ----
// Slot index = (SimAntics object id - 1). The table is the cEdithObjectModule (see addresses.hpp).
uintptr_t ObjTablePtr() {
    uintptr_t g = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pITSGlobals));
    if (!g) {
        using FnGetGlobals = uintptr_t (__cdecl*)();
        g = reinterpret_cast<FnGetGlobals>(addr::Rebase(addr::kFunc_TSGetGlobals))();
    }
    if (!g) return 0;
    void** vt = *reinterpret_cast<void***>(g);
    if (!vt) return 0;
    using FnGetModule = uintptr_t (__thiscall*)(void*);
    uintptr_t mod = reinterpret_cast<FnGetModule>(vt[0x68 / 4])(reinterpret_cast<void*>(g));
    if (mod < 0x10000u) return 0;
    // Validate: must really be the object module, otherwise our layout assumptions do not hold.
    if (*reinterpret_cast<const uintptr_t*>(mod) != addr::Rebase(addr::kVTable_cEdithObjectModule)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ts2::se::PushConsoleLog("[Co-op] AVISO: tabela de objetos inesperada (vtable=%08X); sincronia de moveis desativada.",
                                    static_cast<unsigned>(*reinterpret_cast<const uintptr_t*>(mod)));
        }
        return 0;
    }
    return mod;
}

uintptr_t ObjFolderPtr() {
    uintptr_t g = *reinterpret_cast<const uintptr_t*>(addr::Rebase(addr::kGlobal_pITSGlobals));
    if (!g) {
        using FnGetGlobals = uintptr_t (__cdecl*)();
        g = reinterpret_cast<FnGetGlobals>(addr::Rebase(addr::kFunc_TSGetGlobals))();
    }
    if (!g) return 0;
    void** vt = *reinterpret_cast<void***>(g);
    if (!vt) return 0;
    using FnGetFolder = uintptr_t (__thiscall*)(void*);
    uintptr_t folder = reinterpret_cast<FnGetFolder>(vt[0x60 / 4])(reinterpret_cast<void*>(g));
    return folder < 0x10000u ? 0 : folder;
}

uint32_t ReadObjGuid(uintptr_t obj) {
    if (!obj) return 0;
    uintptr_t sel = *reinterpret_cast<const uintptr_t*>(obj + addr::obj_off::kSelectorPtr);
    if (sel < 0x10000u) return 0;
    uintptr_t objDef = *reinterpret_cast<const uintptr_t*>(sel + 0x60);
    if (objDef < 0x10000u) return 0;
    void** defVt = *reinterpret_cast<void***>(objDef);
    if (!defVt) return 0;
    using FnGetDefProp = uint32_t (__thiscall*)(void*, int);
    return reinterpret_cast<FnGetDefProp>(defVt[0x0C / 4])(reinterpret_cast<void*>(objDef), 0xCA);
}

uintptr_t ObjAt(uint32_t index) {
    uintptr_t mod = ObjTablePtr();
    if (!mod) return 0;
    uintptr_t b = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecBegin);
    uintptr_t e = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecEnd);
    if (b < 0x10000u || e < b || ((e - b) >> 2) <= index) return 0;
    void** vt = *reinterpret_cast<void***>(mod);
    using FnGetObject = uintptr_t (__thiscall*)(void*, int);
    uintptr_t obj = reinterpret_cast<FnGetObject>(vt[addr::kObjTable_GetObjectSlot])(
        reinterpret_cast<void*>(mod), static_cast<int>(index) + 1);
    return obj < 0x10000u ? 0 : obj;
}

uint32_t RealCount() {
    uintptr_t mod = ObjTablePtr();
    if (!mod) return 0;
    uintptr_t b = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecBegin);
    uintptr_t e = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecEnd);
    if (b < 0x10000u || e < b) return 0;
    return static_cast<uint32_t>((e - b) >> 2);
}

bool RealRead(uint32_t id, ObjPose* out) {
    uintptr_t obj = ObjAt(id);
    if (!obj) return false;
    uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
    if (vt < addr::Rebase(0xA0F00000u) || vt >= addr::Rebase(0xA1200000u)) return false; // not a game vtable
    bool isPerson = (vt == addr::Rebase(addr::kVTable_cTSPerson));
    if (isPerson && !g_stats.simSyncEnabled) return false;

    void** vtbl = *reinterpret_cast<void***>(obj);
    if (!isPerson) {
        // Filter out non-furniture (e.g. jigs, routing emitters, controllers, effects)
        using FnGetType = int (__thiscall*)(void*);
        int objType = reinterpret_cast<FnGetType>(vtbl[addr::obj_vt::kGetObjectType / 4])(reinterpret_cast<void*>(obj));
        if (objType != 4 && objType != 8 && objType != 9) return false;
        // Filter out objects held in a Sim's hands or sitting in a container slot
        if (*reinterpret_cast<const int16_t*>(obj + addr::obj_off::kContainerID) != 0) return false;
        if (*reinterpret_cast<const uintptr_t*>(obj + addr::obj_off::kContainerPtr) != 0) return false;
        if (*reinterpret_cast<const uint8_t*>(obj + addr::obj_off::kInWorld) == 0) return false;
        // Multi-tile objects: only track the lead tile object so sub-tiles never fight
        using FnGetLead = uintptr_t (__thiscall*)(void*);
        uintptr_t lead = reinterpret_cast<FnGetLead>(vtbl[addr::obj_vt::kGetLeadObject / 4])(reinterpret_cast<void*>(obj));
        if (lead && lead != obj) return false;
    }

    out->tx    = *reinterpret_cast<const int32_t*>(obj + addr::obj_off::kTileX);
    out->ty    = *reinterpret_cast<const int32_t*>(obj + addr::obj_off::kTileY);
    out->level = *reinterpret_cast<const int16_t*>(obj + addr::obj_off::kLevel);
    out->rot   = *reinterpret_cast<const uint16_t*>(obj + addr::obj_off::kRotation) & 7u;
    out->guid  = ReadObjGuid(obj);
    // The temporary "Walk Here" target (and anything without a catalog GUID) is created and destroyed by the game's own
    // interactions; mirroring it creates duplicates the engine does not expect (crashed the peer).
    if (!isPerson && (out->guid == addr::kGUID_GoHereObject || out->guid == 0)) return false;
    if (out->tx < 0 || out->ty < 0) return false; // -65536 (0xFFFF0000) = out of world
    if (out->level < 1 || out->level > 16) return false;
    return true;
}

bool RealIsSim(uint32_t id) {
    uintptr_t obj = ObjAt(id);
    return obj && *reinterpret_cast<const uintptr_t*>(obj) == addr::Rebase(addr::kVTable_cTSPerson);
}

bool RealApply(uint32_t id, const ObjPose& want) {
    uintptr_t obj = ObjAt(id);
    if (!obj) return false;
    bool isPerson = (*reinterpret_cast<const uintptr_t*>(obj) == addr::Rebase(addr::kVTable_cTSPerson));
    // Exact slot only: the sync layer maps peer slots to local ones, so a different catalog object here means the
    // tables diverged and touching it would move the wrong thing.
    if (!isPerson && want.guid != 0) {
        uint32_t localGuid = ReadObjGuid(obj);
        if (localGuid != 0 && localGuid != want.guid) return false;
    }
    ts2::hook::Trace("apply slot=%u %s guid=%08X -> (%d,%d,L%d,r%u)", id, isPerson ? "SIM" : "obj", want.guid,
                     want.tx, want.ty, (int)want.level, (unsigned)want.rot);
    void** vt = *reinterpret_cast<void***>(obj);

    using FnVoid     = void (__thiscall*)(void*);
    using FnSetLoc   = void (__thiscall*)(void*, float*, float, int);
    using FnSetOrient= void (__thiscall*)(void*, int32_t*, int);
    using FnTileToPos= void (__cdecl*)(float*, const int32_t*, int);

    int32_t tile[2] = {want.tx, want.ty};
    float pos[3] = {};
    reinterpret_cast<FnTileToPos>(addr::Rebase(addr::kFunc_TilePosToWorld))(pos, tile, want.level);
    int32_t B = (10 - static_cast<int32_t>(want.rot)) & 7;

    void* self = reinterpret_cast<void*>(obj);
    reinterpret_cast<FnVoid>(vt[addr::obj_vt::kBeginEdit / 4])(self);
    reinterpret_cast<FnSetLoc>(vt[addr::obj_vt::kSetLocation / 4])(self, pos, 0.0f, 1);
    reinterpret_cast<FnSetOrient>(vt[addr::obj_vt::kSetOrientation / 4])(self, &B, 1);
    reinterpret_cast<FnVoid>(vt[addr::obj_vt::kEndEdit / 4])(self);
    return true;
}

bool RealCreate(uint32_t* ioSlot, const ObjPose& want) {
    if (want.guid == 0 || want.tx < 0 || want.ty < 0) return false;
    uintptr_t mod = ObjTablePtr();
    uintptr_t folder = ObjFolderPtr();
    if (!mod || !folder) return false;

    void** folderVt = *reinterpret_cast<void***>(folder);
    if (!folderVt) return false;
    using FnGetSelector = void* (__thiscall*)(void*, uint32_t);
    void* sel = reinterpret_cast<FnGetSelector>(folderVt[0x38 / 4])(reinterpret_cast<void*>(folder), want.guid);
    if (!sel) return false;

    void** modVt = *reinterpret_cast<void***>(mod);
    using FnCreateObj = int16_t (__thiscall*)(void*, void*, int, int, int);
    ts2::hook::Trace("create guid=%08X at (%d,%d)", want.guid, want.tx, want.ty);
    int16_t newId = reinterpret_cast<FnCreateObj>(modVt[addr::kObjTable_CreateObjectSlot])(
        reinterpret_cast<void*>(mod), sel, 0, 0, 0);
    ts2::hook::Trace("create -> id %d", (int)newId);
    if (newId <= 0) return false;
    if (ioSlot) *ioSlot = static_cast<uint32_t>(newId - 1);   // the game picks the slot, which may differ from the peer's
    return RealApply(static_cast<uint32_t>(newId - 1), want);
}

bool RealDestroy(uint32_t id, const ObjPose& oldPose) {
    uintptr_t mod = ObjTablePtr();
    if (!mod) return false;
    uintptr_t obj = ObjAt(id);
    if (!obj) return false;
    if (oldPose.guid != 0 && ReadObjGuid(obj) != oldPose.guid) return false; // tables diverged: never delete a different object
    ts2::hook::Trace("destroy slot=%u guid=%08X", id, oldPose.guid);
    void** modVt = *reinterpret_cast<void***>(mod);
    using FnDeleteObj = void (__thiscall*)(void*, int);
    reinterpret_cast<FnDeleteObj>(modVt[addr::kObjTable_DeleteObjectSlot])(
        reinterpret_cast<void*>(mod), static_cast<int>(id) + 1);
    return true;
}

uint32_t RealLotToken() {
    static uint32_t lastTick = 0, epoch = 1;
    uintptr_t mod = ObjTablePtr();
    if (!mod) return 0;
    uint32_t tick = ts2::se::GetSimulatorTickCount();
    if (tick < lastTick) ++epoch;      // a new lot restarted the simulator clock
    lastTick = tick;
    uintptr_t b = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecBegin);
    return epoch * 2654435761u ^ static_cast<uint32_t>(b);
}

constexpr uintptr_t kPersonSubobjectOffset = addr::person_off::kSubobjectOffset;

int32_t RealSelectedSim() {
    uintptr_t sel = ts2::se::GetSelectedSimPtr();
    if (sel < 0x10000u) return -1;
    // The lot's person list stores the cITSPerson sub-object of cTSPerson (vtable offset 0x2F0);
    // the object table stores the primary cTSObject pointer. Verified in-game: table ptr = list ptr - 0x2F0.
    sel -= kPersonSubobjectOffset;
    uintptr_t mod = ObjTablePtr();
    if (!mod) return -1;
    uintptr_t b = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecBegin);
    uintptr_t e = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecEnd);
    if (b < 0x10000u || e < b) return -1;
    uint32_t n = static_cast<uint32_t>((e - b) >> 2);
    for (uint32_t i = 0; i < n; ++i)
        if (*reinterpret_cast<const uintptr_t*>(b + i * 4u) == sel) return static_cast<int32_t>(i);
    return -1;
}

uintptr_t FindSimBySlotOrNeighborId(uint32_t slot, int16_t neighborId, uint32_t* outResolvedSlot = nullptr) {
    if (RealIsSim(slot)) {
        uintptr_t obj = ObjAt(slot);
        uintptr_t itsp = obj + kPersonSubobjectOffset;
        int16_t nid = *reinterpret_cast<const int16_t*>(itsp + addr::person_off::kNeighborID);
        if (neighborId <= 0 || nid == neighborId) {
            if (outResolvedSlot) *outResolvedSlot = slot;
            return obj;
        }
    }
    if (neighborId > 0) {
        uint32_t n = RealCount();
        for (uint32_t i = 0; i < n; ++i) {
            if (!RealIsSim(i)) continue;
            uintptr_t obj = ObjAt(i);
            int16_t nid = *reinterpret_cast<const int16_t*>(obj + kPersonSubobjectOffset + addr::person_off::kNeighborID);
            if (nid == neighborId) {
                if (outResolvedSlot) *outResolvedSlot = i;
                return obj;
            }
        }
    }
    if (RealIsSim(slot)) {
        if (outResolvedSlot) *outResolvedSlot = slot;
        return ObjAt(slot);
    }
    return 0;
}

bool RealReadSimState(uint32_t id, int16_t* outNeighborId, int8_t outMotives[16]) {
    if (!RealIsSim(id)) return false;
    uintptr_t obj = ObjAt(id);
    if (!obj) return false;
    uintptr_t itsp = obj + kPersonSubobjectOffset;
    if (outNeighborId) {
        *outNeighborId = *reinterpret_cast<const int16_t*>(itsp + addr::person_off::kNeighborID);
    }
    if (outMotives) {
        uintptr_t mot = *reinterpret_cast<const uintptr_t*>(itsp + addr::person_off::kMotivesPtr);
        for (int m = 0; m < 16; ++m) {
            float val = (mot > 0x10000u)
                ? *reinterpret_cast<const float*>(mot + addr::kOffset_cTSMotives_Array + m * 4)
                : 0.0f;
            if (val < -100.0f) val = -100.0f;
            if (val > 100.0f)  val = 100.0f;
            outMotives[m] = static_cast<int8_t>(val);
        }
    }
    return true;
}

bool RealApplySimState(uint32_t id, int16_t neighborId, const int8_t motives[16]) {
    uintptr_t obj = FindSimBySlotOrNeighborId(id, neighborId);
    if (!obj) return false;
    uintptr_t itsp = obj + kPersonSubobjectOffset;
    uintptr_t mot = *reinterpret_cast<const uintptr_t*>(itsp + addr::person_off::kMotivesPtr);
    if (mot <= 0x10000u) return false;
    for (int m = 0; m < 16; ++m) {
        if (m == 0) continue; // Mood is derived by the game
        *reinterpret_cast<float*>(mot + addr::kOffset_cTSMotives_Array + m * 4) = static_cast<float>(motives[m]);
    }
    return true;
}

int32_t RealResolveSim(uint32_t slot, int16_t neighborId);

bool RealApplyInteraction(const SimInteractionEvent& ev) {
    if (ev.actorNeighborId <= 0) return false; // only Sims we can identify for sure
    uint32_t resolvedActorSlot = ev.actorSlot;
    uintptr_t actorObj = FindSimBySlotOrNeighborId(ev.actorSlot, ev.actorNeighborId, &resolvedActorSlot);
    if (!actorObj) return false;

    uintptr_t targetObj = 0;
    if (ev.targetGuid == addr::kGUID_GoHereObject) {
        // Walk Here / Go Here: spawn temporary GoHere target at (targetTx, targetTy, targetLevel)
        uintptr_t mod = ObjTablePtr();
        uintptr_t folder = ObjFolderPtr();
        if (mod && folder) {
            void** folderVt = *reinterpret_cast<void***>(folder);
            using FnGetSelector = void* (__thiscall*)(void*, uint32_t);
            void* sel = reinterpret_cast<FnGetSelector>(folderVt[0x38 / 4])(
                reinterpret_cast<void*>(folder), addr::kGUID_GoHereObject);
            if (sel) {
                void** modVt = *reinterpret_cast<void***>(mod);
                using FnCreateObj = int16_t (__thiscall*)(void*, void*, int, int, int);
                int16_t newId = reinterpret_cast<FnCreateObj>(modVt[addr::kObjTable_CreateObjectSlot])(
                    reinterpret_cast<void*>(mod), sel, 0, 0, 0);
                if (newId > 0) {
                    ObjPose p = {ev.targetTx, ev.targetTy, ev.targetLevel, 0, addr::kGUID_GoHereObject};
                    RealApply(static_cast<uint32_t>(newId - 1), p);
                    targetObj = ObjAt(static_cast<uint32_t>(newId - 1));
                }
            }
        }
    } else if (ev.targetNeighborId > 0) {
        int32_t ts = RealResolveSim(0, ev.targetNeighborId);
        if (ts >= 0) targetObj = ObjAt(static_cast<uint32_t>(ts));
    } else {
        targetObj = ObjAt(ev.targetSlot);
        if (!targetObj || (ev.targetGuid != 0 && ReadObjGuid(targetObj) != ev.targetGuid)) {
            uint32_t n = RealCount();
            int64_t bestDist = 0x7FFFFFFFFFFFFFFFll;
            uintptr_t best = 0;
            for (uint32_t k = 0; k < n; ++k) {
                uintptr_t cand = ObjAt(k);
                if (!cand || ReadObjGuid(cand) != ev.targetGuid) continue;
                int32_t cx = *reinterpret_cast<const int32_t*>(cand + addr::obj_off::kTileX);
                int32_t cy = *reinterpret_cast<const int32_t*>(cand + addr::obj_off::kTileY);
                int64_t dx = static_cast<int64_t>(cx) - ev.targetTx;
                int64_t dy = static_cast<int64_t>(cy) - ev.targetTy;
                int64_t d2 = dx * dx + dy * dy;
                if (d2 < bestDist) { bestDist = d2; best = cand; }
            }
            if (best) targetObj = best;
        }
    }
    if (!targetObj) return false;

    using FnGetAllocCOM = void* (__cdecl*)();
    void* pCOM = reinterpret_cast<FnGetAllocCOM>(addr::Rebase(addr::kFunc_GZGetAllocatorCOM))();
    if (!pCOM) return false;
    void** comVt = *reinterpret_cast<void***>(pCOM);
    if (!comVt) return false;

    void* pInteraction = nullptr;
    using FnCreateInstance = bool (__thiscall*)(void*, uint32_t, uint32_t, void**);
    reinterpret_cast<FnCreateInstance>(comVt[0x0C / 4])(
        pCOM, addr::kCLSID_cTSInteraction, addr::kIID_cITSInteraction, &pInteraction);
    if (!pInteraction) return false;

    void* pITSPerson = reinterpret_cast<void*>(actorObj + kPersonSubobjectOffset);
    void** interVt = *reinterpret_cast<void***>(pInteraction);
    using FnInitInter = bool (__thiscall*)(void*, void*, void*, int, int, int);
    using FnSetParams = void (__thiscall*)(void*, const int16_t*);
    using FnRelease   = int  (__thiscall*)(void*);

    reinterpret_cast<FnInitInter>(interVt[0x20 / 4])(
        pInteraction, pITSPerson, reinterpret_cast<void*>(targetObj),
        ev.interactionIndex, ev.priority, 0 /* 5th arg is an object pointer (cause), not flags: the game derefs it */);
    // Init() zeroes the flags word at +0x68; restore the original autonomous/continuation bits afterwards.
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pInteraction) + 0x68) = ev.flags;
    reinterpret_cast<FnSetParams>(interVt[0x6C / 4])(pInteraction, ev.params);

    g_applyingRemoteInteraction = true;
    void** personVt = *reinterpret_cast<void***>(pITSPerson);
    bool ok = reinterpret_cast<Fn_EnqueueInteraction>(personVt[0x1E8 / 4])(pITSPerson, pInteraction) != 0;
    g_applyingRemoteInteraction = false;

    reinterpret_cast<FnRelease>(interVt[0x08 / 4])(pInteraction);
    return ok;
}

int32_t RealResolveSim(uint32_t slot, int16_t neighborId) {
    uint32_t out = 0;
    if (neighborId <= 0) return RealIsSim(slot) ? static_cast<int32_t>(slot) : -1;
    // Only an exact NeighborID match: the peer's Sim must never be confused with another one of ours.
    uint32_t n = RealCount();
    for (uint32_t i = 0; i < n; ++i) {
        if (!RealIsSim(i)) continue;
        uintptr_t obj = ObjAt(i);
        if (*reinterpret_cast<const int16_t*>(obj + kPersonSubobjectOffset + addr::person_off::kNeighborID) == neighborId) { out = i; return static_cast<int32_t>(out); }
    }
    return -1;
}

int16_t RealSimNeighborId(uint32_t slot) {
    if (!RealIsSim(slot)) return 0;
    return *reinterpret_cast<const int16_t*>(ObjAt(slot) + kPersonSubobjectOffset + addr::person_off::kNeighborID);
}

const ObjectBackend kRealBackend = {
    &RealCount, &RealRead, &RealApply, &RealLotToken, &RealIsSim, &RealSelectedSim,
    &RealCreate, &RealDestroy, &RealReadSimState, &RealApplySimState, &RealApplyInteraction,
    &RealResolveSim, &RealSimNeighborId
};
const ObjectBackend* g_objBackend = &kRealBackend;

// A lot is loaded and has been running for a few seconds (object table populated, simulator in Live/Buy/Build).
bool LocalLotReady() {
    static DWORD since = 0;
    DWORD now = GetTickCount();
    int16_t st = ts2::se::GetSimulatorState();
    bool up = g_objBackend && g_objBackend->count && g_objBackend->count() > 0 && st >= 1 && st <= 3;
    if (!up) { since = 0; return false; }
    if (!since) since = now;
    return now - since > 4000;
}

// ---- Shared logic ----
ObjPose g_objShadow[kMaxObj]; // last pose both sides agreed on
ObjPose g_objLast[kMaxObj];   // pose seen at the previous scan (debounce)
uint8_t g_objKnown[kMaxObj];
uint8_t g_objMissing[kMaxObj]; // consecutive scans where a known object was missing/deleted
ObjRec  g_objInbox[kMaxObj];  // remote change waiting for the game thread
uint8_t g_objInHas[kMaxObj];
uint8_t g_objInSnap[kMaxObj];
uint32_t g_objInCount   = 0;
uint32_t g_objLotToken  = 0;
bool     g_objBaseline  = false;
bool     g_objSnapAsked = false;
bool     g_objSnapPending = false; // Host: a snapshot was requested, send it from the game thread
DWORD    g_objNextScan  = 0;
uint32_t g_objLogBudget = 12;
ObjPose  g_simSent;           // last pose of my Sim that we sent
int32_t  g_simSentIdx   = -1;
DWORD    g_simSentTick  = 0;
DWORD    g_nextSimScan  = 0;
DWORD    g_lastMotivesTx = 0;

constexpr uint32_t kMaxInterQueue = 128;
SimInteractionEvent g_interInbox[kMaxInterQueue];
uint32_t g_interInHead  = 0;
uint32_t g_interInCount = 0;

constexpr uint32_t kMaxSimStateQueue = 16;
SimStatePayload g_simStateInbox[kMaxSimStateQueue];
uint32_t g_simStateInCount = 0;

// Objects appearing/disappearing are replicated as purchases/sales only while the game is in Buy (2) or Build (3)
// mode; in Live mode the game spawns/removes objects by itself (newspaper, trash, visitors' items) and those must
// not be mirrored. TS2COOP_FORCEBUY=1 lets the automated two-instance test exercise the path without the UI.
bool PurchaseModeActive() {
    static int forced = -1;
    if (forced < 0) { char b[4] = {}; GetEnvironmentVariableA("TS2COOP_FORCEBUY", b, sizeof(b)); forced = b[0] == '1'; }
    if (forced) return true;
    int16_t m = ts2::se::GetSimulatorState();
    return m == 2 || m == 3;
}

bool IsSimId(uint32_t id) { return g_objBackend && g_objBackend->isSim && g_objBackend->isSim(id); }

// ---- Slot mapping ---------------------------------------------------------------------------------------------
// The wire protocol speaks HOST slots. The Host uses its own slots as they are; the Guest keeps a two way map between
// host slots and its own, built when it reconciles its lot against the Host's snapshot (the two tables rarely line
// up: different saves, objects the game spawned by itself, purchases that landed in other free slots, ...).
constexpr uint16_t kNoSlot = 0xFFFF;
uint16_t g_r2l[kMaxObj];   // host slot -> my slot
uint16_t g_l2r[kMaxObj];   // my slot   -> host slot
bool     g_mapActive = false;

bool IsClient() { return g_stats.role == Role::Client; }

void MapReset() {
    for (uint32_t i = 0; i < kMaxObj; ++i) { g_r2l[i] = kNoSlot; g_l2r[i] = kNoSlot; }
}
void MapSet(uint32_t wire, uint32_t local) {
    if (wire >= kMaxObj || local >= kMaxObj) return;
    if (g_r2l[wire] != kNoSlot) g_l2r[g_r2l[wire]] = kNoSlot;
    if (g_l2r[local] != kNoSlot) g_r2l[g_l2r[local]] = kNoSlot;
    g_r2l[wire]  = static_cast<uint16_t>(local);
    g_l2r[local] = static_cast<uint16_t>(wire);
}
void MapDropLocal(uint32_t local) {
    if (local >= kMaxObj) return;
    if (g_l2r[local] != kNoSlot) g_r2l[g_l2r[local]] = kNoSlot;
    g_l2r[local] = kNoSlot;
}
uint32_t WireToLocal(uint32_t wire) {
    if (!IsClient() || !g_mapActive) return wire;
    return wire < kMaxObj ? g_r2l[wire] : kNoSlot;
}
uint32_t LocalToWire(uint32_t local) {
    if (!IsClient() || !g_mapActive) return local;
    return local < kMaxObj ? g_l2r[local] : kNoSlot;
}

// ---- Inbound queues (filled by the network pump, drained on the game thread) ---------------------------------
struct ObjEvent { uint8_t mode; ObjRec r; };
constexpr uint32_t kMaxEvents = 512;
ObjEvent g_events[kMaxEvents];
uint32_t g_evHead = 0, g_evCount = 0;

ObjRec   g_snapRecs[kMaxObj];  // objects of the Host's snapshot being received
uint32_t g_snapCount = 0;
ObjRec   g_snapSims[64];       // ... and its Sims (NeighborID travels in oldPose.guid)
uint32_t g_snapSimCount = 0;
bool     g_snapRecv  = false;  // between "begin" and "end"
bool     g_snapReady = false;  // complete snapshot waiting to be applied

// ---- Guest: lot reconciliation against the Host's snapshot ------------------------------------------------------
ObjPose  g_locPose[kMaxObj];
uint8_t  g_locOk[kMaxObj];
uint8_t  g_locClaim[kMaxObj];
uint8_t  g_snapMatched[kMaxObj];
uint16_t g_reconDel[kMaxObj];  // local slots to delete
uint16_t g_reconCre[kMaxObj];  // indexes into g_snapRecs to create
struct Recon { bool active; uint32_t nDel, nCre, iDel, iCre, posed, mapped; } g_recon = {};

void ObjResetLot() {
    g_objBaseline  = false;
    g_objSnapAsked = false;
    g_objInCount   = 0;
    g_interInHead = g_interInCount = 0;
    g_simStateInCount = 0;
    for (uint32_t i = 0; i < kMaxObj; ++i) {
        g_objKnown[i] = 0;
        g_objMissing[i] = 0;
        g_objInHas[i] = 0;
    }
    g_simSentIdx = -1;
    g_stats.objTracked = 0;
    MapReset();
    g_mapActive = false;
    g_evHead = g_evCount = 0;
    g_snapCount = g_snapSimCount = 0;
    g_snapRecv = g_snapReady = false;
    g_recon = {};
}

void ObjQueueInbox(const uint8_t* p, uint32_t len) {
    if (len < sizeof(ObjBatchHdr)) return;
    ObjBatchHdr h;
    memcpy(&h, p, sizeof(h));
    if (h.snapshot == 2 && !g_stats.simSyncEnabled) {
        g_stats.simSyncEnabled = true; // the peer syncs its Sim: follow it
        ts2::se::PushConsoleLog("[Co-op] Parceiro esta sincronizando o Sim dele: sincronia de Sims ligada neste lado.");
    }
    if (h.snapshot == 9) { g_snapCount = g_snapSimCount = 0; g_snapRecv = true; g_snapReady = false; return; }
    if (h.snapshot == 6) { if (g_snapRecv) { g_snapRecv = false; g_snapReady = true; } return; }
    uint32_t n = h.count;
    if (sizeof(h) + static_cast<size_t>(n) * sizeof(ObjRec) > len) n = (len - sizeof(h)) / sizeof(ObjRec);
    for (uint32_t i = 0; i < n; ++i) {
        ObjRec r;
        memcpy(&r, p + sizeof(h) + i * sizeof(ObjRec), sizeof(r));
        switch (h.snapshot) {
            case 1:
                if (g_snapRecv && r.id < kMaxObj && g_snapCount < kMaxObj) g_snapRecs[g_snapCount++] = r;
                break;
            case 7:
                if (g_snapRecv && g_snapSimCount < 64) g_snapSims[g_snapSimCount++] = r;
                break;
            case 3: case 4: case 5:
                if (g_evCount < kMaxEvents) {
                    uint32_t at = (g_evHead + g_evCount) % kMaxEvents;
                    g_events[at].mode = h.snapshot;
                    g_events[at].r = r;
                    ++g_evCount;
                }
                break;
            default: // 0: move, 2: peer Sim pose (keyed by the sender's slot, the latest one wins)
                if (r.id >= kMaxObj) break;
                if (h.snapshot == 2) { ++g_stats.peerSimsSeen; g_stats.peerSimIdx = r.id; }
                if (!g_objInHas[r.id]) { g_objInHas[r.id] = 1; ++g_objInCount; }
                g_objInbox[r.id]  = r;
                g_objInSnap[r.id] = h.snapshot;
                break;
        }
    }
}

void NoteAgreed(uint32_t slot, const ObjPose& fallback) {
    if (slot >= kMaxObj) return;
    ObjPose after = fallback;
    g_objBackend->read(slot, &after);
    g_objShadow[slot] = g_objLast[slot] = after; // what we have now is the agreed pose: never echoed back
    g_objKnown[slot] = 1;
    g_objMissing[slot] = 0;
}

void ObjHandleEvent(const ObjEvent& e) {
    const ObjRec& r = e.r;
    if (e.mode == 3) { // Buy/Build: the peer put a new object into the lot
        if (!g_objBackend->create) return;
        if (r.newPose.guid == 0 || r.newPose.guid == addr::kGUID_GoHereObject) return; // temporary game objects are never mirrored
        ObjPose cur;
        if (IsClient()) {
            uint32_t L = WireToLocal(r.id);
            if (L != kNoSlot && L < g_objBackend->count() && g_objBackend->read(L, &cur) && cur.guid == r.newPose.guid && PoseEq(cur, r.newPose)) {
                NoteAgreed(L, r.newPose); // duplicate delivery
                return;
            }
        }
        uint32_t slot = r.id;
        if (!g_objBackend->create(&slot, r.newPose)) {
            ts2::se::PushConsoleLog("[Co-op] FALHA ao criar objeto do parceiro (slot #%u, GUID=%08X, pos %d,%d).", r.id, r.newPose.guid, r.newPose.tx, r.newPose.ty);
            return;
        }
        ++g_stats.objCreated; ++g_stats.objApplied;
        NoteAgreed(slot, r.newPose);
        if (IsClient()) {
            MapSet(r.id, slot);
        } else {
            // Tell the Guest which Host slot its purchase landed in (r.id was the Guest's own slot)
            uint8_t buf[sizeof(ObjBatchHdr) + sizeof(ObjRec)];
            ObjBatchHdr h = {1, 5};
            ObjRec a = r;
            a.id = static_cast<uint16_t>(slot);
            a.oldPose.tx = static_cast<int32_t>(r.id);
            a.newPose = g_objShadow[slot];
            memcpy(buf, &h, sizeof(h));
            memcpy(buf + sizeof(h), &a, sizeof(a));
            QueueFrame(Msg::ObjUpdate, buf, sizeof(buf));
        }
        ts2::se::PushConsoleLog("[Co-op] Objeto comprado/criado sincronizado (slot do parceiro #%u -> local #%u, GUID=%08X).", r.id, slot, r.newPose.guid);
        return;
    }
    if (e.mode == 4) { // Buy/Build: the peer sold/deleted an object
        uint32_t L = WireToLocal(r.id);
        if (L == kNoSlot || !g_objBackend->destroy) return;
        if (g_objBackend->destroy(L, r.oldPose)) {
            g_objKnown[L] = 0;
            g_objMissing[L] = 0;
            if (IsClient()) MapDropLocal(L);
            ++g_stats.objDeleted; ++g_stats.objApplied;
            ts2::se::PushConsoleLog("[Co-op] Objeto vendido/removido sincronizado (slot #%u, GUID=%08X).", L, r.oldPose.guid);
        }
        return;
    }
    if (e.mode == 5 && IsClient()) { // Host tells us where our purchase went
        uint32_t guestSlot = static_cast<uint32_t>(r.oldPose.tx);
        ObjPose cur;
        if (guestSlot < kMaxObj && g_objBackend->read(guestSlot, &cur) && cur.guid == r.newPose.guid) MapSet(r.id, guestSlot);
    }
}

// Guest: compare the whole lot with the Host's snapshot, build the slot map and queue the creates/deletes that
// make both lots identical. Poses of matched objects are fixed right away.
void ReconBegin() {
    uint32_t n = g_objBackend->count();
    if (n > kMaxObj) n = kMaxObj;
    MapReset();
    g_mapActive = true;
    for (uint32_t id = 0; id < n; ++id) {
        g_locClaim[id] = 0;
        g_locOk[id] = (!IsSimId(id) && g_objBackend->read(id, &g_locPose[id])) ? 1 : 0;
    }
    for (uint32_t i = 0; i < g_snapCount; ++i) g_snapMatched[i] = 0;
    uint32_t exact = 0, moved = 0;
    // pass 1: same slot, same catalog object
    for (uint32_t i = 0; i < g_snapCount; ++i) {
        uint32_t s = g_snapRecs[i].id;
        if (s < n && g_locOk[s] && !g_locClaim[s] && g_locPose[s].guid == g_snapRecs[i].newPose.guid) {
            g_locClaim[s] = 1; g_snapMatched[i] = 1; MapSet(s, s); ++exact;
        }
    }
    // pass 2: same catalog object in another slot (closest one wins)
    for (uint32_t i = 0; i < g_snapCount; ++i) {
        if (g_snapMatched[i]) continue;
        const ObjPose& w = g_snapRecs[i].newPose;
        int64_t best = 0x7FFFFFFFFFFFFFFFll;
        int32_t bestId = -1;
        for (uint32_t id = 0; id < n; ++id) {
            if (!g_locOk[id] || g_locClaim[id] || g_locPose[id].guid != w.guid) continue;
            int64_t dx = static_cast<int64_t>(g_locPose[id].tx) - w.tx, dy = static_cast<int64_t>(g_locPose[id].ty) - w.ty;
            int64_t d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; bestId = static_cast<int32_t>(id); }
        }
        if (bestId >= 0) { g_locClaim[bestId] = 1; g_snapMatched[i] = 1; MapSet(g_snapRecs[i].id, static_cast<uint32_t>(bestId)); ++moved; }
    }
    g_recon = {};
    g_recon.active = true;
    for (uint32_t i = 0; i < g_snapCount; ++i) {
        if (!g_snapMatched[i]) { g_reconCre[g_recon.nCre++] = static_cast<uint16_t>(i); continue; }
        uint32_t L = g_r2l[g_snapRecs[i].id];
        ++g_recon.mapped;
        if (!PoseEq(g_locPose[L], g_snapRecs[i].newPose) && g_objBackend->apply(L, g_snapRecs[i].newPose)) {
            ++g_recon.posed; ++g_stats.objApplied;
        }
        NoteAgreed(L, g_snapRecs[i].newPose);
    }
    for (uint32_t id = 0; id < n; ++id)
        if (g_locOk[id] && !g_locClaim[id] && g_locPose[id].guid != 0 && g_objBackend->destroy) g_reconDel[g_recon.nDel++] = static_cast<uint16_t>(id);
    ts2::se::PushConsoleLog("[Co-op] Sincronizando o lote com o Host: %u moveis no snapshot, %u iguais, %u em outro slot, %u reposicionados, %u a criar, %u a remover.",
                            g_snapCount, exact, moved, g_recon.posed, g_recon.nCre, g_recon.nDel);
}

void ReconFinish() {
    // The Host's Sims: put them where the Host has them (one time, at join / resync)
    uint32_t placed = 0;
    for (uint32_t i = 0; i < g_snapSimCount; ++i) {
        const ObjRec& r = g_snapSims[i];
        int16_t nid = static_cast<int16_t>(r.oldPose.guid);
        int32_t slot = g_objBackend->resolveSim ? g_objBackend->resolveSim(r.id, nid) : static_cast<int32_t>(r.id);
        if (slot < 0) continue;
        ObjPose cur;
        if (!g_objBackend->read(static_cast<uint32_t>(slot), &cur)) continue;
        ObjPose want = r.newPose;
        if (!PoseEq(cur, want) && g_objBackend->apply(static_cast<uint32_t>(slot), want)) ++placed;
    }
    g_recon.active = false;
    g_objBaseline = false; // re-baseline from the now identical lot
    ts2::se::PushConsoleLog("[Co-op] Lote sincronizado com o Host: %u criados, %u removidos, %u Sims posicionados.",
                            g_recon.iCre, g_recon.iDel, placed);
}

void ReconStep() {
    uint32_t budget = 24;
    while (budget && g_recon.iDel < g_recon.nDel) {
        --budget;
        uint32_t L = g_reconDel[g_recon.iDel++];
        ObjPose cur;
        if (!g_objBackend->read(L, &cur) || cur.guid != g_locPose[L].guid) continue;
        if (g_objBackend->destroy(L, cur)) {
            g_objKnown[L] = 0; g_objMissing[L] = 0; MapDropLocal(L);
            ++g_stats.objDeleted;
        }
    }
    while (budget && g_recon.iCre < g_recon.nCre) {
        --budget;
        const ObjRec& r = g_snapRecs[g_reconCre[g_recon.iCre++]];
        uint32_t slot = r.id;
        if (g_objBackend->create && g_objBackend->create(&slot, r.newPose)) {
            MapSet(r.id, slot);
            NoteAgreed(slot, r.newPose);
            ++g_stats.objCreated;
        } else {
            static uint32_t warn = 6;
            if (warn) { --warn; ts2::se::PushConsoleLog("[Co-op] Nao consegui criar o objeto #%u (GUID=%08X) do Host.", r.id, r.newPose.guid); }
        }
    }
    if (g_recon.iDel >= g_recon.nDel && g_recon.iCre >= g_recon.nCre) ReconFinish();
}

void ObjApplyInbox() {
    if (!g_objBackend) return;

    // 1. Queued Sim motives/state updates (matched by NeighborID)
    for (uint32_t i = 0; i < g_simStateInCount; ++i) {
        const SimStatePayload& sp = g_simStateInbox[i];
        // sp.slot is the peer's slot; our copy of that Sim is found by NeighborID
        int32_t L = g_objBackend->resolveSim ? g_objBackend->resolveSim(sp.slot, sp.neighborId) : static_cast<int32_t>(sp.slot);
        if (L < 0) continue;
        if (g_objBackend->applySimState && g_objBackend->applySimState(static_cast<uint32_t>(L), sp.neighborId, sp.motives)) {
            ++g_stats.motivesSynced;
        }
    }
    g_simStateInCount = 0;

    // 2. Queued SimAntics interactions (Pie Menu / Walk Here / Object interactions)
    while (g_interInCount > 0) {
        SimInteractionEvent ev = g_interInbox[g_interInHead];
        g_interInHead = (g_interInHead + 1) % kMaxInterQueue;
        --g_interInCount;
        if (ev.targetSlot != 0xFFFF) {
            uint32_t L = WireToLocal(ev.targetSlot);
            if (L != kNoSlot) ev.targetSlot = static_cast<uint16_t>(L);
        }
        if (g_objBackend->applyInteraction && g_objBackend->applyInteraction(ev)) {
            ++g_stats.interactionsSynced;
            g_stats.lastSyncedInteractionIdx = ev.interactionIndex;
            g_stats.lastSyncedTargetSlot     = ev.targetSlot;
            ts2::se::PushConsoleLog("[Co-op] Acao sincronizada no Sim #%u (NID %d) -> Alvo #%u guid=%08X (Menu=%d, Tree=%d, prio=%d, flags=%X, params=%d,%d,%d,%d)",
                                    ev.actorSlot, (int)ev.actorNeighborId, ev.targetSlot, ev.targetGuid, ev.interactionIndex, ev.actionTreeId,
                                    ev.priority, ev.flags, ev.params[0], ev.params[1], ev.params[2], ev.params[3]);
        }
    }

    // 3. Buy/Build creates, deletes and acks, in order
    for (uint32_t budget = 32; budget && g_evCount > 0; --budget) {
        ObjEvent e = g_events[g_evHead];
        g_evHead = (g_evHead + 1) % kMaxEvents;
        --g_evCount;
        ObjHandleEvent(e);
    }

    // 4. Lot reconciliation (Guest)
    if (g_snapReady && !g_recon.active) {
        g_snapReady = false;
        if (IsClient()) ReconBegin();
    }
    if (g_recon.active) ReconStep();

    if (g_objInCount == 0) return;

    // 5. Poses (moves of furniture, the peer's Sim)
    uint32_t budget = kObjApplyPerTick;
    uint32_t n = g_objBackend->count();
    for (uint32_t id = 0; id < kMaxObj && g_objInCount > 0 && budget > 0; ++id) {
        if (!g_objInHas[id]) continue;
        --budget;
        uint8_t mode = g_objInSnap[id];
        ObjRec r = g_objInbox[id];
        g_objInHas[id] = 0;
        --g_objInCount;

        ObjPose cur;
        if (mode == 2) { // the peer's Sim: matched by NeighborID, never echoed back
            if (!g_stats.simSyncEnabled) continue;
            int16_t nid = static_cast<int16_t>(r.oldPose.guid);
            int32_t L = g_objBackend->resolveSim ? g_objBackend->resolveSim(id, nid) : static_cast<int32_t>(id);
            if (L < 0 || static_cast<uint32_t>(L) >= n || !g_objBackend->read(static_cast<uint32_t>(L), &cur)) {
                static uint32_t budgetWarn = 6;
                if (budgetWarn) { --budgetWarn; ts2::se::PushConsoleLog("[Co-op] AVISO: Sim do parceiro (NeighborID %d) nao existe aqui.", (int)nid); }
                continue;
            }
            // Walking Sims are driven by the replicated interactions; only snap them back when they drifted.
            int64_t dx = static_cast<int64_t>(cur.tx) - r.newPose.tx, dy = static_cast<int64_t>(cur.ty) - r.newPose.ty;
            // A routed/animated Sim (sitting, using an object) is positioned by its own animation; snapping it mid-action
            // leaves it in the wrong pose. Only correct a drift that is large and persists for several updates.
            const int64_t kDrift = 4 * 65536;
            static uint8_t driftHits[kMaxObj];
            uint32_t Lu = static_cast<uint32_t>(L);
            if (Lu < kMaxObj) {
                if (cur.level == r.newPose.level && dx * dx + dy * dy <= kDrift * kDrift) { driftHits[Lu] = 0; continue; }
                if (++driftHits[Lu] < 4) continue;
                driftHits[Lu] = 0;
            }
            if (g_objBackend->apply(static_cast<uint32_t>(L), r.newPose)) {
                ++g_stats.simsApplied;
                static uint32_t budgetLog = 10;
                if (budgetLog) {
                    --budgetLog;
                    ts2::se::PushConsoleLog("[Co-op] Sim #%d do parceiro reposicionado de (%d,%d) para (%d,%d)", L, cur.tx, cur.ty, r.newPose.tx, r.newPose.ty);
                }
            }
            continue;
        }

        uint32_t L = WireToLocal(id);
        if (L == kNoSlot || L >= n || !g_objBackend->read(L, &cur)) continue;
        if (PoseEq(cur, r.newPose)) { NoteAgreed(L, cur); continue; }
        bool allowed = PoseEq(cur, r.oldPose) || (r.newPose.guid != 0 && cur.guid == r.newPose.guid);
        if (!allowed) {
            ++g_stats.objConflicts;
            if (g_objLogBudget) {
                --g_objLogBudget;
                ts2::se::PushConsoleLog("[Co-op] Movel #%u ignorado: posicao local (%d,%d) difere da esperada (%d,%d)",
                                        L, cur.tx, cur.ty, r.oldPose.tx, r.oldPose.ty);
            }
            continue;
        }
        if (g_objBackend->apply(L, r.newPose)) {
            NoteAgreed(L, r.newPose);
            ++g_stats.objApplied;
        }
    }
}

// Host: stream the whole lot (every object and every Sim) so the Guest can mirror it exactly.
void ObjSendSnapshot() {
    if (!g_objBackend || !g_stats.peerConnected) return;
    if (!g_objBaseline || kTxCap - g_txUsed < 700 * 1024) { g_objSnapPending = true; return; } // try again next scan
    static uint8_t buf[sizeof(ObjBatchHdr) + kObjBatch * sizeof(ObjRec)];
    uint32_t n = g_objBackend->count();
    if (n > kMaxObj) n = kMaxObj;
    uint32_t cnt = 0, sent = 0;
    auto control = [&](uint8_t mode) { ObjBatchHdr h = {0, mode}; QueueFrame(Msg::ObjUpdate, &h, sizeof(h)); };
    auto flush = [&](uint8_t mode) {
        if (cnt == 0) return;
        ObjBatchHdr h = {static_cast<uint16_t>(cnt), mode};
        memcpy(buf, &h, sizeof(h));
        QueueFrame(Msg::ObjUpdate, buf, sizeof(h) + cnt * sizeof(ObjRec));
        sent += cnt;
        cnt = 0;
    };
    control(9);
    for (uint32_t id = 0; id < n; ++id) {
        ObjPose cur;
        if (IsSimId(id)) continue;
        if (!g_objBackend->read(id, &cur)) continue;
        ObjRec r;
        r.id = static_cast<uint16_t>(id);
        r.oldPose = cur;
        r.newPose = cur;
        memcpy(buf + sizeof(ObjBatchHdr) + cnt * sizeof(ObjRec), &r, sizeof(r));
        if (++cnt == kObjBatch) flush(1);
    }
    flush(1);
    uint32_t sims = 0;
    for (uint32_t id = 0; id < n && sims < 64; ++id) {
        ObjPose cur;
        if (!IsSimId(id) || !g_objBackend->read(id, &cur)) continue;
        ObjRec r;
        r.id = static_cast<uint16_t>(id);
        r.newPose = cur;
        r.oldPose = cur;
        r.oldPose.guid = static_cast<uint32_t>(static_cast<uint16_t>(g_objBackend->simNeighborId ? g_objBackend->simNeighborId(id) : 0));
        memcpy(buf + sizeof(ObjBatchHdr) + cnt * sizeof(ObjRec), &r, sizeof(r));
        ++cnt; ++sims;
    }
    flush(7);
    control(6);
    ts2::se::PushConsoleLog("[Co-op] Snapshot do lote enviado: %u moveis, %u Sims.", sent - sims, sims);
}

void ObjScan() {
    uint32_t token = g_objBackend->lotToken();
    uint32_t n = g_objBackend->count();
    if (n > kMaxObj) n = kMaxObj;
    if (token != g_objLotToken) { ObjResetLot(); g_objLotToken = token; }
    g_stats.objSyncAvailable = (n != 0);
    if (n == 0) return;

    if (!g_objBaseline) {
        uint32_t tracked = 0;
        for (uint32_t id = 0; id < n; ++id) {
            ObjPose cur;
            if (IsSimId(id)) { g_objKnown[id] = 0; continue; }
            if (g_objBackend->read(id, &cur)) {
                g_objShadow[id] = g_objLast[id] = cur;
                g_objKnown[id] = 1;
                g_objMissing[id] = 0;
                ++tracked;
            } else {
                g_objKnown[id] = 0;
                g_objMissing[id] = 0;
            }
        }
        g_objBaseline = true;
        g_stats.objTracked = tracked;
        ts2::se::PushConsoleLog("[Co-op] Sincronia de moveis: %u objetos monitorados.", tracked);
        return;
    }

    if (g_stats.role == Role::Client && !g_objSnapAsked && g_stats.peerConnected) {
        QueueFrame(Msg::ObjSnapReq, nullptr, 0);
        g_objSnapAsked = true;
        ts2::se::PushConsoleLog("[Co-op] Pedindo o lote completo ao Host.");
    }

    // A Guest that has not mirrored the Host's lot yet (or is doing it right now) must not send anything: its slots
    // are not the Host's slots until the map exists.
    const bool holdBack = IsClient() && (!g_mapActive || g_recon.active);
    if (!g_stats.peerConnected || !g_stats.objSyncEnabled || holdBack) {
        // Keep the shadow current so reconnecting does not flood the peer with stale changes.
        uint32_t tracked = 0;
        for (uint32_t id = 0; id < n; ++id) {
            ObjPose cur;
            if (IsSimId(id)) { g_objKnown[id] = 0; continue; }
            if (g_objBackend->read(id, &cur)) {
                g_objShadow[id] = g_objLast[id] = cur;
                g_objKnown[id] = 1;
                g_objMissing[id] = 0;
                ++tracked;
            } else {
                g_objKnown[id] = 0;
                g_objMissing[id] = 0;
            }
        }
        g_stats.objTracked = tracked;
        return;
    }
    if (kTxCap - g_txUsed < 128 * 1024) return; // let the socket drain first

    static uint8_t buf[sizeof(ObjBatchHdr) + kObjBatch * sizeof(ObjRec)];
    uint32_t cnt = 0;
    auto flush = [&](uint8_t snapMode) {
        if (cnt == 0) return;
        ObjBatchHdr h = {static_cast<uint16_t>(cnt), snapMode};
        memcpy(buf, &h, sizeof(h));
        QueueFrame(Msg::ObjUpdate, buf, sizeof(h) + cnt * sizeof(ObjRec));
        g_stats.objSent += cnt;
        cnt = 0;
    };
    uint32_t tracked = 0;
    for (uint32_t id = 0; id < n; ++id) {
        ObjPose cur;
        if (IsSimId(id)) { g_objKnown[id] = 0; continue; }
        if (!g_objBackend->read(id, &cur)) {
            if (g_objKnown[id]) {
                // Only treat as Buy/Build deletion if the game is in Buy (2) or Build (3) mode
                if (PurchaseModeActive() && g_objShadow[id].guid != 0) {
                    if (++g_objMissing[id] >= 2) {
                        uint32_t w = LocalToWire(id);
                        if (w != kNoSlot) {
                            flush(0);
                            ObjRec delRec = {static_cast<uint16_t>(w), g_objShadow[id], g_objShadow[id]};
                            memcpy(buf + sizeof(ObjBatchHdr), &delRec, sizeof(delRec));
                            cnt = 1;
                            flush(4);
                        }
                        if (IsClient()) MapDropLocal(id);
                        g_objKnown[id] = 0;
                        g_objMissing[id] = 0;
                    }
                } else {
                    g_objKnown[id] = 0;
                    g_objMissing[id] = 0;
                }
            }
            continue;
        }
        g_objMissing[id] = 0;
        ++tracked;
        if (!g_objKnown[id]) {
            g_objShadow[id] = g_objLast[id] = cur;
            g_objKnown[id] = 1;
            if (PurchaseModeActive() && cur.guid != 0) {
                flush(0);
                ObjRec createRec = {static_cast<uint16_t>(id), cur, cur}; // Guest: id is its own slot, the Host acks the real one
                memcpy(buf + sizeof(ObjBatchHdr), &createRec, sizeof(createRec));
                cnt = 1;
                flush(3);
            }
            continue;
        }
        if (PoseEq(cur, g_objShadow[id])) { g_objLast[id] = cur; continue; }
        if (!PoseEq(cur, g_objLast[id])) { g_objLast[id] = cur; continue; } // still moving
        uint32_t w = LocalToWire(id);
        if (w == kNoSlot) { g_objShadow[id] = g_objLast[id] = cur; continue; } // not known to the Host (yet)
        ObjRec r;
        r.id = static_cast<uint16_t>(w);
        r.oldPose = g_objShadow[id];
        r.newPose = cur;
        memcpy(buf + sizeof(ObjBatchHdr) + cnt * sizeof(ObjRec), &r, sizeof(r));
        g_objShadow[id] = cur;
        if (++cnt == kObjBatch) flush(0);
    }
    g_stats.objTracked = tracked;
    flush(0);
}

// Each player streams the pose & motives of the Sim they control (their selected Sim) whenever it changes, plus a
// keep-alive every second so the other side snaps its copy back if its own AI wandered off.
void ObjScanSims() {
    if (!g_objBackend || !g_objBackend->isSim || !g_objBackend->selectedSim) return;
    if (!g_stats.peerConnected || !g_stats.simSyncEnabled || !g_objBaseline) return;
    g_stats.mySimIdx = g_objBackend->selectedSim();
    // Host-authoritative: the Host runs the AI of every Sim and streams their pose and motives; the Guest's copies
    // are followers (its own commands still reach the Host as replicated interactions).
    if (IsClient()) return;
    if (kTxCap - g_txUsed < 128 * 1024) return;
    DWORD now = GetTickCount();
    const bool sendMotives = g_objBackend->readSimState && now - g_lastMotivesTx >= 1000;
    if (sendMotives) g_lastMotivesTx = now;
    uint32_t n = g_objBackend->count();
    if (n > kMaxObj) n = kMaxObj;
    static struct { uint16_t slot; ObjPose sent; DWORD tick; } st[16];
    static uint32_t stCount = 0;
    for (uint32_t id = 0; id < n; ++id) {
        if (!g_objBackend->isSim(id)) continue;
        ObjPose cur;
        if (!g_objBackend->read(id, &cur)) continue;
        if (sendMotives) {
            SimStatePayload sp = {};
            sp.slot = static_cast<uint16_t>(id);
            sp.isControlled = 1;
            if (g_objBackend->readSimState(id, &sp.neighborId, sp.motives)) QueueFrame(Msg::SimState, &sp, sizeof(sp));
        }
        uint32_t k = 0;
        while (k < stCount && st[k].slot != id) ++k;
        if (k == stCount) {
            if (stCount >= 16) continue;
            st[k].slot = static_cast<uint16_t>(id); st[k].sent = ObjPose(); st[k].sent.tx = -1; st[k].tick = 0;
            ++stCount;
        }
        if (PoseEq(cur, st[k].sent) && now - st[k].tick < 1000) continue;
        st[k].sent = cur;
        st[k].tick = now;
        uint8_t buf[sizeof(ObjBatchHdr) + sizeof(ObjRec)];
        ObjBatchHdr h = {1, 2};
        ObjRec r;
        r.id = static_cast<uint16_t>(id);
        r.oldPose = cur;
        r.oldPose.guid = static_cast<uint32_t>(static_cast<uint16_t>(g_objBackend->simNeighborId ? g_objBackend->simNeighborId(id) : 0));
        r.newPose = cur;
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), &r, sizeof(r));
        QueueFrame(Msg::ObjUpdate, buf, sizeof(buf));
        ++g_stats.simsSent;
    }
}

// Diagnostic (real game only): lists the Sims of the lot, which one the game calls "selected" and
// where each lives in the object table. Printed every 6 s while connected.
void DumpLotPersons() {
    if (g_objBackend != &kRealBackend || !g_stats.peerConnected || !g_stats.simSyncEnabled) return;
    static DWORD next = 0;
    DWORD now = GetTickCount();
    if (now < next) return;
    next = now + 20000;
    uintptr_t mod = ObjTablePtr();
    if (!mod) return;
    uintptr_t pb = *reinterpret_cast<const uintptr_t*>(mod + 0x1A70);
    uintptr_t pe = *reinterpret_cast<const uintptr_t*>(mod + 0x1A74);
    uintptr_t tb = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecBegin);
    uintptr_t te = *reinterpret_cast<const uintptr_t*>(mod + addr::kObjTable_VecEnd);
    if (pb < 0x10000u || pe < pb || tb < 0x10000u || te < tb) return;
    uint32_t np = static_cast<uint32_t>((pe - pb) >> 2), nt = static_cast<uint32_t>((te - tb) >> 2);
    uintptr_t sel = ts2::se::GetSelectedSimPtr();
    ts2::se::PushConsoleLog("[Co-op] Sims: %u na lista, %u objetos na tabela; selecionado=%08X (slot %d); enviados=%u aplicados=%u vistos=%u",
                            np, nt, static_cast<unsigned>(sel), g_stats.mySimIdx, g_stats.simsSent, g_stats.simsApplied, g_stats.peerSimsSeen);
    for (uint32_t i = 0; i < np && i < 12; ++i) {
        uintptr_t p = *reinterpret_cast<const uintptr_t*>(pb + i * 4u);
        if (p < 0x10000u) continue;
        int32_t slot = -1;
        for (uint32_t k = 0; k < nt; ++k)
            if (*reinterpret_cast<const uintptr_t*>(tb + k * 4u) == p - kPersonSubobjectOffset) { slot = static_cast<int32_t>(k); break; }
        ObjPose pose = {};
        if (slot >= 0) g_objBackend->read(static_cast<uint32_t>(slot), &pose);
        ts2::se::PushConsoleLog("[Co-op]   Sim %u: slot=%d flags=%s pos=(%d,%d)%s", i, slot,
                                p == sel ? "selecionado" : "-", pose.tx, pose.ty, p == sel ? " <==" : "");
    }
}

// ----------------------------------------------------------------------------
// Incoming frames
// ----------------------------------------------------------------------------
// (object sync frames are queued here and applied later from the game thread: ObjectSyncTick)
void HandleFrame(Msg type, const uint8_t* p, uint32_t len) {
    ++g_stats.packetsReceived;
    switch (type) {
        case Msg::Hello: {
            if (len >= sizeof(HelloPayload)) {
                HelloPayload h;
                memcpy(&h, p, sizeof(h));
                if (h.version != kProtoVersion) {
                    ts2::se::PushConsoleLog("[Co-op] AVISO: versao do protocolo diferente (peer=%u, local=%u)", h.version, kProtoVersion);
                }
            }
            if (g_stats.role == Role::Host) SendStateNow();
            break;
        }
        case Msg::State: {
            if (g_stats.role == Role::Client && len >= sizeof(StatePayload)) {
                StatePayload s;
                memcpy(&s, p, sizeof(s));
                ApplyHostState(s);
            }
            break;
        }
        case Msg::SpeedPause: {
            if (len < sizeof(SpeedPausePayload)) break;
            SpeedPausePayload s;
            memcpy(&s, p, sizeof(s));
            ts2::se::SetSimulatorSpeed(s.speed);
            ts2::se::SetSimulatorPaused(s.pause > 0);
            ts2::se::PushConsoleLog("[Co-op RX] Velocidade=%d Pausa=%d", s.speed, s.pause);
            break;
        }
        case Msg::Bhav: {
            if (len < sizeof(BhavPayload)) break;
            BhavPayload ev;
            memcpy(&ev, p, sizeof(ev));
            ++g_stats.bhavEventsSynced;
            if (ev.eventId == 0x0081 && ev.stackObjId < 256) {
                ts2::se::SetSharedRegister(static_cast<uint8_t>(ev.stackObjId), ev.arg0, false);
            }
            break;
        }
        case Msg::ResyncReq: {
            if (g_stats.role == Role::Host) SendStateNow();
            break;
        }
        case Msg::ObjUpdate: {
            if (g_stats.objSyncEnabled) ObjQueueInbox(p, len);
            break;
        }
        case Msg::ObjSnapReq: {
            if (g_stats.role == Role::Host && g_stats.objSyncEnabled) g_objSnapPending = true;
            break;
        }
        case Msg::SimState: {
            if (len >= sizeof(SimStatePayload)) {
                SimStatePayload sp;
                memcpy(&sp, p, sizeof(sp));
                if (sp.isControlled) {
                    g_stats.peerSimIdx        = sp.slot;
                    g_stats.peerSimNeighborId = sp.neighborId;
                }
                if (g_simStateInCount < kMaxSimStateQueue) {
                    g_simStateInbox[g_simStateInCount++] = sp;
                }
            }
            break;
        }
        case Msg::SimInteraction: {
            if (len >= sizeof(SimInteractionEvent)) {
                SimInteractionEvent ev;
                memcpy(&ev, p, sizeof(ev));
                if (g_interInCount < kMaxInterQueue) {
                    uint32_t tail = (g_interInHead + g_interInCount) % kMaxInterQueue;
                    g_interInbox[tail] = ev;
                    ++g_interInCount;
                }
            }
            break;
        }
        case Msg::XferBegin: {
            if (g_stats.role == Role::Client && len >= sizeof(XferBeginPayload)) {
                XferBeginPayload b;
                memcpy(&b, p, sizeof(b));
                GuestBegin(b);
            }
            break;
        }
        case Msg::XferFile:    if (g_stats.role == Role::Client) GuestFile(p, len);  break;
        case Msg::XferChunk:   if (g_stats.role == Role::Client) GuestChunk(p, len); break;
        case Msg::XferFileEnd: if (g_stats.role == Role::Client) GuestFileEnd();     break;
        case Msg::XferEnd:     if (g_stats.role == Role::Client) GuestEnd();         break;
        case Msg::XferAbort: {
            char why[100] = {};
            if (len > 0) memcpy(why, p, len < 96 ? len : 96);
            if (g_stats.role == Role::Host) {
                AbortSend(why[0] ? why : "O convidado recusou a transferencia.");
            } else if (g_rxFile != INVALID_HANDLE_VALUE) {
                CloseHandle(g_rxFile); g_rxFile = INVALID_HANDLE_VALUE; g_rxActive = false;
            }
            break;
        }
        case Msg::XferAck: {
            if (g_stats.role == Role::Host) {
                g_stats.xferState = XferState::Done;
                SetXferMessage("Convidado instalou %s. Peca para ele carregar a vizinhanca.", g_stats.xferHood);
            }
            break;
        }
    }
}

void ParseRx() {
    uint32_t off = 0;
    while (g_rxLen - off >= sizeof(FrameHdr)) {
        FrameHdr hdr;
        memcpy(&hdr, g_rx + off, sizeof(hdr));
        if (hdr.len > kMaxFrame) { CloseConnection("quadro invalido"); return; }
        if (g_rxLen - off < sizeof(FrameHdr) + hdr.len) break;
        HandleFrame(static_cast<Msg>(hdr.type), g_rx + off + sizeof(FrameHdr), hdr.len);
        if (g_conn == INVALID_SOCKET) return; // handler may have closed the connection
        off += sizeof(FrameHdr) + hdr.len;
    }
    if (off > 0) {
        g_rxLen -= off;
        if (g_rxLen) memmove(g_rx, g_rx + off, g_rxLen);
    }
}

void PumpRx() {
    for (int i = 0; i < 64 && g_conn != INVALID_SOCKET; ++i) {
        if (g_rxLen >= kRxCap) { ParseRx(); if (g_rxLen >= kRxCap) { CloseConnection("buffer cheio"); return; } }
        int r = recv(g_conn, reinterpret_cast<char*>(g_rx + g_rxLen), static_cast<int>(kRxCap - g_rxLen), 0);
        if (r > 0) {
            g_rxLen += static_cast<uint32_t>(r);
            ParseRx();
        } else if (r == 0) {
            CloseConnection("conexao encerrada pelo parceiro");
            return;
        } else {
            if (WSAGetLastError() != WSAEWOULDBLOCK) CloseConnection("erro de rede");
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// Connection management
// ----------------------------------------------------------------------------
void ResetLinkState() {
    g_txHead = g_txUsed = 0;
    g_rxLen = 0;
    if (g_xferFile != INVALID_HANDLE_VALUE) { CloseHandle(g_xferFile); g_xferFile = INVALID_HANDLE_VALUE; }
    if (g_rxFile != INVALID_HANDLE_VALUE)   { CloseHandle(g_rxFile);   g_rxFile   = INVALID_HANDLE_VALUE; }
    g_xferSending = false;
    g_rxActive    = false;
    g_sendPending = false;
}

void CloseConnection(const char* reason) {
    if (g_conn != INVALID_SOCKET) {
        closesocket(g_conn);
        g_conn = INVALID_SOCKET;
    }
    bool wasConnected = g_stats.peerConnected;
    g_stats.peerConnected = false;
    g_objSnapPending = false;
    g_connecting = false;
    g_nextConnectTick = GetTickCount() + 2000;
    if (g_stats.xferState == XferState::Sending || g_stats.xferState == XferState::Receiving) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Transferencia interrompida: %s", reason);
    }
    ResetLinkState();
    if (wasConnected) {
        ts2::se::PushConsoleLog("[Co-op] Parceiro desconectado (%s).", reason);
    }
}

void OnConnected(bool loopback) {
    // Test harness only: lets two processes with different USERPROFILEs transfer over 127.0.0.1
    char probe[4] = {};
    if (loopback && GetEnvironmentVariableA("TS2_COOP_TEST", probe, sizeof(probe)) > 0) loopback = false;
    g_stats.peerConnected = true;
    g_stats.peerLoopback  = loopback;
    ResetLinkState();
    ObjResetLot(); // start from a fresh object baseline
    g_lastStateTx = 0;
    ULONG nb = 1;
    ioctlsocket(g_conn, FIONBIO, &nb);
    int one = 1;
    setsockopt(g_conn, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    SendHello();
    SetConnectMessage("Conectado%s.", loopback ? " (mesmo PC)" : "");
    ts2::se::PushConsoleLog("[Co-op] Parceiro conectado%s!", loopback ? " (mesmo PC)" : "");
}

void HostAccept() {
    if (g_listen == INVALID_SOCKET) return;
    sockaddr_in from = {};
    int fromLen = sizeof(from);
    SOCKET s = accept(g_listen, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (s == INVALID_SOCKET) return;
    if (g_conn != INVALID_SOCKET) { closesocket(s); return; } // only one guest
    g_conn = s;
    bool loop = (from.sin_addr.s_addr & 0xFFu) == 127u;
    strncpy(g_stats.peerIp, loop ? "127.0.0.1" : "(convidado)", sizeof(g_stats.peerIp) - 1);
    OnConnected(loop);
}

void GuestTryConnect() {
    DWORD now = GetTickCount();
    if (!g_connecting) {
        if (now < g_nextConnectTick) return;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { g_nextConnectTick = now + 2000; return; }
        ULONG nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(g_stats.peerPort);
        addr.sin_addr.s_addr = inet_addr(g_stats.peerIp);
        int rc = connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        int err = (rc == 0) ? 0 : WSAGetLastError();
        if (rc == 0 || err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY) {
            g_conn = s;
            g_connecting = true;
            g_connectStartTick = now;
            if (rc == 0) {
                g_connecting = false;
                OnConnected((addr.sin_addr.s_addr & 0xFFu) == 127u);
            } else {
                SetConnectMessage("Conectando a %s:%u...", g_stats.peerIp, g_stats.peerPort);
            }
        } else {
            closesocket(s);
            g_nextConnectTick = now + 2000;
            SetConnectMessage("Falha (WSA=%d). Tentando novamente...", err);
        }
        return;
    }

    // Waiting for the non-blocking connect to complete
    fd_set wr = {}, ex = {};
    wr.fd_count = 1; wr.fd_array[0] = g_conn;
    ex.fd_count = 1; ex.fd_array[0] = g_conn;
    timeval tv = {0, 0};
    int sel = select(0, nullptr, &wr, &ex, &tv);
    if (sel > 0) {
        int soerr = 0, sl = sizeof(soerr);
        getsockopt(g_conn, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl);
        if (wr.fd_count > 0 && soerr == 0) {
            g_connecting = false;
            uint32_t ip = inet_addr(g_stats.peerIp);
            OnConnected((ip & 0xFFu) == 127u);
            return;
        }
        closesocket(g_conn);
        g_conn = INVALID_SOCKET;
        g_connecting = false;
        g_nextConnectTick = now + 2000;
        SetConnectMessage("Recusado pelo Host. Tentando novamente...");
        return;
    }
    if (now - g_connectStartTick > 5000) {
        closesocket(g_conn);
        g_conn = INVALID_SOCKET;
        g_connecting = false;
        g_nextConnectTick = now + 1000;
        SetConnectMessage("Tempo esgotado. Tentando novamente...");
    }
}

// Hook: cTSSimulator::SimulateTick @ 0xA0482F90 (ecx = cITSSimulator*) - only used to pump the network
uint8_t __fastcall Hooked_cTSSimulator_SimulateTick(void* pSimulatorRaw, void* /*edx*/) {
    PollNetwork();
    return g_detourSimulateTick.Original<Fn_SimulateTick>()(pSimulatorRaw);
}

// Hook: cTSSimulator::OnIdle @ 0xA04829B0 (vtable[10], runs every frame even when paused).
// A top-level, single-threaded place where it is safe to move objects.
void __fastcall Hooked_cTSSimulator_OnIdle(void* pSimulatorRaw, void* /*edx*/) {
    g_detourOnIdle.Original<Fn_OnIdle>()(pSimulatorRaw);
    ts2::qol::RunPendingSave();
    ObjectSyncTick();
}

// Hook: cITSPerson::EnqueueInteraction @ 0xA0512360 (vtable[122] +0x1E8, 5-byte prologue: 55 8B EC 6A FF)
// Intercepts user-directed Pie Menu / Walk Here interactions on the locally controlled Sim and replicates them to the peer!
uint8_t __fastcall Hooked_cTSPerson_EnqueueInteraction(void* pITSPerson, void* /*edx*/, void* pITSInteraction) {
    // Host-authoritative Sims: only the Host's AI picks autonomous actions (for every Sim) and it replicates them. The
    // Guest's AI is silenced. Commands the players give (pie menu, Walk Here) are applied locally and replicated to
    // the other side, whichever Sim they target, so both players can order any Sim around.
    bool coop = !g_applyingRemoteInteraction && g_stats.peerConnected && g_stats.simSyncEnabled && pITSPerson && pITSInteraction &&
                LocalLotReady() && !(IsClient() && (!g_mapActive || g_recon.active));
    bool replicate = false;
    int16_t nid = 0;
    uint32_t flags = 0;
    int32_t prio = 0;
    if (coop) {
        uintptr_t ib = reinterpret_cast<uintptr_t>(pITSInteraction);
        flags = *reinterpret_cast<const uint32_t*>(ib + addr::inter_off::kFlags);
        prio  = *reinterpret_cast<const int32_t*>(ib + addr::inter_off::kPriority);
        nid   = *reinterpret_cast<const int16_t*>(reinterpret_cast<uintptr_t>(pITSPerson) + addr::person_off::kNeighborID);
        const bool autonomous = (flags & 1u) != 0 && (flags & 2u) == 0;
        const bool user = !autonomous && (flags & 2u) == 0 && prio >= 0x32;
        if (autonomous && IsClient() && g_stats.blockPeerAutonomy) {
            static uint32_t blocked = 0;
            if (++blocked <= 3) ts2::se::PushConsoleLog("[Co-op] IA local do convidado silenciada (NID %d): quem decide as acoes autonomas e o Host.", (int)nid);
            return 0;
        }
        replicate = user || (autonomous && !IsClient());
    }
    uint8_t ok = g_detourEnqueueInteraction.Original<Fn_EnqueueInteraction>()(pITSPerson, pITSInteraction);
    if (coop && nid > 0) {
        static uint32_t seen = 0;
        if (++seen <= 60) {
            uintptr_t ib = reinterpret_cast<uintptr_t>(pITSInteraction);
            ts2::se::PushConsoleLog("[Co-op] fila: NID %d recebeu interacao tree=%d idx=%d prio=%d flags=%X alvo=%d ok=%d %s", (int)nid,
                (int)*reinterpret_cast<const int16_t*>(ib + addr::inter_off::kActionTreeID),
                (int)*reinterpret_cast<const int32_t*>(ib + addr::inter_off::kInteractionIndex), (int)prio, flags,
                (int)*reinterpret_cast<const int16_t*>(ib + addr::inter_off::kTargetObjectID), (int)ok, replicate ? "-> replica" : "(local)");
        }
    }
    if (ok && coop && replicate && nid > 0) {
        uintptr_t ib = reinterpret_cast<uintptr_t>(pITSInteraction);
        int16_t actorObjId  = *reinterpret_cast<const int16_t*>(ib + addr::inter_off::kActorObjectID);
        int16_t targetObjId = *reinterpret_cast<const int16_t*>(ib + addr::inter_off::kTargetObjectID);
        if (actorObjId > 0 && targetObjId > 0) {
            SimInteractionEvent ev = {};
            ev.actorSlot        = static_cast<uint16_t>(actorObjId - 1);
            ev.actorNeighborId  = nid;
            ev.targetSlot       = static_cast<uint16_t>(targetObjId - 1);
            ev.interactionIndex = *reinterpret_cast<const int32_t*>(ib + addr::inter_off::kInteractionIndex);
            ev.priority         = prio;
            ev.actionTreeId     = *reinterpret_cast<const int16_t*>(ib + addr::inter_off::kActionTreeID);
            ev.flags            = flags;
            for (int k = 0; k < 4; ++k) {
                ev.params[k] = *reinterpret_cast<const int16_t*>(ib + addr::inter_off::kParams + k * 2);
            }
            if (uintptr_t tObj = ObjAt(ev.targetSlot)) {
                ev.targetGuid  = ReadObjGuid(tObj);
                ev.targetTx    = *reinterpret_cast<const int32_t*>(tObj + addr::obj_off::kTileX);
                ev.targetTy    = *reinterpret_cast<const int32_t*>(tObj + addr::obj_off::kTileY);
                ev.targetLevel = *reinterpret_cast<const int16_t*>(tObj + addr::obj_off::kLevel);
                if (RealIsSim(ev.targetSlot)) { // the target is another Sim: it is identified by NeighborID, not slot
                    ev.targetNeighborId = *reinterpret_cast<const int16_t*>(tObj + kPersonSubobjectOffset + addr::person_off::kNeighborID);
                    ev.targetSlot = 0xFFFF;
                }
            }
            if (IsClient() && ev.targetSlot != 0xFFFF) { // the wire speaks Host slots
                uint32_t w = LocalToWire(ev.targetSlot);
                if (w != kNoSlot) ev.targetSlot = static_cast<uint16_t>(w);
            }
            SendSimInteraction(ev);
        }
    }
    return ok;
}

// ----------------------------------------------------------------------------
// Two-instance test driver. Active only when the environment variable TS2COOP_AUTO is "host" or "join":
// once a lot is running it hosts/joins on its own and exercises object/Sim sync, logging "[TEST]" lines
// that the test script compares between the two game instances.
// ----------------------------------------------------------------------------
void TestAutoTick() {
    static int mode = -1; // 0 = off, 1 = host, 2 = join
    if (mode < 0) {
        char b[16] = {};
        GetEnvironmentVariableA("TS2COOP_AUTO", b, sizeof(b));
        mode = (strcmp(b, "host") == 0) ? 1 : (strcmp(b, "join") == 0) ? 2 : 0;
        if (mode) ts2::se::PushConsoleLog("[TEST] modo automatico: %s", mode == 1 ? "host" : "guest");
    }
    if (!mode || !g_objBackend) return;
    const uint32_t n = g_objBackend->count();
    const DWORD now = GetTickCount();
    static DWORD tLot = 0, tConn = 0, tLog = 0;
    static bool started = false;
    static uint32_t step = 0;
    static int32_t objA = -1, objB = -1;
    static ObjPose poseA = {};
    if (n == 0) { tLot = 0; return; }
    if (!tLot) tLot = now;
    if (!started) {
        const DWORD wait = (mode == 1) ? 10000 : 20000;
        if (now - tLot < wait) return;
        started = true;
        if (mode == 1) StartHost(27025); else JoinHost("127.0.0.1", 27025);
        return;
    }
    if (!g_stats.peerConnected) { tConn = 0; return; }
    if (!tConn) { tConn = now; ts2::se::PushConsoleLog("[TEST] conectado"); }
    const DWORD dt = now - tConn;

    if (now - tLog >= 4000) {
        tLog = now;
        uint32_t crc = 0, tracked = 0, sims = 0;
        for (uint32_t id = 0; id < n && id < kMaxObj; ++id) {
            ObjPose p;
            if (IsSimId(id)) {
                ++sims;
                if (g_objBackend->read(id, &p))
                    ts2::se::PushConsoleLog("[TEST] sim slot=%u pos=(%d,%d,%d) rot=%u", id, p.tx, p.ty, (int)p.level, (unsigned)p.rot);
                continue;
            }
            if (!g_objBackend->read(id, &p)) continue;
            ++tracked;
            uint32_t v[5] = {(uint32_t)p.tx, (uint32_t)p.ty, (uint32_t)p.level, p.rot, p.guid}; // slot independent
            uint32_t hh = 2166136261u;
            for (int k = 0; k < 5; ++k) { hh ^= v[k]; hh *= 16777619u; }
            crc += hh;
        }
        ts2::se::PushConsoleLog("[TEST] estado n=%u moveis=%u sims=%u hash=%08X mySim=%d peerSim=%d aplic=%u criados=%u removidos=%u conflitos=%u",
                                n, tracked, sims, crc, g_stats.mySimIdx, g_stats.peerSimIdx, g_stats.objApplied, g_stats.objCreated,
                                g_stats.objDeleted, g_stats.objConflicts);
    }

    // choose two movable props once
    if (objA < 0) {
        int32_t found = 0;
        for (uint32_t id = 0; id < n && id < kMaxObj && found < 2; ++id) {
            ObjPose p;
            if (IsSimId(id) || !g_objBackend->read(id, &p) || p.guid == 0) continue;
            if (found == 0) { objA = static_cast<int32_t>(id); poseA = p; } else objB = static_cast<int32_t>(id);
            ++found;
        }
        return;
    }
    ObjPose p;
    auto sync = [&](uint32_t s, DWORD at) { if (step == s && dt > at) { step = s + 1; return true; } return false; };
    if (mode == 1 && sync(0, 12000) && g_objBackend->read(objA, &p)) {
        ObjPose q = p; q.tx += 1;
        bool ok = g_objBackend->apply(objA, q);
        ts2::se::PushConsoleLog("[TEST] host moveu obj %d (%d,%d)->(%d,%d) ok=%d", objA, p.tx, p.ty, q.tx, q.ty, ok ? 1 : 0);
    }
    if (mode == 2 && sync(1, 18000) && objB >= 0 && g_objBackend->read(objB, &p)) {
        ObjPose q = p; q.ty += 1;
        bool ok = g_objBackend->apply(objB, q);
        ts2::se::PushConsoleLog("[TEST] guest moveu obj %d (%d,%d)->(%d,%d) ok=%d", objB, p.tx, p.ty, q.tx, q.ty, ok ? 1 : 0);
    }
    if (sync(2, 25000)) {
        int32_t my = g_objBackend->selectedSim ? g_objBackend->selectedSim() : -1;
        if (my >= 0 && g_objBackend->read(my, &p)) {
            ObjPose q = p; q.tx += 3;
            bool ok = g_objBackend->apply(my, q);
            ts2::se::PushConsoleLog("[TEST] %s teleportou o proprio Sim %d (%d,%d)->(%d,%d) ok=%d", mode == 1 ? "host" : "guest", my, p.tx, p.ty, q.tx, q.ty, ok ? 1 : 0);
        } else {
            ts2::se::PushConsoleLog("[TEST] %s sem Sim selecionado (idx=%d)", mode == 1 ? "host" : "guest", my);
        }
    }
    if (mode == 1 && sync(3, 35000) && g_objBackend->create) {
        ObjPose q = poseA; q.tx += 2;
        uint32_t slot = 0;
        bool ok = g_objBackend->create(&slot, q);
        ts2::se::PushConsoleLog("[TEST] host criou objeto guid=%08X em (%d,%d) ok=%d", q.guid, q.tx, q.ty, ok ? 1 : 0);
    }
}

} // namespace

void SendSimInteraction(const SimInteractionEvent& ev) {
    if (!g_stats.peerConnected || !g_stats.simSyncEnabled) return;
    if (QueueFrame(Msg::SimInteraction, &ev, sizeof(ev))) {
        ++g_stats.interactionsSynced;
        g_stats.lastSyncedInteractionIdx = ev.interactionIndex;
        g_stats.lastSyncedTargetSlot     = ev.targetSlot;
    }
}

void ObjectSyncTick() {
    if (g_objBackend) TestAutoTick();
    if (!g_objBackend || g_stats.role == Role::Disconnected) return;
    // Never touch the object table while a lot is loading / unloading or in the neighborhood view
    if (!LocalLotReady()) return;
    if (g_stats.peerConnected) ObjApplyInbox();
    DWORD now = GetTickCount();
    if (now >= g_nextSimScan) { g_nextSimScan = now + 100; ObjScanSims(); DumpLotPersons(); }
    if (now < g_objNextScan) return;
    g_objNextScan = now + kObjScanEveryMs;
    ObjScan();
    if (g_objSnapPending) { g_objSnapPending = false; ObjSendSnapshot(); }
}

void SetObjectBackendForTest(const ObjectBackend* backend) {
    g_objBackend = backend ? backend : &kRealBackend;
    ObjResetLot();
    g_objLotToken = 0;
}

void SetObjectSyncEnabled(bool enabled) {
    g_stats.objSyncEnabled = enabled;
    ts2::se::PushConsoleLog("[Co-op] Sincronia de moveis %s.", enabled ? "ligada" : "desligada");
    if (enabled) ObjResetLot(); // re-baseline so we do not send stale differences
}

void SetBlockPeerAutonomy(bool enabled) {
    g_stats.blockPeerAutonomy = enabled;
    ts2::se::PushConsoleLog("[Co-op] IA do convidado: %s.", enabled ? "silenciada (segue o Host)" : "livre");
}

void SetSimSyncEnabled(bool enabled) {
    g_stats.simSyncEnabled = enabled;
    ts2::se::PushConsoleLog("[Co-op] Sincronia de Sims: %s.", enabled ? "ligada" : "desligada");
    ObjResetLot();
}

void RequestObjectSnapshot() {
    if (!g_stats.peerConnected) return;
    if (g_stats.role == Role::Client) {
        QueueFrame(Msg::ObjSnapReq, nullptr, 0);
        ts2::se::PushConsoleLog("[Co-op] Pedindo snapshot de moveis ao Host.");
    } else {
        g_objSnapPending = true; // Host: push the current poses to the Guest
    }
}

// ============================================================================
// Public API
// ============================================================================
bool Initialize() {
    WSADATA wsa = {};
    if (WSAStartup(0x0202, &wsa) == 0) g_wsaInitialized = true;

    bool okTick = ts2::hook::InstallDetour(
        g_detourSimulateTick,
        addr::Rebase(addr::kFunc_cTSSimulator_SimulateTick),
        reinterpret_cast<void*>(&Hooked_cTSSimulator_SimulateTick),
        9
    );
    bool okIdle = ts2::hook::InstallDetour(
        g_detourOnIdle,
        addr::Rebase(addr::kFunc_cTSSimulator_OnIdle),
        reinterpret_cast<void*>(&Hooked_cTSSimulator_OnIdle),
        9
    );
    bool okInter = ts2::hook::InstallDetour(
        g_detourEnqueueInteraction,
        addr::Rebase(addr::kFunc_cTSPerson_EnqueueInteraction),
        reinterpret_cast<void*>(&Hooked_cTSPerson_EnqueueInteraction),
        5
    );
    ts2::se::PushConsoleLog("[Co-op v2] Pronto: TCP host-autoritativo (SimulateTick=%d, OnIdle=%d, EnqueueInter=%d)",
                            okTick ? 1 : 0, okIdle ? 1 : 0, okInter ? 1 : 0);
    return okTick;
}

void Shutdown() {
    Disconnect();
    ts2::hook::RemoveDetour(g_detourEnqueueInteraction);
    ts2::hook::RemoveDetour(g_detourOnIdle);
    ts2::hook::RemoveDetour(g_detourSimulateTick);
    if (g_wsaInitialized) {
        WSACleanup();
        g_wsaInitialized = false;
    }
}

bool StartHost(uint16_t port, const char*, uint16_t) {
    Disconnect();
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) {
        ts2::se::PushConsoleLog("[Co-op] ERRO: socket() falhou (WSA=%d)", WSAGetLastError());
        return false;
    }
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    ULONG nb = 1;
    ioctlsocket(g_listen, FIONBIO, &nb);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = 0;
    if (bind(g_listen, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR ||
        listen(g_listen, 1) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        ts2::se::PushConsoleLog("[Co-op] ERRO: nao consegui escutar na porta TCP %u (WSA=%d)", port, err);
        SetConnectMessage("Erro ao abrir porta %u (WSA=%d)", port, err);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    g_stats.role = Role::Host;
    g_stats.localPort = port;
    g_stats.peerConnected = false;
    SetConnectMessage("Aguardando convidado na porta TCP %u...", port);
    ts2::se::PushConsoleLog("[Co-op] Host escutando na porta TCP %u", port);
    return true;
}

bool JoinHost(const char* hostIp, uint16_t hostPort, uint16_t) {
    Disconnect();
    strncpy(g_stats.peerIp, (hostIp && *hostIp) ? hostIp : "127.0.0.1", sizeof(g_stats.peerIp) - 1);
    g_stats.peerIp[sizeof(g_stats.peerIp) - 1] = 0;
    if (inet_addr(g_stats.peerIp) == 0xFFFFFFFFu) {
        SetConnectMessage("IP invalido: %s", g_stats.peerIp);
        return false;
    }
    g_stats.peerPort = hostPort;
    g_stats.role = Role::Client;
    g_stats.peerConnected = false;
    g_connecting = false;
    g_nextConnectTick = 0;
    SetConnectMessage("Conectando a %s:%u...", g_stats.peerIp, hostPort);
    ts2::se::PushConsoleLog("[Co-op] Convidado conectando a %s:%u", g_stats.peerIp, hostPort);
    return true;
}

void Disconnect() {
    if (g_conn != INVALID_SOCKET) { closesocket(g_conn); g_conn = INVALID_SOCKET; }
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    ResetLinkState();
    g_connecting = false;
    bool wasActive = (g_stats.role != Role::Disconnected);
    g_stats.role = Role::Disconnected;
    g_stats.peerConnected = false;
    g_stats.peerLoopback = false;
    g_stats.xferState = XferState::Idle;
    g_stats.xferMessage[0] = 0;
    if (wasActive) {
        SetConnectMessage("Desconectado.");
        ts2::se::PushConsoleLog("[Co-op] Sessao encerrada.");
    }
}

void PollNetwork() {
    if (g_inPoll || g_stats.role == Role::Disconnected) return;
    g_inPoll = true;

    if (g_stats.role == Role::Host && g_conn == INVALID_SOCKET) HostAccept();
    if (g_stats.role == Role::Client && (g_conn == INVALID_SOCKET || g_connecting)) GuestTryConnect();

    if (g_stats.peerConnected && g_conn != INVALID_SOCKET) {
        PumpRx();

        if (g_conn != INVALID_SOCKET && g_stats.role == Role::Host) {
            DWORD now = GetTickCount();
            if (g_sendPending && now >= g_sendAtTick) StartSendNow();
            PumpSend();
            if (now - g_lastStateTx >= kStateEveryMs) {
                g_lastStateTx = now;
                SendStateNow();
            }
        }
        FlushTx();
    }

    g_inPoll = false;
}

bool SendNeighborhoodToGuest(uint32_t delayMs) {
    if (g_stats.role != Role::Host || !g_stats.peerConnected) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Hospede e aguarde o convidado conectar primeiro.");
        return false;
    }
    if (g_stats.peerLoopback) {
        g_stats.xferState = XferState::Error;
        SetXferMessage("Convidado no mesmo PC usa a mesma pasta: nada a enviar.");
        return false;
    }
    if (g_xferSending) return false;
    g_sendPending = true;
    g_sendAtTick = GetTickCount() + delayMs;
    g_stats.xferState = XferState::WaitSave;
    SetXferMessage("Salvando o jogo... enviando em %u s", delayMs / 1000);
    return true;
}

void SendBhavEvent(uint16_t eventId, uint16_t stackObjId, int16_t arg0, int16_t arg1) {
    if (!g_stats.peerConnected) return;
    BhavPayload p = {eventId, stackObjId, arg0, arg1};
    QueueFrame(Msg::Bhav, &p, sizeof(p));
    ++g_stats.bhavEventsSynced;
}

void SendSpeedPauseCommand(int16_t speedMult, bool paused) {
    ts2::se::SetSimulatorSpeed(speedMult);
    ts2::se::SetSimulatorPaused(paused);
    if (!g_stats.peerConnected) return;
    SpeedPausePayload p = {speedMult, static_cast<int16_t>(paused ? 1 : 0)};
    QueueFrame(Msg::SpeedPause, &p, sizeof(p));
}

void ForceResyncNow() {
    if (!g_stats.peerConnected) return;
    if (g_stats.role == Role::Host) {
        SendStateNow();
        g_objSnapPending = true; // and re-mirror the whole lot onto the Guest
        ts2::se::PushConsoleLog("[Co-op] Estado enviado ao convidado.");
    } else {
        QueueFrame(Msg::ResyncReq, nullptr, 0);
        QueueFrame(Msg::ObjSnapReq, nullptr, 0); // and mirror the Host's lot again
        ts2::se::PushConsoleLog("[Co-op] Resync solicitado ao Host.");
    }
}

const CoopStats& GetStats() { return g_stats; }

void SetStrictLockstep(bool enabled) { g_stats.lockstepStrict = enabled; }

} // namespace ts2::coop
