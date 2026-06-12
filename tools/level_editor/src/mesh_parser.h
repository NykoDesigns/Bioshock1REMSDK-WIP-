#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "camera.h"

struct MeshVertex {
    float x, y, z;    // position
    float nx, ny, nz; // normal
    float u, v;       // texture coordinates
    float u2, v2;     // lightmap UV (BSP only: texel coords / atlasSize)
    float tx, ty, tz, tw; // tangent (xyz = direction, w = handedness ±1)
};

struct MeshTriangle {
    uint16_t i0, i1, i2;
};

struct ParsedMesh {
    std::string name;
    std::string textureName; // material/texture name from glTF (without _shader suffix)
    std::vector<MeshVertex> vertices;
    std::vector<MeshTriangle> triangles;
    Vec3 boundsMin = {0,0,0};
    Vec3 boundsMax = {0,0,0};
    bool valid = false;
    bool isWater = false;      // BSP: water surface (rendered with transparency + animation)
    uint8_t zoneMask[16] = {}; // BSP: union of zone visibility masks for all nodes in this chunk
    uint8_t zoneIndex = 0;     // BSP: zone this chunk belongs to (byte at +96)
    float lightMapScale = 0.0f;  // BSP: lightmap texel density (from FBspSurf 20B tail)
    int32_t lightMapIndex = 0;   // BSP: lightmap atlas texture index (0=none, 1-N=1-based)
    std::string lightMapName;    // BSP: lightmap cache key (e.g. "LM_Texture49")
    std::string normalMapName;   // Normal map texture name (from Shader .props.txt)
    std::string specMapName;     // Specular map texture name (from Shader .props.txt)
    std::string emissiveMapName; // Emissive/self-illumination texture name (from Shader .props.txt)
    std::string bsmMaterialName; // StaticMesh: material/shader name from BSM Materials array (authoritative)
    std::string sourceExportDir; // cross-map: UModel export dir this mesh was loaded from (empty = current map)
};

// Parse a StaticMesh export's serial data into vertices + triangles.
// This handles BioShock's UE2.5 Vengeance StaticMesh binary format.
ParsedMesh ParseStaticMesh(const uint8_t* serialData, int serialSize,
                           const std::vector<std::string>& names);

// Load a mesh from a pre-exported glTF file (from UModel).
// Returns a valid ParsedMesh if the file exists and loads correctly.
ParsedMesh LoadMeshFromGLTF(const std::string& gltfPath, const std::string& meshName);

// Load all meshes from a UModel export directory for a given map.
// exportDir should contain a StaticMesh/ subdirectory with .gltf + .bin files.
std::vector<ParsedMesh> LoadAllMeshesFromExportDir(const std::string& exportDir);
