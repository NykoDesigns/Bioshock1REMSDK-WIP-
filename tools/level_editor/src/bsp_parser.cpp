#include "bsp_parser.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <algorithm>

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
//   +78: BYTE  NumVertices      — polygon vertex count (100% planarity match confirmed)
//   +79: BYTE  iZone[1]         — front-side zone (or Pad)
//   +80: INT32 iLeaf[0]         — often -1
//   +84: INT32 iLeaf[1]         — often -1
//   +88: INT32 (unknown)        — NOT NumVertices; was misidentified
//   +92: INT32 iContentBound    — Vengeance-added
//   +96: INT32 iRenderZone      — Vengeance-added (0-127)
struct BSPNode {
    float planeX, planeY, planeZ, planeW;
    uint8_t zoneMask[16]; // +16: 128-bit zone visibility mask
    int32_t iVertPool;    // +32
    int32_t iSurf;        // +36
    int32_t iBack;        // +40 (back child, -1 = leaf)
    int32_t iFront;       // +44 (front child, -1 = leaf)
    uint8_t numVertices;  // +78: BYTE (NOT +88 which was a misidentification)
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

    // Raw dump first 3 nodes for debugging field offsets
    for (int i = 0; i < 3 && i < numNodes; i++) {
        size_t noff = pos + (size_t)i * 100;
        printf("[BSP-NODE-RAW] Node[%d] at 0x%X:\n", i, (int)noff);
        printf("  +0:  %08X %08X %08X %08X (FPlane)\n",
               *(uint32_t*)(data+noff), *(uint32_t*)(data+noff+4),
               *(uint32_t*)(data+noff+8), *(uint32_t*)(data+noff+12));
        printf("  +16: %08X%08X%08X%08X (ZoneMask)\n",
               *(uint32_t*)(data+noff+16), *(uint32_t*)(data+noff+20),
               *(uint32_t*)(data+noff+24), *(uint32_t*)(data+noff+28));
        int32_t f32, f36, f40, f44, f48, f88, f92;
        memcpy(&f32, data+noff+32, 4); memcpy(&f36, data+noff+36, 4);
        memcpy(&f40, data+noff+40, 4); memcpy(&f44, data+noff+44, 4);
        memcpy(&f48, data+noff+48, 4);
        memcpy(&f88, data+noff+88, 4); memcpy(&f92, data+noff+92, 4);
        float fb52, fb56, fb60, fb64;
        memcpy(&fb52, data+noff+52, 4); memcpy(&fb56, data+noff+56, 4);
        memcpy(&fb60, data+noff+60, 4); memcpy(&fb64, data+noff+64, 4);
        printf("  +32=%d +36=%d +40=%d +44=%d +48=%d\n", f32, f36, f40, f44, f48);
        printf("  +52=(%.1f,%.1f,%.1f) +64=%.1f (BoundSphere)\n", fb52, fb56, fb60, fb64);
        printf("  +76=0x%02X +77=%d +78=%d +88=%d +92=%d +96=%d\n",
               data[noff+76], data[noff+77], data[noff+78], f88, f92,
               *(int32_t*)(data+noff+96));
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
        nodes[i].numVertices = data[noff + 78]; // +78: NumVertices BYTE (100% planarity match confirmed by probe)
        nodes[i].iZone = data[noff + 77]; // +77: back-zone byte, 100% ZoneMask correlation confirmed
    }
    pos += nodeBytes;
    return true;
}

// FBspSurf: 8B per-element Vengeance header + ~52B body (C.1.3)
struct BSPSurf {
    int materialRef;     // CI object ref: positive=export(1-based), negative=import, 0=null
    int32_t polyFlags;
    int32_t pBase;       // index into Points[] — texture origin (pan offsets baked in for v>=78)
    int32_t vNormal;     // index into Vectors[] — surface normal
    int32_t vTextureU;   // index into Vectors[] — texture U axis
    int32_t vTextureV;   // index into Vectors[] — texture V axis
    int32_t iLightMap;   // Lightmap texture atlas index: 0=none, 1-N = 1-based index into lightmap texture list
    // PanU/PanV NOT serialized for package version >= 78 (BioShock=141)
    // Pan offsets are baked into pBase. Confirmed by UT2004 Engine.dll JGE+0x34 skip.
    float lightMapScale;  // from 20B tail: FPlane(16B) + LightMapScale(4B)
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
        memcpy(&surfs[i].iLightMap, data + pos + 20, 4);
        pos += 24;

