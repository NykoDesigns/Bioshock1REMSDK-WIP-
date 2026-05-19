#pragma once

#include <string>
#include <vector>
#include <unordered_map>

struct LoadedTexture {
    unsigned int glTexture = 0;
    int width = 0;
    int height = 0;
};

// Texture cache - loads TGA files from UModel export directory
class TextureCache {
public:
    // Set the base directory where textures are stored (e.g. Z:\UEViewer\export\1-Medical\Texture)
    void SetTextureDir(const std::string& dir) { m_TextureDir = dir; }
    
    // Get or load a texture by name. Returns 0 if not found.
    unsigned int GetTexture(const std::string& textureName);
    
    // Release all loaded textures
    void Clear();

private:
    std::string m_TextureDir;
    std::unordered_map<std::string, LoadedTexture> m_Cache;
    
    LoadedTexture LoadTGA(const std::string& path);
};
