#include "viewport.h"
#include <cstring>
#include <cmath>

// We include GL after SDL sets up the context
#ifdef _WIN32
#include <Windows.h>
#endif
#include <GL/gl.h>

// Minimal OpenGL 3.3 function loading (we only need a handful)
// In production you'd use glad/glew, but for bootstrapping:
#include "gl_funcs.h"

static const char* vertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec3 vWorldPos;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vUV = aUV;
}
)";

static const char* fragSrc = R"(
#version 330 core
uniform vec3 uColor;
uniform int uLit;
uniform int uHasTexture;
uniform sampler2D uTexture;
uniform float uClipMinZ;
uniform float uClipMaxZ;
in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vUV;
out vec4 FragColor;
void main() {
    // Section clip: discard fragments outside Z range
    if (vWorldPos.z < uClipMinZ || vWorldPos.z > uClipMaxZ) discard;

    if (uLit == 0) {
        FragColor = vec4(uColor, 1.0);
        return;
    }
    // Two-sided lighting with warm/cool tint
    vec3 N = normalize(vNormal);
    vec3 lightDir1 = normalize(vec3(0.4, -0.7, 0.5));
    vec3 lightDir2 = normalize(vec3(-0.3, 0.2, -0.6));
    float diff1 = abs(dot(N, lightDir1));
    float diff2 = abs(dot(N, lightDir2)) * 0.3;
    float ambient = 0.25;
    
    vec3 baseColor = uColor;
    if (uHasTexture != 0) {
        baseColor = texture(uTexture, vUV).rgb;
    }
    vec3 color = baseColor * (ambient + diff1 * 0.65 + diff2);
    FragColor = vec4(color, 1.0);
}
)";

static unsigned int CompileShader(const char* src, unsigned int type)
{
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        printf("[Shader Error] %s\n", log);
    }
    return s;
}

bool Viewport::Init()
{
    // Load GL functions
    if (!LoadGLFunctions()) return false;

    // Compile shaders
    unsigned int vs = CompileShader(vertSrc, GL_VERTEX_SHADER);
    unsigned int fs = CompileShader(fragSrc, GL_FRAGMENT_SHADER);
    m_ShaderProgram = glCreateProgram();
    glAttachShader(m_ShaderProgram, vs);
    glAttachShader(m_ShaderProgram, fs);
    glLinkProgram(m_ShaderProgram);
    GLint linkOk = 0;
    glGetProgramiv(m_ShaderProgram, GL_LINK_STATUS, &linkOk);
    if (!linkOk) {
        char log[512];
        glGetProgramInfoLog(m_ShaderProgram, 512, nullptr, log);
        printf("[Link Error] %s\n", log);
    } else {
        printf("[Viewport] Shader compiled OK (program=%u)\n", m_ShaderProgram);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    m_LocMVP = glGetUniformLocation(m_ShaderProgram, "uMVP");
    m_LocColor = glGetUniformLocation(m_ShaderProgram, "uColor");
    m_LocModel = glGetUniformLocation(m_ShaderProgram, "uModel");
    m_LocLit = glGetUniformLocation(m_ShaderProgram, "uLit");
    m_LocTexSampler = glGetUniformLocation(m_ShaderProgram, "uTexture");
    m_LocHasTexture = glGetUniformLocation(m_ShaderProgram, "uHasTexture");
    m_LocTexScale = glGetUniformLocation(m_ShaderProgram, "uTexScale");
    m_LocClipMinZ = glGetUniformLocation(m_ShaderProgram, "uClipMinZ");
    m_LocClipMaxZ = glGetUniformLocation(m_ShaderProgram, "uClipMaxZ");

    // Unit cube VAO (for actor boxes)
    float cubeVerts[] = {
        // 12 triangles = 36 verts (positions only)
        -1,-1,-1, -1,-1,1, -1,1,1,   -1,-1,-1, -1,1,1, -1,1,-1,
        1,-1,-1, 1,1,1, 1,-1,1,      1,-1,-1, 1,1,-1, 1,1,1,
        -1,-1,-1, 1,-1,1, -1,-1,1,   -1,-1,-1, 1,-1,-1, 1,-1,1,
        -1,1,-1, -1,1,1, 1,1,1,      -1,1,-1, 1,1,1, 1,1,-1,
        -1,-1,-1, -1,1,-1, 1,1,-1,   -1,-1,-1, 1,1,-1, 1,-1,-1,
        -1,-1,1, 1,1,1, -1,1,1,      -1,-1,1, 1,-1,1, 1,1,1,
    };

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVerts), cubeVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    BuildGridMesh();
    return true;
}

