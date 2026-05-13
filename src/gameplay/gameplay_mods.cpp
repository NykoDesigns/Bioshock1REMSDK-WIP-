#include "gameplay_mods.h"
#include "teleport_plasmid.h"
#include "../engine/uobject.h"
#include "../engine/class_factory.h"
#include "../hooks/process_event.h"
#include "../core/log.h"
#include <cmath>
#include <vector>
#include <algorithm>
#include <Windows.h>

namespace bs1sdk {

// ═══════════════════════════════════════════════════════════════════════
// Shared State
// ═══════════════════════════════════════════════════════════════════════

static int s_ModsHookId = -1;
static bool s_ModsInitialized = false;

// Cached offsets
static int32_t s_LocationOff = -1;
static int32_t s_RotationOff = -1;
static int32_t s_HealthOff = -1;

struct FVec3 { float X, Y, Z; };
struct FRot  { int32_t Pitch, Yaw, Roll; };

static float VecDist(const FVec3& a, const FVec3& b) {
    float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static bool CacheOffsets()
{
    if (s_LocationOff >= 0) return true;
    UStruct* actorClass = FindClass("Actor");
    if (!actorClass) return false;
    auto props = WalkProperties(actorClass);
    for (auto& p : props) {
        if (p.Name == "Location") s_LocationOff = p.Offset;
        if (p.Name == "Rotation") s_RotationOff = p.Offset;
        if (p.Name == "Health")   s_HealthOff   = p.Offset;
    }
    return (s_LocationOff >= 0);
}

// ─── IsA helper ─────────────────────────────────────────────────────
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

// ═══════════════════════════════════════════════════════════════════════
// Goal 2: Decoy Plasmid → Teleportation
// ═══════════════════════════════════════════════════════════════════════
// DecoyHumanAbility fires UseAbility → spawns DecoyHuman at a position.
// We intercept the DecoyHuman spawn (PreBeginPlay) and read its Location,
// then teleport the player there and destroy the decoy.

static bool s_DecoyTeleportEnabled = true;
static DWORD s_DecoySpawnTime = 0;
static bool  s_DecoyTeleported = false;

bool IsDecoyTeleportEnabled() { return s_DecoyTeleportEnabled; }
void SetDecoyTeleportEnabled(bool e) { s_DecoyTeleportEnabled = e; }

static bool HandleDecoyTeleport(UObject* obj, const std::string& funcName)
{
    if (!s_DecoyTeleportEnabled) return false;

    // Detect the DecoyHuman actor spawning
    if (!(IsA(obj, "DecoyHuman") || obj->GetObjClassName() == "DecoyHuman"))
        return false;

    if (funcName == "PreBeginPlay") {
        s_DecoySpawnTime = GetTickCount();
        s_DecoyTeleported = false;
        LOG_INFO("[DecoyTP] Decoy spawned — will teleport player to its position");
        return false;
    }

    // On BeginPlay the decoy has its final location set
    if (!s_DecoyTeleported && funcName == "BeginPlay") {
        FVec3 decoyLoc = obj->GetField<FVec3>(s_LocationOff);

        if (decoyLoc.X != 0.0f || decoyLoc.Y != 0.0f || decoyLoc.Z != 0.0f) {
            // Teleport player to decoy position (offset Z for feet)
            DoTeleportTo(decoyLoc.X, decoyLoc.Y, decoyLoc.Z + 40.0f);
            s_DecoyTeleported = true;
            LOG_INFO("[DecoyTP] Teleported to ({:.0f}, {:.0f}, {:.0f})",
                     decoyLoc.X, decoyLoc.Y, decoyLoc.Z);

            // Kill the decoy quickly by setting a very short lifespan
            // Find LifeSpan property and set to 0.01
            UObject* cls = obj->GetClass();
            if (cls) {
                auto props = WalkProperties(reinterpret_cast<UStruct*>(cls));
                for (auto& p : props) {
                    if (p.Name == "LifeSpan") {
                        obj->SetField<float>(p.Offset, 0.01f);
                        break;
                    }
                }
            }
        }
        return false;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════
// Goal 3: Security Command → Friendly Bot Spawner (Limit 3)
// ═══════════════════════════════════════════════════════════════════════
// SecurityBeaconAbility fires BeaconProjectile. We intercept the beacon
// impact and instead of tagging an enemy, spawn a friendly SecurityBot.

static bool s_FriendlyBotsEnabled = true;
static int  s_FriendlyBotLimit = 3;
static std::vector<UObject*> s_FriendlyBots;

bool IsFriendlyBotsEnabled() { return s_FriendlyBotsEnabled; }
void SetFriendlyBotsEnabled(bool e) { s_FriendlyBotsEnabled = e; }
int  GetFriendlyBotCount() { return (int)s_FriendlyBots.size(); }
void SetFriendlyBotLimit(int limit) { s_FriendlyBotLimit = limit; }

// Clean up destroyed bots from tracking list
static void PruneDeadBots()
{
    s_FriendlyBots.erase(
        std::remove_if(s_FriendlyBots.begin(), s_FriendlyBots.end(),
            [](UObject* bot) {
                if (!bot) return true;
                // Check if bot is still alive by verifying health > 0
                if (s_HealthOff >= 0) {
                    float hp = bot->GetField<float>(s_HealthOff);
                    if (hp <= 0.0f) return true;
                }
                return false;
            }),
        s_FriendlyBots.end());
}

static void SpawnFriendlyBotAt(float x, float y, float z)
{
    PruneDeadBots();

    if ((int)s_FriendlyBots.size() >= s_FriendlyBotLimit) {
        LOG_WARN("[FriendlyBot] Bot limit ({}) reached — destroying oldest", s_FriendlyBotLimit);
        // Kill the oldest bot by setting health to 0
        if (!s_FriendlyBots.empty() && s_HealthOff >= 0) {
            s_FriendlyBots[0]->SetField<float>(s_HealthOff, 0.0f);
            s_FriendlyBots.erase(s_FriendlyBots.begin());
        }
    }

    // Find an existing SecurityBot and relocate it, OR find the SecurityBot
    // class and try to summon one via existing SpawnSecurityBot cheat function
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Strategy: Find a SecurityBot that is NOT in our friendly list
    UObject* botToRecruit = nullptr;
    for (int i = 0; i < objCount && !botToRecruit; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == "Class") continue;

        if (IsA(obj, "SecurityBot")) {
            // Check it's not already in our friendly list
            bool alreadyFriendly = false;
            for (auto* fb : s_FriendlyBots)
                if (fb == obj) { alreadyFriendly = true; break; }
            if (!alreadyFriendly) {
                botToRecruit = obj;
            }
        }
    }

    if (!botToRecruit) {
        LOG_WARN("[FriendlyBot] No SecurityBot found to recruit. Need SecurityBot spawner.");
        return;
    }

    // Move it to position
    FVec3 spawnPos = { x, y, z + 100.0f }; // Offset up so it doesn't clip ground
    botToRecruit->SetField<FVec3>(s_LocationOff, spawnPos);

    // Make it friendly: set disposition properties
    UObject* cls = botToRecruit->GetClass();
    if (cls) {
        auto props = WalkProperties(reinterpret_cast<UStruct*>(cls));
        for (auto& p : props) {
            if (p.Name == "DispositionToPlayer") {
                botToRecruit->SetField<int32_t>(p.Offset, 2); // Friendly
            }
        }
    }

    s_FriendlyBots.push_back(botToRecruit);
    LOG_INFO("[FriendlyBot] Recruited SecurityBot at ({:.0f}, {:.0f}, {:.0f}) — {}/{} bots",
             x, y, z, s_FriendlyBots.size(), s_FriendlyBotLimit);
}

static bool HandleFriendlyBots(UObject* obj, const std::string& funcName)
{
    if (!s_FriendlyBotsEnabled) return false;

    // Intercept BeaconProjectile impact
    if (!(IsA(obj, "BeaconProjectile") || obj->GetObjClassName() == "BeaconProjectile"))
        return false;

    // On impact events, spawn a friendly bot at that location instead
    static DWORD s_BeaconSpawn = 0;
    static bool  s_BotSpawned = false;

    if (funcName == "PreBeginPlay") {
        s_BeaconSpawn = GetTickCount();
        s_BotSpawned = false;
        return false;
    }

    DWORD elapsed = GetTickCount() - s_BeaconSpawn;
    if (elapsed < 150) return false;

    if (!s_BotSpawned &&
        (funcName == "BaseChange" || funcName == "Destroyed" ||
         funcName == "HitWall" || funcName == "Touch")) {

        FVec3 loc = obj->GetField<FVec3>(s_LocationOff);
        if (loc.X != 0.0f || loc.Y != 0.0f || loc.Z != 0.0f) {
            SpawnFriendlyBotAt(loc.X, loc.Y, loc.Z);
            s_BotSpawned = true;
        }
        return false;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════
// Goal 5: Revolver → Rosie Rivets
// ═══════════════════════════════════════════════════════════════════════
// Swap the Pistol's ProjectileClass to the Rosie rivet projectile.
// Done once at init, by finding the Pistol ammo class and overwriting
// its ProjectileClass property pointer.

static bool s_RivetPistolEnabled = false;
static int32_t s_ProjectileClassOff = -1;
static UObject* s_OrigPistolProjectile = nullptr;
static UObject* s_RosieProjectileClass = nullptr;

bool IsRivetPistolEnabled() { return s_RivetPistolEnabled; }

void SetRivetPistolEnabled(bool enabled)
{
    if (enabled == s_RivetPistolEnabled) return;
    s_RivetPistolEnabled = enabled;

    // Find Pistol_Bullet ammo object
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    UObject* pistolBulletAmmo = nullptr;
    UObject* rosieWeapon = nullptr;

    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);

        std::string cn = obj->GetObjClassName();
        if (cn == "Pistol_Bullet" && obj->GetName() != "Pistol_Bullet") {
            pistolBulletAmmo = obj;
        }
        // Find any ranged weapon used by Rosie for its projectile class
        if (cn == "RosieRangedWeapon" || obj->GetName() == "RosieRangedWeapon") {
            rosieWeapon = obj;
        }
    }

    // Cache ProjectileClass offset if needed
    if (s_ProjectileClassOff < 0 && pistolBulletAmmo) {
        UObject* cls = pistolBulletAmmo->GetClass();
        if (cls) {
            auto props = WalkProperties(reinterpret_cast<UStruct*>(cls));
            for (auto& p : props) {
                if (p.Name == "ProjectileClass") {
                    s_ProjectileClassOff = p.Offset;
                    break;
                }
            }
        }
    }

    if (s_ProjectileClassOff < 0 || !pistolBulletAmmo) {
        LOG_WARN("[RivetPistol] Could not find Pistol_Bullet ammo or ProjectileClass offset");
        s_RivetPistolEnabled = false;
        return;
    }

    if (enabled) {
        // Save original
        s_OrigPistolProjectile = pistolBulletAmmo->GetField<UObject*>(s_ProjectileClassOff);

        // Find Rosie's projectile class
        if (rosieWeapon && s_ProjectileClassOff >= 0) {
            // Try to get ProjectileClass from the Rosie weapon
            UObject* rCls = rosieWeapon->GetClass();
            if (rCls) {
                auto rProps = WalkProperties(reinterpret_cast<UStruct*>(rCls));
                for (auto& p : rProps) {
                    if (p.Name == "ProjectileClass") {
                        s_RosieProjectileClass = rosieWeapon->GetField<UObject*>(p.Offset);
                        break;
                    }
                }
            }
        }

        if (s_RosieProjectileClass) {
            pistolBulletAmmo->SetField<UObject*>(s_ProjectileClassOff, s_RosieProjectileClass);
            LOG_INFO("[RivetPistol] Swapped Pistol projectile → Rosie rivet projectile!");
        } else {
            // Fallback: find ShockProjectile or CrossbowProjectile for a visible projectile
            LOG_WARN("[RivetPistol] Could not find Rosie projectile class — looking for alternatives");
            for (int i = 0; i < objCount; i++) {
                uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
                if (!ptr) continue;
                UObject* obj = reinterpret_cast<UObject*>(ptr);
                if (obj->GetName() == "CrossbowProjectile" && obj->GetObjClassName() == "Class") {
                    pistolBulletAmmo->SetField<UObject*>(s_ProjectileClassOff, obj);
                    LOG_INFO("[RivetPistol] Swapped Pistol projectile → CrossbowProjectile (fallback)");
                    break;
                }
            }
        }
    } else {
        // Restore original
        if (s_OrigPistolProjectile) {
            pistolBulletAmmo->SetField<UObject*>(s_ProjectileClassOff, s_OrigPistolProjectile);
            LOG_INFO("[RivetPistol] Restored original Pistol projectile");
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Goal 6: Splicer Factions
// ═══════════════════════════════════════════════════════════════════════
// Tag splicers into two factions. Use AddForcedEnemy on cross-faction
// pairs so they attack each other. Hook IsFriendly to return true for
// same-faction, false for different.

static bool s_FactionsEnabled = false;

// Map object pointers to faction IDs (0 = untagged, 1 = faction A, 2 = faction B)
struct FactionEntry { UObject* pawn; int faction; };
static std::vector<FactionEntry> s_FactionMap;

static void ApplyFactionHostilities();

bool IsSplicerFactionsEnabled() { return s_FactionsEnabled; }
void SetSplicerFactionsEnabled(bool e)
{
    s_FactionsEnabled = e;
    if (!e) s_FactionMap.clear();
    LOG_INFO("[Factions] Splicer factions {}", e ? "ENABLED" : "DISABLED");
}

void TagSplicerFaction(int factionId)
{
    if (!s_FactionsEnabled) {
        LOG_WARN("[Factions] Enable factions first (factions on)");
        return;
    }

    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player || s_LocationOff < 0) return;

    FVec3 playerLoc = player->GetField<FVec3>(s_LocationOff);

    // Find nearest splicer (ShockPawn that isn't the player)
    auto& globals = GetEngineGlobals();
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    UObject* nearest = nullptr;
    float nearDist = 999999.0f;

    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj == player) continue;
        if (obj->GetObjClassName() == "Class") continue;

        if (IsA(obj, "ShockPawn") && !IsA(obj, "ShockPlayer")) {
            FVec3 loc = obj->GetField<FVec3>(s_LocationOff);
            float dist = VecDist(playerLoc, loc);
            if (dist < nearDist) {
                nearDist = dist;
                nearest = obj;
            }
        }
    }

    if (!nearest) {
        LOG_WARN("[Factions] No splicer found nearby");
        return;
    }

    // Remove old entry if exists
    s_FactionMap.erase(
        std::remove_if(s_FactionMap.begin(), s_FactionMap.end(),
            [&](const FactionEntry& e) { return e.pawn == nearest; }),
        s_FactionMap.end());

    s_FactionMap.push_back({nearest, factionId});

    LOG_INFO("[Factions] Tagged {} as Faction {} (dist={:.0f}) — {} tagged total",
             nearest->GetObjClassName(), factionId, nearDist, s_FactionMap.size());

    // Now make all cross-faction pawns enemies of each other
    ApplyFactionHostilities();
}

static void ApplyFactionHostilities()
{
    // For every pair of tagged pawns in different factions,
    // use AddForcedEnemy to make them fight
    for (size_t i = 0; i < s_FactionMap.size(); i++) {
        for (size_t j = i + 1; j < s_FactionMap.size(); j++) {
            if (s_FactionMap[i].faction == s_FactionMap[j].faction) continue;
            if (!s_FactionMap[i].pawn || !s_FactionMap[j].pawn) continue;

            // Find AddForcedEnemy on the AI controller and call it
            // For now, use the simpler approach: set Berserk on each pawn
            // toward the other. Berserk makes them attack everything nearby.
            UObject* pawnA = s_FactionMap[i].pawn;
            UObject* pawnB = s_FactionMap[j].pawn;

            // Use the SetBerserk function on ShockPawn
            UStruct* pawnClass = FindClass("ShockPawn");
            if (!pawnClass) continue;

            // Find SetBerserk function
            UField* child = pawnClass->GetChildren();
            int limit = 2000;
            while (child && limit-- > 0) {
                if (child->GetObjClassName() == "Function" && child->GetName() == "SetBerserk") {
                    UFunction* setBerserk = reinterpret_cast<UFunction*>(child);
                    ProcessEventFn origPE = GetOriginalProcessEvent();
                    if (origPE) {
                        // SetBerserk(float Effectiveness, UObject* StateInstigator)
                        struct { float Effectiveness; UObject* Instigator; } parms;
                        parms.Effectiveness = 1.0f;

                        // Make A hostile to B and vice versa
                        parms.Instigator = pawnB;
                        origPE(pawnA, setBerserk, &parms, nullptr);
                        parms.Instigator = pawnA;
                        origPE(pawnB, setBerserk, &parms, nullptr);

                        LOG_INFO("[Factions] {} vs {} — both set to berserk",
                                 pawnA->GetName(), pawnB->GetName());
                    }
                    break;
                }
                child = child->GetNext();
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Goal 7: Chain Lightning Electro Bolt
// ═══════════════════════════════════════════════════════════════════════
// When an ElectricBolt hits an enemy, search for nearby enemies and
// apply electric damage to them too (chaining).

static bool  s_ChainLightningEnabled = true;
static float s_ChainRadius = 500.0f;
static int   s_ChainMaxJumps = 3;
static float s_ChainDamageFalloff = 0.5f; // Each jump does 50% of previous

bool IsChainLightningEnabled() { return s_ChainLightningEnabled; }
void SetChainLightningEnabled(bool e) { s_ChainLightningEnabled = e; }
void SetChainLightningRadius(float r) { s_ChainRadius = r; }
void SetChainLightningJumps(int j) { s_ChainMaxJumps = j; }
void SetChainLightningDamageFalloff(float f) { s_ChainDamageFalloff = f; }

// Track recent chain events to avoid infinite loops
static DWORD s_LastChainTime = 0;
static std::vector<UObject*> s_ChainHitList;

static void ChainLightningFrom(UObject* hitPawn, float damage, int jumpsLeft)
{
    if (jumpsLeft <= 0 || damage < 1.0f) return;

    FVec3 origin = hitPawn->GetField<FVec3>(s_LocationOff);

    auto& globals = GetEngineGlobals();
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Find nearby ShockPawns within chain radius
    std::vector<std::pair<UObject*, float>> candidates;

    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj == hitPawn) continue;
        if (obj->GetObjClassName() == "Class") continue;

        // Skip already-chained targets
        bool alreadyHit = false;
        for (auto* h : s_ChainHitList)
            if (h == obj) { alreadyHit = true; break; }
        if (alreadyHit) continue;

        // Must be a ShockPawn (enemy), not the player
        if (IsA(obj, "ShockPawn") && !IsA(obj, "ShockPlayer")) {
            FVec3 loc = obj->GetField<FVec3>(s_LocationOff);
            float dist = VecDist(origin, loc);
            if (dist <= s_ChainRadius && dist > 0) {
                // Check pawn is alive
                if (s_HealthOff >= 0) {
                    float hp = obj->GetField<float>(s_HealthOff);
                    if (hp <= 0.0f) continue;
                }
                candidates.push_back({obj, dist});
            }
        }
    }

    // Sort by distance — chain to nearest first
    std::sort(candidates.begin(), candidates.end(),
              [](auto& a, auto& b) { return a.second < b.second; });

    // Chain to nearest target
    if (!candidates.empty()) {
        UObject* target = candidates[0].first;
        s_ChainHitList.push_back(target);

        // Apply damage by reducing health directly
        if (s_HealthOff >= 0) {
            float currentHP = target->GetField<float>(s_HealthOff);
            float newHP = currentHP - damage;
            target->SetField<float>(s_HealthOff, newHP);

            FVec3 tLoc = target->GetField<FVec3>(s_LocationOff);
            LOG_INFO("[ChainLightning] Chained to {} at ({:.0f},{:.0f},{:.0f}) for {:.0f} dmg (jump {}, HP: {:.0f}→{:.0f})",
                     target->GetObjClassName(), tLoc.X, tLoc.Y, tLoc.Z,
                     damage, s_ChainMaxJumps - jumpsLeft + 1, currentHP, newHP);
        }

        // Recurse — chain further
        ChainLightningFrom(target, damage * s_ChainDamageFalloff, jumpsLeft - 1);
    }
}

static bool HandleChainLightning(UObject* obj, const std::string& funcName)
{
    if (!s_ChainLightningEnabled) return false;

    // Detect ElectricBolt hitting an enemy
    // The ElectricBolt plasmid fires a bolt that triggers TakeDamage on the target.
    // We watch for TakeDamage events on ShockPawns where the damage type involves electricity.

    if (funcName != "TakeDamage") return false;

    // Only process on ShockPawns (enemies), not the player
    if (!IsA(obj, "ShockPawn") || IsA(obj, "ShockPlayer")) return false;

    // Rate-limit: don't chain if we just chained within 200ms (avoid chain-of-chains)
    DWORD now = GetTickCount();
    if (now - s_LastChainTime < 200) return false;

    // Check if this damage is from an electric source
    // We can't easily read the TakeDamage parms struct without knowing its exact layout.
    // Instead, we use a heuristic: check if an ElectricBolt ability was recently used.
    // For now, trigger on ANY TakeDamage to an enemy while the chain system is active.
    // TODO: Refine to only trigger on electric damage by reading parms->DamageType

    // Start chain from this target
    s_LastChainTime = now;
    s_ChainHitList.clear();
    s_ChainHitList.push_back(obj);

    // Estimate damage from health delta (we'll read health before/after)
    float baseDamage = 50.0f; // Default chain damage
    if (s_HealthOff >= 0) {
        float hp = obj->GetField<float>(s_HealthOff);
        if (hp > 0) baseDamage = hp * 0.15f; // 15% of target's current health
        if (baseDamage < 10.0f) baseDamage = 10.0f;
        if (baseDamage > 200.0f) baseDamage = 200.0f;
    }

    LOG_INFO("[ChainLightning] Initial hit on {} — starting chain (radius={:.0f}, jumps={}, dmg={:.0f})",
             obj->GetObjClassName(), s_ChainRadius, s_ChainMaxJumps, baseDamage);

    ChainLightningFrom(obj, baseDamage * s_ChainDamageFalloff, s_ChainMaxJumps);

    return false; // Don't block original TakeDamage
}

// ═══════════════════════════════════════════════════════════════════════
// Master Hook — Routes all ProcessEvent calls to the right handler
// ═══════════════════════════════════════════════════════════════════════

bool InitGameplayMods()
{
    if (s_ModsInitialized) return true;

    if (!IsProcessEventHooked()) InitProcessEventHook();
    if (!CacheOffsets()) {
        LOG_ERROR("[GameplayMods] Failed to cache property offsets");
        return false;
    }

    ProcessEventHook hook;
    hook.Name = "GameplayMods";
    hook.FunctionFilter = ""; // Listen to all events
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        std::string funcName = func->GetName();

        // Goal 2: Decoy → Teleport
        if (HandleDecoyTeleport(obj, funcName)) return true;

        // Goal 3: Security → Friendly Bots
        // (Only if DecoyTeleport is OFF for Security Bullseye — they use same projectile)
        // Actually, Security uses BeaconProjectile, Decoy uses DecoyHuman. No conflict.
        if (HandleFriendlyBots(obj, funcName)) return true;

        // Goal 7: Chain Lightning
        if (HandleChainLightning(obj, funcName)) return true;

        return false;
    };

    s_ModsHookId = RegisterProcessEventHook(hook);
    s_ModsInitialized = true;

    LOG_INFO("═══════════════════════════════════════════════════");
    LOG_INFO("[GameplayMods] ALL MODS ACTIVE!");
    LOG_INFO("  [2] Decoy Plasmid → Teleportation    {}", s_DecoyTeleportEnabled ? "ON" : "OFF");
    LOG_INFO("  [3] Security → Friendly Bots (max {}) {}", s_FriendlyBotLimit, s_FriendlyBotsEnabled ? "ON" : "OFF");
    LOG_INFO("  [5] Revolver → Rivet Gun              {}", s_RivetPistolEnabled ? "ON" : "OFF");
    LOG_INFO("  [6] Splicer Factions                   {}", s_FactionsEnabled ? "ON" : "OFF");
    LOG_INFO("  [7] Chain Lightning (r={:.0f}, j={})    {}", s_ChainRadius, s_ChainMaxJumps, s_ChainLightningEnabled ? "ON" : "OFF");
    LOG_INFO("═══════════════════════════════════════════════════");
    return true;
}

void ShutdownGameplayMods()
{
    if (s_ModsHookId >= 0) {
        UnregisterProcessEventHook(s_ModsHookId);
        s_ModsHookId = -1;
    }
    // Restore rivet pistol
    if (s_RivetPistolEnabled) SetRivetPistolEnabled(false);
    s_FriendlyBots.clear();
    s_FactionMap.clear();
    s_ModsInitialized = false;
    LOG_INFO("[GameplayMods] All mods shut down");
}

std::string GetGameplayModsStatus()
{
    if (!s_ModsInitialized) return "Not initialized — run 'initmods'";

    PruneDeadBots();

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "Gameplay Mods:\n"
        "  [2] Decoy Teleport:    %s\n"
        "  [3] Friendly Bots:     %s (%d/%d alive)\n"
        "  [5] Rivet Pistol:      %s\n"
        "  [6] Splicer Factions:  %s (%d tagged)\n"
        "  [7] Chain Lightning:   %s (r=%.0f, j=%d, f=%.0f%%)",
        s_DecoyTeleportEnabled ? "ON" : "OFF",
        s_FriendlyBotsEnabled ? "ON" : "OFF", (int)s_FriendlyBots.size(), s_FriendlyBotLimit,
        s_RivetPistolEnabled ? "ON" : "OFF",
        s_FactionsEnabled ? "ON" : "OFF", (int)s_FactionMap.size(),
        s_ChainLightningEnabled ? "ON" : "OFF", s_ChainRadius, s_ChainMaxJumps,
        s_ChainDamageFalloff * 100.0f);
    return buf;
}

} // namespace bs1sdk
