#pragma once

#include "net_common.h"
#include <string>
#include <vector>
#include <cstdint>

namespace bs1sdk {

// ─── True Co-op Roles ────────────────────────────────────────────────────

enum class TrueCoopRole {
    None,       // Not in a true co-op session
    TrueHost,   // Authority — runs full simulation, sends world state
    TrueClient  // Puppet — freezes local sim, receives world state
};

// ─── New Packet Types for True Co-op ─────────────────────────────────────
// These extend the existing PacketType enum conceptually.
// We use high byte values to avoid collision.

namespace TrueCoopPackets {
    constexpr uint8_t WorldStateBatch = 0x50; // Batched actor positions
    constexpr uint8_t ActorSpawn      = 0x51; // New actor appeared
    constexpr uint8_t ActorDestroy    = 0x52; // Actor removed/killed
    constexpr uint8_t AnimState       = 0x53; // Animation sync
    constexpr uint8_t P2Input         = 0x60; // Player 2 input from client
    constexpr uint8_t P2State         = 0x61; // Player 2 pawn state (host->client)
    constexpr uint8_t FreezeAck       = 0x70; // Client confirms sim frozen
    constexpr uint8_t WorldReady      = 0x71; // Host confirms world state is streaming
}

// ─── Per-Actor Sync Data ─────────────────────────────────────────────────

#pragma pack(push, 1)

struct ActorSyncData {
    uint32_t nameHash;              // FNV-1a of actor name
    float posX, posY, posZ;         // world position
    float rotPitch, rotYaw, rotRoll;// rotation
    float health;                   // current health (-1 = unchanged)
    uint8_t flags;                  // bit0=hidden, bit1=deleted, bit2=physics
    uint8_t _pad[3];
};

struct WorldStateBatchData {
    uint32_t frameNum;              // host frame counter
    uint8_t  actorCount;            // number of actors in this batch (max 30)
    uint8_t  batchIndex;            // batch N of M for this frame
    uint8_t  totalBatches;          // M
    uint8_t  _pad;
    ActorSyncData actors[30];       // 30 * 40 = 1200 bytes (fits UDP)
};

struct P2InputData {
    float moveForward, moveRight;   // -1 to 1
    float lookPitch, lookYaw;       // absolute angles
    uint16_t buttonMask;            // fire, altfire, jump, use, crouch, reload
    uint8_t weaponSlot;             // weapon switch
    uint8_t _pad;
    uint32_t sequenceNum;           // monotonic
};

struct ActorSpawnData {
    uint32_t nameHash;
    char className[32];
    char actorName[32];
    float posX, posY, posZ;
    float rotPitch, rotYaw, rotRoll;
};

struct ActorDestroyData {
    uint32_t nameHash;
    float posX, posY, posZ;         // for fuzzy matching
};

#pragma pack(pop)

// ─── Public API ──────────────────────────────────────────────────────────

/// Get the current true co-op role
TrueCoopRole GetTrueCoopRole();

/// Set the role (called when establishing session)
void SetTrueCoopRole(TrueCoopRole role);

/// Is the local instance the host (authority)?
bool IsTrueHost();

/// Is the local instance the client (puppet)?
bool IsTrueClient();

/// Initialize the true co-op system
bool InitTrueCoop();

/// Lazily initialize all co-op subsystems (safe to call multiple times)
void EnsureSubsystemsReady();

/// Shutdown and cleanup
void ShutdownTrueCoop();

/// Called every frame — handles world state broadcast (host) or application (client)
void TrueCoopTick(float deltaTime);

/// Get debug status string
std::string GetTrueCoopStatus();

// ─── Phase 1: Client Simulation Freeze ───────────────────────────────────

/// Freeze all AI, physics, and game logic on the client
bool FreezeClientSimulation();

/// Unfreeze (restore normal game operation)
void UnfreezeClientSimulation();

/// Is the simulation currently frozen?
bool IsSimulationFrozen();

/// Get list of functions being blocked (for debug display)
std::vector<std::string> GetFrozenFunctions();

} // namespace bs1sdk
