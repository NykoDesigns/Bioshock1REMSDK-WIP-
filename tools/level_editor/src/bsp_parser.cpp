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

// FBspNode: fields needed for triangulation + BSP tree traversal + zone filtering
struct BSPNode {
    float planeX, planeY, planeZ, planeW;
    uint8_t zoneMask[16]; // 128-bit zone visibility mask at +16
    int32_t iVertPool;    // +32
    int32_t iSurf;        // +36
    int32_t iBack;        // +40 (back child, -1 = leaf)
    int32_t iFront;       // +44 (front child, -1 = leaf)
    uint8_t numVertices;  // +88 (INT32)
    uint8_t iZone;        // +77 (BYTE, zone index - 100% ZoneMask correlation)
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
    // Diagnostic: print first few vectors and points
    for (int vi = 0; vi < 6 && vi < numVectors; vi++)
        printf("[BSP]   Vec[%d] = (%.6f, %.6f, %.6f)\n", vi,
               vectors[vi*3+0], vectors[vi*3+1], vectors[vi*3+2]);
    for (int pi = 0; pi < 3 && pi < numPoints; pi++)
        printf("[BSP]   Pt[%d] = (%.1f, %.1f, %.1f)\n", pi,
               points[pi*3+0], points[pi*3+1], points[pi*3+2]);

