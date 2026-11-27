// Client-side ENet wrapper. Connecting is fire-and-forget and never fatal:
// if there's no server (or the address is wrong), the plugin keeps working
// exactly as it did before networking existed - it just never reports
// IsConnected()/IsReady().
//
// Auto-reconnect: once Connect() has been called, the client keeps trying
// in the background (every kReconnectIntervalMs, see NetworkClient.cpp)
// whenever it isn't connected - covers "server was down when the game
// started" and "server crashed/restarted while playing" without needing to
// reload the plugin. This does not apply to an app-level handshake
// rejection (protocol mismatch) - retrying that would just repeat the same
// rejection forever, so it's logged once and left alone.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ao/Protocol.h"

struct _ENetHost;
struct _ENetPeer;

namespace ao {

class NetworkClient {
public:
    NetworkClient();
    ~NetworkClient();

    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    // Starts an asynchronous connection attempt. Safe to call even if ENet
    // failed to initialise (it will just do nothing). Does not block.
    //
    // accountUser/accountPassword (protocol v4, 2026-11-27): optional - pass
    // empty strings for the old anonymous behaviour. If accountUser is
    // non-empty, accountPassword is hashed (SHA-256) right here before the
    // handshake is sent - see Sha256.h - the plaintext password itself is
    // never put on the wire.
    void Connect(const std::string& host, uint16_t port, const std::string& playerName,
                 const std::string& accountUser = std::string(),
                 const std::string& accountPassword = std::string());

    void Disconnect();

    // Must be called regularly (the polling thread calls it once per tick)
    // to pump ENet's event queue, finish the handshake, and dispatch
    // incoming messages. Never blocks.
    void Poll();

    bool IsConnected() const { return m_connected; }
    bool IsReady() const { return m_connected && m_handshakeAccepted; }
    uint32_t LocalPlayerId() const { return m_localPlayerId; }
    // 0 if this connection is anonymous (no account given to Connect()) or
    // the handshake hasn't completed yet - otherwise the PERSISTENT account
    // id the server assigned, the same every time this account logs in
    // (protocol v4, 2026-11-27).
    uint32_t LocalAccountId() const { return m_localAccountId; }

    // No-ops (silently) if !IsReady().
    void SendPlayerState(const proto::PlayerState& state);
    void SendChatMessage(const std::string& text);

    // No-op (silently) if !IsReady(). See Protocol.h's DamageReport struct
    // for the full design (item 5, "weapon/damage sync") - victimPlayerId is
    // whichever remote player's stand-in ped RemotePedManager just observed
    // losing health/armour (RemotePeds.h's damage-detected callback).
    // Reliable, unlike SendPlayerState - a damage event must never be
    // silently dropped the way an occasional stale position update can be.
    void SendDamageReport(uint32_t victimPlayerId, float healthDelta, float armourDelta);

    // Thread-safe snapshot of who the server has told us is online (built
    // from PlayerJoined/PlayerLeft, plus the latest health from PlayerState),
    // id -> a ready-to-display "Name (you) [HP n]" string. Cheap to call
    // every tick - the overlay (step: in-game HUD, see Overlay.h) uses this
    // instead of real CPeds, since we don't yet have a verified address to
    // spawn one on this build (see docs on CPopulation::AddPed). Showing
    // health here is the easiest way to tell if a remote player just died
    // without a real 3D ped to look at.
    std::vector<std::pair<uint32_t, std::string>> GetPlayers() const;

    // Fired (from whichever thread calls Poll(), i.e. the polling thread)
    // whenever a ChatMessage arrives, including the echo of our own sent
    // messages (the server broadcasts to everyone, sender included).
    void SetChatCallback(std::function<void(uint32_t playerId, const std::string& text)> cb);

    // Fired (from the polling thread, same as SetChatCallback) for every
    // PlayerState belonging to a remote player (never for our own - already
    // filtered out) and for every PlayerLeft. Used by RemotePedManager to
    // spawn/move/retire a real CPed for each remote player - see RemotePeds.h.
    void SetPlayerStateCallback(std::function<void(const proto::PlayerState&)> cb);
    void SetPlayerLeftCallback(std::function<void(uint32_t playerId)> cb);

    // Fired (from the polling thread, same as the others) for every
    // DamageReport the server broadcasts, including the echo of one this
    // client itself just sent. Main.cpp checks victimPlayerId against
    // LocalPlayerId() and, if it matches, applies the damage to the local
    // player's own ped via LocalPlayer::ApplyDamage() - every other client
    // just ignores a report that doesn't name them.
    void SetDamageReportCallback(std::function<void(const proto::DamageReport&)> cb);

    // Fired (from the polling thread, same as the others) for every
    // PurchaseDelivery the server sends - protocol v5, 2026-11-27, item 7
    // ("store/purchase delivery"). Main.cpp checks accountId against
    // LocalAccountId() itself before acting on it (never trust the server
    // alone to have addressed it correctly) - see Protocol.h's
    // PurchaseDelivery struct and docs/MEMORY.md for the full design.
    void SetPurchaseDeliveryCallback(std::function<void(const proto::PurchaseDelivery&)> cb);

private:
    void HandleConnect();
    void HandleDisconnect();
    void HandlePacket(const uint8_t* data, size_t len);

    // Does the actual ENet host/peer setup using whatever is already in
    // m_lastHost/m_lastPort/m_pendingPlayerName/m_pendingAccountUser/
    // m_pendingAccountPasswordHash, WITHOUT touching any of them - shared by
    // Connect() (which sets those fields fresh right before calling this)
    // and Poll()'s auto-reconnect path, which must reuse the SAME account
    // credentials an earlier Connect() call already hashed. Fixes a real bug
    // (2026-11-27): Poll() used to call the public Connect() again with no
    // accountUser/accountPassword args, which silently wiped the stored
    // credentials (accountUser.empty() == true) and reconnected the player
    // as anonymous every single time the connection dropped and
    // auto-reconnected - see docs/MEMORY.md.
    void DoConnect();

    _ENetHost* m_host = nullptr;
    _ENetPeer* m_peer = nullptr;
    bool       m_enetOk = false;
    bool       m_connected = false;
    bool       m_handshakeAccepted = false;
    uint32_t   m_localPlayerId = 0;
    uint32_t   m_localAccountId = 0;
    std::string m_pendingPlayerName;
    std::string m_pendingAccountUser;
    std::array<uint8_t, 32> m_pendingAccountPasswordHash{};

    // Auto-reconnect bookkeeping (see the class comment above).
    bool        m_autoReconnect = false;
    bool        m_reconnectScheduled = false;
    uint32_t    m_nextReconnectAtMs = 0;
    std::string m_lastHost;
    uint16_t    m_lastPort = 0;

    mutable std::mutex m_playersMutex;
    std::map<uint32_t, std::string> m_baseNames;        // guarded by m_playersMutex: id -> raw name (no suffixes)
    std::unordered_map<uint32_t, float> m_playerHealth; // guarded by m_playersMutex: id -> latest PlayerState.health
                                                         // (defaults to 100 for a player we haven't gotten a
                                                         // PlayerState from yet)

    std::function<void(uint32_t, const std::string&)> m_chatCallback;
    std::function<void(const proto::PlayerState&)>    m_playerStateCallback;
    std::function<void(uint32_t)>                     m_playerLeftCallback;
    std::function<void(const proto::DamageReport&)>   m_damageReportCallback;
    std::function<void(const proto::PurchaseDelivery&)> m_purchaseDeliveryCallback;
};

} // namespace ao
