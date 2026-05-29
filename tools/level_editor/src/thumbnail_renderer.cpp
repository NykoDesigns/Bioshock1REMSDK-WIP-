#include "thumbnail_renderer.h"
#include "camera.h"
#include <cmath>
#include <cstdio>
#include <algorithm>

#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif

// Vertex shader: transform + pass normal + UVs
static const char* s_ThumbVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
out vec3 vNormal;
out vec3 vPos;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = aNormal;
    vPos = aPos;
    vUV = aUV;
}
)";

// Fragment shader: directional light + ambient + optional texture
static const char* s_ThumbFrag = R"(
#version 330 core
in vec3 vNormal;
in vec3 vPos;
in vec2 vUV;
uniform vec3 uLightDir;
uniform vec3 uBaseColor;
uniform sampler2D uTexture;
uniform int uHasTexture;
out vec4 FragColor;
void main() {
    vec3 N = normalize(vNormal);
    float NdL = max(dot(N, uLightDir), 0.0);
    // Hemisphere ambient: warm from above, cool from below
    float upness = N.y * 0.5 + 0.5;
    vec3 ambient = mix(vec3(0.10, 0.10, 0.14), vec3(0.25, 0.27, 0.32), upness);
    // Base color: texture or solid
    vec3 baseColor = uBaseColor;
    if (uHasTexture != 0) {
        baseColor = texture(uTexture, vUV).rgb;
    }
    vec3 diffuse = baseColor * NdL * 0.75;
    // Rim light for depth
    vec3 viewDir = normalize(-vPos);
    float rim = pow(1.0 - max(dot(N, viewDir), 0.0), 3.0) * 0.12;
    vec3 color = ambient * baseColor + diffuse + vec3(rim);
    // Subtle tone mapping
    color = color / (color + vec3(1.0));
    FragColor = vec4(color, 1.0);
}
)";

// Background gradient shader
static const char* s_BgVert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.999, 1.0);
    vUV = aPos * 0.5 + 0.5;
}
)";

static const char* s_BgFrag = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
void main() {
    // Dark gradient background (like UE4 asset thumbnails)
    vec3 top = vec3(0.18, 0.20, 0.24);
    vec3 bot = vec3(0.08, 0.09, 0.10);
    vec3 color = mix(bot, top, vUV.y);
    FragColor = vec4(color, 1.0);
}
)";

GLuint ThumbnailRenderer::CompileShader(const char* vertSrc, const char* fragSrc)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertSrc, nullptr);
    glCompileShader(vs);
    GLint ok; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(vs, 512, nullptr, log);
        printf("[Thumbnail] VS error: %s\n", log);
        glDeleteShader(vs);
        return 0;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragSrc, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(fs, 512, nullptr, log);
        printf("[Thumbnail] FS error: %s\n", log);
        glDeleteShader(vs); glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(prog, 512, nullptr, log);
        printf("[Thumbnail] Link error: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

bool ThumbnailRenderer::Init(int thumbSize)
{
    m_ThumbSize = thumbSize;

    // Compile mesh shader
    m_Shader = CompileShader(s_ThumbVert, s_ThumbFrag);
    if (!m_Shader) return false;
    m_LocMVP = glGetUniformLocation(m_Shader, "uMVP");
    m_LocLightDir = glGetUniformLocation(m_Shader, "uLightDir");
    m_LocBaseColor = glGetUniformLocation(m_Shader, "uBaseColor");
    m_LocTexSampler = glGetUniformLocation(m_Shader, "uTexture");
    m_LocHasTexture = glGetUniformLocation(m_Shader, "uHasTexture");

    // Create FBO
    glGenFramebuffers(1, &m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);

    // Color texture (reused for each render, then copied out)
    glGenTextures(1, &m_TempColorTex);
    glBindTexture(GL_TEXTURE_2D, m_TempColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_ThumbSize, m_ThumbSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_TempColorTex, 0);

    // Depth renderbuffer
    glGenRenderbuffers(1, &m_DepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_DepthRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_ThumbSize, m_ThumbSize);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_DepthRBO);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("[Thumbnail] FBO incomplete!\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Create shared VAO/VBO/IBO for mesh uploads
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_IBO);

    printf("[Thumbnail] Renderer initialized (%dx%d)\n", m_ThumbSize, m_ThumbSize);
    return true;
}

