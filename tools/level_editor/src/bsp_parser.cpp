#include "bsp_parser.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <unordered_map>

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

// Vengeance FBspNode (100 bytes) — fully resolved layout:
//   +0:  FPlane (16B)           — plane equation
//   +16: ZoneMask (16B)         — 128-bit zone visibility bitmask
//   +32: INT32 iVertPool        — vertex pool index
//   +36: INT32 iSurf            — surface index
//   +40: INT32 iBack            — back child (-1 = leaf)
//   +44: INT32 iFront           — front child (-1 = leaf)
//   +48: INT32 iPlane           — coplanar node index
//   +52: float BoundOrigin XYZ  — Vengeance-added bounding sphere (12B)
//   +64: float BoundRadius      — Vengeance-added
//   +68: INT32 iCollisionBound  — often -1
//   +72: INT32 iRenderBound     — 0 or -1
//   +76: BYTE  NodeFlags
//   +77: BYTE  iZone[0]         — back-side zone (100% ZoneMask correlation)
//   +78: BYTE  iZone[1]         — front-side zone
//   +79: BYTE  Pad
//   +80: INT32 iLeaf[0]         — often -1
//   +84: INT32 iLeaf[1]         — often -1
//   +88: INT32 NumVertices      — expanded from UE2 BYTE
//   +92: INT32 iContentBound    — Vengeance-added
//   +96: INT32 iRenderZone      — Vengeance-added (0-127)
struct BSPNode {
    float planeX, planeY, planeZ, planeW;
    uint8_t zoneMask[16]; // +16: 128-bit zone visibility mask
    int32_t iVertPool;    // +32
    int32_t iSurf;        // +36
    int32_t iBack;        // +40 (back child, -1 = leaf)
    int32_t iFront;       // +44 (front child, -1 = leaf)
    uint8_t numVertices;  // +88: INT32 in file, clamped to uint8
    uint8_t iZone;        // +77: back-zone byte (100% ZoneMask correlation confirmed)
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

    // Probe: compare +88 vs +92 for numVertices
    {
        int poly88 = 0, poly92 = 0;
        int maxNV88 = 0, maxNV92 = 0;
        long long sumNV88 = 0, sumNV92 = 0;
        for (int i = 0; i < numNodes; i++) {
            size_t noff = pos + (size_t)i * 100;
            int32_t nv88, nv92;
            memcpy(&nv88, data + noff + 88, 4);
            memcpy(&nv92, data + noff + 92, 4);
            if (nv88 >= 3 && nv88 < 256) { poly88++; sumNV88 += nv88; if (nv88 > maxNV88) maxNV88 = nv88; }
            if (nv92 >= 3 && nv92 < 256) { poly92++; sumNV92 += nv92; if (nv92 > maxNV92) maxNV92 = nv92; }
        }
        printf("[BSP] NumVerts offset probe: +88: %d polys (avg=%.1f max=%d), +92: %d polys (avg=%.1f max=%d)\n",
               poly88, poly88 ? (double)sumNV88/poly88 : 0.0, maxNV88,
               poly92, poly92 ? (double)sumNV92/poly92 : 0.0, maxNV92);
    }

    nodes.resize(numNodes);
    for (int i = 0; i < numNodes; i++) {
        size_t noff = pos + (size_t)i * 100;
        // FPlane at +0 (16B)
        memcpy(&nodes[i].planeX, data + noff, 4);
        memcpy(&nodes[i].planeY, data + noff + 4, 4);
        memcpy(&nodes[i].planeZ, data + noff + 8, 4);
        memcpy(&nodes[i].planeW, data + noff + 12, 4);
        // See struct comment above for full 100B layout
        memcpy(nodes[i].zoneMask, data + noff + 16, 16);
        memcpy(&nodes[i].iVertPool, data + noff + 32, 4);
        memcpy(&nodes[i].iSurf, data + noff + 36, 4);
        memcpy(&nodes[i].iBack, data + noff + 40, 4);
        memcpy(&nodes[i].iFront, data + noff + 44, 4);
        int32_t nv; memcpy(&nv, data + noff + 88, 4);
        nodes[i].numVertices = (nv >= 0 && nv < 256) ? (uint8_t)nv : 0;
        nodes[i].iZone = data[noff + 77]; // +77: back-zone byte, 100% ZoneMask correlation confirmed
    }
    pos += nodeBytes;
    return true;
}

