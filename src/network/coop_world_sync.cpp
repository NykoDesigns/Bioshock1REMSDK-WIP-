#include "coop_world_sync.h"
#include "net_manager.h"
#include "coop_true.h"
#include "../core/log.h"
#include "../engine/world.h"
#include "../debug/coop_debug.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <fstream>
#include <chrono>
#include <mutex>

namespace bs1sdk {

// ─── FNV-1a Hash ─────────────────────────────────────────────────────────

static uint32_t FNV1a(const std::string& str)
{
    uint32_t hash = 0x811c9dc5;
    for (char c : str) {
        hash ^= (uint8_t)c;
        hash *= 0x01000193;
    }
    return hash;
}

// ─── State ───────────────────────────────────────────────────────────────

static WorldSyncConfig s_Config;
static WorldSyncStats s_Stats;
static std::mutex s_Mutex;
static bool s_Initialized = false;

// Host state
static std::map<uint32_t, SyncedActor> s_HostRegistry;
static float s_HostSendAccum = 0.0f;
static float s_HostTime = 0.0f;

// Client state
static std::map<uint32_t, ClientActor> s_ClientActors;
static float s_ClientTime = 0.0f;

// Bandwidth tracking
static int s_PacketsSent = 0, s_PacketsRecv = 0;
static int s_BytesSent = 0, s_BytesRecv = 0;
static float s_BandwidthAccum = 0.0f;

// ─── Logging ─────────────────────────────────────────────────────────────

static std::ofstream s_SyncLog;

static void SyncLog(const char* fmt, ...)
{
    if (!s_SyncLog.is_open()) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_SyncLog << "[" << std::fixed;
    s_SyncLog.precision(2);
    s_SyncLog << s_HostTime << "] " << buf << "\n";
    s_SyncLog.flush();
}

// ─── Init / Shutdown ─────────────────────────────────────────────────────

void InitWorldSync()
{
    if (s_Initialized) return;
    s_Initialized = true;

    std::string logPath = std::string(GetDebugDir()) + "/world_sync_log.txt";
    s_SyncLog.open(logPath, std::ios::trunc);
    SyncLog("WorldSync initialized. Role=%s", IsTrueHost() ? "Host" : "Client");

    s_HostRegistry.clear();
    s_ClientActors.clear();
    s_Stats = {};

    LOG_INFO("[WorldSync] Initialized");
}

void ShutdownWorldSync()
{
    if (!s_Initialized) return;
    SyncLog("WorldSync shutdown. Tracked=%d", (int)s_HostRegistry.size());
    s_SyncLog.close();
    s_Initialized = false;
}

WorldSyncConfig& GetWorldSyncConfig() { return s_Config; }
WorldSyncStats GetWorldSyncStats() { return s_Stats; }

// ─── Host: Build Actor Registry ──────────────────────────────────────────

static void HostBuildRegistry()
{
    if (!IsWorldSystemReady()) return;

    // Get player position for distance culling
    FVec3 playerPos;
    bool hasPlayer = GetPlayerPosition(playerPos);

    auto actors = GetAllActors();
    int tracked = 0;

    for (auto* actor : actors) {
        if (!actor) continue;
        if (tracked >= s_Config.MaxActorsPerTick) break;

        std::string name = actor->GetName();
        std::string cn = actor->GetObjClassName();
        uint32_t hash = FNV1a(name);

        // Skip non-gameplay actors (lights, volumes, nav points, emitters)
        if (cn == "Light" || cn == "PointLight" || cn == "SpotLight" ||
            cn == "DirectionalLight" || cn == "NavigationPoint" ||
            cn == "PathNode" || cn == "Volume" || cn == "PhysicsVolume" ||
            cn == "BlockingVolume" || cn == "Emitter" || cn == "Sound" ||
            cn == "StaticMeshActor" || cn == "Brush") {
            continue;
        }

        FVec3 pos;
        if (!GetActorPosition(actor, pos)) continue;

        // Distance cull
        if (hasPlayer) {
            float dx = pos.X - playerPos.X;
            float dy = pos.Y - playerPos.Y;
            float dz = pos.Z - playerPos.Z;
            float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist > s_Config.MaxSyncRadius) continue;
        }

        // Update or insert
        auto& sa = s_HostRegistry[hash];
        if (sa.NameHash == 0) {
            // New actor
            sa.Ptr = actor;
            sa.NameHash = hash;
            sa.Name = name;
            sa.ClassName = cn;
            sa.PosX = pos.X; sa.PosY = pos.Y; sa.PosZ = pos.Z;
            sa.Dirty = true;
            SyncLog("NEW ACTOR: [%s] %s hash=0x%08X pos=(%.0f,%.0f,%.0f)",
                    cn.c_str(), name.c_str(), hash, pos.X, pos.Y, pos.Z);
        } else {
            // Check if moved
            float dx = pos.X - sa.PosX;
            float dy = pos.Y - sa.PosY;
            float dz = pos.Z - sa.PosZ;
            float moved = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (moved > s_Config.MoveThreshold || !s_Config.DeltaCompression) {
                sa.Dirty = true;
            }
            sa.PosX = pos.X; sa.PosY = pos.Y; sa.PosZ = pos.Z;
            sa.Ptr = actor;
        }

        // Read rotation + mark dirty if rotated
        FRot3 rot{};
        if (GetActorRotation(actor, rot)) {
            float newPitch = rot.Pitch * (360.0f / 65536.0f);
            float newYaw = rot.Yaw * (360.0f / 65536.0f);
            float newRoll = rot.Roll * (360.0f / 65536.0f);
            float dYaw = std::abs(newYaw - sa.RotYaw);
            if (dYaw > 2.0f) sa.Dirty = true; // rotated > 2 degrees
            sa.RotPitch = newPitch;
            sa.RotYaw = newYaw;
            sa.RotRoll = newRoll;
        }

        // Read health from Pawn-like actors
        sa.Health = -1;
        if (cn.find("Pawn") != std::string::npos ||
            cn.find("Splicer") != std::string::npos ||
            cn.find("BigDaddy") != std::string::npos ||
            cn.find("Aggressor") != std::string::npos ||
            cn.find("Fighter") != std::string::npos ||
            cn.find("Security") != std::string::npos) {
            float hp = 0;
            if (GetActorProperty(actor, "Health", &hp, sizeof(hp))) {
                sa.Health = hp;
            }
        }

        tracked++;
    }

