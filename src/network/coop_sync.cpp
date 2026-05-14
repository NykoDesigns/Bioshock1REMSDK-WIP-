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
static int  s_DeathHookId = -1;
static int  s_TriggerHookId = -1;

// Property offsets (shared with coop_bridge via extern or re-cached)
static int s_SyncLocOffset = -1;
static int s_SyncHealthOffset = -1;

// Flag to prevent echo (don't re-send damage we received from remote)
static bool s_ApplyingRemoteDamage = false;
static bool s_ApplyingRemoteDeath = false;

// Track player death state for respawn detection
static bool s_LocalPlayerDead = false;
static float s_DeathCheckTimer = 0.0f;

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

        // ACTUAL TakeDamage signature (from decompiled ShockPawn.uc):
        // TakeDamage(DamageStimuliSet DamageStimuli, float CritChance, Actor Damager,
        //            Vector HitLocation, Vector HitNormal, Vector HitImpulseDirection,
        //            name EffectEventName, float DamageAttenuation,
        //            optional name HitHighBone, optional name HitLowBone,
        //            optional bool WasMeleeAttack)
        //
        // Parms layout (UE2 packed sequentially):
        //   +0x00: UObject* DamageStimuli (4 bytes, pointer)
        //   +0x04: float CritChance (4 bytes)
        //   +0x08: UObject* Damager (4 bytes, pointer)
        //   +0x0C: FVector HitLocation (12 bytes: X,Y,Z)
        //   +0x18: FVector HitNormal (12 bytes)
        //   +0x24: FVector HitImpulseDirection (12 bytes)
        //   +0x30: FName EffectEventName (8 bytes)
        //   +0x38: float DamageAttenuation (4 bytes)
        if (!parms) return false;

        // We can't easily extract the numerical damage from the DamageStimuliSet
        // object without walking its properties. For sync, use DamageAttenuation
        // as an approximate scalar, or just send a "this enemy was hit" event.
        // The health delta on the target is more reliable.
        float prevHealth = 0, postHealth = 0;
        if (s_SyncHealthOffset > 0) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
            memcpy(&prevHealth, raw + s_SyncHealthOffset, 4);
        }

        // We'll read damage as health delta on next tick; for now, send a
        // nominal damage value of 1.0 to indicate "this NPC was hit"
        float damage = 1.0f;
        if (damage <= 0.0f) return false;

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
static EnemyDeathData s_PendingEnemyDeaths[16];
static int s_PendingEnemyDeathCount = 0;
static PlayerDeathData s_PendingPlayerDeaths[4];
static int s_PendingPlayerDeathCount = 0;
static PlayerRespawnData s_PendingPlayerRespawns[4];
static int s_PendingPlayerRespawnCount = 0;
static TriggerSyncData s_PendingTriggers[16];
static int s_PendingTriggerCount = 0;

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

void QueueEnemyDeathPacket(const EnemyDeathData& death)
{
    if (s_PendingEnemyDeathCount < 16) {
        s_PendingEnemyDeaths[s_PendingEnemyDeathCount++] = death;
    }
}

void QueuePlayerDeathPacket(const PlayerDeathData& death)
{
    if (s_PendingPlayerDeathCount < 4) {
        s_PendingPlayerDeaths[s_PendingPlayerDeathCount++] = death;
    }
}

void QueuePlayerRespawnPacket(const PlayerRespawnData& respawn)
{
    if (s_PendingPlayerRespawnCount < 4) {
        s_PendingPlayerRespawns[s_PendingPlayerRespawnCount++] = respawn;
    }
}

void QueueTriggerSyncPacket(const TriggerSyncData& trigger)
{
    if (s_PendingTriggerCount < 16) {
        s_PendingTriggers[s_PendingTriggerCount++] = trigger;
    }
}

// ─── IsA helper (check class inheritance) ────────────────────────────

