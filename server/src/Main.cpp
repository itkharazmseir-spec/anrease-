// AndreasOnline dedicated server - step 2 skeleton.
//
// What this does right now:
//   - listens for ENet connections on a UDP port (default 7777)
//   - requires a Handshake with a matching protocol version before treating
//     a connection as a real player
//   - tracks connected players (id, name, last known PlayerState)
//   - rebroadcasts PlayerState and ChatMessage from one client to everyone
//     (including back to the sender - the client is expected to ignore a
//     PlayerState/ChatMessage whose playerId is its own)
//   - announces PlayerJoined/PlayerLeft to everyone, and replays the current
//     roster (as a PlayerJoined per existing player) to a client that just
//     finished its handshake, so it learns who is already online
//
// What this deliberately does NOT do yet (future steps):
//   - no world simulation, no vehicle ownership/authority, no anti-cheat
//     validation of incoming positions
//   - no graceful shutdown (Ctrl+C just kills the process)
#include <enet/enet.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>

#include "ao/Protocol.h"
#include "ao/Version.h"

namespace {

void LogLine(const char* fmt, ...) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    std::va_list args;
    va_start(args, fmt);
    char msg[512];
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::printf("[%s] %s\n", ts, msg);
    std::fflush(stdout);
}

std::string ClampedName(const char* raw, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && raw[len] != '\0') {
        ++len;
    }
    return std::string(raw, len);
}

struct ConnectedPlayer {
    uint32_t              id = 0;
    std::string           name;
    ENetPeer*             peer = nullptr;
    bool                  handshakeDone = false;
    ao::proto::PlayerState lastState{};
};

constexpr enet_uint8 kChannelReliable   = 0;
constexpr enet_uint8 kChannelUnreliable = 1;
constexpr enet_uint8 kChannelCount      = 2;

uint32_t g_nextPlayerId = 1;
std::unordered_map<uint32_t, ConnectedPlayer> g_players; // key: playerId

uint32_t PlayerIdFromPeer(ENetPeer* peer) {
    // We stash the assigned id directly in ENetPeer::data once the
    // handshake completes (0 means "not handshaken yet").
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(peer->data));
}

void SendTo(ENetPeer* peer, enet_uint8 channel, const void* data, size_t len, bool reliable) {
    ENetPacket* packet = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
    enet_peer_send(peer, channel, packet);
}

void BroadcastToAll(ENetHost* host, enet_uint8 channel, const void* data, size_t len, bool reliable) {
    ENetPacket* packet = enet_packet_create(
        data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
    enet_host_broadcast(host, channel, packet);
}

template <typename T>
void SendMessage(ENetPeer* peer, ao::proto::MessageType type, const T& payload, bool reliable) {
    uint8_t buf[1 + sizeof(T)];
    buf[0] = static_cast<uint8_t>(type);
    std::memcpy(buf + 1, &payload, sizeof(T));
    SendTo(peer, reliable ? kChannelReliable : kChannelUnreliable, buf, sizeof(buf), reliable);
}

template <typename T>
void BroadcastMessage(ENetHost* host, ao::proto::MessageType type, const T& payload, bool reliable) {
    uint8_t buf[1 + sizeof(T)];
    buf[0] = static_cast<uint8_t>(type);
    std::memcpy(buf + 1, &payload, sizeof(T));
    BroadcastToAll(host, reliable ? kChannelReliable : kChannelUnreliable, buf, sizeof(buf), reliable);
}

void HandleHandshake(ENetHost* host, ENetPeer* peer, const uint8_t* data, size_t len) {
    if (len != sizeof(ao::proto::Handshake)) {
        LogLine("rejecting peer: malformed Handshake (size %zu, expected %zu)",
                 len, sizeof(ao::proto::Handshake));
        enet_peer_disconnect_now(peer, 0);
        return;
    }
    ao::proto::Handshake hs{};
    std::memcpy(&hs, data, sizeof(hs));

    if (hs.protocolVersion != ao::kProtocolVersion) {
        LogLine("rejecting peer: protocol mismatch (got %u, expected %u)",
                 hs.protocolVersion, ao::kProtocolVersion);
        ao::proto::HandshakeAck ack{};
        ack.result = ao::proto::HandshakeResult::ProtocolMismatch;
        ack.assignedPlayerId = 0;
        SendMessage(peer, ao::proto::MessageType::HandshakeAck, ack, /*reliable=*/true);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    const uint32_t id = g_nextPlayerId++;
    ConnectedPlayer& player = g_players[id];
    player.id = id;
    player.name = ClampedName(hs.playerName, ao::proto::kMaxNameLen);
    if (player.name.empty()) {
        player.name = "Player" + std::to_string(id);
    }
    player.peer = peer;
    player.handshakeDone = true;
    peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));

    LogLine("player %u ('%s') connected", id, player.name.c_str());

    ao::proto::HandshakeAck ack{};
    ack.result = ao::proto::HandshakeResult::Accepted;
    ack.assignedPlayerId = id;
    SendMessage(peer, ao::proto::MessageType::HandshakeAck, ack, /*reliable=*/true);

    // Tell the new player about everyone already online...
    for (const auto& [otherId, other] : g_players) {
        if (otherId == id || !other.handshakeDone) continue;
        ao::proto::PlayerJoined joined{};
        joined.playerId = otherId;
        std::strncpy(joined.playerName, other.name.c_str(), ao::proto::kMaxNameLen - 1);
        SendMessage(peer, ao::proto::MessageType::PlayerJoined, joined, /*reliable=*/true);
    }

    // ...and tell everyone (including the new player) that they joined.
    ao::proto::PlayerJoined joined{};
    joined.playerId = id;
    std::strncpy(joined.playerName, player.name.c_str(), ao::proto::kMaxNameLen - 1);
    BroadcastMessage(host, ao::proto::MessageType::PlayerJoined, joined, /*reliable=*/true);
}

