#pragma once

#include "camera.h"
#include "bsm_document.h"
#include "texture_loader.h"
#include <vector>

// 3D viewport renderer using OpenGL
class Viewport {
public:
    bool Init();
    void Shutdown();

    // Render the 3D scene into an ImGui window
    void Render(BSMDocument& doc, int selectedActor);

    // Handle input (called within ImGui window context)
    void HandleInput(float dt);

    // Pick actor under mouse
    int PickActor(const BSMDocument& doc, float mouseX, float mouseY, float viewW, float viewH);

    Camera& GetCamera() { return m_Camera; }

    // Upload parsed meshes to GPU (call after document loads)
    void UploadMeshes(const std::vector<ParsedMesh>& meshes, const std::string& textureDir = "");
    void UploadBSP(const std::vector<ParsedMesh>& bspMeshes, const std::string& textureDir = "");
    void ClearMeshes();

private:
    Camera m_Camera;
    unsigned int m_ShaderProgram = 0;
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    unsigned int m_GridVAO = 0;
    unsigned int m_GridVBO = 0;
    int m_GridVertCount = 0;

    // Shader uniforms
    int m_LocMVP = -1;
    int m_LocColor = -1;
    int m_LocModel = -1;
    int m_LocLit = -1;

    // Per-mesh GPU data
    struct MeshGPU {
        unsigned int vao = 0;
        unsigned int vbo = 0;
        unsigned int ibo = 0;
        unsigned int textureId = 0;
        int indexCount = 0;
        int vertCount = 0;
        int texWidth = 256;
        int texHeight = 256;
        float centerX = 0, centerY = 0, centerZ = 0; // bounding box center
        uint8_t zoneMask[16] = {}; // 128-bit zone visibility bitmask
    };
    std::vector<MeshGPU> m_MeshGPU;
    std::vector<MeshGPU> m_BSPGPU; // BSP geometry chunks
    TextureCache m_TextureCache;
    int m_LocTexSampler = -1;
    int m_LocHasTexture = -1;
    int m_LocTexScale = -1;
    int m_LocClipMinZ = -1;
    int m_LocClipMaxZ = -1;

public:
    // Section clip controls (Z height range)
    float m_ClipMinZ = -500000.0f;
    float m_ClipMaxZ = 500000.0f;
    bool m_ClipEnabled = false;
    // Distance-based BSP culling
    float m_DrawRadius = 15000.0f; // Show BSP within this radius of camera
    bool m_DrawRadiusEnabled = true;
    // Zone-based BSP visibility
    bool m_ZoneFilterEnabled = false;
    int m_CameraZone = -1; // current camera zone (updated each frame)
private:

    void DrawGrid();
    void DrawActorBox(const Vec3& pos, const Vec3& color, float size, bool selected);
    void BuildGridMesh();
};
