#pragma once

#include "camera.h"
#include "bsm_document.h"
#include "texture_loader.h"
#include <vector>

// Gizmo modes (W=Translate, E=Rotate, R=Scale)
enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoAxis { None, X, Y, Z };

// Viewport rendering modes (cycle with F2, like UnrealEd's Alt+1-4)
enum class ViewMode {
    Lit,           // Textured + directional lighting (default)
    Unlit,         // Textured, full brightness, no shading
    Wireframe,     // Wireframe overlay on flat-shaded geometry
    LitNoTexture,  // Lit but no textures (shows geometry clearly)
    SurfaceID,     // Unique color per BSP material chunk (debug)
    ZoneColor,     // Color by zone index (debug)
    LightmapScale, // Heat map by lightmap texel density (debug)
    COUNT
};

// Scene point light (extracted from BSM Light actors)
struct SceneLight {
    float pos[3] = {0,0,0};
    float color[3] = {1,1,1};
    float radius = 1000.0f;
    float brightness = 1.0f;
};

// All adjustable lighting parameters (shown in Lighting panel)
struct LightingSettings {
    // Directional light (sun/key light) — bright, like UE viewport
    float sunYaw = 45.0f;
    float sunPitch = -35.0f;
    float sunColor[3] = {1.0f, 0.95f, 0.9f};
    float sunIntensity = 0.8f;
    bool sunEnabled = true;

    // Fill light (softer, opposite side)
    float fillColor[3] = {0.4f, 0.45f, 0.6f};
    float fillIntensity = 0.3f;

    // Ambient — visible baseline so nothing is pure black
    float ambientSkyColor[3] = {0.18f, 0.20f, 0.28f};
    float ambientGroundColor[3] = {0.12f, 0.13f, 0.16f};
    float ambientIntensity = 1.0f;

    // Fog — light blue-green depth fog for underwater Rapture atmosphere
    bool fogEnabled = true;
    float fogColor[3] = {0.01f, 0.025f, 0.04f};
    float fogStart = 2000.0f;
    float fogEnd = 15000.0f;

    // Tone mapping & exposure
    float exposure = 1.2f;
    float gamma = 2.2f;
    bool toneMapping = true;

    // Point lights from level
    bool useLevelLights = true;
    float pointLightMultiplier = 1.8f;
};

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
    TextureCache& GetTextureCache() { return m_TextureCache; }

    // Upload parsed meshes to GPU (call after document loads)
    void UploadMeshes(const std::vector<ParsedMesh>& meshes, const std::string& textureDir = "");
    void UploadBSP(const std::vector<ParsedMesh>& bspMeshes, const std::string& textureDir = "");
    void ClearMeshes();
    // Re-link untextured meshes after bulk textures become available in cache
    int RelinkMeshTextures(const std::vector<ParsedMesh>& meshes);

    // Query mesh GPU state (for diagnostics)
    bool HasMeshTexture(int meshIndex) const {
        if (meshIndex < 0 || meshIndex >= (int)m_MeshGPU.size()) return false;
        return m_MeshGPU[meshIndex].textureId != 0;
    }

