#include "coop_p2.h"
#include "coop_world_sync.h"
#include "net_manager.h"
#include "../core/log.h"
#include "../engine/world.h"
#include "../engine/function_caller.h"
#include "../hooks/process_event.h"
#include "../debug/coop_debug.h"

#include <Windows.h>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <fstream>
#include <mutex>

namespace bs1sdk {

// ─── State ───────────────────────────────────────────────────────────────

static bool s_P2Initialized = false;
static UObject* s_P2Pawn = nullptr;         // The commandeered pawn on host
static std::string s_P2PawnName;
static P2PawnState s_P2State{};             // Latest state
static P2PawnState s_ClientP2State{};       // Client's received state
static bool s_HasCameraOverride = false;

// Input state
static P2InputData s_LastInput{};
static float s_InputSendAccum = 0.0f;
static uint32_t s_InputSeqNum = 0;

// Combat tracking
static int s_P2Kills = 0;
static int s_P2Deaths = 0;
static float s_P2DamageDealt = 0;
static float s_P2DamageTaken = 0;

// Logging
static std::ofstream s_P2Log;

static void P2Log(const char* fmt, ...)
{
    if (!s_P2Log.is_open()) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_P2Log << buf << "\n";
    s_P2Log.flush();
}

// ─── Init / Shutdown ─────────────────────────────────────────────────────

void InitP2System()
{
    if (s_P2Initialized) return;
    s_P2Initialized = true;

    std::string logPath = std::string(DEBUG_DIR) + "/p2_system_log.txt";
    s_P2Log.open(logPath, std::ios::trunc);
    P2Log("P2 system initialized. Role=%s", IsTrueHost() ? "Host" : "Client");

    LOG_INFO("[P2] System initialized");
}

void ShutdownP2System()
{
    if (!s_P2Initialized) return;
    P2DestroyPawn();
    P2Log("P2 system shutdown. Kills=%d Deaths=%d DmgDealt=%.0f DmgTaken=%.0f",
           s_P2Kills, s_P2Deaths, s_P2DamageDealt, s_P2DamageTaken);
    s_P2Log.close();
    s_P2Initialized = false;
}

// ─── Host: Spawn P2 Pawn ─────────────────────────────────────────────────
// Strategy: Find an existing ShockAI/Pawn that we can commandeer, or find
// the DecoyHuman class and see if we can spawn one via ProcessEvent.
// Fallback: Use any existing pawn and relocate it to player's position.

bool P2SpawnPawn()
{
    if (!IsTrueHost()) return false;
    if (s_P2Pawn) return true; // already spawned

    // Strategy 1: Try to find an unused DecoyHuman or friendly NPC
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return false;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    UObject* bestCandidate = nullptr;
    std::string bestClass;

    // Look for potential P2 bodies in priority order
    static const char* searchClasses[] = {
        "DecoyHuman", "FriendlyNPC", "ShockPlayer", "ShockPawn", "Pawn", nullptr
    };

    for (int ci = 0; searchClasses[ci] && !bestCandidate; ci++) {
        for (int i = 0; i < objCount && i < 200000; i++) {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
            if (!ptr) continue;
            UObject* obj = reinterpret_cast<UObject*>(ptr);
            std::string cn = obj->GetObjClassName();
            if (cn == searchClasses[ci]) {
                // Don't steal the actual player
                if (cn == "ShockPlayer") {
                    // Find a second one if it exists, or skip
                    continue;
                }
                bestCandidate = obj;
                bestClass = cn;
                break;
            }
        }
    }

    if (!bestCandidate) {
        // Fallback: Try spawning via ProcessEvent on GameInfo
        // Call GameInfo.Spawn(class'Pawn') — this may or may not work
        P2Log("WARN: No candidate pawn found for P2. Attempting spawn via GameInfo...");

        UObject* gameInfo = FindObjectByClassName("ShockGameInfo");
        if (gameInfo) {
            // For now, log and fail gracefully — spawn mechanism needs testing
            P2Log("Found GameInfo at 0x%08X, spawn not yet implemented", (uint32_t)(uintptr_t)gameInfo);
            LOG_WARN("[P2] No candidate pawn found. GameInfo spawn not yet implemented.");
            DebugSessionLog("P2: No pawn candidate found, spawn needed");
            return false;
        }

        LOG_ERROR("[P2] Cannot spawn P2 pawn — no candidate and no GameInfo");
        return false;
    }

    s_P2Pawn = bestCandidate;
    s_P2PawnName = bestCandidate->GetName();

    // Move P2 pawn near the player
    FVec3 playerPos;
    if (GetPlayerPosition(playerPos)) {
        FVec3 p2Pos = {playerPos.X + 100.0f, playerPos.Y + 100.0f, playerPos.Z};
        SetActorPosition(s_P2Pawn, p2Pos);
    }

    P2Log("P2 pawn spawned: [%s] %s at 0x%08X",
           bestClass.c_str(), s_P2PawnName.c_str(), (uint32_t)(uintptr_t)s_P2Pawn);
    LOG_INFO("[P2] Pawn commandeered: [{}] {}", bestClass, s_P2PawnName);
    DebugSessionLogf("P2 pawn: [%s] %s", bestClass.c_str(), s_P2PawnName.c_str());

    return true;
}

bool P2DestroyPawn()
{
    if (!s_P2Pawn) return false;
    P2Log("P2 pawn released: %s", s_P2PawnName.c_str());
    s_P2Pawn = nullptr;
    s_P2PawnName.clear();
    s_HasCameraOverride = false;
    return true;
}

bool P2IsSpawned() { return s_P2Pawn != nullptr; }
UObject* P2GetPawn() { return s_P2Pawn; }

// ─── Host: Apply P2 Input ────────────────────────────────────────────────
// Move the P2 pawn based on received input.
// Uses direct position manipulation (simplest approach first).

void P2ApplyInput(const P2InputData& input)
{
    if (!s_P2Pawn || !IsTrueHost()) return;

    // Convert input to movement delta
    float moveSpeed = 300.0f; // units per second (tune later)
    float dt = 1.0f / 60.0f; // assume 60fps for now

    // Calculate forward/right vectors from yaw
    float yawRad = input.lookYaw * 3.14159f / 180.0f;
    float fwdX = std::cos(yawRad);
    float fwdY = std::sin(yawRad);
    float rightX = std::sin(yawRad);
    float rightY = -std::cos(yawRad);

    float moveX = (input.moveForward * fwdX + input.moveRight * rightX) * moveSpeed * dt;
    float moveY = (input.moveForward * fwdY + input.moveRight * rightY) * moveSpeed * dt;
    float moveZ = 0; // TODO: jump

    // Apply jump
    if (input.buttonMask & P2Button::Jump) {
        moveZ = 200.0f * dt; // upward impulse
    }

    // Get current position and apply delta
    FVec3 pos;
    if (GetActorPosition(s_P2Pawn, pos)) {
        pos.X += moveX;
        pos.Y += moveY;
        pos.Z += moveZ;
        SetActorPosition(s_P2Pawn, pos);
    }

    // Handle combat buttons
    if (input.buttonMask & P2Button::Fire) {
        P2CombatEvent evt{};
        evt.Action = P2CombatAction::WeaponFire;
        evt.WeaponId = input.weaponSlot;
        // Aim direction from look angles
        float pitchRad = input.lookPitch * 3.14159f / 180.0f;
        evt.DirX = std::cos(pitchRad) * std::cos(yawRad);
        evt.DirY = std::cos(pitchRad) * std::sin(yawRad);
        evt.DirZ = std::sin(pitchRad);
        P2ProcessCombat(evt);
    }

    if (input.buttonMask & P2Button::Melee) {
        P2CombatEvent evt{};
        evt.Action = P2CombatAction::MeleeSwing;
        float pitchRad = input.lookPitch * 3.14159f / 180.0f;
        evt.DirX = std::cos(pitchRad) * std::cos(yawRad);
        evt.DirY = std::cos(pitchRad) * std::sin(yawRad);
        evt.DirZ = std::sin(pitchRad);
        P2ProcessCombat(evt);
    }

    if (input.buttonMask & P2Button::Plasmid) {
        P2CombatEvent evt{};
        evt.Action = P2CombatAction::PlasmidCast;
        evt.WeaponId = input.weaponSlot; // plasmid slot
        P2ProcessCombat(evt);
    }

    s_LastInput = input;
}

// ─── Host: Get P2 State ──────────────────────────────────────────────────

P2PawnState P2GetState()
{
    P2PawnState state{};
    if (!s_P2Pawn) {
        state.IsAlive = 0;
        return state;
    }

    FVec3 pos;
    if (GetActorPosition(s_P2Pawn, pos)) {
        state.PosX = pos.X;
        state.PosY = pos.Y;
        state.PosZ = pos.Z;
    }

    state.RotPitch = s_LastInput.lookPitch;
    state.RotYaw = s_LastInput.lookYaw;
    state.Health = 100.0f;  // TODO: read from pawn
    state.EVE = 100.0f;     // TODO: read from pawn
    state.WeaponId = s_LastInput.weaponSlot;
    state.IsAlive = 1;
    state.IsCrouching = (s_LastInput.buttonMask & P2Button::Crouch) ? 1 : 0;
    state.FrameNum = GetWorldSyncStats().HostFrame;

    s_P2State = state;
    return state;
}

// ─── Host: Process Combat ────────────────────────────────────────────────

void P2ProcessCombat(const P2CombatEvent& event)
{
    if (!s_P2Pawn || !IsTrueHost()) return;

    switch (event.Action) {
    case P2CombatAction::WeaponFire: {
        P2Log("COMBAT: WeaponFire weapon=%d dir=(%.2f,%.2f,%.2f)",
               event.WeaponId, event.DirX, event.DirY, event.DirZ);

        // Hitscan: trace from P2 position in aim direction
        FVec3 p2Pos;
        if (!GetActorPosition(s_P2Pawn, p2Pos)) break;

        float traceLen = 5000.0f;
        float endX = p2Pos.X + event.DirX * traceLen;
        float endY = p2Pos.Y + event.DirY * traceLen;
        float endZ = p2Pos.Z + event.DirZ * traceLen;

        // Find nearest actor in that direction (simplified)
        auto actors = GetActorsInRadius(p2Pos, 2000.0f);
        UObject* bestTarget = nullptr;
        float bestDot = 0.5f; // minimum dot product (cos 60 degrees)

        for (auto* actor : actors) {
            if (actor == s_P2Pawn) continue;
            std::string cn = actor->GetObjClassName();
            // Only damage enemy pawns
            if (cn.find("Thug") == std::string::npos &&
                cn.find("Aggressor") == std::string::npos &&
                cn.find("Crawler") == std::string::npos &&
                cn.find("Assassin") == std::string::npos &&
                cn.find("BigDaddy") == std::string::npos &&
                cn.find("Turret") == std::string::npos &&
                cn.find("SecurityBot") == std::string::npos) continue;

            FVec3 targetPos;
            if (!GetActorPosition(actor, targetPos)) continue;

            // Dot product with aim direction
            float dx = targetPos.X - p2Pos.X;
            float dy = targetPos.Y - p2Pos.Y;
            float dz = targetPos.Z - p2Pos.Z;
            float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist < 1.0f) continue;
            dx /= dist; dy /= dist; dz /= dist;

            float dot = dx * event.DirX + dy * event.DirY + dz * event.DirZ;
            if (dot > bestDot && dist < traceLen) {
                bestDot = dot;
                bestTarget = actor;
            }
        }

        if (bestTarget) {
            // Deal damage via ProcessEvent (TakeDamage)
            float damage = 50.0f; // TODO: weapon-specific
            P2Log("  HIT: %s (dot=%.2f)", bestTarget->GetName().c_str(), bestDot);
            s_P2DamageDealt += damage;

            // Try to call TakeDamage on the target
            // ProcessEvent signature: TakeDamage(int Damage, Pawn InstigatedBy, ...)
            // For now just log — actual PE call needs param buffer construction
            DebugSessionLogf("P2 HIT: %s for %.0f damage", bestTarget->GetName().c_str(), damage);
        }
        break;
    }

    case P2CombatAction::MeleeSwing: {
        P2Log("COMBAT: MeleeSwing");
        // Find nearest actor within 150 units in front
        FVec3 p2Pos;
        if (!GetActorPosition(s_P2Pawn, p2Pos)) break;

        auto nearby = GetActorsInRadius(p2Pos, 150.0f);
        for (auto* actor : nearby) {
            if (actor == s_P2Pawn) continue;
            std::string cn = actor->GetObjClassName();
            if (cn.find("Thug") != std::string::npos ||
                cn.find("Aggressor") != std::string::npos ||
                cn.find("Crawler") != std::string::npos ||
                cn.find("BigDaddy") != std::string::npos) {
                P2Log("  MELEE HIT: %s", actor->GetName().c_str());
                s_P2DamageDealt += 30.0f; // wrench damage
                break;
            }
        }
        break;
    }

    case P2CombatAction::PlasmidCast:
        P2Log("COMBAT: PlasmidCast id=%d", event.WeaponId);
        // TODO: invoke plasmid ability on P2 pawn
        break;

    case P2CombatAction::TakeDamage:
        P2Log("COMBAT: P2 took %.0f damage", event.Amount);
        s_P2DamageTaken += event.Amount;
        // TODO: reduce P2 health, check death
        break;

    case P2CombatAction::Death:
        P2Log("COMBAT: P2 DIED");
        s_P2Deaths++;
        P2Kill();
        break;

    case P2CombatAction::Respawn:
        P2Log("COMBAT: P2 RESPAWN");
        P2Respawn();
        break;

    default:
        break;
    }
}

