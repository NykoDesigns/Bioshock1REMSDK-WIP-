#include "coop_inventory.h"
#include "coop_true.h"
#include "net_manager.h"
#include "../core/log.h"
#include "../debug/coop_debug.h"

#include <cstring>
#include <cstdarg>
#include <fstream>

namespace bs1sdk {

// ─── State ───────────────────────────────────────────────────────────────

static P2InventoryState s_Inventory;
static bool s_Initialized = false;
static std::ofstream s_InvLog;

static void InvLog(const char* fmt, ...)
{
    if (!s_InvLog.is_open()) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_InvLog << buf << "\n";
    s_InvLog.flush();
}

// ─── Init / Shutdown ─────────────────────────────────────────────────────

void InitP2Inventory()
{
    if (s_Initialized) return;
    s_Initialized = true;

    std::string logPath = std::string(DEBUG_DIR) + "/p2_inventory_log.txt";
    s_InvLog.open(logPath, std::ios::trunc);
    InvLog("P2 Inventory initialized");

    memset(&s_Inventory, 0, sizeof(s_Inventory));
    P2GiveStartingLoadout();

    LOG_INFO("[P2Inv] Initialized with starting loadout");
}

void ShutdownP2Inventory()
{
    if (!s_Initialized) return;
    InvLog("P2 Inventory shutdown");
    s_InvLog.close();
    s_Initialized = false;
}

P2InventoryState& GetP2Inventory() { return s_Inventory; }

// ─── Starting Loadout ────────────────────────────────────────────────────

void P2GiveStartingLoadout()
{
    // Wrench (slot 0)
    s_Inventory.Weapons[0].SlotId = 0;
    s_Inventory.Weapons[0].ClassName = "Wrench";
    s_Inventory.Weapons[0].DisplayName = "Wrench";
    s_Inventory.Weapons[0].Owned = true;
    s_Inventory.Weapons[0].AmmoCount = -1; // infinite
    s_Inventory.Weapons[0].MaxAmmo = -1;
    s_Inventory.Weapons[0].MagazineSize = -1;
    s_Inventory.Weapons[0].CurrentMagazine = -1;

    // Pistol (slot 1)
    s_Inventory.Weapons[1].SlotId = 1;
    s_Inventory.Weapons[1].ClassName = "Pistol";
    s_Inventory.Weapons[1].DisplayName = "Pistol";
    s_Inventory.Weapons[1].Owned = true;
    s_Inventory.Weapons[1].AmmoCount = 48;
    s_Inventory.Weapons[1].MaxAmmo = 48;
    s_Inventory.Weapons[1].MagazineSize = 6;
    s_Inventory.Weapons[1].CurrentMagazine = 6;

    // Electro Bolt (plasmid 0)
    s_Inventory.Plasmids[0].SlotId = 0;
    s_Inventory.Plasmids[0].ClassName = "ElectricBoltAbility";
    s_Inventory.Plasmids[0].DisplayName = "Electro Bolt";
    s_Inventory.Plasmids[0].Owned = true;
    s_Inventory.Plasmids[0].Level = 1;

    s_Inventory.ActiveWeapon = 1; // pistol
    s_Inventory.ActivePlasmid = 0;
    s_Inventory.Health = 100.0f;
    s_Inventory.MaxHealth = 100.0f;
    s_Inventory.EVE = 100.0f;
    s_Inventory.MaxEVE = 100.0f;
    s_Inventory.Credits = 100;
    s_Inventory.MedkitCount = 3;
    s_Inventory.EVEHypoCount = 3;

    InvLog("Starting loadout: Wrench, Pistol (48), Electro Bolt, 100HP, 100EVE, $100");
}

// ─── Pickup ──────────────────────────────────────────────────────────────

bool P2PickupItem(const P2PickupEvent& pickup)
{
    switch (pickup.Type) {
    case PickupType::Ammo: {
        int slot = pickup.SlotId;
        if (slot < 0 || slot >= 10) return false;
        if (!s_Inventory.Weapons[slot].Owned) return false;
        int maxAdd = s_Inventory.Weapons[slot].MaxAmmo - s_Inventory.Weapons[slot].AmmoCount;
        int added = std::min(pickup.Amount, maxAdd);
        s_Inventory.Weapons[slot].AmmoCount += added;
        InvLog("PICKUP: +%d ammo for %s (now %d/%d)",
               added, s_Inventory.Weapons[slot].DisplayName.c_str(),
               s_Inventory.Weapons[slot].AmmoCount, s_Inventory.Weapons[slot].MaxAmmo);
        return true;
    }
    case PickupType::Weapon: {
        int slot = pickup.SlotId;
        if (slot < 0 || slot >= 10) return false;
        s_Inventory.Weapons[slot].Owned = true;
        s_Inventory.Weapons[slot].ClassName = pickup.ItemName;
        s_Inventory.Weapons[slot].DisplayName = pickup.ItemName;
        s_Inventory.Weapons[slot].AmmoCount = pickup.Amount;
        InvLog("PICKUP: Weapon '%s' in slot %d", pickup.ItemName, slot);
        return true;
    }
    case PickupType::Health: {
        float add = std::min((float)pickup.Amount, s_Inventory.MaxHealth - s_Inventory.Health);
        s_Inventory.Health += add;
        InvLog("PICKUP: +%.0f health (now %.0f)", add, s_Inventory.Health);
        return true;
    }
    case PickupType::EVE: {
        float add = std::min((float)pickup.Amount, s_Inventory.MaxEVE - s_Inventory.EVE);
        s_Inventory.EVE += add;
        InvLog("PICKUP: +%.0f EVE (now %.0f)", add, s_Inventory.EVE);
        return true;
    }
    case PickupType::ADAM:
        s_Inventory.ADAM += pickup.Amount;
        InvLog("PICKUP: +%d ADAM (now %d)", pickup.Amount, s_Inventory.ADAM);
        return true;
    case PickupType::Credits:
        s_Inventory.Credits = std::min(s_Inventory.Credits + pickup.Amount, s_Inventory.MaxCredits);
        InvLog("PICKUP: +%d credits (now %d)", pickup.Amount, s_Inventory.Credits);
        return true;
    case PickupType::Plasmid: {
        int slot = pickup.SlotId;
        if (slot < 0 || slot >= 8) return false;
        s_Inventory.Plasmids[slot].Owned = true;
        s_Inventory.Plasmids[slot].ClassName = pickup.ItemName;
        s_Inventory.Plasmids[slot].DisplayName = pickup.ItemName;
        InvLog("PICKUP: Plasmid '%s' in slot %d", pickup.ItemName, slot);
        return true;
    }
    default:
        return false;
    }
}

// ─── Vending ─────────────────────────────────────────────────────────────

bool P2VendingBuy(const VendingTransaction& transaction)
{
    if (s_Inventory.Credits < transaction.Cost) {
        InvLog("VENDING: Cannot afford '%s' ($%d, have $%d)",
               transaction.ItemName, transaction.Cost, s_Inventory.Credits);
        return false;
    }

    s_Inventory.Credits -= transaction.Cost;

    P2PickupEvent pickup{};
    pickup.Type = transaction.ItemType;
    pickup.SlotId = transaction.SlotId;
    pickup.Amount = 1;
    strncpy(pickup.ItemName, transaction.ItemName, sizeof(pickup.ItemName) - 1);

    InvLog("VENDING: Bought '%s' for $%d (credits now $%d)",
           transaction.ItemName, transaction.Cost, s_Inventory.Credits);

    return P2PickupItem(pickup);
}

// ─── Weapon/Plasmid Switch ───────────────────────────────────────────────

void P2SwitchWeapon(int slot)
{
    if (slot < 0 || slot >= 10) return;
    if (!s_Inventory.Weapons[slot].Owned) return;
    s_Inventory.ActiveWeapon = slot;
    InvLog("SWITCH: Weapon -> %s (slot %d)",
           s_Inventory.Weapons[slot].DisplayName.c_str(), slot);
}

void P2SwitchPlasmid(int slot)
{
    if (slot < 0 || slot >= 8) return;
    if (!s_Inventory.Plasmids[slot].Owned) return;
    s_Inventory.ActivePlasmid = slot;
    InvLog("SWITCH: Plasmid -> %s (slot %d)",
           s_Inventory.Plasmids[slot].DisplayName.c_str(), slot);
}

// ─── Consumables ─────────────────────────────────────────────────────────

bool P2UseMedkit()
{
    if (s_Inventory.MedkitCount <= 0) return false;
    if (s_Inventory.Health >= s_Inventory.MaxHealth) return false;
    s_Inventory.MedkitCount--;
    float heal = std::min(50.0f, s_Inventory.MaxHealth - s_Inventory.Health);
    s_Inventory.Health += heal;
    InvLog("USE: Medkit (+%.0f HP, now %.0f, %d left)",
           heal, s_Inventory.Health, s_Inventory.MedkitCount);
    return true;
}

bool P2UseEVEHypo()
{
    if (s_Inventory.EVEHypoCount <= 0) return false;
    if (s_Inventory.EVE >= s_Inventory.MaxEVE) return false;
    s_Inventory.EVEHypoCount--;
    float restore = std::min(50.0f, s_Inventory.MaxEVE - s_Inventory.EVE);
    s_Inventory.EVE += restore;
    InvLog("USE: EVE Hypo (+%.0f EVE, now %.0f, %d left)",
           restore, s_Inventory.EVE, s_Inventory.EVEHypoCount);
    return true;
}

bool P2ConsumeAmmo()
{
    int slot = s_Inventory.ActiveWeapon;
    if (slot < 0 || slot >= 10) return false;
    auto& w = s_Inventory.Weapons[slot];
    if (!w.Owned) return false;
    if (w.AmmoCount == -1) return true; // infinite (wrench)
    if (w.CurrentMagazine <= 0) return false; // need reload
    w.CurrentMagazine--;
    if (w.CurrentMagazine <= 0 && w.AmmoCount > 0) {
        // Auto-reload
        int reload = std::min(w.MagazineSize, w.AmmoCount);
        w.CurrentMagazine = reload;
        w.AmmoCount -= reload;
    }
    return true;
}

bool P2ConsumeEVE(float amount)
{
    if (s_Inventory.EVE < amount) return false;
    s_Inventory.EVE -= amount;
    return true;
}

// ─── Serialization ───────────────────────────────────────────────────────

void P2SerializeInventory(uint8_t* buffer, int& size)
{
    // Simple flat copy for now
    size = sizeof(P2InventoryState);
    memcpy(buffer, &s_Inventory, size);
}

void P2DeserializeInventory(const uint8_t* buffer, int size)
{
    if (size >= (int)sizeof(P2InventoryState)) {
        memcpy(&s_Inventory, buffer, sizeof(P2InventoryState));
    }
}

// ─── Debug ───────────────────────────────────────────────────────────────

void DumpP2Inventory()
{
    std::string filepath = std::string(DEBUG_DIR) + "/p2_inventory.txt";
    std::ofstream out(filepath);
    out << "=== P2 Inventory ===\n";
    out << "Health: " << s_Inventory.Health << "/" << s_Inventory.MaxHealth << "\n";
    out << "EVE: " << s_Inventory.EVE << "/" << s_Inventory.MaxEVE << "\n";
    out << "ADAM: " << s_Inventory.ADAM << "\n";
    out << "Credits: " << s_Inventory.Credits << "/" << s_Inventory.MaxCredits << "\n";
    out << "Medkits: " << s_Inventory.MedkitCount << "\n";
    out << "EVE Hypos: " << s_Inventory.EVEHypoCount << "\n\n";

    out << "─── Weapons ───\n";
    for (int i = 0; i < 10; i++) {
        auto& w = s_Inventory.Weapons[i];
        if (!w.Owned) continue;
        char line[128];
        if (w.AmmoCount == -1) {
            std::snprintf(line, sizeof(line), "  [%d] %s (infinite)%s\n",
                         i, w.DisplayName.c_str(),
                         i == s_Inventory.ActiveWeapon ? " <ACTIVE>" : "");
        } else {
            std::snprintf(line, sizeof(line), "  [%d] %s (%d/%d mag, %d reserve)%s\n",
                         i, w.DisplayName.c_str(),
                         w.CurrentMagazine, w.MagazineSize, w.AmmoCount,
                         i == s_Inventory.ActiveWeapon ? " <ACTIVE>" : "");
        }
        out << line;
    }

    out << "\n─── Plasmids ───\n";
    for (int i = 0; i < 8; i++) {
        auto& p = s_Inventory.Plasmids[i];
        if (!p.Owned) continue;
        char line[128];
        std::snprintf(line, sizeof(line), "  [%d] %s (Lv%d)%s\n",
                     i, p.DisplayName.c_str(), p.Level,
                     i == s_Inventory.ActivePlasmid ? " <ACTIVE>" : "");
        out << line;
    }

    out.close();
    LOG_INFO("[P2Inv] Inventory dumped -> {}", filepath);
}

std::string GetP2InventoryStatus()
{
    char buf[512];
    auto& w = s_Inventory.Weapons[s_Inventory.ActiveWeapon];
    std::snprintf(buf, sizeof(buf),
        "HP: %.0f/%.0f  EVE: %.0f/%.0f  $%d  ADAM: %d\n"
        "Weapon: %s  Medkits: %d  Hypos: %d",
        s_Inventory.Health, s_Inventory.MaxHealth,
        s_Inventory.EVE, s_Inventory.MaxEVE,
        s_Inventory.Credits, s_Inventory.ADAM,
        w.Owned ? w.DisplayName.c_str() : "None",
        s_Inventory.MedkitCount, s_Inventory.EVEHypoCount);
    return buf;
}

} // namespace bs1sdk
