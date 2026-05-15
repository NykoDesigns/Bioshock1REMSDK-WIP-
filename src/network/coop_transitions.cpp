#include "coop_transitions.h"
#include "coop_true.h"
#include "coop_p2.h"
#include "coop_world_sync.h"
#include "net_manager.h"
#include "../core/log.h"
#include "../engine/world.h"
#include "../hooks/process_event.h"
#include "../debug/coop_debug.h"

#include <algorithm>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <cstring>
#include <cmath>

namespace bs1sdk {

// ─── State ───────────────────────────────────────────────────────────────

static bool s_Initialized = false;
static TransitionInfo s_Transition;
static CutsceneState s_Cutscene;
static std::vector<BigDaddyState> s_BigDaddies;
static int s_NullGuards = 0;
static std::vector<std::string> s_NullGuardContexts;
static float s_Time = 0;

// PE hook IDs
static int s_LevelHookId = -1;
static int s_CutsceneHookId = -1;
static int s_BigDaddyHookId = -1;

// Logging
static std::ofstream s_TransLog;

static void TransLog(const char* fmt, ...)
{
    if (!s_TransLog.is_open()) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_TransLog << "[" << std::fixed;
    s_TransLog.precision(1);
    s_TransLog << s_Time << "] " << buf << "\n";
    s_TransLog.flush();
}

// ─── Init / Shutdown ─────────────────────────────────────────────────────

void InitTransitions()
{
    if (s_Initialized) return;
    s_Initialized = true;

    std::string logPath = std::string(GetDebugDir()) + "/transitions_log.txt";
    s_TransLog.open(logPath, std::ios::trunc);
    TransLog("Transitions system initialized");

    // Detect current level
    if (IsWorldSystemReady()) {
        LevelInfo lvl = GetCurrentLevel();
        s_Transition.CurrentLevel = lvl.LevelName;
        TransLog("Initial level: %s", lvl.LevelName.c_str());
    }

    // PE hooks are NOT registered here — they are deferred until a co-op session
    // starts via ActivateTransitionHooks() to avoid overhead during normal play.

    LOG_INFO("[Transitions] Initialized (PE hooks deferred until co-op session)");
}

void ShutdownTransitions()
{
    if (!s_Initialized) return;

    if (s_LevelHookId >= 0) UnregisterProcessEventHook(s_LevelHookId);
    if (s_CutsceneHookId >= 0) UnregisterProcessEventHook(s_CutsceneHookId);
    if (s_BigDaddyHookId >= 0) UnregisterProcessEventHook(s_BigDaddyHookId);

    TransLog("Transitions shutdown. NullGuards=%d", s_NullGuards);
    s_TransLog.close();
    s_Initialized = false;
}

// ─── Deferred PE Hook Registration ───────────────────────────────────────

static bool s_HooksActive = false;

void ActivateTransitionHooks()
{
    if (s_HooksActive) return;
    if (!IsProcessEventHooked()) return;
    s_HooksActive = true;

    // Level change detection
    ProcessEventHook levelHook;
    levelHook.Name = "TransitionLevel";
    levelHook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        if (!obj || !func) return false;
        try {
            std::string fn = func->GetName();
            if (fn == "ServerTravel" || fn == "ClientTravel" ||
                fn == "LoadMap" || fn == "SwitchLevel") {
                std::string cn = obj->GetObjClassName();
                TransLog("LEVEL CHANGE detected: %s.%s", cn.c_str(), fn.c_str());
                DebugSessionLogf("Level change: %s.%s", cn.c_str(), fn.c_str());
            }
        } catch (...) { RegisterNullGuard("LevelHook PE crash"); }
        return false;
    };
    s_LevelHookId = RegisterProcessEventHook(levelHook);

    // Cutscene detection
    ProcessEventHook cutsceneHook;
    cutsceneHook.Name = "TransitionCutscene";
    cutsceneHook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        if (!obj || !func) return false;
        try {
            std::string fn = func->GetName();
            if (fn.find("Cutscene") != std::string::npos ||
                fn.find("Cinematic") != std::string::npos ||
                fn.find("Matinee") != std::string::npos) {
                if (!s_Cutscene.Active) {
                    std::string cn = obj->GetObjClassName();
                    TransLog("CUTSCENE START: %s.%s", cn.c_str(), fn.c_str());
                    OnCutsceneStart(fn);
                }
            }
        } catch (...) { RegisterNullGuard("CutsceneHook PE crash"); }
        return false;
    };
    s_CutsceneHookId = RegisterProcessEventHook(cutsceneHook);

    // Big Daddy damage hook
    ProcessEventHook bdHook;
    bdHook.Name = "TransitionBigDaddy";
    bdHook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        if (!obj || !func) return false;
        try {
            std::string fn = func->GetName();
            if (fn == "TakeDamage" || fn == "Died") {
                std::string cn = obj->GetObjClassName();
                bool isBD = cn.find("BigDaddy") != std::string::npos ||
                            cn.find("Rosie") != std::string::npos ||
                            cn.find("Bouncer") != std::string::npos;
                if (isBD && fn == "TakeDamage") {
                    TransLog("BIG DADDY DAMAGE: %s.%s", cn.c_str(), fn.c_str());
                    OnBigDaddyDamage(obj, 0, false);
                }
                if (isBD && fn == "Died") {
                    TransLog("BIG DADDY DIED: %s", cn.c_str());
                    OnBigDaddyDeath(obj);
                }
            }
        } catch (...) { RegisterNullGuard("BigDaddyHook PE crash"); }
        return false;
    };
    s_BigDaddyHookId = RegisterProcessEventHook(bdHook);

    TransLog("PE hooks activated for co-op session");
    LOG_INFO("[Transitions] PE hooks now active");
}

