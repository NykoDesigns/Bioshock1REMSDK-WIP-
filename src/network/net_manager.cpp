#include "net_manager.h"
#include "coop_sync.h"
#include "udp_socket.h"
#include "../core/log.h"

#include <chrono>
#include <cstdlib>
#include <ctime>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static UdpSocket     s_Socket;
static NetRole       s_Role = NetRole::None;
static NetPeer       s_RemotePeer;
static std::string   s_LocalName;
static std::string   s_LocalLevel;
static uint32_t      s_SessionId = 0;
static float         s_Uptime = 0.0f;
static float         s_SendAccum = 0.0f;
static uint32_t      s_LocalSeqNum = 0;
static PlayerStateData s_LastSentState{};

static OnRemoteStateFunc s_OnRemoteState;
static OnPeerEventFunc   s_OnPeerEvent;
static OnChatFunc        s_OnChat;
static float             s_PingAccum = 0.0f; // ping keepalive timer

// Buffer for outgoing packets
static uint8_t s_SendBuf[NET_MAX_PACKET];
static uint8_t s_RecvBuf[NET_MAX_PACKET];

// ─── Helpers ───────────────────────────────────────────────────────────

static uint32_t GenerateSessionId()
{
    srand((unsigned)time(nullptr) ^ (unsigned)clock());
    return (uint32_t)rand() ^ ((uint32_t)rand() << 16);
}

static void BuildPacket(uint8_t* buf, int& outSize, PacketType type, const void* payload, uint16_t payloadSize)
{
    PacketHeader hdr{};
    hdr.magic = NET_PROTOCOL_MAGIC;
    hdr.version = NET_PROTOCOL_VERSION;
    hdr.type = type;
    hdr.size = payloadSize;

    memcpy(buf, &hdr, sizeof(hdr));
    if (payload && payloadSize > 0) {
        memcpy(buf + sizeof(hdr), payload, payloadSize);
    }
    outSize = sizeof(hdr) + payloadSize;
}

static bool SendPacket(PacketType type, const void* payload, uint16_t payloadSize)
{
    if (!s_RemotePeer.connected && type != PacketType::Handshake) return false;

    int size = 0;
    BuildPacket(s_SendBuf, size, type, payload, payloadSize);
    return s_Socket.SendTo(s_SendBuf, size, s_RemotePeer.ip, s_RemotePeer.port);
}

// ─── Packet Handlers ───────────────────────────────────────────────────

static void HandleHandshake(const std::string& fromIp, uint16_t fromPort,
                            const HandshakeData* data)
{
    if (s_Role == NetRole::Host) {
        // Accept connection
        s_RemotePeer.ip = fromIp;
        s_RemotePeer.port = fromPort;
        s_RemotePeer.name = data->playerName;
        s_RemotePeer.levelName = data->levelName;
        s_RemotePeer.sessionId = data->sessionId;
        s_RemotePeer.lastRecvTime = s_Uptime;
        s_RemotePeer.connected = true;

        LOG_INFO("[Net] Player '{}' connected from {}:{} (level: {})",
                 data->playerName, fromIp, fromPort, data->levelName);

        // Warn if levels don't match
        if (s_LocalLevel.length() > 0 && s_RemotePeer.levelName.length() > 0 &&
            s_LocalLevel != s_RemotePeer.levelName) {
            LOG_WARN("[Net] LEVEL MISMATCH! Local='{}' Remote='{}' - sync will not work!",
                     s_LocalLevel, s_RemotePeer.levelName);
        }

        // Send HandshakeAck with our info
        HandshakeData ack{};
        strncpy(ack.playerName, s_LocalName.c_str(), sizeof(ack.playerName) - 1);
        strncpy(ack.levelName, s_LocalLevel.c_str(), sizeof(ack.levelName) - 1);
        ack.sessionId = s_SessionId;
        SendPacket(PacketType::HandshakeAck, &ack, sizeof(ack));

        if (s_OnPeerEvent) s_OnPeerEvent(s_RemotePeer, true);
    }
}

