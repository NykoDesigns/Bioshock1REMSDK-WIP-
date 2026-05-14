#include "world.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../core/pattern.h"
#include "../core/hooks.h"
#include "../hooks/process_event.h"
#include "../debug/crash_handler.h"

#include <Windows.h>
#include <Psapi.h>
#include <fstream>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>

namespace bs1sdk {

// ─── State ──────────────────────────────────────────────────────────────

static bool s_WorldInit = false;
static uintptr_t s_GWorld = 0;
static uintptr_t s_GEngine = 0;
static int32_t s_ActorsOffset = -1; // offset of Actors TArray within ULevel

// GNatives
static uintptr_t s_GNativesAddr = 0;
static NativeFunc* s_GNatives = nullptr;
static NativeFunc s_OriginalNatives[4096] = {};
static bool s_NativesInit = false;

// Tick hook
static bool s_TickHooked = false;
static std::vector<std::pair<int, TickCallback>> s_TickCallbacks;
static std::mutex s_TickMutex;
static int s_NextTickId = 1;
static float s_TickRate = 0.0f;
static uint64_t s_LastTickTime = 0;
static uint64_t s_TickCount = 0;

// Original tick function pointer
typedef void (__thiscall* EngineTickFn)(void* thisEngine, float deltaTime);
static EngineTickFn s_OriginalTick = nullptr;

// ─── World System ────────────────────────────────────────────────────────

// Helper: SEH-safe scan for actors array offset within a ULevel object.
// Isolated to avoid C2712 (__try + object unwinding conflict).
#pragma warning(push)
#pragma warning(disable: 4733)
static int DiscoverActorsOffset(const uint8_t* levelBytes, uintptr_t modBase, size_t modSize)
{
    for (int off = 0x40; off < 0x200; off += 4) {
        __try {
            uintptr_t dataPtr = *reinterpret_cast<const uintptr_t*>(levelBytes + off);
            int32_t count = *reinterpret_cast<const int32_t*>(levelBytes + off + 4);
            int32_t max = *reinterpret_cast<const int32_t*>(levelBytes + off + 8);
            
            if (count < 10 || count > 10000) continue;
            if (max < count || max > 50000) continue;
            if (dataPtr < 0x10000 || !Memory::IsValidPtr(dataPtr)) continue;
            
            int valid = 0;
            int limit = (count < 10) ? count : 10;
            for (int j = 0; j < limit; j++) {
                uintptr_t actorPtr = *reinterpret_cast<uintptr_t*>(dataPtr + j * 4);
                if (actorPtr == 0) { valid++; continue; }
                if (actorPtr < 0x10000) break;
                if (!Memory::IsValidPtr(actorPtr)) break;
                uintptr_t vtable = *reinterpret_cast<uintptr_t*>(actorPtr);
                if (vtable >= modBase && vtable < modBase + modSize)
                    valid++;
            }
            
            if (valid >= 8) return off;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    return -1;
}
#pragma warning(pop)

bool InitWorldSystem()
{
    if (s_WorldInit) return true;

    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) {
        LOG_WARN("WorldSystem: GObjects/GNames not ready");
        return false;
    }

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    UObject* engine = nullptr;
    UObject* level = nullptr;

    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        std::string cn = obj->GetObjClassName();
        
        if (!engine && cn == "GameEngine") {
            engine = obj;
            s_GEngine = (uintptr_t)obj;
        }
        if (!level && cn == "Level") {
            level = obj;
            s_GWorld = (uintptr_t)obj;
        }
        
        if (engine && level) break;
    }

    if (engine) {
        globals.GEngine = (uintptr_t)engine;
        LOG_INFO("WorldSystem: GEngine found at 0x{:08X} ({})", 
                 (uint32_t)(uintptr_t)engine, engine->GetFullPath());
    }

    if (level) {
        globals.GWorld = (uintptr_t)level;
        LOG_INFO("WorldSystem: Level found at 0x{:08X} ({})",
                 (uint32_t)(uintptr_t)level, level->GetFullPath());
        
        const uint8_t* levelBytes = reinterpret_cast<const uint8_t*>(level);
        uintptr_t modBase = Memory::GetModuleBase(nullptr);
        size_t modSize = Memory::GetModuleSize(nullptr);
        
        s_ActorsOffset = DiscoverActorsOffset(levelBytes, modBase, modSize);
        if (s_ActorsOffset >= 0) {
            int32_t count = *reinterpret_cast<const int32_t*>(levelBytes + s_ActorsOffset + 4);
            int32_t max = *reinterpret_cast<const int32_t*>(levelBytes + s_ActorsOffset + 8);
            LOG_INFO("WorldSystem: Actors TArray at ULevel+0x{:X} (count={}, max={})",
                     s_ActorsOffset, count, max);
        }
    }

    s_WorldInit = (level != nullptr && s_ActorsOffset >= 0);
    if (s_WorldInit) {
        LOG_INFO("WorldSystem: Initialized successfully");
    } else {
        LOG_WARN("WorldSystem: Partial init (engine={}, level={}, actorsOff={})",
                 engine != nullptr, level != nullptr, s_ActorsOffset);
    }
    return s_WorldInit;
}

bool IsWorldSystemReady()
{
    return s_WorldInit;
}

LevelInfo GetCurrentLevel()
{
    LevelInfo info;
    if (!s_WorldInit || !s_GWorld) return info;

    UObject* level = reinterpret_cast<UObject*>(s_GWorld);
    info.LevelPtr = s_GWorld;
    info.LevelName = level->GetFullPath();

    if (s_ActorsOffset >= 0) {
        const uint8_t* levelBytes = reinterpret_cast<const uint8_t*>(level);
        info.ActorsArrayPtr = *reinterpret_cast<const uintptr_t*>(levelBytes + s_ActorsOffset);
        info.ActorCount = *reinterpret_cast<const int32_t*>(levelBytes + s_ActorsOffset + 4);
    }

    return info;
}

std::vector<UObject*> GetAllActors()
{
    std::vector<UObject*> result;
    if (!s_WorldInit || !s_GWorld || s_ActorsOffset < 0) return result;

    const uint8_t* levelBytes = reinterpret_cast<const uint8_t*>(s_GWorld);
    uintptr_t dataPtr = *reinterpret_cast<const uintptr_t*>(levelBytes + s_ActorsOffset);
    int32_t count = *reinterpret_cast<const int32_t*>(levelBytes + s_ActorsOffset + 4);

    if (!dataPtr || count <= 0) return result;

    result.reserve(count);
    for (int i = 0; i < count; i++) {
        uintptr_t actorPtr = *reinterpret_cast<uintptr_t*>(dataPtr + i * 4);
        if (actorPtr && actorPtr > 0x10000)
            result.push_back(reinterpret_cast<UObject*>(actorPtr));
    }
    return result;
}

std::vector<UObject*> GetActorsOfClass(const std::string& className)
{
    std::vector<UObject*> result;
    auto actors = GetAllActors();
    for (auto* actor : actors) {
        if (actor->GetObjClassName() == className)
            result.push_back(actor);
    }
    return result;
}

UObject* FindActor(const std::string& className)
{
    auto actors = GetAllActors();
    for (auto* actor : actors) {
        if (actor->GetObjClassName() == className)
            return actor;
    }
    return nullptr;
}

std::vector<UObject*> GetActorsInRadius(FVec3 center, float radius, const std::string& classFilter)
{
    std::vector<UObject*> result;
    float radiusSq = radius * radius;
    
    auto actors = GetAllActors();
    for (auto* actor : actors) {
        if (!classFilter.empty() && actor->GetObjClassName() != classFilter)
            continue;
        
        FVec3 pos;
        if (!GetActorPosition(actor, pos)) continue;
        
        float dx = pos.X - center.X;
        float dy = pos.Y - center.Y;
        float dz = pos.Z - center.Z;
        float distSq = dx*dx + dy*dy + dz*dz;
        
        if (distSq <= radiusSq)
            result.push_back(actor);
    }
    return result;
}

bool GetPlayerPosition(FVec3& outPos)
{
    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) return false;
    return GetActorPosition(player, outPos);
}

