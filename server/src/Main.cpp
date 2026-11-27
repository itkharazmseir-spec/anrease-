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
//   - basic anti-cheat/anti-grief validation (2026-11-27), all in
//     HandlePlayerState/HandleDamageReport below:
//       * clamps self-reported health/armour into GTA SA's normal 0-100
//         range before relaying
//       * rejects a vehicleModel outside the known real vehicle-id range
//         (kMinVehicleModel/kMaxVehicleModel) - this is as much anti-grief
//         as anti-cheat, since every OTHER client feeds this value straight
//         into CreateCarForScript_RGL (see RemotePeds.cpp's 2026-11-27
//         deadlock writeup) - an invalid id could hang/crash everyone else,
//         not just the sender
//       * speed/teleport sanity check on PlayerState, using the SERVER's
//         own clock for elapsed time (never the client's self-reported
//         tickMs, which a cheat could fake) - a jump further than
//         kMaxOnFootSpeedMps/kMaxVehicleSpeedMps permits for the real
//         elapsed time is rejected (not relayed) and logged
//       * rate-limits DamageReport the same way as chat (kMinDamageIntervalMs)
//         so a modified client can't claim an impossibly fast kill rate by
//         flooding max-clamped hits
//     None of this is a complete anti-cheat system - see docs/MEMORY.md for
//     the honest list of what it does NOT cover (health/position are still
//     fundamentally self-reported by each client, just now bounds-checked;
//     there is no account/ban system, and repeated violations are only
//     logged, never auto-kicked, to avoid kicking a legitimate player
//     during a lag spike)
//
//   - persistent accounts (protocol v4, 2026-11-27 - see
//     shared/include/ao/Protocol.h and server/src/AccountStore.h): a
//     Handshake may optionally include accountUser + a client-side SHA-256
//     hash of the password (the plaintext password itself never crosses the
//     network - see shared/include/ao/Sha256.h). A new username is
//     auto-registered on first use; an existing one must match its stored
//     hash or the connection is rejected with HandshakeResult::AuthFailed.
//     Leaving accountUser empty keeps the connection fully anonymous
//     (unchanged from before v4) - this is what FakePlayer.exe and other
//     test tools still do. Honest limitations (see docs/MEMORY.md and
//     Sha256.h's own comment for the full writeup): no per-account salt yet,
//     storage is a flat text file (accounts.dat) not a real database, and
//     this is not yet wired to anything that spends real money - that would
//     need a website over HTTPS first, not this raw UDP handshake.
//   - store/purchase delivery (protocol v5, 2026-11-27 - see
//     shared/include/ao/Protocol.h's PurchaseDelivery struct): polls a
//     plain text file, purchases.txt (next to this executable, see
//     PollPurchaseQueue below), every kPurchasePollIntervalMs, and relays
//     each line to whichever CONNECTED player has the matching accountId.
//     This file is the entire "interface" a future website/store backend
//     needs to integrate with - it only ever has to append one line per
//     purchase, nothing about this server's own process. Honest
//     limitation: if the target player isn't connected right now, that
//     purchase is silently dropped (logged, not queued) - there is no
//     "deliver next time they log in" design yet; see docs/MEMORY.md.
//
// What this deliberately does NOT do yet (future steps):
//   - no world simulation, no vehicle ownership/authority
//   - no server-side authoritative health (a modified client that ignores
//     incoming DamageReport locally cannot currently be forced to take
//     damage - see docs/MEMORY.md)
//   - no ban list, no per-account salt, no real database (see AccountStore.h)
#include <enet/enet.h>
#include <cmath>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "AccountStore.h"

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
    uint32_t              lastStateRecvMs = 0; // enet_time_get() at last
                                                // ACCEPTED PlayerState - the
                                                // server's own clock, used
                                                // for the speed/teleport
                                                // sanity check (never the
                                                // client's self-reported
                                                // tickMs, which a cheat
                                                // could fake)
    uint32_t              speedViolationStreak = 0; // consecutive rejected
                                                      // PlayerState updates
                                                      // for the speed check,
                                                      // just for the log
                                                      // line's "repeated"
                                                      // escalation wording
    ao::proto::PlayerState lastRejectedState{}; // most recent update that
                                                 // failed the speed check,
                                                 // kept to detect "settled
                                                 // at the new spot" (see
                                                 // kResyncConvergeDist)
    bool                  hasLastRejectedState = false;
    uint32_t              lastDamageMs = 0; // enet_time_get() at last
                                             // accepted DamageReport where
                                             // this player was the attacker,
                                             // for rate-limiting
    uint32_t              accountId = 0; // 0 = anonymous connection (no
                                          // accountUser given at handshake);
                                          // otherwise the persistent id from
                                          // AccountStore - see HandleHandshake
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