void HandlePlayerState(ENetHost* host, ENetPeer* peer, const uint8_t* data, size_t len) {
    const uint32_t id = PlayerIdFromPeer(peer);
    if (id == 0) return; // no handshake yet, ignore
    if (len != sizeof(ao::proto::PlayerState)) return;

    auto it = g_players.find(id);
    if (it == g_players.end()) return;

    ao::proto::PlayerState state{};
    std::memcpy(&state, data, sizeof(state));
    state.playerId = id; // never trust the sender's own id field

    // Drop a reordered/stale update (tickMs is the sender's own monotonic
    // clock, so this only protects against this one peer's packets
    // overtaking each other, not clock differences between peers).
    if (state.tickMs < it->second.lastState.tickMs &&
        (it->second.lastState.tickMs - state.tickMs) < 0x80000000u) {
        return;
    }
    it->second.lastState = state;

    BroadcastMessage(host, ao::proto::MessageType::PlayerState, state, /*reliable=*/false);
}

void HandleChatMessage(ENetHost* host, ENetPeer* peer, const uint8_t* data, size_t len) {
    const uint32_t id = PlayerIdFromPeer(peer);
    if (id == 0) return;
    if (len != sizeof(ao::proto::ChatMessage)) return;

    ao::proto::ChatMessage msg{};
    std::memcpy(&msg, data, sizeof(msg));
    msg.playerId = id;

    auto it = g_players.find(id);
    const std::string text = ClampedName(msg.text, ao::proto::kMaxChatLen);
    LogLine("chat: %s: %s", it != g_players.end() ? it->second.name.c_str() : "?", text.c_str());

    BroadcastMessage(host, ao::proto::MessageType::ChatMessage, msg, /*reliable=*/true);
}

void HandleDisconnect(ENetHost* host, ENetPeer* peer) {
    const uint32_t id = PlayerIdFromPeer(peer);
    if (id == 0) return; // disconnected before finishing handshake

    auto it = g_players.find(id);
    const std::string name = (it != g_players.end()) ? it->second.name : "?";
    g_players.erase(id);
    peer->data = nullptr;

    LogLine("player %u ('%s') disconnected", id, name.c_str());

    ao::proto::PlayerLeft left{};
    left.playerId = id;
    BroadcastMessage(host, ao::proto::MessageType::PlayerLeft, left, /*reliable=*/true);
}

void HandlePacket(ENetHost* host, ENetPeer* peer, const ENetPacket* packet) {
    if (packet->dataLength < 1) return;
    const auto type = static_cast<ao::proto::MessageType>(packet->data[0]);
    const uint8_t* body = packet->data + 1;
    const size_t bodyLen = packet->dataLength - 1;

    switch (type) {
        case ao::proto::MessageType::Handshake:
            HandleHandshake(host, peer, body, bodyLen);
            break;
        case ao::proto::MessageType::PlayerState:
            HandlePlayerState(host, peer, body, bodyLen);
            break;
        case ao::proto::MessageType::ChatMessage:
            HandleChatMessage(host, peer, body, bodyLen);
            break;
        default:
            // Unknown/unexpected message type for something the server
            // should receive (HandshakeAck/PlayerJoined/PlayerLeft are
            // server -> client only); ignore it.
            break;
    }
}

} // namespace

int main(int argc, char** argv) {
    uint16_t port = 7777;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
        if (port == 0) port = 7777;
    }

    if (enet_initialize() != 0) {
        LogLine("ERROR: enet_initialize failed");
        return 1;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    ENetHost* host = enet_host_create(&address, /*peerCount=*/32, kChannelCount, 0, 0);
    if (!host) {
        LogLine("ERROR: enet_host_create failed (port %u already in use?)", port);
        enet_deinitialize();
        return 1;
    }

    LogLine("%s %s listening on UDP port %u (protocol %u)",
             ao::kProductName, ao::kVersionString, port, ao::kProtocolVersion);

    ENetEvent event;
    for (;;) {
        while (enet_host_service(host, &event, 1000) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    LogLine("peer connected from %x:%u (awaiting handshake)",
                             event.peer->address.host, event.peer->address.port);
                    event.peer->data = nullptr;
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                    HandlePacket(host, event.peer, event.packet);
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    HandleDisconnect(host, event.peer);
                    break;
                default:
                    break;
            }
        }
    }

    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