static void HandleHandshakeAck(const std::string& fromIp, uint16_t fromPort,
                                const HandshakeData* data)
{
    if (s_Role == NetRole::Client && !s_RemotePeer.connected) {
        s_RemotePeer.name = data->playerName;
        s_RemotePeer.levelName = data->levelName;
        s_RemotePeer.sessionId = data->sessionId;
        s_RemotePeer.lastRecvTime = s_Uptime;
        s_RemotePeer.connected = true;

        LOG_INFO("[Net] Connected to host '{}' at {}:{} (level: {})",
                 data->playerName, fromIp, fromPort, data->levelName);

        if (s_LocalLevel.length() > 0 && s_RemotePeer.levelName.length() > 0 &&
            s_LocalLevel != s_RemotePeer.levelName) {
            LOG_WARN("[Net] LEVEL MISMATCH! Local='{}' Remote='{}' - sync will not work!",
                     s_LocalLevel, s_RemotePeer.levelName);
        }

        if (s_OnPeerEvent) s_OnPeerEvent(s_RemotePeer, true);
    }
}

static void HandlePlayerState(const PlayerStateData* data)
{
    if (!s_RemotePeer.connected) return;

    // Only accept if sequence number is newer (handles out-of-order packets)
    if (data->sequenceNum <= s_RemotePeer.lastSeqNum && s_RemotePeer.lastSeqNum != 0) return;

    s_RemotePeer.lastState = *data;
    s_RemotePeer.lastSeqNum = data->sequenceNum;
    s_RemotePeer.lastRecvTime = s_Uptime;

    if (s_OnRemoteState) s_OnRemoteState(s_RemotePeer, *data);
}

static void HandleDisconnect()
{
    if (s_RemotePeer.connected) {
        LOG_INFO("[Net] Peer '{}' disconnected", s_RemotePeer.name);
        s_RemotePeer.connected = false;
        if (s_OnPeerEvent) s_OnPeerEvent(s_RemotePeer, false);
    }
}

static void HandlePing()
{
    s_RemotePeer.lastRecvTime = s_Uptime;
    SendPacket(PacketType::Pong, nullptr, 0);
}

// ─── Process Incoming ──────────────────────────────────────────────────

static void ProcessIncoming()
{
    std::string fromIp;
    uint16_t fromPort;

    // Drain all available packets
    for (int i = 0; i < 64; i++) {
        int bytes = s_Socket.RecvFrom(s_RecvBuf, NET_MAX_PACKET, fromIp, fromPort);
        if (bytes <= 0) break;

        if (bytes < (int)sizeof(PacketHeader)) continue;

        auto* hdr = reinterpret_cast<PacketHeader*>(s_RecvBuf);
        if (!hdr->IsValid()) continue;
        if (hdr->size > bytes - (int)sizeof(PacketHeader)) continue;

        const uint8_t* payload = s_RecvBuf + sizeof(PacketHeader);

        switch (hdr->type) {
        case PacketType::Handshake:
            if (hdr->size >= sizeof(HandshakeData))
                HandleHandshake(fromIp, fromPort, reinterpret_cast<const HandshakeData*>(payload));
            break;
        case PacketType::HandshakeAck:
            if (hdr->size >= sizeof(HandshakeData))
                HandleHandshakeAck(fromIp, fromPort, reinterpret_cast<const HandshakeData*>(payload));
            break;
        case PacketType::PlayerState:
            if (hdr->size >= sizeof(PlayerStateData))
                HandlePlayerState(reinterpret_cast<const PlayerStateData*>(payload));
            break;
        case PacketType::Ping:
            HandlePing();
            break;
        case PacketType::Pong:
            s_RemotePeer.lastRecvTime = s_Uptime;
            break;
        case PacketType::Disconnect:
            HandleDisconnect();
            break;
        case PacketType::Damage:
            if (hdr->size >= sizeof(DamageData))
                QueueDamagePacket(*reinterpret_cast<const DamageData*>(payload));
            break;
        case PacketType::WorldEvent:
            if (hdr->size >= sizeof(WorldEventData))
                QueueWorldEventPacket(*reinterpret_cast<const WorldEventData*>(payload));
            break;
        case PacketType::Chat:
            if (hdr->size >= sizeof(ChatData)) {
                auto* chatData = reinterpret_cast<const ChatData*>(payload);
                s_RemotePeer.lastRecvTime = s_Uptime;
                if (s_OnChat) s_OnChat(s_RemotePeer.name, chatData->message);
            }
            break;
        case PacketType::EnemyDeath:
            if (hdr->size >= sizeof(EnemyDeathData)) {
                s_RemotePeer.lastRecvTime = s_Uptime;
                QueueEnemyDeathPacket(*reinterpret_cast<const EnemyDeathData*>(payload));
            }
            break;
        case PacketType::PlayerDeath:
            if (hdr->size >= sizeof(PlayerDeathData)) {
                s_RemotePeer.lastRecvTime = s_Uptime;
                QueuePlayerDeathPacket(*reinterpret_cast<const PlayerDeathData*>(payload));
            }
            break;
        case PacketType::PlayerRespawn:
            if (hdr->size >= sizeof(PlayerRespawnData)) {
                s_RemotePeer.lastRecvTime = s_Uptime;
                QueuePlayerRespawnPacket(*reinterpret_cast<const PlayerRespawnData*>(payload));
            }
            break;
        case PacketType::TriggerSync:
            if (hdr->size >= sizeof(TriggerSyncData)) {
                s_RemotePeer.lastRecvTime = s_Uptime;
                QueueTriggerSyncPacket(*reinterpret_cast<const TriggerSyncData*>(payload));
            }
            break;
        case PacketType::EnemyHPSync:
            if (hdr->size >= sizeof(EnemyHPSyncData)) {
                s_RemotePeer.lastRecvTime = s_Uptime;
                QueueEnemyHPSyncPacket(*reinterpret_cast<const EnemyHPSyncData*>(payload));
            }
            break;
        case PacketType::PlayerAction:
            if (hdr->size >= sizeof(PlayerActionData)) {
                s_RemotePeer.lastRecvTime = s_Uptime;
                QueuePlayerActionPacket(*reinterpret_cast<const PlayerActionData*>(payload));
            }
            break;
        default:
            break;
        }
    }
}

