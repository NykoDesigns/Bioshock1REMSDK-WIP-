#pragma once

#include "coop_true.h"
#include "../engine/uobject.h"
#include <string>
#include <cstdint>

namespace bs1sdk {

// ─── Player 2 Pawn System ────────────────────────────────────────────────
// Phase 3: Spawn a P2 pawn on host, driven by client inputs.
// Phase 4: P2 weapon fire, plasmid use, damage attribution.
//
// Strategy:
// - On host: find/commandeer an NPC (DecoyHuman or ShockAI pawn) as P2 body
// - Apply P2InputData to move/aim/fire this pawn
// - Broadcast P2 pawn state back to client
// - On client: override local camera to P2 position

// ─── P2 Button Mask Bits ─────────────────────────────────────────────────

namespace P2Button {
    constexpr uint16_t Fire     = 0x0001;
    constexpr uint16_t AltFire  = 0x0002;
    constexpr uint16_t Jump     = 0x0004;
    constexpr uint16_t Use      = 0x0008;
    constexpr uint16_t Crouch   = 0x0010;
    constexpr uint16_t Reload   = 0x0020;
    constexpr uint16_t Melee    = 0x0040;
    constexpr uint16_t Plasmid  = 0x0080;
}

// ─── P2 State (host -> client) ───────────────────────────────────────────

struct P2PawnState {
    float PosX, PosY, PosZ;
    float RotPitch, RotYaw;
    float Health;
    float EVE;
    uint8_t WeaponId;
    uint8_t IsAlive;
    uint8_t IsCrouching;
    uint8_t _pad;
    uint32_t FrameNum;
};

// ─── P2 Combat Event ─────────────────────────────────────────────────────

enum class P2CombatAction : uint8_t {
    WeaponFire = 0,
    PlasmidCast = 1,
    MeleeSwing = 2,
    Reload = 3,
    WeaponSwitch = 4,
    TakeDamage = 5,
    Death = 6,
    Respawn = 7,
};

struct P2CombatEvent {
    P2CombatAction Action;
    uint8_t WeaponId;
    uint8_t _pad[2];
    float DirX, DirY, DirZ;    // aim direction
    float Amount;               // damage amount (for TakeDamage)
    uint32_t TargetHash;        // target actor name hash (for damage)
};

// ─── Public API ──────────────────────────────────────────────────────────

/// Initialize P2 system
void InitP2System();
void ShutdownP2System();

/// Host: spawn or commandeer a pawn for P2
bool P2SpawnPawn();
bool P2DestroyPawn();
bool P2IsSpawned();
UObject* P2GetPawn();

/// Host: apply input from client to P2 pawn
void P2ApplyInput(const P2InputData& input);

/// Host: get current P2 state to broadcast
P2PawnState P2GetState();

/// Host: process combat action from client
void P2ProcessCombat(const P2CombatEvent& event);

/// Client: capture local input and send to host
void P2CaptureAndSendInput(float deltaTime);

/// Client: receive and apply P2 state (camera override)
void P2ApplyRemoteState(const P2PawnState& state);

/// Client: get camera override position/rotation
bool P2GetCameraOverride(float& x, float& y, float& z, float& pitch, float& yaw);

/// Handle incoming P2 packets
void HandleP2Input(const P2InputData& data);
void HandleP2State(const P2PawnState& state);
void HandleP2Combat(const P2CombatEvent& event);

/// Get P2 status string
std::string GetP2Status();

/// Dump P2 debug info
void DumpP2State();

/// P2 death/respawn
void P2Kill();
void P2Respawn();

} // namespace bs1sdk
