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

// Action tracking for visual effects
static int   s_LastAction = -1;       // ActionType or -1
static float s_ActionTimer = 999.0f;  // time since last action

// AI aggro timer — make nearby enemies target the puppet
static float s_AggroAccum = 0.0f;
constexpr float AGGRO_INTERVAL = 2.0f;  // every 2 seconds
constexpr float AGGRO_RANGE = 3000.0f;  // 30m radius
static UFunction* s_AddForcedEnemyFunc = nullptr;
static bool s_IsAIPuppet = false;       // true if puppet is a ShockPawn subclass

// Spawn retry cooldown — don't spam every frame
static float s_SpawnRetryTimer = 999.0f;  // start high so first attempt is immediate
constexpr float SPAWN_RETRY_INTERVAL = 5.0f;

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

/// Helper: check if an object inherits from a given base class name.
static bool PuppetIsA(UObject* obj, const std::string& baseName)
{
    if (!obj) return false;
    UObject* cls = obj->GetClass();
    UField* current = reinterpret_cast<UField*>(cls);
    int safety = 64;
    while (current && safety-- > 0) {
        if (current->GetName() == baseName) return true;
        current = reinterpret_cast<UField*>(
            reinterpret_cast<UStruct*>(current)->GetSuperField());
    }
    return false;
}

