#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Editor Bridge — TCP server inside the running game that receives commands
// from BS1LevelEditor (move actor, select actor, query state).
// Purely additive module: does NOT modify any existing SDK code.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <functional>

namespace bs1sdk {

// ── Protocol ──────────────────────────────────────────────────────────────
// All messages are little-endian. Format:
//   [4B magic 'B1ED'] [2B type] [2B payload_len] [payload...]
//
// Editor → Game:
//   0x01 MOVE_ACTOR    { int32 nameHash, float x,y,z }              (16B)
//   0x02 ROTATE_ACTOR  { int32 nameHash, int32 pitch,yaw,roll }     (16B)
//   0x03 SELECT_ACTOR  { int32 nameHash }                            (4B)
//   0x04 QUERY_ACTORS  { }                                           (0B)
//   0x05 QUERY_PLAYER  { }                                           (0B)
//   0x10 PING          { }                                           (0B)
//
// Game → Editor:
//   0x81 ACTOR_POS     { int32 nameHash, float x,y,z, int32 p,y,r } (28B)
//   0x82 PLAYER_POS    { float x,y,z, int32 p,y,r }                 (24B)
//   0x83 ACTOR_LIST    { int32 count, then count*{nameHash(4)+classHash(4)+pos(12)} } (variable)
//   0x90 PONG          { }                                           (0B)

constexpr uint32_t BRIDGE_MAGIC  = 0x44453142;  // "B1ED" little-endian
constexpr uint16_t BRIDGE_PORT   = 19760;

// Message types (editor → game)
constexpr uint16_t MSG_MOVE_ACTOR   = 0x01;
constexpr uint16_t MSG_ROTATE_ACTOR = 0x02;
constexpr uint16_t MSG_SELECT_ACTOR = 0x03;
constexpr uint16_t MSG_QUERY_ACTORS = 0x04;
constexpr uint16_t MSG_QUERY_PLAYER = 0x05;
constexpr uint16_t MSG_PING         = 0x10;

// Message types (game → editor)
constexpr uint16_t MSG_ACTOR_POS    = 0x81;
constexpr uint16_t MSG_PLAYER_POS   = 0x82;
constexpr uint16_t MSG_ACTOR_LIST   = 0x83;
constexpr uint16_t MSG_PONG         = 0x90;

#pragma pack(push, 1)
struct BridgeHeader {
    uint32_t magic;
    uint16_t type;
    uint16_t payloadLen;
};

struct MoveActorPayload {
    uint32_t nameHash;
    float x, y, z;
};

struct RotateActorPayload {
    uint32_t nameHash;
    int32_t pitch, yaw, roll;
};

struct SelectActorPayload {
    uint32_t nameHash;
};

struct ActorPosPayload {
    uint32_t nameHash;
    float x, y, z;
    int32_t pitch, yaw, roll;
};

struct PlayerPosPayload {
    float x, y, z;
    int32_t pitch, yaw, roll;
};
#pragma pack(pop)

// ── API ───────────────────────────────────────────────────────────────────

/// Start the editor bridge TCP server on localhost:BRIDGE_PORT.
/// Registers a tick callback for processing commands.
/// Returns true if server started successfully.
bool StartEditorBridge();

/// Stop the editor bridge and clean up.
void StopEditorBridge();

/// Check if the bridge is running.
bool IsEditorBridgeRunning();

/// Get number of connected editors.
int GetEditorBridgeClientCount();

/// Get bridge statistics.
struct BridgeStats {
    int messagesReceived = 0;
    int messagesSent = 0;
    int actorMoves = 0;
    int errors = 0;
};
BridgeStats GetEditorBridgeStats();

} // namespace bs1sdk
