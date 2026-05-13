#pragma once

#include <string>

namespace bs1sdk {

/// Initialize the custom Teleport plasmid.
/// - Clones an existing plasmid class
/// - Registers ProcessEvent hook for activation
/// - Implements aim-based teleportation
/// Returns true on success.
bool InitTeleportPlasmid();

/// Give the teleport plasmid to the player.
/// This attempts to call AddAvailablePlasmid on the player's PlasmidManager.
bool GivePlayerTeleportPlasmid();

/// Execute a teleport (can be called directly from console/Lua).
/// Moves the player forward by 'distance' units in their look direction.
bool DoTeleport(float distance = 1000.0f);

/// Get status string for the teleport plasmid system.
std::string GetTeleportStatus();

} // namespace bs1sdk
