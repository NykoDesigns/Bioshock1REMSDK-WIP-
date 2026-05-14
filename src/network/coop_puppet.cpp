#include "coop_puppet.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../hooks/process_event.h"

#include <cstring>
#include <cmath>
#include <string>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static UObject*   s_Puppet = nullptr;      // The spawned actor
static UFunction* s_SpawnFunc = nullptr;    // Cached Actor.Spawn function
static UFunction* s_DestroyFunc = nullptr;  // Cached Actor.Destroy function
static bool       s_Initialized = false;

// Property offsets on the puppet (cached once after spawn)
static int s_PuppetLocOffset = -1;
static int s_PuppetRotOffset = -1;

// Smoothed position for interpolation
static float s_InterpX = 0, s_InterpY = 0, s_InterpZ = 0;
static float s_TargetX = 0, s_TargetY = 0, s_TargetZ = 0;
static float s_TargetPitch = 0, s_TargetYaw = 0;
static bool  s_HasTarget = false;

// ─── Helpers ───────────────────────────────────────────────────────────

/// Find a UFunction by name on a class, walking the Children linked list.
static UFunction* FindFunctionOnClass(UStruct* cls, const char* funcName)
{
    if (!cls) return nullptr;
    UField* child = cls->GetChildren();
    int limit = 4000;
    while (child && limit-- > 0) {
        if (child->GetObjClassName() == "Function" && child->GetName() == funcName) {
            return reinterpret_cast<UFunction*>(child);
        }
        child = child->GetNext();
    }
    // Walk super class
    UField* super = cls->GetSuperField();
    if (super && limit > 0) {
        return FindFunctionOnClass(reinterpret_cast<UStruct*>(super), funcName);
    }
    return nullptr;
}

/// Find a UClass* object by class name from GObjects.
static UObject* FindClassObject(const char* className)
{
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return nullptr;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    for (int i = 0; i < objCount && i < 100000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == "Class" && obj->GetName() == className) {
            return obj;
        }
    }
    return nullptr;
}

/// Cache property offsets on the puppet once after spawning.
static void CachePuppetOffsets()
{
    if (!s_Puppet || s_PuppetLocOffset > 0) return;

    UStruct* cls = reinterpret_cast<UStruct*>(s_Puppet->GetClass());
    if (!cls) return;

    std::vector<PropertyInfo> props = WalkProperties(cls);
    PropertyInfo* loc = FindProperty(cls, "Location", props);
    if (loc) s_PuppetLocOffset = loc->Offset;
    PropertyInfo* rot = FindProperty(cls, "Rotation", props);
    if (rot) s_PuppetRotOffset = rot->Offset;

    LOG_INFO("[Puppet] Cached offsets: Loc={} Rot={}", s_PuppetLocOffset, s_PuppetRotOffset);
}

/// Set a property on the puppet by name.
static bool SetPuppetProperty(const char* propName, const void* value, int size)
{
    if (!s_Puppet) return false;
    UStruct* cls = reinterpret_cast<UStruct*>(s_Puppet->GetClass());
    if (!cls) return false;

    std::vector<PropertyInfo> props = WalkProperties(cls);
    PropertyInfo* pi = FindProperty(cls, propName, props);
    if (!pi) return false;

    uint8_t* raw = reinterpret_cast<uint8_t*>(s_Puppet);
    memcpy(raw + pi->Offset, value, size);
    return true;
}

// ─── Public API ────────────────────────────────────────────────────────

bool InitGhostPuppet()
{
    if (s_Initialized) return true;

    // Cache the Spawn function from the Actor class
    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) {
        LOG_WARN("[Puppet] No ShockPlayer found — can't cache Spawn/Destroy");
        return false;
    }

    UStruct* playerClass = reinterpret_cast<UStruct*>(player->GetClass());
    s_SpawnFunc = FindFunctionOnClass(playerClass, "Spawn");
    s_DestroyFunc = FindFunctionOnClass(playerClass, "Destroy");

    if (!s_SpawnFunc) {
        LOG_WARN("[Puppet] Could not find Actor.Spawn function");
        return false;
    }

    s_Initialized = true;
    LOG_INFO("[Puppet] Ghost puppet system initialized (Spawn={}, Destroy={})",
             (void*)s_SpawnFunc, (void*)s_DestroyFunc);
    return true;
}

