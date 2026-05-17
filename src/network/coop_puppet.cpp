#include "coop_puppet.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../engine/world.h"
#include "../hooks/process_event.h"
#include "../debug/crash_handler.h"

#include <Windows.h>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static UObject*   s_Puppet = nullptr;      // The spawned actor
static UFunction* s_SpawnFunc = nullptr;    // Cached Actor.Spawn function
static UFunction* s_DestroyFunc = nullptr;  // Cached Actor.Destroy function
static UFunction* s_SetLocFunc = nullptr;   // Cached Actor.SetLocation (updates octree!)
static UFunction* s_SetRotFunc = nullptr;   // Cached Actor.SetRotation (updates octree!)
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

// Direct MoveActor call (bypasses ProcessEvent entirely)
// ULevel::MoveActor signature: INT __thiscall MoveActor(AActor*, FVector Delta, FRotator DeltaRot, DWORD Flags, FCheckResult* Hit)
// In MSVC 32-bit thiscall, FVector/FRotator POD structs are placed directly on stack.
typedef int (__thiscall *MoveActorFn)(
    void* thisLevel,                              // ULevel* in ECX
    void* actor,                                  // AActor*
    float dX, float dY, float dZ,                 // FVector Delta (by value)
    int rotPitch, int rotYaw, int rotRoll,         // FRotator DeltaRot (by value)
    unsigned int flags,                            // DWORD MoveFlags
    void* hitResult                                // FCheckResult* (NULL)
);

static MoveActorFn s_MoveActorFn = nullptr;
static void*       s_ULevel = nullptr;
static int         s_XLevelOffset = -1;  // offset of XLevel on Actor

// Diagnosis results (set by DiagnoseAndDiscover, used by UpdateGhostPuppet)
static uintptr_t s_SetLocNativeAddr = 0;
static uintptr_t s_SetRotNativeAddr = 0;
static bool      s_MoveActorDiscovered = false;

/// Scan the first N bytes of a native function for E8 (CALL rel32) instructions.
/// Returns all call targets found.
static std::vector<uintptr_t> ScanForCallTargets(uintptr_t funcAddr, int scanBytes)
{
    std::vector<uintptr_t> targets;
    if (!funcAddr || !IsSafeToRead(reinterpret_cast<const void*>(funcAddr), scanBytes))
        return targets;

    const uint8_t* code = reinterpret_cast<const uint8_t*>(funcAddr);
    for (int i = 0; i < scanBytes - 5; i++) {
        if (code[i] == 0xE8) {
            // E8 XX XX XX XX = CALL rel32
            int32_t rel = *(int32_t*)(code + i + 1);
            uintptr_t target = funcAddr + i + 5 + rel;
            // Sanity: target should be in code range
            if (target > 0x10000 && target < 0x7FFFFFFF &&
                IsSafeToRead(reinterpret_cast<const void*>(target), 4)) {
                targets.push_back(target);
            }
            i += 4; // skip the rel32 bytes
        }
    }
    return targets;
}

/// Find ULevel from the puppet's XLevel field or GObjects.
static void* FindULevel(UObject* actor)
{
    // Method 1: Read XLevel from the actor's properties
    UStruct* cls = reinterpret_cast<UStruct*>(actor->GetClass());
    if (cls) {
        std::vector<PropertyInfo> props = WalkProperties(cls);
        PropertyInfo* xlev = FindProperty(cls, "XLevel", props);
        if (!xlev) xlev = FindProperty(cls, "Level", props);
        if (xlev) {
            s_XLevelOffset = xlev->Offset;
            uintptr_t levelPtr = *(uintptr_t*)(reinterpret_cast<uint8_t*>(actor) + xlev->Offset);
            if (levelPtr && IsSafeToRead(reinterpret_cast<const void*>(levelPtr), 0x10)) {
                LOG_INFO("[Puppet] ULevel from XLevel property @ offset {} = 0x{:08X}",
                         xlev->Offset, (uint32_t)levelPtr);
                return reinterpret_cast<void*>(levelPtr);
            }
        }
    }

    // Method 2: Scan GObjects for class "Level"
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return nullptr;
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    for (int i = 0; i < objCount && i < 120000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr || ptr < 0x10000u) continue;
        if (!IsSafeToRead(reinterpret_cast<const void*>(ptr), 0x34)) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == "Level") {
            LOG_INFO("[Puppet] ULevel from GObjects[{}] = 0x{:08X}", i, (uint32_t)ptr);
            return reinterpret_cast<void*>(ptr);
        }
    }
    return nullptr;
}