/// Make nearby enemies aggro the puppet by calling AddForcedEnemy.
static void AggroNearbyEnemies()
{
    if (!s_Puppet || !s_IsAIPuppet) return;

    ProcessEventFn origPE = GetOriginalProcessEvent();
    if (!origPE) return;

    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid() || s_PuppetLocOffset <= 0) return;

    // Get puppet position
    float px, py, pz;
    const uint8_t* praw = reinterpret_cast<const uint8_t*>(s_Puppet);
    memcpy(&px, praw + s_PuppetLocOffset, 4);
    memcpy(&py, praw + s_PuppetLocOffset + 4, 4);
    memcpy(&pz, praw + s_PuppetLocOffset + 8, 4);

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    int aggroCount = 0;
    for (int i = 0; i < objCount && i < 100000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj == s_Puppet) continue;

        std::string cn = obj->GetObjClassName();
        if (cn == "ShockPlayer" || cn == "ShockPlayerController" || cn == "Class")
            continue;

        // Only aggro ShockPawn enemies (check class hierarchy)
        if (!PuppetIsA(obj, "EcologyFighter") && !PuppetIsA(obj, "Aggressor"))
            continue;

        // Check if alive (needs Location offset — reuse puppet's cached offset)
        float ex, ey, ez;
        const uint8_t* eraw = reinterpret_cast<const uint8_t*>(obj);
        memcpy(&ex, eraw + s_PuppetLocOffset, 4);
        memcpy(&ey, eraw + s_PuppetLocOffset + 4, 4);
        memcpy(&ez, eraw + s_PuppetLocOffset + 8, 4);

        float dx = ex - px, dy = ey - py, dz = ez - pz;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq > AGGRO_RANGE * AGGRO_RANGE) continue;

        // Find AddForcedEnemy if not cached
        if (!s_AddForcedEnemyFunc) {
            UStruct* ecls = reinterpret_cast<UStruct*>(obj->GetClass());
            s_AddForcedEnemyFunc = FindFunctionOnClass(ecls, "AddForcedEnemy");
            if (!s_AddForcedEnemyFunc) continue;
        }

        // Call AddForcedEnemy(puppet) on this enemy
        // Parms: UObject* Target (ShockPawn)
        struct { UObject* Target; } parms;
        parms.Target = s_Puppet;
        origPE(obj, s_AddForcedEnemyFunc, &parms, nullptr);
        aggroCount++;

        if (aggroCount >= 8) break; // limit per tick
    }
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

    // Try each class in order of visual quality, falling through on failure.
    // AI classes (SpawnedDecoyHumanAI, DecoyHumanAI) often fail because they
    // need BSM archetype data. BasePlayerAttachment is the reliable fallback.
    const char* classNames[] = {
        "SpawnedDecoyHumanAI",     // human model (Decoy plasmid)
        "DecoyHumanAI",            // abstract but might work
        "BasePlayerAttachment",    // cube — always works
        "StaticMeshActor",         // basic mesh
    };

    // Actor.Spawn parms layout (UE2):
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

    for (const char* className : classNames) {
        UObject* spawnClass = FindClassObject(className);
        if (!spawnClass) {
            LOG_INFO("[Puppet] Class '{}' not found, trying next...", className);
            continue;
        }

        LOG_INFO("[Puppet] Trying to spawn {} at ({:.0f}, {:.0f}, {:.0f})",
                 className, x, y, z);

        SpawnParms parms{};
        parms.SpawnClass = spawnClass;
        parms.SpawnOwner = player;
        parms.SpawnTag[0] = 0; parms.SpawnTag[1] = 0;
        parms.SpawnLocX = x;
        parms.SpawnLocY = y;
        parms.SpawnLocZ = z;
        parms.SpawnRotPitch = 0;
        parms.SpawnRotYaw = 0;
        parms.SpawnRotRoll = 0;
        parms.bNoCollisionFail = 1;
        parms.SpawnLabel[0] = 0; parms.SpawnLabel[1] = 0;
        parms.ReturnValue = nullptr;

        origPE(player, s_SpawnFunc, &parms, nullptr);

        if (parms.ReturnValue) {
            s_Puppet = parms.ReturnValue;
            LOG_INFO("[Puppet] SUCCESS — spawned '{}' as puppet", className);
            break;
        }

        LOG_WARN("[Puppet] Spawn '{}' returned null, trying next...", className);
    }

    if (!s_Puppet) {
        LOG_WARN("[Puppet] All spawn classes failed — will retry in 5 seconds");
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
    SetPuppetProperty("bBlockNonZeroExtentTraces", &bFalse, 4);
    SetPuppetProperty("bBlockZeroExtentTraces", &bFalse, 4);
    SetPuppetProperty("bHidden", &bFalse, 4);
    SetPuppetProperty("bInGameRenderable", &bTrue, 4);

    // Make invincible so enemies can't kill our puppet
    float bigHP = 99999.0f;
    SetPuppetProperty("Health", &bigHP, 4);
    SetPuppetProperty("HealthMax", &bigHP, 4);
    SetPuppetProperty("bIsInvincible", &bTrue, 4);

    // Disable AI brain if this is a ShockAI/DecoyHuman subclass
    std::string puppetClass = s_Puppet->GetObjClassName();
    bool isAIPuppet = (puppetClass.find("Decoy") != std::string::npos ||
                       puppetClass.find("ShockAI") != std::string::npos ||
                       puppetClass.find("Aggressor") != std::string::npos);

    s_IsAIPuppet = isAIPuppet;
    if (isAIPuppet) {
        // Prevent AI from running — set LifeSpan to 0 (infinite) and disable AI tick
        float noLifeSpan = 0.0f;
        SetPuppetProperty("LifeSpan", &noLifeSpan, 4);
        // Make sure it doesn't try to pathfind or attack
        SetPuppetProperty("bCanWalk", &bFalse, 4);
        SetPuppetProperty("bCanFly", &bFalse, 4);
        SetPuppetProperty("bCanSwim", &bFalse, 4);
        // Human-scale draw for AI puppet
        float drawScale = 1.0f;
        SetPuppetProperty("DrawScale", &drawScale, 4);

        // Cache AddForcedEnemy for enemy aggro
        // We'll find it on EcologyFighter/Aggressor classes later when needed
        s_AddForcedEnemyFunc = nullptr;

        LOG_INFO("[Puppet] AI puppet configured: disabled movement/AI, set invincible");
    } else {
        // StaticMesh puppet — scale down the cube to human-ish size
        float drawScale = 0.25f; // ~64 units
        SetPuppetProperty("DrawScale", &drawScale, 4);
    }

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
        // Retry spawn with cooldown to avoid log spam
        s_SpawnRetryTimer += (1.0f / 60.0f);
        if (s_SpawnRetryTimer >= SPAWN_RETRY_INTERVAL) {
            s_SpawnRetryTimer = 0.0f;
            SpawnGhostPuppet(s_TargetX, s_TargetY, s_TargetZ);
        }
        return;
    }

    // Tick the action timer
    float dt = 1.0f / 60.0f; // approximate frame time
    s_ActionTimer += dt;

    // Periodically make nearby enemies aggro the puppet
    if (s_IsAIPuppet) {
        s_AggroAccum += dt;
        if (s_AggroAccum >= AGGRO_INTERVAL) {
            s_AggroAccum = 0.0f;
            AggroNearbyEnemies();
        }
    }

    // Smooth interpolation toward target
    float lerpSpeed = 10.0f; // higher = snappier
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

void NotifyPuppetAction(const PlayerActionData& action)
{
    s_LastAction = static_cast<int>(action.action);
    s_ActionTimer = 0.0f;
    LOG_INFO("[Puppet] Remote player action: {}", s_LastAction);
}

int GetPuppetLastAction(float& timeSinceAction)
{
    timeSinceAction = s_ActionTimer;
    if (s_ActionTimer > 1.0f) return -1; // expired after 1 second
    return s_LastAction;
}

} // namespace bs1sdk
