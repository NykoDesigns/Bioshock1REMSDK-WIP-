#pragma once

#include "net_common.h"

namespace bs1sdk {

/// Initialize co-op rendering (3D markers for remote player).
void InitCoopRender();

/// Render remote player marker in the ImGui overlay.
/// Call this within an ImGui frame (after NewFrame, before Render).
void RenderCoopOverlay();

/// Update the remote player state for rendering.
void SetRemoteRenderState(const PlayerStateData& state, const char* name);

/// Set the local camera for world-to-screen projection.
/// Called every frame from the ProcessEvent tick.
void SetLocalCamera(float camX, float camY, float camZ,
                    float camPitch, float camYaw, float fovDeg);

} // namespace bs1sdk
