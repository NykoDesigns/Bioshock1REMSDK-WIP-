#pragma once

#include "../engine/uobject.h"
#include <string>
#include <vector>
#include <cstdint>

namespace bs1sdk {

// ─── Phase 6: Level Transitions, Cutscenes, Big Daddy, Stability ─────────

// ─── Level Transition State ──────────────────────────────────────────────

enum class TransitionState : uint8_t {
    Normal = 0,         // Playing normally
    HostLoading = 1,    // Host is loading a new level
    ClientLoading = 2,  // Client is following to new level
    Syncing = 3,        // Both loaded, syncing state
    Cutscene = 4,       // In a cutscene
};

struct TransitionInfo {
    TransitionState State = TransitionState::Normal;
    std::string CurrentLevel;
    std::string TargetLevel;
    float TransitionTime = 0;
    bool P2NeedsRespawn = false;
};

// ─── Cutscene Detection ──────────────────────────────────────────────────

struct CutsceneState {
    bool Active = false;
    std::string CutsceneName;
    float StartTime = 0;
    float Duration = 0;
    bool P2InputPaused = false;
};

// ─── Big Daddy Encounter ─────────────────────────────────────────────────

struct BigDaddyState {
    UObject* Ptr = nullptr;
    uint32_t NameHash = 0;
    float Health = 0;
    float MaxHealth = 0;
    bool AggroOnP1 = false;
    bool AggroOnP2 = false;
    float P1DamageDealt = 0;
    float P2DamageDealt = 0;
};

// ─── Public API ──────────────────────────────────────────────────────────

/// Initialize transitions system
void InitTransitions();
void ShutdownTransitions();

/// Called every tick
void TransitionsTick(float deltaTime);

/// Level transition handling
void OnHostLevelChange(const std::string& newLevel);
void OnClientLevelLoaded(const std::string& level);
bool IsInTransition();
TransitionInfo GetTransitionInfo();

/// Cutscene handling
void OnCutsceneStart(const std::string& name);
void OnCutsceneEnd();
bool IsInCutscene();
CutsceneState GetCutsceneState();

/// Big Daddy tracking
void TrackBigDaddies();
std::vector<BigDaddyState> GetBigDaddyStates();
void OnBigDaddyDamage(UObject* bigDaddy, float damage, bool fromP2);
void OnBigDaddyDeath(UObject* bigDaddy);
void OnLittleSisterRescue(bool harvested, int adamAmount);

/// Stability
void RegisterNullGuard(const std::string& context);
int GetNullGuardCount();
void DumpStabilityReport();

/// Status string
std::string GetTransitionsStatus();

} // namespace bs1sdk