// ─── Timeout Check ─────────────────────────────────────────────────────

static void CheckTimeout()
{
    if (!s_RemotePeer.connected) return;
    if (s_Uptime - s_RemotePeer.lastRecvTime > NET_TIMEOUT_SEC) {
        LOG_WARN("[Net] Peer '{}' timed out", s_RemotePeer.name);
        s_RemotePeer.connected = false;
        if (s_OnPeerEvent) s_OnPeerEvent(s_RemotePeer, false);
    }
}

// ─── Public API ────────────────────────────────────────────────────────

bool NetHost(uint16_t port, const std::string& playerName)
{
    if (s_Role != NetRole::None) {
        LOG_WARN("[Net] Already in a session. Disconnect first.");
        return false;
    }

    if (!s_Socket.Bind(port)) return false;

    s_Role = NetRole::Host;
    s_LocalName = playerName;
    s_SessionId = GenerateSessionId();
    s_Uptime = 0.0f;
    s_SendAccum = 0.0f;
    s_LocalSeqNum = 0;
    s_RemotePeer = {};

    LOG_INFO("[Net] Hosting co-op session on port {} as '{}'", port, playerName);
    LOG_INFO("[Net] Waiting for player to join...");
    return true;
}

bool NetJoin(const std::string& hostIp, uint16_t port, const std::string& playerName)
{
    if (s_Role != NetRole::None) {
        LOG_WARN("[Net] Already in a session. Disconnect first.");
        return false;
    }

    // Bind to any available port
    if (!s_Socket.Bind(0)) {
        // Try a specific port
        if (!s_Socket.Bind(port + 1)) return false;
    }

    s_Role = NetRole::Client;
    s_LocalName = playerName;
    s_SessionId = GenerateSessionId();
    s_Uptime = 0.0f;
    s_SendAccum = 0.0f;
    s_LocalSeqNum = 0;

    s_RemotePeer = {};
    s_RemotePeer.ip = hostIp;
    s_RemotePeer.port = port;

    // Send handshake with level info
    HandshakeData hs{};
    strncpy(hs.playerName, playerName.c_str(), sizeof(hs.playerName) - 1);
    strncpy(hs.levelName, s_LocalLevel.c_str(), sizeof(hs.levelName) - 1);
    hs.sessionId = s_SessionId;

    int size = 0;
    BuildPacket(s_SendBuf, size, PacketType::Handshake, &hs, sizeof(hs));
    s_Socket.SendTo(s_SendBuf, size, hostIp, port);

    LOG_INFO("[Net] Joining {}:{} as '{}' (level: {})...", hostIp, port, playerName, s_LocalLevel);
    return true;
}

void NetDisconnect()
{
    if (s_Role == NetRole::None) return;

    if (s_RemotePeer.connected) {
        SendPacket(PacketType::Disconnect, nullptr, 0);
    }

    s_Socket.Close();
    s_Role = NetRole::None;
    s_RemotePeer = {};
    s_Uptime = 0.0f;
    LOG_INFO("[Net] Disconnected");
}

