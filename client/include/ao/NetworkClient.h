// Client-side ENet wrapper. Connecting is fire-and-forget and never fatal:
// if there's no server (or the address is wrong), the plugin keeps working
// exactly as it did before networking existed - it just never reports
// IsConnected()/IsReady().
#pragma once

#include <cstdint>
#include <string>

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
    void Connect(const std::string& host, uint16_t port, const std::string& playerName);

    void Disconnect();

    // Must be called regularly (the polling thread calls it once per tick)
    // to pump ENet's event queue, finish the handshake, and dispatch
    // incoming messages. Never blocks.
    void Poll();

    bool IsConnected() const { return m_connected; }
    bool IsReady() const { return m_connected && m_handshakeAccepted; }
    uint32_t LocalPlayerId() const { return m_localPlayerId; }

    // No-ops (silently) if !IsReady().
    void SendPlayerState(const proto::PlayerState& state);
    void SendChatMessage(const std::string& text);

private:
    void HandleConnect();
    void HandleDisconnect();
    void HandlePacket(const uint8_t* data, size_t len);

    _ENetHost* m_host = nullptr;
    _ENetPeer* m_peer = nullptr;
    bool       m_enetOk = false;
    bool       m_connected = false;
    bool       m_handshakeAccepted = false;
    uint32_t   m_localPlayerId = 0;
    std::string m_pendingPlayerName;
};

} // namespace ao
