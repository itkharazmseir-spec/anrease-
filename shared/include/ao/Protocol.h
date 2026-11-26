// Wire protocol shared by the AndreasOnline client and server.
//
// Transport: ENet, 2 channels.
//   Channel 0 (reliable, ordered)   - Handshake, HandshakeAck, PlayerJoined,
//                                     PlayerLeft, ChatMessage
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
};

struct ChatMessage {
    uint32_t playerId;             // same convention as PlayerState.playerId
    char     text[kMaxChatLen];    // not guaranteed null-terminated
};

#pragma pack(pop)

} // namespace ao::proto
