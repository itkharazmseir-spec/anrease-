#include "ao/NetworkClient.h"

#include <cstdio>
#include <cstring>

#include <enet/enet.h>

#include "Log.h"
#include "ao/Version.h"

#if defined(_WIN32)
#include <windows.h> // GetTickCount
#endif

namespace ao {

namespace {
constexpr enet_uint8 kChannelReliable   = 0;
constexpr enet_uint8 kChannelUnreliable = 1;
constexpr enet_uint8 kChannelCount      = 2;
constexpr uint32_t   kReconnectIntervalMs = 5000;

bool g_enetInitialized = false;

uint32_t NowMs() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetTickCount());
#else
    return 0;
#endif
}

std::string ClampedName(const char* raw, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && raw[len] != '\0') ++len;
    return std::string(raw, len);
}
} // namespace

NetworkClient::NetworkClient() {
    if (!g_enetInitialized) {
        g_enetInitialized = (enet_initialize() == 0);
        if (!g_enetInitialized) {
            log::Error("NetworkClient: enet_initialize failed - networking disabled");
        }
    }
    m_enetOk = g_enetInitialized;
}

NetworkClient::~NetworkClient() {
    Disconnect();
    // Deliberately not calling enet_deinitialize(): this object is a single
    // process-lifetime global (see Main.cpp), so there is nothing useful to
    // clean up before the process exits anyway.
}

void NetworkClient::Connect(const std::string& host, uint16_t port, const std::string& playerName) {
    if (!m_enetOk) return;

    // Remember these so Poll() can retry with the same parameters later -
    // set before anything below can fail/return early, and before
    // Disconnect() (which doesn't touch these fields), so a reconnect always
    // has the right target even if this particular attempt never gets as
    // far as creating a peer.
    m_lastHost = host;
    m_lastPort = port;
    m_pendingPlayerName = playerName;
    m_autoReconnect = true;
    m_reconnectScheduled = false; // this call *is* the (re)connect attempt

    Disconnect(); // drop any previous attempt/connection first

    m_host = reinterpret_cast<_ENetHost*>(enet_host_create(nullptr, 1, kChannelCount, 0, 0));
    if (!m_host) {
        log::Error("NetworkClient: enet_host_create failed");
        return;
    }

    ENetAddress addr{};
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        log::Error("NetworkClient: could not resolve server address '%s'", host.c_str());
        enet_host_destroy(reinterpret_cast<ENetHost*>(m_host));
        m_host = nullptr;
        return;
    }
    addr.port = port;

    m_peer = reinterpret_cast<_ENetPeer*>(
        enet_host_connect(reinterpret_cast<ENetHost*>(m_host), &addr, kChannelCount, 0));
    if (!m_peer) {
        log::Error("NetworkClient: enet_host_connect failed");
        enet_host_destroy(reinterpret_cast<ENetHost*>(m_host));
        m_host = nullptr;
        return;
    }

    log::Info("NetworkClient: connecting to %s:%u as '%s'...", host.c_str(), port, playerName.c_str());
}

void NetworkClient::Disconnect() {
    if (m_peer) {
        enet_peer_reset(reinterpret_cast<ENetPeer*>(m_peer));
        m_peer = nullptr;
    }
    if (m_host) {
        enet_host_destroy(reinterpret_cast<ENetHost*>(m_host));
        m_host = nullptr;
    }
    m_connected = false;
    m_handshakeAccepted = false;
    m_localPlayerId = 0;

    std::lock_guard<std::mutex> lock(m_playersMutex);
    m_baseNames.clear();
    m_playerHealth.clear();
}

void NetworkClient::HandleConnect() {
    m_connected = true;
    log::Info("NetworkClient: connected, sending handshake");

    proto::Handshake hs{};
    hs.protocolVersion = kProtocolVersion;
    std::memset(hs.playerName, 0, sizeof(hs.playerName));
    std::strncpy(hs.playerName, m_pendingPlayerName.c_str(), proto::kMaxNameLen - 1);

    uint8_t buf[1 + sizeof(proto::Handshake)];
    buf[0] = static_cast<uint8_t>(proto::MessageType::Handshake);
    std::memcpy(buf + 1, &hs, sizeof(hs));

    ENetPacket* packet = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(reinterpret_cast<ENetPeer*>(m_peer), kChannelReliable, packet);
}

