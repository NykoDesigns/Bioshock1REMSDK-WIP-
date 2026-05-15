#include "coop_debug.h"
#include "crash_handler.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../engine/world.h"
#include "../hooks/process_event.h"
#include "../network/net_manager.h"
#include "../network/coop_bridge.h"

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <chrono>
#include <mutex>
#include <atomic>
#include <functional>
#include <filesystem>

namespace bs1sdk {

// ─── Debug Directory (resolved at runtime) ───────────────────────────────

static char s_DebugDir[MAX_PATH] = {0};

const char* GetDebugDir()
{
    if (s_DebugDir[0] == 0) {
        // Resolve to BS1SDK_dumps next to the game EXE
        char exePath[MAX_PATH];
        GetModuleFileNameA(GetModuleHandleA(NULL), exePath, MAX_PATH);
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) *lastSlash = '\0';
        snprintf(s_DebugDir, MAX_PATH, "%s\\BS1SDK_dumps", exePath);
    }
    return s_DebugDir;
}

// ─── Helpers ─────────────────────────────────────────────────────────────

static std::string GetTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt;
    localtime_s(&lt, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

static std::string GetTimestampFile()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt;
    localtime_s(&lt, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

static void EnsureDir()
{
    CreateDirectoryA(GetDebugDir(), NULL);
}

// ─── Session Log ─────────────────────────────────────────────────────────

static std::mutex s_LogMutex;

void DebugSessionLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(s_LogMutex);
    EnsureDir();
    std::ofstream out(std::string(GetDebugDir()) + "/session_log.txt", std::ios::app);
    out << "[" << GetTimestamp() << "] " << msg << "\n";
    out.flush();
}

void DebugSessionLogf(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    DebugSessionLog(buf);
}

// ─── Actor Census ────────────────────────────────────────────────────────

std::map<std::string, ActorCensusEntry> RunActorCensus()
{
    std::map<std::string, ActorCensusEntry> census;

    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return census;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);

        std::string cn = obj->GetObjClassName();
        auto& entry = census[cn];
        entry.ClassName = cn;
        entry.Count++;
        if ((int)entry.SampleNames.size() < 5)
            entry.SampleNames.push_back(obj->GetName());
    }

    return census;
}

void DumpActorCensus()
{
    EnsureDir();
    auto census = RunActorCensus();

    // Sort by count descending
    std::vector<std::pair<std::string, ActorCensusEntry>> sorted(census.begin(), census.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.Count > b.second.Count; });

    std::string filepath = std::string(GetDebugDir()) + "/actor_census.txt";
    std::ofstream out(filepath);
    out << "=== Actor Census === " << GetTimestamp() << "\n";
    out << "Total classes: " << sorted.size() << "\n\n";

    int totalObjects = 0;
    for (auto& [cn, entry] : sorted) {
        totalObjects += entry.Count;
        out << entry.Count << "x " << cn << "\n";
        for (auto& name : entry.SampleNames) {
            out << "    " << name << "\n";
        }
    }
    out << "\nTotal objects: " << totalObjects << "\n";
    out.close();

    LOG_INFO("[Debug] Actor census: {} classes, {} objects -> {}", sorted.size(), totalObjects, filepath);
    DebugSessionLogf("Actor census: %d classes, %d objects", (int)sorted.size(), totalObjects);
}

// ─── Property Offset Table ───────────────────────────────────────────────

static int FindPropOffset(UStruct* cls, const std::string& propName)
{
    auto props = WalkProperties(cls);
    for (auto& p : props) {
        if (p.Name == propName) return p.Offset;
    }
    return -1;
}

std::map<std::string, ClassOffsets> DiscoverAllClassOffsets()
{
    std::map<std::string, ClassOffsets> result;

    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return result;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Collect unique class objects
    std::map<std::string, UStruct*> classes;
    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() == "Class") {
            std::string name = obj->GetName();
            if (classes.find(name) == classes.end())
                classes[name] = reinterpret_cast<UStruct*>(obj);
        }
    }

    // For each class, find key offsets
    static const char* targetClasses[] = {
        "Actor", "Pawn", "ShockPlayer", "ShockPlayerController",
        "ShockAIController", "ShockAI", "AIController",
        "ShockWeapon", "ShockProjectile", "ShockAbility",
        "ShockGameInfo", "Trigger", "Mover", "Light",
        "SecurityBot", "BigDaddy", "LittleSister",
        "MeleeThug", "RangedAggressor", "CeilingCrawler",
        "Assassin", "Grenadier", "SpawnManager",
        "AggressorSpawner", "PhysicsVolume", "Volume",
        "NavigationPoint", "PlayerStart", "Emitter",
        nullptr
    };

    for (int ci = 0; targetClasses[ci]; ci++) {
        std::string clsName = targetClasses[ci];
        auto it = classes.find(clsName);
        if (it == classes.end()) continue;

        UStruct* cls = it->second;
        ClassOffsets co;
        co.ClassName = clsName;
        co.PropertiesSize = cls->GetPropertiesSize();
        co.Location = FindPropOffset(cls, "Location");
        co.Rotation = FindPropOffset(cls, "Rotation");
        co.Velocity = FindPropOffset(cls, "Velocity");
        co.Health = FindPropOffset(cls, "Health");
        co.Physics = FindPropOffset(cls, "Physics");
        co.CollisionRadius = FindPropOffset(cls, "CollisionRadius");
        co.bHidden = FindPropOffset(cls, "bHidden");
        co.bDeleteMe = FindPropOffset(cls, "bDeleteMe");
        co.LifeSpan = FindPropOffset(cls, "LifeSpan");
        result[clsName] = co;
    }

    return result;
}

