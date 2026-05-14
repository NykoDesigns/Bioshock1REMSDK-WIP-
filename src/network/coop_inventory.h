#pragma once

#include "../engine/uobject.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace bs1sdk {

// ─── P2 Inventory System ─────────────────────────────────────────────────
// Phase 5: Shadow inventory for Player 2 on the host.
// The host tracks P2's weapons, ammo, plasmids, and tonics separately.
// Synced to client for HUD rendering.

// ─── Weapon Slot ─────────────────────────────────────────────────────────

struct P2WeaponSlot {
    uint8_t SlotId = 0;
    std::string ClassName;          // e.g. "Pistol", "Shotgun"
    std::string DisplayName;
    bool Owned = false;
    int AmmoCount = 0;
    int MaxAmmo = 0;
    int MagazineSize = 0;
    int CurrentMagazine = 0;
};

// ─── Plasmid Slot ────────────────────────────────────────────────────────

struct P2PlasmidSlot {
    uint8_t SlotId = 0;
    std::string ClassName;
    std::string DisplayName;
    bool Owned = false;
    int Level = 1;                  // upgrade level
};

// ─── Inventory State ─────────────────────────────────────────────────────

struct P2InventoryState {
    P2WeaponSlot Weapons[10];       // weapon slots 0-9
    P2PlasmidSlot Plasmids[8];      // plasmid slots 0-7
    int ActiveWeapon = 0;
    int ActivePlasmid = 0;
    int ADAM = 0;
    int Credits = 0;
    int MaxCredits = 500;
    float Health = 100.0f;
    float MaxHealth = 100.0f;
    float EVE = 100.0f;
    float MaxEVE = 100.0f;
    int MedkitCount = 0;
    int EVEHypoCount = 0;
};

// ─── Pickup Event ────────────────────────────────────────────────────────

enum class PickupType : uint8_t {
    Ammo = 0,
    Weapon = 1,
    Health = 2,
    EVE = 3,
    ADAM = 4,
    Credits = 5,
    Audio = 6,
    Key = 7,
    Tonic = 8,
    Plasmid = 9,
};

struct P2PickupEvent {
    PickupType Type;
    uint8_t SlotId;         // weapon/plasmid slot
    uint8_t _pad[2];
    int Amount;             // ammo count, credit amount, etc.
    uint32_t ActorHash;     // pickup actor that was consumed
    char ItemName[32];      // display name
};

// ─── Vending Transaction ─────────────────────────────────────────────────

struct VendingTransaction {
    uint32_t VendingHash;   // vending machine actor hash
    char ItemName[32];
    int Cost;
    PickupType ItemType;
    uint8_t SlotId;
    uint8_t _pad[2];
};

// ─── Public API ──────────────────────────────────────────────────────────

/// Initialize P2 inventory
void InitP2Inventory();
void ShutdownP2Inventory();

/// Get/set P2 inventory state
P2InventoryState& GetP2Inventory();

/// P2 picks up an item
bool P2PickupItem(const P2PickupEvent& pickup);

/// P2 buys from vending machine
bool P2VendingBuy(const VendingTransaction& transaction);

/// P2 switches weapon
void P2SwitchWeapon(int slot);
void P2SwitchPlasmid(int slot);

/// P2 uses consumable
bool P2UseMedkit();
bool P2UseEVEHypo();

/// P2 fires weapon (consumes ammo)
bool P2ConsumeAmmo();

/// P2 uses plasmid (consumes EVE)
bool P2ConsumeEVE(float amount);

/// Give P2 starting equipment
void P2GiveStartingLoadout();

/// Serialize inventory for network sync
void P2SerializeInventory(uint8_t* buffer, int& size);
void P2DeserializeInventory(const uint8_t* buffer, int size);

/// Dump inventory state
void DumpP2Inventory();

/// Get status string
std::string GetP2InventoryStatus();

} // namespace bs1sdk
