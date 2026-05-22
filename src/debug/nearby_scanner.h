#pragma once

#include <string>

namespace bs1sdk {

/// Configurable radius for nearby actor scanning
void SetScanRadius(float radius);
float GetScanRadius();

/// Execute a nearby actor scan around the player and export to JSON.
/// Output file: BS1SDK_dumps/runtime_nearby_actors.json
/// Returns the number of actors exported, or -1 on failure.
int ExecuteNearbyScan();

/// Execute a FULL comprehensive scan of all GObjects.
/// Dumps all actors (with full property trees), all StaticMesh objects,
/// all Texture objects, all Material objects to a large JSON.
/// Output file: BS1SDK_dumps/runtime_full_scan.json
/// This is what the level editor needs to resolve everything.
int ExecuteFullScan();

/// Console command handler for "scan" / "scanfull" / "scanradius"
void HandleScanCommand(const std::string& args);

} // namespace bs1sdk