/// Discover MoveActor by scanning SetLocation's native code for CALL instructions.
/// SetLocation internally calls: Stack.Step(), P_FINISH, then MoveActor.
/// MoveActor is typically the LAST call in the function.
static bool DiscoverMoveActor(uintptr_t setLocNativeAddr)
{
    LOG_INFO("[Puppet] Scanning native at 0x{:08X} for MoveActor...", (uint32_t)setLocNativeAddr);

    // Dump first 16 bytes for reference
    const uint8_t* code = reinterpret_cast<const uint8_t*>(setLocNativeAddr);
    LOG_INFO("[Puppet]   Bytes: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}  {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
             code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
             code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);

    auto targets = ScanForCallTargets(setLocNativeAddr, 256);
    LOG_INFO("[Puppet]   Found {} CALL targets in SetLocation native:", targets.size());
    for (size_t i = 0; i < targets.size(); i++) {
        LOG_INFO("[Puppet]     CALL[{}] -> 0x{:08X}", i, (uint32_t)targets[i]);
    }

    // Heuristic: MoveActor is typically the last or second-to-last CALL.
    // SetLocation's native does: P_GET_VECTOR (1-2 calls), P_FINISH (1 call),
    // then GetLevel()->MoveActor() (1-2 calls: GetLevel + MoveActor, or just MoveActor if inlined).
    // The LAST call is usually MoveActor.
    if (targets.size() >= 2) {
        // Try the last CALL as MoveActor
        s_MoveActorFn = reinterpret_cast<MoveActorFn>(targets.back());
        LOG_INFO("[Puppet]   Selected CALL[{}] = 0x{:08X} as MoveActor candidate",
                 targets.size() - 1, (uint32_t)targets.back());
        return true;
    } else if (targets.size() == 1) {
        s_MoveActorFn = reinterpret_cast<MoveActorFn>(targets[0]);
        LOG_INFO("[Puppet]   Only one CALL found, using it as MoveActor");
        return true;
    }

    LOG_WARN("[Puppet]   No CALL instructions found in SetLocation native!");
    return false;
}

/// Check if bytes at a pointer look like a valid x86 function prologue.
static bool IsLikelyCode(const uint8_t* p)
{
    uint8_t b = p[0];
    // Single-byte push instructions (common prologues)
    if (b == 0x55 || b == 0x56 || b == 0x53 || b == 0x51 || b == 0x57) return true;
    // push imm8
    if (b == 0x6A) return true;
    // sub esp, imm8 (83 EC XX)
    if (b == 0x83 && p[1] == 0xEC) return true;
    // sub esp, imm32 (81 EC XX XX XX XX)
    if (b == 0x81 && p[1] == 0xEC) return true;
    // mov reg, reg (8B XX) — common in prologues
    if (b == 0x8B) return true;
    // mov eax, imm32 (B8 XX XX XX XX)
    if (b == 0xB8) return true;
    return false;
}

