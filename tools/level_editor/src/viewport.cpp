#include "viewport.h"
#include <cstring>
#include <cmath>
#include <algorithm>

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
layout(location = 3) in vec4 aTangent;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec3 vWorldPos;
out vec2 vUV;
out vec3 vTangent;
out vec3 vBitangent;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    mat3 normalMat = mat3(uModel);
    vNormal = normalMat * aNormal;
    vTangent = normalMat * aTangent.xyz;
    vBitangent = cross(vNormal, vTangent) * aTangent.w;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vUV = aUV;
}
)";

static const char* fragSrc = R"(
#version 330 core
#define MAX_POINT_LIGHTS 128

// Basic
uniform vec3 uColor;
uniform int uLit;           // 0=flat, 1=lit, 2=unlit textured, 3=lit no-texture, 4=emissive
uniform int uHasTexture;
uniform sampler2D uTexture;
uniform float uClipMinZ;
uniform float uClipMaxZ;
uniform float uEmissive;    // emissive multiplier (0=normal, >0=self-illuminating)
uniform sampler2D uNormalMap;
uniform int uHasNormalMap;  // 1 if normal map bound
uniform sampler2D uSpecMap;
uniform int uHasSpecMap;    // 1 if specular map bound

// Lighting
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uFillDir;
uniform vec3 uFillColor;
uniform vec3 uAmbientSky;
uniform vec3 uAmbientGround;
uniform vec3 uFogColor;
uniform vec3 uFogParams;    // x=start, y=end, z=enabled
uniform vec3 uExposureGamma;// x=exposure, y=gamma, z=toneMapping

// Point lights
uniform int uNumPointLights;
uniform vec4 uPLightPosR[MAX_POINT_LIGHTS]; // xyz=pos, w=radius
uniform vec3 uPLightColor[MAX_POINT_LIGHTS];

// Shadow mapping
uniform int uShadowEnabled;
uniform mat4 uShadowMat;
uniform sampler2D uShadowMap;

// Water/transparency
uniform float uAlpha;

in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vUV;
in vec3 vTangent;
in vec3 vBitangent;
out vec4 FragColor;

// ACES filmic tone mapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

// Post-processing: exposure, tone mapping, gamma
vec3 postProcess(vec3 c) {
    c *= uExposureGamma.x;
    if (uExposureGamma.z > 0.5) c = ACESFilm(c);
    return pow(c, vec3(1.0 / uExposureGamma.y));
}

void main() {
    if (vWorldPos.z < uClipMinZ || vWorldPos.z > uClipMaxZ) discard;

    // Alpha test: discard transparent pixels (vegetation, grates, glass cutouts)
    if (uHasTexture != 0) {
        float alpha = texture(uTexture, vUV).a;
        if (alpha < 0.5) discard;
    }

    // Mode 0: flat color (grid, wireframe, helpers)
    if (uLit == 0) {
        FragColor = vec4(uColor, 1.0);
        return;
    }

    // Mode 4: emissive (light icons, glow) — bypass lighting
    if (uLit == 4) {
        FragColor = vec4(uColor, 1.0);
        return;
    }

    // Mode 2: unlit textured (apply post-processing for consistency)
    if (uLit == 2) {
        vec3 base = uColor;
        if (uHasTexture != 0) base = texture(uTexture, vUV).rgb;
        FragColor = vec4(postProcess(base), 1.0);
        return;
    }

    // --- Full lighting (modes 1 and 3) ---
    vec3 N = normalize(vNormal);
    // Apply normal map if available (tangent-space -> world-space)
    if (uHasNormalMap != 0) {
        vec3 T = normalize(vTangent);
        vec3 B = normalize(vBitangent);
        mat3 TBN = mat3(T, B, N);
        vec3 mapN = texture(uNormalMap, vUV).rgb * 2.0 - 1.0;
        N = normalize(TBN * mapN);
    }
    vec3 V = normalize(uCamPos - vWorldPos);

    // Specular intensity from map (white=shiny, black=matte)
    float specMask = 1.0;
    if (uHasSpecMap != 0) {
        specMask = texture(uSpecMap, vUV).r;
    }

    // Hemisphere ambient: blend sky/ground based on normal Z
    float hemi = N.z * 0.5 + 0.5;
    vec3 ambient = mix(uAmbientGround, uAmbientSky, hemi);

    // Directional sun: two-sided wrap (front=full, back=25% — shows depth in interiors)
    float NdotSun = dot(N, uSunDir);
    float diffSun = max(NdotSun, 0.0) + max(-NdotSun, 0.0) * 0.25;
    // Blinn-Phong specular (front faces only for natural highlights)
    vec3 halfSun = normalize(uSunDir + V);
    float specSun = pow(max(dot(N, halfSun), 0.0), 48.0) * 0.2 * step(0.0, NdotSun) * specMask;
    vec3 sunContrib = uSunColor * (diffSun + specSun);

    // Fill light (same two-sided wrap, softer back contribution)
    float NdotFill = dot(N, uFillDir);
    float diffFill = max(NdotFill, 0.0) + max(-NdotFill, 0.0) * 0.15;
    vec3 fillContrib = uFillColor * diffFill;

    // Point lights with UE4-style attenuation: saturate(1-(d/r)^2)^2
    vec3 pointContrib = vec3(0.0);
    for (int i = 0; i < uNumPointLights && i < MAX_POINT_LIGHTS; i++) {
        vec3 lpos = uPLightPosR[i].xyz;
        float lrad = uPLightPosR[i].w;
        vec3 toLight = lpos - vWorldPos;
        float dist = length(toLight);
        if (dist > lrad || dist < 0.01) continue;
        vec3 L = toLight / dist;

        // UE4 window attenuation
        float d = dist / lrad;
        float atten = clamp(1.0 - d * d, 0.0, 1.0);
        atten *= atten;

        // Two-sided diffuse with wrap
        float NdotPL = dot(N, L);
        float diffPL = max(NdotPL, 0.0) + max(-NdotPL, 0.0) * 0.15;

        // Specular from point light (front face only) — high for wet Art Deco surfaces
        vec3 halfPL = normalize(L + V);
        float specPL = pow(max(dot(N, halfPL), 0.0), 32.0) * 0.35 * step(0.0, NdotPL) * specMask;

        pointContrib += uPLightColor[i] * (diffPL + specPL) * atten;
    }

    // Fresnel rim lighting + environment reflection
    float fresnel = pow(1.0 - max(abs(dot(N, V)), 0.0), 4.0);
    vec3 fresnelColor = uAmbientSky * 1.5 + uSunColor * 0.15;
    vec3 fresnelContrib = fresnelColor * fresnel * 0.06;

    // Environment reflection (hemisphere gradient based on reflection direction)
    vec3 reflDir = reflect(-V, N);
    float envUp = reflDir.z * 0.5 + 0.5; // 0=down, 1=up
    vec3 envColor = mix(uAmbientGround * 0.5, uAmbientSky * 1.5 + uSunColor * 0.3, envUp);
    float envFresnel = fresnel * specMask * 0.15; // only reflective where specular map says so
    fresnelContrib += envColor * envFresnel;

    // Shadow mapping
    float shadow = 1.0;
    if (uShadowEnabled > 0) {
        vec4 sc = uShadowMat * vec4(vWorldPos, 1.0);
        vec3 sp = sc.xyz / sc.w;
        sp = sp * 0.5 + 0.5;
        if (sp.x > 0.0 && sp.x < 1.0 && sp.y > 0.0 && sp.y < 1.0 && sp.z < 1.0) {
            float bias = max(0.005 * (1.0 - abs(dot(N, uSunDir))), 0.001);
            // 5-tap PCF
            float s = 0.0;
            vec2 texel = vec2(1.0) / vec2(textureSize(uShadowMap, 0));
            for (int x = -1; x <= 1; x++) {
                for (int y = -1; y <= 1; y++) {
                    float d = texture(uShadowMap, sp.xy + vec2(x,y)*texel).r;
                    s += (sp.z - bias > d) ? 0.0 : 1.0;
                }
            }
            shadow = s / 9.0;
            shadow = mix(0.35, 1.0, shadow);
        }
    }

    // Combine all lighting contributions
    vec3 lighting = ambient + (sunContrib + fillContrib) * shadow + pointContrib + fresnelContrib;

    // Surface color
    vec3 baseColor = uColor;
    if (uLit == 1 && uHasTexture != 0) {
        baseColor = texture(uTexture, vUV).rgb;
    }
    vec3 result = baseColor * lighting;

    // Emissive: self-illuminating surfaces (neon signs, lamps, etc.)
    if (uEmissive > 0.0) {
        result += baseColor * uEmissive;
    }

    // Distance fog (quadratic curve for natural falloff)
    if (uFogParams.z > 0.5) {
        float fogDist = length(vWorldPos - uCamPos);
        float fogFactor = clamp((fogDist - uFogParams.x) / (uFogParams.y - uFogParams.x), 0.0, 1.0);
        fogFactor *= fogFactor;
        result = mix(result, uFogColor, fogFactor);
    }

    FragColor = vec4(postProcess(result), uAlpha);
}
)";

// Depth-only shader for shadow map pass
static const char* depthVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";
static const char* depthFragSrc = R"(
#version 330 core
void main() { }
)";

// Billboard icon shader sources (forward-declared here for use in Init)
static const char* iconVertSrc_fwd = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos.x, aPos.y, 0.0, 1.0);
    vUV = aUV;
}
)";

static const char* iconFragSrc_fwd = R"(
#version 330 core
uniform sampler2D uIconTex;
uniform vec3 uIconColor;
uniform int uIconSelected;
in vec2 vUV;
out vec4 FragColor;
void main() {
    vec4 tex = texture(uIconTex, vUV);
    if (tex.a < 0.1) discard;
    vec3 col = tex.rgb * uIconColor;
    if (uIconSelected > 0) {
        col += vec3(0.3, 0.3, 0.1);
    }
    FragColor = vec4(col, tex.a);
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

    // Lighting uniforms
    m_LocCamPos = glGetUniformLocation(m_ShaderProgram, "uCamPos");
    m_LocSunDir = glGetUniformLocation(m_ShaderProgram, "uSunDir");
    m_LocSunColor = glGetUniformLocation(m_ShaderProgram, "uSunColor");
    m_LocFillDir = glGetUniformLocation(m_ShaderProgram, "uFillDir");
    m_LocFillColor = glGetUniformLocation(m_ShaderProgram, "uFillColor");
    m_LocAmbientSky = glGetUniformLocation(m_ShaderProgram, "uAmbientSky");
    m_LocAmbientGround = glGetUniformLocation(m_ShaderProgram, "uAmbientGround");
    m_LocFogColor = glGetUniformLocation(m_ShaderProgram, "uFogColor");
    m_LocFogParams = glGetUniformLocation(m_ShaderProgram, "uFogParams");
    m_LocExposureGamma = glGetUniformLocation(m_ShaderProgram, "uExposureGamma");
    m_LocNumPointLights = glGetUniformLocation(m_ShaderProgram, "uNumPointLights");
    m_LocEmissive = glGetUniformLocation(m_ShaderProgram, "uEmissive");
    m_LocNormalMap = glGetUniformLocation(m_ShaderProgram, "uNormalMap");
    m_LocHasNormalMap = glGetUniformLocation(m_ShaderProgram, "uHasNormalMap");
    m_LocSpecMap = glGetUniformLocation(m_ShaderProgram, "uSpecMap");
    m_LocHasSpecMap = glGetUniformLocation(m_ShaderProgram, "uHasSpecMap");
    m_LocAlpha = glGetUniformLocation(m_ShaderProgram, "uAlpha");
    for (int i = 0; i < MAX_POINT_LIGHTS; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "uPLightPosR[%d]", i);
        m_LocPLightPosR[i] = glGetUniformLocation(m_ShaderProgram, buf);
        snprintf(buf, sizeof(buf), "uPLightColor[%d]", i);
        m_LocPLightColor[i] = glGetUniformLocation(m_ShaderProgram, buf);
    }

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

    // Shadow map uniforms
    m_LocShadowMat = glGetUniformLocation(m_ShaderProgram, "uShadowMat");
    m_LocShadowMap = glGetUniformLocation(m_ShaderProgram, "uShadowMap");
    m_LocShadowEnabled = glGetUniformLocation(m_ShaderProgram, "uShadowEnabled");

    // Compile depth-only shader for shadow pass
    {
        unsigned int dvs = CompileShader(depthVertSrc, GL_VERTEX_SHADER);
        unsigned int dfs = CompileShader(depthFragSrc, GL_FRAGMENT_SHADER);
        m_DepthShader = glCreateProgram();
        glAttachShader(m_DepthShader, dvs);
        glAttachShader(m_DepthShader, dfs);
        glLinkProgram(m_DepthShader);
        glDeleteShader(dvs);
        glDeleteShader(dfs);
        m_LocDepthMVP = glGetUniformLocation(m_DepthShader, "uMVP");
    }

    // Create shadow map FBO
    glGenFramebuffers(1, &m_ShadowFBO);
    glGenTextures(1, &m_ShadowDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_ShadowDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_ShadowMapSize, m_ShadowMapSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = {1,1,1,1};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_ShadowDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    printf("[Viewport] Shadow map FBO created (%dx%d)\n", m_ShadowMapSize, m_ShadowMapSize);

    // Compile billboard icon shader
    {
        unsigned int ivs = CompileShader(iconVertSrc_fwd, GL_VERTEX_SHADER);
        unsigned int ifs = CompileShader(iconFragSrc_fwd, GL_FRAGMENT_SHADER);
        m_IconShader = glCreateProgram();
        glAttachShader(m_IconShader, ivs);
        glAttachShader(m_IconShader, ifs);
        glLinkProgram(m_IconShader);
        glDeleteShader(ivs);
        glDeleteShader(ifs);
        m_LocIconMVP = glGetUniformLocation(m_IconShader, "uMVP");
        m_LocIconTex = glGetUniformLocation(m_IconShader, "uIconTex");
        m_LocIconColor = glGetUniformLocation(m_IconShader, "uIconColor");
        m_LocIconSelected = glGetUniformLocation(m_IconShader, "uIconSelected");
    }

    BuildGridMesh();
    BuildDiamondMesh();
    BuildGizmoMesh();
    BuildBillboardQuad();
    GenerateIconTextures();
    printf("[Viewport] Actor icon textures generated (9 categories)\n");

    // Generate default checkerboard texture (like UE's missing material)
    {
        const int sz = 64;
        unsigned char pixels[sz * sz * 3];
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                bool check = ((x / 8) + (y / 8)) % 2 == 0;
                unsigned char c = check ? 180 : 130;
                pixels[(y * sz + x) * 3 + 0] = c;
                pixels[(y * sz + x) * 3 + 1] = c;
                pixels[(y * sz + x) * 3 + 2] = (unsigned char)(c + 10); // slight blue tint
            }
        }
        glGenTextures(1, &m_DefaultTexture);
        glBindTexture(GL_TEXTURE_2D, m_DefaultTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, sz, sz, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    InitBloom();
    return true;
}