bool GetActorPosition(UObject* actor, FVec3& outPos)
{
    if (!actor) return false;
    
    // Location is a StructProperty (FVector, 12 bytes) on Actor
    // We need to find the offset of 'Location' on this actor's class
    UStruct* cls = reinterpret_cast<UStruct*>(actor->GetClass());
    if (!cls) return false;
    
    // Walk properties to find Location
    UField* child = cls;
    int depth = 0;
    while (child && depth < 64) {
        UStruct* current = reinterpret_cast<UStruct*>(child);
        UField* prop = current->GetChildren();
        int limit = 2000;
        while (prop && limit-- > 0) {
            if (prop->GetObjClassName().find("Property") != std::string::npos) {
                if (prop->GetName() == "Location") {
                    UProperty* locProp = reinterpret_cast<UProperty*>(prop);
                    int32_t offset = locProp->GetPropertyOffset();
                    const uint8_t* base = reinterpret_cast<const uint8_t*>(actor);
                    outPos.X = *reinterpret_cast<const float*>(base + offset);
                    outPos.Y = *reinterpret_cast<const float*>(base + offset + 4);
                    outPos.Z = *reinterpret_cast<const float*>(base + offset + 8);
                    return true;
                }
            }
            prop = prop->GetNext();
        }
        UField* super = current->GetSuperField();
        child = super;
        depth++;
    }
    return false;
}