private:
    Camera m_Camera;
    unsigned int m_ShaderProgram = 0;
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    unsigned int m_GridVAO = 0;
    unsigned int m_GridVBO = 0;
    int m_GridVertCount = 0;
    int m_AxisVertStart = 0; // vertex offset where axis lines begin in grid VBO
    float m_Time = 0.0f; // accumulated time for animations
    // Light icon diamond mesh (octahedron, position-only)
    unsigned int m_DiamondVAO = 0;
    unsigned int m_DiamondVBO = 0;
    int m_DiamondVertCount = 0;

    // Billboard actor icons (procedurally generated textures)
    unsigned int m_BillboardVAO = 0;
    unsigned int m_BillboardVBO = 0;
    unsigned int m_IconTextures[10] = {}; // 0-8 per ActorCategory, 9=glow radial
    unsigned int m_IconShader = 0;
    int m_LocIconMVP = -1;
    int m_LocIconTex = -1;
    int m_LocIconColor = -1;
    int m_LocIconSelected = -1;
    void GenerateIconTextures();
    void BuildBillboardQuad();
    void RenderActorIcons(const std::vector<EditorActor>& actors, int selectedActor,
                          const Mat4& view, const Mat4& proj, const Vec3& camPos, float radiusSq);

    // Shader uniforms — basic
    int m_LocMVP = -1;
    int m_LocColor = -1;
    int m_LocModel = -1;
    int m_LocLit = -1;

    // Shader uniforms — lighting
    int m_LocCamPos = -1;
    int m_LocSunDir = -1;
    int m_LocSunColor = -1;
    int m_LocFillDir = -1;
    int m_LocFillColor = -1;
    int m_LocAmbientSky = -1;
    int m_LocAmbientGround = -1;
    int m_LocFogColor = -1;
    int m_LocFogParams = -1; // x=start, y=end, z=enabled
    int m_LocExposureGamma = -1; // x=exposure, y=gamma, z=toneMapping
    int m_LocNumPointLights = -1;
    static const int MAX_POINT_LIGHTS = 128;
    int m_LocPLightPosR[MAX_POINT_LIGHTS]; // vec4: xyz=pos, w=radius
    int m_LocPLightColor[MAX_POINT_LIGHTS]; // vec3: rgb * brightness
    int m_LocEmissive = -1; // emissive multiplier for self-lit meshes
    int m_LocNormalMap = -1; // normal map sampler (texture unit 2)
    int m_LocHasNormalMap = -1; // whether normal map is bound
    int m_LocSpecMap = -1; // specular map sampler (texture unit 3)
    int m_LocHasSpecMap = -1; // whether specular map is bound
    int m_LocLightMap = -1; // lightmap sampler (texture unit 4)
    int m_LocHasLightMap = -1; // whether lightmap is bound
    int m_LocAlpha = -1;     // transparency (1.0 = opaque, <1 = water)

    // Per-mesh GPU data
    struct MeshGPU {
        unsigned int vao = 0;
        unsigned int vbo = 0;
        unsigned int ibo = 0;
        unsigned int textureId = 0;
        unsigned int normalMapId = 0; // normal map texture
        unsigned int specularMapId = 0; // specular/gloss map
        int indexCount = 0;
        int vertCount = 0;
        int texWidth = 256;
        int texHeight = 256;
        float centerX = 0, centerY = 0, centerZ = 0; // bounding box center
        uint8_t zoneMask[16] = {}; // 128-bit zone visibility bitmask
        bool isEmissive = false; // self-illuminating (neon signs, lamps, light beams)
        bool hasAlpha = false;   // texture has alpha channel (needs alpha test/blend)
        bool isTwoSided = false; // FacingShader - disable backface culling
        bool isWater = false;    // water surface (transparent + animated)
        float lightMapScale = 0.0f; // BSP lightmap texel density (debug viz)
        unsigned int lightMapId = 0; // BSP lightmap atlas texture ID
        uint8_t zoneIndex = 0;      // BSP zone index (debug viz)
    };
    std::vector<MeshGPU> m_MeshGPU;
    std::vector<MeshGPU> m_BSPGPU; // BSP geometry chunks
    TextureCache m_TextureCache;
    unsigned int m_DefaultTexture = 0; // UE-style gray checkerboard for untextured meshes
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
    float m_DrawRadius = 50000.0f; // Show BSP within this radius of camera
    bool m_DrawRadiusEnabled = false; // disabled by default: show everything
    // Zone-based BSP visibility
    bool m_ZoneFilterEnabled = false; // disabled by default: show all zones
    bool m_WireframeBSP = false; // F1 key: wireframe BSP rendering
    ViewMode m_ViewMode = ViewMode::Lit; // F2 cycles through modes
    int m_CameraZone = -1; // current camera zone (updated each frame)
    LightingSettings m_Lighting; // all lighting controls
    std::vector<SceneLight> m_SceneLights; // extracted from BSM Light actors
    bool m_ShowLightIcons = true; // render light actor diamonds
    bool m_ShowActorIcons = true; // render billboard icons for ALL non-mesh actors
    bool m_GamePreview = false;   // P key: game preview mode (hide editor helpers)
    bool m_ShowUnknownActors = false; // show unclassified placeholder actors in game preview
    bool m_DarkPreview = false;   // BioShock dark preview preset
    bool m_MissingMaterialDebug = false; // render unresolved textures as magenta
    bool m_ShowDecals = true;     // render decal/projector actors
    bool m_Orthographic = false; // T key: toggle orthographic/perspective
    float m_OrthoSize = 20000.0f; // orthographic view size

    // Post-processing effects
    bool m_FXAAEnabled = true;         // Anti-aliasing
    bool m_VignetteEnabled = true;     // Darkened edges
    float m_VignetteStrength = 1.5f;   // Vignette intensity
    bool m_FilmGrainEnabled = false;   // Film grain noise
    float m_FilmGrainStrength = 0.03f; // Grain intensity
    bool m_SSAOEnabled = true;         // Screen-space ambient occlusion
    float m_SSAORadius = 50.0f;        // Sample radius in world units
    float m_SSAOIntensity = 1.0f;      // AO darkening strength
    bool m_GodRaysEnabled = false;     // Volumetric light scattering
    float m_GodRayDensity = 1.0f;      // Ray length
    float m_GodRayWeight = 0.01f;      // Per-sample weight
    float m_GodRayDecay = 0.97f;       // Falloff per sample
    float m_GodRayIntensity = 1.0f;    // Overall god ray brightness
    bool m_WaterEnabled = true;        // Render water surfaces

    // Bloom post-process
    bool m_BloomEnabled = false;
    float m_BloomThreshold = 1.0f;
    float m_BloomIntensity = 0.4f;
    unsigned int m_SceneFBO = 0;
    unsigned int m_SceneColorTex = 0;
    unsigned int m_SceneDepthTex = 0; // depth as texture for SSAO
    unsigned int m_BloomFBO[2] = {};
    unsigned int m_BloomTex[2] = {};
    unsigned int m_BloomShader = 0;
    unsigned int m_CompositeShader = 0;
    unsigned int m_FullscreenVAO = 0;
    unsigned int m_FullscreenVBO = 0;
    int m_SceneFBOWidth = 0, m_SceneFBOHeight = 0;
    float m_LastVP[16] = {}; // cached VP matrix for god ray sun projection
    void InitBloom();
    void ResizeBloomFBOs(int w, int h);
    void RenderBloom();

    // Draw statistics (populated each frame)
    int m_DrawCalls = 0;
    int m_TriCount = 0;

    // Shadow mapping
    unsigned int m_ShadowFBO = 0;
    unsigned int m_ShadowDepthTex = 0;
    int m_ShadowMapSize = 2048;
    bool m_ShadowEnabled = false;
    Mat4 m_ShadowMat = {}; // light VP * bias
    unsigned int m_DepthShader = 0;
    int m_LocDepthMVP = -1;
    int m_LocShadowMat = -1;
    int m_LocShadowMap = -1;
    int m_LocShadowEnabled = -1;

    // Gizmo state (public so App can access for dragging)
    GizmoMode m_GizmoMode = GizmoMode::Translate;
    GizmoAxis m_GizmoHoverAxis = GizmoAxis::None;
    GizmoAxis m_GizmoActiveAxis = GizmoAxis::None;
    bool m_GizmoDragging = false;
    Vec3 m_GizmoDragStartPos = {0,0,0}; // actor pos when drag started
    float m_GizmoDragStartT = 0.0f;     // axis parameter at drag start
    bool m_GizmoVisible = true;

    // Gizmo methods
    void DrawGizmo(const Vec3& actorPos, const Mat4& vp);
    GizmoAxis HitTestGizmo(const Vec3& actorPos, float mx, float my, float vw, float vh, const Mat4& vp);
    bool BeginGizmoDrag(const Vec3& actorPos, float mx, float my, float vw, float vh, const Mat4& vp);
    Vec3 UpdateGizmoDrag(const Vec3& actorPos, float mx, float my, float vw, float vh, const Mat4& vp);
    void EndGizmoDrag();

private:

    void DrawGrid();
    void DrawActorBox(const Vec3& pos, const Vec3& color, float size, bool selected);
    void BuildGridMesh();
    void BuildDiamondMesh();
    void BuildGizmoMesh();

    // Gizmo geometry
    unsigned int m_GizmoVAO = 0;
    unsigned int m_GizmoVBO = 0;
};