void Viewport::Shutdown()
{
    if (m_ShaderProgram) glDeleteProgram(m_ShaderProgram);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_GridVAO) glDeleteVertexArrays(1, &m_GridVAO);
    if (m_GridVBO) glDeleteBuffers(1, &m_GridVBO);
    if (m_DiamondVAO) glDeleteVertexArrays(1, &m_DiamondVAO);
    if (m_DiamondVBO) glDeleteBuffers(1, &m_DiamondVBO);
    if (m_GizmoVAO) glDeleteVertexArrays(1, &m_GizmoVAO);
    if (m_GizmoVBO) glDeleteBuffers(1, &m_GizmoVBO);
    if (m_DepthShader) glDeleteProgram(m_DepthShader);
    if (m_ShadowFBO) glDeleteFramebuffers(1, &m_ShadowFBO);
    if (m_ShadowDepthTex) glDeleteTextures(1, &m_ShadowDepthTex);
    if (m_IconShader) glDeleteProgram(m_IconShader);
    if (m_BillboardVAO) glDeleteVertexArrays(1, &m_BillboardVAO);
    if (m_BillboardVBO) glDeleteBuffers(1, &m_BillboardVBO);
    for (int i = 0; i < 10; i++) if (m_IconTextures[i]) glDeleteTextures(1, &m_IconTextures[i]);
    if (m_DefaultTexture) glDeleteTextures(1, &m_DefaultTexture);
    if (m_BloomShader) glDeleteProgram(m_BloomShader);
    if (m_CompositeShader) glDeleteProgram(m_CompositeShader);
    if (m_FullscreenVAO) glDeleteVertexArrays(1, &m_FullscreenVAO);
    if (m_FullscreenVBO) glDeleteBuffers(1, &m_FullscreenVBO);
    if (m_SceneFBO) glDeleteFramebuffers(1, &m_SceneFBO);
    if (m_SceneColorTex) glDeleteTextures(1, &m_SceneColorTex);
    if (m_SceneDepthTex) glDeleteTextures(1, &m_SceneDepthTex);
    for (int i = 0; i < 2; i++) {
        if (m_BloomFBO[i]) glDeleteFramebuffers(1, &m_BloomFBO[i]);
        if (m_BloomTex[i]) glDeleteTextures(1, &m_BloomTex[i]);
    }
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

    // Axis lines start here (drawn separately with colors)
    m_AxisVertStart = m_GridVertCount;
    // X axis (red): along X at Y=0, Z=0
    verts.push_back(-halfSize); verts.push_back(0); verts.push_back(0);
    verts.push_back(halfSize); verts.push_back(0); verts.push_back(0);
    // Y axis (green): along Y at X=0, Z=0
    verts.push_back(0); verts.push_back(-halfSize); verts.push_back(0);
    verts.push_back(0); verts.push_back(halfSize); verts.push_back(0);
    // Z axis (blue): along Z at X=0, Y=0
    verts.push_back(0); verts.push_back(0); verts.push_back(-halfSize);
    verts.push_back(0); verts.push_back(0); verts.push_back(halfSize);

    glGenVertexArrays(1, &m_GridVAO);
    glGenBuffers(1, &m_GridVBO);
    glBindVertexArray(m_GridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_GridVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
}

