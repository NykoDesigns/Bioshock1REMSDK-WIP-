#include "coop_render.h"
#include "../core/log.h"

#include <imgui.h>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace bs1sdk {

// ─── State ─────────────────────────────────────────────────────────────

static PlayerStateData s_RemoteState{};
static char s_RemoteName[32] = "Partner";
static bool s_HasRemoteState = false;

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
    s_RemoteState = state;
    if (name) strncpy(s_RemoteName, name, sizeof(s_RemoteName) - 1);
    s_HasRemoteState = true;
}

void SetLocalCamera(float camX, float camY, float camZ,
                    float camPitch, float camYaw, float fovDeg)
{
    s_CamX = camX; s_CamY = camY; s_CamZ = camZ;
    s_CamPitch = camPitch; s_CamYaw = camYaw;
    s_CamFov = fovDeg > 10.0f ? fovDeg : 90.0f;
    s_HasCamera = true;
}

void RenderCoopOverlay()
{
    if (!s_HasRemoteState || !s_HasCamera) return;

    // Update screen size from ImGui
    ImGuiIO& io = ImGui::GetIO();
    s_ScreenW = io.DisplaySize.x;
    s_ScreenH = io.DisplaySize.y;

    Vec3 remotePos = { s_RemoteState.posX, s_RemoteState.posY, s_RemoteState.posZ };
    Vec3 cam = { s_CamX, s_CamY, s_CamZ };

    // Distance to remote player
    float dist = Len(Sub(remotePos, cam));

    // Project to screen
    Vec2 screen;
    if (!WorldToScreen(remotePos, screen)) return; // behind camera

    // Clamp to screen bounds with margin
    if (screen.x < -50 || screen.x > s_ScreenW + 50) return;
    if (screen.y < -50 || screen.y > s_ScreenH + 50) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Scale marker by distance (closer = bigger)
    float scale = 1.0f;
    if (dist > 500.0f) scale = 500.0f / dist;
    if (scale < 0.3f) scale = 0.3f;

    float markerSize = 12.0f * scale;

    // ─── Draw diamond marker ───
    ImVec2 center(screen.x, screen.y);
    ImU32 colOuter = IM_COL32(0, 200, 255, 220);  // cyan outline
    ImU32 colInner = IM_COL32(0, 150, 255, 120);  // cyan fill

    dl->AddQuadFilled(
        ImVec2(center.x, center.y - markerSize),       // top
        ImVec2(center.x + markerSize * 0.7f, center.y), // right
        ImVec2(center.x, center.y + markerSize),       // bottom
        ImVec2(center.x - markerSize * 0.7f, center.y), // left
        colInner
    );
    dl->AddQuad(
        ImVec2(center.x, center.y - markerSize),
        ImVec2(center.x + markerSize * 0.7f, center.y),
        ImVec2(center.x, center.y + markerSize),
        ImVec2(center.x - markerSize * 0.7f, center.y),
        colOuter, 2.0f
    );

    // ─── Name tag ───
    char label[64];
    snprintf(label, sizeof(label), "%s [%.0fm]", s_RemoteName, dist / 100.0f);
    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 textPos(center.x - textSize.x * 0.5f, center.y - markerSize - textSize.y - 4);

    // Background for readability
    dl->AddRectFilled(
        ImVec2(textPos.x - 3, textPos.y - 1),
        ImVec2(textPos.x + textSize.x + 3, textPos.y + textSize.y + 1),
        IM_COL32(0, 0, 0, 160), 3.0f
    );
    dl->AddText(textPos, IM_COL32(0, 220, 255, 255), label);

    // ─── Health bar ───
    float barW = 50.0f * scale;
    float barH = 5.0f;
    float healthPct = s_RemoteState.health / 100.0f; // assume max 100
    if (healthPct > 1.0f) healthPct = 1.0f;
    if (healthPct < 0.0f) healthPct = 0.0f;

    ImVec2 barStart(center.x - barW * 0.5f, center.y + markerSize + 4);
    ImVec2 barEnd(barStart.x + barW, barStart.y + barH);

    // Background
    dl->AddRectFilled(barStart, barEnd, IM_COL32(40, 40, 40, 180), 2.0f);
    // Health fill (green → red gradient)
    ImU32 hpCol = healthPct > 0.5f ? IM_COL32(0, 220, 80, 220) : IM_COL32(220, 60, 0, 220);
    dl->AddRectFilled(barStart, ImVec2(barStart.x + barW * healthPct, barEnd.y), hpCol, 2.0f);
    dl->AddRect(barStart, barEnd, IM_COL32(200, 200, 200, 120), 2.0f);
}

} // namespace bs1sdk
