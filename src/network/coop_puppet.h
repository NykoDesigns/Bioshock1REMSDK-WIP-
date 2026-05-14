#pragma once

#include "net_common.h"

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

} // namespace bs1sdk
