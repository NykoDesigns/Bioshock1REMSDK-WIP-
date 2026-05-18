#include "coop_render.h"
#include "coop_puppet.h"
#include "net_manager.h"
#include "../core/log.h"

#include <imgui.h>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static PlayerStateData s_RemoteState{};
static PlayerStateData s_InterpState{};   // smoothed state for rendering
static char s_RemoteName[32] = "Partner";
static bool s_HasRemoteState = false;
static float s_LastUpdateTime = 0.0f;     // when we last got a real update
static float s_InterpAlpha = 0.0f;        // interpolation progress

// Camera state
static float s_CamX = 0, s_CamY = 0, s_CamZ = 0;
static float s_CamPitch = 0, s_CamYaw = 0;
static float s_CamFov = 90.0f;
static bool  s_HasCamera = false;

// Screen dimensions (updated from ImGui IO)
static float s_ScreenW = 1920.0f;
static float s_ScreenH = 1080.0f;

// ─── Math helpers ──────────────────────────────────────────────────────

static constexpr float PI = 3.14159265358979323846f;
static constexpr float DEG2RAD = PI / 180.0f;

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

static Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float Len(Vec3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

/// World-to-screen projection using camera parameters.
/// Returns true if the point is in front of the camera, fills screenPos.
static bool WorldToScreen(Vec3 worldPos, Vec2& screenPos)
{
    if (!s_HasCamera) return false;

    // Build camera basis vectors from pitch/yaw (UE2 convention: yaw=Z axis, pitch=Y axis)
    float cp = cosf(s_CamPitch * DEG2RAD);
    float sp = sinf(s_CamPitch * DEG2RAD);
    float cy = cosf(s_CamYaw * DEG2RAD);
    float sy = sinf(s_CamYaw * DEG2RAD);

    // Forward vector (X axis in UE2)
    Vec3 forward = { cp * cy, cp * sy, sp };
    // Right vector (Y axis in UE2)
    Vec3 right = { sy, -cy, 0.0f };
    // Up vector (Z axis in UE2)
    Vec3 up = { -sp * cy, -sp * sy, cp };

    // Vector from camera to target
    Vec3 cam = { s_CamX, s_CamY, s_CamZ };
    Vec3 delta = Sub(worldPos, cam);

    // Project onto camera axes
    float fwd = Dot(delta, forward);
    float rgt = Dot(delta, right);
    float upd = Dot(delta, up);

    // Behind camera check
    if (fwd < 1.0f) return false;

    // Perspective projection
    float tanHalfFov = tanf((s_CamFov * 0.5f) * DEG2RAD);
    float aspect = s_ScreenW / s_ScreenH;

    float screenX = (rgt / (fwd * tanHalfFov * aspect)) * 0.5f + 0.5f;
    float screenY = (-upd / (fwd * tanHalfFov)) * 0.5f + 0.5f;

    screenPos.x = screenX * s_ScreenW;
    screenPos.y = screenY * s_ScreenH;

    return true;
}

// ─── Public API ────────────────────────────────────────────────────────

void InitCoopRender()
{
    s_HasRemoteState = false;
    s_HasCamera = false;
    LOG_INFO("[Co-op Render] Initialized");
}

void SetRemoteRenderState(const PlayerStateData& state, const char* name)
{
    // Store previous interpolated position as new start point
    if (s_HasRemoteState) {
        s_InterpState = s_RemoteState; // keep old target as interp start
    } else {
        // First update — snap directly
        s_InterpState.posX = state.posX;
        s_InterpState.posY = state.posY;
        s_InterpState.posZ = state.posZ;
    }
    s_RemoteState = state;
    if (name) strncpy(s_RemoteName, name, sizeof(s_RemoteName) - 1);
    s_HasRemoteState = true;
    s_InterpAlpha = 0.0f; // reset interpolation on new data
}

void SetLocalCamera(float camX, float camY, float camZ,
                    float camPitch, float camYaw, float fovDeg)
{
    s_CamX = camX; s_CamY = camY; s_CamZ = camZ;
    s_CamPitch = camPitch; s_CamYaw = camYaw;
    s_CamFov = fovDeg > 10.0f ? fovDeg : 90.0f;
    s_HasCamera = true;
}

static float Lerp(float a, float b, float t) { return a + (b - a) * t; }

void RenderCoopOverlay()
{
    // Periodic diagnostic log (every 5 seconds)
    static float s_DiagTimer = 0;
    static bool s_PrevHasRemote = false;
    s_DiagTimer += 1.0f / 60.0f; // approximate frame time
    if (s_DiagTimer >= 5.0f || s_HasRemoteState != s_PrevHasRemote) {
        s_DiagTimer = 0;
        s_PrevHasRemote = s_HasRemoteState;
        LOG_INFO("[Co-op Render] hasRemote={} hasCam={} camPos=({:.0f},{:.0f},{:.0f}) connected={}",
                 s_HasRemoteState, s_HasCamera, s_CamX, s_CamY, s_CamZ, IsNetConnected());
        if (s_HasRemoteState) {
            LOG_INFO("[Co-op Render] Remote: ({:.0f},{:.0f},{:.0f}) name={}",
                     s_RemoteState.posX, s_RemoteState.posY, s_RemoteState.posZ, s_RemoteName);
        }
    }
    if (!s_HasRemoteState || !s_HasCamera) return;

    // Update screen size from ImGui
    ImGuiIO& io = ImGui::GetIO();
    s_ScreenW = io.DisplaySize.x;
    s_ScreenH = io.DisplaySize.y;

    // ─── Interpolation ───
    // Smoothly lerp from last known position to new position
    // At 30Hz updates, we interpolate over ~33ms per update
    s_InterpAlpha += io.DeltaTime * 15.0f; // reach target in ~66ms (smooth at 60fps)
    if (s_InterpAlpha > 1.0f) s_InterpAlpha = 1.0f;

    float t = s_InterpAlpha;
    // Smooth step for nicer easing
    t = t * t * (3.0f - 2.0f * t);

    float rx = Lerp(s_InterpState.posX, s_RemoteState.posX, t);
    float ry = Lerp(s_InterpState.posY, s_RemoteState.posY, t);
    float rz = Lerp(s_InterpState.posZ, s_RemoteState.posZ, t);

    Vec3 remotePos = { rx, ry, rz };
    Vec3 cam = { s_CamX, s_CamY, s_CamZ };

    // Distance to remote player
    float dist = Len(Sub(remotePos, cam));

    // Project to screen
    Vec2 screen;
    bool onScreen = WorldToScreen(remotePos, screen);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // ─── Edge indicator when off-screen ───
    if (!onScreen || screen.x < 0 || screen.x > s_ScreenW || screen.y < 0 || screen.y > s_ScreenH) {
        // Show an arrow at the edge of the screen pointing toward the player
        Vec3 delta = Sub(remotePos, cam);
        float angle = atan2f(delta.y, delta.x) - s_CamYaw * DEG2RAD;
        float edgeX = s_ScreenW * 0.5f + cosf(angle) * (s_ScreenW * 0.45f);
        float edgeY = s_ScreenH * 0.5f + sinf(angle) * (s_ScreenH * 0.40f);
        edgeX = fmaxf(30.0f, fminf(s_ScreenW - 30.0f, edgeX));
        edgeY = fmaxf(30.0f, fminf(s_ScreenH - 30.0f, edgeY));

        // Arrow triangle pointing outward
        float arrowSize = 10.0f;
        dl->AddTriangleFilled(
            ImVec2(edgeX + cosf(angle) * arrowSize, edgeY + sinf(angle) * arrowSize),
            ImVec2(edgeX + cosf(angle + 2.3f) * arrowSize, edgeY + sinf(angle + 2.3f) * arrowSize),
            ImVec2(edgeX + cosf(angle - 2.3f) * arrowSize, edgeY + sinf(angle - 2.3f) * arrowSize),
            IM_COL32(0, 200, 255, 180)
        );

        // Distance label near arrow
        char distLabel[32];
        snprintf(distLabel, sizeof(distLabel), "%.0fm", dist / 100.0f);
        ImVec2 ts = ImGui::CalcTextSize(distLabel);
        dl->AddText(ImVec2(edgeX - ts.x * 0.5f, edgeY + 12), IM_COL32(0, 200, 255, 200), distLabel);
        return;
    }

    // Scale marker by distance (closer = bigger)
    float scale = 1.0f;
    if (dist > 500.0f) scale = 500.0f / dist;
    if (scale < 0.3f) scale = 0.3f;
    if (scale > 2.0f) scale = 2.0f;

    float markerSize = 14.0f * scale;

    // ─── Draw diamond marker with glow ───
    ImVec2 center(screen.x, screen.y);

    // Outer glow
    ImU32 colGlow = IM_COL32(0, 180, 255, 50);
    for (int i = 3; i >= 1; i--) {
        float gs = markerSize + i * 3.0f;
        dl->AddQuadFilled(
            ImVec2(center.x, center.y - gs),
            ImVec2(center.x + gs * 0.7f, center.y),
            ImVec2(center.x, center.y + gs),
            ImVec2(center.x - gs * 0.7f, center.y),
            colGlow
        );
    }

    // Filled diamond
    ImU32 colInner = IM_COL32(0, 160, 255, 160);
    dl->AddQuadFilled(
        ImVec2(center.x, center.y - markerSize),
        ImVec2(center.x + markerSize * 0.7f, center.y),
        ImVec2(center.x, center.y + markerSize),
        ImVec2(center.x - markerSize * 0.7f, center.y),
        colInner
    );

    // Outline
    ImU32 colOuter = IM_COL32(0, 220, 255, 240);
    dl->AddQuad(
        ImVec2(center.x, center.y - markerSize),
        ImVec2(center.x + markerSize * 0.7f, center.y),
        ImVec2(center.x, center.y + markerSize),
        ImVec2(center.x - markerSize * 0.7f, center.y),
        colOuter, 2.0f
    );

    // ─── Vertical line from marker to floor (depth cue) ───
    float lineLen = 30.0f * scale;
    dl->AddLine(
        ImVec2(center.x, center.y + markerSize),
        ImVec2(center.x, center.y + markerSize + lineLen),
        IM_COL32(0, 180, 255, 60), 1.0f
    );

    // ─── Name tag + distance ───
    char label[64];
    snprintf(label, sizeof(label), "%s  %.0fm", s_RemoteName, dist / 100.0f);
    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 textPos(center.x - textSize.x * 0.5f, center.y - markerSize - textSize.y - 6);

    // Background pill
    dl->AddRectFilled(
        ImVec2(textPos.x - 6, textPos.y - 2),
        ImVec2(textPos.x + textSize.x + 6, textPos.y + textSize.y + 2),
        IM_COL32(0, 0, 0, 180), 6.0f
    );
    // Cyan border on pill
    dl->AddRect(
        ImVec2(textPos.x - 6, textPos.y - 2),
        ImVec2(textPos.x + textSize.x + 6, textPos.y + textSize.y + 2),
        IM_COL32(0, 180, 255, 100), 6.0f, 0, 1.0f
    );
    dl->AddText(textPos, IM_COL32(255, 255, 255, 240), label);

    // ─── Health bar ───
    float barW = 60.0f * scale;
    if (barW < 30.0f) barW = 30.0f;
    float barH = 4.0f;
    float healthPct = s_RemoteState.health / 200.0f; // BioShock max HP is 200
    if (healthPct > 1.0f) healthPct = 1.0f;
    if (healthPct < 0.0f) healthPct = 0.0f;

    ImVec2 barStart(center.x - barW * 0.5f, center.y + markerSize + 4);
    ImVec2 barEnd(barStart.x + barW, barStart.y + barH);

    // Background
    dl->AddRectFilled(barStart, barEnd, IM_COL32(0, 0, 0, 160), 2.0f);

    // Health fill — gradient from green to yellow to red
    ImU32 hpCol;
    if (healthPct > 0.6f)      hpCol = IM_COL32(40, 220, 80, 230);   // green
    else if (healthPct > 0.3f) hpCol = IM_COL32(220, 200, 40, 230);  // yellow
    else                       hpCol = IM_COL32(220, 50, 30, 230);   // red

    dl->AddRectFilled(barStart, ImVec2(barStart.x + barW * healthPct, barEnd.y), hpCol, 2.0f);
    dl->AddRect(barStart, barEnd, IM_COL32(200, 200, 200, 80), 2.0f);

    // ─── Action effects ───
    float actionTime;
    int actionType = GetPuppetLastAction(actionTime);
    if (actionType >= 0 && actionTime < 0.5f) {
        float fade = 1.0f - (actionTime / 0.5f); // fade out over 0.5s
        uint8_t alpha = (uint8_t)(fade * 220);

        switch (actionType) {
        case 0: { // MeleeSwing — draw arc sweep
            float arcRadius = 30.0f * scale;
            float startAngle = -0.8f + actionTime * 6.0f; // sweep animation
            for (int seg = 0; seg < 8; seg++) {
                float a1 = startAngle + seg * 0.2f;
                float a2 = startAngle + (seg + 1) * 0.2f;
                dl->AddLine(
                    ImVec2(center.x + cosf(a1) * arcRadius, center.y + sinf(a1) * arcRadius),
                    ImVec2(center.x + cosf(a2) * arcRadius, center.y + sinf(a2) * arcRadius),
                    IM_COL32(255, 200, 50, alpha), 3.0f * fade
                );
            }
            break;
        }
        case 1: { // WeaponFire — muzzle flash burst
            float burstSize = 20.0f * scale * (1.0f - actionTime * 2.0f);
            if (burstSize > 2.0f) {
                ImVec2 muzzle(center.x, center.y - markerSize * 0.5f);
                dl->AddCircleFilled(muzzle, burstSize, IM_COL32(255, 220, 100, alpha));
                dl->AddCircleFilled(muzzle, burstSize * 0.5f, IM_COL32(255, 255, 200, alpha));
            }
            break;
        }
        case 2: { // PlasmidCast — blue/electric glow
            float glowSize = 25.0f * scale * fade;
            dl->AddCircleFilled(center, glowSize, IM_COL32(50, 150, 255, (uint8_t)(alpha * 0.4f)));
            dl->AddCircle(center, glowSize, IM_COL32(100, 200, 255, alpha), 12, 2.0f);
            dl->AddCircle(center, glowSize * 1.5f, IM_COL32(80, 180, 255, (uint8_t)(alpha * 0.3f)), 12, 1.0f);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace bs1sdk
