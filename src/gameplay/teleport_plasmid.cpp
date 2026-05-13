#include "teleport_plasmid.h"
#include "../engine/uobject.h"
#include "../engine/class_factory.h"
#include "../hooks/process_event.h"
#include "../core/log.h"
#include <cmath>

namespace bs1sdk {

// ─── State ──────────────────────────────────────────────────────────────

static UStruct* s_TeleportClass = nullptr;
static int s_TeleportHookId = -1;
static bool s_Initialized = false;
static float s_TeleportDistance = 800.0f;

// ─── Math helpers ───────────────────────────────────────────────────────

struct FVector {
    float X, Y, Z;
};

struct FRotator {
    int32_t Pitch, Yaw, Roll; // UE2 uses int32 rotator units (0-65535 = 0-360)
};

static constexpr float PI = 3.14159265358979f;
static constexpr float URot2Rad = PI / 32768.0f; // UE rotation units to radians

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

// ─── Teleport Logic ─────────────────────────────────────────────────────

bool DoTeleport(float distance)
{
    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) {
        LOG_ERROR("[Teleport] No player found");
        return false;
    }

    UStruct* cls = FindClass("ShockPlayer");
    if (!cls) return false;

    auto props = WalkProperties(cls);
    PropertyInfo* locProp = nullptr;
    PropertyInfo* rotProp = nullptr;

    for (auto& p : props) {
        if (p.Name == "Location") locProp = &p;
        if (p.Name == "Rotation") rotProp = &p;
    }

    if (!locProp || !rotProp) {
        LOG_ERROR("[Teleport] Could not find Location/Rotation properties");
        return false;
    }

    // Read current position and rotation
    FVector loc = player->GetField<FVector>(locProp->Offset);
    FRotator rot = player->GetField<FRotator>(rotProp->Offset);

    // Calculate forward direction
    FVector dir = RotationToDirection(rot);

    // Move forward by distance
    FVector newLoc;
    newLoc.X = loc.X + dir.X * distance;
    newLoc.Y = loc.Y + dir.Y * distance;
    newLoc.Z = loc.Z + dir.Z * distance;

    // Write new position
    player->SetField<FVector>(locProp->Offset, newLoc);

    LOG_INFO("[Teleport] Moved from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f})",
             loc.X, loc.Y, loc.Z, newLoc.X, newLoc.Y, newLoc.Z);
    return true;
}

// ─── Plasmid Hijack ─────────────────────────────────────────────────────
// Hijacks the Security Bullseye plasmid (SecurityBeaconAbility).
// When the player fires Security Bullseye, we block the projectile
// and teleport them forward instead.

bool InitTeleportPlasmid()
{
    if (s_Initialized) return true;

    if (!IsProcessEventHooked()) InitProcessEventHook();

    // Hook UseAbility on SecurityBeaconAbility objects.
    // The call chain is:
    //   Player fires plasmid → SecurityBeaconAbility.UseAbility()
    //                        → spawns BeaconProjectile
    // We intercept UseAbility and block it, doing teleport instead.
    //
    // We also catch the projectile spawn as a fallback.

    ProcessEventHook hook;
    hook.Name = "TeleportHijack";
    hook.FunctionFilter = ""; // Need to check multiple function names
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        std::string funcName = func->GetName();
        std::string className = obj->GetObjClassName();

        // Primary: intercept when SecurityBeaconAbility fires
        if (className == "SecurityBeaconAbility") {
            if (funcName == "UseAbility" || funcName == "StartedUsingAbility") {
                LOG_INFO("[Teleport] Security Bullseye fired → Teleporting {:.0f} units!",
                         s_TeleportDistance);
                DoTeleport(s_TeleportDistance);
                return true; // Block the original projectile
            }
        }

        // Fallback: block the beacon projectile from spawning
        if (className == "BeaconProjectile") {
            return true; // Block any BeaconProjectile events
        }

        return false;
    };

    s_TeleportHookId = RegisterProcessEventHook(hook);
    s_Initialized = true;

    LOG_INFO("[Teleport] Hijacked Security Bullseye → Teleport Plasmid!");
    LOG_INFO("[Teleport] Buy/equip Security Bullseye at any Gatherer's Garden, fire it to teleport.");
    LOG_INFO("[Teleport] Distance: {:.0f} units (change with 'tpdist <n>')", s_TeleportDistance);
    return true;
}

// ─── Give to Player ─────────────────────────────────────────────────────

bool GivePlayerTeleportPlasmid()
{
    if (!s_Initialized) {
        if (!InitTeleportPlasmid()) return false;
    }

    LOG_INFO("[Teleport] Hijack active on Security Bullseye.");
    LOG_INFO("[Teleport] If you already have Security Bullseye, equip and fire it.");
    LOG_INFO("[Teleport] Otherwise, buy it at any Gatherer's Garden.");
    return true;
}

// ─── Distance Config ────────────────────────────────────────────────────

void SetTeleportDistance(float dist)
{
    s_TeleportDistance = dist;
    LOG_INFO("[Teleport] Distance set to {:.0f}", dist);
}

float GetTeleportDistance()
{
    return s_TeleportDistance;
}

// ─── Status ─────────────────────────────────────────────────────────────

std::string GetTeleportStatus()
{
    if (!s_Initialized) return "Not initialized — run 'initplasmid'";
    return "Active: Security Bullseye → Teleport (" +
           std::to_string((int)s_TeleportDistance) + " units)";
}

} // namespace bs1sdk