void Viewport::BuildDiamondMesh()
{
    // Octahedron: 6 points, 8 triangles = 24 vertices (position-only, flat shading)
    float v[] = {
        // Top 4 faces (+Z apex)
         0, 0, 1,   1, 0, 0,   0, 1, 0,
         0, 0, 1,   0, 1, 0,  -1, 0, 0,
         0, 0, 1,  -1, 0, 0,   0,-1, 0,
         0, 0, 1,   0,-1, 0,   1, 0, 0,
        // Bottom 4 faces (-Z apex)
         0, 0,-1,   0, 1, 0,   1, 0, 0,
         0, 0,-1,  -1, 0, 0,   0, 1, 0,
         0, 0,-1,   0,-1, 0,  -1, 0, 0,
         0, 0,-1,   1, 0, 0,   0,-1, 0,
    };
    m_DiamondVertCount = 24;
    glGenVertexArrays(1, &m_DiamondVAO);
    glGenBuffers(1, &m_DiamondVBO);
    glBindVertexArray(m_DiamondVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_DiamondVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
}

// Forward declarations for static math functions (defined below)
static Mat4 MulMat4(const Mat4& a, const Mat4& b);
static Mat4 TranslateMat4(float x, float y, float z);
static Mat4 ScaleMat4(float s);
static Mat4 ScaleMat4xyz(float sx, float sy, float sz);

void Viewport::BuildGizmoMesh()
{
    // 3 axis lines: each 2 vertices (origin to unit direction)
    float v[] = {
        0,0,0, 1,0,0, // X axis
        0,0,0, 0,1,0, // Y axis
        0,0,0, 0,0,1, // Z axis
    };
    glGenVertexArrays(1, &m_GizmoVAO);
    glGenBuffers(1, &m_GizmoVBO);
    glBindVertexArray(m_GizmoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_GizmoVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
}

void Viewport::BuildBillboardQuad()
{
    // 2D quad with UV: position XY (-1..1), UV (0..1)
    float v[] = {
        -1, -1,  0, 0,
         1, -1,  1, 0,
         1,  1,  1, 1,
        -1, -1,  0, 0,
         1,  1,  1, 1,
        -1,  1,  0, 1,
    };
    glGenVertexArrays(1, &m_BillboardVAO);
    glGenBuffers(1, &m_BillboardVBO);
    glBindVertexArray(m_BillboardVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_BillboardVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
}

// Bloom bright-pass + blur shader (ping-pong gaussian)
static const char* bloomVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

static const char* bloomFragSrc = R"(
#version 330 core
uniform sampler2D uTex;
uniform int uPass; // 0=bright extract, 1=horiz blur, 2=vert blur
uniform float uThreshold;
in vec2 vUV;
out vec4 FragColor;

void main() {
    if (uPass == 0) {
        // Bright-pass extract
        vec3 col = texture(uTex, vUV).rgb;
        float brightness = dot(col, vec3(0.2126, 0.7152, 0.0722));
        if (brightness > uThreshold) {
            FragColor = vec4(col * (brightness - uThreshold), 1.0);
        } else {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        }
    } else {
        // 9-tap gaussian blur
        vec2 texSize = vec2(textureSize(uTex, 0));
        vec2 dir = (uPass == 1) ? vec2(1.0/texSize.x, 0.0) : vec2(0.0, 1.0/texSize.y);
        float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
        vec3 result = texture(uTex, vUV).rgb * weights[0];
        for (int i = 1; i < 5; i++) {
            result += texture(uTex, vUV + dir * float(i)).rgb * weights[i];
            result += texture(uTex, vUV - dir * float(i)).rgb * weights[i];
        }
        FragColor = vec4(result, 1.0);
    }
}
)";

// Composite shader: blend scene + bloom + FXAA + vignette + film grain + SSAO
static const char* compositeFragSrc = R"(
#version 330 core
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform sampler2D uDepth;
uniform float uBloomIntensity;
uniform vec2 uInvResolution; // 1.0/width, 1.0/height
uniform float uTime;
uniform int uFXAAEnabled;
uniform int uVignetteEnabled;
uniform float uVignetteStrength;
uniform int uFilmGrainEnabled;
uniform float uFilmGrainStrength;
uniform int uSSAOEnabled;
uniform float uSSAOIntensity;
uniform int uGodRaysEnabled;
uniform vec2 uSunScreenPos; // sun position in [0,1] screen space
uniform float uGodRayDensity;
uniform float uGodRayWeight;
uniform float uGodRayDecay;
uniform float uGodRayIntensity;
in vec2 vUV;
out vec4 FragColor;

// FXAA 3.11 (simplified Nvidia implementation)
vec3 fxaa(sampler2D tex, vec2 uv, vec2 invRes) {
    float FXAA_REDUCE_MIN = 1.0/128.0;
    float FXAA_REDUCE_MUL = 1.0/8.0;
    float FXAA_SPAN_MAX = 8.0;

    vec3 rgbNW = texture(tex, uv + vec2(-1.0, -1.0) * invRes).rgb;
    vec3 rgbNE = texture(tex, uv + vec2( 1.0, -1.0) * invRes).rgb;
    vec3 rgbSW = texture(tex, uv + vec2(-1.0,  1.0) * invRes).rgb;
    vec3 rgbSE = texture(tex, uv + vec2( 1.0,  1.0) * invRes).rgb;
    vec3 rgbM  = texture(tex, uv).rgb;

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * FXAA_REDUCE_MUL, FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * invRes;

    vec3 rgbA = 0.5 * (texture(tex, uv + dir * (1.0/3.0 - 0.5)).rgb +
                        texture(tex, uv + dir * (2.0/3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(tex, uv + dir * -0.5).rgb +
                                       texture(tex, uv + dir *  0.5).rgb);
    float lumaB = dot(rgbB, luma);
    if (lumaB < lumaMin || lumaB > lumaMax) return rgbA;
    return rgbB;
}

// Film grain noise
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// God rays: radial blur from sun screen position (volumetric light scattering)
vec3 godRays(sampler2D tex, vec2 uv, vec2 sunPos) {
    int NUM_SAMPLES = 32;
    vec2 deltaUV = (uv - sunPos) * uGodRayDensity / float(NUM_SAMPLES);
    vec2 sampleUV = uv;
    vec3 accumColor = vec3(0.0);
    float illuminationDecay = 1.0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        sampleUV -= deltaUV;
        vec3 sampleColor = texture(tex, sampleUV).rgb;
        // Only scatter bright pixels
        float luma = dot(sampleColor, vec3(0.299, 0.587, 0.114));
        sampleColor *= max(luma - 0.5, 0.0) * 2.0;
        sampleColor *= illuminationDecay * uGodRayWeight;
        accumColor += sampleColor;
        illuminationDecay *= uGodRayDecay;
    }
    return accumColor * uGodRayIntensity;
}

// SSAO: sample depth neighbors to estimate occlusion
float computeSSAO(vec2 uv, vec2 invRes) {
    float depth = texture(uDepth, uv).r;
    if (depth >= 0.9999) return 1.0; // skybox, no AO

    float ao = 0.0;
    const int samples = 8;
    // Poisson-disk offsets
    vec2 offsets[8] = vec2[8](
        vec2(-0.94, -0.34), vec2(0.18, -0.93),
        vec2(0.95,  0.12), vec2(-0.27, 0.96),
        vec2(-0.68, 0.72), vec2(0.74, -0.64),
        vec2(-0.41, -0.82), vec2(0.53, 0.61)
    );
    float radius = 4.0; // pixels
    for (int i = 0; i < samples; i++) {
        vec2 sampleUV = uv + offsets[i] * radius * invRes;
        float sampleDepth = texture(uDepth, sampleUV).r;
        float depthDiff = depth - sampleDepth;
        // Count as occluded if neighbor is closer to camera (smaller depth in [0,1])
        if (depthDiff > 0.0001 && depthDiff < 0.005)
            ao += 1.0;
    }
    return 1.0 - (ao / float(samples));
}

void main() {
    // Scene color (with optional FXAA)
    vec3 scene;
    if (uFXAAEnabled != 0) {
        scene = fxaa(uScene, vUV, uInvResolution);
    } else {
        scene = texture(uScene, vUV).rgb;
    }

    // SSAO (darken corners and contact shadows)
    if (uSSAOEnabled != 0) {
        float ao = computeSSAO(vUV, uInvResolution);
        scene *= mix(1.0, ao, uSSAOIntensity);
    }

    // Add bloom
    vec3 bloom = texture(uBloom, vUV).rgb;
    scene += bloom * uBloomIntensity;

    // God rays (volumetric light scattering from sun)
    if (uGodRaysEnabled != 0) {
        scene += godRays(uScene, vUV, uSunScreenPos);
    }

    // Vignette (darkened edges)
    if (uVignetteEnabled != 0) {
        vec2 q = vUV - 0.5;
        float vign = 1.0 - dot(q, q) * uVignetteStrength;
        scene *= clamp(vign, 0.0, 1.0);
    }

    // Film grain (subtle noise overlay)
    if (uFilmGrainEnabled != 0) {
        float grain = hash(vUV * 1000.0 + uTime * 100.0) - 0.5;
        scene += grain * uFilmGrainStrength;
    }

    FragColor = vec4(scene, 1.0);
}
)";

void Viewport::InitBloom()
{
    // Fullscreen quad (same layout as billboard: pos2D + uv)
    float fsv[] = {
        -1, -1,  0, 0,
         1, -1,  1, 0,
         1,  1,  1, 1,
        -1, -1,  0, 0,
         1,  1,  1, 1,
        -1,  1,  0, 1,
    };
    glGenVertexArrays(1, &m_FullscreenVAO);
    glGenBuffers(1, &m_FullscreenVBO);
    glBindVertexArray(m_FullscreenVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_FullscreenVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fsv), fsv, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Bloom shader (bright extract + blur)
    {
        unsigned int vs = CompileShader(bloomVertSrc, GL_VERTEX_SHADER);
        unsigned int fs = CompileShader(bloomFragSrc, GL_FRAGMENT_SHADER);
        m_BloomShader = glCreateProgram();
        glAttachShader(m_BloomShader, vs);
        glAttachShader(m_BloomShader, fs);
        glLinkProgram(m_BloomShader);
        glDeleteShader(vs);
        glDeleteShader(fs);
    }

    // Composite shader
    {
        unsigned int vs = CompileShader(bloomVertSrc, GL_VERTEX_SHADER);
        unsigned int fs = CompileShader(compositeFragSrc, GL_FRAGMENT_SHADER);
        m_CompositeShader = glCreateProgram();
        glAttachShader(m_CompositeShader, vs);
        glAttachShader(m_CompositeShader, fs);
        glLinkProgram(m_CompositeShader);
        glDeleteShader(vs);
        glDeleteShader(fs);
    }

    printf("[Viewport] Bloom post-process initialized\n");
}

void Viewport::ResizeBloomFBOs(int w, int h)
{
    if (w == m_SceneFBOWidth && h == m_SceneFBOHeight) return;
    m_SceneFBOWidth = w;
    m_SceneFBOHeight = h;

    // Delete old
    if (m_SceneFBO) glDeleteFramebuffers(1, &m_SceneFBO);
    if (m_SceneColorTex) glDeleteTextures(1, &m_SceneColorTex);
    if (m_SceneDepthTex) glDeleteTextures(1, &m_SceneDepthTex);
    for (int i = 0; i < 2; i++) {
        if (m_BloomFBO[i]) glDeleteFramebuffers(1, &m_BloomFBO[i]);
        if (m_BloomTex[i]) glDeleteTextures(1, &m_BloomTex[i]);
    }

    // Scene FBO (full res, HDR float16)
    glGenFramebuffers(1, &m_SceneFBO);
    glGenTextures(1, &m_SceneColorTex);
    glBindTexture(GL_TEXTURE_2D, m_SceneColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Depth as texture (so SSAO/composite can sample it)
    glGenTextures(1, &m_SceneDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_SceneDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SceneColorTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_SceneDepthTex, 0);

    // Bloom FBOs (half res for blur)
    int bw = w / 2, bh = h / 2;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    for (int i = 0; i < 2; i++) {
        glGenFramebuffers(1, &m_BloomFBO[i]);
        glGenTextures(1, &m_BloomTex[i]);
        glBindTexture(GL_TEXTURE_2D, m_BloomTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_BloomFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_BloomTex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Viewport::RenderBloom()
{
    if (!m_BloomEnabled || !m_BloomShader || !m_CompositeShader || !m_SceneFBO) return;

    int bw = m_SceneFBOWidth / 2, bh = m_SceneFBOHeight / 2;
    if (bw < 1 || bh < 1) return;

    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(m_FullscreenVAO);

    // Pass 1: Bright extract from scene → bloom[0]
    glBindFramebuffer(GL_FRAMEBUFFER, m_BloomFBO[0]);
    glViewport(0, 0, bw, bh);
    glUseProgram(m_BloomShader);
    glUniform1i(glGetUniformLocation(m_BloomShader, "uTex"), 0);
    glUniform1i(glGetUniformLocation(m_BloomShader, "uPass"), 0);
    glUniform1f(glGetUniformLocation(m_BloomShader, "uThreshold"), m_BloomThreshold);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_SceneColorTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 2: Horizontal blur bloom[0] → bloom[1]
    glBindFramebuffer(GL_FRAMEBUFFER, m_BloomFBO[1]);
    glUniform1i(glGetUniformLocation(m_BloomShader, "uPass"), 1);
    glBindTexture(GL_TEXTURE_2D, m_BloomTex[0]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 3: Vertical blur bloom[1] → bloom[0]
    glBindFramebuffer(GL_FRAMEBUFFER, m_BloomFBO[0]);
    glUniform1i(glGetUniformLocation(m_BloomShader, "uPass"), 2);
    glBindTexture(GL_TEXTURE_2D, m_BloomTex[1]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Second blur pass for more spread
    glBindFramebuffer(GL_FRAMEBUFFER, m_BloomFBO[1]);
    glUniform1i(glGetUniformLocation(m_BloomShader, "uPass"), 1);
    glBindTexture(GL_TEXTURE_2D, m_BloomTex[0]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindFramebuffer(GL_FRAMEBUFFER, m_BloomFBO[0]);
    glUniform1i(glGetUniformLocation(m_BloomShader, "uPass"), 2);
    glBindTexture(GL_TEXTURE_2D, m_BloomTex[1]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 4: Composite scene + bloom + post-FX → screen
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_SceneFBOWidth, m_SceneFBOHeight);
    glUseProgram(m_CompositeShader);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uScene"), 0);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uBloom"), 1);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uBloomIntensity"), m_BloomIntensity);
    glUniform2f(glGetUniformLocation(m_CompositeShader, "uInvResolution"),
                1.0f / (float)m_SceneFBOWidth, 1.0f / (float)m_SceneFBOHeight);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uTime"), m_Time);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uFXAAEnabled"), m_FXAAEnabled ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uVignetteEnabled"), m_VignetteEnabled ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uVignetteStrength"), m_VignetteStrength);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uFilmGrainEnabled"), m_FilmGrainEnabled ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uFilmGrainStrength"), m_FilmGrainStrength);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uSSAOEnabled"), m_SSAOEnabled ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uSSAOIntensity"), m_SSAOIntensity);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uGodRaysEnabled"), m_GodRaysEnabled ? 1 : 0);
    // Compute sun screen position by projecting a far point along sun direction
    {
        float sunYaw = m_Lighting.sunYaw * 3.14159f / 180.0f;
        float sunPitch = m_Lighting.sunPitch * 3.14159f / 180.0f;
        float sdx = cosf(sunPitch) * cosf(sunYaw);
        float sdy = cosf(sunPitch) * sinf(sunYaw);
        float sdz = sinf(sunPitch);
        // Sun "position" = camera + sunDir * farDistance (simulates distant sun)
        Vec3 cp = m_Camera.GetPosition();
        float sunWorld[3] = { cp.x + sdx * 100000.0f, cp.y + sdy * 100000.0f, cp.z + sdz * 100000.0f };
        // Project through VP matrix
        float clip[4] = {0};
        for (int r = 0; r < 4; r++)
            clip[r] = m_LastVP[0*4+r]*sunWorld[0] + m_LastVP[1*4+r]*sunWorld[1] + m_LastVP[2*4+r]*sunWorld[2] + m_LastVP[3*4+r];
        float sunScreenX = 0.5f, sunScreenY = 0.5f;
        if (clip[3] > 0.001f) {
            sunScreenX = (clip[0] / clip[3]) * 0.5f + 0.5f;
            sunScreenY = (clip[1] / clip[3]) * 0.5f + 0.5f;
        }
        glUniform2f(glGetUniformLocation(m_CompositeShader, "uSunScreenPos"), sunScreenX, sunScreenY);
    }
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uGodRayDensity"), m_GodRayDensity);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uGodRayWeight"), m_GodRayWeight);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uGodRayDecay"), m_GodRayDecay);
    glUniform1f(glGetUniformLocation(m_CompositeShader, "uGodRayIntensity"), m_GodRayIntensity);
    glUniform1i(glGetUniformLocation(m_CompositeShader, "uDepth"), 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_SceneColorTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_BloomTex[0]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_SceneDepthTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_DEPTH_TEST);
}

// Helper to draw a filled circle into a pixel buffer
static void DrawCircle(uint8_t* buf, int w, int h, int cx, int cy, int r, uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa)
{
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            int dx = x - cx, dy = y - cy;
            if (dx*dx + dy*dy <= r*r) {
                int idx = (y * w + x) * 4;
                buf[idx] = pr; buf[idx+1] = pg; buf[idx+2] = pb; buf[idx+3] = pa;
            }
        }
    }
}