void Viewport::Shutdown()
{
    if (m_ShaderProgram) glDeleteProgram(m_ShaderProgram);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_GridVAO) glDeleteVertexArrays(1, &m_GridVAO);
    if (m_GridVBO) glDeleteBuffers(1, &m_GridVBO);
}

void Viewport::BuildGridMesh()
{
    // Ground grid: 100x100, 1000 units apart
    std::vector<float> verts;
    float halfSize = 50000.0f;
    float step = 1000.0f;
    for (float x = -halfSize; x <= halfSize; x += step) {
        verts.push_back(x); verts.push_back(-halfSize); verts.push_back(0);
        verts.push_back(x); verts.push_back(halfSize); verts.push_back(0);
    }
    for (float y = -halfSize; y <= halfSize; y += step) {
        verts.push_back(-halfSize); verts.push_back(y); verts.push_back(0);
        verts.push_back(halfSize); verts.push_back(y); verts.push_back(0);
    }
    m_GridVertCount = (int)(verts.size() / 3);

    glGenVertexArrays(1, &m_GridVAO);
    glGenBuffers(1, &m_GridVBO);
    glBindVertexArray(m_GridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_GridVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
}

static Mat4 MulMat4(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            r.m[j*4+i] = 0;
            for (int k = 0; k < 4; k++)
                r.m[j*4+i] += a.m[k*4+i] * b.m[j*4+k];
        }
    return r;
}

