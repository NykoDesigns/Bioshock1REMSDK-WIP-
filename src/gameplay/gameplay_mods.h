#pragma once

#include <string>

namespace bs1sdk {

// ═══════════════════════════════════════════════════════════════════════
// Gameplay Mods — All plasmid/weapon/AI hijacks
// ═══════════════════════════════════════════════════════════════════════

/// Initialize ALL gameplay mod hooks at once.
bool InitGameplayMods();

/// Shutdown all gameplay mods, remove hooks.
void ShutdownGameplayMods();

/// Get combined status string.
std::string GetGameplayModsStatus();

// ─── Goal 2: Decoy → Teleport ───────────────────────────────────────
bool IsDecoyTeleportEnabled();
void SetDecoyTeleportEnabled(bool enabled);

// ─── Goal 3: Security Command → Friendly Bots ──────────────────────
bool IsFriendlyBotsEnabled();
void SetFriendlyBotsEnabled(bool enabled);
int  GetFriendlyBotCount();
void SetFriendlyBotLimit(int limit);

// ─── Goal 5: Revolver → Rivets ─────────────────────────────────────
bool IsRivetPistolEnabled();
void SetRivetPistolEnabled(bool enabled);

// ─── Goal 6: Splicer Factions ──────────────────────────────────────
bool IsSplicerFactionsEnabled();
void SetSplicerFactionsEnabled(bool enabled);
void TagSplicerFaction(int factionId);  // Tags nearest splicer

// ─── Goal 7: Chain Lightning ───────────────────────────────────────
bool IsChainLightningEnabled();
void SetChainLightningEnabled(bool enabled);
void SetChainLightningRadius(float radius);
void SetChainLightningJumps(int maxJumps);
void SetChainLightningDamageFalloff(float falloff);

} // namespace bs1sdk
