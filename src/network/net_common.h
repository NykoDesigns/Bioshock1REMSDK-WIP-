#pragma once

#include <cstdint>
#include <cstring>

namespace bs1sdk {

// ─── Network Constants ─────────────────────────────────────────────────

constexpr uint16_t NET_DEFAULT_PORT = 27015;
constexpr uint32_t NET_PROTOCOL_MAGIC = 0x42533153; // "BS1S"
constexpr uint8_t  NET_PROTOCOL_VERSION = 1;
constexpr int      NET_MAX_PACKET = 1024;
constexpr int      NET_TICK_RATE_HZ = 30;           // state updates per second
constexpr float    NET_TICK_INTERVAL = 1.0f / NET_TICK_RATE_HZ;
constexpr float    NET_TIMEOUT_SEC = 5.0f;           // peer timeout

// ─── Packet Types ──────────────────────────────────────────────────────

enum class PacketType : uint8_t {
    Handshake    = 0x01,  // Initial connection
    HandshakeAck = 0x02,  // Connection accepted
    PlayerState  = 0x10,  // Position, rotation, health, weapon
    PlayerAction = 0x11,  // Discrete actions (fire, use, jump)
    Damage       = 0x20,  // Damage event forwarding
    WorldEvent   = 0x30,  // Door, trigger, pickup sync
    Chat         = 0x40,  // Text chat
    Ping         = 0xF0,  // Keepalive
    Pong         = 0xF1,  // Keepalive response
    Disconnect   = 0xFF,  // Clean disconnect
};

// ─── Packet Header ─────────────────────────────────────────────────────
// All packets start with this 8-byte header.

#pragma pack(push, 1)

struct PacketHeader {
    uint32_t   magic;      // NET_PROTOCOL_MAGIC
    uint8_t    version;    // NET_PROTOCOL_VERSION
    PacketType type;       // packet type
    uint16_t   size;       // payload size (after header)

    bool IsValid() const {
        return magic == NET_PROTOCOL_MAGIC && version == NET_PROTOCOL_VERSION;
    }
};
static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

// ─── Player State Packet ───────────────────────────────────────────────
// Sent at NET_TICK_RATE_HZ. Compact: 48 bytes payload.

struct PlayerStateData {
    float posX, posY, posZ;     // world position
    float rotPitch, rotYaw;     // view rotation (no roll)
    float health;               // current health
    float eve;                  // current EVE
    uint8_t  weaponId;          // current weapon index
    uint8_t  isCrouching;       // crouch state
    uint8_t  isFiring;          // fire state
    uint8_t  _pad;              // alignment
    uint32_t sequenceNum;       // monotonic counter for ordering
};
static_assert(sizeof(PlayerStateData) == 36, "PlayerStateData must be 36 bytes");

// ─── Handshake Packet ──────────────────────────────────────────────────

struct HandshakeData {
    char playerName[32];        // null-terminated player name
    uint32_t sessionId;         // random session identifier
};

// ─── Damage Packet ─────────────────────────────────────────────────────

struct DamageData {
    float amount;
    float hitX, hitY, hitZ;     // hit location
    uint8_t damageType;         // damage type enum
    uint8_t _pad[3];
};

// ─── Chat Packet ───────────────────────────────────────────────────────

struct ChatData {
    char message[256];          // null-terminated
};

#pragma pack(pop)

} // namespace bs1sdk