    // ─── 6. Nodes (100B each) ───
    printf("[BSP] Pre-nodes pos: 0x%X\n", (int)pos);
    // Peek at CompactIndex and first node bytes
    {
        size_t br;
        int peekCount = ReadCompactIndex(serialData + pos, endPos - pos, br);
        printf("[BSP] Node count CI: %d (CI size: %d bytes)\n", peekCount, (int)br);
        size_t dataStart = pos + br;
        printf("[BSP] Node[0] raw (100 bytes):\n");
        for (int b = 0; b < 100 && dataStart + b + 4 <= endPos; b += 4) {
            float fv; int32_t iv;
            memcpy(&fv, serialData + dataStart + b, 4);
            memcpy(&iv, serialData + dataStart + b, 4);
            printf("  +%2d: float=%12.3f  int=%10d  hex=0x%08X\n", b, fv, iv, (unsigned)iv);
        }
        // Vengeance FBspNode layout (100 bytes) based on UE2 source + 8-byte ZoneMask expansion:
        // +0:FPlane(16) +16:ZoneMask(16) +32:iVertPool(4) +36:iSurf(4)
        // +40:iBack(4) +44:iFront(4) +48:iPlane(4)
        // +52:iCollBound(4) +56:iRendBound(4) +60:iZone[2](2B) +62:NumVert(1B) +63:NodeFlags(1B)
        // +64:iLeaf[2](8B) +72..+87:unknown(16B) +88:NumVerticesINT(4) +92:NodeFlagsINT(4) +96:iLeafINT(4)
        printf("[BSP] Vengeance FBspNode layout probe (UE2+8 shift theory):\n");
        int shown = 0;
        for (int ni = 0; ni < peekCount && shown < 10; ni++) {
            size_t noff = dataStart + (size_t)ni * 100;
            int32_t nv; memcpy(&nv, serialData + noff + 88, 4);
            int32_t iBack2, iFront2;
            memcpy(&iBack2, serialData + noff + 40, 4);
            memcpy(&iFront2, serialData + noff + 44, 4);
            if (nv < 3 && iBack2 == -1 && iFront2 == -1) continue;
            int32_t iPlane2, iCollB, iRendB;
            memcpy(&iPlane2, serialData + noff + 48, 4);
            memcpy(&iCollB, serialData + noff + 52, 4);
            memcpy(&iRendB, serialData + noff + 56, 4);
            uint8_t zoneBack = serialData[noff + 60];
            uint8_t zoneFront = serialData[noff + 61];
            uint8_t numVertByte = serialData[noff + 62];
            uint8_t flagsByte = serialData[noff + 63];
            int32_t iLeaf0, iLeaf1;
            memcpy(&iLeaf0, serialData + noff + 64, 4);
            memcpy(&iLeaf1, serialData + noff + 68, 4);
            uint8_t realZone0 = serialData[noff + 77]; // confirmed iZone[0]
            uint8_t realZone1 = serialData[noff + 78]; // probable iZone[1]
            printf("  node[%d] nv=%d iB=%d iF=%d iPlane=%d | iZone=[%d,%d](+77,+78)\n",
                   ni, nv, iBack2, iFront2, iPlane2, realZone0, realZone1);
            // Print +52..+76 as floats to check if bounding box
            float f52,f56,f60,f64,f68,f72,f76,f80,f84;
            memcpy(&f52, serialData+noff+52, 4);
            memcpy(&f56, serialData+noff+56, 4);
            memcpy(&f60, serialData+noff+60, 4);
            memcpy(&f64, serialData+noff+64, 4);
            memcpy(&f68, serialData+noff+68, 4);
            memcpy(&f72, serialData+noff+72, 4);
            memcpy(&f76, serialData+noff+76, 4);
            memcpy(&f80, serialData+noff+80, 4);
            memcpy(&f84, serialData+noff+84, 4);
            printf("    floats: +52=%.1f +56=%.1f +60=%.1f +64=%.1f +68=%.1f +72=%.1f\n",
                   f52, f56, f60, f64, f68, f72);
            printf("    floats: +76=%.1f +80=%.1f +84=%.1f | INT: +88=%d +92=%d +96=%d\n",
                   f76, f80, f84,
                   *(int32_t*)(serialData+noff+88), *(int32_t*)(serialData+noff+92),
                   *(int32_t*)(serialData+noff+96));
            shown++;
        }
        // Explicitly check +77 and +78 for iZone correlation
        {
            int match77=0,miss77=0,match78=0,miss78=0,total=0;
            for (int ni = 0; ni < peekCount; ni++) {
                size_t noff = dataStart + (size_t)ni * 100;
                int32_t nv; memcpy(&nv, serialData + noff + 88, 4);
                if (nv < 3) continue;
                total++;
                uint8_t z0 = serialData[noff + 77], z1 = serialData[noff + 78];
                if (z0 < 128 && (serialData[noff+16+(z0/8)] & (1<<(z0%8)))) match77++; else miss77++;
                if (z1 < 128 && (serialData[noff+16+(z1/8)] & (1<<(z1%8)))) match78++; else miss78++;
            }
            printf("[BSP] iZone correlation: +77=%d/%d (%.0f%%), +78=%d/%d (%.0f%%)\n",
                   match77, total, 100.0f*match77/total, match78, total, 100.0f*match78/total);
        }
        // Try ALL possible byte offsets for iZone by checking correlation with ZoneMask bits
        printf("[BSP] Scanning all byte offsets 52..99 for iZone correlation with ZoneMask:\n");
        for (int testOff = 52; testOff < 100; testOff++) {
            int matchCount = 0, testTotal = 0;
            for (int ni = 0; ni < peekCount; ni++) {
                size_t noff = dataStart + (size_t)ni * 100;
                int32_t nv; memcpy(&nv, serialData + noff + 88, 4);
                if (nv < 3) continue;
                uint8_t val = serialData[noff + testOff];
                if (val >= 128) continue;
                testTotal++;
                int byteIdx = val / 8;
                int bitIdx = val % 8;
                if (serialData[noff + 16 + byteIdx] & (1 << bitIdx)) matchCount++;
            }
            if (testTotal > 50 && matchCount > testTotal / 2) {
                printf("  +%d: %d/%d match (%.0f%%)\n", testOff, matchCount, testTotal,
                       100.0f * matchCount / testTotal);
            }
        }
        // Also try INT32 offsets
        for (int testOff = 52; testOff <= 96; testOff += 4) {
            int matchCount = 0, testTotal = 0;
            int maxVal = 0;
            for (int ni = 0; ni < peekCount; ni++) {
                size_t noff = dataStart + (size_t)ni * 100;
                int32_t nv; memcpy(&nv, serialData + noff + 88, 4);
                if (nv < 3) continue;
                int32_t val; memcpy(&val, serialData + noff + testOff, 4);
                if (val < 0 || val >= 128) continue;
                if (val > maxVal) maxVal = val;
                testTotal++;
                int byteIdx = val / 8;
                int bitIdx = val % 8;
                if (serialData[noff + 16 + byteIdx] & (1 << bitIdx)) matchCount++;
            }
            if (testTotal > 50) {
                printf("  INT32 +%d: %d/%d match (%.0f%%) maxVal=%d\n", testOff, matchCount, testTotal,
                       100.0f * matchCount / testTotal, maxVal);
            }
        }
    }

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
                printf("[BSP] FVerts: %d (direct read at 0x%X)\n", numVerts, (int)(pos - br2));
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
        for (auto& n : nodes) {
            if (n.numVertices < 3) continue;
            if (n.iVertPool < 0 || n.iVertPool + n.numVertices > (int)verts.size()) continue;
            valid3++;
        }
        printf("[BSP] Polygon nodes: %d (of %d), FVerts: %d\n", valid3, (int)nodes.size(), (int)verts.size());
    }

    // Dump raw FBspNode fields for structure analysis
    {
        int numN = (int)nodes.size();
        size_t nodeArrayStart = pos - (size_t)numN * 100;
        printf("[BSP] FBspNode raw dump (numNodes=%d):\n", numN);
        for (int ni = 0; ni < 3 && ni < numN; ni++) {
            size_t noff = nodeArrayStart + (size_t)ni * 100;
            printf("  Node[%d] INT32 at each offset:\n", ni);
            for (int off = 0; off < 100; off += 4) {
                int32_t val; memcpy(&val, serialData + noff + off, 4);
                bool isNodeIdx = (val == -1 || (val >= 0 && val < numN));
                bool isSurfIdx = (val >= 0 && val < (int)surfs.size());
                bool isVertPool = (val >= 0 && val < (int)verts.size());
                printf("    +%2d: %10d%s%s%s\n", off, val,
                       isNodeIdx ? " [nodeIdx]" : "",
                       isSurfIdx ? " [surfIdx]" : "",
                       isVertPool ? " [vertPool]" : "");
            }
        }
    }

    // Winding order diagnostic: check cross product of first triangle vs plane normal
    {
        int cwCount = 0, ccwCount = 0;
        for (int d = 0; d < (int)nodes.size(); d++) {
            auto& node = nodes[d];
            if (node.numVertices < 3) continue;
            if (node.iVertPool < 0 || node.iVertPool + node.numVertices > (int)verts.size()) continue;
            int v0i = verts[node.iVertPool].pVertex;
            int v1i = verts[node.iVertPool + 1].pVertex;
            int v2i = verts[node.iVertPool + 2].pVertex;
            if (v0i < 0 || v0i >= numPoints || v1i < 0 || v1i >= numPoints || v2i < 0 || v2i >= numPoints) continue;
            float e1x = points[v1i*3]-points[v0i*3], e1y = points[v1i*3+1]-points[v0i*3+1], e1z = points[v1i*3+2]-points[v0i*3+2];
            float e2x = points[v2i*3]-points[v0i*3], e2y = points[v2i*3+1]-points[v0i*3+1], e2z = points[v2i*3+2]-points[v0i*3+2];
            float cx = e1y*e2z - e1z*e2y, cy = e1z*e2x - e1x*e2z, cz = e1x*e2y - e1y*e2x;
            float dot = cx * node.planeX + cy * node.planeY + cz * node.planeZ;
            if (dot > 0) ccwCount++; else if (dot < 0) cwCount++;
        }
        printf("[BSP] Winding: %d CCW, %d CW (use %s)\n", ccwCount, cwCount, ccwCount > cwCount ? "GL_CCW" : "GL_CW");
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
    std::unordered_map<int, MaterialGroup> groups; // key: materialRef

    int totalVerts = 0, totalTris = 0;

    for (auto& node : nodes) {
        if (node.numVertices < 3) continue;
        if (node.iVertPool < 0) continue;
        if (node.iVertPool + node.numVertices > (int)verts.size()) continue;

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

        // Initialize group if new
        if (groups.find(matRef) == groups.end()) {
            MaterialGroup g;
            g.matName = ResolveMaterialName(matRef, exports, importNames);
            groups[matRef] = std::move(g);
        }
        auto& group = groups[matRef];

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
    for (auto& [matRef, group] : groups) {
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

    printf("[BSP] Triangulated: %d vertices, %d triangles in %d material chunks\n",
           totalVerts, totalTris, (int)results.size());

    // Zone mask diversity diagnostic
    {
        int allBitsSet = 0, partial = 0, empty = 0;
        for (auto& r : results) {
            bool allOnes = true, allZeros = true;
            for (int zi = 0; zi < 16; zi++) {
                if (r.zoneMask[zi] != 0xFF) allOnes = false;
                if (r.zoneMask[zi] != 0x00) allZeros = false;
            }
            if (allOnes) allBitsSet++;
            else if (allZeros) empty++;
            else partial++;
        }
        printf("[BSP] Zone masks: %d all-visible, %d partial, %d empty\n", allBitsSet, partial, empty);
        // Show first 5 partial masks
        int shown = 0;
        for (int ri = 0; ri < (int)results.size() && shown < 5; ri++) {
            bool allOnes = true;
            for (int zi = 0; zi < 16; zi++) if (results[ri].zoneMask[zi] != 0xFF) { allOnes = false; break; }
            if (!allOnes) {
                printf("[BSP]   chunk[%d] '%s' mask: %02X%02X%02X%02X %02X%02X%02X%02X\n", ri,
                       results[ri].textureName.c_str(),
                       results[ri].zoneMask[3], results[ri].zoneMask[2], results[ri].zoneMask[1], results[ri].zoneMask[0],
                       results[ri].zoneMask[7], results[ri].zoneMask[6], results[ri].zoneMask[5], results[ri].zoneMask[4]);
                shown++;
            }
        }
    }

    // Diagnostic: show first 10 BSP chunk texture names and UV ranges
    for (int d = 0; d < 10 && d < (int)results.size(); d++) {
        float uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
        for (auto& v : results[d].vertices) {
            if (v.u < uMin) uMin = v.u; if (v.u > uMax) uMax = v.u;
            if (v.v < vMin) vMin = v.v; if (v.v > vMax) vMax = v.v;
        }
        printf("[BSP] chunk[%d] texName='%s' uv=[%.3f..%.3f, %.3f..%.3f] (%d tris)\n", d,
               results[d].textureName.c_str(), uMin, uMax, vMin, vMax,
               (int)results[d].triangles.size());
    }

    return results;
}
