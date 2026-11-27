// Wire protocol shared by the AndreasOnline client and server.
//
// Transport: ENet, 2 channels.
//   Channel 0 (reliable, ordered)   - Handshake, HandshakeAck, PlayerJoined,
//                                     PlayerLeft, ChatMessage, DamageReport
//                                     (added protocol v3, 2026-11-27 - a
//                                     damage event must never be silently
//                                     dropped the way an occasional stale
//                                     PlayerState can be)
//   Channel 1 (unreliable/unsequenced) - PlayerState (high frequency, it's
//                                     fine to drop or reorder an old one;
//                                     'tickMs' lets a receiver discard a
//                                     PlayerState packet that arrived out of
//                                     order and is older than the last one it
//                                     already applied for that player)
//
// Every packet on the wire starts with a single MessageType byte, followed
// immediately by the raw bytes of the matching struct below (no padding,
// no alignment - see #pragma pack). Client and server are both built for
// little-endian x86/x64 (Windows client, Windows+Linux server), so no byte
// swapping is done. If a big-endian target is ever added, this must change.
#pragma once
#include <cstdint>

namespace ao::proto {

enum class MessageType : uint8_t {
    Handshake    = 1, // client -> server
    HandshakeAck = 2, // server -> client
    PlayerJoined = 3, // server -> client (broadcast; also sent once per
                       // already-connected player to a client right after
                       // it gets HandshakeAck, so it learns who's already
                       // online)
    PlayerLeft   = 4, // server -> client (broadcast)
    PlayerState  = 5, // client -> server, then server -> client (broadcast,
                       // playerId filled in by the server)
    ChatMessage  = 6, // client -> server, then server -> client (broadcast,
                       // playerId filled in by the server)
    DamageReport = 7, // client -> server, then server -> client (broadcast;
                       // added protocol v3, 2026-11-27 - see DamageReport
                       // struct below and docs/MEMORY.md for the full design)
};

constexpr uint32_t kMaxNameLen = 24; // includes room for a null terminator
constexpr uint32_t kMaxChatLen = 128;

#pragma pack(push, 1)

struct Handshake {
    uint16_t protocolVersion; // compared against ao::kProtocolVersion, which
                               // is also a uint16_t - keep these in sync
    char     playerName[kMaxNameLen]; // not guaranteed null-terminated by
                                       // the sender; a reader must clamp
                                       // before treating it as a C string
};

enum class HandshakeResult : uint8_t {
    Accepted         = 0,
    ProtocolMismatch = 1,
    ServerFull       = 2,
};

struct HandshakeAck {
    HandshakeResult result;
    uint32_t        assignedPlayerId; // only meaningful if result == Accepted
};

struct PlayerJoined {
    uint32_t playerId;
    char     playerName[kMaxNameLen];
};

struct PlayerLeft {
    uint32_t playerId;
};

struct PlayerState {
    uint32_t playerId;     // ignored/overwritten by the server when a
                            // client sends this; filled in with the sender's
                            // id before the server rebroadcasts it
    uint32_t tickMs;        // sender's local monotonic milliseconds
                            // (e.g. GetTickCount on Windows); used only to
                            // detect/drop a reordered, stale update
    float    x, y, z;
    float    headingRad;
    float    health;
    float    armour;
    int32_t  vehicleModel;  // -1 == on foot
    int32_t  pedModel;      // the sender's own CEntity::m_nModelIndex (added
                             // protocol v2). Only MODEL_PLAYER (0, "CJ") and
                             // MODEL_CSPLAY (1, the alternate player-select
                             // skin) are ever applied on the receiving end -
                             // see RemotePeds.cpp for why: every other ped
                             // model is only guaranteed loaded on the sender's
                             // own machine, not on a viewer's, and spawning an
                             // unloaded model risks a crash with no verified
                             // way yet to check streaming state for this
                             // build. Any other value here is informational
                             // only until that is solved.
};

struct ChatMessage {
    uint32_t playerId;             // same convention as PlayerState.playerId
    char     text[kMaxChatLen];    // not guaranteed null-terminated
};

// Item 5 ("weapon/damage sync", added protocol v3, 2026-11-27): deliberately
// NOT a "player X fired weapon Y at player Z" event - that would need a
// verified RGL address for CPed::GiveWeapon/CWeapon::Initialise to even know
// when/what a player fired, which this project does not have (see
// docs/RGL_COMPAT_NOTES.md's "Item 5" section for the full investigation and
// why - merged/un-splittable function blobs in this build, plus one
// coincidental false-positive address and an unresolvable CFG-style indirect
// thunk). Instead, this reports an *already-computed outcome*: the sender's
// own client noticed its local stand-in CPed for `victimPlayerId` (see
// RemotePeds.cpp) lost health/armour according to the game's OWN engine
// simulation (a real bullet, explosion, collision, etc. that the sender's
// own game already processed against that CPed) - no weapon-specific game
// code is called or needed to produce this message at all, only the
// already-verified, already-used CPed::m_fHealth/m_fArmour field reads.
struct DamageReport {
    uint32_t victimPlayerId;   // whose OWN real ped (on their own machine)
                                // this damage should be applied to
    uint32_t attackerPlayerId; // ignored/overwritten by the server with the
                                // sender's own id, same convention as
                                // PlayerState.playerId - informational only
                                // (e.g. a future kill-feed), not used to
                                // decide who the damage applies to
    float    healthDelta;      // amount to subtract from the victim's own
                                // CPed::m_fHealth; clamped to [0, 100] by the
                                // server before it is ever broadcast, so a
                                // buggy/malicious sender can't claim damage
                                // bigger than the biggest legitimate hit
    float    armourDelta;      // same idea, for CPed::m_fArmour
};

#pragma pack(pop)

} // namespace ao::proto
