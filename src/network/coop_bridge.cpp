#include "coop_bridge.h"
#include "coop_render.h"
#include "coop_sync.h"
#include "coop_puppet.h"
#include "coop_economy.h"
#include "coop_save.h"
#include "net_manager.h"
#include "net_common.h"
#include "../core/log.h"
#include "../engine/uobject.h"
#include "../hooks/process_event.h"
#include "../debug/crash_handler.h"

#include <cstring>
#include <thread>
#include <atomic>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static bool s_Initialized = false;
static bool s_Active = false;
static float s_SendAccum = 0.0f;

// Background keepalive thread (runs even when game is alt-tabbed)
static std::atomic<bool> s_KeepAliveRunning{false};
static std::thread s_KeepAliveThread;

// Chat callback for overlay
static OnChatFunc s_LocalChatCallback;

// Cached property offsets for reading player state
static int s_LocOffset = -1;     // Location (Vector)
static int s_RotOffset = -1;     // Rotation (Rotator)
static int s_HealthOffset = -1;  // Health (float)
// static int s_EveOffset = -1;     // EVE (float) - TODO: find property name

// Level tracking
static std::string s_LocalLevelName;
static std::string s_RemoteLevelName;
static bool s_LevelMismatch = false;

// ─── Level Detection ───────────────────────────────────────────────────

/// Get current map name by finding ShockPlayer's outermost package.
static std::string DetectCurrentLevel()
{
    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) return "";

    // Walk Outer chain to get the outermost package (= map name)
    UObject* outer = player->GetOuter();
    UObject* prev = player;
    while (outer) {
        prev = outer;
        outer = outer->GetOuter();
    }

    std::string name = prev->GetName();

    // Strip common prefixes/suffixes if needed
    // BioShock maps are like "1-Welcome", "1-Medical", etc.
    return name;
}

// ─── Player State Reading ──────────────────────────────────────────────

static bool CachePlayerOffsets()
{
    if (s_LocOffset > 0 && s_RotOffset > 0 && s_HealthOffset > 0) return true;

    UObject* player = FindObjectByClassName("ShockPlayer");
    if (!player) return false;

    UStruct* cls = reinterpret_cast<UStruct*>(player->GetClass());
    if (!cls) return false;

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

    if (s_LocOffset > 0) {
        LOG_INFO("[Co-op] Cached offsets: Loc={}, Rot={}, Health={}", s_LocOffset, s_RotOffset, s_HealthOffset);
    }
    return s_LocOffset > 0;
}