/// One-time diagnosis: scan UFunction bytes to find the REAL Func pointer,
/// discover MoveActor from it, and find ULevel.
/// Writes full dump to debug_dumps/puppet_diagnosis.txt for easy sharing.
/// Separated from UpdateGhostPuppet because __try can't coexist with C++ unwinding.
static void DiagnoseAndDiscover()
{
    // Open dump file
    CreateDirectoryA("debug_dumps", nullptr);
    FILE* df = fopen("debug_dumps/puppet_diagnosis.txt", "w");

    // Helper: write to both log and file
    auto emit = [&](const char* line) {
        LOG_INFO("{}", line);
        if (df) { fprintf(df, "%s\n", line); fflush(df); }
    };

    char buf[512];

    // ── Section 1: UFunction hex dump (full object 0x00-0xBF = 192 bytes) ──
    const uint8_t* slRaw = reinterpret_cast<const uint8_t*>(s_SetLocFunc);
    snprintf(buf, sizeof(buf), "=== SetLocation UFunction @ 0x%08X ===",
             (uint32_t)(uintptr_t)s_SetLocFunc);
    emit(buf);

    for (int row = 0x00; row < 0xC0; row += 0x10) {
        snprintf(buf, sizeof(buf),
                 "  +%02X: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                 row,
                 slRaw[row+0], slRaw[row+1], slRaw[row+2], slRaw[row+3],
                 slRaw[row+4], slRaw[row+5], slRaw[row+6], slRaw[row+7],
                 slRaw[row+8], slRaw[row+9], slRaw[row+10], slRaw[row+11],
                 slRaw[row+12], slRaw[row+13], slRaw[row+14], slRaw[row+15]);
        emit(buf);
    }

    // Known fields
    uint16_t iNative = *(uint16_t*)(slRaw + 0x68);
    snprintf(buf, sizeof(buf), "iNative(+0x68)=%u", iNative);
    emit(buf);

    // UObject name for verification
    std::string funcName = s_SetLocFunc->GetName();
    snprintf(buf, sizeof(buf), "UFunction Name: %s", funcName.c_str());
    emit(buf);

    // ── Section 2: x86 prologue scan (+0x00 to +0xBC) ──
    emit("");
    emit("=== Scanning ALL UFunction offsets for x86 code pointers ===");

    uintptr_t bestFuncAddr = 0;
    int bestFuncOffset = -1;

    for (int off = 0x00; off <= 0xBC; off += 4) {
        uintptr_t val = *(uintptr_t*)(slRaw + off);
        if (val < 0x00400000 || val > 0x7FFFFFFF) continue;
        if (!IsSafeToRead(reinterpret_cast<const void*>(val), 16)) continue;

        const uint8_t* codeAt = reinterpret_cast<const uint8_t*>(val);
        bool looksLikeCode = IsLikelyCode(codeAt);

        snprintf(buf, sizeof(buf),
                 "  +0x%02X: 0x%08X -> [%02X %02X %02X %02X %02X %02X %02X %02X] %s",
                 off, (uint32_t)val,
                 codeAt[0], codeAt[1], codeAt[2], codeAt[3],
                 codeAt[4], codeAt[5], codeAt[6], codeAt[7],
                 looksLikeCode ? "<<< LOOKS LIKE CODE" : "");
        emit(buf);

        if (looksLikeCode && bestFuncAddr == 0) {
            bestFuncAddr = val;
            bestFuncOffset = off;
        }
    }

    if (bestFuncAddr) {
        s_SetLocNativeAddr = bestFuncAddr;
        snprintf(buf, sizeof(buf), "FOUND Func at +0x%02X = 0x%08X",
                 bestFuncOffset, (uint32_t)bestFuncAddr);
        emit(buf);

        // Dump 64 bytes of actual native code for analysis
        emit("");
        emit("=== Native code bytes (first 64) ===");
        const uint8_t* nc = reinterpret_cast<const uint8_t*>(bestFuncAddr);
        for (int row = 0; row < 64; row += 16) {
            snprintf(buf, sizeof(buf),
                     "  +%02X: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                     row,
                     nc[row+0], nc[row+1], nc[row+2], nc[row+3],
                     nc[row+4], nc[row+5], nc[row+6], nc[row+7],
                     nc[row+8], nc[row+9], nc[row+10], nc[row+11],
                     nc[row+12], nc[row+13], nc[row+14], nc[row+15]);
            emit(buf);
        }

        s_MoveActorDiscovered = DiscoverMoveActor(bestFuncAddr);
    } else {
        emit("WARNING: No x86 code pointer found in entire UFunction!");
        emit("Dumping ALL pointer-like values for manual analysis:");
        for (int off = 0x00; off <= 0xBC; off += 4) {
            uintptr_t val = *(uintptr_t*)(slRaw + off);
            if (val > 0x10000 && val < 0x7FFFFFFF) {
                bool readable = IsSafeToRead(reinterpret_cast<const void*>(val), 4);
                snprintf(buf, sizeof(buf), "  +0x%02X: 0x%08X readable=%s",
                         off, (uint32_t)val, readable ? "YES" : "NO");
                emit(buf);
            }
        }
    }

    // ── Section 3: SetRotation probe ──
    emit("");
    emit("=== SetRotation UFunction ===");
    if (s_SetRotFunc) {
        const uint8_t* srRaw = reinterpret_cast<const uint8_t*>(s_SetRotFunc);
        snprintf(buf, sizeof(buf), "SetRotation UFunction @ 0x%08X",
                 (uint32_t)(uintptr_t)s_SetRotFunc);
        emit(buf);

        for (int off = 0x00; off <= 0xBC; off += 4) {
            uintptr_t val = *(uintptr_t*)(srRaw + off);
            if (val < 0x00400000 || val > 0x7FFFFFFF) continue;
            if (!IsSafeToRead(reinterpret_cast<const void*>(val), 16)) continue;
            const uint8_t* codeAt = reinterpret_cast<const uint8_t*>(val);
            if (IsLikelyCode(codeAt)) {
                s_SetRotNativeAddr = val;
                snprintf(buf, sizeof(buf),
                         "SetRotation Func at +0x%02X = 0x%08X [%02X %02X %02X %02X]",
                         off, (uint32_t)val, codeAt[0], codeAt[1], codeAt[2], codeAt[3]);
                emit(buf);
                break;
            }
        }
        if (!s_SetRotNativeAddr)
            emit("WARNING: SetRotation: no x86 code pointer found");
    } else {
        emit("SetRotation UFunction NOT found");
    }

    // ── Section 4: Puppet actor dump (first 512 bytes) ──
    emit("");
    snprintf(buf, sizeof(buf), "=== Puppet Actor @ 0x%08X (%s) ===",
             (uint32_t)(uintptr_t)s_Puppet, s_Puppet->GetName().c_str());
    emit(buf);
    snprintf(buf, sizeof(buf), "Puppet class: %s  LocOffset=%d RotOffset=%d",
             s_Puppet->GetObjClassName().c_str(), s_PuppetLocOffset, s_PuppetRotOffset);
    emit(buf);

    const uint8_t* actRaw = reinterpret_cast<const uint8_t*>(s_Puppet);
    for (int row = 0x00; row < 0x200; row += 0x10) {
        if (!IsSafeToRead(actRaw + row, 16)) break;
        snprintf(buf, sizeof(buf),
                 "  +%03X: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                 row,
                 actRaw[row+0], actRaw[row+1], actRaw[row+2], actRaw[row+3],
                 actRaw[row+4], actRaw[row+5], actRaw[row+6], actRaw[row+7],
                 actRaw[row+8], actRaw[row+9], actRaw[row+10], actRaw[row+11],
                 actRaw[row+12], actRaw[row+13], actRaw[row+14], actRaw[row+15]);
        emit(buf);
    }

    // ── Section 5: ULevel search ──
    emit("");
    emit("=== ULevel Search ===");

    s_ULevel = FindULevel(s_Puppet);
    if (s_ULevel) {
        snprintf(buf, sizeof(buf), "ULevel = 0x%08X (from property)", (uint32_t)(uintptr_t)s_ULevel);
        emit(buf);
    } else {
        emit("ULevel NOT found via property — scanning actor memory for Level object");
        for (int off = 0x80; off <= 0x1FC; off += 4) {
            if (!IsSafeToRead(actRaw + off, 4)) break;
            uintptr_t val = *(uintptr_t*)(actRaw + off);
            if (val < 0x10000 || val > 0x7FFFFFFF) continue;
            if (!IsSafeToRead(reinterpret_cast<const void*>(val), 0x34)) continue;
            UObject* maybeLevel = reinterpret_cast<UObject*>(val);
            std::string cn = maybeLevel->GetObjClassName();
            if (cn == "Level") {
                s_ULevel = reinterpret_cast<void*>(val);
                snprintf(buf, sizeof(buf), "ULevel from actor+0x%03X = 0x%08X", off, (uint32_t)val);
                emit(buf);
                break;
            }
        }
        if (!s_ULevel)
            emit("ULevel NOT FOUND anywhere!");
    }

    // ── Summary ──
    emit("");
    emit("=== SUMMARY ===");
    if (s_MoveActorDiscovered && s_ULevel) {
        snprintf(buf, sizeof(buf), "Direct MoveActor READY: ULevel=0x%08X MoveActor=0x%08X",
                 (uint32_t)(uintptr_t)s_ULevel, (uint32_t)(uintptr_t)s_MoveActorFn);
        emit(buf);
    } else if (s_MoveActorDiscovered && !s_ULevel) {
        emit("MoveActor found but NO ULevel — cannot call directly");
        s_MoveActorDiscovered = false;
    } else {
        emit("No direct MoveActor — will try PE then raw write");
    }

    if (df) {
        fclose(df);
        LOG_INFO("[Puppet] Diagnosis written to debug_dumps/puppet_diagnosis.txt");
    }
}

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