void ThumbnailRenderer::Shutdown()
{
    for (GLuint tex : m_Thumbnails) {
        if (tex) glDeleteTextures(1, &tex);
    }
    m_Thumbnails.clear();
    if (m_FBO) { glDeleteFramebuffers(1, &m_FBO); m_FBO = 0; }
    if (m_DepthRBO) { glDeleteRenderbuffers(1, &m_DepthRBO); m_DepthRBO = 0; }
    if (m_TempColorTex) { glDeleteTextures(1, &m_TempColorTex); m_TempColorTex = 0; }
    if (m_Shader) { glDeleteProgram(m_Shader); m_Shader = 0; }
    if (m_VAO) { glDeleteVertexArrays(1, &m_VAO); m_VAO = 0; }
    if (m_VBO) { glDeleteBuffers(1, &m_VBO); m_VBO = 0; }
    if (m_IBO) { glDeleteBuffers(1, &m_IBO); m_IBO = 0; }
}

void ThumbnailRenderer::SetMeshes(const std::vector<ParsedMesh>& meshes)
{
    // Clean old thumbnails
    for (GLuint tex : m_Thumbnails) {
        if (tex) glDeleteTextures(1, &tex);
    }
    m_Thumbnails.clear();
    m_Meshes.clear();
    m_NameToIndex.clear();
    m_GeneratedCount = 0;
    m_NextToGenerate = 0;

    // Only include valid non-BSP meshes
    for (int i = 0; i < (int)meshes.size(); i++) {
        if (!meshes[i].valid) continue;
        if (meshes[i].name == "__BSP__") continue;
        if (meshes[i].vertices.empty() || meshes[i].triangles.empty()) continue;
        m_NameToIndex[meshes[i].name] = (int)m_Meshes.size();
        m_Meshes.push_back(&meshes[i]);
    }

    m_Thumbnails.resize(m_Meshes.size(), 0);
    printf("[Thumbnail] %d meshes queued for thumbnail generation\n", (int)m_Meshes.size());
}

