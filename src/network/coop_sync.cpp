#include "coop_sync.h"
#include "net_manager.h"
#include "net_common.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../hooks/process_event.h"

#include <cstring>
#include <cmath>
#include <string>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static bool s_SyncInitialized = false;
static int  s_DamageHookId = -1;
static int  s_WorldHookId = -1;

// Property offsets (shared with coop_bridge via extern or re-cached)
static int s_SyncLocOffset = -1;

// Flag to prevent echo (don't re-send damage we received from remote)
static bool s_ApplyingRemoteDamage = false;

// ─── Helpers ───────────────────────────────────────────────────────────

/// FNV-1a 32-bit hash for actor name matching across instances.
static uint32_t FnvHash(const std::string& str)
{
    uint32_t hash = 0x811c9dc5;
    for (char c : str) {
        hash ^= (uint8_t)c;
        hash *= 0x01000193;
    }
    return hash;
}

/// Find nearest actor by name hash and position (fuzzy match).
static UObject* FindActorByHashAndPos(uint32_t nameHash, float tx, float ty, float tz, float tolerance = 200.0f)
{
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return nullptr;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    UObject* bestMatch = nullptr;
    float bestDist = tolerance;

    for (int i = 0; i < objCount && i < 100000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);

        // Quick name hash check
        std::string name = obj->GetName();
        if (FnvHash(name) != nameHash) continue;

        // Position check if we have Location offset
        if (s_SyncLocOffset > 0) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
            float ox, oy, oz;
            memcpy(&ox, raw + s_SyncLocOffset, 4);
            memcpy(&oy, raw + s_SyncLocOffset + 4, 4);
            memcpy(&oz, raw + s_SyncLocOffset + 8, 4);

            float dx = ox - tx, dy = oy - ty, dz = oz - tz;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < bestDist) {
                bestDist = dist;
                bestMatch = obj;
            }
        } else {
            // No position check available, return first name match
            return obj;
        }
    }

    return bestMatch;
}

// ─── Damage Sync ───────────────────────────────────────────────────────

/// Apply damage from remote peer to a local actor.
static void ApplyRemoteDamage(const DamageData& dmg)
{
    UObject* target = FindActorByHashAndPos(dmg.targetNameHash,
                                            dmg.targetX, dmg.targetY, dmg.targetZ);
    if (!target) return;

    // Find the TakeDamage function on the target
    UStruct* cls = reinterpret_cast<UStruct*>(target->GetClass());
    if (!cls) return;

    // We'll directly modify the Health property as a fallback
    // since calling TakeDamage via ProcessEvent requires parameter struct knowledge
    std::vector<PropertyInfo> props = WalkProperties(cls);
    PropertyInfo* healthPi = FindProperty(cls, "Health", props);
    if (!healthPi) return;

    float currentHealth;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(target);
    memcpy(&currentHealth, raw + healthPi->Offset, 4);

    float newHealth = currentHealth - dmg.amount;
    if (newHealth < 0.0f) newHealth = 0.0f;

    // Apply
    s_ApplyingRemoteDamage = true;
    uint8_t* rawW = reinterpret_cast<uint8_t*>(target);
    memcpy(rawW + healthPi->Offset, &newHealth, 4);
    s_ApplyingRemoteDamage = false;

    LOG_INFO("[Co-op Sync] Applied {:.0f} damage to '{}' (HP: {:.0f} -> {:.0f})",
             dmg.amount, target->GetName(), currentHealth, newHealth);
}