// ─── Tick ────────────────────────────────────────────────────────────────

void TransitionsTick(float deltaTime)
{
    if (!s_Initialized) return;
    s_Time += deltaTime;

    // Periodic level check (every 3 seconds)
    static float levelCheckAccum = 0;
    levelCheckAccum += deltaTime;
    if (levelCheckAccum >= 3.0f) {
        levelCheckAccum = 0;
        if (IsWorldSystemReady()) {
            LevelInfo lvl = GetCurrentLevel();
            if (!lvl.LevelName.empty() && lvl.LevelName != s_Transition.CurrentLevel) {
                TransLog("LEVEL CHANGED: %s -> %s",
                        s_Transition.CurrentLevel.c_str(), lvl.LevelName.c_str());
                OnHostLevelChange(lvl.LevelName);
            }
        }
    }

    // Track Big Daddies
    static float bdTrackAccum = 0;
    bdTrackAccum += deltaTime;
    if (bdTrackAccum >= 2.0f) {
        bdTrackAccum = 0;
        TrackBigDaddies();
    }

    // Cutscene timeout (auto-end after 120s if not manually ended)
    if (s_Cutscene.Active) {
        s_Cutscene.Duration = s_Time - s_Cutscene.StartTime;
        if (s_Cutscene.Duration > 120.0f) {
            TransLog("CUTSCENE AUTO-END (timeout 120s)");
            OnCutsceneEnd();
        }
    }
}

// ─── Level Transitions ───────────────────────────────────────────────────

void OnHostLevelChange(const std::string& newLevel)
{
    std::string oldLevel = s_Transition.CurrentLevel;
    s_Transition.CurrentLevel = newLevel;
    s_Transition.TargetLevel = newLevel;
    s_Transition.State = TransitionState::HostLoading;
    s_Transition.TransitionTime = s_Time;
    s_Transition.P2NeedsRespawn = true;

    TransLog("HOST LEVEL CHANGE: %s -> %s", oldLevel.c_str(), newLevel.c_str());
    DebugSessionLogf("Level transition: %s -> %s", oldLevel.c_str(), newLevel.c_str());

    // Notify client via network
    if (IsNetConnected()) {
        LevelSyncData lvl{};
        strncpy(lvl.levelName, newLevel.c_str(), sizeof(lvl.levelName) - 1);
        lvl.isLoading = 1;
        NetSendRawPacket(PacketType::LevelSync, &lvl, sizeof(lvl));
    }

    // Destroy P2 pawn (will respawn after load)
    if (P2IsSpawned()) {
        P2DestroyPawn();
    }

    // Clear Big Daddy tracking
    s_BigDaddies.clear();
}

void OnClientLevelLoaded(const std::string& level)
{
    s_Transition.CurrentLevel = level;
    s_Transition.State = TransitionState::Syncing;
    TransLog("CLIENT level loaded: %s", level.c_str());

    // Notify host we're ready
    if (IsNetConnected()) {
        LevelSyncData lvl{};
        strncpy(lvl.levelName, level.c_str(), sizeof(lvl.levelName) - 1);
        lvl.isLoading = 0; // finished
        NetSendRawPacket(PacketType::LevelSync, &lvl, sizeof(lvl));
    }

    s_Transition.State = TransitionState::Normal;
}

bool IsInTransition()
{
    return s_Transition.State == TransitionState::HostLoading ||
           s_Transition.State == TransitionState::ClientLoading ||
           s_Transition.State == TransitionState::Syncing;
}

TransitionInfo GetTransitionInfo() { return s_Transition; }

// ─── Cutscenes ───────────────────────────────────────────────────────────

void OnCutsceneStart(const std::string& name)
{
    s_Cutscene.Active = true;
    s_Cutscene.CutsceneName = name;
    s_Cutscene.StartTime = s_Time;
    s_Cutscene.P2InputPaused = true;
    TransLog("CUTSCENE START: %s (P2 input paused)", name.c_str());
}

void OnCutsceneEnd()
{
    TransLog("CUTSCENE END: %s (duration=%.1fs)",
            s_Cutscene.CutsceneName.c_str(), s_Cutscene.Duration);
    s_Cutscene.Active = false;
    s_Cutscene.P2InputPaused = false;
}

