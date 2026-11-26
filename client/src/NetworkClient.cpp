#include "ao/NetworkClient.h"

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

    m_pendingPlayerName = playerName;
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
            } else {
                const char* reason =
                    ack.result == proto::HandshakeResult::ProtocolMismatch ? "protocol mismatch" :
                    ack.result == proto::HandshakeResult::ServerFull       ? "server full" :
                                                                              "unknown";
                log::Error("NetworkClient: handshake rejected (%s)", reason);
            }
            break;
        }
        case proto::MessageType::PlayerJoined: {
            if (bodyLen != sizeof(proto::PlayerJoined)) return;
            proto::PlayerJoined joined{};
            std::memcpy(&joined, body, sizeof(joined));
            if (joined.playerId == m_localPlayerId) break; // that's us
            log::Info("NetworkClient: player %u ('%s') joined", joined.playerId,
                      ClampedName(joined.playerName, proto::kMaxNameLen).c_str());
            break;
        }
        case proto::MessageType::PlayerLeft: {
            if (bodyLen != sizeof(proto::PlayerLeft)) return;
            proto::PlayerLeft left{};
            std::memcpy(&left, body, sizeof(left));
            log::Info("NetworkClient: player %u left", left.playerId);
            break;
        }
        case proto::MessageType::PlayerState: {
            if (bodyLen != sizeof(proto::PlayerState)) return;
            proto::PlayerState state{};
            std::memcpy(&state, body, sizeof(state));
            if (state.playerId == m_localPlayerId) break; // our own, echoed back
            // Step 3 (not implemented yet) will move/spawn a real CPed here
            // instead of just logging it.
            log::Info("NetworkClient: remote player %u pos %.3f %.3f %.3f veh %d",
                      state.playerId, state.x, state.y, state.z, state.vehicleModel);
            break;
        }
        case proto::MessageType::ChatMessage: {
            if (bodyLen != sizeof(proto::ChatMessage)) return;
            proto::ChatMessage msg{};
            std::memcpy(&msg, body, sizeof(msg));
            log::Info("NetworkClient: chat from %u: %s", msg.playerId,
                      ClampedName(msg.text, proto::kMaxChatLen).c_str());
            break;
        }
        default:
            break;
    }
}

void NetworkClient::Poll() {
    if (!m_enetOk || !m_host) return;

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