    s_Stats.TrackedActors = tracked;
    s_Stats.DirtyActors = 0;
    for (auto& [h, sa] : s_HostRegistry) {
        if (sa.Dirty) s_Stats.DirtyActors++;
    }
}

// ─── Host: Send World State Batches ──────────────────────────────────────

static void HostSendBatches()
{
    // Collect dirty actors, sorted by priority
    std::vector<SyncedActor*> dirty;
    for (auto& [hash, sa] : s_HostRegistry) {
        if (sa.Dirty) dirty.push_back(&sa);
    }

    if (dirty.empty()) return;

    // Sort by priority (higher first)
    std::sort(dirty.begin(), dirty.end(),
              [](const SyncedActor* a, const SyncedActor* b) { return a->Priority > b->Priority; });

    // Pack into batches of 30
    int totalBatches = ((int)dirty.size() + 29) / 30;
    int batchIdx = 0;

    for (int i = 0; i < (int)dirty.size(); i += 30) {
        WorldStateBatchData batch{};
        batch.frameNum = s_Stats.HostFrame;
        batch.batchIndex = (uint8_t)batchIdx;
        batch.totalBatches = (uint8_t)totalBatches;

        int count = std::min(30, (int)dirty.size() - i);
        batch.actorCount = (uint8_t)count;

        for (int j = 0; j < count; j++) {
            auto* sa = dirty[i + j];
            auto& ad = batch.actors[j];
            ad.nameHash = sa->NameHash;
            ad.posX = sa->PosX;
            ad.posY = sa->PosY;
            ad.posZ = sa->PosZ;
            ad.rotPitch = sa->RotPitch;
            ad.rotYaw = sa->RotYaw;
            ad.rotRoll = sa->RotRoll;
            ad.health = sa->Health;
            ad.flags = sa->Flags;

            sa->Dirty = false;
            sa->LastSendTime = s_HostTime;
        }

        // Send via network (payload only — header added by SendPacket)
        uint16_t payloadSize = (uint16_t)(8 + count * sizeof(ActorSyncData));
        NetSendRawPacket((PacketType)TrueCoopPackets::WorldStateBatch,
                         &batch, payloadSize);

        s_PacketsSent++;
        s_BytesSent += 8 + count * sizeof(ActorSyncData);
        batchIdx++;
    }

    s_Stats.HostFrame++;
    s_Stats.LastSendTime = s_HostTime;
}