void NetTick(float deltaTime)
{
    if (s_Role == NetRole::None) return;

    s_Uptime += deltaTime;

    // Process incoming packets
    ProcessIncoming();

    // Check for timeout
    CheckTimeout();

    // Client: retry handshake if not connected yet
    if (s_Role == NetRole::Client && !s_RemotePeer.connected) {
        s_SendAccum += deltaTime;
        if (s_SendAccum >= 1.0f) { // retry every second
            s_SendAccum = 0.0f;
            HandshakeData hs{};
            strncpy(hs.playerName, s_LocalName.c_str(), sizeof(hs.playerName) - 1);
            strncpy(hs.levelName, s_LocalLevel.c_str(), sizeof(hs.levelName) - 1);
            hs.sessionId = s_SessionId;

            int size = 0;
            BuildPacket(s_SendBuf, size, PacketType::Handshake, &hs, sizeof(hs));
            s_Socket.SendTo(s_SendBuf, size, s_RemotePeer.ip, s_RemotePeer.port);
        }
    }

    // Send keepalive ping every 2 seconds to prevent timeout during alt-tab / pauses
    if (s_RemotePeer.connected) {
        s_PingAccum += deltaTime;
        if (s_PingAccum >= 2.0f) {
            s_PingAccum = 0.0f;
            SendPacket(PacketType::Ping, nullptr, 0);
        }
    }
}

void NetSendPlayerState(const PlayerStateData& state)
{
    if (!s_RemotePeer.connected) return;

    // Rate limit: only send at NET_TICK_RATE_HZ
    PlayerStateData out = state;
    out.sequenceNum = ++s_LocalSeqNum;

    SendPacket(PacketType::PlayerState, &out, sizeof(out));
    s_LastSentState = out;
}

void NetSendChat(const std::string& message)
{
    if (!s_RemotePeer.connected) return;

    ChatData chat{};
    strncpy(chat.message, message.c_str(), sizeof(chat.message) - 1);
    SendPacket(PacketType::Chat, &chat, sizeof(chat));
}

NetRole GetNetRole()
{
    return s_Role;
}

bool IsNetConnected()
{
    return s_RemotePeer.connected;
}

std::string GetNetStatus()
{
    switch (s_Role) {
    case NetRole::None:
        return "Network: Not connected";
    case NetRole::Host:
        if (s_RemotePeer.connected)
            return "Host: '" + s_RemotePeer.name + "' connected (" +
                   s_RemotePeer.ip + ":" + std::to_string(s_RemotePeer.port) +
                   ") seq=" + std::to_string(s_RemotePeer.lastSeqNum) +
                   " uptime=" + std::to_string((int)s_Uptime) + "s";
        return "Host: Waiting for player on port " + std::to_string(s_Socket.GetLocalPort());
    case NetRole::Client:
        if (s_RemotePeer.connected)
            return "Client: Connected to '" + s_RemotePeer.name + "' at " +
                   s_RemotePeer.ip + ":" + std::to_string(s_RemotePeer.port) +
                   " seq=" + std::to_string(s_RemotePeer.lastSeqNum) +
                   " uptime=" + std::to_string((int)s_Uptime) + "s";
        return "Client: Connecting to " + s_RemotePeer.ip + ":" + std::to_string(s_RemotePeer.port) + "...";
    }
    return "Unknown";
}

const NetPeer* GetRemotePeer()
{
    return s_RemotePeer.connected ? &s_RemotePeer : nullptr;
}

void SetOnRemoteState(OnRemoteStateFunc fn) { s_OnRemoteState = std::move(fn); }
void SetOnPeerEvent(OnPeerEventFunc fn) { s_OnPeerEvent = std::move(fn); }
void SetOnChat(OnChatFunc fn) { s_OnChat = std::move(fn); }

bool NetSendRawPacket(PacketType type, const void* data, uint16_t size)
{
    return SendPacket(type, data, size);
}

void NetSetLocalLevel(const std::string& levelName)
{
    s_LocalLevel = levelName;
}

std::string NetGetLocalLevel()
{
    return s_LocalLevel;
}

std::string NetGetRemoteLevel()
{
    return s_RemotePeer.levelName;
}

} // namespace bs1sdk