/// Set a property on the puppet by name (handles BoolProperty bitfields correctly).
static bool SetPuppetProperty(const char* propName, const void* value, int size)
{
    if (!s_Puppet) return false;
    // Delegate to SetActorProperty which handles BoolProperty bitmasks
    return SetActorProperty(s_Puppet, propName, value, size);
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
    s_SetLocFunc = FindFunctionOnClass(playerClass, "SetLocation");
    s_SetRotFunc = FindFunctionOnClass(playerClass, "SetRotation");

    if (!s_SpawnFunc) {
        LOG_WARN("[Puppet] Could not find Actor.Spawn function");
        return false;
    }

    if (!s_SetLocFunc) LOG_WARN("[Puppet] SetLocation not found — puppet won't visually move!");
    if (!s_SetRotFunc) LOG_WARN("[Puppet] SetRotation not found — puppet won't visually rotate!");

    s_Initialized = true;
    LOG_INFO("[Puppet] Ghost puppet system initialized (Spawn={}, Destroy={}, SetLoc={}, SetRot={})",
             (void*)s_SpawnFunc, (void*)s_DestroyFunc, (void*)s_SetLocFunc, (void*)s_SetRotFunc);
    return true;
}

void ShutdownGhostPuppet()
{
    DestroyGhostPuppet();
    s_SpawnFunc = nullptr;
    s_DestroyFunc = nullptr;
    s_SetLocFunc = nullptr;
    s_SetRotFunc = nullptr;
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

    // Prefer classes in this order — many fallbacks for different game states
    const char* borrowClasses[] = {
        "StaticMeshActor", "InterpActor", "KActor",
        "Decoration", "Emitter", "Note",
        "Trigger", "BlockingVolume", "PhysicsVolume"
    };

    UObject* bestCandidate = nullptr;
    float bestDist = 0.0f;
    int candidatesScanned = 0;
    int totalChecked = 0;
    int nullPtrs = 0;
    int badPtr = 0;
    int noClass = 0;
    int nameErrors = 0;
    static bool s_DiagLogged = false;

    // Single-pass scan: check all objects once, matching any borrow class
    for (int i = 0; i < objCount && i < 120000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) { nullPtrs++; continue; }

        // Quick validity check: reject obviously bad pointers (IsSafeToRead handles LAA range)
        if (ptr < 0x10000u) { badPtr++; continue; }

        UObject* obj = reinterpret_cast<UObject*>(ptr);
        totalChecked++;

        // Read class pointer directly at +0x30, with minimal safety check
        if (!IsSafeToRead(reinterpret_cast<const void*>(ptr), 0x34)) { badPtr++; continue; }
        uintptr_t clsPtr = *reinterpret_cast<uintptr_t*>(ptr + UObject::OFFSET_CLASS);
        if (!clsPtr || clsPtr < 0x10000u) { noClass++; continue; }
        if (!IsSafeToRead(reinterpret_cast<const void*>(clsPtr), 0x30)) { noClass++; continue; }

        // Read the class FName directly (at +0x28 on the class object)
        FName classFName = *reinterpret_cast<FName*>(clsPtr + UObject::OFFSET_NAME);
        std::string cn = classFName.ToString();
        if (cn.empty() || cn[0] == '<') { nameErrors++; continue; }

        // Log first few class names on first scan for diagnostics
        if (!s_DiagLogged && i <= 4 && !cn.empty()) {
            LOG_INFO("[Puppet] GObj[{}] class='{}' ptr=0x{:08X}", i, cn, (uint32_t)ptr);
        }

        // Check against all borrow classes
        bool isMatch = false;
        for (const char* bClass : borrowClasses) {
            if (cn == bClass) { isMatch = true; break; }
        }
        if (!isMatch) continue;
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

    LOG_INFO("[Puppet] Scanned {} candidates, locOff={} (total={}, null={}, badPtr={}, noClass={}, nameErr={}, objCount={})",
             candidatesScanned, locOff, totalChecked, nullPtrs, badPtr, noClass, nameErrors, objCount);
    s_DiagLogged = true;

    if (!bestCandidate) {
        LOG_WARN("[Puppet] No world actor found to borrow as puppet");
        return false;
    }

    s_Puppet = bestCandidate;
    LOG_INFO("[Puppet] Borrowed world actor: {} ({}) dist={:.0f}",
             s_Puppet->GetName(), s_Puppet->GetObjClassName(), sqrtf(bestDist));

    // Cache property offsets on the borrowed actor
    CachePuppetOffsets();

    // ── Make movable: StaticMeshActors are bStatic=true by default,
    //    which means UE2 never updates their render position when Location
    //    changes. We must clear bStatic and give it physics so the engine
    //    re-registers the actor in the render octree each frame.
    int32_t bFalse = 0;
    int32_t bTrue = 1;

    // CRITICAL: allow the engine to move this actor's rendered mesh
    SetPuppetProperty("bStatic", &bFalse, 4);
    SetPuppetProperty("bWorldGeometry", &bFalse, 4);
    // PHYS_Interpolating = 9 (confirmed via enum dump). Engine updates render pos each tick.
    uint8_t physInterp = 9;
    SetPuppetProperty("Physics", &physInterp, 1);
    // Always render regardless of distance
    SetPuppetProperty("bAlwaysRelevant", &bTrue, 4);

    // Disable collision so it doesn't block anything
    SetPuppetProperty("bCollideActors", &bFalse, 4);
    SetPuppetProperty("bBlockActors", &bFalse, 4);
    SetPuppetProperty("bBlockPlayers", &bFalse, 4);
    SetPuppetProperty("bBlockHavok", &bFalse, 4);
    SetPuppetProperty("bHidden", &bFalse, 4);

    // Make it bright and fully lit so it's always visible as "the partner"
    SetPuppetProperty("bUnlit", &bTrue, 4);
    uint8_t ambientGlow = 254;
    SetPuppetProperty("AmbientGlow", &ambientGlow, 1);

    // Keep at human-ish scale so it's easy to see
    float drawScale = 1.0f;
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

    // ─── Move puppet ───
    // ProcessEvent + SetLocation crashes on borrowed StaticMeshActors because
    // they're in the static octree and MoveActor can't manipulate them there.
    // Solution: call MoveActor DIRECTLY, bypassing ProcessEvent entirely.
    static bool s_Diagnosed = false;
    static bool s_SetLocSafe = true;
    static bool s_SetRotSafe = true;
    ProcessEventFn origPE = GetOriginalProcessEvent();

    // One-time: diagnose UFunction layout, discover MoveActor, find ULevel
    if (!s_Diagnosed && s_SetLocFunc) {
        s_Diagnosed = true;
        DiagnoseAndDiscover();
    }

    // Read current position from puppet (needed for MoveActor delta computation)
    float curX = 0, curY = 0, curZ = 0;
    if (s_PuppetLocOffset > 0) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(s_Puppet);
        memcpy(&curX, raw + s_PuppetLocOffset, 4);
        memcpy(&curY, raw + s_PuppetLocOffset + 4, 4);
        memcpy(&curZ, raw + s_PuppetLocOffset + 8, 4);
    }

    bool locWritten = false;

    // Attempt 1: Direct MoveActor call (bypasses ProcessEvent entirely)
    if (!locWritten && s_MoveActorDiscovered && s_MoveActorFn && s_ULevel) {
        float dX = s_InterpX - curX;
        float dY = s_InterpY - curY;
        float dZ = s_InterpZ - curZ;

        // Skip if delta is negligible
        if (dX * dX + dY * dY + dZ * dZ > 0.01f) {
            __try {
                s_MoveActorFn(s_ULevel, s_Puppet,
                              dX, dY, dZ,
                              0, 0, 0,      // no rotation delta
                              0, nullptr);   // no flags, no hit result
                locWritten = true;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                DWORD exCode = GetExceptionCode();
                LOG_WARN("[Puppet] MoveActor CRASHED (exception=0x{:08X}) — disabling", exCode);
                s_MoveActorDiscovered = false;
            }
        } else {
            locWritten = true; // no movement needed
        }
    }

    // Attempt 2: ProcessEvent + SetLocation (may crash on static actors)
    if (!locWritten && origPE && s_SetLocFunc && s_SetLocSafe) {
        struct {
            float X, Y, Z;
            uint32_t bNoCheck;
            uint32_t ReturnValue;
        } locParms{};
        locParms.X = s_InterpX;
        locParms.Y = s_InterpY;
        locParms.Z = s_InterpZ;
        locParms.bNoCheck = 1;
        __try {
            origPE(s_Puppet, s_SetLocFunc, &locParms, nullptr);
            locWritten = true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            DWORD exCode = GetExceptionCode();
            LOG_WARN("[Puppet] SetLocation PE crashed (exception=0x{:08X}) — disabling PE path", exCode);
            s_SetLocSafe = false;
        }
    }

    // Fallback: raw memory write (position data correct, but mesh won't move visually)
    if (!locWritten && s_PuppetLocOffset > 0) {
        uint8_t* raw = reinterpret_cast<uint8_t*>(s_Puppet);
        memcpy(raw + s_PuppetLocOffset, &s_InterpX, 4);
        memcpy(raw + s_PuppetLocOffset + 4, &s_InterpY, 4);
        memcpy(raw + s_PuppetLocOffset + 8, &s_InterpZ, 4);
    }

    // Rotation: raw write (SetRotation via PE also crashes, and MoveActor handles rotation delta)
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

