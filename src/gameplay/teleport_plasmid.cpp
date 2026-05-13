#include "teleport_plasmid.h"
#include "../engine/uobject.h"
#include "../engine/class_factory.h"
#include "../hooks/process_event.h"
#include "../core/log.h"
#include <cmath>
#include <atomic>

namespace bs1sdk {

// ─── State ──────────────────────────────────────────────────────────────

static int s_HijackHookId = -1;
static bool s_Initialized = false;
static float s_TeleportDistance = 800.0f;

// Projectile tracking: when a hijacked projectile exists, we poll its location
static UObject* s_TrackedBeacon = nullptr;       // BeaconProjectile we're watching
static UObject* s_TrackedDart = nullptr;         // SummonGathererDartProjectile we're watching
static bool s_WaitingForBeaconHit = false;
static bool s_WaitingForDartHit = false;

// Cached property offsets (resolved once)
static int32_t s_LocationOffset = -1;
static int32_t s_RotationOffset = -1;

// ─── Math helpers ───────────────────────────────────────────────────────

struct FVector {
    float X, Y, Z;
};

struct FRotator {
    int32_t Pitch, Yaw, Roll; // UE2 uses int32 rotator units (0-65535 = 0-360)
};

static constexpr float PI_F = 3.14159265358979f;
static constexpr float URot2Rad = PI_F / 32768.0f;

static FVector RotationToDirection(const FRotator& rot)
{
    float pitch = static_cast<float>(rot.Pitch) * URot2Rad;
    float yaw = static_cast<float>(rot.Yaw) * URot2Rad;

    FVector dir;
    dir.X = cosf(pitch) * cosf(yaw);
    dir.Y = cosf(pitch) * sinf(yaw);
    dir.Z = -sinf(pitch);
    return dir;
}

// ─── Property Cache ─────────────────────────────────────────────────────

static bool CachePropertyOffsets()
{
    if (s_LocationOffset >= 0) return true;

    UStruct* actorClass = FindClass("Actor");
    if (!actorClass) return false;

    auto props = WalkProperties(actorClass);
    for (auto& p : props) {
        if (p.Name == "Location") s_LocationOffset = p.Offset;
        if (p.Name == "Rotation") s_RotationOffset = p.Offset;
    }

    return (s_LocationOffset >= 0 && s_RotationOffset >= 0);
}

// ─── Teleport Logic ─────────────────────────────────────────────────────

bool DoTeleport(float distance)
{
    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) {
        LOG_ERROR("[Teleport] No player found");
        return false;
    }

    if (!CachePropertyOffsets()) {
        LOG_ERROR("[Teleport] Could not resolve Location/Rotation offsets");
        return false;
    }

    FVector loc = player->GetField<FVector>(s_LocationOffset);
    FRotator rot = player->GetField<FRotator>(s_RotationOffset);

    FVector dir = RotationToDirection(rot);

    FVector newLoc;
    newLoc.X = loc.X + dir.X * distance;
    newLoc.Y = loc.Y + dir.Y * distance;
    newLoc.Z = loc.Z + dir.Z * distance;

    player->SetField<FVector>(s_LocationOffset, newLoc);

    LOG_INFO("[Teleport] Blinked from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f})",
             loc.X, loc.Y, loc.Z, newLoc.X, newLoc.Y, newLoc.Z);
    return true;
}

bool DoTeleportTo(float x, float y, float z)
{
    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) {
        LOG_ERROR("[Teleport] No player found");
        return false;
    }

    if (!CachePropertyOffsets()) return false;

    FVector newLoc = { x, y, z };
    player->SetField<FVector>(s_LocationOffset, newLoc);

    LOG_INFO("[Teleport] Teleported to ({:.0f}, {:.0f}, {:.0f})", x, y, z);
    return true;
}

// ─── Class Hierarchy Check ──────────────────────────────────────────────
// Walk the SuperField chain of an object's Class to check if it inherits
// from a given base class name (like UE's IsA).