void DumpPropertyOffsets()
{
    EnsureDir();
    auto offsets = DiscoverAllClassOffsets();

    std::string filepath = std::string(GetDebugDir()) + "/property_offsets.txt";
    std::ofstream out(filepath);
    out << "=== Property Offset Table === " << GetTimestamp() << "\n";
    out << "Classes discovered: " << offsets.size() << "\n\n";

    for (auto& [cn, co] : offsets) {
        out << cn << " (size=0x" << std::hex << co.PropertiesSize << std::dec << "):\n";
        if (co.Location >= 0)        out << "  Location        = +0x" << std::hex << co.Location << std::dec << "\n";
        if (co.Rotation >= 0)        out << "  Rotation        = +0x" << std::hex << co.Rotation << std::dec << "\n";
        if (co.Velocity >= 0)        out << "  Velocity        = +0x" << std::hex << co.Velocity << std::dec << "\n";
        if (co.Health >= 0)          out << "  Health          = +0x" << std::hex << co.Health << std::dec << "\n";
        if (co.Physics >= 0)         out << "  Physics         = +0x" << std::hex << co.Physics << std::dec << "\n";
        if (co.CollisionRadius >= 0) out << "  CollisionRadius = +0x" << std::hex << co.CollisionRadius << std::dec << "\n";
        if (co.bHidden >= 0)         out << "  bHidden         = +0x" << std::hex << co.bHidden << std::dec << "\n";
        if (co.bDeleteMe >= 0)       out << "  bDeleteMe       = +0x" << std::hex << co.bDeleteMe << std::dec << "\n";
        if (co.LifeSpan >= 0)        out << "  LifeSpan        = +0x" << std::hex << co.LifeSpan << std::dec << "\n";
        out << "\n";
    }

    out.close();
    LOG_INFO("[Debug] Property offsets: {} classes -> {}", offsets.size(), filepath);
    DebugSessionLogf("Property offsets: %d classes dumped", (int)offsets.size());
}

// ─── ProcessEvent Frequency Tracker ──────────────────────────────────────

static std::mutex s_PEMutex;
static std::map<std::string, PETrackEntry> s_PETracker;
static std::atomic<bool> s_PETracking{false};
static float s_PETrackDuration = 10.0f;
static float s_PETrackElapsed = 0.0f;
static int s_PEHookId = -1;
static int s_PETickId = -1;

void StartPETracker(float durationSeconds)
{
    if (s_PETracking) return;

    s_PETracker.clear();
    s_PETrackDuration = durationSeconds;
    s_PETrackElapsed = 0.0f;
    s_PETracking = true;

    // Register PE hook to count calls
    if (IsProcessEventHooked()) {
        ProcessEventHook hook;
        hook.Name = "PETracker";
        hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
            if (!s_PETracking) return false;

            std::string funcName = func->GetName();
            std::string className = obj->GetObjClassName();
            std::string key = className + "." + funcName;

            std::lock_guard<std::mutex> lock(s_PEMutex);
            auto& entry = s_PETracker[key];
            if (entry.Count == 0) {
                entry.FunctionName = funcName;
                entry.ClassName = className;
                entry.FirstTime = s_PETrackElapsed;
            }
            entry.Count++;
            entry.LastTime = s_PETrackElapsed;
            return false;
        };
        s_PEHookId = RegisterProcessEventHook(hook);
    }

    // Register tick callback to track elapsed time and auto-stop
    s_PETickId = RegisterTickCallback([](float dt) {
        if (!s_PETracking) return;
        s_PETrackElapsed += dt;
        if (s_PETrackElapsed >= s_PETrackDuration) {
            StopPETracker();
            DumpPEFrequency();
            LOG_INFO("[Debug] PE tracker auto-stopped after {:.1f}s", s_PETrackDuration);
        }
    });

    LOG_INFO("[Debug] PE frequency tracker started ({:.0f}s)", durationSeconds);
    DebugSessionLogf("PE tracker started (%.0fs)", durationSeconds);
}