static Mat4 TranslateMat4(float x, float y, float z)
{
    Mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = 1; m.m[5] = 1; m.m[10] = 1; m.m[15] = 1;
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

static Mat4 ScaleMat4(float s)
{
    Mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = s; m.m[5] = s; m.m[10] = s; m.m[15] = 1;
    return m;
}

static Mat4 ScaleMat4xyz(float sx, float sy, float sz)
{
    Mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = sx; m.m[5] = sy; m.m[10] = sz; m.m[15] = 1;
    return m;
}

static Mat4 RotateYMat4(float deg)
{
    float r = deg * 3.14159265f / 180.0f;
    float c = cosf(r), s = sinf(r);
    Mat4 m; memset(&m, 0, sizeof(m));
    m.m[0] = c;  m.m[2] = s;
    m.m[5] = 1;
    m.m[8] = -s; m.m[10] = c;
    m.m[15] = 1;
    return m;
}

static Mat4 RotateXMat4(float deg)
{
    float r = deg * 3.14159265f / 180.0f;
    float c = cosf(r), s = sinf(r);
    Mat4 m; memset(&m, 0, sizeof(m));
    m.m[0] = 1;
    m.m[5] = c;  m.m[6] = s;
    m.m[9] = -s; m.m[10] = c;
    m.m[15] = 1;
    return m;
}

static Mat4 RotateZMat4(float deg)
{
    float r = deg * 3.14159265f / 180.0f;
    float c = cosf(r), s = sinf(r);
    Mat4 m; memset(&m, 0, sizeof(m));
    m.m[0] = c;  m.m[1] = s;
    m.m[4] = -s; m.m[5] = c;
    m.m[10] = 1;
    m.m[15] = 1;
    return m;
}

// Build full actor transform: Translate * RotateYaw * RotatePitch * RotateRoll * Scale
static Mat4 BuildActorTransform(const Vec3& loc, const Vec3& rot, const Vec3& scale)
{
    Mat4 T = TranslateMat4(loc.x, loc.y, loc.z);
    Mat4 S = ScaleMat4xyz(scale.x, scale.y, scale.z);
    // UE2: Yaw=rot.y around Z, Pitch=rot.x around Y, Roll=rot.z around X
    Mat4 Ry = RotateZMat4(rot.y);  // Yaw rotates around world Z (up)
    Mat4 Rp = RotateYMat4(rot.x);  // Pitch rotates around Y (right)
    Mat4 Rr = RotateXMat4(rot.z);  // Roll rotates around X (forward)
    // Combine: T * Yaw * Pitch * Roll * Scale
    Mat4 R = MulMat4(Ry, MulMat4(Rp, Rr));
    return MulMat4(T, MulMat4(R, S));
}

void Viewport::Render(BSMDocument& doc, int selectedActor)
{
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(m_ShaderProgram);

    // Set clip plane uniforms
    if (m_ClipEnabled) {
        glUniform1f(m_LocClipMinZ, m_ClipMinZ);
        glUniform1f(m_LocClipMaxZ, m_ClipMaxZ);
    } else {
        glUniform1f(m_LocClipMinZ, -1e9f);
        glUniform1f(m_LocClipMaxZ, 1e9f);
    }

    // Get viewport size from GL
    int vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float aspect = (vp[2] > 0 && vp[3] > 0) ? (float)vp[2] / (float)vp[3] : 1.0f;

    Mat4 view = m_Camera.GetViewMatrix();
    Mat4 proj = m_Camera.GetProjectionMatrix(aspect);
    Mat4 vp_mat = MulMat4(proj, view);

    // Identity model matrix for grid/unlit things
    Mat4 identity;
    memset(&identity, 0, sizeof(identity));
    identity.m[0] = 1; identity.m[5] = 1; identity.m[10] = 1; identity.m[15] = 1;
    
    // Draw grid (unlit)
    DrawGrid();
    glUniform1i(m_LocLit, 0);
    glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, identity.m);
    glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, vp_mat.m);
    glUniform3f(m_LocColor, 0.2f, 0.2f, 0.25f);
    glBindVertexArray(m_GridVAO);
    glDrawArrays(GL_LINES, 0, m_GridVertCount);

    // Draw BSP geometry (level shell - walls, floors, ceilings)
    if (!m_BSPGPU.empty()) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW); // BSP winding confirmed CCW
        glUniform1i(m_LocLit, 1);
        glUniform1i(m_LocTexSampler, 0);
        glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, vp_mat.m);
        glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, identity.m);

        // Distance-based culling: only draw BSP chunks near the camera
        Vec3 camPos = m_Camera.GetPosition();
        float radiusSq = m_DrawRadius * m_DrawRadius;

        for (auto& gpu : m_BSPGPU) {
            if (gpu.vao == 0 || gpu.indexCount == 0) continue;
            if (!gpu.textureId) continue; // skip untextured BSP (water cubemaps etc.)

            // Distance cull: skip chunks whose center is too far from camera
            if (m_DrawRadiusEnabled) {
                float dx = gpu.centerX - camPos.x;
                float dy = gpu.centerY - camPos.y;
                float dz = gpu.centerZ - camPos.z;
                if (dx*dx + dy*dy + dz*dz > radiusSq) continue;
            }

            // Zone visibility: check if camera's zone bit is set in chunk's ZoneMask
            if (m_ZoneFilterEnabled && m_CameraZone >= 0 && m_CameraZone < 128) {
                int byteIdx = m_CameraZone / 8;
                int bitIdx = m_CameraZone % 8;
                if (!(gpu.zoneMask[byteIdx] & (1 << bitIdx))) continue;
            }

            glBindVertexArray(gpu.vao);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gpu.textureId);
            glUniform1i(m_LocHasTexture, 1);
            glUniform3f(m_LocColor, 1.0f, 1.0f, 1.0f);
            glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT, nullptr);
        }
    }

    // Draw actors - solid lit meshes like a real level editor
    if (doc.IsLoaded()) {
        auto& actors = doc.GetActors();
        
        // Set texture sampler to unit 0
        glUniform1i(m_LocTexSampler, 0);
        
        // Render all meshes as solid with diffuse lighting
        glUniform1i(m_LocLit, 1);
        for (int i = 0; i < (int)actors.size(); i++) {
            auto& a = actors[i];
            if (!a.visible) continue;
            if (a.meshIndex < 0 || a.meshIndex >= (int)m_MeshGPU.size()) continue;
            auto& gpu = m_MeshGPU[a.meshIndex];
            if (gpu.vao == 0 || gpu.indexCount == 0) continue;

            bool isSel = (i == selectedActor);
            
            Mat4 model = BuildActorTransform(a.location, a.rotation, a.scale);
            Mat4 mvp = MulMat4(vp_mat, model);
            glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, model.m);
            glBindVertexArray(gpu.vao);

            // Bind texture if available
            if (gpu.textureId && !isSel) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, gpu.textureId);
                glUniform1i(m_LocHasTexture, 1);
                glUniform3f(m_LocColor, 1.0f, 1.0f, 1.0f);
            } else {
                glUniform1i(m_LocHasTexture, 0);
                if (isSel) {
                    glUniform3f(m_LocColor, 0.9f, 0.5f, 0.1f);
                } else {
                    glUniform3f(m_LocColor, 0.55f, 0.55f, 0.6f);
                }
            }
            glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT, nullptr);
            
            // Wireframe overlay for selected actor
            if (isSel) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glUniform1i(m_LocLit, 0);
                glUniform3f(m_LocColor, 1.0f, 0.8f, 0.0f);
                glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT, nullptr);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glUniform1i(m_LocLit, 1);
            }
        }
        
        // Render non-mesh actors as small colored points (lights only)
        glUniform1i(m_LocLit, 0);
        glPointSize(5.0f);
        for (int i = 0; i < (int)actors.size(); i++) {
            auto& a = actors[i];
            if (!a.visible) continue;
            if (a.meshIndex >= 0 && a.meshIndex < (int)m_MeshGPU.size() &&
                m_MeshGPU[a.meshIndex].vao != 0) continue;
            
            bool isSel = (i == selectedActor);
            ActorCategory cat = CategorizeActor(a.className);
            if (!isSel && cat != ActorCategory::Light) continue;

            Vec3 color = CategoryColor(cat);
            if (isSel) color = {1.0f, 1.0f, 0.0f};
            
            Mat4 model = TranslateMat4(a.location.x, a.location.y, a.location.z);
            Mat4 mvp = MulMat4(vp_mat, model);
            glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, model.m);
            glUniform3f(m_LocColor, color.x, color.y, color.z);
            glBindVertexArray(m_VAO);
            glDrawArrays(GL_POINTS, 0, 1);
        }
    }
}

