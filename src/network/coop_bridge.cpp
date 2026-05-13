#include "coop_bridge.h"
#include "coop_render.h"
#include "coop_sync.h"
#include "net_manager.h"
#include "net_common.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../hooks/process_event.h"

#include <cstring>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static bool s_Initialized = false;
static bool s_Active = false;
static float s_SendAccum = 0.0f;

// Cached property offsets for reading player state
static int s_LocOffset = -1;     // Location (Vector)
static int s_RotOffset = -1;     // Rotation (Rotator)
static int s_HealthOffset = -1;  // Health (float)
// static int s_EveOffset = -1;     // EVE (float) - TODO: find property name

// Remote player puppet
static UObject* s_PuppetPawn = nullptr;

// ─── Player State Reading ──────────────────────────────────────────────

static bool CachePlayerOffsets()
{
    // Walk GObjects to find ShockPlayer class and cache property offsets
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return false;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    for (int i = 0; i < objCount && i < 100000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);

        std::string cn = obj->GetObjClassName();
        if (cn != "ShockPlayer" && cn != "Pawn") continue;

        std::string name = obj->GetName();

        // Walk properties to find Location, Rotation, Health
        // These are on the Actor → Pawn → ShockPlayer hierarchy
        UObject* cls = obj->GetClass();
        if (!cls) continue;

        // Try to find properties by iterating the property chain
        // Property offsets discovered in property_layout.txt
        // For now, use known offsets from UE2.5 Actor class:
        //   Location typically at offset ~0x64-0x70 in Actor
        //   Rotation typically at offset ~0x70-0x7C in Actor
        //   Health at various offsets in Pawn subclass

        // We'll use ProcessEvent hook tick to read these dynamically
        break;
    }

    return true;
}

static PlayerStateData ReadLocalPlayerState()
{
    PlayerStateData state{};

    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return state;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Find the local player controller / pawn
    for (int i = 0; i < objCount && i < 100000; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);

        std::string cn = obj->GetObjClassName();

        // ShockPlayer is the player pawn in BioShock
        if (cn == "ShockPlayer") {
            // Read Location property (3 floats at the cached offset)
            if (s_LocOffset > 0) {
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                memcpy(&state.posX, raw + s_LocOffset, 4);
                memcpy(&state.posY, raw + s_LocOffset + 4, 4);
                memcpy(&state.posZ, raw + s_LocOffset + 8, 4);
            }

            // Read Rotation (3 int32s -> convert to float degrees)
            if (s_RotOffset > 0) {
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                int32_t pitch, yaw;
                memcpy(&pitch, raw + s_RotOffset, 4);
                memcpy(&yaw, raw + s_RotOffset + 4, 4);
                // UE2 rotator: 65536 = 360 degrees
                state.rotPitch = pitch * (360.0f / 65536.0f);
                state.rotYaw = yaw * (360.0f / 65536.0f);
            }

            // Read Health
            if (s_HealthOffset > 0) {
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                memcpy(&state.health, raw + s_HealthOffset, 4);
            }

            break;
        }
    }

    return state;
}

// ─── Remote Player Puppet ──────────────────────────────────────────────

static void UpdatePuppet(const PlayerStateData& remoteState)
{
    // Update the rendering system with remote state
    const NetPeer* peer = GetRemotePeer();
    SetRemoteRenderState(remoteState, peer ? peer->name.c_str() : "Partner");

    // Also try to move a puppet pawn if we have one
    if (s_PuppetPawn && s_LocOffset > 0) {
        uint8_t* raw = reinterpret_cast<uint8_t*>(s_PuppetPawn);
        memcpy(raw + s_LocOffset, &remoteState.posX, 4);
        memcpy(raw + s_LocOffset + 4, &remoteState.posY, 4);
        memcpy(raw + s_LocOffset + 8, &remoteState.posZ, 4);

        if (s_RotOffset > 0) {
            // Convert float degrees back to UE2 rotator ints
            int32_t pitch = (int32_t)(remoteState.rotPitch * (65536.0f / 360.0f));
            int32_t yaw = (int32_t)(remoteState.rotYaw * (65536.0f / 360.0f));
            memcpy(raw + s_RotOffset, &pitch, 4);
            memcpy(raw + s_RotOffset + 4, &yaw, 4);
        }
    }
}

// ─── ProcessEvent Hook for Tick ────────────────────────────────────────

static int s_HookId = -1;

static void OnRemoteStateReceived(const NetPeer& peer, const PlayerStateData& state)
{
    UpdatePuppet(state);
}

static void OnPeerConnectionEvent(const NetPeer& peer, bool connected)
{
    if (connected) {
        LOG_INFO("[Co-op] Player '{}' joined!", peer.name);
    } else {
        LOG_INFO("[Co-op] Player '{}' left.", peer.name);
        // TODO: hide/destroy puppet
    }
}

// ─── Public API ────────────────────────────────────────────────────────

