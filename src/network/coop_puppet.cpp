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

    // ── Strategy: "Borrow" an existing world actor instead of spawning ──
    // ProcessEvent(Spawn) doesn't work for native functions in BioShock
    // Remastered. Instead, find a distant StaticMeshActor, claim it as
    // our puppet, and move it to the partner's position.

    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) {
        LOG_WARN("[Puppet] No ShockPlayer found");
        return false;
    }

    // Find Location offset from player class (needed to measure distance)
    int locOff = -1;
    {
        UStruct* cls = reinterpret_cast<UStruct*>(player->GetClass());
        if (cls) {
            std::vector<PropertyInfo> allProps = WalkProperties(cls);
            PropertyInfo* pi = FindProperty(cls, "Location", allProps);
            if (pi) locOff = pi->Offset;
        }
    }

    // Read player location to find the most distant actor
    float playerX = 0, playerY = 0, playerZ = 0;
    if (locOff > 0) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(player);
        memcpy(&playerX, raw + locOff, 4);
        memcpy(&playerY, raw + locOff + 4, 4);
        memcpy(&playerZ, raw + locOff + 8, 4);
    }

    // Scan GObjects for a StaticMeshActor to borrow
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return false;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Prefer classes in this order — StaticMeshActor has 3000+ instances
    const char* borrowClasses[] = {
        "StaticMeshActor", "Decoration", "BasePlayerAttachment"
    };

    UObject* bestCandidate = nullptr;
    float bestDist = 0.0f;
    int candidatesScanned = 0;

    for (const char* bClass : borrowClasses) {
        if (bestCandidate) break;
        for (int i = 0; i < objCount && i < 100000; i++) {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
            if (!ptr) continue;
            UObject* obj = reinterpret_cast<UObject*>(ptr);
            if (obj->GetObjClassName() != bClass) continue;
            if (obj == player) continue;
            candidatesScanned++;

            if (locOff > 0) {
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                float ax, ay, az;
                memcpy(&ax, raw + locOff, 4);
                memcpy(&ay, raw + locOff + 4, 4);
                memcpy(&az, raw + locOff + 8, 4);

                float dx = ax - playerX, dy = ay - playerY, dz = az - playerZ;
                float dist = dx*dx + dy*dy + dz*dz;

                // Pick the farthest one (least likely to be noticed missing)
                if (dist > bestDist) {
                    bestDist = dist;
                    bestCandidate = obj;
                }
            } else {
                // No location offset — just grab first one
                bestCandidate = obj;
                break;
            }
        }
    }

    LOG_INFO("[Puppet] Scanned {} candidate actors, locOff={}", candidatesScanned, locOff);

    if (!bestCandidate) {
        LOG_WARN("[Puppet] No world actor found to borrow as puppet");
        return false;
    }

    s_Puppet = bestCandidate;
    LOG_INFO("[Puppet] Borrowed world actor: {} ({}) dist={:.0f}",
             s_Puppet->GetName(), s_Puppet->GetObjClassName(), sqrtf(bestDist));

    // Cache property offsets on the borrowed actor
    CachePuppetOffsets();

    // Configure: disable collision so it doesn't block anything
    int32_t bFalse = 0;
    int32_t bTrue = 1;
    SetPuppetProperty("bCollideActors", &bFalse, 4);
    SetPuppetProperty("bBlockActors", &bFalse, 4);
    SetPuppetProperty("bBlockPlayers", &bFalse, 4);
    SetPuppetProperty("bBlockHavok", &bFalse, 4);
    SetPuppetProperty("bHidden", &bFalse, 4);

    // Make it glow so it stands out as "the partner"
    SetPuppetProperty("bUnlit", &bTrue, 4);
    uint8_t ambientGlow = 254;
    SetPuppetProperty("AmbientGlow", &ambientGlow, 1);

    // Scale down so it's not huge — it'll be whatever mesh the borrowed
    // actor already had (barrel, crate, etc.) but smaller and glowing
    float drawScale = 0.35f;
    SetPuppetProperty("DrawScale", &drawScale, 4);

    s_IsAIPuppet = false;
    LOG_INFO("[Puppet] Puppet configured: no collision, glowing, scale={:.2f}", drawScale);

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
