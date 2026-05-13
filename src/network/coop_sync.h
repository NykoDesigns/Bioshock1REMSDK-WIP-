#pragma once

#include <string>

namespace bs1sdk {

/// Initialize co-op damage and world state synchronization.
/// Hooks ProcessEvent for TakeDamage, doors, triggers, pickups.
bool InitCoopSync();

/// Shutdown sync hooks.
void ShutdownCoopSync();

/// Process incoming damage/world packets (called from NetTick).
void CoopSyncProcessPackets();

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
void QueueDamagePacket(const DamageData& dmg);
void QueueWorldEventPacket(const WorldEventData& evt);

} // namespace bs1sdk
