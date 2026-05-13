#pragma once

#include <string>

namespace bs1sdk {

/// Mod configuration — loaded from mod_config.json next to the DLL.
/// Controls which gameplay mods are active and their settings.

struct ModConfig {
    // Goal 2: Decoy → Teleport
    bool decoyTeleport = true;

    // Goal 3: Friendly Bots
    bool friendlyBots = true;
    int  friendlyBotLimit = 3;

    // Goal 5: Rivet Pistol
    bool rivetPistol = false;

    // Goal 6: Splicer Factions
    bool splicerFactions = false;

    // Goal 7: Chain Lightning
    bool chainLightning = true;
    float chainRadius = 500.0f;
    int   chainMaxJumps = 3;
    float chainDamageFalloff = 0.5f;

    // General
    bool autoInitMods = true;   // Init gameplay mods on startup
    bool showOverlay = true;    // Show ImGui overlay (F1)
};

/// Load config from JSON file. Returns defaults if file not found.
ModConfig LoadModConfig();

/// Save current config to JSON file.
void SaveModConfig(const ModConfig& cfg);

/// Get the directory where the DLL lives (for finding config files).
std::string GetModDirectory();

} // namespace bs1sdk
