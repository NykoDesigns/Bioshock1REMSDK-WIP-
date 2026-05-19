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
    void UploadBSP(const std::vector<ParsedMesh>& bspMeshes);
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
    };
    std::vector<MeshGPU> m_MeshGPU;
    std::vector<MeshGPU> m_BSPGPU; // BSP geometry chunks
    TextureCache m_TextureCache;
    int m_LocTexSampler = -1;
    int m_LocHasTexture = -1;

    void DrawGrid();
    void DrawActorBox(const Vec3& pos, const Vec3& color, float size, bool selected);
    void BuildGridMesh();
};