// Anti-cheat (2026-11-27): speed/teleport sanity check for PlayerState.
// These are generous upper bounds on how far a player could plausibly move
// between two updates, in game units (meters, same scale GTA SA itself
// uses) per second - picked well above any legitimate movement (sprinting
// on foot is roughly 7-8 m/s; even fast cars in the base game top out well
// under 90 m/s) so a normal player, even through a lag spike, should never
// be falsely flagged - this is meant to catch a blatant teleport/speed-hack,
// not borderline cases.
constexpr float kMaxOnFootSpeedMps  = 20.0f;
constexpr float kMaxVehicleSpeedMps = 90.0f;
// If more real time than this has passed since the last accepted update,
// the gap itself explains any distance (lag spike, brief stall) - skip the
// speed check entirely for that one update rather than comparing across a
// gap wide enough that literally any position would be plausible.
constexpr uint32_t kSpeedCheckMaxGapMs = 5000;
// 2026-11-27, found via the first real playtest with the fixed purchase
// poll above: a LEGITIMATE instantaneous position jump happens routinely
// in GTA SA (e.g. walking through a door into an interior swaps the
// player onto a completely different, far-away set of world coordinates).
// The checks above have no way to tell that apart from an actual
// speed-hack, so the first update after a real interior transition always
// gets rejected - but a real speed-hack keeps reporting a NEW position
// every ~100ms (still moving away), while a one-off legitimate jump
// reports essentially the SAME new position on the next update or two
// (the player just stands in the new spot). If two consecutive rejected
// updates are within this distance of each other, that's "stopped, not
// still running" - treat it as a resync rather than making the player
// wait out the full kSpeedCheckMaxGapMs grace window (which, at 18:42 in
// a real test, took over 5 seconds and spammed ~50 "possible
// speed/teleport hack" log lines, including a false
// "consider investigating/banning this player" escalation, for a
// perfectly innocent building entry).
constexpr float kResyncConvergeDist = 3.0f;

// Anti-grief (2026-11-27): the only vehicleModel values this server will
// relay - the documented standard GTA SA vehicle id range (plugin-sdk
// eVehicleModel.h: MODEL_LANDSTAL=400 through the last ordinary vehicle
// slot, 611). Every vehicle a player can legitimately drive falls in this
// range; this exists purely to reject a garbage model id, which every
// OTHER client would otherwise feed straight into their own
// CreateCarForScript_RGL (see client/src/RemotePeds.cpp's 2026-11-27
// deadlock writeup - that call is not proven safe against an arbitrary
// invalid model, so a player sending one on purpose could hang or crash
// everyone else's game, not just their own).
constexpr int32_t kMinVehicleModel = 400;
constexpr int32_t kMaxVehicleModel = 611;

// Anti-cheat (2026-11-27): a player may be the ATTACKER in at most one
// accepted DamageReport per this many milliseconds - well above the fire
// rate of any real weapon in the base game, so this only catches a client
// sending reports far faster than any real weapon could, never a
// legitimate fast-firing weapon.
constexpr uint32_t kMinDamageIntervalMs = 80;

// Item 7 ("store/purchase delivery", 2026-11-27): how often PollPurchaseQueue
// re-reads purchases.txt - see that function and the top-of-file comment.
constexpr uint32_t kPurchasePollIntervalMs = 2000;

uint32_t g_nextPlayerId = 1;
uint32_t g_maxPlayers   = 32; // overridable via argv[2]; also used as the
                               // ENetHost peerCount
