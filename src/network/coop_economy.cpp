#include "coop_economy.h"
#include "net_manager.h"
#include "net_common.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../hooks/process_event.h"
#include "../debug/crash_handler.h"

#include <cstring>
#include <string>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static bool  s_EcoInit = false;
static float s_EcoAccum = 0.0f;
constexpr float ECO_SYNC_INTERVAL = 3.0f;  // sync economy every 3s

// Cached property offsets on ShockPlayer
static int s_ADAMOffset = -1;
static int s_CreditsOffset = -1;
static int s_MaxCreditsOffset = -1;

// Last known values for delta detection
static int s_LastADAM = 0;
static int s_LastCredits = 0;

// Pending incoming economy data
static EconomySyncData s_PendingEco{};
static bool s_HasPendingEco = false;

// Hook ID for AddADAM/AddCredits interception
static int s_EcoHookId = -1;

// ─── Helpers ───────────────────────────────────────────────────────────

static UObject* FindShockPlayer()
{
    return FindObjectByClassName("ShockPlayer");
}

static void CacheEconomyOffsets()
{
    if (s_ADAMOffset > 0) return; // already cached

    UStruct* cls = FindClass("ShockPlayer");
    if (!cls) return;

    std::vector<PropertyInfo> props = WalkProperties(cls);
    PropertyInfo* pi;

    pi = FindProperty(cls, "ADAM", props);
    if (pi) s_ADAMOffset = pi->Offset;

    pi = FindProperty(cls, "Credits", props);
    if (pi) s_CreditsOffset = pi->Offset;

    pi = FindProperty(cls, "MaxCredits", props);
    if (pi) s_MaxCreditsOffset = pi->Offset;

    LOG_INFO("[Economy] Cached offsets: ADAM={}, Credits={}, MaxCredits={}",
             s_ADAMOffset, s_CreditsOffset, s_MaxCreditsOffset);
}

static void ReadLocalEconomy(int& adam, int& credits, int& maxCredits)
{
    adam = 0; credits = 0; maxCredits = 9999;
    UObject* player = FindShockPlayer();
    if (!player || !IsSafeToRead(player, 4096)) return;

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(player);
    if (s_ADAMOffset > 0 && IsSafeToRead(raw + s_ADAMOffset, 4))
        memcpy(&adam, raw + s_ADAMOffset, 4);
    if (s_CreditsOffset > 0 && IsSafeToRead(raw + s_CreditsOffset, 4))
        memcpy(&credits, raw + s_CreditsOffset, 4);
    if (s_MaxCreditsOffset > 0 && IsSafeToRead(raw + s_MaxCreditsOffset, 4))
        memcpy(&maxCredits, raw + s_MaxCreditsOffset, 4);
}

static void WriteLocalEconomy(int adam, int credits)
{
    UObject* player = FindShockPlayer();
    if (!player || !IsSafeToRead(player, 4096)) return;

    uint8_t* raw = reinterpret_cast<uint8_t*>(player);
    if (s_ADAMOffset > 0 && IsSafeToRead(raw + s_ADAMOffset, 4))
        memcpy(raw + s_ADAMOffset, &adam, 4);
    if (s_CreditsOffset > 0 && IsSafeToRead(raw + s_CreditsOffset, 4))
        memcpy(raw + s_CreditsOffset, &credits, 4);
}

// ─── Hook: detect ADAM/Credit gains ────────────────────────────────────

