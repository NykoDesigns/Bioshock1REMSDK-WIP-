#include "mesh_parser.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

// Minimal glTF JSON parsing - we only need accessors, bufferViews, and buffer URI.
// UModel's output is very predictable: accessor 0 = indices, accessor 1 = positions.

struct GLTFAccessor {
    int bufferView = -1;
    int componentType = 0; // 5123=uint16, 5126=float
    int count = 0;
    std::string type; // "SCALAR", "VEC3", etc.
};

struct GLTFBufferView {
    int buffer = 0;
    int byteOffset = 0;
    int byteLength = 0;
};

// Simple JSON value extraction (works for UModel's clean output)
static int ParseInt(const std::string& json, const std::string& key, int startPos = 0)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search, startPos);
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return -1;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return atoi(json.c_str() + pos);
}

static std::string ParseString(const std::string& json, const std::string& key, int startPos = 0)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search, startPos);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Parse all accessors from the JSON
static std::vector<GLTFAccessor> ParseAccessors(const std::string& json)
{
    std::vector<GLTFAccessor> result;
    size_t pos = json.find("\"accessors\"");
    if (pos == std::string::npos) return result;
    
    // Find the array
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;
    
    // Find each object in the array
    size_t arrayEnd = json.find(']', pos);
    size_t objStart = json.find('{', pos);
    while (objStart != std::string::npos && objStart < arrayEnd) {
        size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos) break;
        
        std::string obj = json.substr(objStart, objEnd - objStart + 1);
        GLTFAccessor acc;
        acc.bufferView = ParseInt(obj, "bufferView");
        acc.componentType = ParseInt(obj, "componentType");
        acc.count = ParseInt(obj, "count");
        acc.type = ParseString(obj, "type");
        result.push_back(acc);
        
        objStart = json.find('{', objEnd);
    }
    return result;
}

// Parse all bufferViews from the JSON
static std::vector<GLTFBufferView> ParseBufferViews(const std::string& json)
{
    std::vector<GLTFBufferView> result;
    size_t pos = json.find("\"bufferViews\"");
    if (pos == std::string::npos) return result;
    
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;
    
    size_t arrayEnd = json.find(']', pos);
    size_t objStart = json.find('{', pos);
    while (objStart != std::string::npos && objStart < arrayEnd) {
        size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos) break;
        
        std::string obj = json.substr(objStart, objEnd - objStart + 1);
        GLTFBufferView bv;
        bv.buffer = ParseInt(obj, "buffer");
        bv.byteOffset = ParseInt(obj, "byteOffset");
        bv.byteLength = ParseInt(obj, "byteLength");
        result.push_back(bv);
        
        objStart = json.find('{', objEnd);
    }
    return result;
}