void Viewport::UploadMeshes(const std::vector<ParsedMesh>& meshes, const std::string& textureDir)
{
    ClearMeshes();
    m_TextureCache.Clear();
    if (!textureDir.empty())
        m_TextureCache.SetTextureDir(textureDir);
    m_MeshGPU.resize(meshes.size());

    for (int i = 0; i < (int)meshes.size(); i++) {
        auto& mesh = meshes[i];
        auto& gpu = m_MeshGPU[i];
        if (mesh.vertices.empty()) continue;

        glGenVertexArrays(1, &gpu.vao);
        glGenBuffers(1, &gpu.vbo);

        glBindVertexArray(gpu.vao);

        // Vertex buffer (position + normal + UV interleaved)
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(MeshVertex),
                     mesh.vertices.data(), GL_STATIC_DRAW);
        // Position: location 0, offset 0
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)0);
        glEnableVertexAttribArray(0);
        // Normal: location 1, offset 12 bytes (after 3 floats)
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // UV: location 2, offset 24 bytes (after 6 floats)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        gpu.vertCount = (int)mesh.vertices.size();
        
        // Load texture for this mesh
        gpu.textureId = m_TextureCache.GetTexture(mesh.textureName);

        // Index buffer (if triangles available)
        if (!mesh.triangles.empty()) {
            glGenBuffers(1, &gpu.ibo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.triangles.size() * sizeof(MeshTriangle),
                         mesh.triangles.data(), GL_STATIC_DRAW);
            gpu.indexCount = (int)mesh.triangles.size() * 3;
        }
    }
    int texturedCount = 0;
    int untexturedLogged = 0;
    for (int i = 0; i < (int)m_MeshGPU.size(); i++) {
        if (m_MeshGPU[i].textureId) { texturedCount++; }
        else if (untexturedLogged < 15 && !meshes[i].textureName.empty()) {
            printf("[Viewport] No texture for: '%s' (mesh: %s)\n", meshes[i].textureName.c_str(), meshes[i].name.c_str());
            untexturedLogged++;
        }
    }
    printf("[Viewport] Uploaded %d meshes to GPU (%d textured, %d untextured)\n", (int)meshes.size(), texturedCount, (int)meshes.size() - texturedCount);
}