        // CI Actor (variable length UObject reference)
        ReadCompactIndex(data + pos, endPos - pos, br); pos += br;

        // 20B remaining: FPlane(16B surface plane) + float LightMapScale(4B)
        // PanU/PanV NOT present — skipped for version >= 78 (confirmed by UT2004 serializer)
        if (pos + 20 > endPos) { surfs.resize(i); return true; }
        memcpy(&surfs[i].lightMapScale, data + pos + 16, 4);
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
                                         std::vector<BSPTreeNodeOut>* outTree,
                                         const std::vector<std::string>* lightMapNames)
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
    size_t preNodePos = pos; // save for later probing
    std::vector<BSPNode> nodes;
    if (!ParseNodes(serialData, pos, endPos, nodes)) return results;
    int numNodes_saved = (int)nodes.size(); // save count for probing

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

    auto ReadFVertArray = [&](const uint8_t* base, int count, std::vector<BSPVert>& out) {
        out.resize(count);
        for (int i = 0; i < count; i++) {
            memcpy(&out[i].pVertex, base + (size_t)i * 8, 4);
            memcpy(&out[i].iSide, base + (size_t)i * 8 + 4, 4);
        }
    };

    // Validate FVert candidate by checking entries actually referenced by polygon nodes
    auto ValidateReferencedFVerts = [&](const uint8_t* base, int count) -> float {
        int checked = 0, good = 0;
        for (auto& n : nodes) {
            if (n.numVertices < 3) continue;
            if (n.iVertPool < 0 || n.iVertPool + n.numVertices > count) continue;
            // Check first and last vertex of each polygon
            for (int vi = 0; vi < n.numVertices; vi += std::max(1, n.numVertices - 1)) {
                int32_t pv; memcpy(&pv, base + (size_t)(n.iVertPool + vi) * 8, 4);
                checked++;
                if (pv >= 0 && pv < numPoints) good++;
            }
            if (checked > 200) break; // enough to be confident
        }
        return checked > 0 ? (float)good / (float)checked : 0.0f;
    };

    std::vector<BSPVert> verts;
    {
        size_t br2;
        int numVerts = ReadCompactIndex(serialData + pos, endPos - pos, br2);
        printf("[BSP] FVert CI at 0x%X: count=%d (min needed=%d)\n", (int)pos, numVerts, minVertCount);
        if (numVerts >= minVertCount && numVerts < 5000000) {
            size_t dataStart = pos + br2;
            size_t vertBytes = (size_t)numVerts * 8;
            if (dataStart + vertBytes <= endPos) {
                float validity = ValidateReferencedFVerts(serialData + dataStart, numVerts);
                printf("[BSP] FVert direct probe: count=%d, referenced validity=%.1f%%\n", numVerts, validity * 100.0f);
                if (validity >= 0.95f) {
                    printf("[BSP] FVerts: %d (direct read at 0x%X)\n", numVerts, (int)pos);
                    ReadFVertArray(serialData + dataStart, numVerts, verts);
                    pos = dataStart + vertBytes;
                } else {
                    printf("[BSP] FVert direct read REJECTED (%.1f%% referenced valid, need 95%%)\n", validity * 100.0f);
                }
            }
        }
    }

    // Fallback: scan for FVert array if direct read failed or was rejected
    if (verts.empty()) {
        float bestValidity = 0;
        size_t bestOffset = 0;
        int bestCount = 0;
        for (size_t sc = preSurfPos; sc + 10 < endPos; sc++) {
            size_t br2;
            int arrCount = ReadCompactIndex(serialData + sc, endPos - sc, br2);
            if (arrCount < minVertCount || arrCount > 5000000) continue;
            size_t dataStart = sc + br2;
            size_t dataBytes = (size_t)arrCount * 8;
            if (dataStart + dataBytes > endPos) continue;
            float validity = ValidateReferencedFVerts(serialData + dataStart, arrCount);
            if (validity > bestValidity) {
                bestValidity = validity;
                bestOffset = sc;
                bestCount = arrCount;
            }
            if (validity >= 0.99f) break; // near-perfect match, stop searching
        }
        if (bestValidity >= 0.90f && bestCount > 0) {
            size_t br2;
            ReadCompactIndex(serialData + bestOffset, endPos - bestOffset, br2);
            size_t dataStart = bestOffset + br2;
            printf("[BSP] FVerts: %d (scan fallback at 0x%X, ref validity=%.1f%%)\n",
                   bestCount, (int)bestOffset, bestValidity * 100.0f);
            ReadFVertArray(serialData + dataStart, bestCount, verts);
            pos = dataStart + (size_t)bestCount * 8;
        }
    }
    if (verts.empty()) return results;

    // ─── Probe: find real numVertices offset by testing every byte position ───
    // For each byte offset, interpret it as numVertices and check planarity match
    {
        // Find node data start: preNodePos has CI prefix, skip it to get to raw node data
        size_t br_tmp;
        ReadCompactIndex(serialData + preNodePos, endPos - preNodePos, br_tmp);
        size_t nodesStart = preNodePos + br_tmp; // start of first node's 100B data
        printf("[BSP-PROBE] Testing byte offsets for numVertices (nodes at 0x%X)...\n", (int)nodesStart);
        int bestByteOff = -1;
        float bestScore = 0;
        for (int byteOff = 32; byteOff < 100; byteOff++) {
            int tested = 0, matched = 0;
            for (int ni = 0; ni < numNodes_saved && tested < 800; ni++) {
                size_t noff = nodesStart + (size_t)ni * 100;
                uint8_t candidate = serialData[noff + byteOff];
                if (candidate < 3 || candidate > 200) continue;
                // Read iVertPool from +32
                int32_t pool; memcpy(&pool, serialData + noff + 32, 4);
                if (pool < 0 || pool + candidate > (int)verts.size()) continue;
                // Read plane from +0
                float plX, plY, plZ, plW;
                memcpy(&plX, serialData+noff, 4); memcpy(&plY, serialData+noff+4, 4);
                memcpy(&plZ, serialData+noff+8, 4); memcpy(&plW, serialData+noff+12, 4);
                float nrm = std::sqrt(plX*plX + plY*plY + plZ*plZ);
                if (nrm < 0.5f) continue;
                int goodVerts = 0;
                bool bad = false;
                for (int v = 0; v < candidate; v++) {
                    int pi = verts[pool + v].pVertex;
                    if (pi < 0 || pi >= numPoints) { bad = true; break; }
                    float px = points[pi*3], py = points[pi*3+1], pz = points[pi*3+2];
                    float d = std::abs(px*plX + py*plY + pz*plZ - plW);
                    if (d <= 1.0f) goodVerts++;
                    else break;
                }
                if (bad) continue;
                tested++;
                if (goodVerts == candidate) matched++;
            }
            float score = tested > 50 ? (float)matched / tested : 0;
            if (score > bestScore) { bestScore = score; bestByteOff = byteOff; }
            if (tested > 50 && score > 0.3f)
                printf("[BSP-PROBE] byte+%d: %d/%d matched (%.1f%%)\n", byteOff, matched, tested, score*100);
        }
        printf("[BSP-PROBE] Best numVerts byte offset: +%d (%.1f%% planarity match)\n", bestByteOff, bestScore*100);
    }

    // ─── Diagnostics ───
    {
        int valid3 = 0;
        int nv3_7 = 0, nv8_15 = 0, nv16_31 = 0, nv32_63 = 0, nv64plus = 0;
        int planarOK = 0, planarBad = 0;
        float maxPlaneDist = 0;
        double totalArea = 0;
        int areaSmall = 0, areaMed = 0, areaLarge = 0, areaHuge = 0;
        for (auto& n : nodes) {
            if (n.numVertices < 3) continue;
            if (n.iVertPool < 0 || n.iVertPool + n.numVertices > (int)verts.size()) continue;
            valid3++;
            if (n.numVertices <= 7) nv3_7++;
            else if (n.numVertices <= 15) nv8_15++;
            else if (n.numVertices <= 31) nv16_31++;
            else if (n.numVertices <= 63) nv32_63++;
            else nv64plus++;

            // Planarity check: verify vertices lie on node's plane
            bool allOnPlane = true;
            for (int v = 0; v < n.numVertices; v++) {
                int pi = verts[n.iVertPool + v].pVertex;
                if (pi < 0 || pi >= numPoints) { allOnPlane = false; break; }
                float px = points[pi*3], py = points[pi*3+1], pz = points[pi*3+2];
                float dist = std::abs(px * n.planeX + py * n.planeY + pz * n.planeZ - n.planeW);
                if (dist > maxPlaneDist) maxPlaneDist = dist;
                if (dist > 1.0f) allOnPlane = false;
            }
            if (allOnPlane) planarOK++;
            else {
                planarBad++;
                if (planarBad <= 3) {
                    printf("[BSP-PLANAR-FAIL] Node[%d]: plane=(%.4f,%.4f,%.4f,%.4f) iVertPool=%d nv=%d\n",
                           (int)(&n - &nodes[0]), n.planeX, n.planeY, n.planeZ, n.planeW, n.iVertPool, n.numVertices);
                    for (int v = 0; v < std::min((int)n.numVertices + 2, 12); v++) {
                        int vIdx = n.iVertPool + v;
                        if (vIdx < 0 || vIdx >= (int)verts.size()) break;
                        int pi = verts[vIdx].pVertex;
                        int si = verts[vIdx].iSide;
                        if (pi >= 0 && pi < numPoints) {
                            float px = points[pi*3], py = points[pi*3+1], pz = points[pi*3+2];
                            float dist = px * n.planeX + py * n.planeY + pz * n.planeZ - n.planeW;
                            printf("    vert[%d] pIdx=%d iSide=%d pos=(%.1f,%.1f,%.1f) planeDist=%.1f%s\n",
                                   v, pi, si, px, py, pz, dist, v >= n.numVertices ? " (BEYOND)" : "");
                        } else {
                            printf("    vert[%d] pIdx=%d iSide=%d (OUT OF RANGE)%s\n",
                                   v, pi, si, v >= n.numVertices ? " (BEYOND)" : "");
                        }
                    }
                }
            }

            // Area estimate using first triangle
            int p0 = verts[n.iVertPool].pVertex, p1 = verts[n.iVertPool+1].pVertex, p2 = verts[n.iVertPool+2].pVertex;
            if (p0 >= 0 && p0 < numPoints && p1 >= 0 && p1 < numPoints && p2 >= 0 && p2 < numPoints) {
                float ax = points[p1*3]-points[p0*3], ay = points[p1*3+1]-points[p0*3+1], az = points[p1*3+2]-points[p0*3+2];
                float bx = points[p2*3]-points[p0*3], by = points[p2*3+1]-points[p0*3+1], bz = points[p2*3+2]-points[p0*3+2];
                float cx = ay*bz-az*by, cy = az*bx-ax*bz, cz = ax*by-ay*bx;
                float area = 0.5f * std::sqrt(cx*cx+cy*cy+cz*cz);
                totalArea += area;
                if (area < 100) areaSmall++;
                else if (area < 10000) areaMed++;
                else if (area < 1000000) areaLarge++;
                else areaHuge++;
            }
        }
        printf("[BSP] Polygon nodes: %d (of %d), FVerts: %d\n", valid3, (int)nodes.size(), (int)verts.size());
        printf("[BSP] NumVerts distribution: 3-7:%d  8-15:%d  16-31:%d  32-63:%d  64+:%d\n",
               nv3_7, nv8_15, nv16_31, nv32_63, nv64plus);
        // Analyze how many extra verts each failing polygon has
        int extraHist[8] = {}; // 0=all bad, 1=1 extra, 2=2 extra, ...
        for (auto& n : nodes) {
            if (n.numVertices < 3) continue;
            if (n.iVertPool < 0 || n.iVertPool + n.numVertices > (int)verts.size()) continue;
            int goodCount = 0;
            for (int v = 0; v < n.numVertices; v++) {
                int pi = verts[n.iVertPool + v].pVertex;
                if (pi < 0 || pi >= numPoints) break;
                float px = points[pi*3], py = points[pi*3+1], pz = points[pi*3+2];
                float dist = std::abs(px * n.planeX + py * n.planeY + pz * n.planeZ - n.planeW);
                if (dist > 1.0f) break;
                goodCount++;
            }
            int extra = n.numVertices - goodCount;
            if (extra > 0 && extra < 8) extraHist[extra]++;
            else if (extra >= 8) extraHist[7]++;
        }
        printf("[BSP] Extra vert distribution: ");
        for (int i = 1; i < 8; i++) printf("+%d=%d ", i, extraHist[i]);
        printf("\n");
        printf("[BSP] Planarity: %d OK, %d BAD (maxDist=%.2f)\n", planarOK, planarBad, maxPlaneDist);
        printf("[BSP] Area dist: <100=%d  100-10K=%d  10K-1M=%d  >1M=%d  total=%.0f\n",
               areaSmall, areaMed, areaLarge, areaHuge, totalArea);

        // Zone mask analysis: how many unique zones exist and avg bits set per node
        int totalBitsSet = 0;
        std::unordered_map<int, int> zonePopularity;
        for (auto& n : nodes) {
            if (n.numVertices < 3) continue;
            int bits = 0;
            for (int b = 0; b < 16; b++)
                for (int bi = 0; bi < 8; bi++)
                    if (n.zoneMask[b] & (1 << bi)) { bits++; zonePopularity[b*8+bi]++; }
            totalBitsSet += bits;
        }
        printf("[BSP] Zone masks: avg %.1f bits/node, %d unique zones used\n",
               valid3 > 0 ? (float)totalBitsSet / valid3 : 0.0f, (int)zonePopularity.size());
        // Show zones with most polygons
        std::vector<std::pair<int,int>> zSorted(zonePopularity.begin(), zonePopularity.end());
        std::sort(zSorted.begin(), zSorted.end(), [](auto&a, auto&b){return a.second > b.second;});
        printf("[BSP] Top 5 zones: ");
        for (int i = 0; i < 5 && i < (int)zSorted.size(); i++)
            printf("zone%d=%d ", zSorted[i].first, zSorted[i].second);
        printf("\n");
    }

    // ─── Post-FVerts: parse FLightMapIndex array from UModel ───
    // UModel layout after FVerts: SharedSides(TArray<int32>), NumZones(CI), Zone[](CI+16B each), 
    // Polys(CI obj ref), LightMap(TArray<FLightMapIndex>), LightBits(TArray), LightMapTextures(TArray)
    // Ref: docs/reverse-engineering/BioShock_Texture_Lightmap_Format.md §5
    std::unordered_map<int, BSPLightMapInfo> surfToLightMap; // iSurf → lightmap data
    {
        size_t lmPos = pos;
        size_t br2;

        // 1. SharedSides: CI count + count*4 bytes
        int numSharedSides = ReadCompactIndex(serialData + lmPos, endPos - lmPos, br2);
        lmPos += br2;
        if (numSharedSides > 0 && numSharedSides < 10000000)
            lmPos += (size_t)numSharedSides * 4;

        // 2. Zones: CI count, each = CI objref + 16B fixed data
        int numZones = ReadCompactIndex(serialData + lmPos, endPos - lmPos, br2);
        lmPos += br2;
        for (int z = 0; z < numZones && lmPos + 1 < endPos; z++) {
            ReadCompactIndex(serialData + lmPos, endPos - lmPos, br2);
            lmPos += br2 + 16;
        }

        // 3. Polys object reference (CI)
        ReadCompactIndex(serialData + lmPos, endPos - lmPos, br2);
        lmPos += br2;

        // 4. LightMap array: CI count, then FLightMapIndex entries
        int numLightMaps = ReadCompactIndex(serialData + lmPos, endPos - lmPos, br2);
        lmPos += br2;
        printf("[BSP-LM] Parsing %d FLightMapIndex entries at 0x%X\n", numLightMaps, (int)lmPos);

        int lmParsed = 0;
        if (numLightMaps > 0 && numLightMaps < 200000) {
            for (int li = 0; li < numLightMaps && lmPos + 8 < endPos; li++) {
                // ObjHeader: INT32 check (==4) + INT32 version (==2)
                int32_t check, ver;
                memcpy(&check, serialData + lmPos, 4);
                memcpy(&ver, serialData + lmPos + 4, 4);
                lmPos += 8;
                if (check != 4) {
                    printf("[BSP-LM] Bad ObjHeader check=%d at entry %d, aborting\n", check, li);
                    break;
                }

                BSPLightMapInfo info;
                if (lmPos + 76 > endPos) break; // iSurf(4) + SizeX(4) + SizeY(4) + Matrix(64)
                memcpy(&info.iSurf, serialData + lmPos, 4); lmPos += 4;
                memcpy(&info.sizeX, serialData + lmPos, 4); lmPos += 4;
                memcpy(&info.sizeY, serialData + lmPos, 4); lmPos += 4;
                // WorldToLightMap: 4x4 float matrix (64 bytes, row-major)
                memcpy(info.worldToLightMap, serialData + lmPos, 64); lmPos += 64;
                // Pan/UVBias: 3 floats (12 bytes) — not used for UV build
                lmPos += 12;

                // Lights TArray<FLightMapLight>
                int numLights = ReadCompactIndex(serialData + lmPos, endPos - lmPos, br2);
                lmPos += br2;
                if (numLights < 0 || numLights > 100) { break; }

                for (int lti = 0; lti < numLights && lmPos + 8 < endPos; lti++) {
                    // ObjHeader: check==4, ver==1
                    int32_t lcheck;
                    memcpy(&lcheck, serialData + lmPos, 4);
                    lmPos += 8; // skip check + version
                    if (lcheck != 4) break;

                    // 3× objref (CI each) — light actor references
                    for (int r = 0; r < 3; r++) {
                        ReadCompactIndex(serialData + lmPos, endPos - lmPos, br2);
                        lmPos += br2;
                    }
                    // iAtlas(4) + TileX(4) + TileY(4) = 12 bytes
                    if (lmPos + 12 > endPos) break;
                    int32_t iAtlas, tileX, tileY;
                    memcpy(&iAtlas, serialData + lmPos, 4); lmPos += 4;
                    memcpy(&tileX,  serialData + lmPos, 4); lmPos += 4;
                    memcpy(&tileY,  serialData + lmPos, 4); lmPos += 4;

                    // Use first light entry's atlas info (primary baked light)
                    if (lti == 0) {
                        info.iAtlas = iAtlas;
                        info.tileX = tileX;
                        info.tileY = tileY;
                    }
                }

                if (info.iSurf >= 0 && info.sizeX > 0 && info.sizeY > 0) {
                    surfToLightMap[info.iSurf] = info;
                    lmParsed++;
                }
            }
        }
        printf("[BSP-LM] Parsed %d/%d lightmap entries (%d surfs total)\n",
               lmParsed, numLightMaps, (int)surfs.size());
        if (lmParsed > 0) {
            // Diagnostic: atlas distribution
            std::unordered_map<int, int> atlasDist;
            for (auto& kv : surfToLightMap) atlasDist[kv.second.iAtlas]++;
            printf("[BSP-LM] Atlas distribution: ");
            for (auto& kv : atlasDist) printf("atlas%d=%d ", kv.first, kv.second);
            printf("\n");
        }
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

    // ─── UV Diagnostics: dump raw surf field values ───
    {
        printf("[BSP-UV] numVectors=%d, numPoints=%d\n", numVectors, numPoints);
        printf("[BSP-UV] First 10 surfs raw field values:\n");
        for (int si = 0; si < (int)surfs.size() && si < 10; si++) {
            auto& s = surfs[si];
            printf("  Surf[%d]: matRef=%d polyFlags=0x%X pBase=%d vNormal=%d vTextureU=%d vTextureV=%d LMScale=%.1f\n",
                   si, s.materialRef, (unsigned)s.polyFlags, s.pBase, s.vNormal, s.vTextureU, s.vTextureV, s.lightMapScale);
            if (s.vTextureU >= 0 && s.vTextureU < numVectors &&
                s.vTextureV >= 0 && s.vTextureV < numVectors) {
                float ux = vectors[s.vTextureU*3], uy = vectors[s.vTextureU*3+1], uz = vectors[s.vTextureU*3+2];
                float vx = vectors[s.vTextureV*3], vy = vectors[s.vTextureV*3+1], vz = vectors[s.vTextureV*3+2];
                float uMag = std::sqrt(ux*ux + uy*uy + uz*uz);
                float vMag = std::sqrt(vx*vx + vy*vy + vz*vz);
                printf("    texU=(%.6f,%.6f,%.6f |%.6f|) texV=(%.6f,%.6f,%.6f |%.6f|)\n",
                       ux, uy, uz, uMag, vx, vy, vz, vMag);
            }
        }
        // iLightMap field analysis (lightmap atlas index per surface)
        int lm0 = 0;
        std::unordered_map<int, int> lmIdxDist;
        for (auto& s : surfs) {
            if (s.iLightMap == 0) { lm0++; continue; }
            lmIdxDist[s.iLightMap]++;
        }
        printf("[BSP-LM] iLightMap: %d surfaces with no LM, %d unique LM indices used\n",
               lm0, (int)lmIdxDist.size());
        printf("[BSP-LM] LM index distribution: ");
        for (auto& kv : lmIdxDist) printf("lm%d=%d ", kv.first, kv.second);
        printf("\n");

        // LightMapScale distribution
        int lmZero = 0, lmSmall = 0, lmMed = 0, lmLarge = 0, lmHuge = 0;
        float lmMin = 1e9f, lmMax = 0.0f;
        for (auto& s : surfs) {
            if (s.lightMapScale <= 0.0f) { lmZero++; continue; }
            if (s.lightMapScale < lmMin) lmMin = s.lightMapScale;
            if (s.lightMapScale > lmMax) lmMax = s.lightMapScale;
            if (s.lightMapScale < 4.0f) lmSmall++;
            else if (s.lightMapScale < 16.0f) lmMed++;
            else if (s.lightMapScale < 64.0f) lmLarge++;
            else lmHuge++;
        }
        printf("[BSP-LM] LightMapScale: zero=%d <4=%d 4-16=%d 16-64=%d 64+=%d range=[%.1f..%.1f]\n",
               lmZero, lmSmall, lmMed, lmLarge, lmHuge, lmMin, lmMax);
    }

    // ─── 9. Triangulate BSP nodes, grouped by material ───────────────────────
    // Each unique material becomes a separate ParsedMesh with textureName set.

    struct MaterialGroup {
        std::string matName;
        std::vector<MeshVertex> vertices;
        std::vector<MeshTriangle> triangles;
        uint8_t zoneMask[16] = {}; // union of all nodes' ZoneMasks
        float lightMapScaleSum = 0.0f;
        int lightMapScaleCount = 0;
        uint8_t zoneIndex = 0;
        int32_t lightMapIndex = 0; // dominant lightmap atlas index for this group
        std::string lightMapName;  // resolved cache key (e.g. "LM_Texture49")
    };
    std::unordered_map<int64_t, MaterialGroup> groups; // key: materialRef * 65536 + iZone * 256 + atlas

    int totalVerts = 0, totalTris = 0;

    int skippedBadVert = 0;

    for (auto& node : nodes) {
        if (node.numVertices < 3) continue;
        if (node.iVertPool < 0) continue;
        if (node.iVertPool + node.numVertices > (int)verts.size()) continue;

        // Pre-check: validate all vertex indices
        {
            bool badPoly = false;
            for (int v = 0; v < node.numVertices; v++) {
                int pi = verts[node.iVertPool + v].pVertex;
                if (pi < 0 || pi >= numPoints) { badPoly = true; break; }
            }
            if (badPoly) { skippedBadVert++; continue; }
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

        // Look up per-surface lightmap data from the parsed FLightMapIndex array
        int lmAtlas = -1;
        const BSPLightMapInfo* lmInfo = nullptr;
        if (surfIdx >= 0) {
            auto lmIt = surfToLightMap.find(surfIdx);
            if (lmIt != surfToLightMap.end()) {
                lmInfo = &lmIt->second;
                lmAtlas = lmInfo->iAtlas;
            }
        }

        // Group by (material, zone, atlas) so each chunk maps to one lightmap texture
        int atlasKey = (lmAtlas >= 0) ? lmAtlas : 255;
        int64_t groupKey = (int64_t)matRef * 65536 + (int64_t)node.iZone * 256 + atlasKey;
        if (groups.find(groupKey) == groups.end()) {
            MaterialGroup g;
            g.matName = ResolveMaterialName(matRef, exports, importNames);
            groups[groupKey] = std::move(g);
        }
        auto& group = groups[groupKey];
        group.zoneIndex = node.iZone;

        // Accumulate lightmap scale and index from surface
        if (surf && surf->lightMapScale > 0.0f && surf->lightMapScale < 1e6f) {
            group.lightMapScaleSum += surf->lightMapScale;
            group.lightMapScaleCount++;
        }
        // Resolve lightmap atlas texture name from FLightMapIndex
        if (lmInfo && lmInfo->iAtlas >= 0 && group.lightMapIndex == 0) {
            group.lightMapIndex = lmInfo->iAtlas + 1; // 1-based for compatibility
            if (lightMapNames && lmInfo->iAtlas < (int)lightMapNames->size()) {
                std::string bsmName = (*lightMapNames)[lmInfo->iAtlas];
                std::string catName;
                for (char c : bsmName) { if (c != '_') catName += c; }
                group.lightMapName = "LM_" + catName;
            }
        }

        // Accumulate zone visibility: OR node's mask into group's mask
        for (int zi = 0; zi < 16; zi++) group.zoneMask[zi] |= node.zoneMask[zi];

        // Check if we need to split this group (uint16 index limit)
        if ((int)group.vertices.size() + node.numVertices > 60000) {
            // Flush current group to results
            if (!group.triangles.empty()) {
                ParsedMesh chunk;
                chunk.name = "__BSP__";
                chunk.textureName = StripShaderSuffix(group.matName);
                chunk.lightMapScale = group.lightMapScaleCount > 0 ? group.lightMapScaleSum / group.lightMapScaleCount : 0.0f;
                chunk.lightMapIndex = group.lightMapIndex;
                chunk.lightMapName = group.lightMapName;
                chunk.zoneIndex = group.zoneIndex;
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
            group.vertices.clear();
            group.triangles.clear();
        }

        // Compute normal (from node plane)
        float nx = node.planeX;
        float ny = node.planeY;
        float nz = node.planeZ;
        float nlen = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (nlen > 0.001f) { nx /= nlen; ny /= nlen; nz /= nlen; }

        // Get texture vectors, base point, and pan offsets from surf (for UV computation)
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

            MeshVertex mv = {};
            mv.x = points[pointIdx * 3 + 0];
            mv.y = points[pointIdx * 3 + 1];
            mv.z = points[pointIdx * 3 + 2];
            mv.nx = nx;
            mv.ny = ny;
            mv.nz = nz;

            // Compute diffuse UV from texture vectors: dot(vertex - base, texAxis)
            if (hasTexVecs) {
                float dx = mv.x - baseX;
                float dy = mv.y - baseY;
                float dz = mv.z - baseZ;
                mv.u = dx * texUx + dy * texUy + dz * texUz;
                mv.v = dx * texVx + dy * texVy + dz * texVz;
                // Tangent from texture U axis (normalized)
                float tlen = std::sqrt(texUx*texUx + texUy*texUy + texUz*texUz);
                if (tlen > 0.001f) { mv.tx = texUx/tlen; mv.ty = texUy/tlen; mv.tz = texUz/tlen; }
                else { mv.tx = 1; mv.ty = 0; mv.tz = 0; }
                mv.tw = 1.0f;
            } else {
                mv.u = 0;
                mv.v = 0;
                mv.tx = 1; mv.ty = 0; mv.tz = 0; mv.tw = 1;
            }

            // Lightmap UVs: use WorldToLightMap matrix from FLightMapIndex
            // Formula (§6): (U',V') = WorldToLightMap × worldPos
            //   U = U' * (SizeX/1024) + (TileX + 0.5) / 1024
            //   V = V' * (SizeY/1024) + (TileY + 0.5) / 1024
            if (lmInfo && lmInfo->iAtlas >= 0) {
                const float* m = lmInfo->worldToLightMap;
                // Row-major 4x4 matrix multiply: result = M * [x,y,z,1]
                float lmU = m[0]*mv.x + m[1]*mv.y + m[2]*mv.z + m[3];
                float lmV = m[4]*mv.x + m[5]*mv.y + m[6]*mv.z + m[7];
                // Pack into atlas: scale by tile size, offset by tile position
                mv.u2 = lmU * (lmInfo->sizeX / 1024.0f) + (lmInfo->tileX + 0.5f) / 1024.0f;
                mv.v2 = lmV * (lmInfo->sizeY / 1024.0f) + (lmInfo->tileY + 0.5f) / 1024.0f;
            } else {
                mv.u2 = 0;
                mv.v2 = 0;
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
        bool isWaterSurf = strippedName.find("Water") != std::string::npos ||
                           strippedName.find("water") != std::string::npos ||
                           strippedName.find("CalmWater") != std::string::npos ||
                           strippedName.find("Calmwater") != std::string::npos;
        if (strippedName.find("FakeBackdrop") != std::string::npos ||
            strippedName.find("ZoningOnly") != std::string::npos ||
            strippedName.find("PortalBrush") != std::string::npos ||
            strippedName.find("AntiPortalBrush") != std::string::npos ||
            strippedName.find("Glass") != std::string::npos ||
            strippedName.find("glass") != std::string::npos ||
            strippedName.find("Window") != std::string::npos ||
            strippedName.find("window") != std::string::npos ||
            strippedName.find("Fog") != std::string::npos ||
            strippedName.find("Sky") != std::string::npos ||
            strippedName.find("Invisible") != std::string::npos ||
            strippedName.find("invisible") != std::string::npos ||
            strippedName.find("Trigger") != std::string::npos ||
            strippedName.find("Volume") != std::string::npos ||
            strippedName.find("Utility") != std::string::npos ||
            strippedName == "DefaultTexture" ||
            strippedName == "BlackTexture" ||
            strippedName == "Black")
            continue;

        ParsedMesh chunk;
        chunk.name = "__BSP__";
        chunk.textureName = strippedName;
        chunk.isWater = isWaterSurf;
        chunk.lightMapScale = group.lightMapScaleCount > 0 ? group.lightMapScaleSum / group.lightMapScaleCount : 0.0f;
        chunk.lightMapIndex = group.lightMapIndex;
        chunk.lightMapName = group.lightMapName;
        chunk.zoneIndex = group.zoneIndex;
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

    printf("[BSP] Triangulated: %d vertices, %d triangles in %d material chunks (skipped: %d bad-vert)\n",
           totalVerts, totalTris, (int)results.size(), skippedBadVert);

    return results;
}