void StopPETracker()
{
    s_PETracking = false;
    if (s_PEHookId >= 0) {
        UnregisterProcessEventHook(s_PEHookId);
        s_PEHookId = -1;
    }
    if (s_PETickId >= 0) {
        UnregisterTickCallback(s_PETickId);
        s_PETickId = -1;
    }
}

bool IsPETrackerRunning()
{
    return s_PETracking;
}

void DumpPEFrequency()
{
    EnsureDir();
    std::lock_guard<std::mutex> lock(s_PEMutex);

    // Sort by count descending
    std::vector<std::pair<std::string, PETrackEntry>> sorted(s_PETracker.begin(), s_PETracker.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.Count > b.second.Count; });

    std::string filepath = std::string(GetDebugDir()) + "/pe_frequency.txt";
    std::ofstream out(filepath);
    out << "=== ProcessEvent Frequency === " << GetTimestamp() << "\n";
    out << "Duration: " << s_PETrackElapsed << "s\n";
    out << "Unique function+class combos: " << sorted.size() << "\n\n";

    // Summary: top functions by total calls
    out << "─── TOP 50 BY CALL COUNT ───\n\n";
    out << std::left;
    char line[256];
    std::snprintf(line, sizeof(line), "%-40s %-25s %10s %8s\n",
                  "Function", "Class", "Count", "Hz");
    out << line;
    std::snprintf(line, sizeof(line), "%-40s %-25s %10s %8s\n",
                  "────────", "─────", "─────", "──");
    out << line;

    int shown = 0;
    uint64_t totalCalls = 0;
    for (auto& [key, entry] : sorted) {
        totalCalls += entry.Count;
        if (shown++ < 50) {
            float hz = (s_PETrackElapsed > 0) ? entry.Count / s_PETrackElapsed : 0;
            std::snprintf(line, sizeof(line), "%-40s %-25s %10llu %7.1f\n",
                         entry.FunctionName.c_str(), entry.ClassName.c_str(),
                         (unsigned long long)entry.Count, hz);
            out << line;
        }
    }

    out << "\nTotal PE calls: " << totalCalls << "\n";
    if (s_PETrackElapsed > 0) {
        out << "Average PE calls/sec: " << (uint64_t)(totalCalls / s_PETrackElapsed) << "\n";
    }

    // Group by function name (across all classes)
    out << "\n─── GROUPED BY FUNCTION NAME ───\n\n";
    std::map<std::string, uint64_t> funcTotals;
    for (auto& [key, entry] : sorted) {
        funcTotals[entry.FunctionName] += entry.Count;
    }
    std::vector<std::pair<std::string, uint64_t>> funcSorted(funcTotals.begin(), funcTotals.end());
    std::sort(funcSorted.begin(), funcSorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (auto& [fn, count] : funcSorted) {
        float hz = (s_PETrackElapsed > 0) ? count / s_PETrackElapsed : 0;
        std::snprintf(line, sizeof(line), "%-50s %10llu (%6.1f Hz)\n",
                     fn.c_str(), (unsigned long long)count, hz);
        out << line;
    }

    // Functions that contain Tick/Timer/AI keywords (CRITICAL for freeze)
    out << "\n─── TICK/TIMER/AI FUNCTIONS (freeze candidates) ───\n\n";
    for (auto& [key, entry] : sorted) {
        std::string fn = entry.FunctionName;
        // Case-insensitive check
        std::string fnLower = fn;
        std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(), ::tolower);
        if (fnLower.find("tick") != std::string::npos ||
            fnLower.find("timer") != std::string::npos ||
            fnLower.find("think") != std::string::npos ||
            fnLower.find("ai") != std::string::npos ||
            fnLower.find("spawn") != std::string::npos ||
            fnLower.find("physics") != std::string::npos ||
            fnLower.find("movement") != std::string::npos) {
            float hz = (s_PETrackElapsed > 0) ? entry.Count / s_PETrackElapsed : 0;
            std::snprintf(line, sizeof(line), "  %-35s on %-25s %8llu (%5.1f Hz)\n",
                         fn.c_str(), entry.ClassName.c_str(),
                         (unsigned long long)entry.Count, hz);
            out << line;
        }
    }

    out.close();
    LOG_INFO("[Debug] PE frequency: {} combos, {} total calls -> {}",
             sorted.size(), totalCalls, filepath);
    DebugSessionLogf("PE frequency: %d combos, %llu total calls",
                     (int)sorted.size(), (unsigned long long)totalCalls);
}

// ─── AI Function Catalog ─────────────────────────────────────────────────