void Viewport::UploadBSP(const std::vector<ParsedMesh>& bspMeshes, const std::string& textureDir)
{
    // Clear old BSP data
    for (auto& gpu : m_BSPGPU) {
        if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
        if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
        if (gpu.ibo) glDeleteBuffers(1, &gpu.ibo);
    }
    m_BSPGPU.clear();

    int bspTextured = 0;
    for (auto& mesh : bspMeshes) {
        if (mesh.vertices.empty() || mesh.triangles.empty()) continue;

        MeshGPU gpu = {};

        // Load texture first to get dimensions for UV normalization
        int texW = 256, texH = 256;
        if (!mesh.textureName.empty()) {
            LoadedTexture texInfo = m_TextureCache.GetTextureInfo(mesh.textureName);
            gpu.textureId = texInfo.glTexture;
            if (gpu.textureId) {
                texW = texInfo.width > 0 ? texInfo.width : 256;
                texH = texInfo.height > 0 ? texInfo.height : 256;
                gpu.texWidth = texW;
                gpu.texHeight = texH;
                bspTextured++;
            }
        }

        // Normalize BSP UVs from texels to [0,1] on CPU (avoids GPU float precision loss)
        std::vector<MeshVertex> verts = mesh.vertices; // copy to modify UVs
        float invW = 1.0f / (float)texW;
        float invH = 1.0f / (float)texH;
        for (auto& v : verts) {
            v.u *= invW;
            v.v *= invH;
        }

        glGenVertexArrays(1, &gpu.vao);
        glGenBuffers(1, &gpu.vbo);

        glBindVertexArray(gpu.vao);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(MeshVertex),
                     verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        gpu.vertCount = (int)verts.size();

        glGenBuffers(1, &gpu.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.triangles.size() * sizeof(MeshTriangle),
                     mesh.triangles.data(), GL_STATIC_DRAW);
        gpu.indexCount = (int)mesh.triangles.size() * 3;

        gpu.centerX = (mesh.boundsMin.x + mesh.boundsMax.x) * 0.5f;
        gpu.centerY = (mesh.boundsMin.y + mesh.boundsMax.y) * 0.5f;
        gpu.centerZ = (mesh.boundsMin.z + mesh.boundsMax.z) * 0.5f;
        memcpy(gpu.zoneMask, mesh.zoneMask, 16);
        m_BSPGPU.push_back(gpu);
    }
    printf("[Viewport] Uploaded %d BSP chunks to GPU (%d textured)\n", (int)m_BSPGPU.size(), bspTextured);
    fflush(stdout);

    // Write BSP texture log to file for GUI debugging
    FILE* logF = fopen("bsp_texture_log.txt", "w");
    if (logF) {
        fprintf(logF, "BSP chunks: %d, textured: %d\n", (int)m_BSPGPU.size(), bspTextured);
        for (int i = 0; i < (int)m_BSPGPU.size() && i < (int)bspMeshes.size(); i++) {
            fprintf(logF, "  [%d] tex='%s' glId=%u %dx%d verts=%d\n", i,
                    bspMeshes[i].textureName.c_str(), m_BSPGPU[i].textureId,
                    m_BSPGPU[i].texWidth, m_BSPGPU[i].texHeight,
                    m_BSPGPU[i].vertCount);
        }
        // Show UV ranges for first 5 textured chunks (after CPU normalization)
        int shown = 0;
        for (int ci = 0; ci < (int)bspMeshes.size() && shown < 5; ci++) {
            if (m_BSPGPU.size() <= (size_t)ci) break;
            if (!m_BSPGPU[ci].textureId) continue;
            float uMin=1e9f, uMax=-1e9f, vMin=1e9f, vMax=-1e9f;
            int tw = m_BSPGPU[ci].texWidth, th = m_BSPGPU[ci].texHeight;
            float invW = 1.0f/(float)tw, invH = 1.0f/(float)th;
            for (auto& v : bspMeshes[ci].vertices) {
                float u = v.u * invW, vv = v.v * invH;
                if (u < uMin) uMin = u; if (u > uMax) uMax = u;
                if (vv < vMin) vMin = vv; if (vv > vMax) vMax = vv;
            }
            fprintf(logF, "  UV[%d] normalized: u=[%.2f..%.2f] v=[%.2f..%.2f] texSize=%dx%d\n",
                    ci, uMin, uMax, vMin, vMax, tw, th);
            shown++;
        }
        fclose(logF);
    }
}

