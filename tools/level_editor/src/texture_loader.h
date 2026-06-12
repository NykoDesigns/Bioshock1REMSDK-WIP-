#pragma once

#include <string>
#include <vector>
#include <unordered_map>

struct LoadedTexture {
    unsigned int glTexture = 0;
    int width = 0;
    int height = 0;
    bool hasAlpha = false; // true if texture has meaningful alpha channel
};

// Texture cache - loads TGA files from UModel export directory
class TextureCache {
public:
    // Set the base directory where textures are stored (e.g. Z:\UEViewer\export\1-Medical\Texture)
    void SetTextureDir(const std::string& dir) { m_TextureDir = dir; }
    
    // Add additional search directories (e.g. other map exports for cross-map fallback)
    void AddSearchDir(const std::string& dir) { m_ExtraSearchDirs.push_back(dir); }
    
    // Scan all map export directories under the root export path and add their Texture folders
    void AddAllMapTextureDirs(const std::string& exportRoot);
    
    // Get or load a texture by name. Returns 0 if not found.
    unsigned int GetTexture(const std::string& textureName);
    unsigned int GetDiffuse(const std::string& textureName) { return GetTexture(textureName); }
    
    // Get or load a normal map by base texture name. Returns 0 if not found.
    unsigned int GetNormalMap(const std::string& textureName);
    
    // Get or load a specular map by base texture name. Returns 0 if not found.
    unsigned int GetSpecularMap(const std::string& textureName);
    
    // Get texture info including dimensions. Returns default (0,0,0) if not found.
    LoadedTexture GetTextureInfo(const std::string& textureName);
    
    // Upload DXT1 compressed data directly as a GL texture.
    // Returns GL texture ID, or 0 on failure.
    unsigned int UploadDXT1(const std::string& name, const uint8_t* data, int width, int height);
    
    // Upload DXT3 compressed data (explicit alpha). Returns GL texture ID, or 0 on failure.
    unsigned int UploadDXT3(const std::string& name, const uint8_t* data, int width, int height);
    
    // Upload DXT5 compressed data (interpolated alpha). Returns GL texture ID, or 0 on failure.
    unsigned int UploadDXT5(const std::string& name, const uint8_t* data, int width, int height);
    
    // Upload BC5/3DC/ATI2N compressed data (2-channel, normal maps). Returns GL texture ID, or 0.
    unsigned int UploadBC5(const std::string& name, const uint8_t* data, int width, int height);
    
    // Upload compressed texture with auto-detected format (by BioShock format enum ordinal).
    // Format: 3=DXT1, 7=DXT3, 8=DXT5, 12=3DC/BC5. Returns GL texture ID, or 0.
    unsigned int UploadCompressed(const std::string& name, const uint8_t* data,
                                  int width, int height, int bioshockFormat);
    
    // Release all loaded textures
    void Clear();

private:
    std::string m_TextureDir;
    std::vector<std::string> m_ExtraSearchDirs;
    std::unordered_map<std::string, LoadedTexture> m_Cache;
    
    LoadedTexture LoadTGA(const std::string& path);
};