void DumpAIFunctions()
{
    EnsureDir();

    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    struct FuncEntry {
        std::string ClassName;
        std::string FuncName;
        uint32_t Flags;
        bool IsNative;
        uint16_t NativeIdx;
    };

    std::vector<FuncEntry> results;

    // Scan all Class objects for interesting functions
    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (obj->GetObjClassName() != "Class") continue;

        std::string className = obj->GetName();
        UStruct* cls = reinterpret_cast<UStruct*>(obj);

        // Walk direct children only (not inherited)
        UField* child = cls->GetChildren();
        int limit = 2000;
        while (child && limit-- > 0) {
            if (child->GetObjClassName() == "Function") {
                std::string fn = child->GetName();
                std::string fnLower = fn;
                std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(), ::tolower);

                if (fnLower.find("tick") != std::string::npos ||
                    fnLower.find("timer") != std::string::npos ||
                    fnLower.find("think") != std::string::npos ||
                    fnLower.find("ai") != std::string::npos ||
                    fnLower.find("spawn") != std::string::npos ||
                    fnLower.find("physics") != std::string::npos ||
                    fnLower.find("movement") != std::string::npos ||
                    fnLower.find("update") != std::string::npos ||
                    fnLower.find("pawn") != std::string::npos ||
                    fnLower.find("controller") != std::string::npos ||
                    fnLower.find("damage") != std::string::npos ||
                    fnLower.find("death") != std::string::npos ||
                    fnLower.find("attack") != std::string::npos ||
                    fnLower.find("fire") != std::string::npos ||
                    fnLower.find("enemy") != std::string::npos ||
                    fnLower.find("target") != std::string::npos ||
                    fnLower.find("friendly") != std::string::npos) {

                    UFunction* func = reinterpret_cast<UFunction*>(child);
                    FuncEntry fe;
                    fe.ClassName = className;
                    fe.FuncName = fn;
                    fe.Flags = func->GetFunctionFlags();
                    fe.IsNative = func->IsNative();
                    fe.NativeIdx = func->GetNativeIndex();
                    results.push_back(fe);
                }
            }
            child = child->GetNext();
        }
    }

    // Sort by class then function name
    std::sort(results.begin(), results.end(), [](const FuncEntry& a, const FuncEntry& b) {
        if (a.ClassName != b.ClassName) return a.ClassName < b.ClassName;
        return a.FuncName < b.FuncName;
    });

    std::string filepath = std::string(GetDebugDir()) + "/ai_functions.txt";
    std::ofstream out(filepath);
    out << "=== AI/Tick/Spawn Function Catalog === " << GetTimestamp() << "\n";
    out << "Total matching functions: " << results.size() << "\n\n";

    std::string lastClass;
    for (auto& fe : results) {
        if (fe.ClassName != lastClass) {
            out << "\n── " << fe.ClassName << " ──\n";
            lastClass = fe.ClassName;
        }
        char line[256];
        std::snprintf(line, sizeof(line), "  %-40s flags=0x%04X%s%s\n",
                     fe.FuncName.c_str(), fe.Flags,
                     fe.IsNative ? " [Native]" : "",
                     fe.NativeIdx > 0 ? (" idx=" + std::to_string(fe.NativeIdx)).c_str() : "");
        out << line;
    }

    out.close();
    LOG_INFO("[Debug] AI functions: {} matches -> {}", results.size(), filepath);
    DebugSessionLogf("AI function catalog: %d functions", (int)results.size());
}

// ─── Snapshot Diff ───────────────────────────────────────────────────────

struct ActorSnap {
    std::string Name;
    std::string ClassName;
    float PosX, PosY, PosZ;
};

static std::vector<ActorSnap> s_SnapA, s_SnapB;

static std::vector<ActorSnap> TakeActorSnapshot()
{
    std::vector<ActorSnap> snap;
    if (!IsWorldSystemReady()) return snap;

    auto actors = GetAllActors();
    for (auto* actor : actors) {
        ActorSnap as;
        as.Name = actor->GetName();
        as.ClassName = actor->GetObjClassName();
        FVec3 pos;
        if (GetActorPosition(actor, pos)) {
            as.PosX = pos.X; as.PosY = pos.Y; as.PosZ = pos.Z;
        } else {
            as.PosX = as.PosY = as.PosZ = 0;
        }
        snap.push_back(as);
    }
    return snap;
}

void MarkSnapshotA()
{
    s_SnapA = TakeActorSnapshot();
    LOG_INFO("[Debug] Snapshot A: {} actors", s_SnapA.size());
    DebugSessionLogf("Snapshot A: %d actors", (int)s_SnapA.size());
}

void MarkSnapshotB()
{
    s_SnapB = TakeActorSnapshot();
    LOG_INFO("[Debug] Snapshot B: {} actors", s_SnapB.size());
    DebugSessionLogf("Snapshot B: %d actors", (int)s_SnapB.size());
}