ParsedMesh LoadMeshFromGLTF(const std::string& gltfPath, const std::string& meshName)
{
    ParsedMesh mesh;
    mesh.name = meshName;
    mesh.valid = false;

    // Read JSON file
    std::ifstream jsonFile(gltfPath);
    if (!jsonFile.is_open()) return mesh;
    
    std::stringstream ss;
    ss << jsonFile.rdbuf();
    std::string json = ss.str();
    jsonFile.close();

    // Parse accessors and buffer views
    auto accessors = ParseAccessors(json);
    auto bufferViews = ParseBufferViews(json);
    std::string binUri = ParseString(json, "uri");
    
    if (accessors.size() < 2 || bufferViews.size() < 2 || binUri.empty())
        return mesh;

    // Parse material name for texture lookup
    std::string matName = ParseString(json, "name", (int)json.find("\"materials\""));
    // Strip "_shader" or "_Shader" suffix to get texture name
    if (matName.size() > 7) {
        std::string suffix = matName.substr(matName.size() - 7);
        if (suffix == "_shader" || suffix == "_Shader")
            matName = matName.substr(0, matName.size() - 7);
    }
    mesh.textureName = matName;

    // Find index accessor (componentType 5123 = uint16, type SCALAR)
    // Find position accessor (first VEC3), normal accessor (second VEC3), UV accessor (VEC2)
    int idxAccessorIdx = -1;
    int posAccessorIdx = -1;
    int nrmAccessorIdx = -1;
    int uvAccessorIdx = -1;
    for (int i = 0; i < (int)accessors.size(); i++) {
        if (accessors[i].componentType == 5123 && accessors[i].type == "SCALAR")
            idxAccessorIdx = i;
        if (accessors[i].componentType == 5126 && accessors[i].type == "VEC3") {
            if (posAccessorIdx < 0)
                posAccessorIdx = i;
            else if (nrmAccessorIdx < 0)
                nrmAccessorIdx = i;
        }
        if (accessors[i].componentType == 5126 && accessors[i].type == "VEC2" && uvAccessorIdx < 0)
            uvAccessorIdx = i;
    }
    
    if (idxAccessorIdx < 0 || posAccessorIdx < 0) return mesh;

    auto& idxAcc = accessors[idxAccessorIdx];
    auto& posAcc = accessors[posAccessorIdx];
    
    if (idxAcc.bufferView < 0 || idxAcc.bufferView >= (int)bufferViews.size()) return mesh;
    if (posAcc.bufferView < 0 || posAcc.bufferView >= (int)bufferViews.size()) return mesh;

    auto& idxBV = bufferViews[idxAcc.bufferView];
    auto& posBV = bufferViews[posAcc.bufferView];

    // Build path to .bin file (same directory as .gltf)
    std::string binPath = gltfPath;
    size_t lastSlash = binPath.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        binPath = binPath.substr(0, lastSlash + 1) + binUri;
    else
        binPath = binUri;

    // Read binary buffer
    std::ifstream binFile(binPath, std::ios::binary | std::ios::ate);
    if (!binFile.is_open()) return mesh;
    
    size_t binSize = (size_t)binFile.tellg();
    binFile.seekg(0);
    std::vector<uint8_t> binData(binSize);
    binFile.read((char*)binData.data(), binSize);
    binFile.close();

    // Extract positions
    int numVerts = posAcc.count;
    if (numVerts < 3) return mesh;
    if (posBV.byteOffset + numVerts * 12 > (int)binSize) return mesh;

    mesh.vertices.resize(numVerts);
    const float* posData = (const float*)(binData.data() + posBV.byteOffset);
    
    // Get normal data if available
    const float* nrmData = nullptr;
    if (nrmAccessorIdx >= 0) {
        auto& nrmAcc = accessors[nrmAccessorIdx];
        if (nrmAcc.bufferView >= 0 && nrmAcc.bufferView < (int)bufferViews.size()) {
            auto& nrmBV = bufferViews[nrmAcc.bufferView];
            if (nrmBV.byteOffset + numVerts * 12 <= (int)binSize)
                nrmData = (const float*)(binData.data() + nrmBV.byteOffset);
        }
    }
    
    // Get UV data if available
    const float* uvData = nullptr;
    if (uvAccessorIdx >= 0) {
        auto& uvAcc = accessors[uvAccessorIdx];
        if (uvAcc.bufferView >= 0 && uvAcc.bufferView < (int)bufferViews.size()) {
            auto& uvBV = bufferViews[uvAcc.bufferView];
            if (uvBV.byteOffset + numVerts * 8 <= (int)binSize)
                uvData = (const float*)(binData.data() + uvBV.byteOffset);
        }
    }
    
    float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
    float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
    
    for (int i = 0; i < numVerts; i++) {
        // glTF uses Y-up, UE uses Z-up
        // UModel transform: UE(x,y,z) -> glTF(x/100, z/100, -y/100)
        // Inverse: UE.x = gltf.x*100, UE.y = -gltf.z*100, UE.z = gltf.y*100
        float gx = posData[i * 3 + 0];
        float gy = posData[i * 3 + 1];
        float gz = posData[i * 3 + 2];
        
        mesh.vertices[i].x = gx * 100.0f;
        mesh.vertices[i].y = -gz * 100.0f;
        mesh.vertices[i].z = gy * 100.0f;
        
        // Transform normals the same way (no scale, just axis swap)
        if (nrmData) {
            float gnx = nrmData[i * 3 + 0];
            float gny = nrmData[i * 3 + 1];
            float gnz = nrmData[i * 3 + 2];
            mesh.vertices[i].nx = gnx;
            mesh.vertices[i].ny = -gnz;
            mesh.vertices[i].nz = gny;
        } else {
            mesh.vertices[i].nx = 0;
            mesh.vertices[i].ny = 0;
            mesh.vertices[i].nz = 1;
        }
        
        // UV coordinates
        if (uvData) {
            mesh.vertices[i].u = uvData[i * 2 + 0];
            mesh.vertices[i].v = uvData[i * 2 + 1];
        } else {
            mesh.vertices[i].u = 0;
            mesh.vertices[i].v = 0;
        }
        
        minX = std::min(minX, mesh.vertices[i].x);
        minY = std::min(minY, mesh.vertices[i].y);
        minZ = std::min(minZ, mesh.vertices[i].z);
        maxX = std::max(maxX, mesh.vertices[i].x);
        maxY = std::max(maxY, mesh.vertices[i].y);
        maxZ = std::max(maxZ, mesh.vertices[i].z);
    }
    
    mesh.boundsMin = {minX, minY, minZ};
    mesh.boundsMax = {maxX, maxY, maxZ};

    // Extract indices
    int numIndices = idxAcc.count;
    if (numIndices < 3) return mesh;
    if (idxBV.byteOffset + numIndices * 2 > (int)binSize) return mesh;
    
    const uint16_t* idxData = (const uint16_t*)(binData.data() + idxBV.byteOffset);
    int numTris = numIndices / 3;
    mesh.triangles.resize(numTris);
    for (int i = 0; i < numTris; i++) {
        mesh.triangles[i].i0 = idxData[i * 3 + 0];
        mesh.triangles[i].i1 = idxData[i * 3 + 1];
        mesh.triangles[i].i2 = idxData[i * 3 + 2];
    }

    mesh.valid = true;
    return mesh;
}

std::vector<ParsedMesh> LoadAllMeshesFromExportDir(const std::string& exportDir)
{
    std::vector<ParsedMesh> meshes;
    std::string meshDir = exportDir + "\\StaticMesh\\";
    
    // Find all .gltf files in the directory
    WIN32_FIND_DATAA fd;
    std::string pattern = meshDir + "*.gltf";
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        printf("[GLTF] No files found in: %s\n", meshDir.c_str());
        return meshes;
    }

    int loaded = 0, failed = 0;
    do {
        std::string filename = fd.cFileName;
        // Extract mesh name from filename (remove .gltf extension)
        std::string name = filename.substr(0, filename.size() - 5);
        std::string fullPath = meshDir + filename;
        
        ParsedMesh m = LoadMeshFromGLTF(fullPath, name);
        if (m.valid) {
            meshes.push_back(std::move(m));
            loaded++;
        } else {
            failed++;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    printf("[GLTF] Loaded %d meshes (%d failed) from %s\n", loaded, failed, meshDir.c_str());
    return meshes;
}