// ─── Client: Capture Input ───────────────────────────────────────────────

void P2CaptureAndSendInput(float deltaTime)
{
    if (!IsTrueClient()) return;

    s_InputSendAccum += deltaTime;
    if (s_InputSendAccum < 1.0f / 60.0f) return; // 60Hz input
    s_InputSendAccum = 0;

    P2InputData input{};

    // Read keyboard state
    if (GetAsyncKeyState('W') & 0x8000) input.moveForward += 1.0f;
    if (GetAsyncKeyState('S') & 0x8000) input.moveForward -= 1.0f;
    if (GetAsyncKeyState('D') & 0x8000) input.moveRight += 1.0f;
    if (GetAsyncKeyState('A') & 0x8000) input.moveRight -= 1.0f;

    if (GetAsyncKeyState(VK_SPACE) & 0x8000) input.buttonMask |= P2Button::Jump;
    if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) input.buttonMask |= P2Button::Crouch;
    if (GetAsyncKeyState('E') & 0x8000) input.buttonMask |= P2Button::Use;
    if (GetAsyncKeyState('R') & 0x8000) input.buttonMask |= P2Button::Reload;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) input.buttonMask |= P2Button::Fire;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) input.buttonMask |= P2Button::AltFire;
    if (GetAsyncKeyState('V') & 0x8000) input.buttonMask |= P2Button::Melee;
    if (GetAsyncKeyState('Q') & 0x8000) input.buttonMask |= P2Button::Plasmid;

    // Look direction — read from local camera state if available
    // For now, use stored state from last P2 state update
    input.lookPitch = s_ClientP2State.RotPitch;
    input.lookYaw = s_ClientP2State.RotYaw;

    // Weapon slot (number keys)
    for (int i = 0; i < 9; i++) {
        if (GetAsyncKeyState('1' + i) & 0x8000) {
            input.weaponSlot = (uint8_t)i;
            break;
        }
    }

    input.sequenceNum = s_InputSeqNum++;

    // Send to host
    NetSendRawPacket((PacketType)TrueCoopPackets::P2Input, &input, sizeof(input));
}