// Helper to draw a line into a pixel buffer
static void DrawLine(uint8_t* buf, int w, int h, int x0, int y0, int x1, int y1, uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa)
{
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (int i = 0; i < 200; i++) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            int idx = (y0 * w + x0) * 4;
            buf[idx] = pr; buf[idx+1] = pg; buf[idx+2] = pb; buf[idx+3] = pa;
            // Thicken
            if (x0+1 < w) { int i2 = (y0*w+x0+1)*4; buf[i2]=pr; buf[i2+1]=pg; buf[i2+2]=pb; buf[i2+3]=pa; }
            if (y0+1 < h) { int i2 = ((y0+1)*w+x0)*4; buf[i2]=pr; buf[i2+1]=pg; buf[i2+2]=pb; buf[i2+3]=pa; }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

// Helper to draw a filled rect
static void DrawRect(uint8_t* buf, int w, int h, int rx, int ry, int rw, int rh, uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa)
{
    for (int y = ry; y < ry + rh; y++) {
        for (int x = rx; x < rx + rw; x++) {
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            int idx = (y * w + x) * 4;
            buf[idx] = pr; buf[idx+1] = pg; buf[idx+2] = pb; buf[idx+3] = pa;
        }
    }
}

void Viewport::GenerateIconTextures()
{
    const int SZ = 32; // 32x32 pixel icons
    uint8_t pixels[SZ * SZ * 4];

    auto upload = [&](int idx) {
        glGenTextures(1, &m_IconTextures[idx]);
        glBindTexture(GL_TEXTURE_2D, m_IconTextures[idx]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SZ, SZ, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    };

    // 0: Spawner — skull/enemy icon (red X in circle)
    memset(pixels, 0, sizeof(pixels));
    DrawCircle(pixels, SZ, SZ, 16, 16, 14, 200, 50, 50, 255);
    DrawLine(pixels, SZ, SZ, 6, 6, 26, 26, 255, 220, 220, 255);
    DrawLine(pixels, SZ, SZ, 26, 6, 6, 26, 255, 220, 220, 255);
    upload(0);

    // 1: Trigger — lightning bolt (yellow triangle)
    memset(pixels, 0, sizeof(pixels));
    DrawLine(pixels, SZ, SZ, 16, 2, 6, 16, 255, 255, 50, 255);
    DrawLine(pixels, SZ, SZ, 6, 16, 18, 14, 255, 255, 50, 255);
    DrawLine(pixels, SZ, SZ, 18, 14, 10, 30, 255, 255, 50, 255);
    DrawCircle(pixels, SZ, SZ, 16, 16, 4, 255, 255, 100, 200);
    upload(1);

    // 2: Light — lightbulb (orange/yellow circle with rays)
    memset(pixels, 0, sizeof(pixels));
    DrawCircle(pixels, SZ, SZ, 16, 14, 9, 255, 220, 50, 255); // bulb
    DrawRect(pixels, SZ, SZ, 13, 23, 6, 6, 200, 200, 200, 255); // base
    // Rays
    DrawLine(pixels, SZ, SZ, 16, 0, 16, 3, 255, 255, 150, 255);
    DrawLine(pixels, SZ, SZ, 28, 6, 25, 8, 255, 255, 150, 255);
    DrawLine(pixels, SZ, SZ, 4, 6, 7, 8, 255, 255, 150, 255);
    DrawLine(pixels, SZ, SZ, 30, 14, 27, 14, 255, 255, 150, 255);
    DrawLine(pixels, SZ, SZ, 2, 14, 5, 14, 255, 255, 150, 255);
    upload(2);

    // 3: Door — door/arch (blue rectangle with handle)
    memset(pixels, 0, sizeof(pixels));
    DrawRect(pixels, SZ, SZ, 8, 4, 16, 26, 80, 130, 255, 255);
    DrawRect(pixels, SZ, SZ, 10, 6, 12, 22, 40, 80, 180, 255);
    DrawCircle(pixels, SZ, SZ, 19, 18, 2, 255, 255, 200, 255); // handle
    upload(3);

    // 4: Pickup — gem/item (green diamond)
    memset(pixels, 0, sizeof(pixels));
    DrawLine(pixels, SZ, SZ, 16, 2, 28, 16, 50, 255, 50, 255);
    DrawLine(pixels, SZ, SZ, 28, 16, 16, 30, 50, 255, 50, 255);
    DrawLine(pixels, SZ, SZ, 16, 30, 4, 16, 50, 255, 50, 255);
    DrawLine(pixels, SZ, SZ, 4, 16, 16, 2, 50, 255, 50, 255);
    DrawCircle(pixels, SZ, SZ, 16, 16, 6, 100, 255, 100, 200);
    upload(4);

    // 5: Effect/Emitter — sparkle (cyan star)
    memset(pixels, 0, sizeof(pixels));
    DrawLine(pixels, SZ, SZ, 16, 2, 16, 30, 100, 255, 255, 255);
    DrawLine(pixels, SZ, SZ, 2, 16, 30, 16, 100, 255, 255, 255);
    DrawLine(pixels, SZ, SZ, 6, 6, 26, 26, 50, 200, 200, 255);
    DrawLine(pixels, SZ, SZ, 26, 6, 6, 26, 50, 200, 200, 255);
    DrawCircle(pixels, SZ, SZ, 16, 16, 4, 200, 255, 255, 255);
    upload(5);

    // 6: PlayerStart — person/arrow (white arrow up)
    memset(pixels, 0, sizeof(pixels));
    DrawCircle(pixels, SZ, SZ, 16, 8, 5, 255, 255, 255, 255);  // head
    DrawRect(pixels, SZ, SZ, 12, 13, 8, 12, 255, 255, 255, 255); // body
    DrawLine(pixels, SZ, SZ, 8, 26, 12, 30, 200, 200, 255, 255);  // legs
    DrawLine(pixels, SZ, SZ, 24, 26, 20, 30, 200, 200, 255, 255);
    upload(6);

    // 7: StaticMesh — cube (gray wireframe cube)
    memset(pixels, 0, sizeof(pixels));
    DrawRect(pixels, SZ, SZ, 6, 6, 20, 20, 150, 150, 150, 200);
    DrawLine(pixels, SZ, SZ, 6, 6, 26, 6, 200, 200, 200, 255);
    DrawLine(pixels, SZ, SZ, 26, 6, 26, 26, 200, 200, 200, 255);
    DrawLine(pixels, SZ, SZ, 26, 26, 6, 26, 200, 200, 200, 255);
    DrawLine(pixels, SZ, SZ, 6, 26, 6, 6, 200, 200, 200, 255);
    DrawLine(pixels, SZ, SZ, 10, 2, 30, 2, 200, 200, 200, 255);
    DrawLine(pixels, SZ, SZ, 30, 2, 30, 22, 200, 200, 200, 255);
    upload(7);

    // 8: Other — question mark (dark gray circle with ?)
    memset(pixels, 0, sizeof(pixels));
    DrawCircle(pixels, SZ, SZ, 16, 16, 12, 100, 100, 100, 255);
    DrawLine(pixels, SZ, SZ, 12, 10, 16, 8, 220, 220, 220, 255);
    DrawLine(pixels, SZ, SZ, 16, 8, 20, 10, 220, 220, 220, 255);
    DrawLine(pixels, SZ, SZ, 20, 10, 18, 16, 220, 220, 220, 255);
    DrawLine(pixels, SZ, SZ, 18, 16, 16, 18, 220, 220, 220, 255);
    DrawCircle(pixels, SZ, SZ, 16, 22, 2, 220, 220, 220, 255);
    upload(8);

    // 9: Glow — soft radial gradient for light halos (64x64 for smoothness)
    {
        const int GSZ = 64;
        uint8_t glowPx[GSZ * GSZ * 4];
        memset(glowPx, 0, sizeof(glowPx));
        for (int y = 0; y < GSZ; y++) {
            for (int x = 0; x < GSZ; x++) {
                float dx = (x - GSZ/2.0f) / (GSZ/2.0f);
                float dy = (y - GSZ/2.0f) / (GSZ/2.0f);
                float d = sqrtf(dx*dx + dy*dy);
                float alpha = 1.0f - d;
                if (alpha < 0) alpha = 0;
                alpha = alpha * alpha; // quadratic falloff for soft edge
                int idx = (y * GSZ + x) * 4;
                glowPx[idx] = 255;
                glowPx[idx+1] = 255;
                glowPx[idx+2] = 255;
                glowPx[idx+3] = (uint8_t)(alpha * 180.0f);
            }
        }
        glGenTextures(1, &m_IconTextures[9]);
        glBindTexture(GL_TEXTURE_2D, m_IconTextures[9]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GSZ, GSZ, 0, GL_RGBA, GL_UNSIGNED_BYTE, glowPx);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
}

void Viewport::RenderActorIcons(const std::vector<EditorActor>& actors, int selectedActor,
                                 const Mat4& view, const Mat4& proj, const Vec3& camPos, float radiusSq)
{
    if (!m_ShowActorIcons || !m_IconShader || !m_BillboardVAO || m_GamePreview) return;

    glUseProgram(m_IconShader);
    glBindVertexArray(m_BillboardVAO);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); // don't write to depth (billboards are transparent helpers)

    glActiveTexture(GL_TEXTURE0);
    glUniform1i(m_LocIconTex, 0);

    // Camera right and up vectors from view matrix (columns 0 and 1 transposed)
    Vec3 camRight = {view.m[0], view.m[4], view.m[8]};
    Vec3 camUp = {view.m[1], view.m[5], view.m[9]};

    for (int i = 0; i < (int)actors.size(); i++) {
        auto& a = actors[i];
        if (!a.visible || !a.hasLocation) continue;

        // Skip actors that already have meshes rendered (unless they're lights)
        if (!a.isLight && a.meshIndex >= 0 && a.meshIndex < (int)m_MeshGPU.size() &&
            m_MeshGPU[a.meshIndex].vao != 0 && m_MeshGPU[a.meshIndex].indexCount > 0) continue;

        // Distance cull
        if (m_DrawRadiusEnabled) {
            float dx = a.location.x - camPos.x;
            float dy = a.location.y - camPos.y;
            float dz = a.location.z - camPos.z;
            if (dx*dx + dy*dy + dz*dz > radiusSq * 4.0f) continue;
        }

        // NaN check
        if (std::isnan(a.location.x) || std::isnan(a.location.y) || std::isnan(a.location.z)) continue;

        // Determine category and icon
        ActorCategory cat = CategorizeActor(a.className);
        int texIdx = (int)cat;
        if (texIdx < 0 || texIdx > 8) texIdx = 8;

        Vec3 col = CategoryColor(cat);
        bool isSel = (i == selectedActor) || a.selected;

        // Billboard size: scale with distance for readability, clamp between 40-200 world units
        float dist = sqrtf((a.location.x - camPos.x) * (a.location.x - camPos.x) +
                           (a.location.y - camPos.y) * (a.location.y - camPos.y) +
                           (a.location.z - camPos.z) * (a.location.z - camPos.z));
        float iconSize = dist * 0.02f;
        if (iconSize < 40.0f) iconSize = 40.0f;
        if (iconSize > 200.0f) iconSize = 200.0f;

        // Build billboard model matrix: translate to actor pos, orient toward camera
        // Billboard = pos + right * x * size + up * y * size
        Mat4 model;
        memset(&model, 0, sizeof(model));
        model.m[0] = camRight.x * iconSize;
        model.m[1] = camRight.y * iconSize;
        model.m[2] = camRight.z * iconSize;
        model.m[4] = camUp.x * iconSize;
        model.m[5] = camUp.y * iconSize;
        model.m[6] = camUp.z * iconSize;
        model.m[8] = 0; model.m[9] = 0; model.m[10] = 1; // Z doesn't matter for billboard
        model.m[12] = a.location.x;
        model.m[13] = a.location.y;
        model.m[14] = a.location.z;
        model.m[15] = 1.0f;

        Mat4 vp = MulMat4(proj, view);
        Mat4 mvp = MulMat4(vp, model);

        glUniformMatrix4fv(m_LocIconMVP, 1, GL_FALSE, mvp.m);
        glUniform3f(m_LocIconColor, col.x, col.y, col.z);
        glUniform1i(m_LocIconSelected, isSel ? 1 : 0);
        glBindTexture(GL_TEXTURE_2D, m_IconTextures[texIdx]);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        m_DrawCalls++;
    }

    // Second pass: additive glow halos for light actors (volumetric feel)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive blending
    glBindTexture(GL_TEXTURE_2D, m_IconTextures[9]); // glow texture
    glUniform1i(m_LocIconSelected, 0);
    Mat4 vp2 = MulMat4(proj, view);

    for (int i = 0; i < (int)actors.size(); i++) {
        auto& a = actors[i];
        if (!a.visible || !a.hasLocation || !a.isLight) continue;

        // Distance cull
        if (m_DrawRadiusEnabled) {
            float dx = a.location.x - camPos.x;
            float dy = a.location.y - camPos.y;
            float dz = a.location.z - camPos.z;
            if (dx*dx + dy*dy + dz*dz > radiusSq * 4.0f) continue;
        }
        if (std::isnan(a.location.x) || std::isnan(a.location.y) || std::isnan(a.location.z)) continue;

        // Light's glow color (from actual light color)
        float lr = a.lightColorR / 255.0f;
        float lg = a.lightColorG / 255.0f;
        float lb = a.lightColorB / 255.0f;

        // Glow size proportional to light radius (clamped)
        float dist = sqrtf((a.location.x - camPos.x) * (a.location.x - camPos.x) +
                           (a.location.y - camPos.y) * (a.location.y - camPos.y) +
                           (a.location.z - camPos.z) * (a.location.z - camPos.z));
        float glowSize = a.lightRadius * 0.3f;
        if (glowSize < 60.0f) glowSize = 60.0f;
        if (glowSize > 800.0f) glowSize = 800.0f;
        // Also scale with distance to maintain some screen presence
        float distScale = dist * 0.015f;
        if (distScale > glowSize) glowSize = distScale;

        Mat4 model;
        memset(&model, 0, sizeof(model));
        model.m[0] = camRight.x * glowSize;
        model.m[1] = camRight.y * glowSize;
        model.m[2] = camRight.z * glowSize;
        model.m[4] = camUp.x * glowSize;
        model.m[5] = camUp.y * glowSize;
        model.m[6] = camUp.z * glowSize;
        model.m[8] = 0; model.m[9] = 0; model.m[10] = 1;
        model.m[12] = a.location.x;
        model.m[13] = a.location.y;
        model.m[14] = a.location.z;
        model.m[15] = 1.0f;

        Mat4 mvpGlow = MulMat4(vp2, model);
        glUniformMatrix4fv(m_LocIconMVP, 1, GL_FALSE, mvpGlow.m);
        // Attenuate glow with distance for subtlety
        float attenuate = 1.0f - (dist / (m_DrawRadius * 2.0f));
        if (attenuate < 0.1f) attenuate = 0.1f;
        glUniform3f(m_LocIconColor, lr * attenuate * 0.6f, lg * attenuate * 0.6f, lb * attenuate * 0.6f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        m_DrawCalls++;
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
}

// Project world point to screen coordinates [0..vw, 0..vh]
static bool WorldToScreen(const Vec3& world, const float* vpMat, float vw, float vh, float& sx, float& sy)
{
    float x = world.x, y = world.y, z = world.z;
    float clipX = vpMat[0]*x + vpMat[4]*y + vpMat[8]*z + vpMat[12];
    float clipY = vpMat[1]*x + vpMat[5]*y + vpMat[9]*z + vpMat[13];
    float clipW = vpMat[3]*x + vpMat[7]*y + vpMat[11]*z + vpMat[15];
    if (clipW < 0.001f) return false;
    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;
    sx = (ndcX * 0.5f + 0.5f) * vw;
    sy = (1.0f - (ndcY * 0.5f + 0.5f)) * vh;
    return true;
}

// Distance from point P to line segment AB (2D screen space)
static float PointToSegmentDist2D(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float len2 = dx*dx + dy*dy;
    if (len2 < 0.001f) return sqrtf((px-ax)*(px-ax) + (py-ay)*(py-ay));
    float t = ((px-ax)*dx + (py-ay)*dy) / len2;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float cx = ax + t * dx, cy = ay + t * dy;
    return sqrtf((px-cx)*(px-cx) + (py-cy)*(py-cy));
}

// Get axis direction vector
static Vec3 GizmoAxisDir(GizmoAxis axis)
{
    switch (axis) {
        case GizmoAxis::X: return {1,0,0};
        case GizmoAxis::Y: return {0,1,0};
        case GizmoAxis::Z: return {0,0,1};
        default: return {0,0,0};
    }
}

// Compute parameter along axis closest to mouse ray
static float RayAxisParameter(const Vec3& rayO, const Vec3& rayD, const Vec3& axisO, const Vec3& axisD)
{
    // Solve for closest point between two lines
    Vec3 w = {axisO.x - rayO.x, axisO.y - rayO.y, axisO.z - rayO.z};
    float b = rayD.x*axisD.x + rayD.y*axisD.y + rayD.z*axisD.z;
    float d = rayD.x*w.x + rayD.y*w.y + rayD.z*w.z;
    float e = axisD.x*w.x + axisD.y*w.y + axisD.z*w.z;
    float denom = 1.0f - b*b;
    if (fabsf(denom) < 0.0001f) return 0.0f; // parallel
    return (b*d - e) / denom;
}

// Unproject screen coordinates to world ray
static void ScreenToRay(float mx, float my, float vw, float vh, const float* vpMat, Vec3& rayO, Vec3& rayD)
{
    // Invert VP matrix (simple cofactor method for 4x4)
    // We'll use a simplified approach: unproject two points on near/far plane
    float invVP[16];
    // -- 4x4 matrix inverse --
    const float* m = vpMat;
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-10f) { rayO = {0,0,0}; rayD = {0,0,1}; return; }
    float invDet = 1.0f / det;
    for (int i = 0; i < 16; i++) invVP[i] = inv[i] * invDet;

    // NDC coordinates
    float ndcX = (mx / vw) * 2.0f - 1.0f;
    float ndcY = 1.0f - (my / vh) * 2.0f;

    // Unproject near point (z=-1)
    float nw = invVP[3]*ndcX + invVP[7]*ndcY + invVP[11]*(-1.0f) + invVP[15];
    float nx = (invVP[0]*ndcX + invVP[4]*ndcY + invVP[8]*(-1.0f) + invVP[12]) / nw;
    float ny = (invVP[1]*ndcX + invVP[5]*ndcY + invVP[9]*(-1.0f) + invVP[13]) / nw;
    float nz = (invVP[2]*ndcX + invVP[6]*ndcY + invVP[10]*(-1.0f) + invVP[14]) / nw;

    // Unproject far point (z=1)
    float fw = invVP[3]*ndcX + invVP[7]*ndcY + invVP[11]*(1.0f) + invVP[15];
    float fx = (invVP[0]*ndcX + invVP[4]*ndcY + invVP[8]*(1.0f) + invVP[12]) / fw;
    float fy = (invVP[1]*ndcX + invVP[5]*ndcY + invVP[9]*(1.0f) + invVP[13]) / fw;
    float fz = (invVP[2]*ndcX + invVP[6]*ndcY + invVP[10]*(1.0f) + invVP[14]) / fw;

    rayO = {nx, ny, nz};
    float dx = fx - nx, dy = fy - ny, dz = fz - nz;
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 0.0001f) len = 1.0f;
    rayD = {dx/len, dy/len, dz/len};
}

void Viewport::DrawGizmo(const Vec3& actorPos, const Mat4& vp)
{
    // Scale gizmo to maintain fixed screen size
    Vec3 camPos = m_Camera.GetPosition();
    float dx = actorPos.x - camPos.x, dy = actorPos.y - camPos.y, dz = actorPos.z - camPos.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    float scale = dist * 0.08f;
    if (scale < 50.0f) scale = 50.0f;

    Mat4 model = MulMat4(TranslateMat4(actorPos.x, actorPos.y, actorPos.z), ScaleMat4(scale));
    Mat4 mvp = MulMat4(vp, model);

    glDisable(GL_DEPTH_TEST); // gizmo always on top
    glUniform1i(m_LocLit, 0);
    glUniform1i(m_LocHasTexture, 0);
    glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, model.m);
    glBindVertexArray(m_GizmoVAO);

    // Draw thick axis lines
    glLineWidth(3.0f);
    float xBright = (m_GizmoHoverAxis == GizmoAxis::X || m_GizmoActiveAxis == GizmoAxis::X) ? 1.0f : 0.7f;
    float yBright = (m_GizmoHoverAxis == GizmoAxis::Y || m_GizmoActiveAxis == GizmoAxis::Y) ? 1.0f : 0.7f;
    float zBright = (m_GizmoHoverAxis == GizmoAxis::Z || m_GizmoActiveAxis == GizmoAxis::Z) ? 1.0f : 0.7f;
    glUniform3f(m_LocColor, xBright, 0.15f, 0.15f);
    glDrawArrays(GL_LINES, 0, 2);
    glUniform3f(m_LocColor, 0.15f, yBright, 0.15f);
    glDrawArrays(GL_LINES, 2, 2);
    glUniform3f(m_LocColor, 0.2f, 0.3f, zBright);
    glDrawArrays(GL_LINES, 4, 2);
    glLineWidth(1.0f);

    // Draw arrow tips (diamonds at each axis end)
    float tipScale = scale * 0.12f;
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_DiamondVAO);

    // X tip
    Mat4 tipM = MulMat4(TranslateMat4(actorPos.x + scale, actorPos.y, actorPos.z),
                        ScaleMat4xyz(tipScale*0.6f, tipScale*0.6f, tipScale*1.5f));
    // Rotate diamond to point along X: swap X and Z
    // Actually just use non-uniform scale to elongate along X
    tipM = MulMat4(TranslateMat4(actorPos.x + scale, actorPos.y, actorPos.z),
                   ScaleMat4xyz(tipScale*1.5f, tipScale*0.6f, tipScale*0.6f));
    Mat4 tipMVP = MulMat4(vp, tipM);
    glUniform3f(m_LocColor, xBright, 0.15f, 0.15f);
    glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, tipMVP.m);
    glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, tipM.m);
    glDrawArrays(GL_TRIANGLES, 0, m_DiamondVertCount);

    // Y tip
    tipM = MulMat4(TranslateMat4(actorPos.x, actorPos.y + scale, actorPos.z),
                   ScaleMat4xyz(tipScale*0.6f, tipScale*1.5f, tipScale*0.6f));
    tipMVP = MulMat4(vp, tipM);
    glUniform3f(m_LocColor, 0.15f, yBright, 0.15f);
    glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, tipMVP.m);
    glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, tipM.m);
    glDrawArrays(GL_TRIANGLES, 0, m_DiamondVertCount);

    // Z tip
    tipM = MulMat4(TranslateMat4(actorPos.x, actorPos.y, actorPos.z + scale),
                   ScaleMat4xyz(tipScale*0.6f, tipScale*0.6f, tipScale*1.5f));
    tipMVP = MulMat4(vp, tipM);
    glUniform3f(m_LocColor, 0.2f, 0.3f, zBright);
    glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, tipMVP.m);
    glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, tipM.m);
    glDrawArrays(GL_TRIANGLES, 0, m_DiamondVertCount);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

