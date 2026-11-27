// AndreasOnline dedicated server - step 2 skeleton.
//
// What this does right now:
//   - listens for ENet connections on a UDP port (default 7777)
//   - requires a Handshake with a matching protocol version before treating
//     a connection as a real player
//   - tracks connected players (id, name, last known PlayerState)
//   - rebroadcasts PlayerState, ChatMessage, and DamageReport (protocol v3,
//     2026-11-27 - see shared/include/ao/Protocol.h) from one client to
//     everyone (including back to the sender - the client is expected to
//     ignore one of these whose relevant id is its own, except a
//     DamageReport it should act on - see NetworkClient.h)
//   - announces PlayerJoined/PlayerLeft to everyone, and replays the current
//     roster (as a PlayerJoined per existing player) to a client that just
//     finished its handshake, so it learns who is already online
//   - enforces a max player cap (default 32, overridable via argv[2]) and
//     replies HandshakeResult::ServerFull when it's reached
//   - rate-limits chat (max 1 message per kMinChatIntervalMs per player) to
//     cut down on spam
//   - on Windows, handles Ctrl+C / console close gracefully: disconnects all
//     peers cleanly instead of just dying
//   - reads port/maxPlayers from server.cfg next to the executable (see
//     ServerConfig.h), overridable by command-line args
//   - strips control characters from player names and chat text before
//     storing/broadcasting them (SanitizeAscii/SanitizeCharBuffer)
//
// What this deliberately does NOT do yet (future steps):
//   - no world simulation, no vehicle ownership/authority, no anti-cheat
//     validation of incoming positions
#include <enet/enet.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
// enet/win32.h (included via enet/enet.h above, before this point) already
// pulled in <winsock2.h>, which guards itself with _WINSOCKAPI_ - windows.h
// checks that guard internally and skips its own (conflicting) <winsock.h>,
// so it's safe to include windows.h afterward as long as enet/enet.h stays
// above this block. Do NOT move this include above the enet one.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "ao/Protocol.h"
#include "ao/Version.h"
#include "ServerConfig.h"

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

// Replaces any non-printable byte (control characters, including newline/
// tab/escape) with '?' in place. A malicious or buggy client could otherwise
// send a player name or chat message containing e.g. embedded newlines or
// terminal escape sequences, which would get relayed verbatim to every other
// client (corrupting their log files/UI) and printed verbatim into this
// server's own console/log.
void SanitizeAscii(std::string& s) {
    for (char& c : s) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            c = '?';
        }
    }
}

// Same idea, but for a fixed-size char buffer that isn't guaranteed to be
// null-terminated (the wire format's char[] fields) - sanitizes up to the
// first '\0' or maxLen, whichever comes first, and mutates in place so the
// *actual bytes that get broadcast* are cleaned, not just a derived copy.
void SanitizeCharBuffer(char* buf, size_t maxLen) {
    for (size_t i = 0; i < maxLen && buf[i] != '\0'; ++i) {
        const auto uc = static_cast<unsigned char>(buf[i]);
        if (uc < 0x20 || uc == 0x7F) {
            buf[i] = '?';
        }
    }
}

struct ConnectedPlayer {
    uint32_t              id = 0;
    std::string           name;
    ENetPeer*             peer = nullptr;
    bool                  handshakeDone = false;
    ao::proto::PlayerState lastState{};
    uint32_t              lastChatMs = 0; // enet_time_get() at last accepted
                                           // ChatMessage, for anti-spam
    uint32_t              lastPosLogMs = 0; // enet_time_get() at last position
                                             // log line, for throttling (see
                                             // kPosLogIntervalMs)
};

constexpr enet_uint8 kChannelReliable   = 0;
constexpr enet_uint8 kChannelUnreliable = 1;
constexpr enet_uint8 kChannelCount      = 2;

// A player may send at most one ChatMessage per this many milliseconds;
// anything faster is silently dropped (not queued/delayed) rather than
// disconnecting the spammer.
constexpr uint32_t kMinChatIntervalMs = 400;

// Position log (2026-11-27): PlayerState arrives ~10 times/sec per player
// (see client/src/Main.cpp's kNetSendIntervalMs), so logging every one would
// flood the server console/log file. Throttled to once per this many
// milliseconds per player instead - frequent enough to find where someone
// actually is within a few seconds of asking, not so frequent it drowns out
// the join/leave/chat/damage lines this server already logs. This exists
// purely as an operator-facing convenience (e.g. to find real coordinates to
// hand to the standalone FakePlayer.exe test tool) - it changes no protocol
// behaviour and is not read back by any client.
constexpr uint32_t kPosLogIntervalMs = 5000;

uint32_t g_nextPlayerId = 1;
uint32_t g_maxPlayers   = 32; // overridable via argv[2]; also used as the
                               // ENetHost peerCount
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

    if (g_players.size() >= g_maxPlayers) {
        LogLine("rejecting peer: server full (%zu/%u)", g_players.size(), g_maxPlayers);
        ao::proto::HandshakeAck ack{};
        ack.result = ao::proto::HandshakeResult::ServerFull;
        ack.assignedPlayerId = 0;
        SendMessage(peer, ao::proto::MessageType::HandshakeAck, ack, /*reliable=*/true);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    const uint32_t id = g_nextPlayerId++;
    ConnectedPlayer& player = g_players[id];
    player.id = id;
    player.name = ClampedName(hs.playerName, ao::proto::kMaxNameLen);
    SanitizeAscii(player.name);
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

    const uint32_t now = enet_time_get();
    if (it->second.lastPosLogMs == 0 || (now - it->second.lastPosLogMs) >= kPosLogIntervalMs) {
        it->second.lastPosLogMs = now;
        LogLine("player %u ('%s') pos %.3f %.3f %.3f  heading %.3f rad  veh %d",
                id, it->second.name.c_str(), state.x, state.y, state.z,
                state.headingRad, state.vehicleModel);
    }

    BroadcastMessage(host, ao::proto::MessageType::PlayerState, state, /*reliable=*/false);
}

