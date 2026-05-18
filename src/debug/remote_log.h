#pragma once

#include <cstdint>
#include <cstdio>

namespace bs1sdk {

/// Initialize remote log relay system.
/// On the CLIENT: hooks into the log system and sends log messages to the host.
/// On the HOST: opens a file to receive and record client log messages.
void InitRemoteLog();

/// Shutdown the remote log relay.
void ShutdownRemoteLog();

/// Called by net_manager when a RemoteLog packet is received (host side).
void OnRemoteLogReceived(const void* data, uint16_t size);

/// Drain the client-side log ring buffer — sends queued logs to host.
/// Call from CoopTick or NetTick on client side.
void DrainRemoteLogRing();

/// Flush the remote log file (called periodically or on crash).
void FlushRemoteLog();

/// Get the path to the client log file (for crash reports).
const char* GetRemoteLogPath();

} // namespace bs1sdk
