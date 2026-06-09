#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Editor Bridge Client — connects from BS1LevelEditor to the running game.
// Sends actor move/rotate commands, receives player position updates.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <functional>
#include <vector>

// Protocol constants (must match editor_bridge.h in the SDK)
constexpr uint32_t BRIDGE_MAGIC  = 0x44453142;  // "B1ED"
constexpr uint16_t BRIDGE_PORT   = 19760;

constexpr uint16_t MSG_MOVE_ACTOR   = 0x01;
constexpr uint16_t MSG_ROTATE_ACTOR = 0x02;
constexpr uint16_t MSG_SELECT_ACTOR = 0x03;
constexpr uint16_t MSG_QUERY_ACTORS = 0x04;
constexpr uint16_t MSG_QUERY_PLAYER = 0x05;
constexpr uint16_t MSG_PING         = 0x10;

constexpr uint16_t MSG_ACTOR_POS    = 0x81;
constexpr uint16_t MSG_PLAYER_POS   = 0x82;
constexpr uint16_t MSG_ACTOR_LIST   = 0x83;
constexpr uint16_t MSG_PONG         = 0x90;

#pragma pack(push, 1)
struct BridgeHeader {
    uint32_t magic;
    uint16_t type;
    uint16_t payloadLen;
};
#pragma pack(pop)

struct PlayerPosUpdate {
    float x, y, z;
    int32_t pitch, yaw, roll;
};

class EditorBridgeClient {
public:
    EditorBridgeClient() = default;
    ~EditorBridgeClient();

    // Connect to the game's bridge server. Returns true on success.
    bool Connect(const std::string& host = "127.0.0.1", uint16_t port = BRIDGE_PORT);

    // Disconnect from the game.
    void Disconnect();

    // Check if connected.
    bool IsConnected() const { return m_Connected; }

    // Poll for incoming messages (call once per frame). Non-blocking.
    void Poll();

    // Send commands to the game
    bool SendMoveActor(uint32_t nameHash, float x, float y, float z);
    bool SendRotateActor(uint32_t nameHash, int32_t pitch, int32_t yaw, int32_t roll);
    bool SendSelectActor(uint32_t nameHash);
    bool SendQueryActors();
    bool SendQueryPlayer();
    bool SendPing();

    // Last known player position (updated by Poll)
    bool HasPlayerPos() const { return m_HasPlayerPos; }
    PlayerPosUpdate GetPlayerPos() const { return m_PlayerPos; }

    // Connection stats
    int GetMessagesSent() const { return m_Sent; }
    int GetMessagesReceived() const { return m_Received; }
    float GetLatencyMs() const { return m_LatencyMs; }

    // Hash helper (FNV-1a, must match game side)
    static uint32_t HashName(const std::string& name);

private:
    bool SendRaw(uint16_t type, const void* payload, uint16_t len);

    uintptr_t m_Socket = ~(uintptr_t)0;  // INVALID_SOCKET
    bool m_Connected = false;
    bool m_HasPlayerPos = false;
    PlayerPosUpdate m_PlayerPos = {};
    int m_Sent = 0;
    int m_Received = 0;
    float m_LatencyMs = 0.0f;
    double m_PingSentTime = 0.0;
};
