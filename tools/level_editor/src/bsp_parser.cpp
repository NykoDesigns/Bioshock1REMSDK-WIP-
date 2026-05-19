#include "bsp_parser.h"
#include <cstring>
#include <cmath>
#include <cstdio>

// ─── Internal helpers ───────────────────────────────────────────────────────

static int ReadCompactIndex(const uint8_t* data, size_t maxLen, size_t& bytesRead)
{
    int output = 0;
    bool isNeg = false;
    bytesRead = 0;
    for (int i = 0; i < 5 && bytesRead < maxLen; i++) {
        uint8_t b = data[bytesRead++];
        if (i == 0) {
            isNeg = (b & 0x80) != 0;
            output = b & 0x3F;
            if ((b & 0x40) == 0) break;
        } else if (i == 4) {
            output |= (b & 0x1F) << (6 + 3 * 7);
        } else {
            output |= (b & 0x7F) << (6 + (i - 1) * 7);
            if ((b & 0x80) == 0) break;
        }
    }
    return isNeg ? -output : output;
}

static int32_t ReadInt32(const uint8_t* d, size_t& pos) {
    int32_t v; memcpy(&v, d + pos, 4); pos += 4; return v;
}

// Skip UE property tags until "None" sentinel
static size_t SkipProperties(const uint8_t* data, size_t pos, size_t endPos,
                              const std::vector<std::string>& names)
{
    while (pos < endPos - 5) {
        size_t br;
        int nameIdx = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
        pos += 4; // name number
        if (nameIdx == 0) return pos; // "None" sentinel

        if (nameIdx < 0 || nameIdx >= (int)names.size()) return pos;
        if (pos >= endPos) return pos;

        uint8_t info = data[pos++];
        int type = info & 0x0F;
        int sizeBits = (info >> 4) & 0x07;
        int arrayFlag = (info >> 7) & 1;

        if (type == 10) { // PT_Struct
            ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
            pos += 4;
        }

        int propSize = 0;
        switch (sizeBits) {
            case 0: propSize = 1; break;
            case 1: propSize = 2; break;
            case 2: propSize = 4; break;
            case 3: propSize = 12; break;
            case 4: propSize = 16; break;
            case 5: propSize = data[pos++]; break;
            case 6: { uint16_t v; memcpy(&v, data+pos, 2); pos += 2; propSize = v; } break;
            case 7: { uint32_t v; memcpy(&v, data+pos, 4); pos += 4; propSize = (int)v; } break;
        }

        if (type == 3) { // Bool - no payload
        } else {
            if (arrayFlag) {
                uint8_t b = data[pos++];
                if ((b & 0xC0) == 0x80) pos++;
                else if ((b & 0xC0) == 0xC0) pos += 3;
            }
            pos += propSize;
        }
    }
    return pos;
}

// ─── BSP Format (BioShock UE2.5 Vengeance, C.1 spec) ────────────────────────
//
// UModel::Serialize layout after UObject base:
//   8B   Vengeance class header (check=4, sv=7)
//   TArray<FVector> Vectors
//   TArray<FVector> Points
//   CI NumNodes + 100B × NumNodes (FBspNode)
//   CI NumSurfs + (8B header + 52B) × NumSurfs (FBspSurf)
//   CI NumVerts + 8B × NumVerts (FVert: INT32 pVertex + INT32 iSide)
//
// FBspNode (100B):
//   +0:  FPlane (16B: X,Y,Z,W)
//   +16: ZoneMask (16B: 128-bit)
//   +32: BYTE NodeFlags
//   ... iVertPool(INT32), iSurf(INT32), NumVertices(BYTE) ...
//
// For rendering: triangulate each node's polygon fan using its vertices.

// TArray on disk: CompactIndex count + count × element data
static bool ReadTArrayVectors(const uint8_t* data, size_t& pos, size_t endPos,
                               std::vector<float>& outVec3s)
{
    if (pos + 1 > endPos) return false;
    size_t br;
    int count = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
    if (count < 0 || count > 5000000) return false;
    size_t bytes = (size_t)count * 12;
    if (pos + bytes > endPos) return false;
    outVec3s.resize(count * 3);
    memcpy(outVec3s.data(), data + pos, bytes);
    pos += bytes;
    return true;
}