// ─── Host Tick ───────────────────────────────────────────────────────────

void WorldSyncHostTick(float deltaTime)
{
    if (!s_Initialized || !IsTrueHost()) return;

    s_HostTime += deltaTime;

    // Bandwidth tracking
    s_BandwidthAccum += deltaTime;
    if (s_BandwidthAccum >= 1.0f) {
        s_Stats.PacketsSentThisSec = s_PacketsSent;
        s_Stats.BytesSentThisSec = s_BytesSent;
        s_Stats.PacketsRecvThisSec = s_PacketsRecv;
        s_Stats.BytesRecvThisSec = s_BytesRecv;
        s_PacketsSent = s_PacketsRecv = s_BytesSent = s_BytesRecv = 0;
        s_BandwidthAccum = 0;
    }

    // Build registry every frame
    HostBuildRegistry();

    // Send at configured rate
    s_HostSendAccum += deltaTime;
    float sendInterval = 1.0f / s_Config.HostSendRate;
    if (s_HostSendAccum >= sendInterval) {
        s_HostSendAccum -= sendInterval;
        HostSendBatches();
    }
}

// ─── Client: Handle Incoming Packets ─────────────────────────────────────

void HandleWorldStateBatch(const WorldStateBatchData& batch)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_PacketsRecv++;
    s_Stats.LastRecvTime = s_ClientTime;
    s_Stats.HostFrame = batch.frameNum;

    for (int i = 0; i < batch.actorCount && i < 30; i++) {
        const auto& ad = batch.actors[i];

        auto& ca = s_ClientActors[ad.nameHash];
        if (ca.NameHash == 0) {
            ca.NameHash = ad.nameHash;
            ca.Matched = false;
        }

        // Store in interpolation buffer
        int idx = ca.StateHead;
        ca.States[idx].PosX = ad.posX;
        ca.States[idx].PosY = ad.posY;
        ca.States[idx].PosZ = ad.posZ;
        ca.States[idx].RotPitch = ad.rotPitch;
        ca.States[idx].RotYaw = ad.rotYaw;
        ca.States[idx].RotRoll = ad.rotRoll;
        ca.States[idx].Health = ad.health;
        ca.States[idx].Timestamp = s_ClientTime;

        ca.StateHead = (ca.StateHead + 1) % 3;
        if (ca.StateCount < 3) ca.StateCount++;
        ca.LastUpdateTime = s_ClientTime;
    }
}

void HandleActorSpawn(const ActorSpawnData& data)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    SyncLog("SPAWN received: [%s] %s hash=0x%08X", data.className, data.actorName, data.nameHash);

    auto& ca = s_ClientActors[data.nameHash];
    ca.NameHash = data.nameHash;
    ca.Name = data.actorName;
    ca.ClassName = data.className;
    ca.Matched = false; // will try to match next tick
}

void HandleActorDestroy(const ActorDestroyData& data)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    SyncLog("DESTROY received: hash=0x%08X", data.nameHash);
    s_ClientActors.erase(data.nameHash);
}

// ─── Client: Match + Interpolate + Apply ─────────────────────────────────

static void ClientMatchActors()
{
    if (!IsWorldSystemReady()) return;

    auto localActors = GetAllActors();
    std::map<uint32_t, UObject*> localByHash;
    for (auto* a : localActors) {
        if (!a) continue;
        uint32_t h = FNV1a(a->GetName());
        localByHash[h] = a;
    }

    int matched = 0, unmatched = 0;
    for (auto& [hash, ca] : s_ClientActors) {
        auto it = localByHash.find(hash);
        if (it != localByHash.end()) {
            ca.LocalPtr = it->second;
            ca.Matched = true;
            matched++;
        } else {
            ca.Matched = false;
            unmatched++;
        }
    }

    s_Stats.ClientMatchedActors = matched;
    s_Stats.ClientUnmatchedActors = unmatched;
}