std::string GetPuppetDiagnostics()
{
    char buf[1024];
    const auto& globals = GetEngineGlobals();
    int32_t objCount = 0;
    uintptr_t objData = 0;
    if (globals.IsValid()) {
        objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
        objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    }

    std::snprintf(buf, sizeof(buf),
        "=== Puppet Diagnostics ===\n"
        "Initialized: %s\n"
        "Puppet ptr: 0x%08X\n"
        "SpawnFunc: 0x%08X\n"
        "DestroyFunc: 0x%08X\n"
        "LocOffset: %d  RotOffset: %d\n"
        "HasTarget: %s  (%.0f, %.0f, %.0f)\n"
        "GObjects: data=0x%08X count=%d\n"
        "IsAIPuppet: %s\n"
        "RetryTimer: %.1f / %.1f",
        s_Initialized ? "YES" : "NO",
        (uint32_t)(uintptr_t)s_Puppet,
        (uint32_t)(uintptr_t)s_SpawnFunc,
        (uint32_t)(uintptr_t)s_DestroyFunc,
        s_PuppetLocOffset, s_PuppetRotOffset,
        s_HasTarget ? "YES" : "NO", s_TargetX, s_TargetY, s_TargetZ,
        (uint32_t)objData, objCount,
        s_IsAIPuppet ? "YES" : "NO",
        s_SpawnRetryTimer, SPAWN_RETRY_INTERVAL);

    std::string result = buf;

    // If puppet is alive, show its current position
    if (s_Puppet && s_PuppetLocOffset > 0) {
        float px, py, pz;
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(s_Puppet);
        memcpy(&px, raw + s_PuppetLocOffset, 4);
        memcpy(&py, raw + s_PuppetLocOffset + 4, 4);
        memcpy(&pz, raw + s_PuppetLocOffset + 8, 4);
        std::snprintf(buf, sizeof(buf), "\nPuppet pos: (%.0f, %.0f, %.0f)", px, py, pz);
        result += buf;
    }

    // Quick class scan check — test first 5 non-null objects
    if (globals.IsValid() && objData && objCount > 0) {
        result += "\nFirst 5 GObj classes: ";
        int shown = 0;
        for (int i = 0; i < objCount && i < 1000 && shown < 5; i++) {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
            if (!ptr) continue;
            UObject* obj = reinterpret_cast<UObject*>(ptr);
            std::string cn = obj->GetObjClassName();
            if (cn.empty() || cn == "<bad_ptr>" || cn == "<no_class>") {
                result += "[BAD] ";
            } else {
                result += cn + " ";
            }
            shown++;
        }
    }

    // Direct test: try FindObjectByClassName
    UObject* testPlayer = FindObjectByClassName("ShockPlayer");
    std::snprintf(buf, sizeof(buf), "\nFindObjectByClassName('ShockPlayer'): 0x%08X",
                 (uint32_t)(uintptr_t)testPlayer);
    result += buf;

    // Scan for any class name containing "Player" or "Shock"
    if (globals.IsValid() && objData && objCount > 0) {
        result += "\nPlayer-related objects found:";
        int playerHits = 0;
        std::string seenClasses;
        for (int i = 0; i < objCount && i < 150000; i++) {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
            if (!ptr) continue;
            UObject* obj = reinterpret_cast<UObject*>(ptr);
            if (!IsSafeToRead(obj, 64)) continue;
            std::string cn = obj->GetObjClassName();
            // Check for anything player-related
            if (cn.find("Player") != std::string::npos ||
                cn.find("player") != std::string::npos ||
                cn.find("ShockPlayer") != std::string::npos) {
                // Only log unique class names, up to 10
                if (seenClasses.find(cn) == std::string::npos && playerHits < 10) {
                    std::string objName = obj->GetName();
                    std::snprintf(buf, sizeof(buf), "\n  [%d] cls='%s' name='%s' ptr=0x%08X",
                                 i, cn.c_str(), objName.c_str(), (uint32_t)ptr);
                    result += buf;
                    seenClasses += cn + ";";
                    playerHits++;
                }
            }
        }
        if (playerHits == 0) result += " NONE!";
    }

    // Scan for puppet borrow candidates using direct pointer reads
    if (globals.IsValid() && objData && objCount > 0) {
        int meshActors = 0, interpActors = 0, decorations = 0, emitters = 0;
        int totalBad = 0, totalGood = 0;
        for (int i = 0; i < objCount && i < 150000; i++) {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
            if (!ptr) continue;
            if (ptr < 0x10000u) { totalBad++; continue; }
            if (!IsSafeToRead(reinterpret_cast<const void*>(ptr), 0x34)) { totalBad++; continue; }
            uintptr_t clsPtr = *reinterpret_cast<uintptr_t*>(ptr + UObject::OFFSET_CLASS);
            if (!clsPtr || clsPtr < 0x10000u || !IsSafeToRead(reinterpret_cast<const void*>(clsPtr), 0x30)) { totalBad++; continue; }
            FName classFName = *reinterpret_cast<FName*>(clsPtr + UObject::OFFSET_NAME);
            std::string cn = classFName.ToString();
            if (cn.empty() || cn[0] == '<') { totalBad++; continue; }
            totalGood++;
            if (cn == "StaticMeshActor") meshActors++;
            else if (cn == "InterpActor") interpActors++;
            else if (cn == "Decoration") decorations++;
            else if (cn == "Emitter") emitters++;
        }
        std::snprintf(buf, sizeof(buf),
            "\nBorrow candidates: SMA=%d Interp=%d Decor=%d Emit=%d"
            "\nObjects: %d good, %d bad (of %d total)",
            meshActors, interpActors, decorations, emitters,
            totalGood, totalBad, objCount);
        result += buf;
    }

    return result;
}

} // namespace bs1sdk