// FBspSurf: 8B per-element Vengeance header + ~52B body (C.1.3)
struct BSPSurf {
    int materialRef;     // CI object ref: positive=export(1-based), negative=import, 0=null
    int32_t polyFlags;
    int32_t pBase;       // index into Points[] — texture origin
    int32_t vNormal;     // index into Vectors[] — surface normal
    int32_t vTextureU;   // index into Vectors[] — texture U axis
    int32_t vTextureV;   // index into Vectors[] — texture V axis
};

static bool ParseSurfs(const uint8_t* data, size_t& pos, size_t endPos,
                        std::vector<BSPSurf>& surfs)
{
    if (pos + 1 > endPos) return false;
    size_t br;
    int numSurfs = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
    if (numSurfs < 0 || numSurfs > 500000) return false;

    surfs.resize(numSurfs);
    size_t surfStart = pos;
    for (int i = 0; i < numSurfs; i++) {
        // 8B per-element Vengeance header (check=4, sub_ver)
        if (pos + 8 > endPos) { surfs.resize(i); return true; }
        pos += 8;

        // CI Material (UObject reference)
        surfs[i].materialRef = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;

        // 24B fixed: PolyFlags(4) + pBase(4) + vNormal(4) + vTextureU(4) + vTextureV(4) + iBrushPoly(4)
        if (pos + 24 > endPos) { surfs.resize(i); return true; }
        memcpy(&surfs[i].polyFlags,  data + pos, 4);
        memcpy(&surfs[i].pBase,      data + pos + 4, 4);
        memcpy(&surfs[i].vNormal,    data + pos + 8, 4);
        memcpy(&surfs[i].vTextureU,  data + pos + 12, 4);
        memcpy(&surfs[i].vTextureV,  data + pos + 16, 4);
        pos += 24;

        // CI Actor (variable length UObject reference)
        ReadCompactIndex(data + pos, endPos - pos, br); pos += br;

        // 20B remaining fixed fields (PanU, PanV, iZone, LightMap, etc.)
        if (pos + 20 > endPos) { surfs.resize(i); return true; }
        pos += 20;
    }
    printf("[BSP] Parsed %d surfs (%d bytes)\n", numSurfs, (int)(pos - surfStart));
    return true;
}

// FVert: INT32 pVertex + INT32 iSide (8B each, per C.1.4)
struct BSPVert {
    int32_t pVertex; // index into Points array
    int32_t iSide;
};


// ─── Resolve material ref to texture name ────────────────────────────────────

static std::string ResolveMaterialName(int materialRef,
                                        const std::vector<BSPExportInfo>& exports,
                                        const std::vector<std::string>& importNames)
{
    if (materialRef == 0) return "";
    if (materialRef > 0) {
        int idx = materialRef - 1;
        if (idx < (int)exports.size()) return exports[idx].objectName;
    } else {
        int idx = -materialRef - 1;
        if (idx < (int)importNames.size()) return importNames[idx];
    }
    return "";
}

// Strip _Shader / _shader suffix for texture lookup
static std::string StripShaderSuffix(const std::string& name)
{
    std::string result = name;
    if (result.size() > 7) {
        std::string suffix7 = result.substr(result.size() - 7);
        if (suffix7 == "_Shader" || suffix7 == "_shader")
            result = result.substr(0, result.size() - 7);
    }
    return result;
}

// ─── Main BSP Parser ─────────────────────────────────────────────────────────

