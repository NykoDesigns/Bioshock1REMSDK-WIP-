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
constexpr float    NET_TIMEOUT_SEC = 15.0f;          // peer timeout (generous for VPN/Hamachi)

// ─── Packet Types ──────────────────────────────────────────────────────

enum class PacketType : uint8_t {
    Handshake    = 0x01,  // Initial connection
    HandshakeAck = 0x02,  // Connection accepted
    PlayerState  = 0x10,  // Position, rotation, health, weapon
    PlayerAction = 0x11,  // Discrete actions (fire, melee, plasmid)
    Damage       = 0x20,  // Damage event forwarding
    EnemyDeath   = 0x21,  // Enemy killed notification
    EnemyHPSync  = 0x22,  // Periodic enemy health snapshot
    PlayerDeath  = 0x23,  // Local player died
    PlayerRespawn= 0x24,  // Local player respawned
    WorldEvent   = 0x30,  // Door, trigger, pickup sync
    TriggerSync  = 0x31,  // Story/script trigger sync
    LevelSync    = 0x35,  // Level transition notification
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
    char levelName[32];         // current map (e.g. "1-Welcome")
    uint32_t sessionId;         // random session identifier
};

// ─── Damage Packet ─────────────────────────────────────────────────────

struct DamageData {
    float amount;
    float hitX, hitY, hitZ;     // hit location (for target matching)
    float targetX, targetY, targetZ; // target actor position (for fuzzy match)
    uint8_t damageType;         // damage type enum
    uint8_t _pad[3];
    uint32_t targetNameHash;    // FNV-1a hash of target actor name
};

// ─── World Event Packet ────────────────────────────────────────────────

struct WorldEventData {
    uint8_t  eventType;         // 0=door, 1=trigger, 2=pickup, 3=button
    uint8_t  state;             // new state (0=closed, 1=open, etc.)
    uint8_t  _pad[2];
    float    posX, posY, posZ;  // location of the actor (for matching)
    uint32_t actorNameHash;     // FNV-1a hash of actor name
};

// ─── Chat Packet ───────────────────────────────────────────────────────

struct ChatData {
    char message[256];          // null-terminated
};

// ─── Enemy Death Packet ────────────────────────────────────────────

struct EnemyDeathData {
    char     className[32];       // e.g. "MeleeThug"
    uint32_t nameHash;            // FNV-1a of full actor name
    float    posX, posY, posZ;    // location at time of death
};

// ─── Player Death Packet ───────────────────────────────────────────

struct PlayerDeathData {
    float posX, posY, posZ;       // where player died
    char  killerClass[32];        // what killed them
};

// ─── Player Respawn Packet ─────────────────────────────────────────

struct PlayerRespawnData {
    float posX, posY, posZ;       // where player respawned
};

// ─── Trigger Sync Packet ───────────────────────────────────────────

struct TriggerSyncData {
    uint32_t triggerNameHash;     // FNV-1a of trigger actor name
    float    posX, posY, posZ;    // trigger location
    uint8_t  state;               // 0=off, 1=on
    uint8_t  _pad[3];
};

// ─── Player Action Packet ─────────────────────────────────────────

enum class ActionType : uint8_t {
    MeleeSwing   = 0,   // Wrench or melee weapon swing
    WeaponFire   = 1,   // Gun fired
    PlasmidCast  = 2,   // Plasmid ability used
    Reload       = 3,   // Weapon reload
};

struct PlayerActionData {
    ActionType action;
    uint8_t    weaponId;          // which weapon/plasmid
    uint8_t    _pad[2];
    float      dirX, dirY, dirZ;  // aim direction at time of action
};

// ─── Enemy HP Sync Packet ─────────────────────────────────────────
// Batched: up to 8 enemies per packet for bandwidth efficiency.

struct EnemyHPEntry {
    uint32_t nameHash;            // FNV-1a of actor name
    float    posX, posY, posZ;    // position for matching
    float    health;              // current HP
};

struct EnemyHPSyncData {
    uint8_t      count;           // number of entries (1-8)
    uint8_t      _pad[3];
    EnemyHPEntry entries[8];
};

// ─── Level Sync Packet ──────────────────────────────────────────

struct LevelSyncData {
    char levelName[64];           // map name (e.g. "1-Medical")
    uint8_t isLoading;            // 1 = entering level, 0 = finished loading
    uint8_t _pad[3];
};

#pragma pack(pop)

} // namespace bs1sdk