static bool IsA(UObject* obj, const std::string& baseClassName)
{
    if (!obj) return false;

    // Get the object's Class (which is a UClass/UStruct)
    UObject* cls = obj->GetClass();
    if (!cls) return false;

    // Walk the SuperField chain: Class → State → Struct → Field → Object
    UField* current = reinterpret_cast<UField*>(cls);
    int safety = 64;
    while (current && safety-- > 0) {
        if (current->GetName() == baseClassName)
            return true;
        current = current->GetSuperField();
    }
    return false;
}

// ─── Big Daddy Summoning ────────────────────────────────────────────────
// Find an existing Protector (Bouncer/Rosie) in the level by walking
// the class hierarchy, move it to the target location, make it friendly.

static void SummonBigDaddyAt(float x, float y, float z)
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    if (!CachePropertyOffsets()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Find a Protector instance by walking class hierarchy (IsA check)
    UObject* protector = nullptr;
    for (int i = 0; i < objCount && !protector; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);

        // Skip Class objects themselves — we want instances
        if (obj->GetObjClassName() == "Class") continue;

        // Check if this object inherits from Protector
        if (IsA(obj, "Protector")) {
            protector = obj;
            LOG_INFO("[BigDaddy] Found Protector instance: {} (class: {})",
                     obj->GetName(), obj->GetObjClassName());
        }
    }

    if (protector) {
        // Move it to the target location
        FVector targetLoc = { x, y, z };
        protector->SetField<FVector>(s_LocationOffset, targetLoc);

        // Walk properties to set friendly disposition
        UObject* clsObj = protector->GetClass();
        if (clsObj) {
            auto props = WalkProperties(reinterpret_cast<UStruct*>(clsObj));
            for (auto& p : props) {
                if (p.Name == "DispositionToPlayer") {
                    // EDispositionToPlayer: 0=None, 1=Hostile, 2=Friendly
                    protector->SetField<int32_t>(p.Offset, 2);
                    LOG_INFO("[BigDaddy] Set DispositionToPlayer = Friendly");
                }
                if (p.Name == "bProtectingPlayer") {
                    protector->SetField<uint8_t>(p.Offset, 1);
                    LOG_INFO("[BigDaddy] Set bProtectingPlayer = true");
                }
            }
        }

        LOG_INFO("[BigDaddy] Summoned friendly {} at ({:.0f}, {:.0f}, {:.0f})",
                 protector->GetObjClassName(), x, y, z);
    } else {
        LOG_WARN("[BigDaddy] No Protector-derived object found in GObjects!");
        LOG_INFO("[BigDaddy] The Big Daddy may not be in the object table yet.");
        LOG_INFO("[BigDaddy] Try approaching one to get it loaded, then fire again.");
    }
}

// ─── Plasmid Hijack System ──────────────────────────────────────────────
// Hooks ProcessEvent to watch for:
// 1. BeaconProjectile events → track its Location → teleport player on impact
// 2. SummonGathererDartProjectile events → track Location → summon BD on impact