std::vector<ParsedMesh> ParseBSPGeometry(const uint8_t* serialData, int serialSize,
                                         const std::vector<std::string>& names,
                                         const std::vector<BSPExportInfo>& exports,
                                         const std::vector<std::string>& importNames,
                                         std::vector<BSPTreeNodeOut>* outTree)
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
    int numVectors = (int)vectors.size() / 3;
    // ─── 5. TArray<FVector> Points ───
    std::vector<float> points;
    if (!ReadTArrayVectors(serialData, pos, endPos, points)) return results;
    int numPoints = (int)points.size() / 3;
    printf("[BSP] Vectors: %d, Points: %d\n", numVectors, numPoints);

    // ─── 6. Nodes (100B each) ───
    std::vector<BSPNode> nodes;
    if (!ParseNodes(serialData, pos, endPos, nodes)) return results;

    // Output BSP tree for zone traversal
    if (outTree) {
        outTree->resize(nodes.size());
        for (size_t i = 0; i < nodes.size(); i++) {
            (*outTree)[i].planeX = nodes[i].planeX;
            (*outTree)[i].planeY = nodes[i].planeY;
            (*outTree)[i].planeZ = nodes[i].planeZ;
            (*outTree)[i].planeW = nodes[i].planeW;
            (*outTree)[i].iFront = nodes[i].iFront;
            (*outTree)[i].iBack = nodes[i].iBack;
            memcpy((*outTree)[i].zoneMask, nodes[i].zoneMask, 16);
            (*outTree)[i].iZone = nodes[i].iZone;
        }
    }

    // ─── 7. Surfs (sequential parse, variable-length per element) ───
    size_t preSurfPos = pos;
    std::vector<BSPSurf> surfs;
    if (!ParseSurfs(serialData, pos, endPos, surfs)) return results;
    printf("[BSP] Nodes: %d, Surfs: %d\n", (int)nodes.size(), (int)surfs.size());

    // ─── 8. Read FVert array (C.1.4: CI count + N × 8B) ───
    // Only consider POLYGON nodes (numVertices >= 3) for pool size calculation
    int minVertCount = 0;
    int totalVertRefs = 0;
    for (auto& n : nodes) {
        if (n.numVertices < 3) continue; // skip branch/leaf nodes
        int end = n.iVertPool + (int)n.numVertices;
        if (end > minVertCount) minVertCount = end;
        totalVertRefs += (int)n.numVertices;
    }
    printf("[BSP] FVert pool: minRequired=%d, totalVertRefs=%d\n", minVertCount, totalVertRefs);

    std::vector<BSPVert> verts;
    {
        size_t br2;
        int numVerts = ReadCompactIndex(serialData + pos, endPos - pos, br2);
        printf("[BSP] FVert CI at 0x%X: count=%d (min needed=%d)\n", (int)pos, numVerts, minVertCount);
        if (numVerts >= minVertCount && numVerts < 1000000) {
            pos += br2;
            size_t vertBytes = (size_t)numVerts * 8;
            if (pos + vertBytes <= endPos) {
                printf("[BSP] FVerts: %d (direct read at 0x%X)\n", numVerts, (int)(pos - br2));
                verts.resize(numVerts);
                for (int i = 0; i < numVerts; i++) {
                    memcpy(&verts[i].pVertex, serialData + pos + (size_t)i * 8, 4);
                    memcpy(&verts[i].iSide, serialData + pos + (size_t)i * 8 + 4, 4);
                }
                pos += vertBytes;
                // Validate: count out-of-range pVertex values
                int badCount = 0, goodCount = 0;
                for (int i = 0; i < numVerts; i++) {
                    if (verts[i].pVertex < 0 || verts[i].pVertex >= numPoints) badCount++;
                    else goodCount++;
                }
                printf("[BSP] FVert validation: %d good, %d BAD (out of range) of %d total\n",
                       goodCount, badCount, numVerts);
            }
        }
    }

    // Fallback: scan for FVert array if direct read failed
    if (verts.empty()) {
        for (size_t sc = preSurfPos; sc + 10 < endPos; sc++) {
            size_t br2;
            int arrCount = ReadCompactIndex(serialData + sc, endPos - sc, br2);
            if (arrCount < minVertCount || arrCount > 1000000) continue;
            size_t dataStart = sc + br2;
            size_t dataBytes = (size_t)arrCount * 8;
            if (dataStart + dataBytes > endPos) continue;
            bool ok = true;
            for (int t = 0; t < 10 && t < arrCount; t++) {
                int32_t pv; memcpy(&pv, serialData + dataStart + (size_t)t * 8, 4);
                if (pv < 0 || pv >= numPoints) { ok = false; break; }
            }
            if (!ok) continue;
            printf("[BSP] FVerts: %d (scan fallback at 0x%X)\n", arrCount, (int)sc);
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

    // ─── Diagnostics ───
    {
        int valid3 = 0;
        int nv3_7 = 0, nv8_15 = 0, nv16_31 = 0, nv32_63 = 0, nv64plus = 0;
        for (auto& n : nodes) {
            if (n.numVertices < 3) continue;
            if (n.iVertPool < 0 || n.iVertPool + n.numVertices > (int)verts.size()) continue;
            valid3++;
            if (n.numVertices <= 7) nv3_7++;
            else if (n.numVertices <= 15) nv8_15++;
            else if (n.numVertices <= 31) nv16_31++;
            else if (n.numVertices <= 63) nv32_63++;
            else nv64plus++;
        }
        printf("[BSP] Polygon nodes: %d (of %d), FVerts: %d\n", valid3, (int)nodes.size(), (int)verts.size());
        printf("[BSP] NumVerts distribution: 3-7:%d  8-15:%d  16-31:%d  32-63:%d  64+:%d\n",
               nv3_7, nv8_15, nv16_31, nv32_63, nv64plus);
    }

    // Count unique materials for diagnostics
    int matResolved = 0, matNull = 0;
    {
        std::unordered_map<int, int> matCounts;
        for (auto& n : nodes) {
            if (n.numVertices < 3) continue;
            if (n.iSurf >= 0 && n.iSurf < (int)surfs.size())
                matCounts[surfs[n.iSurf].materialRef]++;
        }
        for (auto& mc : matCounts) {
            if (mc.first != 0) matResolved++; else matNull++;
        }
        printf("[BSP] Unique materials: %d (+ %d null)\n", matResolved, matNull);
    }

    // ─── 9. Triangulate BSP nodes, grouped by material ───────────────────────
    // Each unique material becomes a separate ParsedMesh with textureName set.

    struct MaterialGroup {
        std::string matName;
        std::vector<MeshVertex> vertices;
        std::vector<MeshTriangle> triangles;
        uint8_t zoneMask[16] = {}; // union of all nodes' ZoneMasks
    };
    std::unordered_map<int64_t, MaterialGroup> groups; // key: materialRef * 256 + iZone

    int totalVerts = 0, totalTris = 0;

    // Compute level bounding box for dynamic degenerate threshold
    float levelMinX=1e30f, levelMinY=1e30f, levelMinZ=1e30f;
    float levelMaxX=-1e30f, levelMaxY=-1e30f, levelMaxZ=-1e30f;
    for (int i = 0; i < numPoints; i++) {
        float px = points[i*3], py = points[i*3+1], pz = points[i*3+2];
        if (px < levelMinX) levelMinX = px; if (px > levelMaxX) levelMaxX = px;
        if (py < levelMinY) levelMinY = py; if (py > levelMaxY) levelMaxY = py;
        if (pz < levelMinZ) levelMinZ = pz; if (pz > levelMaxZ) levelMaxZ = pz;
    }
    float levelSpanX = levelMaxX - levelMinX;
    float levelSpanY = levelMaxY - levelMinY;
    float levelSpanZ = levelMaxZ - levelMinZ;
    float maxLevelSpan = levelSpanX;
    if (levelSpanY > maxLevelSpan) maxLevelSpan = levelSpanY;
    if (levelSpanZ > maxLevelSpan) maxLevelSpan = levelSpanZ;
    // A single polygon spanning more than 50% of the level in any axis is suspicious
    float degenThreshold = maxLevelSpan * 0.5f;
    if (degenThreshold < 10000.0f) degenThreshold = 10000.0f; // minimum floor
    printf("[BSP] Level span: %.0f x %.0f x %.0f, degenerate threshold: %.0f\n",
           levelSpanX, levelSpanY, levelSpanZ, degenThreshold);

    int skippedBadVert = 0, skippedBBox = 0;

    for (auto& node : nodes) {
        if (node.numVertices < 3) continue;
        if (node.iVertPool < 0) continue;
        if (node.iVertPool + node.numVertices > (int)verts.size()) continue;

        // Pre-check: validate all vertex indices and compute bounding box
        {
            float pMinX=1e30f, pMinY=1e30f, pMinZ=1e30f;
            float pMaxX=-1e30f, pMaxY=-1e30f, pMaxZ=-1e30f;
            bool badPoly = false;
            for (int v = 0; v < node.numVertices; v++) {
                int pi = verts[node.iVertPool + v].pVertex;
                if (pi < 0 || pi >= numPoints) { badPoly = true; break; }
                float px = points[pi*3], py = points[pi*3+1], pz = points[pi*3+2];
                if (px < pMinX) pMinX = px; if (px > pMaxX) pMaxX = px;
                if (py < pMinY) pMinY = py; if (py > pMaxY) pMaxY = py;
                if (pz < pMinZ) pMinZ = pz; if (pz > pMaxZ) pMaxZ = pz;
            }
            if (badPoly) { skippedBadVert++; continue; }
            float spanX = pMaxX - pMinX, spanY = pMaxY - pMinY, spanZ = pMaxZ - pMinZ;
            if (spanX > degenThreshold || spanY > degenThreshold || spanZ > degenThreshold) {
                skippedBBox++;
                continue;
            }
        }

        // Get surf for this node (for texture vectors and material)
        int surfIdx = node.iSurf;
        BSPSurf* surf = nullptr;
        if (surfIdx >= 0 && surfIdx < (int)surfs.size())
            surf = &surfs[surfIdx];

        int matRef = surf ? surf->materialRef : 0;

        // Skip invisible/utility surfaces (FakeBackdrop, Portal, Zoning brushes)
        if (surf) {
            const uint32_t PF_Invisible    = 0x00000001;
            const uint32_t PF_FakeBackdrop = 0x00000080;
            const uint32_t PF_Portal       = 0x04000000;
            if (surf->polyFlags & (PF_Invisible | PF_FakeBackdrop | PF_Portal))
                continue;
        }

        // Group by (material, zone) so each chunk is spatially coherent
        int64_t groupKey = (int64_t)matRef * 256 + (int64_t)node.iZone;
        if (groups.find(groupKey) == groups.end()) {
            MaterialGroup g;
            g.matName = ResolveMaterialName(matRef, exports, importNames);
            groups[groupKey] = std::move(g);
        }
        auto& group = groups[groupKey];

        // Accumulate zone visibility: OR node's mask into group's mask
        for (int zi = 0; zi < 16; zi++) group.zoneMask[zi] |= node.zoneMask[zi];

        // Check if we need to split this group (uint16 index limit)
        if ((int)group.vertices.size() + node.numVertices > 60000) {
            // Flush current group to results
            if (!group.triangles.empty()) {
                ParsedMesh chunk;
                chunk.name = "__BSP__";
                chunk.textureName = StripShaderSuffix(group.matName);
                chunk.vertices = std::move(group.vertices);
                chunk.triangles = std::move(group.triangles);
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
            group.vertices.clear();
            group.triangles.clear();
        }

        // Compute normal (from node plane)
        float nx = node.planeX;
        float ny = node.planeY;
        float nz = node.planeZ;
        float nlen = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (nlen > 0.001f) { nx /= nlen; ny /= nlen; nz /= nlen; }

        // Get texture vectors and base point from surf (for UV computation)
        float texUx = 0, texUy = 0, texUz = 0;
        float texVx = 0, texVy = 0, texVz = 0;
        float baseX = 0, baseY = 0, baseZ = 0;
        bool hasTexVecs = false;
        if (surf) {
            int tuIdx = surf->vTextureU;
            int tvIdx = surf->vTextureV;
            int bpIdx = surf->pBase;
            if (tuIdx >= 0 && tuIdx < numVectors &&
                tvIdx >= 0 && tvIdx < numVectors &&
                bpIdx >= 0 && bpIdx < numPoints) {
                texUx = vectors[tuIdx * 3 + 0];
                texUy = vectors[tuIdx * 3 + 1];
                texUz = vectors[tuIdx * 3 + 2];
                texVx = vectors[tvIdx * 3 + 0];
                texVy = vectors[tvIdx * 3 + 1];
                texVz = vectors[tvIdx * 3 + 2];
                baseX = points[bpIdx * 3 + 0];
                baseY = points[bpIdx * 3 + 1];
                baseZ = points[bpIdx * 3 + 2];
                hasTexVecs = true;
            }
        }

        int baseVert = (int)group.vertices.size();
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

            // Compute UV from texture vectors: dot(vertex - base, texAxis)
            // The texture vectors already encode 1/textureSize scaling
            if (hasTexVecs) {
                float dx = mv.x - baseX;
                float dy = mv.y - baseY;
                float dz = mv.z - baseZ;
                // UV in texels; render loop normalizes per texture size via uTexScale uniform
                mv.u = dx * texUx + dy * texUy + dz * texUz;
                mv.v = dx * texVx + dy * texVy + dz * texVz;
            } else {
                mv.u = 0;
                mv.v = 0;
            }

            // Reject vertices with NaN or extreme values
            if (std::isnan(mv.x) || std::isnan(mv.y) || std::isnan(mv.z) ||
                std::abs(mv.x) > 200000.0f || std::abs(mv.y) > 200000.0f || std::abs(mv.z) > 200000.0f) {
                validPoly = false; break;
            }
            group.vertices.push_back(mv);
        }

        if (!validPoly) {
            group.vertices.resize(baseVert);
            continue;
        }

        for (int v = 2; v < node.numVertices; v++) {
            MeshTriangle tri;
            tri.i0 = (uint16_t)baseVert;
            tri.i1 = (uint16_t)(baseVert + v - 1);
            tri.i2 = (uint16_t)(baseVert + v);
            group.triangles.push_back(tri);
        }
    }

    // ─── 10. Finalize all material groups into ParsedMesh results ────────────
    for (auto& [groupKey, group] : groups) {
        if (group.triangles.empty()) continue;

        // Skip utility brush materials that shouldn't be rendered
        std::string strippedName = StripShaderSuffix(group.matName);
        if (strippedName.find("FakeBackdrop") != std::string::npos ||
            strippedName.find("ZoningOnly") != std::string::npos ||
            strippedName.find("PortalBrush") != std::string::npos ||
            strippedName.find("AntiPortalBrush") != std::string::npos ||
            strippedName.find("WaterLowRef") != std::string::npos ||
            strippedName.find("CalmWater") != std::string::npos ||
            strippedName.find("Calmwater") != std::string::npos ||
            strippedName == "DefaultTexture" ||
            strippedName == "BlackTexture" ||
            strippedName == "Black")
            continue;

        ParsedMesh chunk;
        chunk.name = "__BSP__";
        chunk.textureName = strippedName;
        memcpy(chunk.zoneMask, group.zoneMask, 16);
        chunk.vertices = std::move(group.vertices);
        chunk.triangles = std::move(group.triangles);
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

    printf("[BSP] Triangulated: %d vertices, %d triangles in %d material chunks (skipped: %d bad-vert, %d bbox>5k)\n",
           totalVerts, totalTris, (int)results.size(), skippedBadVert, skippedBBox);

    return results;
}
