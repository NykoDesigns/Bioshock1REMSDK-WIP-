#include "coop_bridge.h"
#include "coop_render.h"
#include "coop_sync.h"
#include "coop_puppet.h"
#include "coop_economy.h"
#include "coop_save.h"
#include "coop_true.h"
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
static int s_FlashCountOffset = -1; // FlashCount (byte) — increments on weapon fire
static uint8_t s_LastFlashCount = 0; // Track changes for fire detection

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
    if (s_FlashCountOffset < 0) {
        auto* pi = FindProperty(cls, "FlashCount", allProps);
        if (pi) s_FlashCountOffset = pi->Offset;
    }

    if (s_LocOffset > 0) {
        LOG_INFO("[Co-op] Cached offsets: Loc={}, Rot={}, Health={}, FlashCount={}", 
                 s_LocOffset, s_RotOffset, s_HealthOffset, s_FlashCountOffset);
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

            // Read FlashCount for fire detection
            if (s_FlashCountOffset > 0) {
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(obj);
                uint8_t flashCount = *(raw + s_FlashCountOffset);
                // isFiring = 1 if FlashCount changed this frame
                if (flashCount != s_LastFlashCount) {
                    state.isFiring = 1;
                    s_LastFlashCount = flashCount;
                }
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

    // Detect firing — isFiring=1 means FlashCount changed on sender side
    if (remoteState.isFiring) {
        PlayerActionData action{};
        action.action = ActionType::WeaponFire;
        action.dirX = cosf(remoteState.rotYaw * 3.14159f / 180.0f);
        action.dirY = sinf(remoteState.rotYaw * 3.14159f / 180.0f);
        action.dirZ = sinf(remoteState.rotPitch * 3.14159f / 180.0f);
        NotifyPuppetAction(action);
    }

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

// ─── Crash Info Provider ──────────────────────────────────────────────

static void CrashProvider_CoopState(FILE* f1, FILE* f2)
{
    #define CE(...) CRASH_EMIT(f1, f2, __VA_ARGS__)
    CE("  Active:       %s\n", s_Active ? "YES" : "no");
    CE("  Net Role:     %s\n", GetNetRole() == NetRole::Host ? "HOST" :
                                GetNetRole() == NetRole::Client ? "CLIENT" : "NONE");
    CE("  Connected:    %s\n", IsNetConnected() ? "YES" : "no");
    CE("  Has Puppet:   %s\n", HasGhostPuppet() ? "YES" : "no");
    CE("  Local Level:  %s\n", s_LocalLevelName.empty() ? "(unknown)" : s_LocalLevelName.c_str());
    CE("  Remote Level: %s\n", s_RemoteLevelName.empty() ? "(unknown)" : s_RemoteLevelName.c_str());
    CE("  Level Match:  %s\n", s_LevelMismatch ? "MISMATCH" : "ok");

    const NetPeer* peer = GetRemotePeer();
    if (peer) {
        CE("  Remote Peer:  %s:%u (%s)\n", peer->ip.c_str(), peer->port, peer->name.c_str());
        CE("  Last Recv:    %.1fs ago\n", peer->lastRecvTime);
        CE("  Remote Pos:   (%.0f, %.0f, %.0f)\n",
             peer->lastState.posX, peer->lastState.posY, peer->lastState.posZ);
    } else {
        CE("  Remote Peer:  (none)\n");
    }

    std::string puppetDiag = GetPuppetDiagnostics();
    if (!puppetDiag.empty()) {
        CE("  Puppet Info:  %s\n", puppetDiag.c_str());
    }
    #undef CE
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

    // NO PE hooks needed — CoopTick already reads player state every frame
    // via ReadLocalPlayerState() + SetLocalCamera(). PE hooks add dispatch
    // overhead on every single ProcessEvent call even with filters.

    s_Initialized = true;
    InitCoopRender();
    InitGhostPuppet();
    // NOTE: InitCoopSync() and InitEconomySync() disabled — they register
    // unfiltered PE hooks that cause ~50K heap allocations/frame and tank FPS.
    // Re-enable once those hooks use FunctionFilter or zero-alloc FName checks.

    // Register crash info provider for co-op state
    RegisterCrashInfoProvider("CO-OP STATE", CrashProvider_CoopState);

    LOG_INFO("[Co-op] Bridge initialized (render + puppet)");
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

    // Drive TrueCoopTick directly (no PE-based tick callback needed)
    CrashBreadcrumb("CoopTick: TrueCoopTick");
    TrueCoopTick(deltaTime);

    // Read local player state every frame (needed for camera + send)
    CrashBreadcrumb("CoopTick: ReadLocalPlayerState");
    auto localState = ReadLocalPlayerState();

    // Always update camera for overlay marker rendering
    CrashBreadcrumb("CoopTick: SetLocalCamera");
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
