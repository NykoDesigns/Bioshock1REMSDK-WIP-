#include "teleport_plasmid.h"
#include "../engine/uobject.h"
#include "../engine/class_factory.h"
#include "../hooks/process_event.h"
#include "../core/log.h"
#include <cmath>
#include <atomic>
#include <Windows.h>

namespace bs1sdk {

// ─── State ──────────────────────────────────────────────────────────────

static int s_HijackHookId = -1;
static bool s_Initialized = false;
static float s_TeleportDistance = 800.0f;

// Projectile spawn time tracking — ignore events in the first N ms after spawn
static DWORD s_BeaconSpawnTime = 0;
static DWORD s_DartSpawnTime = 0;
static constexpr DWORD PROJECTILE_MIN_FLIGHT_MS = 150; // Ignore events within 150ms of spawn

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

    // Per-throw state
    static bool s_beaconTeleported = false;
    static bool s_dartSummoned = false;

    ProcessEventHook hook;
    hook.Name = "PlasmidHijacks";
    hook.FunctionFilter = "";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        std::string funcName = func->GetName();

        // ─── SECURITY BULLSEYE → TELEPORT ─────────────────────────
        // From diagnostics: events are PreBeginPlay, BeginPlay, ZoneChange,
        // PhysicsVolumeChange (at spawn), then Tick... then BaseChange/Destroyed
        // at impact. We use Destroyed as the reliable "it hit something" signal,
        // reading its final Location right before it's destroyed.
        if (IsA(obj, "BeaconProjectile") || obj->GetObjClassName() == "BeaconProjectile") {

            // Record spawn time on PreBeginPlay
            if (funcName == "PreBeginPlay") {
                s_BeaconSpawnTime = GetTickCount();
                s_beaconTeleported = false;
                LOG_INFO("[Teleport] Beacon spawned — tracking...");
                return false;
            }

            // Ignore events within first 150ms (these are spawn-time false positives)
            DWORD elapsed = GetTickCount() - s_BeaconSpawnTime;
            if (elapsed < PROJECTILE_MIN_FLIGHT_MS) {
                return false;
            }

            // Impact events: BaseChange or Destroyed (confirmed from log)
            if (!s_beaconTeleported &&
                (funcName == "BaseChange" || funcName == "Destroyed" ||
                 funcName == "EndState" || funcName == "HitWall" ||
                 funcName == "Touch" || funcName == "Landed")) {

                FVector projLoc = obj->GetField<FVector>(s_LocationOffset);

                if (projLoc.X != 0.0f || projLoc.Y != 0.0f || projLoc.Z != 0.0f) {
                    LOG_INFO("[Teleport] Beacon hit ({}) at ({:.0f}, {:.0f}, {:.0f}) after {}ms — teleporting!",
                             funcName, projLoc.X, projLoc.Y, projLoc.Z, elapsed);
                    DoTeleportTo(projLoc.X, projLoc.Y, projLoc.Z + 80.0f);
                    s_beaconTeleported = true;
                }
                return false; // Don't block Destroyed — let cleanup happen
            }

            return false;
        }

        // ─── HYPNOTIZE BIG DADDY → SUMMON ────────────────────────
        // From diagnostics: events are PreBeginPlay, BeginPlay, Tick...,
        // ZoneChange, then GainedChild, BaseChange, EndState, Destroyed at impact.
        if (IsA(obj, "SummonGathererDartProjectile") ||
            obj->GetObjClassName() == "SummonGathererDartProjectile") {

            if (funcName == "PreBeginPlay") {
                s_DartSpawnTime = GetTickCount();
                s_dartSummoned = false;
                LOG_INFO("[BigDaddy] Dart spawned — tracking...");
                return false;
            }

            DWORD elapsed = GetTickCount() - s_DartSpawnTime;
            if (elapsed < PROJECTILE_MIN_FLIGHT_MS) {
                return false;
            }

            // Impact: BaseChange or Destroyed (confirmed from log at end of flight)
            if (!s_dartSummoned &&
                (funcName == "BaseChange" || funcName == "Destroyed" ||
                 funcName == "EndState" || funcName == "HitWall" ||
                 funcName == "Touch" || funcName == "Landed")) {

                FVector projLoc = obj->GetField<FVector>(s_LocationOffset);

                if (projLoc.X != 0.0f || projLoc.Y != 0.0f || projLoc.Z != 0.0f) {
                    LOG_INFO("[BigDaddy] Dart hit ({}) at ({:.0f}, {:.0f}, {:.0f}) after {}ms — summoning!",
                             funcName, projLoc.X, projLoc.Y, projLoc.Z, elapsed);
                    SummonBigDaddyAt(projLoc.X, projLoc.Y, projLoc.Z + 80.0f);
                    s_dartSummoned = true;
                }
                return false; // Don't block cleanup
            }

            return false;
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
