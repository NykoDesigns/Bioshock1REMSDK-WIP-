#pragma once

#include <cstdint>

namespace bs1sdk {

// ─── Runtime Engine Patching ─────────────────────────────────────────────
// Patches the running BioshockHD.exe in-memory to fix known engine bugs.
// Uses trampoline-based hooks: replaces crash-site instructions with JMPs
// to code caves that add null checks, then continue the original flow.
//
// This is the same technique used by major modding frameworks (SKSE, SFSE,
// F4SE, etc.) — rewrite small sections of the game EXE at runtime.

/// Install all known engine patches. Call during SDK init, after crash
/// handler is installed so any failure is logged.
void InstallEnginePatches();

/// Remove all installed patches (restore original bytes).
void RemoveEnginePatches();

/// Patch status for crash report inclusion
struct EnginePatchInfo {
    const char* name;        // Human-readable name
    const char* description; // What it fixes
    uintptr_t   address;     // Patched address in game EXE
    bool        installed;   // True if successfully installed
    bool        triggered;   // True if the null-check path was hit at least once
};

/// Get list of installed patches for diagnostics / crash reports.
/// Returns the number of patches written to `out`.
int GetEnginePatches(EnginePatchInfo* out, int maxCount);

} // namespace bs1sdk