static bool IsA(UObject* obj, const std::string& baseClassName)
{
    if (!obj) return false;
    UObject* cls = obj->GetClass();
    if (!cls) return false;
    UField* current = reinterpret_cast<UField*>(cls);
    int safety = 64;
    while (current && safety-- > 0) {
        if (current->GetName() == baseClassName) return true;
        current = current->GetSuperField();
    }
    return false;
}

// ─── Find actor by class name + position (improved matching) ─────────

static UObject* FindActorByClassAndPos(const std::string& className, float tx, float ty, float tz, float tolerance = 500.0f)
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

        // Match by class name (exact or inheritance)
        std::string cn = obj->GetObjClassName();
        if (cn != className && !IsA(obj, className)) continue;
        if (cn == "Class") continue; // skip class objects

        // Position check
        if (s_SyncLocOffset > 0) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
            float ox, oy, oz;
            memcpy(&ox, raw + s_SyncLocOffset, 4);
            memcpy(&oy, raw + s_SyncLocOffset + 4, 4);
            memcpy(&oz, raw + s_SyncLocOffset + 8, 4);

            float dx = ox - tx, dy = oy - ty, dz = oz - tz;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < bestDist) {
                // Also check the enemy is alive
                if (s_SyncHealthOffset > 0) {
                    float hp;
                    memcpy(&hp, raw + s_SyncHealthOffset, 4);
                    if (hp <= 0.0f) continue; // already dead
                }
                bestDist = dist;
                bestMatch = obj;
            }
        }
    }

    return bestMatch;
}

// ─── Apply Remote Enemy Death ────────────────────────────────────────

static void ApplyRemoteEnemyDeath(const EnemyDeathData& death)
{
    UObject* target = FindActorByClassAndPos(death.className, death.posX, death.posY, death.posZ);
    if (!target) {
        LOG_WARN("[Co-op Sync] Could not find matching {} near ({:.0f},{:.0f},{:.0f}) to kill",
                 death.className, death.posX, death.posY, death.posZ);
        return;
    }

    // Kill by setting health to 0
    if (s_SyncHealthOffset > 0) {
        s_ApplyingRemoteDeath = true;
        uint8_t* raw = reinterpret_cast<uint8_t*>(target);
        float zero = 0.0f;
        memcpy(raw + s_SyncHealthOffset, &zero, 4);
        s_ApplyingRemoteDeath = false;

        LOG_INFO("[Co-op Sync] Killed '{}' ({}) — partner got the kill!",
                 target->GetName(), death.className);
    }
}

// ─── Apply Remote Player Death/Respawn ───────────────────────────────

static void ApplyRemotePlayerDeath(const PlayerDeathData& death)
{
    LOG_WARN("[Co-op] Partner died at ({:.0f},{:.0f},{:.0f}) — killed by {}",
             death.posX, death.posY, death.posZ, death.killerClass);
}

static void ApplyRemotePlayerRespawn(const PlayerRespawnData& respawn)
{
    LOG_INFO("[Co-op] Partner respawned at ({:.0f},{:.0f},{:.0f})",
             respawn.posX, respawn.posY, respawn.posZ);
}

// ─── Apply Remote Trigger ────────────────────────────────────────────

static void ApplyRemoteTrigger(const TriggerSyncData& trigger)
{
    UObject* actor = FindActorByHashAndPos(trigger.triggerNameHash,
                                            trigger.posX, trigger.posY, trigger.posZ, 200.0f);
    if (!actor) return;

    // Try to call Trigger() on the actor via ProcessEvent
    UStruct* cls = reinterpret_cast<UStruct*>(actor->GetClass());
    if (!cls) return;

    UField* child = cls->GetChildren();
    int limit = 2000;
    while (child && limit-- > 0) {
        if (child->GetObjClassName() == "Function" && child->GetName() == "Trigger") {
            ProcessEventFn origPE = GetOriginalProcessEvent();
            if (origPE) {
                // Trigger(Actor Other, Pawn EventInstigator)
                struct { UObject* Other; UObject* Instigator; } parms{};
                parms.Other = FindObjectByClassName("ShockPlayer");
                parms.Instigator = parms.Other;
                origPE(actor, reinterpret_cast<UFunction*>(child), &parms, nullptr);
                LOG_INFO("[Co-op Sync] Triggered '{}' from partner", actor->GetName());
            }
            break;
        }
        child = child->GetNext();
    }
}

