#pragma once

#include "net_common.h"
#include <string>
#include <functional>

namespace bs1sdk {

/// Network role
enum class NetRole { None, Host, Client };

/// Remote peer info
struct NetPeer {
    std::string ip;
    uint16_t    port = 0;
    std::string name;
    std::string levelName;              // remote player's current map
    uint32_t    sessionId = 0;
    float       lastRecvTime = 0.0f;   // seconds since connection
    PlayerStateData lastState{};        // most recent state from this peer
    uint32_t    lastSeqNum = 0;         // for ordering
    bool        connected = false;
};

/// Callback when remote player state is received
using OnRemoteStateFunc = std::function<void(const NetPeer& peer, const PlayerStateData& state)>;

/// Callback when a peer connects/disconnects
using OnPeerEventFunc = std::function<void(const NetPeer& peer, bool connected)>;

// ─── Public API ────────────────────────────────────────────────────────

/// Start hosting a co-op session on the given port.
bool NetHost(uint16_t port = NET_DEFAULT_PORT, const std::string& playerName = "Host");

/// Join a co-op session at the given address.
bool NetJoin(const std::string& hostIp, uint16_t port = NET_DEFAULT_PORT,
             const std::string& playerName = "Client");

/// Disconnect and clean up.
void NetDisconnect();

/// Call every frame — processes incoming packets, sends outgoing state.
void NetTick(float deltaTime);

/// Send local player state to the remote peer.
void NetSendPlayerState(const PlayerStateData& state);

/// Send a chat message.
void NetSendChat(const std::string& message);

/// Get current network status.
NetRole GetNetRole();
bool IsNetConnected();
std::string GetNetStatus();
const NetPeer* GetRemotePeer();

/// Register callbacks.
void SetOnRemoteState(OnRemoteStateFunc fn);
void SetOnPeerEvent(OnPeerEventFunc fn);

/// Level tracking — set before host/join for mismatch detection.
void NetSetLocalLevel(const std::string& levelName);
std::string NetGetLocalLevel();
std::string NetGetRemoteLevel();

/// Raw packet sending (used by coop_sync).
bool NetSendRawPacket(PacketType type, const void* data, uint16_t size);

} // namespace bs1sdk