void HandleDamageReport(ENetHost* host, ENetPeer* peer, const uint8_t* data, size_t len) {
    const uint32_t id = PlayerIdFromPeer(peer);
    if (id == 0) return; // no handshake yet, ignore
    if (len != sizeof(ao::proto::DamageReport)) return;

    auto it = g_players.find(id);
    if (it == g_players.end()) return;

    ao::proto::DamageReport report{};
    std::memcpy(&report, data, sizeof(report));
    report.attackerPlayerId = id; // never trust the sender's own id field

    // The named victim must be a currently-connected player - otherwise this
    // is either stale (they already disconnected) or bogus; drop it rather
    // than broadcasting a report nobody can apply.
    if (g_players.find(report.victimPlayerId) == g_players.end()) return;

    // Clamp to GTA SA's own default health/armour range (see Protocol.h).
    // A buggy or malicious client could otherwise claim an arbitrarily large
    // delta; this bounds the worst case to "as if hit by the single biggest
    // legitimate hit" rather than an unverifiable instant kill from anywhere
    // on the map.
    if (report.healthDelta < 0.0f) report.healthDelta = 0.0f;
    if (report.healthDelta > 100.0f) report.healthDelta = 100.0f;
    if (report.armourDelta < 0.0f) report.armourDelta = 0.0f;
    if (report.armourDelta > 100.0f) report.armourDelta = 100.0f;

    LogLine("player %u dealt %.1f hp / %.1f armour damage to player %u",
            report.attackerPlayerId, report.healthDelta, report.armourDelta, report.victimPlayerId);

    BroadcastMessage(host, ao::proto::MessageType::DamageReport, report, /*reliable=*/true);
}

void HandleChatMessage(ENetHost* host, ENetPeer* peer, const uint8_t* data, size_t len) {
    const uint32_t id = PlayerIdFromPeer(peer);
    if (id == 0) return;
    if (len != sizeof(ao::proto::ChatMessage)) return;

    auto it = g_players.find(id);
    if (it == g_players.end()) return;

    const uint32_t now = enet_time_get();
    if (it->second.lastChatMs != 0 && (now - it->second.lastChatMs) < kMinChatIntervalMs) {
        // Spamming faster than the allowed rate - drop silently rather than
        // disconnecting; the sender just doesn't see their own message
        // broadcast back.
        return;
    }
    it->second.lastChatMs = now;

    ao::proto::ChatMessage msg{};
    std::memcpy(&msg, data, sizeof(msg));
    msg.playerId = id;
    SanitizeCharBuffer(msg.text, ao::proto::kMaxChatLen);

    const std::string text = ClampedName(msg.text, ao::proto::kMaxChatLen);
    LogLine("chat: %s: %s", it->second.name.c_str(), text.c_str());

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
        case ao::proto::MessageType::DamageReport:
            HandleDamageReport(host, peer, body, bodyLen);
            break;
        default:
            // Unknown/unexpected message type for something the server
            // should receive (HandshakeAck/PlayerJoined/PlayerLeft are
            // server -> client only); ignore it.
            break;
    }
}

#if defined(_WIN32)
// volatile: written from the console-control-handler thread, read from
// main()'s loop.
volatile bool g_shutdownRequested = false;

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_shutdownRequested = true;
            // Give main() a moment to notice the flag and shut down
            // gracefully (flush the disconnect broadcast, close the log)
            // before Windows kills the process for real on CTRL_CLOSE_EVENT.
            Sleep(1500);
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    // server.cfg (next to the executable) sets the base port/maxPlayers;
    // command-line args, if given, override whatever it set. Missing file ->
    // ServerConfig's own built-in defaults (port 7777, 32 players).
    const ao::ServerConfig fileCfg = ao::LoadServerConfig("server.cfg");
    uint16_t port = fileCfg.port;
    g_maxPlayers = fileCfg.maxPlayers;

    if (argc > 1) {
        const int argPort = std::atoi(argv[1]);
        if (argPort > 0 && argPort <= 65535) port = static_cast<uint16_t>(argPort);
    }
    if (argc > 2) {
        const int maxPlayers = std::atoi(argv[2]);
        if (maxPlayers > 0) {
            g_maxPlayers = static_cast<uint32_t>(maxPlayers);
        }
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

    if (enet_initialize() != 0) {
        LogLine("ERROR: enet_initialize failed");
        return 1;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    ENetHost* host = enet_host_create(&address, /*peerCount=*/g_maxPlayers, kChannelCount, 0, 0);
    if (!host) {
        LogLine("ERROR: enet_host_create failed (port %u already in use?)", port);
        enet_deinitialize();
        return 1;
    }

    LogLine("%s %s listening on UDP port %u (protocol %u, max players %u)",
             ao::kProductName, ao::kVersionString, port, ao::kProtocolVersion, g_maxPlayers);
    LogLine("usage: %s [port] [maxPlayers]  (or set port=/max_players= in server.cfg)", argv[0]);

    ENetEvent event;
#if defined(_WIN32)
    while (!g_shutdownRequested) {
#else
    for (;;) {
#endif
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

#if defined(_WIN32)
    LogLine("shutting down (console close/ctrl-c) - disconnecting %zu player(s)...", g_players.size());
    for (auto& [id, player] : g_players) {
        if (player.peer) enet_peer_disconnect_now(player.peer, 0);
    }
    enet_host_flush(host);
#endif

    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
