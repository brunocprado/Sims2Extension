#pragma once
#include "ts2/win32_abi.hpp"
#include <stdint.h>

namespace ts2::coop {

// ============================================================================
// TS2 Co-op v2: HOST-AUTHORITATIVE over reliable TCP
//
//  * Host listens on a TCP port; the Guest connects. Messages are length-framed,
//    so nothing is lost or reordered (the old UDP lockstep could silently desync).
//  * The Host is the single source of truth. The Guest never needs the same save:
//    the Host streams its whole neighborhood folder (NXXX) to the Guest, which is
//    installed in the Guest's Neighborhoods directory (the Guest's previous folder
//    with the same name is moved to a CoopBackup directory first, never deleted).
//  * While connected, the Host continuously replicates clock, speed/pause, PRNG
//    seed and family funds to the Guest.
// ============================================================================

enum class Role : uint8_t {
    Disconnected = 0,
    Host         = 1,
    Client       = 2
};

enum class XferState : uint8_t {
    Idle      = 0,
    WaitSave  = 1, // Host: waiting for the game to finish saving before scanning
    Sending   = 2,
    Receiving = 3,
    Done      = 4,
    Error     = 5
};

struct CoopStats {
    Role      role                = Role::Disconnected;
    bool      peerConnected       = false;
    bool      lockstepStrict      = false; // kept for UI compatibility (unused in v2)
    bool      peerLoopback        = false; // true when both instances run on the same PC
    char      peerIp[32]          = "127.0.0.1";
    uint16_t  localPort           = 27025;
    uint16_t  peerPort            = 27025;
    uint32_t  packetsSent         = 0;     // frames
    uint32_t  packetsReceived     = 0;     // frames
    uint32_t  ticksSynced         = 0;     // state frames applied/sent
    uint32_t  interactionsSynced  = 0;
    uint32_t  bhavEventsSynced    = 0;
    uint32_t  desyncCount         = 0;
    uint32_t  lastLocalCrc        = 0;
    uint32_t  lastPeerCrc         = 0;
    uint32_t  lastPeerTick        = 0;
    uint32_t  lastRttMs           = 0;
    int32_t   hostFunds           = 0;     // last funds value received from the Host

    XferState xferState           = XferState::Idle;
    uint32_t  xferBytesDone       = 0;
    uint32_t  xferBytesTotal      = 0;
    uint32_t  xferFilesDone       = 0;
    uint32_t  xferFilesTotal      = 0;
    char      xferHood[8]         = {};
    char      xferMessage[192]    = {};
    char      connectMessage[96]  = {};