// ─── Client: Apply Remote State ──────────────────────────────────────────

void P2ApplyRemoteState(const P2PawnState& state)
{
    s_ClientP2State = state;
    s_HasCameraOverride = (state.IsAlive != 0);
}

bool P2GetCameraOverride(float& x, float& y, float& z, float& pitch, float& yaw)
{
    if (!s_HasCameraOverride) return false;
    x = s_ClientP2State.PosX;
    y = s_ClientP2State.PosY;
    z = s_ClientP2State.PosZ;
    pitch = s_ClientP2State.RotPitch;
    yaw = s_ClientP2State.RotYaw;
    return true;
}

// ─── Packet Handlers ─────────────────────────────────────────────────────

void HandleP2Input(const P2InputData& data)
{
    if (!IsTrueHost()) return;
    P2ApplyInput(data);
}

void HandleP2State(const P2PawnState& state)
{
    if (!IsTrueClient()) return;
    P2ApplyRemoteState(state);
}

void HandleP2Combat(const P2CombatEvent& event)
{
    if (!IsTrueHost()) return;
    P2ProcessCombat(event);
}

// ─── P2 Death/Respawn ────────────────────────────────────────────────────

void P2Kill()
{
    if (!s_P2Pawn) return;
    P2Log("P2 killed. Moving off-map.");
    // Move pawn far away (simple death)
    FVec3 offMap = {0, 0, -10000};
    SetActorPosition(s_P2Pawn, offMap);
    s_P2State.IsAlive = 0;
}

