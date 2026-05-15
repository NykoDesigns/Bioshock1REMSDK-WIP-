#include "coop_true.h"
#include "coop_world_sync.h"
#include "coop_p2.h"
#include "coop_inventory.h"
#include "coop_transitions.h"
#include "coop_testing.h"
#include "net_manager.h"
#include "coop_bridge.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../engine/world.h"
#include "../hooks/process_event.h"
#include "../debug/coop_debug.h"

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace bs1sdk {

// ─── State ───────────────────────────────────────────────────────────────

static TrueCoopRole s_Role = TrueCoopRole::None;
static bool s_Initialized = false;
static bool s_SimFrozen = false;
static int s_FreezeHookId = -1;
static uint32_t s_HostFrameNum = 0;
static int s_TickCallbackId = -1;

// Functions being blocked on client
static std::mutex s_FreezeMutex;
static std::unordered_set<std::string> s_FrozenFuncKeys;  // "ClassName.FuncName"
static std::vector<std::string> s_FreezePatterns;         // function name patterns to block
static uint64_t s_BlockedCount = 0;

// ─── Role Management ─────────────────────────────────────────────────────

TrueCoopRole GetTrueCoopRole() { return s_Role; }

void SetTrueCoopRole(TrueCoopRole role)
{
    if (s_Role == role) return;
    s_Role = role;
    const char* names[] = {"None", "TrueHost", "TrueClient"};
    LOG_INFO("[TrueCoop] Role set to: {}", names[(int)role]);
    DebugSessionLogf("TrueCoop role: %s", names[(int)role]);

    // Initialize subsystems and activate hooks now that a co-op session is starting
    if (role != TrueCoopRole::None) {
        EnsureSubsystemsReady();
        ActivateTransitionHooks();

        // Register tick callback so TrueCoopTick runs every frame
        if (s_TickCallbackId < 0 && IsTickHookActive()) {
            s_TickCallbackId = RegisterTickCallback([](float dt) {
                TrueCoopTick(dt);
            });
            LOG_INFO("[TrueCoop] Tick callback registered (id={})", s_TickCallbackId);
        }
    } else {
        // Role set to None — unregister tick callback
        if (s_TickCallbackId >= 0) {
            UnregisterTickCallback(s_TickCallbackId);
            s_TickCallbackId = -1;
        }
    }
}

bool IsTrueHost() { return s_Role == TrueCoopRole::TrueHost; }
bool IsTrueClient() { return s_Role == TrueCoopRole::TrueClient; }

// ─── Phase 1: Client Simulation Freeze ───────────────────────────────────
// 
// Strategy: Register a ProcessEvent hook that blocks ALL tick/timer/AI
// functions when we're the TrueClient. The host runs everything; the
// client just renders.
//
// We block functions matching these patterns:
//   - *Tick*       (AI ticking, physics ticking)
//   - *Timer*      (game timers)
//   - *Think*      (AI thinking)
//   - *Spawn*      (no local spawning)
//   - *Movement*   (no local movement processing)
//   - *Physics*    (no local physics)
//   - *AI*         (no local AI decisions)
//
// EXCEPTIONS (never block):
//   - PlayerTick on ShockPlayer/ShockPlayerController (need local input)
//   - Rendering functions
//   - HUD/UI functions

static bool ShouldBlockFunction(const std::string& className, const std::string& funcName)
{
    // NEVER block these — needed for local rendering/input
    if (funcName == "PlayerTick") return false;
    if (funcName == "PlayerCalcView") return false;
    if (funcName == "PostRender") return false;
    if (funcName == "DrawHUD") return false;
    if (funcName == "Tick" && (className == "ShockPlayer" || className == "ShockPlayerController"))
        return false;
    if (funcName == "PostBeginPlay" || funcName == "PreBeginPlay") return false;

    // Block Tick on everything else (AI, pawns, spawners, controllers)
    if (funcName == "Tick") return true;
    if (funcName == "Timer") return true;

    // Block all AI-related functions
    if (funcName.find("Think") != std::string::npos) return true;
    if (funcName.find("AI") != std::string::npos) return true;
    if (funcName.find("Movement") != std::string::npos) return true;
    if (funcName.find("Spawn") != std::string::npos) return true;

    // Block physics on non-player
    if (funcName == "PhysicsTick" || funcName == "PhysicsStep") return true;
    if (funcName == "AutonomousPhysics") return true;

    // Block attack/combat processing
    if (funcName.find("Attack") != std::string::npos) return true;
    if (funcName.find("FireWeapon") != std::string::npos) return true;

    // Block enemy-specific functions
    if (funcName == "TakeDamage" && className != "ShockPlayer") return true;
    if (funcName == "Died" && className != "ShockPlayer") return true;

    return false;
}

bool FreezeClientSimulation()
{
    if (s_SimFrozen) return true;
    if (!IsProcessEventHooked()) {
        LOG_ERROR("[TrueCoop] Cannot freeze: ProcessEvent not hooked");
        return false;
    }

    ProcessEventHook hook;
    hook.Name = "TrueCoopFreeze";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        if (!s_SimFrozen) return false;
        if (!obj || !func) return false;

        std::string funcName = func->GetName();
        std::string className = obj->GetObjClassName();

        if (ShouldBlockFunction(className, funcName)) {
            // Track what we're blocking (for debugging)
            std::string key = className + "." + funcName;
            {
                std::lock_guard<std::mutex> lock(s_FreezeMutex);
                s_FrozenFuncKeys.insert(key);
                s_BlockedCount++;
            }
            return true; // BLOCK the call
        }
        return false;
    };

    s_FreezeHookId = RegisterProcessEventHook(hook);
    s_SimFrozen = true;

    LOG_INFO("[TrueCoop] Client simulation FROZEN (blocking AI/physics/spawn)");
    DebugSessionLog("CLIENT SIMULATION FROZEN");

    return true;
}