void DumpSnapshotDiff()
{
    EnsureDir();
    std::string filepath = std::string(GetDebugDir()) + "/snapshot_diff.txt";
    std::ofstream out(filepath);
    out << "=== Snapshot Diff === " << GetTimestamp() << "\n";
    out << "Snap A: " << s_SnapA.size() << " actors\n";
    out << "Snap B: " << s_SnapB.size() << " actors\n\n";

    // Build name->actor maps
    std::map<std::string, ActorSnap*> mapA, mapB;
    for (auto& a : s_SnapA) mapA[a.Name] = &a;
    for (auto& b : s_SnapB) mapB[b.Name] = &b;

    // Actors in B but not A (added)
    out << "─── ADDED (in B, not in A) ───\n";
    int added = 0;
    for (auto& b : s_SnapB) {
        if (mapA.find(b.Name) == mapA.end()) {
            char line[256];
            std::snprintf(line, sizeof(line), "  + [%s] %s at (%.0f, %.0f, %.0f)\n",
                         b.ClassName.c_str(), b.Name.c_str(), b.PosX, b.PosY, b.PosZ);
            out << line;
            added++;
        }
    }
    if (added == 0) out << "  (none)\n";

    // Actors in A but not B (removed)
    out << "\n─── REMOVED (in A, not in B) ───\n";
    int removed = 0;
    for (auto& a : s_SnapA) {
        if (mapB.find(a.Name) == mapB.end()) {
            char line[256];
            std::snprintf(line, sizeof(line), "  - [%s] %s at (%.0f, %.0f, %.0f)\n",
                         a.ClassName.c_str(), a.Name.c_str(), a.PosX, a.PosY, a.PosZ);
            out << line;
            removed++;
        }
    }
    if (removed == 0) out << "  (none)\n";

    // Actors that moved significantly (>50 units)
    out << "\n─── MOVED (>50 units) ───\n";
    int moved = 0;
    for (auto& b : s_SnapB) {
        auto it = mapA.find(b.Name);
        if (it == mapA.end()) continue;
        ActorSnap& a = *it->second;
        float dx = b.PosX - a.PosX;
        float dy = b.PosY - a.PosY;
        float dz = b.PosZ - a.PosZ;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist > 50.0f) {
            char line[256];
            std::snprintf(line, sizeof(line), "  ~ [%s] %s  %.0f units  (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f)\n",
                         b.ClassName.c_str(), b.Name.c_str(), dist,
                         a.PosX, a.PosY, a.PosZ, b.PosX, b.PosY, b.PosZ);
            out << line;
            moved++;
        }
    }
    if (moved == 0) out << "  (none)\n";

    out << "\nSummary: +" << added << " added, -" << removed << " removed, ~" << moved << " moved\n";
    out.close();

    LOG_INFO("[Debug] Diff: +{} -{} ~{} -> {}", added, removed, moved, filepath);
}

// ─── Full Engine Snapshot ────────────────────────────────────────────────