bool InitPlasmidHijacks()
{
    if (s_Initialized) return true;

    if (!IsProcessEventHooked()) InitProcessEventHook();
    if (!CachePropertyOffsets()) {
        LOG_ERROR("[Plasmid] Failed to cache Location/Rotation offsets");
        return false;
    }

    // Diagnostic: track what events fire on projectiles
    static bool s_beaconDiagDone = false;
    static bool s_dartDiagDone = false;

    ProcessEventHook hook;
    hook.Name = "PlasmidHijacks";
    hook.FunctionFilter = "";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        std::string funcName = func->GetName();

        // ─── SECURITY BULLSEYE → TELEPORT ─────────────────────────
        // Log ALL events on BeaconProjectile to diagnose which ones fire
        if (IsA(obj, "BeaconProjectile") || obj->GetObjClassName() == "BeaconProjectile") {
            FVector projLoc = obj->GetField<FVector>(s_LocationOffset);

            if (!s_beaconDiagDone) {
                LOG_INFO("[Teleport][DIAG] BeaconProjectile event: {} @ ({:.0f}, {:.0f}, {:.0f})",
                         funcName, projLoc.X, projLoc.Y, projLoc.Z);
            }

            // Detect impact: any collision/touch/hit event
            if (funcName == "HitWall" || funcName == "Touch" ||
                funcName == "Landed" || funcName == "ProcessLanded" ||
                funcName == "OnAttached" || funcName == "EncroachingOn" ||
                funcName == "EncroachedBy" || funcName == "PhysicsVolumeChange" ||
                funcName == "TriggerEffectEvent" || funcName == "WeaponImpacted" ||
                funcName == "OnDetached") {

                if (projLoc.X != 0.0f || projLoc.Y != 0.0f || projLoc.Z != 0.0f) {
                    LOG_INFO("[Teleport] Beacon impact ({}) at ({:.0f}, {:.0f}, {:.0f})!",
                             funcName, projLoc.X, projLoc.Y, projLoc.Z);
                    DoTeleportTo(projLoc.X, projLoc.Y, projLoc.Z + 80.0f);
                    s_beaconDiagDone = true; // Only teleport once per throw
                }
                return true; // Block original
            }

            // Don't block movement/tick events — projectile needs to fly
            return false;
        }

        // ─── SECURITY BULLSEYE ABILITY FIRE ──────────────────────
        if (obj->GetObjClassName() == "SecurityBeaconAbility") {
            if (funcName == "UseAbility" || funcName == "StartedUsingAbility") {
                LOG_INFO("[Teleport] Security Bullseye fired — waiting for beacon impact...");
                s_beaconDiagDone = false; // Reset for this throw
                return false; // Let it fire the projectile
            }
        }

        // ─── HYPNOTIZE BIG DADDY → SUMMON ────────────────────────
        if (IsA(obj, "SummonGathererDartProjectile") ||
            obj->GetObjClassName() == "SummonGathererDartProjectile") {
            FVector projLoc = obj->GetField<FVector>(s_LocationOffset);

            if (!s_dartDiagDone) {
                LOG_INFO("[BigDaddy][DIAG] Dart event: {} @ ({:.0f}, {:.0f}, {:.0f})",
                         funcName, projLoc.X, projLoc.Y, projLoc.Z);
            }

            if (funcName == "HitWall" || funcName == "Touch" ||
                funcName == "Landed" || funcName == "ProcessLanded" ||
                funcName == "OnAttached" || funcName == "TriggerEffectEvent" ||
                funcName == "WeaponImpacted" || funcName == "OnDetached") {

                if (projLoc.X != 0.0f || projLoc.Y != 0.0f || projLoc.Z != 0.0f) {
                    LOG_INFO("[BigDaddy] Dart impact ({}) at ({:.0f}, {:.0f}, {:.0f})!",
                             funcName, projLoc.X, projLoc.Y, projLoc.Z);
                    SummonBigDaddyAt(projLoc.X, projLoc.Y, projLoc.Z + 80.0f);
                    s_dartDiagDone = true;
                }
                return true;
            }

            return false;
        }

        // ─── HYPNOTIZE ABILITY FIRE ──────────────────────────────
        if (obj->GetObjClassName() == "SummonProtectorAbility" ||
            obj->GetObjClassName() == "SummonProtectorTwoAbility") {
            if (funcName == "UseAbility" || funcName == "StartedUsingAbility") {
                LOG_INFO("[BigDaddy] Hypnotize fired — waiting for dart impact...");
                s_dartDiagDone = false;
                return false;
            }
        }

        return false;
    };

    s_HijackHookId = RegisterProcessEventHook(hook);
    s_Initialized = true;

    LOG_INFO("═══════════════════════════════════════════════════");
    LOG_INFO("[Plasmid Hijacks] ACTIVE!");
    LOG_INFO("  Security Bullseye → Teleport to impact point");
    LOG_INFO("  Hypnotize Big Daddy → Summon friendly BD at impact");
    LOG_INFO("═══════════════════════════════════════════════════");
    return true;
}

// ─── Distance Config ────────────────────────────────────────────────────

void SetTeleportDistance(float dist)
{
    s_TeleportDistance = dist;
    LOG_INFO("[Teleport] Fallback distance set to {:.0f}", dist);
}

float GetTeleportDistance()
{
    return s_TeleportDistance;
}

// ─── Status ─────────────────────────────────────────────────────────────

std::string GetPlasmidHijackStatus()
{
    if (!s_Initialized) return "Not initialized — run 'initplasmid'";
    return "Active: Bullseye→Teleport, Hypnotize→Summon BD";
}

} // namespace bs1sdk
