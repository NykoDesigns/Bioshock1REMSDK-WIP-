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

// ─── Plasmid Initialization ─────────────────────────────────────────────

bool InitTeleportPlasmid()
{
    if (s_Initialized) return true;

    // Step 1: Find an existing plasmid class to clone
    // Look for any concrete ActivePlasmid subclass in the game
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) {
        LOG_ERROR("[Teleport] Engine globals not valid");
        return false;
    }

    // Find InsectSwarmPlasmid or any ActivePlasmid subclass as our donor
    UStruct* donorClass = FindClass("InsectSwarmPlasmid");
    if (!donorClass) donorClass = FindClass("ElectroBolt");
    if (!donorClass) donorClass = FindClass("Incinerate");
    if (!donorClass) donorClass = FindClass("ActivePlasmid");
    if (!donorClass) {
        // Try to find ANY class with "Plasmid" in its name that isn't abstract
        uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
        int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
        for (int i = 0; i < objCount && !donorClass; i++) {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
            if (!ptr) continue;
            UObject* obj = reinterpret_cast<UObject*>(ptr);
            if (obj->GetObjClassName() == "Class") {
                std::string name = obj->GetName();
                if (name.find("Plasmid") != std::string::npos &&
                    name != "Plasmid" && name != "ActivePlasmid" &&
                    name != "WeaponPlasmid" && name != "PhysicalPlasmid" &&
                    name != "EcologyPlasmid" && name != "EngineeringPlasmid") {
                    donorClass = reinterpret_cast<UStruct*>(obj);
                    LOG_INFO("[Teleport] Using donor class: {}", name);
                }
            }
        }
    }

    if (!donorClass) {
        LOG_ERROR("[Teleport] No suitable plasmid donor class found");
        return false;
    }

    LOG_INFO("[Teleport] Donor class: {}", reinterpret_cast<UObject*>(donorClass)->GetName());

    // Step 2: Clone it as "TeleportPlasmid"
    s_TeleportClass = CloneClass(
        reinterpret_cast<UObject*>(donorClass)->GetName(),
        "TeleportPlasmid");

    if (!s_TeleportClass) {
        LOG_ERROR("[Teleport] Failed to clone plasmid class");
        return false;
    }

    // Step 3: Hook ProcessEvent to intercept when our plasmid is "fired"
    // We look for the plasmid's fire/use function being called
    if (!IsProcessEventHooked()) InitProcessEventHook();

    ProcessEventHook hook;
    hook.Name = "TeleportPlasmid";
    hook.FunctionFilter = ""; // Watch all events to find plasmid activation
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        // Check if this is a plasmid firing event on an object that uses our class
        std::string funcName = func->GetName();

        // Common plasmid activation function names
        if (funcName == "InitiateDamage" || funcName == "BeginFiring" ||
            funcName == "OnFiringStarted") {
            // Check if the object's class is our TeleportPlasmid
            UObject* objClass = obj->GetClass();
            if (objClass == reinterpret_cast<UObject*>(s_TeleportClass)) {
                LOG_INFO("[Teleport] Plasmid activated! Teleporting...");
                DoTeleport(800.0f);
                return true; // Block the original behavior
            }
        }
        return false;
    };
    s_TeleportHookId = RegisterProcessEventHook(hook);

    s_Initialized = true;
    LOG_INFO("[Teleport] Custom TeleportPlasmid initialized successfully!");
    return true;
}

// ─── Give to Player ─────────────────────────────────────────────────────

bool GivePlayerTeleportPlasmid()
{
    if (!s_Initialized) {
        if (!InitTeleportPlasmid()) return false;
    }

    // The game's AddAvailablePlasmid takes a Class<Plasmid> argument.
    // Since we can't easily call native functions with class arguments through
    // our current function caller (which uses string params), we use a different
    // approach: directly add to the player's plasmid manager inventory.

    // For now, log that the plasmid is ready and can be triggered
    LOG_INFO("[Teleport] TeleportPlasmid is registered. Use 'teleport' command to activate.");
    LOG_INFO("[Teleport] The plasmid class exists at: 0x{:08X}",
             reinterpret_cast<uintptr_t>(s_TeleportClass));
    return true;
}

// ─── Status ─────────────────────────────────────────────────────────────

std::string GetTeleportStatus()
{
    if (!s_Initialized) return "Not initialized";
    return "Active (class at 0x" +
           std::to_string(reinterpret_cast<uintptr_t>(s_TeleportClass)) + ")";
}

} // namespace bs1sdk