void DumpFullSnapshot(const std::string& label)
{
    EnsureDir();
    std::string filename = std::string(GetDebugDir()) + "/snapshot_" + GetTimestampFile() + ".txt";
    std::ofstream out(filename);

    out << "╔══════════════════════════════════════════════════════════════╗\n";
    out << "║  BS1SDK Engine Snapshot                                     ║\n";
    out << "║  " << GetTimestamp() << "                                       ║\n";
    if (!label.empty())
        out << "║  Label: " << label << "\n";
    out << "╚══════════════════════════════════════════════════════════════╝\n\n";

    // Engine globals
    auto& globals = GetEngineGlobals();
    out << "─── Engine Globals ───\n";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "  GObjects = 0x%08X  GNames = 0x%08X\n",
                 (uint32_t)globals.GObjects, (uint32_t)globals.GNames);
    out << buf;
    std::snprintf(buf, sizeof(buf), "  GEngine  = 0x%08X  GWorld = 0x%08X\n",
                 (uint32_t)globals.GEngine, (uint32_t)globals.GWorld);
    out << buf;
    std::snprintf(buf, sizeof(buf), "  GNatives = 0x%08X (%d populated)\n",
                 (uint32_t)GetNativesTableAddress(), GetNativeCount());
    out << buf;
    std::snprintf(buf, sizeof(buf), "  Tick rate: %.1f FPS  Hook: %s\n",
                 GetTickRate(), IsTickHookActive() ? "active" : "inactive");
    out << buf;
    out << "\n";

    // World info
    if (IsWorldSystemReady()) {
        LevelInfo lvl = GetCurrentLevel();
        out << "─── World ───\n";
        std::snprintf(buf, sizeof(buf), "  Level: %s\n  Actors: %d\n  Level ptr: 0x%08X\n",
                     lvl.LevelName.c_str(), lvl.ActorCount, (uint32_t)lvl.LevelPtr);
        out << buf;
    } else {
        out << "─── World ───\n  NOT READY\n";
    }
    out << "\n";

    // PE stats
    auto& peStats = GetProcessEventStats();
    out << "─── ProcessEvent ───\n";
    std::snprintf(buf, sizeof(buf), "  Total calls: %llu  Blocked: %llu\n",
                 (unsigned long long)peStats.TotalCalls, (unsigned long long)peStats.BlockedCalls);
    out << buf;
    out << "  Last: " << peStats.LastObjectName << "." << peStats.LastFunctionName << "\n\n";

    // Network
    out << "─── Network ───\n";
    out << "  " << GetNetStatus() << "\n";
    if (IsCoopActive()) {
        out << "  Co-op: ACTIVE\n";
        out << "  " << GetCoopStatus() << "\n";
    } else {
        out << "  Co-op: not active\n";
    }
    out << "\n";

    // Player info
    FVec3 playerPos;
    if (GetPlayerPosition(playerPos)) {
        std::snprintf(buf, sizeof(buf), "─── Player ───\n  Position: (%.1f, %.1f, %.1f)\n",
                     playerPos.X, playerPos.Y, playerPos.Z);
        out << buf;
    }
    out << "\n";

    // Actor census (inline)
    auto census = RunActorCensus();
    std::vector<std::pair<std::string, ActorCensusEntry>> sorted(census.begin(), census.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.Count > b.second.Count; });

    out << "─── Actor Census (top 40) ───\n";
    int shown = 0;
    int totalActors = 0;
    for (auto& [cn, entry] : sorted) {
        totalActors += entry.Count;
        if (shown++ < 40) {
            std::snprintf(buf, sizeof(buf), "  %5d  %s\n", entry.Count, cn.c_str());
            out << buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "  Total: %d objects in %d classes\n\n", totalActors, (int)sorted.size());
    out << buf;

    // Key property offsets
    out << "─── Key Offsets ───\n";
    auto offsets = DiscoverAllClassOffsets();
    static const char* keyClasses[] = {"Actor", "Pawn", "ShockPlayer", "ShockAIController", nullptr};
    for (int ci = 0; keyClasses[ci]; ci++) {
        auto it = offsets.find(keyClasses[ci]);
        if (it == offsets.end()) continue;
        auto& co = it->second;
        std::snprintf(buf, sizeof(buf),
            "  %s: Loc=+0x%X Rot=+0x%X Vel=+0x%X HP=+0x%X (size=0x%X)\n",
            co.ClassName.c_str(),
            co.Location, co.Rotation, co.Velocity, co.Health, co.PropertiesSize);
        out << buf;
    }
    out << "\n";

    out.close();
    LOG_INFO("[Debug] Full snapshot -> {}", filename);
    DebugSessionLogf("Full snapshot dumped: %s", label.c_str());
}

// ─── SDK Generation ──────────────────────────────────────────────────────

void DumpSDKHeaders()
{
    EnsureDir();
    std::string sdkDir = std::string(GetDebugDir()) + "/SDK";
    std::filesystem::create_directories(sdkDir);

    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Collect all Class objects
    struct ClassInfo {
        UStruct* cls;
        std::string name;
        std::string superName;
    };
    std::vector<ClassInfo> allClasses;

    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (!IsSafeToRead(obj, 64)) continue;
        std::string cn = obj->GetObjClassName();
        if (cn == "Class") {
            std::string name = obj->GetName();
            UStruct* cls = reinterpret_cast<UStruct*>(obj);
            std::string superName;
            UField* super = cls->GetSuperField();
            if (super && IsSafeToRead(super, 32)) {
                superName = super->GetName();
            }
            allClasses.push_back({cls, name, superName});
        }
    }

    // Sort by name
    std::sort(allClasses.begin(), allClasses.end(),
              [](const ClassInfo& a, const ClassInfo& b) { return a.name < b.name; });

    // Write one file per class
    int written = 0;
    for (auto& ci : allClasses) {
        std::string filename = sdkDir + "/" + ci.name + ".txt";
        std::ofstream f(filename);
        if (!f.is_open()) continue;

        f << "// Class: " << ci.name << "\n";
        if (!ci.superName.empty()) f << "// Super: " << ci.superName << "\n";
        f << "// Address: 0x" << std::hex << (uintptr_t)ci.cls << std::dec << "\n";
        f << "\n";

        // Properties
        std::vector<PropertyInfo> props = WalkProperties(ci.cls);
        std::sort(props.begin(), props.end(),
                  [](const PropertyInfo& a, const PropertyInfo& b) { return a.Offset < b.Offset; });

        f << "// Properties (" << props.size() << "):\n";
        f << "// Offset  Size  Type                    Name\n";
        f << "//──────────────────────────────────────────────────────\n";
        for (auto& p : props) {
            char line[256];
            std::snprintf(line, sizeof(line), "  /* 0x%04X */ %-4d %-24s %s",
                         p.Offset, p.ElementSize, p.TypeName.c_str(), p.Name.c_str());
            f << line;
            if (p.ArrayDim > 1) f << "[" << p.ArrayDim << "]";
            f << "\n";
        }

        // Functions (children that are UFunction)
        f << "\n// Functions:\n";
        UField* child = ci.cls->GetChildren();
        int funcCount = 0;
        int limit = 3000;
        while (child && limit-- > 0) {
            if (!IsSafeToRead(child, 32)) break;
            std::string childClass = child->GetObjClassName();
            if (childClass == "Function") {
                std::string funcName = child->GetName();
                f << "  function " << funcName << "()\n";
                funcCount++;
            }
            child = child->GetNext();
        }
        if (funcCount == 0) f << "  // (none declared on this class)\n";

        f.close();
        written++;
    }

    LOG_INFO("[SDK] Dumped {} class headers to {}", written, sdkDir);
}

