#include "texture_loader.h"
#include <cstdio>
#include <cstring>
#include <fstream>

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
    std::string candidates[] = {
        m_TextureDir + "\\" + textureName + ".tga",
        m_TextureDir + "\\" + textureName + "_diffuse.tga",
        m_TextureDir + "\\" + textureName + "_Diffuse.tga",
        m_TextureDir + "\\" + textureName + "_D.tga",
        m_TextureDir + "\\" + textureName + "_d.tga",
        m_TextureDir + "\\" + textureName + "_diff.tga",
    };
    for (auto& path : candidates) {
        tex = LoadTGA(path);
        if (tex.glTexture) break;
    }
    m_Cache[textureName] = tex;
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
    
    // Upload to OpenGL
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
    
    result.glTexture = tex;
    result.width = width;
    result.height = height;
    return result;
}