void ShutdownGhostPuppet()
{
    DestroyGhostPuppet();
    s_SpawnFunc = nullptr;
    s_DestroyFunc = nullptr;
    s_Initialized = false;
}

bool SpawnGhostPuppet(float x, float y, float z)
{
    if (!s_Initialized) {
        if (!InitGhostPuppet()) return false;
    }

    // Already have one?
    if (s_Puppet) return true;

    ProcessEventFn origPE = GetOriginalProcessEvent();
    if (!origPE) {
        LOG_WARN("[Puppet] No ProcessEvent — can't spawn");
        return false;
    }

    // Find the local player to call Spawn on
    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) {
        LOG_WARN("[Puppet] No ShockPlayer for Spawn call");
        return false;
    }

    // Find the class to spawn. We'll try multiple options in order of preference:
    //   1. BasePlayerAttachment — simple visible StaticMesh actor
    //   2. StaticMeshActor — basic visible mesh
    //   3. Actor — fallback (may not be visible)
    UObject* spawnClass = FindClassObject("BasePlayerAttachment");
    if (!spawnClass) spawnClass = FindClassObject("StaticMeshActor");
    if (!spawnClass) spawnClass = FindClassObject("Actor");

    if (!spawnClass) {
        LOG_WARN("[Puppet] Could not find any suitable class to spawn");
        return false;
    }

    LOG_INFO("[Puppet] Spawning {} at ({:.0f}, {:.0f}, {:.0f})",
             spawnClass->GetName(), x, y, z);

    // Actor.Spawn(Class SpawnClass, optional Actor SpawnOwner,
    //             optional name SpawnTag, optional Vector SpawnLocation,
    //             optional Rotator SpawnRotation, optional bool bNoCollisionFail,
    //             optional name SpawnLabel)
    //
    // Parms layout (UE2 packed for optionals):
    //   +0x00: UObject* SpawnClass          (4)
    //   +0x04: UObject* SpawnOwner          (4)  [optional]
    //   +0x08: FName    SpawnTag            (8)  [optional]
    //   +0x10: FVector  SpawnLocation       (12) [optional]
    //   +0x1C: FRotator SpawnRotation       (12) [optional]
    //   +0x28: uint32   bNoCollisionFail    (4)  [optional, bool stored as int32]
    //   +0x2C: FName    SpawnLabel          (8)  [optional]
    //   +0x34: UObject* ReturnValue         (4)
    struct SpawnParms {
        UObject* SpawnClass;          // +0x00
        UObject* SpawnOwner;          // +0x04
        uint32_t SpawnTag[2];         // +0x08 FName (index + instance)
        float    SpawnLocX;           // +0x10
        float    SpawnLocY;           // +0x14
        float    SpawnLocZ;           // +0x18
        int32_t  SpawnRotPitch;       // +0x1C
        int32_t  SpawnRotYaw;         // +0x20
        int32_t  SpawnRotRoll;        // +0x24
        int32_t  bNoCollisionFail;    // +0x28
        uint32_t SpawnLabel[2];       // +0x2C FName
        UObject* ReturnValue;         // +0x34
    };

    SpawnParms parms{};
    parms.SpawnClass = spawnClass;
    parms.SpawnOwner = player;
    parms.SpawnTag[0] = 0; parms.SpawnTag[1] = 0;  // 'None'
    parms.SpawnLocX = x;
    parms.SpawnLocY = y;
    parms.SpawnLocZ = z;
    parms.SpawnRotPitch = 0;
    parms.SpawnRotYaw = 0;
    parms.SpawnRotRoll = 0;
    parms.bNoCollisionFail = 1; // true — don't fail on collision
    parms.SpawnLabel[0] = 0; parms.SpawnLabel[1] = 0;
    parms.ReturnValue = nullptr;

    // Call Actor.Spawn via ProcessEvent
    origPE(player, s_SpawnFunc, &parms, nullptr);

    s_Puppet = parms.ReturnValue;
    if (!s_Puppet) {
        LOG_WARN("[Puppet] Spawn returned null — actor creation failed");
        return false;
    }

    LOG_INFO("[Puppet] Spawned ghost puppet: {} ({})",
             s_Puppet->GetName(), s_Puppet->GetObjClassName());

    // Cache property offsets
    CachePuppetOffsets();

    // Configure the puppet: no collision, no AI targeting, visible
    int32_t bFalse = 0;
    int32_t bTrue = 1;
    SetPuppetProperty("bCollideActors", &bFalse, 4);
    SetPuppetProperty("bBlockActors", &bFalse, 4);
    SetPuppetProperty("bBlockPlayers", &bFalse, 4);
    SetPuppetProperty("bBlockHavok", &bFalse, 4);
    SetPuppetProperty("bHidden", &bFalse, 4);
    SetPuppetProperty("bInGameRenderable", &bTrue, 4);

    // Set a reasonable draw scale so it's visible but not huge
    // BasePlayerAttachment default is a 256-diameter cube; scale down to ~human size
    float drawScale = 0.25f; // ~64 units ≈ human height ish
    SetPuppetProperty("DrawScale", &drawScale, 4);

    // Make it glow / unlit so it's always visible
    SetPuppetProperty("bUnlit", &bTrue, 4);

    // Set initial position
    s_InterpX = x; s_InterpY = y; s_InterpZ = z;
    s_TargetX = x; s_TargetY = y; s_TargetZ = z;
    s_HasTarget = true;

    return true;
}

