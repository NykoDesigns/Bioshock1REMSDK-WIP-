#include "texture_loader.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#include <GL/gl.h>
#include "gl_funcs.h"

#pragma pack(push, 1)
struct TGAHeader {
    uint8_t  idLength;
    uint8_t  colorMapType;
    uint8_t  imageType;     // 2 = uncompressed RGB, 10 = RLE RGB
    uint16_t colorMapStart;
    uint16_t colorMapLength;
    uint8_t  colorMapBits;
    uint16_t xOrigin;
    uint16_t yOrigin;
    uint16_t width;
    uint16_t height;
    uint8_t  bitsPerPixel;  // 24 or 32
    uint8_t  descriptor;
};
#pragma pack(pop)

unsigned int TextureCache::GetTexture(const std::string& textureName)
{
    if (textureName.empty()) return 0;
    
    auto it = m_Cache.find(textureName);
    if (it != m_Cache.end()) return it->second.glTexture;
    
    // Try multiple naming conventions for the texture file
    LoadedTexture tex;
    static const char* suffixes[] = { ".tga", "_diffuse.tga", "_Diffuse.tga", "_D.tga", "_d.tga", "_diff.tga" };
    
    // First: search primary texture directory
    for (auto& suf : suffixes) {
        tex = LoadTGA(m_TextureDir + "\\" + textureName + suf);
        if (tex.glTexture) break;
    }
    
    // Fallback: search all extra directories (cross-map)
    if (!tex.glTexture) {
        for (auto& dir : m_ExtraSearchDirs) {
            for (auto& suf : suffixes) {
                tex = LoadTGA(dir + "\\" + textureName + suf);
                if (tex.glTexture) break;
            }
            if (tex.glTexture) break;
        }
    }
    
    m_Cache[textureName] = tex;
    return tex.glTexture;
}

unsigned int TextureCache::GetNormalMap(const std::string& textureName)
{
    if (textureName.empty()) return 0;
    
    std::string cacheKey = textureName + "_NRM";
    auto it = m_Cache.find(cacheKey);
    if (it != m_Cache.end()) return it->second.glTexture;
    
    // Try multiple naming conventions for normal map files
    LoadedTexture tex;
    std::string candidates[] = {
        m_TextureDir + "\\" + textureName + "_NormalMap.tga",
        m_TextureDir + "\\" + textureName + "_normalmap.tga",
        m_TextureDir + "\\" + textureName + "_Normal.tga",
        m_TextureDir + "\\" + textureName + "_normal.tga",
        m_TextureDir + "\\" + textureName + "_norm.tga",
        m_TextureDir + "\\" + textureName + "_Norm.tga",
        m_TextureDir + "\\" + textureName + "_n.tga",
        m_TextureDir + "\\" + textureName + "_N.tga",
    };
    for (auto& path : candidates) {
        tex = LoadTGA(path);
        if (tex.glTexture) break;
    }
    m_Cache[cacheKey] = tex;
    return tex.glTexture;
}

unsigned int TextureCache::GetSpecularMap(const std::string& textureName)
{
    if (textureName.empty()) return 0;
    
    std::string cacheKey = textureName + "_SPEC";
    auto it = m_Cache.find(cacheKey);
    if (it != m_Cache.end()) return it->second.glTexture;
    
    LoadedTexture tex;
    std::string candidates[] = {
        m_TextureDir + "\\" + textureName + "_Specular.tga",
        m_TextureDir + "\\" + textureName + "_specular.tga",
        m_TextureDir + "\\" + textureName + "_spec.tga",
        m_TextureDir + "\\" + textureName + "_Spec.tga",
        m_TextureDir + "\\" + textureName + "_s.tga",
        m_TextureDir + "\\" + textureName + "_S.tga",
    };
    for (auto& path : candidates) {
        tex = LoadTGA(path);
        if (tex.glTexture) break;
    }
    m_Cache[cacheKey] = tex;
    return tex.glTexture;
}

LoadedTexture TextureCache::GetTextureInfo(const std::string& textureName)
{
    // Ensure texture is loaded first
    GetTexture(textureName);
    auto it = m_Cache.find(textureName);
    if (it != m_Cache.end()) return it->second;
    return LoadedTexture{};
}

void TextureCache::Clear()
{
    for (auto& pair : m_Cache) {
        if (pair.second.glTexture)
            glDeleteTextures(1, &pair.second.glTexture);
    }
    m_Cache.clear();
}