void P2Respawn()
{
    if (!s_P2Pawn) {
        P2SpawnPawn();
        return;
    }
    // Move to player's position
    FVec3 playerPos;
    if (GetPlayerPosition(playerPos)) {
        playerPos.X += 100; playerPos.Y += 100;
        SetActorPosition(s_P2Pawn, playerPos);
    }
    s_P2State.IsAlive = 1;
    s_P2State.Health = 100.0f;
    P2Log("P2 respawned near player");
}

// ─── Status ──────────────────────────────────────────────────────────────

std::string GetP2Status()
{
    char buf[512];
    if (IsTrueHost()) {
        std::snprintf(buf, sizeof(buf),
            "P2 Pawn: %s (%s)\n"
            "  Alive: %s  Health: %.0f\n"
            "  Pos: (%.0f, %.0f, %.0f)\n"
            "  Kills: %d  Deaths: %d\n"
            "  DmgDealt: %.0f  DmgTaken: %.0f",
            s_P2Pawn ? s_P2PawnName.c_str() : "NONE",
            s_P2Pawn ? "spawned" : "not spawned",
            s_P2State.IsAlive ? "yes" : "no",
            s_P2State.Health,
            s_P2State.PosX, s_P2State.PosY, s_P2State.PosZ,
            s_P2Kills, s_P2Deaths, s_P2DamageDealt, s_P2DamageTaken);
    } else {
        std::snprintf(buf, sizeof(buf),
            "P2 (local)\n"
            "  Camera override: %s\n"
            "  Pos: (%.0f, %.0f, %.0f)\n"
            "  Health: %.0f  EVE: %.0f\n"
            "  Input seq: %u",
            s_HasCameraOverride ? "yes" : "no",
            s_ClientP2State.PosX, s_ClientP2State.PosY, s_ClientP2State.PosZ,
            s_ClientP2State.Health, s_ClientP2State.EVE,
            s_InputSeqNum);
    }
    return buf;
}

void DumpP2State()
{
    std::string filepath = std::string(DEBUG_DIR) + "/p2_state.txt";
    std::ofstream out(filepath);
    out << "=== P2 Pawn State ===\n";
    out << GetP2Status() << "\n";
    out.close();
    LOG_INFO("[P2] State dumped -> {}", filepath);
}

} // namespace bs1sdk
