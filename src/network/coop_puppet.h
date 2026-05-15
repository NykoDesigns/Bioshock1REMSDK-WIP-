#pragma once

#include "net_common.h"
#include <string>

namespace bs1sdk {

/// Initialize the ghost puppet system. Call once after ProcessEvent hook is up.
bool InitGhostPuppet();

/// Shutdown and destroy any active puppet.
void ShutdownGhostPuppet();

/// Spawn or ensure a puppet exists for the remote player.
/// Called when a peer connects.
bool SpawnGhostPuppet(float x, float y, float z);

/// Update the puppet's world position/rotation from remote state.
/// Call this every frame while connected.
void UpdateGhostPuppet(const PlayerStateData& remoteState);

/// Destroy the puppet actor. Called on disconnect.
void DestroyGhostPuppet();

/// Returns true if a puppet is currently alive in the world.
bool HasGhostPuppet();

/// Notify the puppet that the remote player performed an action.
/// Used to display visual effects (melee swing, muzzle flash, etc.)
struct PlayerActionData;
void NotifyPuppetAction(const PlayerActionData& action);

/// Get the puppet's last known action for rendering effects.
/// Returns the action type or -1 if no recent action.
int GetPuppetLastAction(float& timeSinceAction);

/// Get detailed puppet diagnostics string for debug console.
std::string GetPuppetDiagnostics();

} // namespace bs1sdk
