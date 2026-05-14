#pragma once

#include "coop_true.h"
#include "../engine/uobject.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace bs1sdk {

// ─── Actor Registry ──────────────────────────────────────────────────────
// Host builds this every tick — tracks all actors to sync.

struct SyncedActor {
    UObject* Ptr = nullptr;
    uint32_t NameHash = 0;
    std::string Name;
    std::string ClassName;
    float PosX = 0, PosY = 0, PosZ = 0;
    float RotPitch = 0, RotYaw = 0, RotRoll = 0;
    float Health = -1;
    uint8_t Flags = 0;          // bit0=hidden, bit1=deleted, bit2=physics
    bool Dirty = false;         // changed since last send
    float LastSendTime = 0;
    float Priority = 1.0f;      // higher = send more often
};

// ─── Interpolation Buffer (Client) ──────────────────────────────────────
// Client stores received states and interpolates between them.

struct InterpState {
    float PosX, PosY, PosZ;
    float RotPitch, RotYaw, RotRoll;
    float Health;
    float Timestamp;            // when received (local time)
};

struct ClientActor {
    uint32_t NameHash = 0;
    std::string Name;
    std::string ClassName;
    UObject* LocalPtr = nullptr;    // matched local actor
    InterpState States[3];          // ring buffer for interpolation
    int StateHead = 0;
    int StateCount = 0;
    float LastUpdateTime = 0;
    bool Matched = false;           // found local actor to puppet
};

// ─── Configuration ───────────────────────────────────────────────────────

struct WorldSyncConfig {
    float HostSendRate = 10.0f;     // Hz for world state batches
    float MaxSyncRadius = 10000.0f; // only sync actors within this range
    float InterpDelay = 0.1f;       // 100ms interpolation buffer
    int MaxActorsPerTick = 200;     // cap to avoid frame drops
    bool DeltaCompression = true;   // only send changed actors
    float MoveThreshold = 5.0f;     // units — below this, don't mark dirty
};

// ─── Public API ──────────────────────────────────────────────────────────

/// Initialize the world sync system
void InitWorldSync();
void ShutdownWorldSync();

/// Called every frame by TrueCoopTick
void WorldSyncHostTick(float deltaTime);
void WorldSyncClientTick(float deltaTime);

/// Get sync stats for debugging
struct WorldSyncStats {
    int TrackedActors = 0;
    int DirtyActors = 0;
    int PacketsSentThisSec = 0;
    int PacketsRecvThisSec = 0;
    int BytesSentThisSec = 0;
    int BytesRecvThisSec = 0;
    int ClientMatchedActors = 0;
    int ClientUnmatchedActors = 0;
    float LastSendTime = 0;
    float LastRecvTime = 0;
    uint32_t HostFrame = 0;
};
WorldSyncStats GetWorldSyncStats();

/// Get/set config
WorldSyncConfig& GetWorldSyncConfig();

/// Process incoming world state packet (called by net_manager)
void HandleWorldStateBatch(const WorldStateBatchData& batch);
void HandleActorSpawn(const ActorSpawnData& data);
void HandleActorDestroy(const ActorDestroyData& data);

/// Dump current sync state to file
void DumpWorldSyncState();

} // namespace bs1sdk