void Viewport::ClearMeshes()
{
    for (auto& gpu : m_MeshGPU) {
        if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
        if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
        if (gpu.ibo) glDeleteBuffers(1, &gpu.ibo);
    }
    m_MeshGPU.clear();
    for (auto& gpu : m_BSPGPU) {
        if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
        if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
        if (gpu.ibo) glDeleteBuffers(1, &gpu.ibo);
    }
    m_BSPGPU.clear();
}

void Viewport::HandleInput(float dt)
{
    // Input is handled by the App class and forwarded to camera
}

void Viewport::DrawGrid()
{
    // Already drawn in Render
}

int Viewport::PickActor(const BSMDocument& doc, float mouseX, float mouseY, float viewW, float viewH)
{
    if (!doc.IsLoaded()) return -1;

    float aspect = viewW / viewH;
    Mat4 view = m_Camera.GetViewMatrix();
    Mat4 proj = m_Camera.GetProjectionMatrix(aspect);
    Mat4 vp_mat = MulMat4(proj, view);

    // Convert mouse to NDC
    float ndcX = (mouseX / viewW) * 2.0f - 1.0f;
    float ndcY = 1.0f - (mouseY / viewH) * 2.0f;

    // Simple distance-based picking: find actor whose screen-space projection
    // is closest to the mouse
    float bestDist = 30.0f; // pixel threshold
    int bestIdx = -1;

    for (int i = 0; i < (int)doc.GetActors().size(); i++) {
        auto& a = doc.GetActors()[i];
        if (!a.visible) continue;

        // Project actor position to clip space
        float px = vp_mat.m[0]*a.location.x + vp_mat.m[4]*a.location.y + vp_mat.m[8]*a.location.z + vp_mat.m[12];
        float py = vp_mat.m[1]*a.location.x + vp_mat.m[5]*a.location.y + vp_mat.m[9]*a.location.z + vp_mat.m[13];
        float pw = vp_mat.m[3]*a.location.x + vp_mat.m[7]*a.location.y + vp_mat.m[11]*a.location.z + vp_mat.m[15];

        if (pw <= 0.01f) continue; // behind camera

        float sx = (px / pw);
        float sy = (py / pw);

        float dx = (sx - ndcX) * viewW * 0.5f;
        float dy = (sy - ndcY) * viewH * 0.5f;
        float dist = sqrtf(dx*dx + dy*dy);

        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }
    return bestIdx;
}