void NetworkClient::HandleDisconnect() {
    if (m_handshakeAccepted) {
        log::Info("NetworkClient: disconnected from server");
    }
    m_connected = false;
    m_handshakeAccepted = false;
    m_localPlayerId = 0;
    m_peer = nullptr; // the host destroyed the peer object already

    {
        std::lock_guard<std::mutex> lock(m_playersMutex);
        m_baseNames.clear();
        m_playerHealth.clear();
    }

    // This fires both for "lost an established connection" and for "the
    // connect attempt itself never completed" (ENet reports both as a
    // DISCONNECT event) - either way, retry later rather than requiring a
    // plugin/game reload. Not done for an app-level handshake rejection
    // (ProtocolMismatch/ServerFull): that's a successful ENet connection, so
    // no DISCONNECT event happens here - see HandlePacket's HandshakeAck case.
    if (m_autoReconnect) {
        m_reconnectScheduled = true;
        m_nextReconnectAtMs = NowMs() + kReconnectIntervalMs;
        log::Info("NetworkClient: will retry %s:%u in %u ms",
                  m_lastHost.c_str(), m_lastPort, kReconnectIntervalMs);
    }
}

void NetworkClient::HandlePacket(const uint8_t* data, size_t len) {
    if (len < 1) return;
    const auto type = static_cast<proto::MessageType>(data[0]);
    const uint8_t* body = data + 1;
    const size_t bodyLen = len - 1;

    switch (type) {
        case proto::MessageType::HandshakeAck: {
            if (bodyLen != sizeof(proto::HandshakeAck)) return;
            proto::HandshakeAck ack{};
            std::memcpy(&ack, body, sizeof(ack));
            if (ack.result == proto::HandshakeResult::Accepted) {
                m_handshakeAccepted = true;
                m_localPlayerId = ack.assignedPlayerId;
                log::Info("NetworkClient: handshake accepted, player id = %u", m_localPlayerId);
                {
                    std::lock_guard<std::mutex> lock(m_playersMutex);
                    m_baseNames[m_localPlayerId] = m_pendingPlayerName;
                    m_playerHealth[m_localPlayerId] = 100.0f; // until our own PlayerState echoes back
                }
            } else {
                const char* reason =
                    ack.result == proto::HandshakeResult::ProtocolMismatch ? "protocol mismatch" :
                    ack.result == proto::HandshakeResult::ServerFull       ? "server full" :
                                                                              "unknown";
                log::Error("NetworkClient: handshake rejected (%s)", reason);

                // enet_peer_disconnect_later (used by the server for a
                // rejection) still delivers a real ENET_EVENT_TYPE_DISCONNECT
                // once the notification flushes, which would otherwise make
                // HandleDisconnect() schedule a reconnect - retrying forever
                // against a server that will keep rejecting us the same way.
                // ProtocolMismatch can never succeed (the binaries don't
                // match), so stop entirely; ServerFull may free up a slot
                // later, so keep retrying for that one only.
                if (ack.result == proto::HandshakeResult::ProtocolMismatch) {
                    m_autoReconnect = false;
                }
            }
            break;
        }
        case proto::MessageType::PlayerJoined: {
            if (bodyLen != sizeof(proto::PlayerJoined)) return;
            proto::PlayerJoined joined{};
            std::memcpy(&joined, body, sizeof(joined));
            if (joined.playerId == m_localPlayerId) break; // that's us
            {
                const std::string name = ClampedName(joined.playerName, proto::kMaxNameLen);
                log::Info("NetworkClient: player %u ('%s') joined", joined.playerId, name.c_str());
                std::lock_guard<std::mutex> lock(m_playersMutex);
                m_baseNames[joined.playerId] = name;
                m_playerHealth[joined.playerId] = 100.0f; // until their first PlayerState arrives
            }
            break;
        }
        case proto::MessageType::PlayerLeft: {
            if (bodyLen != sizeof(proto::PlayerLeft)) return;
            proto::PlayerLeft left{};
            std::memcpy(&left, body, sizeof(left));
            log::Info("NetworkClient: player %u left", left.playerId);
            {
                std::lock_guard<std::mutex> lock(m_playersMutex);
                m_baseNames.erase(left.playerId);
                m_playerHealth.erase(left.playerId);
            }
            if (m_playerLeftCallback) m_playerLeftCallback(left.playerId);
            break;
        }
        case proto::MessageType::PlayerState: {
            if (bodyLen != sizeof(proto::PlayerState)) return;
            proto::PlayerState state{};
            std::memcpy(&state, body, sizeof(state));
            {
                std::lock_guard<std::mutex> lock(m_playersMutex);
                m_playerHealth[state.playerId] = state.health;
            }
            if (state.playerId == m_localPlayerId) break; // our own, echoed back
            if (m_playerStateCallback) m_playerStateCallback(state);
            break;
        }
        case proto::MessageType::ChatMessage: {
            if (bodyLen != sizeof(proto::ChatMessage)) return;
            proto::ChatMessage msg{};
            std::memcpy(&msg, body, sizeof(msg));
            const std::string text = ClampedName(msg.text, proto::kMaxChatLen);
            log::Info("NetworkClient: chat from %u: %s", msg.playerId, text.c_str());
            if (m_chatCallback) m_chatCallback(msg.playerId, text);
            break;
        }
        case proto::MessageType::DamageReport: {
            if (bodyLen != sizeof(proto::DamageReport)) return;
            proto::DamageReport report{};
            std::memcpy(&report, body, sizeof(report));
            if (m_damageReportCallback) m_damageReportCallback(report);
            break;
        }
        default:
            break;
    }
}

