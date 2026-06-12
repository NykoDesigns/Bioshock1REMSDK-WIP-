#pragma once

#include "mesh_parser.h"
#include <vector>
#include <cstdint>
#include <string>

// Minimal export/import info for resolving BSP material references
struct BSPExportInfo {
    std::string className;   // "Shader", "Texture", etc.
    std::string objectName;  // e.g. "GraniteColor_Shader"
};

// BSP tree node for camera zone detection
struct BSPTreeNodeOut {
    float planeX, planeY, planeZ, planeW;
    int32_t iFront, iBack;
    uint8_t zoneMask[16]; // 128-bit zone visibility
    uint8_t iZone;        // zone index of this node's polygon
};

// Per-surface lightmap descriptor (parsed from UModel's FLightMapIndex + FLightMapLight)
struct BSPLightMapInfo {
    int iSurf = -1;            // back-ref to owning surface
    int sizeX = 0, sizeY = 0;  // lightmap texel dimensions (7..512); UV scale = size/1024
    float worldToLightMap[16] = {}; // 4x4 matrix: world pos → lightmap UV (row-major)
    // From first FLightMapLight entry:
    int iAtlas = -1;           // index into LightMapTextures[] — selects atlas texture
    int tileX = 0, tileY = 0;  // pixel position of this surface's tile in the atlas (0..1016)
};

// Parse BSP geometry from a UModel export's serial data (C.1 spec).
// Returns one ParsedMesh per unique material (each chunk has textureName set).
// exports/importNames allow resolving FBspSurf.Material object references.
// outTree (optional): receives BSP tree nodes for camera zone traversal.
std::vector<ParsedMesh> ParseBSPGeometry(const uint8_t* serialData, int serialSize,
                                         const std::vector<std::string>& names,
                                         const std::vector<BSPExportInfo>& exports,
                                         const std::vector<std::string>& importNames,
                                         std::vector<BSPTreeNodeOut>* outTree = nullptr,
                                         const std::vector<std::string>* lightMapNames = nullptr);