/// Apply world event from remote peer.
static void ApplyRemoteWorldEvent(const WorldEventData& evt)
{
    UObject* actor = FindActorByHashAndPos(evt.actorNameHash,
                                           evt.posX, evt.posY, evt.posZ, 100.0f);
    if (!actor) return;

    // For doors/triggers, we set their state by calling Trigger or toggling bOpen
    UStruct* cls = reinterpret_cast<UStruct*>(actor->GetClass());
    if (!cls) return;

    std::vector<PropertyInfo> props = WalkProperties(cls);

    switch (evt.eventType) {
    case 0: { // Door
        PropertyInfo* pi = FindProperty(cls, "bOpening", props);
        if (!pi) pi = FindProperty(cls, "bOpen", props);
        if (pi) {
            uint8_t* raw = reinterpret_cast<uint8_t*>(actor);
            int32_t state = evt.state;
            memcpy(raw + pi->Offset, &state, 4);
            LOG_INFO("[Co-op Sync] Door '{}' -> state {}", actor->GetName(), evt.state);
        }
        break;
    }
    case 2: { // Pickup — hide/destroy
        PropertyInfo* pi = FindProperty(cls, "bHidden", props);
        if (pi) {
            uint8_t* raw = reinterpret_cast<uint8_t*>(actor);
            int32_t hidden = 1;
            memcpy(raw + pi->Offset, &hidden, 4);
            LOG_INFO("[Co-op Sync] Pickup '{}' collected by remote", actor->GetName());
        }
        break;
    }
    default:
        break;
    }
}

// ─── ProcessEvent Hooks ────────────────────────────────────────────────

static void InstallDamageHook()
{
    ProcessEventHook hook;
    hook.Name = "CoopDamageSync";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        if (s_ApplyingRemoteDamage) return false; // don't echo

        std::string funcName = func->GetName();
        if (funcName != "TakeDamage") return false;

        // Only sync damage to NPCs (ShockPawn, enemies)
        std::string cn = obj->GetObjClassName();
        if (cn == "ShockPlayer" || cn == "ShockPlayerController") return false;

        // Extract damage amount from TakeDamage params
        // TakeDamage(int Damage, Pawn InstigatedBy, vector HitLocation, vector Momentum, class DamageType)
        // In UE2, params are packed sequentially
        if (!parms) return false;

        // Read the first int (damage amount)
        int32_t damage;
        memcpy(&damage, parms, 4);

        if (damage <= 0) return false;

        // Get target position for matching
        float tx = 0, ty = 0, tz = 0;
        if (s_SyncLocOffset > 0) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
            memcpy(&tx, raw + s_SyncLocOffset, 4);
            memcpy(&ty, raw + s_SyncLocOffset + 4, 4);
            memcpy(&tz, raw + s_SyncLocOffset + 8, 4);
        }

        // Send to remote
        std::string name = obj->GetName();
        CoopSendDamage((float)damage, tx, ty, tz, tx, ty, tz,
                       FnvHash(name), 0);

        return false; // don't block
    };
    s_DamageHookId = RegisterProcessEventHook(hook);
}

static void InstallWorldHook()
{
    ProcessEventHook hook;
    hook.Name = "CoopWorldSync";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        std::string funcName = func->GetName();

        // Detect door/mover interactions
        if (funcName == "Open" || funcName == "Close") {
            std::string cn = obj->GetObjClassName();
            if (cn.find("Door") != std::string::npos || cn.find("Mover") != std::string::npos) {
                float px = 0, py = 0, pz = 0;
                if (s_SyncLocOffset > 0) {
                    const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                    memcpy(&px, raw + s_SyncLocOffset, 4);
                    memcpy(&py, raw + s_SyncLocOffset + 4, 4);
                    memcpy(&pz, raw + s_SyncLocOffset + 8, 4);
                }
                uint8_t state = (funcName == "Open") ? 1 : 0;
                CoopSendWorldEvent(0, state, px, py, pz, FnvHash(obj->GetName()));
            }
        }

        // Detect pickups
        if (funcName == "PickedUp" || funcName == "Touch") {
            std::string cn = obj->GetObjClassName();
            if (cn.find("Pickup") != std::string::npos || cn.find("Ammo") != std::string::npos ||
                cn.find("Health") != std::string::npos) {
                float px = 0, py = 0, pz = 0;
                if (s_SyncLocOffset > 0) {
                    const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                    memcpy(&px, raw + s_SyncLocOffset, 4);
                    memcpy(&py, raw + s_SyncLocOffset + 4, 4);
                    memcpy(&pz, raw + s_SyncLocOffset + 8, 4);
                }
                CoopSendWorldEvent(2, 1, px, py, pz, FnvHash(obj->GetName()));
            }
        }

        return false;
    };
    s_WorldHookId = RegisterProcessEventHook(hook);
}

