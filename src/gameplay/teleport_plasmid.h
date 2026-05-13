#pragma once

#include <string>

namespace bs1sdk {

// ─── Teleport Plasmid (hijacks Security Bullseye) ───────────────────────

/// Initialize the plasmid hijack system.
/// Hooks ProcessEvent to intercept Security Bullseye and Hypnotize Big Daddy.
bool InitPlasmidHijacks();

/// Execute a teleport (can be called directly from console/Lua).
/// Moves the player forward by 'distance' units in their look direction.
bool DoTeleport(float distance = 800.0f);

/// Teleport to a specific world position.
bool DoTeleportTo(float x, float y, float z);

/// Set/get teleport distance (for forward-blink fallback).
void SetTeleportDistance(float dist);
float GetTeleportDistance();

/// Get status string for the plasmid hijack system.
std::string GetPlasmidHijackStatus();

// ─── Legacy aliases ─────────────────────────────────────────────────────
inline bool InitTeleportPlasmid() { return InitPlasmidHijacks(); }
inline bool GivePlayerTeleportPlasmid() { return InitPlasmidHijacks(); }
inline std::string GetTeleportStatus() { return GetPlasmidHijackStatus(); }

// ─── Gameplay Mods (Goals 2-7) ──────────────────────────────────────────
// See gameplay_mods.h for the full API. This header just provides the
// combined init for use from dllmain/console.

} // namespace bs1sdk
