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

// ─── Big Daddy Summoning ────────────────────────────────────────────────
// We spawn a friendly Bouncer at the impact position by finding an existing
// Protector in the level and moving it to the target, or by using the
// engine's ConsoleCommand to summon one.

static void SummonBigDaddyAt(float x, float y, float z)
{
    // Strategy: Find an existing ShockPlayerController and use ConsoleCommand
    // "summon ShockAI.Bouncer" — but this spawns at player position.
    // Instead, we find ALL Protector-type objects in the level.
    // If one exists, teleport it to the target and make it friendly.
    // If none exist, we use the forward-spawn trick.

    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    if (!CachePropertyOffsets()) return;

    // Look for any Bouncer or Rosie in the level
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    UObject* protector = nullptr;
    for (int i = 0; i < objCount && !protector; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        std::string cls = obj->GetObjClassName();
        // Look for concrete Bouncer/Rosie instances (not class objects)
        if (cls == "Bouncer" || cls == "Rosie" ||
            cls == "BouncerElite" || cls == "RosieElite") {
            protector = obj;
        }
    }

    if (protector) {
        // Move an existing Big Daddy to the target location and make it friendly
        FVector targetLoc = { x, y, z };
        protector->SetField<FVector>(s_LocationOffset, targetLoc);

        // Try to set DispositionToPlayer to kFriendlyToPlayer (2)
        // DispositionToPlayer is on the Protector class
        UStruct* protClass = FindClass(protector->GetObjClassName());
        if (protClass) {
            auto props = WalkProperties(protClass);
            for (auto& p : props) {
                if (p.Name == "DispositionToPlayer") {
                    // EDispositionToPlayer: 0=None, 1=Hostile, 2=Friendly
                    protector->SetField<int32_t>(p.Offset, 2);
                    break;
                }
                if (p.Name == "bProtectingPlayer") {
                    protector->SetField<int32_t>(p.Offset, 1);
                    break;
                }
            }
        }

        LOG_INFO("[BigDaddy] Summoned friendly {} at ({:.0f}, {:.0f}, {:.0f})",
                 protector->GetObjClassName(), x, y, z);
    } else {
        // No existing Big Daddy found — teleport player there and log
        LOG_WARN("[BigDaddy] No Bouncer/Rosie found in level to relocate!");
        LOG_INFO("[BigDaddy] Tip: There must be a Big Daddy spawned in the level.");
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

    ProcessEventHook hook;
    hook.Name = "PlasmidHijacks";
    hook.FunctionFilter = "";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        std::string className = obj->GetObjClassName();
        std::string funcName = func->GetName();

        // ─── SECURITY BULLSEYE → TELEPORT ─────────────────────────
        // Track the BeaconProjectile. When it hits something (HitWall/Touch/Landed),
        // read its Location and teleport the player there.
        if (className == "BeaconProjectile") {
            if (funcName == "HitWall" || funcName == "Touch" ||
                funcName == "Landed" || funcName == "ProcessLanded" ||
                funcName == "OnAttached" || funcName == "EncroachingOn" ||
                funcName == "EncroachedBy") {
                // Projectile hit something — read its location
                FVector impactLoc = obj->GetField<FVector>(s_LocationOffset);

                // Sanity check — skip if location is zero (uninitialized)
                if (impactLoc.X != 0.0f || impactLoc.Y != 0.0f || impactLoc.Z != 0.0f) {
                    LOG_INFO("[Teleport] Beacon hit at ({:.0f}, {:.0f}, {:.0f}) — teleporting!",
                             impactLoc.X, impactLoc.Y, impactLoc.Z);
                    // Teleport player to impact point (slightly above to avoid clipping)
                    DoTeleportTo(impactLoc.X, impactLoc.Y, impactLoc.Z + 50.0f);
                }
                return true; // Block original beacon behavior
            }

            // Block all other BeaconProjectile events to prevent tagging enemies
            if (funcName == "PostBeginPlay" || funcName == "Tick") {
                // Let these through so the projectile actually moves
                return false;
            }
        }

        // ─── HYPNOTIZE BIG DADDY → SUMMON ────────────────────────
        // Track the SummonGathererDartProjectile. When it hits, summon a
        // friendly Big Daddy at that location.
        if (className == "SummonGathererDartProjectile" ||
            className == "CrossbowProjectile") {
            // Check if this is the hypnotize dart specifically
            // CrossbowProjectile is parent class, but we only want the summon dart
            if (className == "SummonGathererDartProjectile") {
                if (funcName == "HitWall" || funcName == "Touch" ||
                    funcName == "Landed" || funcName == "ProcessLanded" ||
                    funcName == "OnAttached") {
                    FVector impactLoc = obj->GetField<FVector>(s_LocationOffset);

                    if (impactLoc.X != 0.0f || impactLoc.Y != 0.0f || impactLoc.Z != 0.0f) {
                        LOG_INFO("[BigDaddy] Dart hit at ({:.0f}, {:.0f}, {:.0f}) — summoning!",
                                 impactLoc.X, impactLoc.Y, impactLoc.Z);
                        SummonBigDaddyAt(impactLoc.X, impactLoc.Y, impactLoc.Z + 50.0f);
                    }
                    return true; // Block original hypnotize behavior
                }
            }
        }

        // ─── Block the original ability effects ──────────────────
        // Prevent SecurityBeaconAbility from applying its security tag
        if (className == "SecurityBeaconAbility") {
            if (funcName == "UseAbility") {
                // Let it fire the projectile (we need the projectile to track)
                // but log it
                LOG_INFO("[Teleport] Security Bullseye fired — tracking projectile...");
                return false; // Don't block, let projectile spawn
            }
        }

        // Prevent SummonProtectorAbility from doing its normal thing
        if (className == "SummonProtectorAbility" ||
            className == "SummonProtectorTwoAbility") {
            if (funcName == "UseAbility") {
                LOG_INFO("[BigDaddy] Hypnotize fired — tracking dart...");
                return false; // Don't block, let dart spawn
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
