#pragma once
#include "net_common.h"

namespace bs1sdk {

/// Initialize economy sync (hooks AddADAM, AddCredits for shared economy).
bool InitEconomySync();

/// Shutdown economy sync.
void ShutdownEconomySync();

/// Tick economy sync — periodic broadcast of local ADAM/Credits.
void TickEconomySync(float deltaTime);

/// Queue an incoming economy sync packet.
void QueueEconomySyncPacket(const struct EconomySyncData& data);

} // namespace bs1sdk