// ─── Object Inspector ────────────────────────────────────────────────────

std::string InspectObject(const std::string& classNameOrAddr)
{
    UObject* target = nullptr;
    std::string result;
    char buf[512];

    // Try as hex address first
    if (classNameOrAddr.size() > 2 && classNameOrAddr[0] == '0' && classNameOrAddr[1] == 'x') {
        uintptr_t addr = (uintptr_t)std::strtoull(classNameOrAddr.c_str(), nullptr, 16);
        if (IsSafeToRead(reinterpret_cast<void*>(addr), 64)) {
            target = reinterpret_cast<UObject*>(addr);
        }
    }

    // Try as class name
    if (!target) {
        target = FindObjectByClassName(classNameOrAddr);
    }

    if (!target) {
        return "Object not found: " + classNameOrAddr;
    }

    std::string cn = target->GetObjClassName();
    std::string name = target->GetName();
    std::snprintf(buf, sizeof(buf), "=== %s '%s' @ 0x%08X ===\n",
                 cn.c_str(), name.c_str(), (uint32_t)(uintptr_t)target);
    result += buf;

    // Get class and walk properties, reading current values
    UStruct* cls = reinterpret_cast<UStruct*>(target->GetClass());
    if (!cls) return result + "(no class info)\n";

    std::string superName;
    UField* super = cls->GetSuperField();
    if (super && IsSafeToRead(super, 32)) superName = super->GetName();
    result += "Class: " + cn + " extends " + superName + "\n\n";

    std::vector<PropertyInfo> props = WalkProperties(cls);
    std::sort(props.begin(), props.end(),
              [](const PropertyInfo& a, const PropertyInfo& b) { return a.Offset < b.Offset; });

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(target);
    for (auto& p : props) {
        if (p.Offset < 0 || p.Offset > 8192) continue;

        std::snprintf(buf, sizeof(buf), "  [0x%04X] %-20s %-24s = ",
                     p.Offset, p.TypeName.c_str(), p.Name.c_str());
        result += buf;

        // Read and format value based on type
        if (p.TypeName == "IntProperty" || p.TypeName == "BoolProperty") {
            int32_t val = 0;
            memcpy(&val, raw + p.Offset, std::min(p.ElementSize, 4));
            std::snprintf(buf, sizeof(buf), "%d", val);
            result += buf;
        } else if (p.TypeName == "FloatProperty") {
            float val = 0;
            memcpy(&val, raw + p.Offset, 4);
            std::snprintf(buf, sizeof(buf), "%.4f", val);
            result += buf;
        } else if (p.TypeName == "ByteProperty") {
            uint8_t val = *(raw + p.Offset);
            std::snprintf(buf, sizeof(buf), "%u (0x%02X)", val, val);
            result += buf;
        } else if (p.TypeName == "StrProperty" || p.TypeName == "NameProperty") {
            // Just show raw bytes for complex types
            std::snprintf(buf, sizeof(buf), "0x%08X",
                         *reinterpret_cast<const uint32_t*>(raw + p.Offset));
            result += buf;
        } else if (p.TypeName == "StructProperty" && p.ElementSize == 12) {
            // Likely a vector
            float x, y, z;
            memcpy(&x, raw + p.Offset, 4);
            memcpy(&y, raw + p.Offset + 4, 4);
            memcpy(&z, raw + p.Offset + 8, 4);
            std::snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)", x, y, z);
            result += buf;
        } else if (p.TypeName == "ObjectProperty" || p.TypeName == "ClassProperty") {
            uintptr_t objPtr = *reinterpret_cast<const uintptr_t*>(raw + p.Offset);
            if (objPtr && IsSafeToRead(reinterpret_cast<void*>(objPtr), 32)) {
                UObject* ref = reinterpret_cast<UObject*>(objPtr);
                std::snprintf(buf, sizeof(buf), "-> %s '%s'",
                             ref->GetObjClassName().c_str(), ref->GetName().c_str());
                result += buf;
            } else {
                std::snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)objPtr);
                result += buf;
            }
        } else {
            // Generic: show first 4 bytes hex
            if (p.ElementSize >= 4) {
                std::snprintf(buf, sizeof(buf), "0x%08X",
                             *reinterpret_cast<const uint32_t*>(raw + p.Offset));
            } else {
                std::snprintf(buf, sizeof(buf), "0x%02X", *(raw + p.Offset));
            }
            result += buf;
        }
        result += "\n";
    }

    return result;
}