bool IsInCutscene() { return s_Cutscene.Active; }
CutsceneState GetCutsceneState() { return s_Cutscene; }

// ─── Big Daddy Tracking ──────────────────────────────────────────────────

static uint32_t QuickHash(const std::string& s)
{
    uint32_t h = 0x811c9dc5;
    for (char c : s) { h ^= (uint8_t)c; h *= 0x01000193; }
    return h;
}

void TrackBigDaddies()
{
    if (!IsWorldSystemReady()) return;

    auto actors = GetAllActors();
    s_BigDaddies.clear();

    for (auto* actor : actors) {
        if (!actor) continue;
        std::string cn = actor->GetObjClassName();
        if (cn.find("BigDaddy") == std::string::npos &&
            cn.find("Rosie") == std::string::npos &&
            cn.find("Bouncer") == std::string::npos) continue;

        BigDaddyState bd;
        bd.Ptr = actor;
        bd.NameHash = QuickHash(actor->GetName());
        bd.Health = 100; // TODO: read actual health
        bd.MaxHealth = 100;
        s_BigDaddies.push_back(bd);
    }
}

std::vector<BigDaddyState> GetBigDaddyStates() { return s_BigDaddies; }

void OnBigDaddyDamage(UObject* bigDaddy, float damage, bool fromP2)
{
    for (auto& bd : s_BigDaddies) {
        if (bd.Ptr == bigDaddy) {
            if (fromP2) bd.P2DamageDealt += damage;
            else bd.P1DamageDealt += damage;
            bd.Health -= damage;
            TransLog("BD DAMAGE: hash=0x%08X dmg=%.0f from=%s hp=%.0f",
                    bd.NameHash, damage, fromP2 ? "P2" : "P1", bd.Health);
            break;
        }
    }
}

void OnBigDaddyDeath(UObject* bigDaddy)
{
    for (auto it = s_BigDaddies.begin(); it != s_BigDaddies.end(); ++it) {
        if (it->Ptr == bigDaddy) {
            TransLog("BD DIED: hash=0x%08X P1dmg=%.0f P2dmg=%.0f",
                    it->NameHash, it->P1DamageDealt, it->P2DamageDealt);
            DebugSessionLogf("Big Daddy killed! P1=%.0f P2=%.0f damage",
                            it->P1DamageDealt, it->P2DamageDealt);
            s_BigDaddies.erase(it);
            break;
        }
    }
}

void OnLittleSisterRescue(bool harvested, int adamAmount)
{
    TransLog("LITTLE SISTER %s: +%d ADAM",
            harvested ? "HARVESTED" : "RESCUED", adamAmount);
    DebugSessionLogf("Little Sister %s: +%d ADAM shared between players",
                    harvested ? "harvested" : "rescued", adamAmount);
    // ADAM is shared via economy sync (already implemented)
}

// ─── Stability ───────────────────────────────────────────────────────────

void RegisterNullGuard(const std::string& context)
{
    s_NullGuards++;
    s_NullGuardContexts.push_back(context);
    if (s_NullGuards <= 10) {  // only log first 10
        TransLog("NULL GUARD: %s (total=%d)", context.c_str(), s_NullGuards);
    }
}

int GetNullGuardCount() { return s_NullGuards; }

void DumpStabilityReport()
{
    std::string filepath = std::string(GetDebugDir()) + "/stability_report.txt";
    std::ofstream out(filepath);
    out << "=== Stability Report ===\n";
    out << "Null guards triggered: " << s_NullGuards << "\n";
    out << "Big Daddies tracked: " << s_BigDaddies.size() << "\n";
    out << "Cutscenes detected: " << (s_Cutscene.Active ? "IN CUTSCENE" : "none active") << "\n";
    out << "Level transitions: " << s_Transition.CurrentLevel << "\n\n";

    if (!s_NullGuardContexts.empty()) {
        out << "─── Null Guard Contexts ───\n";
        for (auto& ctx : s_NullGuardContexts) {
            out << "  " << ctx << "\n";
        }
    }

    out.close();
    LOG_INFO("[Transitions] Stability report -> {}", filepath);
}

// ─── Status ──────────────────────────────────────────────────────────────

std::string GetTransitionsStatus()
{
    const char* stateNames[] = {"Normal", "HostLoading", "ClientLoading", "Syncing", "Cutscene"};
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "State: %s\n"
        "Level: %s\n"
        "Cutscene: %s\n"
        "BigDaddies: %d alive\n"
        "NullGuards: %d",
        stateNames[(int)s_Transition.State],
        s_Transition.CurrentLevel.c_str(),
        s_Cutscene.Active ? s_Cutscene.CutsceneName.c_str() : "none",
        (int)s_BigDaddies.size(),
        s_NullGuards);
    return buf;
}

} // namespace bs1sdk