// ─── Net Manager Packet Handling ───────────────────────────────────────
// We need to handle Damage and WorldEvent packets in the network manager.
// For now, we'll add processing in the coop_bridge tick via a dedicated handler.

// Store pending packets for processing
static DamageData s_PendingDamage[16];
static int s_PendingDamageCount = 0;
static WorldEventData s_PendingWorldEvents[16];
static int s_PendingWorldEventCount = 0;

void QueueDamagePacket(const DamageData& dmg)
{
    if (s_PendingDamageCount < 16) {
        s_PendingDamage[s_PendingDamageCount++] = dmg;
    }
}

void QueueWorldEventPacket(const WorldEventData& evt)
{
    if (s_PendingWorldEventCount < 16) {
        s_PendingWorldEvents[s_PendingWorldEventCount++] = evt;
    }
}

// ─── Public API ────────────────────────────────────────────────────────

bool InitCoopSync()
{
    if (s_SyncInitialized) return true;

    // Cache Location offset
    const auto& globals = GetEngineGlobals();
    if (globals.IsValid()) {
        UStruct* cls = FindClass("ShockPawn");
        if (!cls) cls = FindClass("Pawn");
        if (cls) {
            std::vector<PropertyInfo> props = WalkProperties(cls);
            PropertyInfo* pi = FindProperty(cls, "Location", props);
            if (pi) s_SyncLocOffset = pi->Offset;
        }
    }

    InstallDamageHook();
    InstallWorldHook();

    s_SyncInitialized = true;
    LOG_INFO("[Co-op Sync] Damage + world state sync initialized (LocOffset={})", s_SyncLocOffset);
    return true;
}

void ShutdownCoopSync()
{
    if (!s_SyncInitialized) return;
    if (s_DamageHookId >= 0) UnregisterProcessEventHook(s_DamageHookId);
    if (s_WorldHookId >= 0) UnregisterProcessEventHook(s_WorldHookId);
    s_DamageHookId = -1;
    s_WorldHookId = -1;
    s_SyncInitialized = false;
}

void CoopSyncProcessPackets()
{
    // Process pending damage
    for (int i = 0; i < s_PendingDamageCount; i++) {
        ApplyRemoteDamage(s_PendingDamage[i]);
    }
    s_PendingDamageCount = 0;

    // Process pending world events
    for (int i = 0; i < s_PendingWorldEventCount; i++) {
        ApplyRemoteWorldEvent(s_PendingWorldEvents[i]);
    }
    s_PendingWorldEventCount = 0;
}

void CoopSendDamage(float amount, float hitX, float hitY, float hitZ,
                    float targetX, float targetY, float targetZ,
                    uint32_t targetNameHash, uint8_t damageType)
{
    if (!IsNetConnected()) return;

    DamageData dmg{};
    dmg.amount = amount;
    dmg.hitX = hitX; dmg.hitY = hitY; dmg.hitZ = hitZ;
    dmg.targetX = targetX; dmg.targetY = targetY; dmg.targetZ = targetZ;
    dmg.targetNameHash = targetNameHash;
    dmg.damageType = damageType;

    // Send via net manager — need to add Damage packet type to manager
    // For now, use the raw SendPacket approach via net_manager internals
    // We'll add a helper in net_manager
    extern bool NetSendRawPacket(PacketType type, const void* data, uint16_t size);
    NetSendRawPacket(PacketType::Damage, &dmg, sizeof(dmg));
}

void CoopSendWorldEvent(uint8_t eventType, uint8_t state,
                        float posX, float posY, float posZ,
                        uint32_t actorNameHash)
{
    if (!IsNetConnected()) return;

    WorldEventData evt{};
    evt.eventType = eventType;
    evt.state = state;
    evt.posX = posX; evt.posY = posY; evt.posZ = posZ;
    evt.actorNameHash = actorNameHash;

    extern bool NetSendRawPacket(PacketType type, const void* data, uint16_t size);
    NetSendRawPacket(PacketType::WorldEvent, &evt, sizeof(evt));
}

} // namespace bs1sdk