void UnfreezeClientSimulation()
{
    if (!s_SimFrozen) return;
    s_SimFrozen = false;

    if (s_FreezeHookId >= 0) {
        UnregisterProcessEventHook(s_FreezeHookId);
        s_FreezeHookId = -1;
    }

    LOG_INFO("[TrueCoop] Client simulation UNFROZEN (blocked {} calls total)", s_BlockedCount);
    DebugSessionLogf("CLIENT SIMULATION UNFROZEN (blocked %llu calls)", (unsigned long long)s_BlockedCount);

    // Dump what was frozen for analysis
    {
        std::lock_guard<std::mutex> lock(s_FreezeMutex);
        std::string filepath = std::string(GetDebugDir()) + "/frozen_functions.txt";
        std::ofstream out(filepath);
        out << "=== Functions Blocked During Freeze ===\n";
        out << "Total blocked calls: " << s_BlockedCount << "\n\n";
        std::vector<std::string> sorted(s_FrozenFuncKeys.begin(), s_FrozenFuncKeys.end());
        std::sort(sorted.begin(), sorted.end());
        for (auto& key : sorted) {
            out << "  " << key << "\n";
        }
        out.close();
        s_FrozenFuncKeys.clear();
        s_BlockedCount = 0;
    }
}

bool IsSimulationFrozen()
{
    return s_SimFrozen;
}

std::vector<std::string> GetFrozenFunctions()
{
    std::lock_guard<std::mutex> lock(s_FreezeMutex);
    return std::vector<std::string>(s_FrozenFuncKeys.begin(), s_FrozenFuncKeys.end());
}

// ─── True Co-op Tick ─────────────────────────────────────────────────────

void TrueCoopTick(float deltaTime)
{
    if (s_Role == TrueCoopRole::None) return;

    // Phase 6: Transitions (both roles)
    TransitionsTick(deltaTime);

    if (IsTrueHost()) {
        s_HostFrameNum++;

        // Phase 2: World state broadcast
        WorldSyncHostTick(deltaTime);

        // Phase 3: Apply P2 input + broadcast P2 state
        if (P2IsSpawned()) {
            P2PawnState p2state = P2GetState();
            // Send P2 state to client at 30Hz
            static float p2SendAccum = 0;
            p2SendAccum += deltaTime;
            if (p2SendAccum >= 1.0f / 30.0f) {
                p2SendAccum = 0;
                NetSendRawPacket((PacketType)TrueCoopPackets::P2State,
                                 &p2state, sizeof(p2state));
            }
        }
    }

    if (IsTrueClient()) {
        // Phase 2: Apply world state from host
        WorldSyncClientTick(deltaTime);

        // Phase 3: Capture and send input to host
        if (!IsInCutscene()) {
            P2CaptureAndSendInput(deltaTime);
        }
    }
}

// ─── Init/Shutdown ───────────────────────────────────────────────────────

static bool s_SubsystemsReady = false;

void EnsureSubsystemsReady()
{
    if (s_SubsystemsReady) return;
    s_SubsystemsReady = true;

    InitWorldSync();
    InitP2System();
    InitP2Inventory();
    InitTransitions();
    InitCoopTesting();

    LOG_INFO("[TrueCoop] Subsystems initialized (all phases)");
    DebugSessionLog("TrueCoop subsystems initialized (all phases)");
}

bool InitTrueCoop()
{
    if (s_Initialized) return true;
    s_Initialized = true;

    // Subsystems are NOT initialized here — they are deferred until a co-op
    // command is used (truehost/truejoin/cooptest/p2spawn) to avoid any
    // interference with normal gameplay.

    LOG_INFO("[TrueCoop] Ready (subsystems deferred until co-op session)");
    DebugSessionLog("TrueCoop ready (deferred)");
    return true;
}

void ShutdownTrueCoop()
{
    if (!s_Initialized) return;
    UnfreezeClientSimulation();

    // Shutdown all subsystems
    ShutdownCoopTesting();
    ShutdownTransitions();
    ShutdownP2Inventory();
    ShutdownP2System();
    ShutdownWorldSync();

    s_Role = TrueCoopRole::None;
    s_Initialized = false;
    DebugSessionLog("TrueCoop shutdown (all phases)");
}

std::string GetTrueCoopStatus()
{
    const char* roleNames[] = {"None", "TrueHost", "TrueClient"};
    std::string status = "Role: ";
    status += roleNames[(int)s_Role];

    if (s_SimFrozen) {
        std::lock_guard<std::mutex> lock(s_FreezeMutex);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "\nFrozen: %d unique funcs, %llu total blocks",
                     (int)s_FrozenFuncKeys.size(), (unsigned long long)s_BlockedCount);
        status += buf;
    }

    if (IsTrueHost()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "\nHost frame: %u", s_HostFrameNum);
        status += buf;
    }

    return status;
}

} // namespace bs1sdk
