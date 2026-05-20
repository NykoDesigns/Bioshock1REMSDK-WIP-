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

// Parse BSP geometry from a UModel export's serial data (C.1 spec).
// Returns one ParsedMesh per unique material (each chunk has textureName set).
// exports/importNames allow resolving FBspSurf.Material object references.
// outTree (optional): receives BSP tree nodes for camera zone traversal.
std::vector<ParsedMesh> ParseBSPGeometry(const uint8_t* serialData, int serialSize,
                                         const std::vector<std::string>& names,
                                         const std::vector<BSPExportInfo>& exports,
                                         const std::vector<std::string>& importNames,
                                         std::vector<BSPTreeNodeOut>* outTree = nullptr);