// FBspNode: we only need iVertPool, iSurf, and NumVertices for triangulation
struct BSPNode {
    float planeX, planeY, planeZ, planeW;
    int32_t iVertPool;
    int32_t iSurf;
    uint8_t numVertices;
};

static bool ParseNodes(const uint8_t* data, size_t& pos, size_t endPos,
                        std::vector<BSPNode>& nodes)
{
    if (pos + 1 > endPos) return false;
    size_t br;
    int numNodes = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
    if (numNodes < 0 || numNodes > 500000) return false;
    size_t nodeBytes = (size_t)numNodes * 100;
    if (pos + nodeBytes > endPos) return false;

    nodes.resize(numNodes);
    for (int i = 0; i < numNodes; i++) {
        size_t noff = pos + (size_t)i * 100;
        // FPlane at +0 (16B)
        memcpy(&nodes[i].planeX, data + noff, 4);
        memcpy(&nodes[i].planeY, data + noff + 4, 4);
        memcpy(&nodes[i].planeZ, data + noff + 8, 4);
        memcpy(&nodes[i].planeW, data + noff + 12, 4);
        // Vengeance FBspNode layout (100B total, validated by scan):
        //   +0:  FPlane (16B)
        //   +16: ZoneMask (16B, 128-bit)
        //   +32: INT32 iVertPool
        //   +36: INT32 iSurf
        //   +40-+84: tree links, collision, zones, etc.
        //   +88: INT32 NumVertices
        //   +92-+96: remaining fields
        memcpy(&nodes[i].iVertPool, data + noff + 32, 4);
        memcpy(&nodes[i].iSurf, data + noff + 36, 4);
        int32_t nv; memcpy(&nv, data + noff + 88, 4);
        nodes[i].numVertices = (nv >= 0 && nv < 256) ? (uint8_t)nv : 0;
    }
    pos += nodeBytes;
    return true;
}

// FBspSurf: 8B Vengeance header + 52B body
// We just need the normal vector index for lighting
struct BSPSurf {
    int32_t vNormal; // index into Vectors array for the surface normal
};

static bool ParseSurfs(const uint8_t* data, size_t& pos, size_t endPos,
                        std::vector<BSPSurf>& surfs)
{
    if (pos + 1 > endPos) return false;
    size_t br;
    int numSurfs = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
    if (numSurfs < 0 || numSurfs > 500000) return false;
    // C.1.3 spec: each surf has 8B Vengeance header + variable CI fields + fixed INT32s.
    // Per-element size varies due to CI Material/Actor refs. We don't need to parse
    // individual surfs since we use node plane normals for rendering. Just record count.
    surfs.resize(numSurfs);
    // pos stays at start of surf data — caller will scan forward for FVerts
    return true;
}

// FVert: INT32 pVertex + INT32 iSide (8B each, per C.1.4)
struct BSPVert {
    int32_t pVertex; // index into Points array
    int32_t iSide;
};


// ─── Main BSP Parser ─────────────────────────────────────────────────────────

