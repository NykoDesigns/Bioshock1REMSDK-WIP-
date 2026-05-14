#pragma once

#include <string>
#include <functional>

namespace bs1sdk {

/// Initialize co-op bridge (hooks ProcessEvent for state reading).
bool InitCoopBridge();

/// Shutdown co-op bridge.
void ShutdownCoopBridge();

/// Host a co-op session.
bool CoopHost(uint16_t port = 27015, const std::string& name = "Host");

/// Join a co-op session.
bool CoopJoin(const std::string& ip, uint16_t port = 27015, const std::string& name = "Client");

/// Disconnect from co-op session.
void CoopDisconnect();

/// Called every frame from the main tick.
void CoopTick(float deltaTime);

/// Get status string for console display.
std::string GetCoopStatus();

/// Is co-op active?
bool IsCoopActive();

/// Send a chat message to co-op partner.
void NetSendChat(const std::string& message);

/// Register a callback for when chat messages are received.
/// The callback receives (senderName, message).
using ChatDisplayFunc = std::function<void(const std::string&, const std::string&)>;
void SetCoopChatCallback(ChatDisplayFunc fn);

} // namespace bs1sdk