static void ClientInterpolateAndApply()
{
    float interpTime = s_ClientTime - s_Config.InterpDelay;

    for (auto& [hash, ca] : s_ClientActors) {
        if (!ca.Matched || !ca.LocalPtr) continue;
        if (ca.StateCount == 0) continue;

        // Use latest state (simple snap for now, interpolation can be refined)
        int latest = (ca.StateHead + 2) % 3; // most recent
        if (ca.StateCount < 3) latest = (ca.StateHead + 3 - 1) % 3;

        InterpState& state = ca.States[latest];

        // Apply position to local actor (raw memcpy — safe for non-static actors)
        FVec3 newPos = {state.PosX, state.PosY, state.PosZ};
        SetActorPosition(ca.LocalPtr, newPos);

        // Apply rotation
        if (state.RotPitch != 0 || state.RotYaw != 0 || state.RotRoll != 0) {
            FRot3 newRot;
            newRot.Pitch = (int32_t)(state.RotPitch * (65536.0f / 360.0f));
            newRot.Yaw = (int32_t)(state.RotYaw * (65536.0f / 360.0f));
            newRot.Roll = (int32_t)(state.RotRoll * (65536.0f / 360.0f));
            SetActorRotation(ca.LocalPtr, newRot);
        }
    }
}

// ─── Client Tick ─────────────────────────────────────────────────────────

void WorldSyncClientTick(float deltaTime)
{
    if (!s_Initialized || !IsTrueClient()) return;

    s_ClientTime += deltaTime;

    // Bandwidth tracking
    s_BandwidthAccum += deltaTime;
    if (s_BandwidthAccum >= 1.0f) {
        s_Stats.PacketsRecvThisSec = s_PacketsRecv;
        s_Stats.BytesRecvThisSec = s_BytesRecv;
        s_PacketsRecv = s_BytesRecv = 0;
        s_BandwidthAccum = 0;
    }

    // Re-match actors periodically (every 2 seconds)
    static float matchAccum = 0;
    matchAccum += deltaTime;
    if (matchAccum >= 2.0f) {
        matchAccum = 0;
        std::lock_guard<std::mutex> lock(s_Mutex);
        ClientMatchActors();
    }

    // Apply interpolated states every frame
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        ClientInterpolateAndApply();
    }
}

// ─── Debug Dump ──────────────────────────────────────────────────────────

void DumpWorldSyncState()
{
    std::string filepath = std::string(GetDebugDir()) + "/world_sync_state.txt";
    std::ofstream out(filepath);
    out << "=== World Sync State ===\n";
    out << "Role: " << (IsTrueHost() ? "Host" : "Client") << "\n";
    out << "Host frame: " << s_Stats.HostFrame << "\n";
    out << "Tracked actors: " << s_Stats.TrackedActors << "\n";
    out << "Dirty actors: " << s_Stats.DirtyActors << "\n";
    out << "Packets/sec sent: " << s_Stats.PacketsSentThisSec << "\n";
    out << "Packets/sec recv: " << s_Stats.PacketsRecvThisSec << "\n";
    out << "Bytes/sec sent: " << s_Stats.BytesSentThisSec << "\n";
    out << "Bytes/sec recv: " << s_Stats.BytesRecvThisSec << "\n";
    out << "Client matched: " << s_Stats.ClientMatchedActors << "\n";
    out << "Client unmatched: " << s_Stats.ClientUnmatchedActors << "\n\n";

    if (IsTrueHost()) {
        out << "─── Host Registry (top 50) ───\n";
        int shown = 0;
        for (auto& [hash, sa] : s_HostRegistry) {
            if (shown++ >= 50) break;
            char line[256];
            std::snprintf(line, sizeof(line),
                "  0x%08X [%s] %s  pos=(%.0f,%.0f,%.0f) dirty=%d\n",
                hash, sa.ClassName.c_str(), sa.Name.c_str(),
                sa.PosX, sa.PosY, sa.PosZ, sa.Dirty);
            out << line;
        }
    } else {
        out << "─── Client Actor Map (top 50) ───\n";
        int shown = 0;
        for (auto& [hash, ca] : s_ClientActors) {
            if (shown++ >= 50) break;
            char line[256];
            std::snprintf(line, sizeof(line),
                "  0x%08X [%s] %s  matched=%d states=%d\n",
                hash, ca.ClassName.c_str(), ca.Name.c_str(),
                ca.Matched, ca.StateCount);
            out << line;
        }
    }

    out.close();
    LOG_INFO("[WorldSync] State dumped -> {}", filepath);
}

} // namespace bs1sdk