static PlayerStateData ReadLocalPlayerState()
{
    PlayerStateData state{};

    // Ensure offsets are cached (PE hook may not be firing)
    if (s_LocOffset < 0) CachePlayerOffsets();

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
    // Update the rendering system with remote state (overlay marker)
    const NetPeer* peer = GetRemotePeer();
    SetRemoteRenderState(remoteState, peer ? peer->name.c_str() : "Partner");

    // Update the in-world ghost puppet (3D model)
    UpdateGhostPuppet(remoteState);
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
        s_RemoteLevelName = peer.levelName;
        s_LevelMismatch = (!s_LocalLevelName.empty() && !s_RemoteLevelName.empty() &&
                           s_LocalLevelName != s_RemoteLevelName);
        if (s_LevelMismatch) {
            LOG_WARN("[Co-op] Player '{}' is on level '{}' but you are on '{}' - sync won't work!",
                     peer.name, peer.levelName, s_LocalLevelName);
        } else {
            LOG_INFO("[Co-op] Player '{}' joined on level '{}'!", peer.name, peer.levelName);
        }
    } else {
        LOG_INFO("[Co-op] Player '{}' left.", peer.name);
        s_RemoteLevelName.clear();
        s_LevelMismatch = false;
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
    SetOnChat([](const std::string& sender, const std::string& msg) {
        LOG_INFO("[Chat] {}: {}", sender, msg);
        if (s_LocalChatCallback) s_LocalChatCallback(sender, msg);
    });

    // Register a ProcessEvent hook for reading player state on Tick
    if (IsProcessEventHooked()) {
        ProcessEventHook hook;
        hook.Name = "CoopBridge";
        hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
            if (!obj || !func || !IsSafeToRead(obj, 32) || !IsSafeToRead(func, 32))
                return false;
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
    InitGhostPuppet();
    InitEconomySync();
    LOG_INFO("[Co-op] Bridge initialized (render + sync + puppet + economy)");
    return true;
}

void ShutdownCoopBridge()
{
    if (!s_Initialized) return;
    CoopDisconnect();
    DestroyGhostPuppet();
    ShutdownGhostPuppet();
    ShutdownEconomySync();
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

    // Detect and set current level before handshake
    s_LocalLevelName = DetectCurrentLevel();
    NetSetLocalLevel(s_LocalLevelName);
    if (s_LocalLevelName.empty()) {
        LOG_WARN("[Co-op] Could not detect level - are you in-game?");
    } else {
        LOG_INFO("[Co-op] Current level: {}", s_LocalLevelName);
    }

    if (!NetHost(port, name)) return false;
    s_Active = true;

    // Start background keepalive thread (survives alt-tab)
    s_KeepAliveRunning = true;
    s_KeepAliveThread = std::thread([]() {
        while (s_KeepAliveRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (s_Active) NetTick(0.5f);
        }
    });
    s_KeepAliveThread.detach();

    return true;
}

bool CoopJoin(const std::string& ip, uint16_t port, const std::string& name)
{
    if (!s_Initialized) InitCoopBridge();

    // Detect and set current level before handshake
    s_LocalLevelName = DetectCurrentLevel();
    NetSetLocalLevel(s_LocalLevelName);
    if (s_LocalLevelName.empty()) {
        LOG_WARN("[Co-op] Could not detect level - are you in-game?");
    } else {
        LOG_INFO("[Co-op] Current level: {}", s_LocalLevelName);
    }

    if (!NetJoin(ip, port, name)) return false;
    s_Active = true;

    // Start background keepalive thread (survives alt-tab)
    s_KeepAliveRunning = true;
    s_KeepAliveThread = std::thread([]() {
        while (s_KeepAliveRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (s_Active) NetTick(0.5f);
        }
    });
    s_KeepAliveThread.detach();

    return true;
}

void CoopDisconnect()
{
    s_KeepAliveRunning = false;
    DestroyGhostPuppet();
    NetDisconnect();
    s_Active = false;
}

static float s_LevelCheckAccum = 0.0f;

void CoopTick(float deltaTime)
{
    if (!s_Active) return;
    CrashSetContext("runtime:CoopTick");

    // Process network
    CrashBreadcrumb("CoopTick: NetTick");
    NetTick(deltaTime);

    // Read local player state every frame (needed for camera + send)
    auto localState = ReadLocalPlayerState();

    // Always update camera for overlay marker rendering
    if (localState.posX != 0 || localState.posY != 0 || localState.posZ != 0) {
        SetLocalCamera(localState.posX, localState.posY, localState.posZ,
                       localState.rotPitch, localState.rotYaw, 90.0f);
    }

    // Send local state at tick rate
    if (IsNetConnected()) {
        s_SendAccum += deltaTime;
        if (s_SendAccum >= NET_TICK_INTERVAL) {
            s_SendAccum -= NET_TICK_INTERVAL;
            NetSendPlayerState(localState);
        }

        // Periodic level change detection (every 2s)
        s_LevelCheckAccum += deltaTime;
        if (s_LevelCheckAccum >= 2.0f) {
            s_LevelCheckAccum = 0.0f;
            std::string currentLevel = DetectCurrentLevel();
            if (!currentLevel.empty() && currentLevel != s_LocalLevelName) {
                std::string oldLevel = s_LocalLevelName;
                s_LocalLevelName = currentLevel;

                // Notify partner of level change
                LevelSyncData lvl{};
                strncpy(lvl.levelName, currentLevel.c_str(), sizeof(lvl.levelName) - 1);
                lvl.isLoading = 0;
                NetSendRawPacket(PacketType::LevelSync, &lvl, sizeof(lvl));

                // Destroy old puppet (will respawn in new level)
                DestroyGhostPuppet();

                LOG_INFO("[Co-op] Level changed: {} -> {}", oldLevel, currentLevel);
            }
        }

        // Process incoming damage/world events + periodic enemy HP sync
        CrashBreadcrumb("CoopTick: CoopSyncProcessPackets");
        CoopSyncProcessPackets(deltaTime);

        // Economy sync (ADAM/Credits sharing)
        CrashBreadcrumb("CoopTick: TickEconomySync");
        TickEconomySync(deltaTime);

        // Tick save file transfer if active
        CrashBreadcrumb("CoopTick: TickSaveTransfer");
        TickSaveTransfer();
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

    // Level info
    if (!s_LocalLevelName.empty())
        status += "\n  Local level: " + s_LocalLevelName;
    if (!s_RemoteLevelName.empty())
        status += "\n  Remote level: " + s_RemoteLevelName;
    if (s_LevelMismatch)
        status += "\n  *** LEVEL MISMATCH - sync disabled! ***";

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

void SetCoopChatCallback(ChatDisplayFunc fn)
{
    s_LocalChatCallback = std::move(fn);
}

} // namespace bs1sdk
