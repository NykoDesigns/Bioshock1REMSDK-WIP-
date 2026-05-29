#pragma once

#include "mesh_parser.h"
#include "texture_loader.h"
#include "gl_funcs.h"
#include <vector>
#include <unordered_map>
#include <string>

// Renders mesh thumbnails to offscreen FBOs for the content browser.
// Uses a single shared FBO and copies results to cached GL textures.
// Generates thumbnails progressively (N per frame) to avoid stalls.
class ThumbnailRenderer {
public:
    bool Init(int thumbSize = 128);
    void Shutdown();

    // Set the mesh list to generate thumbnails for.
    // Call after meshes are loaded (parsed from BSM or glTF).
    void SetMeshes(const std::vector<ParsedMesh>& meshes);

    // Generate up to N thumbnails per call (progressive).
    // Returns true if all thumbnails are generated.
    bool GenerateBatch(int batchSize = 4);

    // Get the GL texture ID for a given mesh index (0 if not yet generated)
    GLuint GetThumbnail(int meshIndex) const;

    // Get thumbnail by mesh name (0 if not found)
    GLuint GetThumbnailByName(const std::string& name) const;

    // How many are done vs total
    int GetGeneratedCount() const { return m_GeneratedCount; }
    int GetTotalCount() const { return (int)m_Meshes.size(); }

    int GetThumbSize() const { return m_ThumbSize; }

    // Set texture cache for textured previews (optional — falls back to solid color)
    void SetTextureCache(TextureCache* cache) { m_TexCache = cache; }
    void SetTextureDir(const std::string& dir) { m_TexDir = dir; }

private:
    TextureCache* m_TexCache = nullptr;
    std::string m_TexDir;
    void RenderMeshToFBO(const ParsedMesh& mesh, GLuint targetTex);
    GLuint CompileShader(const char* vertSrc, const char* fragSrc);

    int m_ThumbSize = 128;
    GLuint m_FBO = 0;
    GLuint m_DepthRBO = 0;
    GLuint m_TempColorTex = 0; // temp render target (copied to final)
    GLuint m_Shader = 0;
    int m_LocMVP = -1;
    int m_LocLightDir = -1;
    int m_LocBaseColor = -1;
    int m_LocTexSampler = -1;
    int m_LocHasTexture = -1;

    // Per-mesh: temporary VAO/VBO/IBO for rendering (reused)
    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_IBO = 0;

    // Source data
    std::vector<const ParsedMesh*> m_Meshes;
    std::unordered_map<std::string, int> m_NameToIndex;

    // Generated textures
    std::vector<GLuint> m_Thumbnails;
    int m_GeneratedCount = 0;
    int m_NextToGenerate = 0;
};