// ─── Death/Trigger Hooks ──────────────────────────────────────────────

static void InstallDeathHook()
{
    ProcessEventHook hook;
    hook.Name = "CoopDeathSync";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        if (s_ApplyingRemoteDeath) return false;

        std::string funcName = func->GetName();
        if (funcName != "Died") return false;

        std::string cn = obj->GetObjClassName();

        // ─── Enemy died ───
        if (cn != "ShockPlayer" && cn != "ShockPlayerController" &&
            (IsA(obj, "ShockPawn") || IsA(obj, "Pawn"))) {

            float px = 0, py = 0, pz = 0;
            if (s_SyncLocOffset > 0) {
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                memcpy(&px, raw + s_SyncLocOffset, 4);
                memcpy(&py, raw + s_SyncLocOffset + 4, 4);
                memcpy(&pz, raw + s_SyncLocOffset + 8, 4);
            }

            EnemyDeathData death{};
            strncpy(death.className, cn.c_str(), sizeof(death.className) - 1);
            death.nameHash = FnvHash(obj->GetName());
            death.posX = px; death.posY = py; death.posZ = pz;

            NetSendRawPacket(PacketType::EnemyDeath, &death, sizeof(death));
            LOG_INFO("[Co-op Sync] Sent enemy death: {} at ({:.0f},{:.0f},{:.0f})",
                     cn, px, py, pz);
            return false;
        }

        // ─── Player died ───
        if (cn == "ShockPlayer") {
            float px = 0, py = 0, pz = 0;
            if (s_SyncLocOffset > 0) {
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                memcpy(&px, raw + s_SyncLocOffset, 4);
                memcpy(&py, raw + s_SyncLocOffset + 4, 4);
                memcpy(&pz, raw + s_SyncLocOffset + 8, 4);
            }

            PlayerDeathData death{};
            death.posX = px; death.posY = py; death.posZ = pz;

            // Try to get killer class from parms
            // Died(Controller Killer, class DamageType, vector HitLoc)
            if (parms) {
                UObject* killer = *reinterpret_cast<UObject**>(parms);
                if (killer) {
                    std::string killerCn = killer->GetObjClassName();
                    strncpy(death.killerClass, killerCn.c_str(), sizeof(death.killerClass) - 1);
                }
            }

            NetSendRawPacket(PacketType::PlayerDeath, &death, sizeof(death));
            s_LocalPlayerDead = true;
            LOG_WARN("[Co-op] You died! Notifying partner...");
            return false;
        }

        return false;
    };
    s_DeathHookId = RegisterProcessEventHook(hook);
}

static void InstallTriggerHook()
{
    ProcessEventHook hook;
    hook.Name = "CoopTriggerSync";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        std::string funcName = func->GetName();
        if (funcName != "TriggerEvent" && funcName != "Trigger") return false;

        std::string cn = obj->GetObjClassName();
        // Only sync Trigger/TriggerVolume/SequenceAction actors
        if (cn.find("Trigger") == std::string::npos &&
            cn.find("Sequence") == std::string::npos &&
            cn.find("Kismet") == std::string::npos)
            return false;

        float px = 0, py = 0, pz = 0;
        if (s_SyncLocOffset > 0) {
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
            memcpy(&px, raw + s_SyncLocOffset, 4);
            memcpy(&py, raw + s_SyncLocOffset + 4, 4);
            memcpy(&pz, raw + s_SyncLocOffset + 8, 4);
        }

        TriggerSyncData trigger{};
        trigger.triggerNameHash = FnvHash(obj->GetName());
        trigger.posX = px; trigger.posY = py; trigger.posZ = pz;
        trigger.state = 1;

        NetSendRawPacket(PacketType::TriggerSync, &trigger, sizeof(trigger));
        return false;
    };
    s_TriggerHookId = RegisterProcessEventHook(hook);
}