GizmoAxis Viewport::HitTestGizmo(const Vec3& actorPos, float mx, float my, float vw, float vh, const Mat4& vp)
{
    // Scale gizmo same as in DrawGizmo
    Vec3 camPos = m_Camera.GetPosition();
    float dx = actorPos.x - camPos.x, dy = actorPos.y - camPos.y, dz = actorPos.z - camPos.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    float scale = dist * 0.08f;
    if (scale < 50.0f) scale = 50.0f;

    const float threshold = 12.0f; // pixels

    // Project origin and each axis tip to screen
    float ox, oy;
    if (!WorldToScreen(actorPos, vp.m, vw, vh, ox, oy)) return GizmoAxis::None;

    Vec3 tips[3] = {
        {actorPos.x + scale, actorPos.y, actorPos.z},
        {actorPos.x, actorPos.y + scale, actorPos.z},
        {actorPos.x, actorPos.y, actorPos.z + scale},
    };
    GizmoAxis axes[3] = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};

    float bestDist = threshold;
    GizmoAxis bestAxis = GizmoAxis::None;
    for (int i = 0; i < 3; i++) {
        float tx, ty;
        if (!WorldToScreen(tips[i], vp.m, vw, vh, tx, ty)) continue;
        float d = PointToSegmentDist2D(mx, my, ox, oy, tx, ty);
        if (d < bestDist) {
            bestDist = d;
            bestAxis = axes[i];
        }
    }
    return bestAxis;
}

bool Viewport::BeginGizmoDrag(const Vec3& actorPos, float mx, float my, float vw, float vh, const Mat4& vp)
{
    GizmoAxis hit = HitTestGizmo(actorPos, mx, my, vw, vh, vp);
    if (hit == GizmoAxis::None) return false;

    m_GizmoActiveAxis = hit;
    m_GizmoDragging = true;
    m_GizmoDragStartPos = actorPos;

    // Compute initial axis parameter
    Vec3 rayO, rayD;
    ScreenToRay(mx, my, vw, vh, vp.m, rayO, rayD);
    Vec3 axisDir = GizmoAxisDir(hit);
    m_GizmoDragStartT = RayAxisParameter(rayO, rayD, actorPos, axisDir);
    return true;
}