bool InitCoopBridge()
{
    if (s_Initialized) return true;

    CachePlayerOffsets();

    // Register callbacks
    SetOnRemoteState(OnRemoteStateReceived);
    SetOnPeerEvent(OnPeerConnectionEvent);

    // Register a ProcessEvent hook for reading player state on Tick
    if (IsProcessEventHooked()) {
        ProcessEventHook hook;
        hook.Name = "CoopBridge";
        hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
            // We use PlayerTick to read state at the right time
            std::string funcName = func->GetName();
            if (funcName == "PlayerTick" || funcName == "Tick") {
                std::string cn = obj->GetObjClassName();
                if (cn == "ShockPlayer" || cn == "ShockPlayerController") {
                    // Cache property offsets on first encounter
                    UStruct* cls = reinterpret_cast<UStruct*>(obj->GetClass());
                    if (cls) {
                        std::vector<PropertyInfo> allProps = WalkProperties(cls);
                        if (s_LocOffset < 0) {
                            auto* pi = FindProperty(cls, "Location", allProps);
                            if (pi) s_LocOffset = pi->Offset;
                        }
                        if (s_RotOffset < 0) {
                            auto* pi = FindProperty(cls, "Rotation", allProps);
                            if (pi) s_RotOffset = pi->Offset;
                        }
                        if (s_HealthOffset < 0) {
                            auto* pi = FindProperty(cls, "Health", allProps);
                            if (pi) s_HealthOffset = pi->Offset;
                        }
                    }

                    // Update camera for rendering (use player pos + rotation as camera)
                    if (s_LocOffset > 0 && s_RotOffset > 0) {
                        const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                        float px, py, pz;
                        memcpy(&px, raw + s_LocOffset, 4);
                        memcpy(&py, raw + s_LocOffset + 4, 4);
                        memcpy(&pz, raw + s_LocOffset + 8, 4);
                        int32_t rPitch, rYaw;
                        memcpy(&rPitch, raw + s_RotOffset, 4);
                        memcpy(&rYaw, raw + s_RotOffset + 4, 4);
                        float pitchDeg = rPitch * (360.0f / 65536.0f);
                        float yawDeg = rYaw * (360.0f / 65536.0f);
                        SetLocalCamera(px, py, pz, pitchDeg, yawDeg, 90.0f);
                    }
                }
            }
            return false; // don't block
        };
        s_HookId = RegisterProcessEventHook(hook);
    }

    s_Initialized = true;
    InitCoopRender();
    InitCoopSync();
    LOG_INFO("[Co-op] Bridge initialized (render + sync)");
    return true;
}

void ShutdownCoopBridge()
{
    if (!s_Initialized) return;
    CoopDisconnect();
    ShutdownCoopSync();
    if (s_HookId >= 0) {
        UnregisterProcessEventHook(s_HookId);
        s_HookId = -1;
    }
    s_Initialized = false;
}

bool CoopHost(uint16_t port, const std::string& name)
{
    if (!s_Initialized) InitCoopBridge();
    if (!NetHost(port, name)) return false;
    s_Active = true;
    return true;
}

bool CoopJoin(const std::string& ip, uint16_t port, const std::string& name)
{
    if (!s_Initialized) InitCoopBridge();
    if (!NetJoin(ip, port, name)) return false;
    s_Active = true;
    return true;
}

void CoopDisconnect()
{
    NetDisconnect();
    s_Active = false;
    s_PuppetPawn = nullptr;
}

void CoopTick(float deltaTime)
{
    if (!s_Active) return;

    // Process network
    NetTick(deltaTime);

    // Send local state at tick rate
    if (IsNetConnected()) {
        s_SendAccum += deltaTime;
        if (s_SendAccum >= NET_TICK_INTERVAL) {
            s_SendAccum -= NET_TICK_INTERVAL;
            auto state = ReadLocalPlayerState();
            NetSendPlayerState(state);
        }

        // Process incoming damage/world events
        CoopSyncProcessPackets();
    }
}

std::string GetCoopStatus()
{
    if (!s_Active) return "Co-op: Not active";

    std::string status = GetNetStatus();

    // Add local state info
    if (IsNetConnected()) {
        auto* peer = GetRemotePeer();
        if (peer) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "\n  Remote pos: (%.0f, %.0f, %.0f) HP=%.0f",
                     peer->lastState.posX, peer->lastState.posY, peer->lastState.posZ,
                     peer->lastState.health);
            status += buf;
        }
    }

    if (s_LocOffset > 0)
        status += "\n  Location offset: +" + std::to_string(s_LocOffset);
    if (s_RotOffset > 0)
        status += "\n  Rotation offset: +" + std::to_string(s_RotOffset);
    if (s_HealthOffset > 0)
        status += "\n  Health offset: +" + std::to_string(s_HealthOffset);

    return status;
}

bool IsCoopActive()
{
    return s_Active;
}

} // namespace bs1sdk
