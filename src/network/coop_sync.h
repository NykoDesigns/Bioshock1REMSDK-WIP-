#pragma once

#include <string>

namespace bs1sdk {

/// Initialize co-op damage and world state synchronization.
/// Hooks ProcessEvent for TakeDamage, Died, doors, triggers, pickups.
bool InitCoopSync();

/// Shutdown sync hooks.
void ShutdownCoopSync();

/// Process incoming packets (called from CoopTick on game thread).
/// deltaTime is used for periodic enemy HP broadcasting.
void CoopSyncProcessPackets(float deltaTime = 0.016f);

/// Send a damage event to remote peer.
void CoopSendDamage(float amount, float hitX, float hitY, float hitZ,
                    float targetX, float targetY, float targetZ,
                    uint32_t targetNameHash, uint8_t damageType);

/// Send a world event to remote peer.
void CoopSendWorldEvent(uint8_t eventType, uint8_t state,
                        float posX, float posY, float posZ,
                        uint32_t actorNameHash);

/// Queue incoming packets for processing on game thread.
struct DamageData;
struct WorldEventData;
struct EnemyDeathData;
struct PlayerDeathData;
struct PlayerRespawnData;
struct TriggerSyncData;
struct EnemyHPSyncData;
struct PlayerActionData;

void QueueDamagePacket(const DamageData& dmg);
void QueueWorldEventPacket(const WorldEventData& evt);
void QueueEnemyDeathPacket(const EnemyDeathData& death);
void QueuePlayerDeathPacket(const PlayerDeathData& death);
void QueuePlayerRespawnPacket(const PlayerRespawnData& respawn);
void QueueTriggerSyncPacket(const TriggerSyncData& trigger);
void QueueEnemyHPSyncPacket(const EnemyHPSyncData& hpSync);
void QueuePlayerActionPacket(const PlayerActionData& action);

} // namespace bs1sdk