Vec3 Viewport::UpdateGizmoDrag(const Vec3& actorPos, float mx, float my, float vw, float vh, const Mat4& vp)
{
    if (!m_GizmoDragging || m_GizmoActiveAxis == GizmoAxis::None)
        return actorPos;

    Vec3 rayO, rayD;
    ScreenToRay(mx, my, vw, vh, vp.m, rayO, rayD);
    Vec3 axisDir = GizmoAxisDir(m_GizmoActiveAxis);
    float t = RayAxisParameter(rayO, rayD, m_GizmoDragStartPos, axisDir);
    float delta = t - m_GizmoDragStartT;

    Vec3 newPos = m_GizmoDragStartPos;
    newPos.x += axisDir.x * delta;
    newPos.y += axisDir.y * delta;
    newPos.z += axisDir.z * delta;
    return newPos;
}

void Viewport::EndGizmoDrag()
{
    m_GizmoDragging = false;
    m_GizmoActiveAxis = GizmoAxis::None;
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
    m_Time += 0.016f; // ~60fps time accumulator for animations

    // Get current viewport size for bloom FBO resize
    GLint vp_cur[4];
    glGetIntegerv(GL_VIEWPORT, vp_cur);
    int vpW = vp_cur[2], vpH = vp_cur[3];
    if (vpW > 0 && vpH > 0 && m_BloomEnabled) {
        ResizeBloomFBOs(vpW, vpH);
        glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFBO);
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    // Match clear color to fog for seamless blending, else UE-style dark viewport
    if (m_Lighting.fogEnabled) {
        glClearColor(m_Lighting.fogColor[0], m_Lighting.fogColor[1], m_Lighting.fogColor[2], 1.0f);
    } else {
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(m_ShaderProgram);
    glUniform1f(m_LocAlpha, 1.0f); // default: fully opaque

    // --- Upload all lighting uniforms once per frame ---
    Vec3 camPos = m_Camera.GetPosition();
    glUniform3f(m_LocCamPos, camPos.x, camPos.y, camPos.z);

    // Sun direction from yaw/pitch angles
    {
        float yawRad = m_Lighting.sunYaw * 3.14159265f / 180.0f;
        float pitchRad = m_Lighting.sunPitch * 3.14159265f / 180.0f;
        float cp = cosf(pitchRad);
        float sunX = cosf(yawRad) * cp;
        float sunY = sinf(yawRad) * cp;
        float sunZ = sinf(pitchRad);
        // Normalize
        float len = sqrtf(sunX*sunX + sunY*sunY + sunZ*sunZ);
        if (len > 0.001f) { sunX /= len; sunY /= len; sunZ /= len; }
        if (m_Lighting.sunEnabled) {
            glUniform3f(m_LocSunDir, sunX, sunY, sunZ);
            glUniform3f(m_LocSunColor,
                m_Lighting.sunColor[0] * m_Lighting.sunIntensity,
                m_Lighting.sunColor[1] * m_Lighting.sunIntensity,
                m_Lighting.sunColor[2] * m_Lighting.sunIntensity);
        } else {
            glUniform3f(m_LocSunDir, 0, 0, 1);
            glUniform3f(m_LocSunColor, 0, 0, 0);
        }
        // Fill light: opposite direction, cooler
        glUniform3f(m_LocFillDir, -sunX, -sunY, -sunZ * 0.5f);
        glUniform3f(m_LocFillColor,
            m_Lighting.fillColor[0] * m_Lighting.fillIntensity,
            m_Lighting.fillColor[1] * m_Lighting.fillIntensity,
            m_Lighting.fillColor[2] * m_Lighting.fillIntensity);
    }

    // Ambient
    glUniform3f(m_LocAmbientSky,
        m_Lighting.ambientSkyColor[0] * m_Lighting.ambientIntensity,
        m_Lighting.ambientSkyColor[1] * m_Lighting.ambientIntensity,
        m_Lighting.ambientSkyColor[2] * m_Lighting.ambientIntensity);
    glUniform3f(m_LocAmbientGround,
        m_Lighting.ambientGroundColor[0] * m_Lighting.ambientIntensity,
        m_Lighting.ambientGroundColor[1] * m_Lighting.ambientIntensity,
        m_Lighting.ambientGroundColor[2] * m_Lighting.ambientIntensity);

    // Fog
    glUniform3f(m_LocFogColor, m_Lighting.fogColor[0], m_Lighting.fogColor[1], m_Lighting.fogColor[2]);
    glUniform3f(m_LocFogParams, m_Lighting.fogStart, m_Lighting.fogEnd,
        m_Lighting.fogEnabled ? 1.0f : 0.0f);

    // Exposure / gamma / tone mapping
    float finalExposure = m_Lighting.exposure;
    float finalGamma = m_Lighting.gamma;
    if (m_GamePreview && m_DarkPreview) {
        // BioShock Dark Preview: dramatically reduce brightness
        finalExposure = 0.4f;
        finalGamma = 2.4f;
        // Override ambient to near-black
        glUniform3f(m_LocAmbientSky, 0.02f, 0.025f, 0.04f);
        glUniform3f(m_LocAmbientGround, 0.01f, 0.01f, 0.01f);
        // Override sun to very dim
        glUniform3f(m_LocSunColor, 0.05f, 0.04f, 0.03f);
        glUniform3f(m_LocFillColor, 0.02f, 0.02f, 0.03f);
        // Enable atmospheric fog
        glUniform3f(m_LocFogColor, 0.01f, 0.015f, 0.025f);
        glUniform3f(m_LocFogParams, 500.0f, 8000.0f, 1.0f);
    }
    glUniform3f(m_LocExposureGamma, finalExposure, finalGamma,
        m_Lighting.toneMapping ? 1.0f : 0.0f);

    // Point lights — upload closest N to camera
    if (m_Lighting.useLevelLights && !m_SceneLights.empty()) {
        // Sort by distance to camera (cheap: just compute dist², pick closest N)
        struct LightDist { int idx; float d2; };
        std::vector<LightDist> ld(m_SceneLights.size());
        for (int i = 0; i < (int)m_SceneLights.size(); i++) {
            float dx = m_SceneLights[i].pos[0] - camPos.x;
            float dy = m_SceneLights[i].pos[1] - camPos.y;
            float dz = m_SceneLights[i].pos[2] - camPos.z;
            ld[i] = {i, dx*dx + dy*dy + dz*dz};
        }
        // Partial sort: get closest MAX_POINT_LIGHTS using O(N) nth_element
        int count = (int)ld.size();
        if (count > MAX_POINT_LIGHTS) {
            std::nth_element(ld.begin(), ld.begin() + MAX_POINT_LIGHTS, ld.end(),
                [](const LightDist& a, const LightDist& b) { return a.d2 < b.d2; });
            count = MAX_POINT_LIGHTS;
        }
        glUniform1i(m_LocNumPointLights, count);
        float mult = m_Lighting.pointLightMultiplier;
        if (m_GamePreview && m_DarkPreview) mult *= 0.35f; // darker point lights in BioShock preview
        for (int i = 0; i < count; i++) {
            auto& sl = m_SceneLights[ld[i].idx];
            glUniform4f(m_LocPLightPosR[i], sl.pos[0], sl.pos[1], sl.pos[2], sl.radius);
            glUniform3f(m_LocPLightColor[i],
                sl.color[0] * sl.brightness * mult,
                sl.color[1] * sl.brightness * mult,
                sl.color[2] * sl.brightness * mult);
        }
    } else {
        glUniform1i(m_LocNumPointLights, 0);
    }

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
    Mat4 proj;
    if (m_Orthographic) {
        float halfW = m_OrthoSize * aspect;
        float halfH = m_OrthoSize;
        proj = Camera::Orthographic(halfW, halfH, m_Camera.nearPlane, m_Camera.farPlane);
    } else {
        proj = m_Camera.GetProjectionMatrix(aspect);
    }
    Mat4 vp_mat = MulMat4(proj, view);
    memcpy(m_LastVP, vp_mat.m, sizeof(m_LastVP));

    // Identity model matrix for grid/unlit things
    Mat4 identity;
    memset(&identity, 0, sizeof(identity));
    identity.m[0] = 1; identity.m[5] = 1; identity.m[10] = 1; identity.m[15] = 1;

    // Reset draw stats
    m_DrawCalls = 0;
    m_TriCount = 0;

    // --- SHADOW MAP PASS ---
    if (m_ShadowEnabled && m_Lighting.sunEnabled && doc.IsLoaded()) {
        float yawRad = m_Lighting.sunYaw * 3.14159265f / 180.0f;
        float pitchRad = m_Lighting.sunPitch * 3.14159265f / 180.0f;
        float cp = cosf(pitchRad);
        float sunX = cosf(yawRad) * cp;
        float sunY = sinf(yawRad) * cp;
        float sunZ = sinf(pitchRad);
        // Light ortho projection centered on camera
        float shadowRange = m_DrawRadiusEnabled ? m_DrawRadius : 20000.0f;
        Vec3 lightDir = {sunX, sunY, sunZ};
        Vec3 lightPos = {camPos.x + lightDir.x * shadowRange,
                         camPos.y + lightDir.y * shadowRange,
                         camPos.z + lightDir.z * shadowRange};
        Vec3 lightTarget = camPos;
        Mat4 lightView = Camera::LookAt(lightPos, lightTarget, {0,0,1});
        Mat4 lightProj = Camera::Orthographic(shadowRange, shadowRange, 1.0f, shadowRange * 3.0f);
        Mat4 lightVP = MulMat4(lightProj, lightView);

        // Render to shadow FBO
        glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowFBO);
        glViewport(0, 0, m_ShadowMapSize, m_ShadowMapSize);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUseProgram(m_DepthShader);
        glEnable(GL_DEPTH_TEST);
        glCullFace(GL_FRONT); // Reduce shadow acne

        // Render BSP into shadow map
        for (auto& gpu : m_BSPGPU) {
            if (gpu.vao == 0 || gpu.indexCount == 0) continue;
            glUniformMatrix4fv(m_LocDepthMVP, 1, GL_FALSE, lightVP.m);
            glBindVertexArray(gpu.vao);
            glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_INT, nullptr);
        }

        // Render actor meshes into shadow map
        auto& actors = doc.GetActors();
        for (int i = 0; i < (int)actors.size(); i++) {
            auto& a = actors[i];
            if (!a.visible || a.meshIndex < 0 || a.meshIndex >= (int)m_MeshGPU.size()) continue;
            auto& gpu = m_MeshGPU[a.meshIndex];
            if (gpu.vao == 0 || gpu.indexCount == 0) continue;
            if (std::isnan(a.location.x) || std::isinf(a.location.x)) continue;
            Mat4 model = BuildActorTransform(a.location, a.rotation, a.scale);
            Mat4 mvp = MulMat4(lightVP, model);
            glUniformMatrix4fv(m_LocDepthMVP, 1, GL_FALSE, mvp.m);
            glBindVertexArray(gpu.vao);
            glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_INT, nullptr);
        }

        glCullFace(GL_BACK);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(vp[0], vp[1], vp[2], vp[3]);

        // Store shadow matrix for main pass
        m_ShadowMat = lightVP;

        // Switch back to main shader
        glUseProgram(m_ShaderProgram);
    }

    // Bind shadow map to texture unit 1
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_ShadowDepthTex);
    glUniform1i(m_LocShadowMap, 1);
    glUniform1i(m_LocShadowEnabled, (m_ShadowEnabled && m_Lighting.sunEnabled) ? 1 : 0);
    if (m_ShadowEnabled && m_Lighting.sunEnabled) {
        glUniformMatrix4fv(m_LocShadowMat, 1, GL_FALSE, m_ShadowMat.m);
    }
    glActiveTexture(GL_TEXTURE0);
    
    // Draw grid (unlit) with colored axis lines — hidden in Game Preview
    if (!m_GamePreview) {
        glUniform1i(m_LocLit, 0);
        glUniform1i(m_LocHasTexture, 0);
        glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, identity.m);
        glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, vp_mat.m);
        glBindVertexArray(m_GridVAO);
        // Grey grid lines
        glUniform3f(m_LocColor, 0.18f, 0.18f, 0.22f);
        glDrawArrays(GL_LINES, 0, m_GridVertCount);
        // Axis lines: X=red, Y=green, Z=blue (2 verts each, starting at m_AxisVertStart)
        glLineWidth(2.0f);
        glUniform3f(m_LocColor, 0.7f, 0.15f, 0.15f);
        glDrawArrays(GL_LINES, m_AxisVertStart, 2);
        glUniform3f(m_LocColor, 0.15f, 0.7f, 0.15f);
        glDrawArrays(GL_LINES, m_AxisVertStart + 2, 2);
        glUniform3f(m_LocColor, 0.2f, 0.3f, 0.8f);
        glDrawArrays(GL_LINES, m_AxisVertStart + 4, 2);
        glLineWidth(1.0f);
    }

    // Draw radius for distance culling (camPos already set above for lighting)
    float radiusSq = m_DrawRadius * m_DrawRadius;

    // Draw BSP geometry (level shell — walls, floors, ceilings)
    if (!m_BSPGPU.empty()) {
        // Disable culling for BSP: interior environments need both face sides visible
        glDisable(GL_CULL_FACE);

        // Debug color helper: deterministic HSV-based color from index
        auto debugColor = [](int idx, float& r, float& g, float& b) {
            // Golden ratio hash for well-distributed hues
            float h = fmodf(idx * 0.618033988f, 1.0f);
            float s = 0.7f, v = 0.9f;
            // HSV to RGB
            int hi = (int)(h * 6.0f) % 6;
            float f = h * 6.0f - hi;
            float p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
            switch (hi) {
                case 0: r=v; g=t; b=p; break;
                case 1: r=q; g=v; b=p; break;
                case 2: r=p; g=v; b=t; break;
                case 3: r=p; g=q; b=v; break;
                case 4: r=t; g=p; b=v; break;
                default: r=v; g=p; b=q; break;
            }
        };

        // Heat map: value 0..1 → blue→cyan→green→yellow→red
        auto heatColor = [](float t, float& r, float& g, float& b) {
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            if (t < 0.25f)      { r=0;          g=t*4;       b=1; }
            else if (t < 0.5f)  { r=0;          g=1;         b=1-(t-0.25f)*4; }
            else if (t < 0.75f) { r=(t-0.5f)*4; g=1;         b=0; }
            else                { r=1;          g=1-(t-0.75f)*4; b=0; }
        };

        // Set shader mode based on ViewMode
        int litMode = 1;
        bool wireframe = m_WireframeBSP;
        bool debugMode = false;
        switch (m_ViewMode) {
            case ViewMode::Lit:          litMode = 1; break;
            case ViewMode::Unlit:        litMode = 2; break;
            case ViewMode::Wireframe:    litMode = 1; wireframe = true; break;
            case ViewMode::LitNoTexture: litMode = 3; break;
            case ViewMode::SurfaceID:    litMode = 2; debugMode = true; break;
            case ViewMode::ZoneColor:    litMode = 2; debugMode = true; break;
            case ViewMode::LightmapScale:litMode = 2; debugMode = true; break;
            default: break;
        }
        glUniform1i(m_LocLit, litMode);
        glUniform1i(m_LocTexSampler, 0);
        glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, vp_mat.m);
        glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, identity.m);

        if (wireframe)
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

        glUniform1f(m_LocEmissive, 0.0f); // BSP is not emissive
        glUniform1i(m_LocHasNormalMap, 0); // BSP uses vertex normals only
        glUniform1i(m_LocHasSpecMap, 0);   // BSP no specular map
        for (int ci = 0; ci < (int)m_BSPGPU.size(); ci++) {
            auto& gpu = m_BSPGPU[ci];
            if (gpu.vao == 0 || gpu.indexCount == 0) continue;
            if (gpu.isWater) continue; // water rendered in separate pass

            // Distance cull
            if (m_DrawRadiusEnabled) {
                float dx = gpu.centerX - camPos.x;
                float dy = gpu.centerY - camPos.y;
                float dz = gpu.centerZ - camPos.z;
                if (dx*dx + dy*dy + dz*dz > radiusSq) continue;
            }

            // Zone visibility
            if (m_ZoneFilterEnabled && m_CameraZone >= 0 && m_CameraZone < 128) {
                int byteIdx = m_CameraZone / 8;
                int bitIdx = m_CameraZone % 8;
                if (!(gpu.zoneMask[byteIdx] & (1 << bitIdx))) continue;
            }

            glBindVertexArray(gpu.vao);

            // Debug view modes: set per-chunk color, disable textures
            if (debugMode) {
                glUniform1i(m_LocHasTexture, 0);
                float r, g, b;
                if (m_ViewMode == ViewMode::SurfaceID) {
                    debugColor(ci, r, g, b);
                } else if (m_ViewMode == ViewMode::ZoneColor) {
                    debugColor(gpu.zoneIndex * 7 + 3, r, g, b);
                } else if (m_ViewMode == ViewMode::LightmapScale) {
                    // Normalize LM scale: 0=no LM (blue), 8=green, 32=yellow, 128+=red
                    float t = gpu.lightMapScale > 0.0f ? logf(gpu.lightMapScale) / logf(128.0f) : 0.0f;
                    heatColor(t, r, g, b);
                    if (gpu.lightMapScale <= 0.0f) { r = 0.15f; g = 0.15f; b = 0.15f; } // no LM = dark gray
                } else {
                    r = g = b = 0.5f;
                }
                glUniform3f(m_LocColor, r, g, b);
            } else if (gpu.textureId) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, gpu.textureId);
                glUniform1i(m_LocHasTexture, 1);
                glUniform3f(m_LocColor, 1.0f, 1.0f, 1.0f);
            } else if (m_MissingMaterialDebug) {
                // Magenta = missing material
                glUniform1i(m_LocHasTexture, 0);
                glUniform3f(m_LocColor, 0.9f, 0.0f, 0.9f);
            } else if (m_DefaultTexture) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_DefaultTexture);
                glUniform1i(m_LocHasTexture, 1);
                glUniform3f(m_LocColor, 1.0f, 1.0f, 1.0f);
            } else {
                glUniform1i(m_LocHasTexture, 0);
                glUniform3f(m_LocColor, 0.45f, 0.45f, 0.50f);
            }
            glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT, nullptr);
            m_DrawCalls++;
            m_TriCount += gpu.indexCount / 3;
        }

        // Water pass: render water surfaces with transparency
        if (m_WaterEnabled) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE); // don't write depth for transparent water
            glUniform1f(m_LocAlpha, 0.4f); // semi-transparent water
            glUniform1i(m_LocHasTexture, 0);
            glUniform1i(m_LocHasNormalMap, 0);
            glUniform1i(m_LocHasSpecMap, 0);
            // Dark blue-green tint for Rapture underwater look
            glUniform3f(m_LocColor, 0.02f, 0.08f, 0.12f);
            glUniform1f(m_LocEmissive, 0.3f); // slightly self-illuminated

            for (auto& gpu : m_BSPGPU) {
                if (!gpu.isWater || gpu.vao == 0 || gpu.indexCount == 0) continue;
                if (m_DrawRadiusEnabled) {
                    float dx = gpu.centerX - camPos.x;
                    float dy = gpu.centerY - camPos.y;
                    float dz = gpu.centerZ - camPos.z;
                    if (dx*dx + dy*dy + dz*dz > radiusSq) continue;
                }
                if (m_ZoneFilterEnabled && m_CameraZone >= 0 && m_CameraZone < 128) {
                    int byteIdx = m_CameraZone / 8;
                    int bitIdx = m_CameraZone % 8;
                    if (!(gpu.zoneMask[byteIdx] & (1 << bitIdx))) continue;
                }
                glBindVertexArray(gpu.vao);
                glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT, nullptr);
                m_DrawCalls++;
                m_TriCount += gpu.indexCount / 3;
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glUniform1f(m_LocAlpha, 1.0f); // restore opaque
            glUniform1f(m_LocEmissive, 0.0f);
        }

        if (wireframe)
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);
    }

    // Draw actors - solid lit meshes like a real level editor
    if (doc.IsLoaded()) {
        auto& actors = doc.GetActors();
        
        // Set texture samplers
        glUniform1i(m_LocTexSampler, 0);
        glUniform1i(m_LocNormalMap, 2); // normal map on unit 2
        glUniform1i(m_LocSpecMap, 3);   // specular map on unit 3
        
        // Render all meshes with current view mode lighting
        int actorLitMode = 1;
        switch (m_ViewMode) {
            case ViewMode::Lit:          actorLitMode = 1; break;
            case ViewMode::Unlit:        actorLitMode = 2; break;
            case ViewMode::Wireframe:    actorLitMode = 1; break;
            case ViewMode::LitNoTexture: actorLitMode = 3; break;
            default: break;
        }
        glUniform1i(m_LocLit, actorLitMode);
        for (int i = 0; i < (int)actors.size(); i++) {
            auto& a = actors[i];
            if (!a.visible) continue;
            // Game Preview: only render actors classified as visible in-game
            if (m_GamePreview && !IsVisibleInGame(a.renderType)) continue;
            if (a.meshIndex < 0 || a.meshIndex >= (int)m_MeshGPU.size()) continue;
            auto& gpu = m_MeshGPU[a.meshIndex];
            if (gpu.vao == 0 || gpu.indexCount == 0) continue;

            bool isSel = (i == selectedActor) || a.selected;

            // Skip actors with NaN/Inf in transform (corrupted properties)
            if (std::isnan(a.location.x) || std::isnan(a.location.y) || std::isnan(a.location.z) ||
                std::isnan(a.rotation.x) || std::isnan(a.rotation.y) || std::isnan(a.rotation.z) ||
                std::isnan(a.scale.x) || std::isnan(a.scale.y) || std::isnan(a.scale.z) ||
                std::isinf(a.location.x) || std::isinf(a.location.y) || std::isinf(a.location.z) ||
                a.scale.x < 0.0001f || a.scale.y < 0.0001f || a.scale.z < 0.0001f) continue;

            // Distance cull actors too (use same radius as BSP)
            if (m_DrawRadiusEnabled) {
                float dx = a.location.x - camPos.x;
                float dy = a.location.y - camPos.y;
                float dz = a.location.z - camPos.z;
                if (dx*dx + dy*dy + dz*dz > radiusSq) continue;
            }

            Mat4 model = BuildActorTransform(a.location, a.rotation, a.scale);
            Mat4 mvp = MulMat4(vp_mat, model);
            glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, model.m);
            glBindVertexArray(gpu.vao);

            // Two-sided materials (foliage, glass, FacingShaders)
            if (gpu.isTwoSided) glDisable(GL_CULL_FACE);

            // Set emissive for self-illuminating meshes
            glUniform1f(m_LocEmissive, gpu.isEmissive ? 1.5f : 0.0f);

            // Bind texture — use checkerboard fallback for untextured meshes
            if (isSel) {
                glUniform1i(m_LocHasTexture, 0);
                glUniform3f(m_LocColor, 0.9f, 0.5f, 0.1f);
            } else if (gpu.textureId) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, gpu.textureId);
                glUniform1i(m_LocHasTexture, 1);
                glUniform3f(m_LocColor, 1.0f, 1.0f, 1.0f);
            } else if (m_MissingMaterialDebug) {
                // Magenta = missing material debug
                glUniform1i(m_LocHasTexture, 0);
                glUniform3f(m_LocColor, 0.9f, 0.0f, 0.9f);
            } else if (m_DefaultTexture) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_DefaultTexture);
                glUniform1i(m_LocHasTexture, 1);
                glUniform3f(m_LocColor, 1.0f, 1.0f, 1.0f);
            } else {
                glUniform1i(m_LocHasTexture, 0);
                glUniform3f(m_LocColor, 0.55f, 0.55f, 0.6f);
            }
            // Bind normal map if available
            if (gpu.normalMapId && !isSel) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, gpu.normalMapId);
                glUniform1i(m_LocHasNormalMap, 1);
            } else {
                glUniform1i(m_LocHasNormalMap, 0);
            }
            // Bind specular map if available
            if (gpu.specularMapId && !isSel) {
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, gpu.specularMapId);
                glUniform1i(m_LocHasSpecMap, 1);
            } else {
                glUniform1i(m_LocHasSpecMap, 0);
            }
            glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT, nullptr);
            if (gpu.isTwoSided) glEnable(GL_CULL_FACE);
            m_DrawCalls++;
            m_TriCount += gpu.indexCount / 3;

            // Pulsing wireframe overlay for selected actor
            if (isSel) {
                float pulse = sinf(m_Time * 4.0f) * 0.5f + 0.5f; // 0..1 pulsing
                float r = 1.0f;
                float g = 0.6f + pulse * 0.4f;
                float b = pulse * 0.3f;
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glLineWidth(2.0f);
                glUniform1i(m_LocLit, 0);
                glUniform3f(m_LocColor, r, g, b);
                glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT, nullptr);
                glLineWidth(1.0f);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glUniform1i(m_LocLit, actorLitMode);
            }
        }
        
        // Render actors with unmatched/missing meshes as wireframe boxes
        // In Game Preview mode: hide all placeholders (unless ShowUnknownActors is on)
        if (!m_GamePreview || m_ShowUnknownActors) {
            glUniform1i(m_LocLit, 0);
            glUniform1i(m_LocHasTexture, 0);
            glUniform1f(m_LocEmissive, 0.0f);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glDisable(GL_CULL_FACE);
            for (int i = 0; i < (int)actors.size(); i++) {
                auto& a = actors[i];
                if (!a.visible || !a.hasLocation) continue;
                if (a.isLight) continue; // lights have their own rendering
                // Only show actors that should have visible geometry but don't
                bool hasMeshGPU = (a.meshIndex >= 0 && a.meshIndex < (int)m_MeshGPU.size() &&
                                   m_MeshGPU[a.meshIndex].vao != 0 && m_MeshGPU[a.meshIndex].indexCount > 0);
                if (hasMeshGPU) continue; // already rendered above
                // In Game Preview: only show Unknown placeholders if enabled
                if (m_GamePreview) {
                    if (a.renderType != ActorRenderType::UnknownPlaceholder) continue;
                } else {
                    // Editor mode: show actors that SHOULD have a mesh but don't
                    if (a.renderType != ActorRenderType::VisibleStaticMesh &&
                        a.renderType != ActorRenderType::VisibleMover &&
                        a.renderType != ActorRenderType::VisiblePickup &&
                        a.renderType != ActorRenderType::VisibleDecoration &&
                        a.renderType != ActorRenderType::UnknownPlaceholder) continue;
                }

                if (std::isnan(a.location.x) || std::isnan(a.location.y) || std::isnan(a.location.z)) continue;

                float dx = a.location.x - camPos.x;
                float dy = a.location.y - camPos.y;
                float dz = a.location.z - camPos.z;
                float distSq = dx*dx + dy*dy + dz*dz;
                if (m_DrawRadiusEnabled && distSq > radiusSq) continue;

                bool isSel = (i == selectedActor) || a.selected;
                // Screen-space constant size: 20 world units at 1000 distance, scales with distance
                float dist = sqrtf(distSq);
                float boxSize = dist * 0.025f;
                if (boxSize < 15.0f) boxSize = 15.0f;
                if (boxSize > 80.0f) boxSize = 80.0f;
                Mat4 model = MulMat4(
                    TranslateMat4(a.location.x, a.location.y, a.location.z),
                    ScaleMat4(boxSize));
                Mat4 mvp = MulMat4(vp_mat, model);
                glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp.m);
                glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, model.m);
                if (isSel)
                    glUniform3f(m_LocColor, 1.0f, 0.7f, 0.0f);
                else
                    glUniform3f(m_LocColor, 0.4f, 0.4f, 0.5f);
                glBindVertexArray(m_DiamondVAO);
                glDrawArrays(GL_TRIANGLES, 0, m_DiamondVertCount);
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glEnable(GL_CULL_FACE);
        }
        glUniform1i(m_LocLit, actorLitMode);

        // Render light actors as colored diamond icons (hidden in Game Preview)
        if (m_ShowLightIcons && m_DiamondVAO && !m_GamePreview) {
            glUniform1i(m_LocLit, 4); // emissive mode
            glUniform1i(m_LocHasTexture, 0);
            glDisable(GL_CULL_FACE); // diamonds visible from all sides

            for (int i = 0; i < (int)actors.size(); i++) {
                auto& a = actors[i];
                if (!a.visible || !a.isLight) continue;
                bool isSel = (i == selectedActor) || a.selected;

                // Distance cull
                if (m_DrawRadiusEnabled) {
                    float dx = a.location.x - camPos.x;
                    float dy = a.location.y - camPos.y;
                    float dz = a.location.z - camPos.z;
                    if (dx*dx + dy*dy + dz*dz > radiusSq * 4.0f) continue; // show lights a bit further
                }

                // Diamond size: 60 world units
                float dSize = 60.0f;
                Mat4 model = MulMat4(
                    TranslateMat4(a.location.x, a.location.y, a.location.z),
                    ScaleMat4(dSize));
                Mat4 mvp = MulMat4(vp_mat, model);
                glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp.m);
                glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, model.m);

                if (isSel) {
                    glUniform3f(m_LocColor, 1.0f, 1.0f, 0.3f);
                } else {
                    // Use the light's actual color
                    float r = a.lightColorR / 255.0f;
                    float g = a.lightColorG / 255.0f;
                    float b = a.lightColorB / 255.0f;
                    // Boost dim lights for visibility
                    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
                    if (maxC < 0.3f) { r += 0.3f; g += 0.3f; b += 0.3f; }
                    glUniform3f(m_LocColor, r, g, b);
                }
                glBindVertexArray(m_DiamondVAO);
                glDrawArrays(GL_TRIANGLES, 0, m_DiamondVertCount);

                // Selected light: show radius as wireframe cube
                if (isSel) {
                    float rad = a.lightRadius;
                    if (rad > 10.0f && rad < 50000.0f) {
                        Mat4 rModel = MulMat4(
                            TranslateMat4(a.location.x, a.location.y, a.location.z),
                            ScaleMat4(rad));
                        Mat4 rMvp = MulMat4(vp_mat, rModel);
                        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                        glUniform1i(m_LocLit, 0);
                        glUniform3f(m_LocColor, 1.0f, 0.9f, 0.3f);
                        glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, rMvp.m);
                        glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, rModel.m);
                        glBindVertexArray(m_VAO); // unit cube
                        glDrawArrays(GL_TRIANGLES, 0, 36);
                        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                        glUniform1i(m_LocLit, 4);
                    }
                }
            }
            glEnable(GL_CULL_FACE);
        }

        // Render Decal/Projector actors as transparent projection volumes
        if (m_ShowDecals && !m_GamePreview) {
            glUniform1i(m_LocLit, 0);
            glUniform1i(m_LocHasTexture, 0);
            glUniform1f(m_LocEmissive, 0.0f);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glDisable(GL_CULL_FACE);
            for (int i = 0; i < (int)actors.size(); i++) {
                auto& a = actors[i];
                if (!a.visible || !a.isProjector) continue;
                if (std::isnan(a.location.x)) continue;

                float dx = a.location.x - camPos.x;
                float dy = a.location.y - camPos.y;
                float dz = a.location.z - camPos.z;
                if (m_DrawRadiusEnabled && (dx*dx + dy*dy + dz*dz) > radiusSq) continue;

                bool isSel = (i == selectedActor) || a.selected;
                // Draw projection volume as elongated box along actor's forward direction
                float projLen = a.projMaxDist;
                float projWidth = projLen * tanf(a.projFOV * 3.14159f / 180.0f);
                if (projWidth < 10.0f) projWidth = 10.0f;
                if (projLen < 10.0f) projLen = 50.0f;

                // Build transform: translate to actor, rotate by actor rotation, scale to projection bounds
                Mat4 model = BuildActorTransform(a.location, a.rotation, {projWidth, projWidth, projLen});
                Mat4 mvp = MulMat4(vp_mat, model);
                glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, mvp.m);
                glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, model.m);

                if (isSel)
                    glUniform3f(m_LocColor, 1.0f, 0.4f, 0.8f); // pink selected
                else
                    glUniform3f(m_LocColor, 0.7f, 0.1f, 0.1f); // dark red = blood/decal
                glBindVertexArray(m_DiamondVAO);
                glDrawArrays(GL_TRIANGLES, 0, m_DiamondVertCount);
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glEnable(GL_CULL_FACE);
            glUniform1i(m_LocLit, actorLitMode);
        }

        // Render billboard icons for all non-mesh actors (spawners, triggers, effects, sounds, etc.)
        RenderActorIcons(actors, selectedActor, view, proj, camPos, radiusSq);

        // Restore main shader after icon rendering
        glUseProgram(m_ShaderProgram);

        // Draw transform gizmo on selected actor (hidden in Game Preview)
        if (selectedActor >= 0 && selectedActor < (int)actors.size() && m_GizmoVisible && !m_GamePreview) {
            auto& sa = actors[selectedActor];
            if (sa.hasLocation) {
                DrawGizmo(sa.location, vp_mat);
            }
        }
    }

    // Bloom post-process: composite scene + bloom to screen
    if (m_BloomEnabled && m_SceneFBO) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        RenderBloom();
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
        // Tangent: location 3, offset 32 bytes (after 8 floats: pos3+nrm3+uv2)
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        gpu.vertCount = (int)mesh.vertices.size();
        
        // Load texture for this mesh
        {
            LoadedTexture texInfo = m_TextureCache.GetTextureInfo(mesh.textureName);
            gpu.textureId = texInfo.glTexture;
            gpu.hasAlpha = texInfo.hasAlpha;
            if (texInfo.width > 0) gpu.texWidth = texInfo.width;
            if (texInfo.height > 0) gpu.texHeight = texInfo.height;
        }
        
        // Detect two-sided materials (FacingShader, foliage, glass)
        {
            std::string lower = mesh.textureName;
            for (auto& c : lower) c = (char)tolower(c);
            gpu.isTwoSided = (lower.find("facing") != std::string::npos ||
                              lower.find("foliage") != std::string::npos ||
                              lower.find("leaf") != std::string::npos ||
                              lower.find("plant") != std::string::npos ||
                              lower.find("grass") != std::string::npos ||
                              lower.find("vine") != std::string::npos ||
                              lower.find("fern") != std::string::npos ||
                              gpu.hasAlpha); // alpha textures are typically two-sided
        }
        
        // Load normal map if available
        gpu.normalMapId = m_TextureCache.GetNormalMap(mesh.textureName);
        // Load specular map if available
        gpu.specularMapId = m_TextureCache.GetSpecularMap(mesh.textureName);

        // Detect emissive meshes (neon signs, light beams, lamps)
        {
            std::string lower = mesh.name;
            for (auto& c : lower) c = (char)tolower(c);
            gpu.isEmissive = (lower.find("light") != std::string::npos ||
                              lower.find("neon") != std::string::npos ||
                              lower.find("glow") != std::string::npos ||
                              lower.find("beam") != std::string::npos ||
                              lower.find("lamp") != std::string::npos ||
                              lower.find("sign") != std::string::npos ||
                              lower.find("bulb") != std::string::npos ||
                              lower.find("emissive") != std::string::npos);
        }

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
    int normalMappedCount = 0, specMappedCount = 0;
    for (auto& g : m_MeshGPU) {
        if (g.normalMapId) normalMappedCount++;
        if (g.specularMapId) specMappedCount++;
    }
    printf("[Viewport] Uploaded %d meshes to GPU (%d textured, %d normal-mapped, %d specular-mapped, %d untextured)\n", (int)meshes.size(), texturedCount, normalMappedCount, specMappedCount, (int)meshes.size() - texturedCount);
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

        // Normalize BSP UVs from texels to [0,1] and shift near zero
        std::vector<MeshVertex> verts = mesh.vertices; // copy to modify UVs
        float invW = 1.0f / (float)texW;
        float invH = 1.0f / (float)texH;
        for (auto& v : verts) {
            v.u *= invW;
            v.v *= invH;
        }
        // Shift UVs close to 0 to avoid GPU interpolation precision loss.
        // Large UV offsets (e.g. u=53.0) cause the GPU to lose fractional
        // precision during rasterization, producing horizontal stripe artifacts.
        // With GL_REPEAT, subtracting the integer floor is visually identical.
        {
            float uMin = 1e30f, vMin = 1e30f;
            for (auto& v : verts) {
                if (v.u < uMin) uMin = v.u;
                if (v.v < vMin) vMin = v.v;
            }
            float uShift = std::floor(uMin);
            float vShift = std::floor(vMin);
            for (auto& v : verts) {
                v.u -= uShift;
                v.v -= vShift;
            }
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
        // Tangent: location 3 (BSP has default tangents from MeshVertex struct)
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
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
        gpu.isWater = mesh.isWater;
        gpu.lightMapScale = mesh.lightMapScale;
        gpu.zoneIndex = mesh.zoneIndex;
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