bool ThumbnailRenderer::GenerateBatch(int batchSize)
{
    if (m_NextToGenerate >= (int)m_Meshes.size()) return true; // all done
    if (!m_FBO || !m_Shader) return true;

    int end = (std::min)(m_NextToGenerate + batchSize, (int)m_Meshes.size());
    for (int i = m_NextToGenerate; i < end; i++) {
        // Allocate output texture
        GLuint outTex;
        glGenTextures(1, &outTex);
        glBindTexture(GL_TEXTURE_2D, outTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_ThumbSize, m_ThumbSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        RenderMeshToFBO(*m_Meshes[i], outTex);
        m_Thumbnails[i] = outTex;
        m_GeneratedCount++;
    }
    m_NextToGenerate = end;
    return m_NextToGenerate >= (int)m_Meshes.size();
}

GLuint ThumbnailRenderer::GetThumbnail(int meshIndex) const
{
    if (meshIndex < 0 || meshIndex >= (int)m_Thumbnails.size()) return 0;
    return m_Thumbnails[meshIndex];
}

GLuint ThumbnailRenderer::GetThumbnailByName(const std::string& name) const
{
    auto it = m_NameToIndex.find(name);
    if (it == m_NameToIndex.end()) return 0;
    return GetThumbnail(it->second);
}

void ThumbnailRenderer::RenderMeshToFBO(const ParsedMesh& mesh, GLuint targetTex)
{
    // Save current viewport
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFBO;
    glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &prevFBO);

    // Bind our FBO with the temp color attachment
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_TempColorTex, 0);
    glViewport(0, 0, m_ThumbSize, m_ThumbSize);

    // Clear with dark gradient background color
    glClearColor(0.12f, 0.13f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    // Compute bounding sphere for camera framing
    float cx = (mesh.boundsMin.x + mesh.boundsMax.x) * 0.5f;
    float cy = (mesh.boundsMin.y + mesh.boundsMax.y) * 0.5f;
    float cz = (mesh.boundsMin.z + mesh.boundsMax.z) * 0.5f;
    float dx = mesh.boundsMax.x - mesh.boundsMin.x;
    float dy = mesh.boundsMax.y - mesh.boundsMin.y;
    float dz = mesh.boundsMax.z - mesh.boundsMin.z;
    float radius = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.5f;
    if (radius < 0.001f) radius = 100.0f;

    // Camera: orbit at 3/4 angle (like UE4 thumbnail view)
    float camDist = radius * 2.2f;
    float yaw = 0.785f;   // 45 degrees
    float pitch = -0.45f;  // slight downward look
    float camX = cx + camDist * std::cos(pitch) * std::sin(yaw);
    float camY = cy + camDist * std::sin(pitch);
    float camZ = cz + camDist * std::cos(pitch) * std::cos(yaw);

    // Build view matrix (look-at)
    float fwdX = cx - camX, fwdY = cy - camY, fwdZ = cz - camZ;
    float fwdLen = std::sqrt(fwdX*fwdX + fwdY*fwdY + fwdZ*fwdZ);
    fwdX /= fwdLen; fwdY /= fwdLen; fwdZ /= fwdLen;

    // up = (0,1,0)
    float rightX = fwdY * 0.0f - fwdZ * 1.0f; // fwd × up (simplified)
    float rightY = fwdZ * 0.0f - fwdX * 0.0f;
    float rightZ = fwdX * 1.0f - fwdY * 0.0f;
    // Actually: right = cross(fwd, worldUp)
    // worldUp = (0, 1, 0)
    rightX = fwdY * 0.0f - fwdZ * 0.0f; // This is wrong, let me use proper cross
    // cross(forward, up) where up=(0,1,0)
    rightX = fwdZ * 0.0f - fwdY * 0.0f; // Nope, let me do it properly
    {
        // right = normalize(cross(fwd, (0,1,0)))
        float upX = 0, upY = 1, upZ = 0;
        rightX = fwdY * upZ - fwdZ * upY;
        rightY = fwdZ * upX - fwdX * upZ;
        rightZ = fwdX * upY - fwdY * upX;
        float rLen = std::sqrt(rightX*rightX + rightY*rightY + rightZ*rightZ);
        if (rLen > 0.001f) { rightX /= rLen; rightY /= rLen; rightZ /= rLen; }
        else { rightX = 1; rightY = 0; rightZ = 0; }
    }
    // up = cross(right, fwd)
    float upX = rightY * fwdZ - rightZ * fwdY;
    float upY = rightZ * fwdX - rightX * fwdZ;
    float upZ = rightX * fwdY - rightY * fwdX;

    // View matrix (column-major for OpenGL)
    float view[16] = {
        rightX, upX, -fwdX, 0,
        rightY, upY, -fwdY, 0,
        rightZ, upZ, -fwdZ, 0,
        -(rightX*camX + rightY*camY + rightZ*camZ),
        -(upX*camX + upY*camY + upZ*camZ),
        -(-fwdX*camX + -fwdY*camY + -fwdZ*camZ),
        1
    };

    // Perspective projection (45 FOV, aspect 1:1)
    float fov = 0.785398f; // 45 degrees
    float nearP = camDist * 0.01f;
    float farP = camDist * 10.0f;
    float f = 1.0f / std::tan(fov * 0.5f);
    float proj[16] = {
        f, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (farP+nearP)/(nearP-farP), -1,
        0, 0, 2*farP*nearP/(nearP-farP), 0
    };

    // MVP = proj * view (column-major multiply)
    float mvp[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            mvp[c*4+r] = 0;
            for (int k = 0; k < 4; k++) {
                mvp[c*4+r] += proj[k*4+r] * view[c*4+k];
            }
        }
    }

    // Upload mesh geometry: position(12) + normal(12) + UV(8) = 32 bytes per vert
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);

    struct ThumbVert { float x, y, z, nx, ny, nz, u, v; };
    std::vector<ThumbVert> verts(mesh.vertices.size());
    for (size_t vi = 0; vi < mesh.vertices.size(); vi++) {
        auto& mv = mesh.vertices[vi];
        verts[vi] = { mv.x, mv.y, mv.z, mv.nx, mv.ny, mv.nz, mv.u, mv.v };
    }
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(ThumbVert), verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_IBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.triangles.size() * sizeof(MeshTriangle), mesh.triangles.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ThumbVert), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(ThumbVert), (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(ThumbVert), (void*)24);

    // Draw
    glUseProgram(m_Shader);
    glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp);
    // Light from upper-right-front
    float lightDir[3] = {0.5f, 0.7f, 0.5f};
    float lLen = std::sqrt(lightDir[0]*lightDir[0] + lightDir[1]*lightDir[1] + lightDir[2]*lightDir[2]);
    lightDir[0] /= lLen; lightDir[1] /= lLen; lightDir[2] /= lLen;
    glUniform3f(m_LocLightDir, lightDir[0], lightDir[1], lightDir[2]);
    // Try to bind the mesh's diffuse texture
    GLuint texId = 0;
    if (m_TexCache && !mesh.textureName.empty()) {
        texId = m_TexCache->GetDiffuse(mesh.textureName);
    }
    if (texId) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texId);
        glUniform1i(m_LocTexSampler, 0);
        glUniform1i(m_LocHasTexture, 1);
        glUniform3f(m_LocBaseColor, 1.0f, 1.0f, 1.0f); // white (texture provides color)
    } else {
        glUniform1i(m_LocHasTexture, 0);
        glUniform3f(m_LocBaseColor, 0.7f, 0.72f, 0.75f); // neutral gray fallback
    }

    glDrawElements(GL_TRIANGLES, (GLsizei)(mesh.triangles.size() * 3), GL_UNSIGNED_SHORT, nullptr);

    // Copy rendered result to the target texture
    glBindTexture(GL_TEXTURE_2D, targetTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, m_ThumbSize, m_ThumbSize, 0);

    // Restore
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glBindVertexArray(0);
    glUseProgram(0);
}