// ─── Public API ────────────────────────────────────────────────────────

bool InitCoopSync()
{
    if (s_SyncInitialized) return true;

    // Cache property offsets
    const auto& globals = GetEngineGlobals();
    if (globals.IsValid()) {
        UStruct* cls = FindClass("ShockPawn");
        if (!cls) cls = FindClass("Pawn");
        if (cls) {
            std::vector<PropertyInfo> props = WalkProperties(cls);
            PropertyInfo* pi = FindProperty(cls, "Location", props);
            if (pi) s_SyncLocOffset = pi->Offset;
            PropertyInfo* hp = FindProperty(cls, "Health", props);
            if (hp) s_SyncHealthOffset = hp->Offset;
        }
    }

    InstallDamageHook();
    InstallWorldHook();
    InstallDeathHook();
    InstallTriggerHook();

    s_SyncInitialized = true;
    s_LocalPlayerDead = false;
    LOG_INFO("[Co-op Sync] Full sync initialized (Loc={}, Health={})", s_SyncLocOffset, s_SyncHealthOffset);
    return true;
}

void ShutdownCoopSync()
{
    if (!s_SyncInitialized) return;
    if (s_DamageHookId >= 0) UnregisterProcessEventHook(s_DamageHookId);
    if (s_WorldHookId >= 0) UnregisterProcessEventHook(s_WorldHookId);
    if (s_DeathHookId >= 0) UnregisterProcessEventHook(s_DeathHookId);
    if (s_TriggerHookId >= 0) UnregisterProcessEventHook(s_TriggerHookId);
    s_DamageHookId = -1;
    s_WorldHookId = -1;
    s_DeathHookId = -1;
    s_TriggerHookId = -1;
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

    // Process pending enemy deaths
    for (int i = 0; i < s_PendingEnemyDeathCount; i++) {
        ApplyRemoteEnemyDeath(s_PendingEnemyDeaths[i]);
    }
    s_PendingEnemyDeathCount = 0;

    // Process pending player deaths
    for (int i = 0; i < s_PendingPlayerDeathCount; i++) {
        ApplyRemotePlayerDeath(s_PendingPlayerDeaths[i]);
    }
    s_PendingPlayerDeathCount = 0;

    // Process pending player respawns
    for (int i = 0; i < s_PendingPlayerRespawnCount; i++) {
        ApplyRemotePlayerRespawn(s_PendingPlayerRespawns[i]);
    }
    s_PendingPlayerRespawnCount = 0;

    // Process pending triggers
    for (int i = 0; i < s_PendingTriggerCount; i++) {
        ApplyRemoteTrigger(s_PendingTriggers[i]);
    }
    s_PendingTriggerCount = 0;

    // Check for player respawn (health went from 0 to >0)
    if (s_LocalPlayerDead && s_SyncHealthOffset > 0) {
        UObject* player = FindObjectByClassName("ShockPlayer");
        if (player) {
            float hp;
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(player);
            memcpy(&hp, raw + s_SyncHealthOffset, 4);
            if (hp > 0.0f) {
                s_LocalPlayerDead = false;
                float px = 0, py = 0, pz = 0;
                if (s_SyncLocOffset > 0) {
                    memcpy(&px, raw + s_SyncLocOffset, 4);
                    memcpy(&py, raw + s_SyncLocOffset + 4, 4);
                    memcpy(&pz, raw + s_SyncLocOffset + 8, 4);
                }
                PlayerRespawnData respawn{};
                respawn.posX = px; respawn.posY = py; respawn.posZ = pz;
                NetSendRawPacket(PacketType::PlayerRespawn, &respawn, sizeof(respawn));
                LOG_INFO("[Co-op] Respawned! Notifying partner...");
            }
        }
    }
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

    NetSendRawPacket(PacketType::WorldEvent, &evt, sizeof(evt));
}

} // namespace bs1sdk