void UpdateGhostPuppet(const PlayerStateData& remoteState)
{
    // Update target position
    s_TargetX = remoteState.posX;
    s_TargetY = remoteState.posY;
    s_TargetZ = remoteState.posZ;
    s_TargetPitch = remoteState.rotPitch;
    s_TargetYaw = remoteState.rotYaw;
    s_HasTarget = true;

    if (!s_Puppet) {
        // Try to spawn if we don't have one yet
        SpawnGhostPuppet(s_TargetX, s_TargetY, s_TargetZ);
        return;
    }

    // Smooth interpolation toward target
    float lerpSpeed = 10.0f; // higher = snappier
    float dt = 1.0f / 60.0f; // approximate; will use real dt when available
    float t = 1.0f - expf(-lerpSpeed * dt);

    s_InterpX += (s_TargetX - s_InterpX) * t;
    s_InterpY += (s_TargetY - s_InterpY) * t;
    s_InterpZ += (s_TargetZ - s_InterpZ) * t;

    // Write position
    if (s_PuppetLocOffset > 0) {
        uint8_t* raw = reinterpret_cast<uint8_t*>(s_Puppet);
        memcpy(raw + s_PuppetLocOffset, &s_InterpX, 4);
        memcpy(raw + s_PuppetLocOffset + 4, &s_InterpY, 4);
        memcpy(raw + s_PuppetLocOffset + 8, &s_InterpZ, 4);
    }

    // Write rotation
    if (s_PuppetRotOffset > 0) {
        int32_t pitch = (int32_t)(s_TargetPitch * (65536.0f / 360.0f));
        int32_t yaw = (int32_t)(s_TargetYaw * (65536.0f / 360.0f));
        int32_t roll = 0;
        uint8_t* raw = reinterpret_cast<uint8_t*>(s_Puppet);
        memcpy(raw + s_PuppetRotOffset, &pitch, 4);
        memcpy(raw + s_PuppetRotOffset + 4, &yaw, 4);
        memcpy(raw + s_PuppetRotOffset + 8, &roll, 4);
    }
}

void DestroyGhostPuppet()
{
    if (!s_Puppet) return;

    ProcessEventFn origPE = GetOriginalProcessEvent();
    if (origPE && s_DestroyFunc) {
        // Destroy() returns bool, no params
        struct { int32_t ReturnValue; } parms{};
        origPE(s_Puppet, s_DestroyFunc, &parms, nullptr);
        LOG_INFO("[Puppet] Destroyed ghost puppet");
    } else {
        // Fallback: just hide it
        int32_t bTrue = 1;
        SetPuppetProperty("bHidden", &bTrue, 4);
        LOG_WARN("[Puppet] Hid puppet (no Destroy function available)");
    }

    s_Puppet = nullptr;
    s_PuppetLocOffset = -1;
    s_PuppetRotOffset = -1;
    s_HasTarget = false;
}

bool HasGhostPuppet()
{
    return s_Puppet != nullptr;
}

} // namespace bs1sdk