bool SetActorPosition(UObject* actor, const FVec3& pos)
{
    if (!actor) return false;
    
    UStruct* cls = reinterpret_cast<UStruct*>(actor->GetClass());
    if (!cls) return false;
    
    UField* child = cls;
    int depth = 0;
    while (child && depth < 64) {
        UStruct* current = reinterpret_cast<UStruct*>(child);
        UField* prop = current->GetChildren();
        int limit = 2000;
        while (prop && limit-- > 0) {
            if (prop->GetObjClassName().find("Property") != std::string::npos) {
                if (prop->GetName() == "Location") {
                    UProperty* locProp = reinterpret_cast<UProperty*>(prop);
                    int32_t offset = locProp->GetPropertyOffset();
                    uint8_t* base = reinterpret_cast<uint8_t*>(actor);
                    *reinterpret_cast<float*>(base + offset) = pos.X;
                    *reinterpret_cast<float*>(base + offset + 4) = pos.Y;
                    *reinterpret_cast<float*>(base + offset + 8) = pos.Z;
                    return true;
                }
            }
            prop = prop->GetNext();
        }
        UField* super = current->GetSuperField();
        child = super;
        depth++;
    }
    return false;
}

float GetActorDistance(UObject* a, UObject* b)
{
    FVec3 posA, posB;
    if (!GetActorPosition(a, posA) || !GetActorPosition(b, posB))
        return -1.0f;
    
    float dx = posA.X - posB.X;
    float dy = posA.Y - posB.Y;
    float dz = posA.Z - posB.Z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

void ForEachActor(std::function<void(UObject* actor, const std::string& className)> callback)
{
    auto actors = GetAllActors();
    for (auto* actor : actors) {
        callback(actor, actor->GetObjClassName());
    }
}

UObject* GetEngine()
{
    if (s_GEngine) return reinterpret_cast<UObject*>(s_GEngine);
    return FindObjectByClassName("GameEngine");
}

UObject* GetGameInfo()
{
    return FindObjectByClassName("ShockGameInfo");
}

UObject* GetPlayerController()
{
    return FindObjectByClassName("ShockPlayerController");
}

// ─── GNatives Table ──────────────────────────────────────────────────────

// SEH-safe scan for a native function pointer table
static uintptr_t ScanForNativesTable(uintptr_t modBase, size_t modSize)
{
    for (uintptr_t scan = modBase; scan < modBase + modSize - 16384; scan += 4) {
        __try {
            uintptr_t* table = reinterpret_cast<uintptr_t*>(scan);
            
            if (table[0] != 0 && (table[0] < modBase || table[0] >= modBase + modSize))
                continue;
            
            int validCount = 0;
            int nullCount = 0;
            for (int i = 0; i < 256; i++) {
                if (table[i] == 0) { nullCount++; continue; }
                if (table[i] >= modBase && table[i] < modBase + modSize)
                    validCount++;
                else
                    break;
            }
            
            if (validCount < 80 || nullCount < 20) continue;
            if (validCount + nullCount < 200) continue;
            
            return scan;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    return 0;
}

bool InitNativeTable()
{
    if (s_NativesInit) return true;

    uintptr_t modBase = Memory::GetModuleBase(nullptr);
    size_t modSize = Memory::GetModuleSize(nullptr);

    uintptr_t found = ScanForNativesTable(modBase, modSize);
    if (!found) {
        LOG_WARN("GNatives: Table not found via heuristic scan");
        return false;
    }

    s_GNativesAddr = found;
    s_GNatives = reinterpret_cast<NativeFunc*>(found);
    
    // Backup all originals
    for (int i = 0; i < 4096; i++) {
        s_OriginalNatives[i] = s_GNatives[i];
    }
    
    // Count valid entries for logging
    int validCount = 0;
    for (int i = 0; i < 256; i++) {
        if (s_GNatives[i]) validCount++;
    }
    
    s_NativesInit = true;
    LOG_INFO("GNatives: Found at 0x{:08X} ({} valid in first 256)",
             (uint32_t)found, validCount);
    return true;
}

NativeFunc GetNative(uint16_t index)
{
    if (!s_NativesInit || index >= 4096) return nullptr;
    return s_GNatives[index];
}

NativeFunc HookNative(uint16_t index, NativeFunc replacement)
{
    if (!s_NativesInit || index >= 4096) return nullptr;
    
    NativeFunc original = s_GNatives[index];
    
    // Patch the table entry (needs write access)
    DWORD oldProtect;
    VirtualProtect(&s_GNatives[index], sizeof(NativeFunc), PAGE_EXECUTE_READWRITE, &oldProtect);
    s_GNatives[index] = replacement;
    VirtualProtect(&s_GNatives[index], sizeof(NativeFunc), oldProtect, &oldProtect);
    
    LOG_INFO("GNatives[{}]: Hooked (orig=0x{:08X}, new=0x{:08X})",
             index, (uint32_t)(uintptr_t)original, (uint32_t)(uintptr_t)replacement);
    return original;
}

void UnhookNative(uint16_t index)
{
    if (!s_NativesInit || index >= 4096) return;
    if (!s_OriginalNatives[index]) return;
    
    DWORD oldProtect;
    VirtualProtect(&s_GNatives[index], sizeof(NativeFunc), PAGE_EXECUTE_READWRITE, &oldProtect);
    s_GNatives[index] = s_OriginalNatives[index];
    VirtualProtect(&s_GNatives[index], sizeof(NativeFunc), oldProtect, &oldProtect);
}

uintptr_t GetNativesTableAddress()
{
    return s_GNativesAddr;
}

int GetNativeCount()
{
    if (!s_NativesInit) return 0;
    int count = 0;
    for (int i = 0; i < 4096; i++) {
        if (s_GNatives[i]) count++;
    }
    return count;
}

void DumpNativesToFile(const std::string& filepath)
{
    if (!s_NativesInit) return;

    uintptr_t modBase = Memory::GetModuleBase(nullptr);
    
    std::ofstream out(filepath);
    out << "=== GNatives Table Dump ===" << std::endl;
    out << "Table address: 0x" << std::hex << s_GNativesAddr << std::dec << std::endl;
    out << "Module base: 0x" << std::hex << modBase << std::dec << std::endl;
    out << std::endl;
    
    int populated = 0;
    for (int i = 0; i < 4096; i++) {
        if (!s_GNatives[i]) continue;
        populated++;
        
        uintptr_t addr = (uintptr_t)s_GNatives[i];
        char line[128];
        std::snprintf(line, sizeof(line), "  Native[%4d] = 0x%08X (+0x%06X)",
                     i, (uint32_t)addr, (uint32_t)(addr - modBase));
        out << line << std::endl;
    }
    
    out << std::endl << "Total populated: " << populated << " / 4096" << std::endl;
    out.close();
    
    LOG_INFO("GNatives dump: {} entries written to {}", populated, filepath);
}

// ─── Engine Tick Hook ────────────────────────────────────────────────────

// SEH helper: call a tick callback safely (no C++ objects in this function)
static void SafeCallTickCallback(void (*rawCb)(float), float dt)
{
    __try {
        rawCb(dt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashBreadcrumb("TickHook: callback crashed (SEH caught)");
    }
}

// SEH helper: call original tick safely
static void SafeCallOriginalTick(void* thisEngine, float dt)
{
    __try {
        if (s_OriginalTick) {
            s_OriginalTick(thisEngine, dt);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashBreadcrumb("TickHook: original tick crashed (SEH caught)");
    }
}

static void __fastcall HookedEngineTick(void* thisEngine, void* /*edx*/, float deltaTime)
{
    s_TickCount++;
    
    // Calculate tick rate
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    if (s_LastTickTime > 0) {
        double elapsed = (double)(now - s_LastTickTime) / 1000000000.0; // ns to seconds
        if (elapsed > 0.0001)
            s_TickRate = 1.0f / (float)elapsed;
    }
    s_LastTickTime = now;
    
    // Call registered callbacks
    {
        std::lock_guard<std::mutex> lock(s_TickMutex);
        for (auto& [id, cb] : s_TickCallbacks) {
            if (cb) cb(deltaTime);
        }
    }
    
    // Call original
    SafeCallOriginalTick(thisEngine, deltaTime);
}

// ─── PE-Based Tick (safe alternative) ────────────────────────────────────
// Instead of hooking a vtable entry (which can hook the wrong function),
// we register a ProcessEvent listener that detects per-frame function calls
// like "Tick" or "PlayerTick" and fires our tick callbacks from there.
// This is safe because it uses the already-proven PE hook infrastructure.

static int s_PETickHookId = -1;
static uint64_t s_PETickCount = 0;

bool InstallTickHook()
{
    if (s_TickHooked) return true;

    if (!IsProcessEventHooked()) {
        // Need PE hook first — try to initialize it
        if (!InitProcessEventHook()) {
            LOG_WARN("TickHook: ProcessEvent hook not available");
            return false;
        }
    }

    ProcessEventHook hook;
    hook.Name = "PETick";
    hook.FunctionFilter = "Tick"; // Only fires for functions named exactly "Tick"
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        // Only fire once per frame — use the FIRST "Tick" call each frame
        // (there can be multiple Tick calls per frame for different objects)
        s_PETickCount++;

        // Calculate delta time from wall clock
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        float deltaTime = 0.016f; // default 60fps
        if (s_LastTickTime > 0) {
            double elapsed = (double)(now - s_LastTickTime) / 1000000000.0;
            if (elapsed > 0.0001 && elapsed < 1.0) {
                deltaTime = (float)elapsed;
                s_TickRate = 1.0f / deltaTime;
            }
        }

        // Only fire callbacks once per unique frame time (debounce)
        // Multiple objects get Tick per frame — we only want the first one
        if (now - s_LastTickTime < 1000000) { // less than 1ms since last
            return false; // same frame, skip
        }
        s_LastTickTime = now;
        s_TickCount++;

        // Fire registered tick callbacks
        {
            std::lock_guard<std::mutex> lock(s_TickMutex);
            for (auto& [id, cb] : s_TickCallbacks) {
                if (cb) cb(deltaTime);
            }
        }

        return false; // never block the original Tick call
    };

    s_PETickHookId = RegisterProcessEventHook(hook);
    s_TickHooked = true;
    LOG_INFO("TickHook: Installed via ProcessEvent listener (safe mode)");
    return true;
}

void RemoveTickHook()
{
    if (!s_TickHooked) return;

    // Remove PE-based tick if active
    if (s_PETickHookId >= 0) {
        UnregisterProcessEventHook(s_PETickHookId);
        s_PETickHookId = -1;
    }

    s_TickHooked = false;
    s_OriginalTick = nullptr;
    
    std::lock_guard<std::mutex> lock(s_TickMutex);
    s_TickCallbacks.clear();
    LOG_INFO("TickHook: Removed");
}

int RegisterTickCallback(TickCallback callback)
{
    std::lock_guard<std::mutex> lock(s_TickMutex);
    int id = s_NextTickId++;
    s_TickCallbacks.push_back({id, std::move(callback)});
    return id;
}

void UnregisterTickCallback(int id)
{
    std::lock_guard<std::mutex> lock(s_TickMutex);
    s_TickCallbacks.erase(
        std::remove_if(s_TickCallbacks.begin(), s_TickCallbacks.end(),
                       [id](const auto& p) { return p.first == id; }),
        s_TickCallbacks.end());
}

bool IsTickHookActive()
{
    return s_TickHooked;
}

float GetTickRate()
{
    return s_TickRate;
}

} // namespace bs1sdk