void TextureCache::AddAllMapTextureDirs(const std::string& exportRoot)
{
    namespace fs = std::filesystem;
    if (exportRoot.empty() || !fs::is_directory(exportRoot)) return;
    
    int added = 0;
    for (auto& entry : fs::directory_iterator(exportRoot)) {
        if (!entry.is_directory()) continue;
        std::string texDir = entry.path().string() + "\\Texture";
        if (fs::is_directory(texDir) && texDir != m_TextureDir) {
            m_ExtraSearchDirs.push_back(texDir);
            added++;
        }
    }
    printf("[TextureCache] Added %d cross-map texture directories from %s\n",
           added, exportRoot.c_str());
}

LoadedTexture TextureCache::LoadTGA(const std::string& path)
{
    LoadedTexture result;
    
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return result;
    
    size_t fileSize = (size_t)file.tellg();
    if (fileSize < sizeof(TGAHeader)) return result;
    
    file.seekg(0);
    std::vector<uint8_t> data(fileSize);
    file.read((char*)data.data(), fileSize);
    file.close();
    
    TGAHeader* hdr = (TGAHeader*)data.data();
    int width = hdr->width;
    int height = hdr->height;
    int bpp = hdr->bitsPerPixel;
    
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return result;
    if (bpp != 24 && bpp != 32) return result;
    
    int channels = bpp / 8;
    const uint8_t* pixelData = data.data() + sizeof(TGAHeader) + hdr->idLength;
    
    // Handle RLE compressed TGA (type 10)
    std::vector<uint8_t> decompressed;
    if (hdr->imageType == 10) {
        decompressed.resize(width * height * channels);
        const uint8_t* src = pixelData;
        uint8_t* dst = decompressed.data();
        int pixelCount = width * height;
        int pixelsDecoded = 0;
        
        while (pixelsDecoded < pixelCount) {
            uint8_t packet = *src++;
            int count = (packet & 0x7F) + 1;
            if (packet & 0x80) {
                // RLE packet
                for (int j = 0; j < count && pixelsDecoded < pixelCount; j++) {
                    memcpy(dst, src, channels);
                    dst += channels;
                    pixelsDecoded++;
                }
                src += channels;
            } else {
                // Raw packet
                for (int j = 0; j < count && pixelsDecoded < pixelCount; j++) {
                    memcpy(dst, src, channels);
                    dst += channels;
                    src += channels;
                    pixelsDecoded++;
                }
            }
        }
        pixelData = decompressed.data();
    } else if (hdr->imageType != 2) {
        return result; // unsupported type
    }
    
    // Convert BGR(A) to RGB(A) for OpenGL
    std::vector<uint8_t> rgbaData(width * height * 4);
    bool flipV = !(hdr->descriptor & 0x20); // bit 5 = top-to-bottom flag
    
    for (int y = 0; y < height; y++) {
        int srcY = flipV ? (height - 1 - y) : y;
        for (int x = 0; x < width; x++) {
            int srcIdx = (srcY * width + x) * channels;
            int dstIdx = (y * width + x) * 4;
            rgbaData[dstIdx + 0] = pixelData[srcIdx + 2]; // R (from B)
            rgbaData[dstIdx + 1] = pixelData[srcIdx + 1]; // G
            rgbaData[dstIdx + 2] = pixelData[srcIdx + 0]; // B (from R)
            rgbaData[dstIdx + 3] = (channels == 4) ? pixelData[srcIdx + 3] : 255;
        }
    }
    
    // Check if texture has meaningful alpha (not all 255)
    bool hasAlpha = false;
    if (channels == 4) {
        for (int i = 0; i < width * height; i++) {
            if (rgbaData[i * 4 + 3] < 250) { hasAlpha = true; break; }
        }
    }

    // Upload to OpenGL
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 16.0f); // Anisotropic filtering
    glGenerateMipmap(GL_TEXTURE_2D);
    
    result.glTexture = tex;
    result.width = width;
    result.height = height;
    result.hasAlpha = hasAlpha;
    return result;
}

unsigned int TextureCache::UploadDXT1(const std::string& name, const uint8_t* data, int width, int height)
{
    if (!data || width <= 0 || height <= 0) return 0;
    
    // Check if already cached
    auto it = m_Cache.find(name);
    if (it != m_Cache.end()) return it->second.glTexture;
    
    // DXT1: 8 bytes per 4x4 block = 0.5 bytes per pixel
    int dataSize = ((width + 3) / 4) * ((height + 3) / 4) * 8;
    
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
                           width, height, 0, dataSize, data);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    LoadedTexture lt;
    lt.glTexture = tex;
    lt.width = width;
    lt.height = height;
    lt.hasAlpha = false;
    m_Cache[name] = lt;
    
    printf("[Tex] Uploaded DXT1 lightmap '%s' (%dx%d, %d bytes)\n", name.c_str(), width, height, dataSize);
    return tex;
}