void NetworkClient::Poll() {
    if (!m_enetOk) return;

    if (m_reconnectScheduled && NowMs() >= m_nextReconnectAtMs) {
        m_reconnectScheduled = false;
        log::Info("NetworkClient: retrying connection to %s:%u...", m_lastHost.c_str(), m_lastPort);
        Connect(m_lastHost, m_lastPort, m_pendingPlayerName);
    }

    if (!m_host) return;

    ENetEvent event;
    while (enet_host_service(reinterpret_cast<ENetHost*>(m_host), &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                HandleConnect();
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                HandlePacket(event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                HandleDisconnect();
                break;
            default:
                break;
        }
    }
}

void NetworkClient::SendPlayerState(const proto::PlayerState& stateIn) {
    if (!IsReady()) return;

    proto::PlayerState state = stateIn;
    state.playerId = m_localPlayerId; // server overwrites it anyway; set for clarity
    state.tickMs = NowMs();

    uint8_t buf[1 + sizeof(proto::PlayerState)];
    buf[0] = static_cast<uint8_t>(proto::MessageType::PlayerState);
    std::memcpy(buf + 1, &state, sizeof(state));

    ENetPacket* packet = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_UNSEQUENCED);
    enet_peer_send(reinterpret_cast<ENetPeer*>(m_peer), kChannelUnreliable, packet);
}

void NetworkClient::SendDamageReport(uint32_t victimPlayerId, float healthDelta, float armourDelta) {
    if (!IsReady()) return;

    proto::DamageReport report{};
    report.victimPlayerId = victimPlayerId;
    report.attackerPlayerId = m_localPlayerId; // server overwrites it anyway; set for clarity
    report.healthDelta = healthDelta;
    report.armourDelta = armourDelta;

    uint8_t buf[1 + sizeof(proto::DamageReport)];
    buf[0] = static_cast<uint8_t>(proto::MessageType::DamageReport);
    std::memcpy(buf + 1, &report, sizeof(report));

    // Reliable (channel 0), unlike SendPlayerState - a damage event must
    // never be silently dropped the way an occasional stale position update
    // can be.
    ENetPacket* packet = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(reinterpret_cast<ENetPeer*>(m_peer), kChannelReliable, packet);
}

std::vector<std::pair<uint32_t, std::string>> NetworkClient::GetPlayers() const {
    std::lock_guard<std::mutex> lock(m_playersMutex);
    std::vector<std::pair<uint32_t, std::string>> result;
    result.reserve(m_baseNames.size());
    for (const auto& [id, name] : m_baseNames) {
        const auto hpIt = m_playerHealth.find(id);
        const float hp = (hpIt != m_playerHealth.end()) ? hpIt->second : 0.0f;

        char buf[80];
        if (id == m_localPlayerId) {
            std::snprintf(buf, sizeof(buf), "%s (you) [HP %.0f]", name.c_str(), hp);
        } else {
            std::snprintf(buf, sizeof(buf), "%s [HP %.0f]", name.c_str(), hp);
        }
        result.emplace_back(id, buf);
    }
    return result;
}

void NetworkClient::SetChatCallback(std::function<void(uint32_t, const std::string&)> cb) {
    // Only ever set once from Main.cpp before the polling thread starts, and
    // only ever invoked from that same polling thread (inside Poll()) - no
    // lock needed for the std::function itself.
    m_chatCallback = std::move(cb);
}

void NetworkClient::SetPlayerStateCallback(std::function<void(const proto::PlayerState&)> cb) {
    m_playerStateCallback = std::move(cb);
}

void NetworkClient::SetPlayerLeftCallback(std::function<void(uint32_t)> cb) {
    m_playerLeftCallback = std::move(cb);
}

void NetworkClient::SetDamageReportCallback(std::function<void(const proto::DamageReport&)> cb) {
    m_damageReportCallback = std::move(cb);
}

void NetworkClient::SendChatMessage(const std::string& text) {
    if (!IsReady()) return;

    proto::ChatMessage msg{};
    msg.playerId = m_localPlayerId;
    std::memset(msg.text, 0, sizeof(msg.text));
    std::strncpy(msg.text, text.c_str(), proto::kMaxChatLen - 1);

    uint8_t buf[1 + sizeof(proto::ChatMessage)];
    buf[0] = static_cast<uint8_t>(proto::MessageType::ChatMessage);
    std::memcpy(buf + 1, &msg, sizeof(msg));

    ENetPacket* packet = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(reinterpret_cast<ENetPeer*>(m_peer), kChannelReliable, packet);
}

} // namespace ao
