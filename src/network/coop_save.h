#pragma once

namespace bs1sdk {

/// Start sending our save file to the remote peer.
/// Call from console command "sendsave" or automatically on connect.
bool StartSaveTransfer();

/// Called when a SaveTransfer chunk arrives from the remote peer.
void OnSaveChunkReceived(const void* data, int size);

/// Called when a SaveTransferAck arrives.
void OnSaveAckReceived(const void* data, int size);

/// Tick the save transfer (call each frame while transferring).
void TickSaveTransfer();

/// Returns true if a save transfer is in progress.
bool IsSaveTransferActive();

/// Returns progress 0.0 - 1.0
float GetSaveTransferProgress();

} // namespace bs1sdk