static void InstallEconomyHook()
{
    ProcessEventHook hook;
    hook.Name = "CoopEconomySync";
    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
        if (!obj || !func || !IsSafeToRead(obj, 32) || !IsSafeToRead(func, 32))
            return false;
        std::string cn = obj->GetObjClassName();
        if (cn != "ShockPlayer") return false;

        std::string fn = func->GetName();

        // Detect ADAM/Credits changes via function calls
        if (fn == "AddADAM" && parms) {
            int amount = *reinterpret_cast<int*>(parms);
            if (amount > 0) {
                int adam, credits, maxCredits;
                ReadLocalEconomy(adam, credits, maxCredits);

                EconomySyncData eco{};
                eco.adam = adam + amount; // will be the new value after this call
                eco.credits = credits;
                eco.maxCredits = maxCredits;
                eco.eventType = 1; // ADAM gained
                eco.eventAmount = amount;
                NetSendRawPacket(PacketType::EconomySync, &eco, sizeof(eco));
            }
            return false;
        }

        if (fn == "AddCredits" && parms) {
            int amount = *reinterpret_cast<int*>(parms);
            if (amount > 0) {
                int adam, credits, maxCredits;
                ReadLocalEconomy(adam, credits, maxCredits);

                EconomySyncData eco{};
                eco.adam = adam;
                eco.credits = credits + amount;
                eco.maxCredits = maxCredits;
                eco.eventType = 2; // Credits gained
                eco.eventAmount = amount;
                NetSendRawPacket(PacketType::EconomySync, &eco, sizeof(eco));
            }
            return false;
        }

        return false;
    };
    s_EcoHookId = RegisterProcessEventHook(hook);
}

// ─── Apply incoming economy sync ──────────────────────────────────────

static void ApplyEconomySync(const EconomySyncData& data)
{
    int localAdam, localCredits, localMaxCredits;
    ReadLocalEconomy(localAdam, localCredits, localMaxCredits);

    bool changed = false;

    switch (data.eventType) {
    case 1: { // ADAM gained by remote — give same ADAM to local
        if (data.eventAmount > 0) {
            int newAdam = localAdam + data.eventAmount;
            WriteLocalEconomy(newAdam, localCredits);
            LOG_INFO("[Economy] Partner gained {} ADAM — local now {}",
                     data.eventAmount, newAdam);
            changed = true;
        }
        break;
    }
    case 2: { // Credits gained by remote — give same credits to local
        if (data.eventAmount > 0) {
            int newCredits = localCredits + data.eventAmount;
            if (newCredits > localMaxCredits) newCredits = localMaxCredits;
            WriteLocalEconomy(localAdam, newCredits);
            LOG_INFO("[Economy] Partner gained {} Credits — local now {}",
                     data.eventAmount, newCredits);
            changed = true;
        }
        break;
    }
    case 0: // Periodic snapshot — just log, don't force-sync values
    default:
        break;
    }
}

// ─── Public API ────────────────────────────────────────────────────────

bool InitEconomySync()
{
    if (s_EcoInit) return true;

    CacheEconomyOffsets();
    InstallEconomyHook();

    // Read initial values
    int initMaxCredits = 0;
    ReadLocalEconomy(s_LastADAM, s_LastCredits, initMaxCredits);

    s_EcoInit = true;
    LOG_INFO("[Economy] Economy sync initialized");
    return true;
}

void ShutdownEconomySync()
{
    if (!s_EcoInit) return;
    if (s_EcoHookId >= 0) UnregisterProcessEventHook(s_EcoHookId);
    s_EcoHookId = -1;
    s_EcoInit = false;
}

void TickEconomySync(float deltaTime)
{
    if (!s_EcoInit) return;

    // Process pending incoming
    if (s_HasPendingEco) {
        CrashBreadcrumb("EcoTick: ApplyEconomySync");
        ApplyEconomySync(s_PendingEco);
        s_HasPendingEco = false;
    }

    // Periodic snapshot broadcast
    s_EcoAccum += deltaTime;
    if (s_EcoAccum >= ECO_SYNC_INTERVAL) {
        s_EcoAccum = 0.0f;

        CrashBreadcrumb("EcoTick: ReadLocalEconomy");
        int adam, credits, maxCredits;
        ReadLocalEconomy(adam, credits, maxCredits);

        EconomySyncData eco{};
        eco.adam = adam;
        eco.credits = credits;
        eco.maxCredits = maxCredits;
        eco.eventType = 0; // snapshot
        eco.eventAmount = 0;

        CrashBreadcrumb("EcoTick: NetSendRawPacket");
        NetSendRawPacket(PacketType::EconomySync, &eco, sizeof(eco));

        s_LastADAM = adam;
        s_LastCredits = credits;
    }
}

void QueueEconomySyncPacket(const EconomySyncData& data)
{
    s_PendingEco = data;
    s_HasPendingEco = true;
}

} // namespace bs1sdk