// ─── Class Hierarchy ─────────────────────────────────────────────────────

void DumpClassHierarchy()
{
    EnsureDir();
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Collect class -> super mapping
    std::map<std::string, std::string> hierarchy; // child -> parent
    std::map<std::string, std::vector<std::string>> children; // parent -> children

    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (!IsSafeToRead(obj, 64)) continue;
        if (obj->GetObjClassName() != "Class") continue;

        std::string name = obj->GetName();
        UStruct* cls = reinterpret_cast<UStruct*>(obj);
        UField* super = cls->GetSuperField();
        std::string superName;
        if (super && IsSafeToRead(super, 32)) {
            superName = super->GetName();
        }
        hierarchy[name] = superName;
        children[superName].push_back(name);
    }

    // Write tree
    std::string filename = std::string(GetDebugDir()) + "/class_hierarchy.txt";
    std::ofstream f(filename);
    f << "// BioShock Remastered - Class Hierarchy\n";
    f << "// Generated by BS1SDK\n";
    f << "// Total classes: " << hierarchy.size() << "\n\n";

    // Print as indented tree starting from Object
    std::function<void(const std::string&, int)> printTree =
        [&](const std::string& parent, int depth) {
        auto it = children.find(parent);
        if (it == children.end()) return;
        auto sorted = it->second;
        std::sort(sorted.begin(), sorted.end());
        for (auto& child : sorted) {
            for (int i = 0; i < depth; i++) f << "  ";
            f << child << "\n";
            printTree(child, depth + 1);
        }
    };

    f << "Object\n";
    printTree("Object", 1);

    // Also print orphans (no parent found)
    f << "\n// Orphan classes (super not found in GObjects):\n";
    for (auto& [name, super] : hierarchy) {
        if (super.empty() && name != "Object") {
            f << name << "\n";
        }
    }

    f.close();
    LOG_INFO("[SDK] Class hierarchy dumped ({} classes) to {}", hierarchy.size(), filename);
}

// ─── All Functions ───────────────────────────────────────────────────────

void DumpAllFunctions()
{
    EnsureDir();
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    std::string filename = std::string(GetDebugDir()) + "/all_functions.txt";
    std::ofstream f(filename);
    f << "// BioShock Remastered - All UFunction Objects\n";
    f << "// Generated by BS1SDK\n\n";

    struct FuncEntry {
        std::string ownerClass;
        std::string funcName;
        uintptr_t addr;
    };
    std::vector<FuncEntry> funcs;

    for (int i = 0; i < objCount && i < 200000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (!IsSafeToRead(obj, 64)) continue;
        if (obj->GetObjClassName() != "Function") continue;

        std::string funcName = obj->GetName();
        std::string ownerClass;
        UObject* outer = obj->GetOuter();
        if (outer && IsSafeToRead(outer, 32)) {
            ownerClass = outer->GetName();
        }
        funcs.push_back({ownerClass, funcName, ptr});
    }

    // Sort by owner class then function name
    std::sort(funcs.begin(), funcs.end(), [](const FuncEntry& a, const FuncEntry& b) {
        if (a.ownerClass != b.ownerClass) return a.ownerClass < b.ownerClass;
        return a.funcName < b.funcName;
    });

    std::string lastOwner;
    for (auto& fe : funcs) {
        if (fe.ownerClass != lastOwner) {
            f << "\n// ── " << fe.ownerClass << " ──\n";
            lastOwner = fe.ownerClass;
        }
        char line[256];
        std::snprintf(line, sizeof(line), "  [0x%08X] %s::%s()\n",
                     (uint32_t)fe.addr, fe.ownerClass.c_str(), fe.funcName.c_str());
        f << line;
    }

    f.close();
    LOG_INFO("[SDK] Dumped {} functions to {}", funcs.size(), filename);
}

// ─── Init/Shutdown ───────────────────────────────────────────────────────

void InitCoopDebug()
{
    EnsureDir();
    DebugSessionLog("═══ SESSION START ═══");
    DebugSessionLogf("GObjects=0x%08X GNames=0x%08X",
                     (uint32_t)GetEngineGlobals().GObjects,
                     (uint32_t)GetEngineGlobals().GNames);

    // Auto-dump a snapshot on init
    DumpFullSnapshot("auto_init");
    DumpActorCensus();
    DumpPropertyOffsets();
    DumpAIFunctions();

    LOG_INFO("[Debug] Co-op debug tools initialized. Dumps in {}", GetDebugDir());
}

void ShutdownCoopDebug()
{
    StopPETracker();
    DebugSessionLog("═══ SESSION END ═══\n");
}

} // namespace bs1sdk