std::unordered_map<uint32_t, ConnectedPlayer> g_players; // key: playerId
ao::AccountStore g_accounts; // loaded once in main() from accounts.dat

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

    // Accounts (protocol v4): an empty accountUser means "anonymous" -
    // unchanged behaviour from before v4, and what FakePlayer.exe and other
    // test tools still send. A non-empty accountUser must either match an
    // existing account's stored password hash, or (first time this username
    // is ever seen) is auto-registered on the spot - there is deliberately no
    // separate "register" step yet, since this is still a single flat-file
    // store, not a real account-creation flow behind a website.
    uint32_t resolvedAccountId = 0;
    {
        const std::string accountUser =
            ClampedName(hs.accountUser, ao::proto::kMaxAccountUserLen);
        if (!accountUser.empty()) {
            std::array<uint8_t, 32> providedHash{};
            std::memcpy(providedHash.data(), hs.accountPasswordHash, ao::proto::kAccountHashLen);

            if (const ao::AccountRecord* existing = g_accounts.Find(accountUser)) {
                if (existing->passwordHash != providedHash) {
                    LogLine("rejecting peer: wrong password for account '%s'",
                             ao::AccountStore::Normalize(accountUser).c_str());
                    ao::proto::HandshakeAck ack{};
                    ack.result = ao::proto::HandshakeResult::AuthFailed;
                    ack.assignedPlayerId = 0;
                    ack.accountId = 0;
                    SendMessage(peer, ao::proto::MessageType::HandshakeAck, ack, /*reliable=*/true);
                    enet_peer_disconnect_later(peer, 0);
                    return;
                }
                resolvedAccountId = existing->accountId;
            } else {
                const ao::AccountRecord& created = g_accounts.Create(
                    accountUser, providedHash, static_cast<uint64_t>(std::time(nullptr)));
                resolvedAccountId = created.accountId;
                LogLine("account '%s' auto-registered as accountId %u",
                         created.username.c_str(), created.accountId);
            }
        }
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
    player.accountId = resolvedAccountId;
    peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));

    LogLine("player %u ('%s') connected%s", id, player.name.c_str(),
             resolvedAccountId != 0 ? " (account)" : " (anonymous)");

    ao::proto::HandshakeAck ack{};
    ack.result = ao::proto::HandshakeResult::Accepted;
    ack.assignedPlayerId = id;
    ack.accountId = resolvedAccountId;
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

    // Anti-cheat: clamp self-reported health/armour into GTA SA's normal
    // 0-100 range before anything else touches them - a malicious client
    // could otherwise broadcast e.g. health=99999, which every other
    // client would write straight into their local copy of this avatar.
    if (state.health < 0.0f) state.health = 0.0f;
    if (state.health > 100.0f) state.health = 100.0f;
    if (state.armour < 0.0f) state.armour = 0.0f;
    if (state.armour > 100.0f) state.armour = 100.0f;

    // Anti-grief: only relay a vehicleModel this server recognizes as a
    // real vehicle id (see kMinVehicleModel/kMaxVehicleModel above). -1
    // (on foot) always passes; anything else out of range is forced back
    // to on-foot rather than forwarded, since every OTHER client feeds
    // this value straight into CreateCarForScript_RGL.
    if (state.vehicleModel != -1 &&
        (state.vehicleModel < kMinVehicleModel || state.vehicleModel > kMaxVehicleModel)) {
        LogLine("player %u ('%s') sent out-of-range vehicleModel %d - treating as on-foot",
                id, it->second.name.c_str(), state.vehicleModel);
        state.vehicleModel = -1;
    }

    // Anti-cheat: speed/teleport sanity check, using the SERVER's own
    // clock for elapsed time - never the client's self-reported tickMs,
    // which a cheat could simply fake to make an impossible jump look slow.
    const uint32_t now = enet_time_get();
    if (it->second.lastStateRecvMs != 0) {
        const uint32_t dtMs = now - it->second.lastStateRecvMs;
        if (dtMs > 0 && dtMs <= kSpeedCheckMaxGapMs) {
            const float dx = state.x - it->second.lastState.x;
            const float dy = state.y - it->second.lastState.y;
            const float dz = state.z - it->second.lastState.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool wasOrIsInVehicle =
                state.vehicleModel != -1 || it->second.lastState.vehicleModel != -1;
            const float maxSpeed = wasOrIsInVehicle ? kMaxVehicleSpeedMps : kMaxOnFootSpeedMps;
            const float maxDist = maxSpeed * (static_cast<float>(dtMs) / 1000.0f);

            if (dist > maxDist) {
                // Before rejecting outright: did the PREVIOUS rejected
                // update land essentially at this same spot? A real
                // speed-hack keeps reporting a new, farther position every
                // ~100ms (it's still moving); a one-off legitimate jump
                // (e.g. walking into a building's interior) reports the
                // same new position again on the very next update, because
                // the player is just standing there now. That distinction
                // is what kResyncConvergeDist detects - treat a converged
                // pair as a resync instead of making the player wait out
                // the full kSpeedCheckMaxGapMs grace window.
                bool resync = false;
                if (it->second.hasLastRejectedState) {
                    const float rdx = state.x - it->second.lastRejectedState.x;
                    const float rdy = state.y - it->second.lastRejectedState.y;
                    const float rdz = state.z - it->second.lastRejectedState.z;
                    const float rdist = std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz);
                    resync = rdist <= kResyncConvergeDist;
                }
                it->second.lastRejectedState = state;
                it->second.hasLastRejectedState = true;

                if (!resync) {
                    ++it->second.speedViolationStreak;
                    LogLine("player %u ('%s') moved %.1f units in %u ms (max plausible %.1f) - "
                            "possible speed/teleport hack, update rejected%s",
                            id, it->second.name.c_str(), dist, dtMs, maxDist,
                            (it->second.speedViolationStreak >= 5)
                                ? " [REPEATED - consider investigating/banning this player]"
                                : "");
                    return; // not broadcast, lastState/lastStateRecvMs untouched
                }

                LogLine("player %u ('%s') settled at a new position %.1f units away "
                        "after %u rejected update(s) - treating as a legitimate "
                        "teleport (e.g. entering a building) and resyncing",
                        id, it->second.name.c_str(), dist, it->second.speedViolationStreak);
                // fall through: accept this update below like any other.
            }
            it->second.speedViolationStreak = 0;
            it->second.hasLastRejectedState = false;
        } else {
            // dtMs == 0, or the gap was wide enough (> kSpeedCheckMaxGapMs)
            // that the speed check was skipped entirely and this update is
            // accepted unconditionally - either way, any pending rejection
            // streak from before is now moot.
            it->second.speedViolationStreak = 0;
            it->second.hasLastRejectedState = false;
        }
    } else {
        it->second.speedViolationStreak = 0;
        it->second.hasLastRejectedState = false;
    }
    it->second.lastStateRecvMs = now;
    it->second.lastState = state;

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

    // Anti-cheat: rate-limit how often this player can be the ATTACKER in
    // an accepted DamageReport - same anti-spam style as chat above, just
    // dropped silently. Without this, a modified client could flood
    // max-clamped (100 hp) reports far faster than any real weapon fires,
    // claiming an impossibly fast kill rate even though each individual
    // report looks like a normal hit.
    const uint32_t nowForRateLimit = enet_time_get();
    if (it->second.lastDamageMs != 0 &&
        (nowForRateLimit - it->second.lastDamageMs) < kMinDamageIntervalMs) {
        return;
    }

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

    it->second.lastDamageMs = nowForRateLimit;

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