    // Object (furniture) placement & Buy/Build creation/deletion sync
    bool      objSyncEnabled      = true;
    bool      simSyncEnabled      = true;  // Sim pose, motives & interaction sync
    bool      blockPeerAutonomy   = true;  // do not let the local AI pick actions for Sims the partner drives
    uint32_t  simsSent            = 0;
    uint32_t  simsApplied         = 0;
    uint32_t  motivesSynced       = 0;
    int32_t   mySimIdx            = -1;    // object slot of the Sim this player controls (-1 = not found)
    int16_t   mySimNeighborId     = 0;     // persistent NeighborID (PersonData[0x1F]) of controlled Sim
    uint32_t  peerSimsSeen        = 0;     // Sim pose packets received from the peer
    int32_t   peerSimIdx          = -1;    // slot of the peer's Sim (last packet)
    int16_t   peerSimNeighborId   = 0;     // persistent NeighborID of the peer's Sim
    int32_t   lastSyncedInteractionIdx = -1;
    uint16_t  lastSyncedTargetSlot     = 0;
    bool      objSyncAvailable    = false; // false when the object folder could not be validated
    uint32_t  objTracked          = 0;     // objects being watched on the current lot
    uint32_t  objSent             = 0;     // pose/create/delete changes sent to the peer
    uint32_t  objApplied          = 0;     // pose changes applied locally
    uint32_t  objCreated          = 0;     // new Buy/Build objects spawned from peer
    uint32_t  objDeleted          = 0;     // deleted/sold Buy/Build objects removed from peer
    uint32_t  objConflicts        = 0;     // remote changes ignored (object differs on this side)
};

// ---- Object & Sim sync backend (the real one talks to the game; the Wine test harness injects a fake) ----
struct ObjPose {
    int32_t  tx    = 0;
    int32_t  ty    = 0;
    int16_t  level = 0;
    uint16_t rot   = 0; // 0..7
    uint32_t guid  = 0; // 32-bit catalog Selector GUID (0 = unknown/legacy)
};

struct SimInteractionEvent {
    uint16_t actorSlot        = 0;
    int16_t  actorNeighborId  = 0;
    uint16_t targetSlot       = 0;
    uint32_t targetGuid       = 0;
    int32_t  targetTx         = 0;
    int32_t  targetTy         = 0;
    int16_t  targetLevel      = 1;
    int32_t  interactionIndex = 0;
    int32_t  priority         = 50;
    int16_t  actionTreeId     = 0;
    int16_t  params[4]        = {};
    uint32_t flags            = 0;
    int16_t  targetNeighborId = 0;   // >0: the target is a Sim (targetSlot is then 0xFFFF)
};

struct ObjectBackend {
    uint32_t (*count)()                                                           = nullptr; // number of object slots (0 when not inside a lot)
    bool     (*read)(uint32_t id, ObjPose* out)                                   = nullptr; // false: empty slot or not a syncable object
    bool     (*apply)(uint32_t id, const ObjPose& pose)                           = nullptr;
    uint32_t (*lotToken)()                                                        = nullptr; // changes whenever a different lot gets loaded
    bool     (*isSim)(uint32_t id)                                                = nullptr; // optional: slot holds a Sim (not furniture)
    int32_t  (*selectedSim)()                                                     = nullptr; // optional: slot of the Sim this player controls, or -1
    bool     (*create)(uint32_t* ioId, const ObjPose& pose)                       = nullptr; // optional: spawn new catalog object with pose.guid; *ioId = hint in, real slot out
    bool     (*destroy)(uint32_t id, const ObjPose& oldPose)                      = nullptr; // optional: delete object at id / oldPose
    bool     (*readSimState)(uint32_t id, int16_t* outNeighborId, int8_t outMotives[16])       = nullptr;
    bool     (*applySimState)(uint32_t id, int16_t neighborId, const int8_t motives[16])       = nullptr;
    bool     (*applyInteraction)(const SimInteractionEvent& ev)                   = nullptr; // optional: enqueue interaction on Sim
    int32_t  (*resolveSim)(uint32_t slot, int16_t neighborId)                     = nullptr; // optional: local slot of the Sim with this NeighborID (-1 = none); slot is a hint
    int16_t  (*simNeighborId)(uint32_t slot)                                      = nullptr; // optional: NeighborID of the Sim at slot (0 = unknown)
};

// Sim sync (both directions): every player streams the pose, motives and interactions of the Sim they control;
// the other side moves/drives its copy of that Sim to match. A side never overwrites the Sim it is controlling.
void SetSimSyncEnabled(bool enabled);
// While on, the local AI never picks actions for Sims the partner drives; they only follow the replicated actions.
void SetBlockPeerAutonomy(bool enabled);
void SetObjectBackendForTest(const ObjectBackend* backend);
void SendSimInteraction(const SimInteractionEvent& ev);

// Runs the object-sync logic once. Called from the cTSSimulator::OnIdle hook (game thread).
void ObjectSyncTick();

// Enables/disables furniture sync. Guest: asks the Host for a full snapshot of object positions.
void SetObjectSyncEnabled(bool enabled);
void RequestObjectSnapshot();

// Installs the SimulateTick hook (used only to pump the network from the game loop)
bool Initialize();
void Shutdown();

// Host: listens on `port` (extra args kept for source compatibility, ignored).
bool StartHost(uint16_t port = 27025, const char* ignoredIp = nullptr, uint16_t ignoredPort = 0);

// Guest: connects to the Host at hostIp:hostPort (3rd arg ignored).
bool JoinHost(const char* hostIp = "127.0.0.1", uint16_t hostPort = 27025, uint16_t ignoredLocalPort = 0);

void Disconnect();

// Pumps socket I/O, transfer state machine and state replication (call every frame).
void PollNetwork();

// Host: after `delayMs` (gives the game time to save) streams the active neighborhood to the Guest.
bool SendNeighborhoodToGuest(uint32_t delayMs = 6000);

// Replicates a BHAV register/event to the peer (primitives 0x0081/0x0082).
void SendBhavEvent(uint16_t eventId, uint16_t stackObjId, int16_t arg0, int16_t arg1);

// Applies speed/pause locally and replicates it to the peer.
void SendSpeedPauseCommand(int16_t speedMult, bool paused);

// Host: pushes the full state right now. Guest: asks the Host to push it.
void ForceResyncNow();

const CoopStats& GetStats();
void SetStrictLockstep(bool enabled);

} // namespace ts2::coop