std::vector<ParsedMesh> ParseBSPGeometry(const uint8_t* serialData, int serialSize,
                                         const std::vector<std::string>& names)
{
    std::vector<ParsedMesh> results;
    if (serialSize < 200) return results;

    size_t endPos = (size_t)serialSize;
    size_t pos = 0;

    // ─── 1. Skip 8-byte Vengeance per-object header ───
    if (pos + 8 > endPos) return results;
    int32_t objCheck = ReadInt32(serialData, pos);
    int32_t objSV = ReadInt32(serialData, pos);
    (void)objSV;
    if (objCheck != 4) return results;

    // ─── 2. Skip tagged properties ───
    pos = SkipProperties(serialData, pos, endPos, names);
    if (pos >= endPos) return results;

    // ─── 2b. UPrimitive base: FBox (25B) + FSphere (16B) = 41B ───
    if (pos + 41 > endPos) return results;
    pos += 41; // skip FBox(25) + FSphere(16)

    // ─── 3. Vengeance class header for UModel (check=4, sv=7) ───
    if (pos + 8 > endPos) return results;
    int32_t mdlCheck = ReadInt32(serialData, pos);
    int32_t mdlSV = ReadInt32(serialData, pos);
    (void)mdlSV;
    if (mdlCheck != 4) return results;

    // ─── 4. TArray<FVector> Vectors ───
    std::vector<float> vectors; // flat array of x,y,z triples
    if (!ReadTArrayVectors(serialData, pos, endPos, vectors)) return results;
    // ─── 5. TArray<FVector> Points ───
    std::vector<float> points;
    if (!ReadTArrayVectors(serialData, pos, endPos, points)) return results;
    int numPoints = (int)points.size() / 3;
    printf("[BSP] Vectors: %d, Points: %d\n", (int)vectors.size() / 3, numPoints);

    // ─── 6. Nodes (100B each) ───
    std::vector<BSPNode> nodes;
    if (!ParseNodes(serialData, pos, endPos, nodes)) return results;

    // ─── 7. Surfs ───
    std::vector<BSPSurf> surfs;
    if (!ParseSurfs(serialData, pos, endPos, surfs)) return results;
    printf("[BSP] Nodes: %d, Surfs: %d\n", (int)nodes.size(), (int)surfs.size());

    // ─── 8. Find FVert array (C.1.4: CI count + N × 8B) ───
    // Surfs have variable-length per-element serialization, so we scan forward
    int minVertCount = 0;
    for (auto& n : nodes) {
        int end = n.iVertPool + (int)n.numVertices;
        if (end > minVertCount) minVertCount = end;
    }

    std::vector<BSPVert> verts;
    {
        size_t br2;
        int numVerts = ReadCompactIndex(serialData + pos, endPos - pos, br2);
        if (numVerts >= minVertCount && numVerts < 1000000) {
            pos += br2;
            size_t vertBytes = (size_t)numVerts * 8;
            if (pos + vertBytes <= endPos) {
                verts.resize(numVerts);
                for (int i = 0; i < numVerts; i++) {
                    memcpy(&verts[i].pVertex, serialData + pos + (size_t)i * 8, 4);
                    memcpy(&verts[i].iSide, serialData + pos + (size_t)i * 8 + 4, 4);
                }
                pos += vertBytes;
            }
        }
    }

    // Fallback: scan for FVert array if direct read failed
    if (verts.empty()) {
        size_t scanStart = pos;
        for (size_t sc = scanStart; sc + 10 < endPos; sc++) {
            size_t br2;
            int arrCount = ReadCompactIndex(serialData + sc, endPos - sc, br2);
            if (arrCount < minVertCount || arrCount > 1000000) continue;
            size_t dataStart = sc + br2;
            size_t dataBytes = (size_t)arrCount * 8;
            if (dataStart + dataBytes > endPos) continue;
            // Quick validation: check a few vertex indices
            bool ok = true;
            for (int t = 0; t < 10 && t < arrCount; t++) {
                int32_t pv; memcpy(&pv, serialData + dataStart + (size_t)t * 8, 4);
                if (pv < 0 || pv >= numPoints) { ok = false; break; }
            }
            if (!ok) continue;
            printf("[BSP] FVerts: %d (found at offset 0x%X)\n", arrCount, (int)sc);
            verts.resize(arrCount);
            for (int i = 0; i < arrCount; i++) {
                memcpy(&verts[i].pVertex, serialData + dataStart + (size_t)i * 8, 4);
                memcpy(&verts[i].iSide, serialData + dataStart + (size_t)i * 8 + 4, 4);
            }
            pos = dataStart + dataBytes;
            break;
        }
    }
    if (verts.empty()) return results;

    // ─── Diagnostics: node quality ───
    {
        int valid3 = 0, skippedPool = 0, skippedVerts = 0, totalNV = 0;
        for (auto& n : nodes) {
            if (n.numVertices < 3) { skippedVerts++; continue; }
            if (n.iVertPool < 0 || n.iVertPool + n.numVertices > (int)verts.size()) { skippedPool++; continue; }
            valid3++;
            totalNV += n.numVertices;
        }
        printf("[BSP] Polygon nodes: %d (of %d), FVerts: %d\n", valid3, (int)nodes.size(), (int)verts.size());
    }

    // ─── 9. Triangulate BSP nodes into renderable mesh chunks ───
    // Split into multiple chunks at 60000 vertices to stay within uint16 index range.

    ParsedMesh chunk;
    chunk.valid = false;
    chunk.name = "__BSP__";
    int totalVerts = 0, totalTris = 0;

    for (auto& node : nodes) {
        if (node.numVertices < 3) continue;
        if (node.iVertPool < 0) continue;
        if (node.iVertPool + node.numVertices > (int)verts.size()) continue;

        // Check if adding this node would overflow the current chunk
        if ((int)chunk.vertices.size() + node.numVertices > 60000 && !chunk.vertices.empty()) {
            // Finalize current chunk
            chunk.valid = true;
            chunk.boundsMin = {1e9f, 1e9f, 1e9f};
            chunk.boundsMax = {-1e9f, -1e9f, -1e9f};
            for (auto& v : chunk.vertices) {
                if (v.x < chunk.boundsMin.x) chunk.boundsMin.x = v.x;
                if (v.y < chunk.boundsMin.y) chunk.boundsMin.y = v.y;
                if (v.z < chunk.boundsMin.z) chunk.boundsMin.z = v.z;
                if (v.x > chunk.boundsMax.x) chunk.boundsMax.x = v.x;
                if (v.y > chunk.boundsMax.y) chunk.boundsMax.y = v.y;
                if (v.z > chunk.boundsMax.z) chunk.boundsMax.z = v.z;
            }
            totalVerts += (int)chunk.vertices.size();
            totalTris += (int)chunk.triangles.size();
            results.push_back(std::move(chunk));
            chunk = ParsedMesh();
            chunk.valid = false;
            chunk.name = "__BSP__";
        }

        float nx = node.planeX;
        float ny = node.planeY;
        float nz = node.planeZ;
        float nlen = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (nlen > 0.001f) { nx /= nlen; ny /= nlen; nz /= nlen; }

        int baseVert = (int)chunk.vertices.size();
        bool validPoly = true;

        for (int v = 0; v < node.numVertices; v++) {
            int vertIdx = node.iVertPool + v;
            int pointIdx = verts[vertIdx].pVertex;
            if (pointIdx < 0 || pointIdx >= numPoints) { validPoly = false; break; }

            MeshVertex mv;
            mv.x = points[pointIdx * 3 + 0];
            mv.y = points[pointIdx * 3 + 1];
            mv.z = points[pointIdx * 3 + 2];
            mv.nx = nx;
            mv.ny = ny;
            mv.nz = nz;
            mv.u = 0;
            mv.v = 0;
            // Reject vertices with NaN or extreme values
            if (std::isnan(mv.x) || std::isnan(mv.y) || std::isnan(mv.z) ||
                std::abs(mv.x) > 200000.0f || std::abs(mv.y) > 200000.0f || std::abs(mv.z) > 200000.0f) {
                validPoly = false; break;
            }
            chunk.vertices.push_back(mv);
        }

        if (!validPoly) {
            chunk.vertices.resize(baseVert);
            continue;
        }

        for (int v = 2; v < node.numVertices; v++) {
            MeshTriangle tri;
            tri.i0 = (uint16_t)baseVert;
            tri.i1 = (uint16_t)(baseVert + v - 1);
            tri.i2 = (uint16_t)(baseVert + v);
            chunk.triangles.push_back(tri);
        }
    }

    // Finalize last chunk
    if (!chunk.triangles.empty()) {
        chunk.valid = true;
        chunk.boundsMin = {1e9f, 1e9f, 1e9f};
        chunk.boundsMax = {-1e9f, -1e9f, -1e9f};
        for (auto& v : chunk.vertices) {
            if (v.x < chunk.boundsMin.x) chunk.boundsMin.x = v.x;
            if (v.y < chunk.boundsMin.y) chunk.boundsMin.y = v.y;
            if (v.z < chunk.boundsMin.z) chunk.boundsMin.z = v.z;
            if (v.x > chunk.boundsMax.x) chunk.boundsMax.x = v.x;
            if (v.y > chunk.boundsMax.y) chunk.boundsMax.y = v.y;
            if (v.z > chunk.boundsMax.z) chunk.boundsMax.z = v.z;
        }
        totalVerts += (int)chunk.vertices.size();
        totalTris += (int)chunk.triangles.size();
        results.push_back(std::move(chunk));
    }

    printf("[BSP] Triangulated: %d vertices, %d triangles in %d chunks\n",
           totalVerts, totalTris, (int)results.size());

    return results;
}