// Item 7 ("store/purchase delivery", 2026-11-27) - see the top-of-file
// comment and docs/MEMORY.md for the full design. Deliberately a drain
// queue, not a durable store: every poll reads the whole file, tries to
// deliver each line, then truncates it to empty regardless of whether a
// delivery actually found a connected recipient - a future website backend
// is expected to only append a line once it already knows (by some means
// of its own) that the target player is online.
void PollPurchaseQueue(ENetHost* host) {
    std::ifstream in("purchases.txt");
    if (!in.is_open()) return; // no file yet - perfectly normal, nothing to do

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    in.close();

    if (lines.empty()) return;

    for (const std::string& entry : lines) {
        // Format: accountId|itemKind(0=Vehicle,1=Weapon)|itemId|amount
        std::istringstream iss(entry);
        std::string accountIdStr, itemKindStr, itemIdStr, amountStr;
        if (!std::getline(iss, accountIdStr, '|') ||
            !std::getline(iss, itemKindStr, '|') ||
            !std::getline(iss, itemIdStr, '|') ||
            !std::getline(iss, amountStr, '|')) {
            LogLine("purchases.txt: skipping malformed line: %s", entry.c_str());
            continue;
        }

        uint32_t accountId = 0;
        int itemKindRaw = -1;
        int32_t itemId = 0;
        uint32_t amount = 0;
        try {
            accountId = static_cast<uint32_t>(std::stoul(accountIdStr));
            itemKindRaw = std::stoi(itemKindStr);
            itemId = static_cast<int32_t>(std::stol(itemIdStr));
            amount = static_cast<uint32_t>(std::stoul(amountStr));
        } catch (...) {
            LogLine("purchases.txt: skipping malformed line: %s", entry.c_str());
            continue;
        }
        if (accountId == 0 || (itemKindRaw != 0 && itemKindRaw != 1)) {
            LogLine("purchases.txt: skipping invalid line: %s", entry.c_str());
            continue;
        }

        bool delivered = false;
        for (auto& [id, player] : g_players) {
            if (player.accountId != accountId) continue;

            ao::proto::PurchaseDelivery delivery{};
            delivery.accountId = accountId;
            delivery.itemKind = static_cast<ao::proto::PurchaseItemKind>(itemKindRaw);
            delivery.itemId = itemId;
            delivery.amount = amount;
            SendMessage(player.peer, ao::proto::MessageType::PurchaseDelivery, delivery, /*reliable=*/true);

            LogLine("delivered purchase (accountId %u, itemKind %d, itemId %d, amount %u) "
                     "to player %u ('%s')",
                     accountId, itemKindRaw, itemId, amount, id, player.name.c_str());
            delivered = true;
            break; // one connected session per accountId in the normal case
        }
        if (!delivered) {
            LogLine("purchases.txt: accountId %u is not currently connected - "
                     "purchase dropped (see known limitation in docs/MEMORY.md)",
                     accountId);
        }
    }

    // Drain the queue now that every line has been attempted, whether or
    // not it found a recipient - see this function's own comment above.
    std::ofstream clearQueue("purchases.txt", std::ios::trunc);
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

    // Accounts (protocol v4): load (or start fresh, if this file doesn't
    // exist yet - e.g. first run) accounts.dat next to the executable,
    // same convention as server.cfg above.
    g_accounts.Load("accounts.dat");
    LogLine("loaded %zu persistent account(s) from accounts.dat", g_accounts.Count());

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
    uint32_t lastPurchasePollMs = 0;

    // 2026-11-27, found during the first live playtest of the store: this
    // used to be checked only AFTER the "while (enet_host_service(...) > 0)"
    // drain loop below had exited. That's fine when the network goes quiet
    // for a moment, but it is NOT fine under steady traffic - this server
    // also broadcasts/receives position updates at roughly 10 Hz (every
    // ~100ms) per connected player, which is faster than the drain loop's
    // own work takes. As long as a new event is always waiting by the time
    // enet_host_service() is called again, its "> 0" condition stays true
    // indefinitely and the loop never falls through - meaning the code that
    // used to sit after it (this purchase poll) could be starved for as
    // long as ANY player stayed connected and moving, which is exactly what
    // happened: purchases.txt sat completely untouched for over a minute
    // with a player actively connected, confirmed via a live test. Calling
    // this once per event too (not just once per drain-loop exit) closes
    // that gap - it's a no-op almost every call (lastPurchasePollMs guards
    // it to roughly once every kPurchasePollIntervalMs regardless of how
    // often it's invoked), so the extra calls cost nothing.
    const auto pollPurchasesIfDue = [&]() {
        const uint32_t now = enet_time_get();
        if (lastPurchasePollMs == 0 || (now - lastPurchasePollMs) >= kPurchasePollIntervalMs) {
            lastPurchasePollMs = now;
            PollPurchaseQueue(host);
        }
    };

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
            pollPurchasesIfDue();
        }

        // Item 7 ("store/purchase delivery") - also reached here in the
        // normal case where there's no network activity right now (the
        // drain loop above never ran this iteration).
        pollPurchasesIfDue();
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
