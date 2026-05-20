#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "camera.h"

struct MeshVertex {
    float x, y, z;    // position
    float nx, ny, nz; // normal
    float u, v;       // texture coordinates
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
    uint8_t zoneMask[16] = {}; // BSP: union of zone visibility masks for all nodes in this chunk
    uint8_t zoneIndex = 0;     // BSP: zone this chunk belongs to (byte at +96)
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
