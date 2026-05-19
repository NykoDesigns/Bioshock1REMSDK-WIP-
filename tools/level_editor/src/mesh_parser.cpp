#include "mesh_parser.h"
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

static float ReadFloat(const uint8_t* d, size_t& pos) {
    float v; memcpy(&v, d + pos, 4); pos += 4; return v;
}
static int32_t ReadInt32(const uint8_t* d, size_t& pos) {
    int32_t v; memcpy(&v, d + pos, 4); pos += 4; return v;
}
static uint16_t ReadUint16(const uint8_t* d, size_t& pos) {
    uint16_t v; memcpy(&v, d + pos, 2); pos += 2; return v;
}
static uint32_t ReadUint32(const uint8_t* d, size_t& pos) {
    uint32_t v; memcpy(&v, d + pos, 4); pos += 4; return v;
}

// Skip UE property tags until "None" sentinel
static size_t SkipProperties(const uint8_t* data, size_t pos, size_t endPos,
                              const std::vector<std::string>& names)
{
    while (pos < endPos - 5) {
        size_t br;
        int nameIdx = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
        pos += 4; // name number
        if (nameIdx == 0) return pos; // "None" sentinel — done

        if (nameIdx < 0 || nameIdx >= (int)names.size()) return pos;
        if (pos >= endPos) return pos;

        uint8_t info = data[pos++];
        int type = info & 0x0F;
        int sizeBits = (info >> 4) & 0x07;
        int arrayFlag = (info >> 7) & 1;

        // Struct name ref
        if (type == 10) { // PT_Struct
            ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
            pos += 4;
        }

        // Decode size
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

        // Bool has no payload, otherwise skip array index + value
        if (type == 3) { // Bool
            // no payload
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

// ─── StaticMesh Format (BioShock UE2.5 Vengeance) ───────────────────────────
//
// From bio4554's validated C.4 spec (100% across all maps):
//
// Object serial data layout:
//   8B   Vengeance per-object header (04 00 00 00, 03 00 00 00)
//   ...  Tagged properties (skip until None sentinel)
//   25B  FBox BoundingBox (from UPrimitive base)
//   16B  FSphere BoundingSphere (from UPrimitive base)
//   8B   Vengeance class header (check=4, sv=8)
//   CI   NumSections
//   14B × NumSections (section data)
//   25B  FBox BoundingBox (repeated by Vengeance)
//   CI   NumVerts
//   24B × NumVerts:
//        12B FVector Position
//        4B  FPackedNormal Tangent (DWORD: signed bytes X,Y,Z,W)
//        4B  FPackedNormal Binormal
//        4B  FPackedNormal Normal (the actual surface normal)
//   CI   NumUVStreams
//   Per UV stream:
//        CI NumUVs
//        8B × NumUVs (float U, float V)
//        4B INT32 CoordinateIndex
//   CI   NumIndices
//   2B × NumIndices (WORD indices)
//   4B   INT32 IndexBuffer.Revision


static ParsedMesh ParseStaticMeshSpec(const uint8_t* serialData, int serialSize,
                                      const std::vector<std::string>& names)
{
    ParsedMesh mesh;
    mesh.name = "";
    mesh.valid = false;

    if (serialSize < 100) return mesh;

    size_t endPos = (size_t)serialSize;
    size_t pos = 0;

    // ─── 1. Skip 8-byte Vengeance per-object header ───
    if (pos + 8 > endPos) return mesh;
    int32_t objCheck = ReadInt32(serialData, pos);
    ReadInt32(serialData, pos); // sub_ver
    if (objCheck != 4) return mesh; // Invalid Vengeance header

    // ─── 2. Skip tagged properties ───
    pos = SkipProperties(serialData, pos, endPos, names);
    if (pos >= endPos) return mesh;

    // ─── 3. UPrimitive base: FBox (25B) + FSphere (16B) ───
    if (pos + 41 > endPos) return mesh;
    // FBox: min(12) + max(12) + isValid(1) = 25
    memcpy(&mesh.boundsMin.x, serialData + pos, 4);
    memcpy(&mesh.boundsMin.y, serialData + pos + 4, 4);
    memcpy(&mesh.boundsMin.z, serialData + pos + 8, 4);
    memcpy(&mesh.boundsMax.x, serialData + pos + 12, 4);
    memcpy(&mesh.boundsMax.y, serialData + pos + 16, 4);
    memcpy(&mesh.boundsMax.z, serialData + pos + 20, 4);
    pos += 25; // FBox
    pos += 16; // FSphere (center + radius, skip)

    // ─── 4. Vengeance class header for StaticMesh (check=4, sv=8) ───
    if (pos + 8 > endPos) return mesh;
    int32_t smCheck = ReadInt32(serialData, pos);
    ReadInt32(serialData, pos); // sub_ver (8 for StaticMesh)
    if (smCheck != 4) return mesh; // Not a valid StaticMesh class header

    // ─── 5. Sections: CI NumSections + 14B each ───
    if (pos + 1 > endPos) return mesh;
    size_t br;
    int numSections = ReadCompactIndex(serialData + pos, endPos - pos, br); pos += br;
    if (numSections < 0 || numSections > 1024) return mesh;
    // Skip section data (14 bytes each)
    size_t sectionBytes = (size_t)numSections * 14;
    if (pos + sectionBytes > endPos) return mesh;
    pos += sectionBytes;

    // ─── 6. Second FBox (25B, repeated by Vengeance) ───
    if (pos + 25 > endPos) return mesh;
    pos += 25;

    // ─── 7. Vertices: CI NumVerts + 48B each ───
    // BioShock Remastered uses full FVector normals (not packed):
    //   12B FVector Position
    //   12B FVector Normal
    //   12B FVector TangentX
    //   12B FVector TangentZ
    // Total: 48 bytes per vertex
    static const int VERTEX_STRIDE = 48;
    if (pos + 1 > endPos) return mesh;
    int numVerts = ReadCompactIndex(serialData + pos, endPos - pos, br); pos += br;
    if (numVerts < 3 || numVerts > 5000000) return mesh;
    size_t vertBytes = (size_t)numVerts * VERTEX_STRIDE;
    if (pos + vertBytes > endPos) return mesh;

    mesh.vertices.resize(numVerts);
    for (int i = 0; i < numVerts; i++) {
        size_t voff = pos + (size_t)i * VERTEX_STRIDE;
        // Position at offset 0 (12 bytes)
        memcpy(&mesh.vertices[i].x, serialData + voff, 4);
        memcpy(&mesh.vertices[i].y, serialData + voff + 4, 4);
        memcpy(&mesh.vertices[i].z, serialData + voff + 8, 4);
        // Normal at offset 12 (12 bytes, full FVector)
        memcpy(&mesh.vertices[i].nx, serialData + voff + 12, 4);
        memcpy(&mesh.vertices[i].ny, serialData + voff + 16, 4);
        memcpy(&mesh.vertices[i].nz, serialData + voff + 20, 4);
        // UVs will be filled from UV stream below
        mesh.vertices[i].u = 0;
        mesh.vertices[i].v = 0;
    }
    pos += vertBytes;

    // ─── 8. UV Streams: CI NumUVStreams, each: CI NumUVs + 8B*N + INT32 ───
    if (pos + 1 > endPos) return mesh;
    int numUVStreams = ReadCompactIndex(serialData + pos, endPos - pos, br); pos += br;
    if (numUVStreams < 0 || numUVStreams > 16) return mesh;

    for (int s = 0; s < numUVStreams; s++) {
        if (pos + 1 > endPos) return mesh;
        int numUVs = ReadCompactIndex(serialData + pos, endPos - pos, br); pos += br;
        if (numUVs < 0 || numUVs > 5000000) return mesh;
        size_t uvBytes = (size_t)numUVs * 8;
        if (pos + uvBytes + 4 > endPos) return mesh;

        // Only use first UV stream for texture mapping
        if (s == 0 && numUVs == numVerts) {
            for (int i = 0; i < numUVs; i++) {
                memcpy(&mesh.vertices[i].u, serialData + pos + (size_t)i * 8, 4);
                memcpy(&mesh.vertices[i].v, serialData + pos + (size_t)i * 8 + 4, 4);
            }
        }
        pos += uvBytes;
        pos += 4; // INT32 CoordinateIndex
    }

    // ─── 9. Indices: CI NumIndices + 2B each + INT32 Revision ───
    if (pos + 1 > endPos) return mesh;
    int numIndices = ReadCompactIndex(serialData + pos, endPos - pos, br); pos += br;
    if (numIndices < 3 || numIndices > 10000000) return mesh;
    size_t idxBytes = (size_t)numIndices * 2;
    if (pos + idxBytes > endPos) return mesh;

    int numTris = numIndices / 3;
    mesh.triangles.resize(numTris);
    for (int i = 0; i < numTris; i++) {
        size_t ioff = pos + (size_t)i * 6;
        memcpy(&mesh.triangles[i].i0, serialData + ioff, 2);
        memcpy(&mesh.triangles[i].i1, serialData + ioff + 2, 2);
        memcpy(&mesh.triangles[i].i2, serialData + ioff + 4, 2);
        // Validate indices
        if (mesh.triangles[i].i0 >= (uint16_t)numVerts ||
            mesh.triangles[i].i1 >= (uint16_t)numVerts ||
            mesh.triangles[i].i2 >= (uint16_t)numVerts) {
            mesh.triangles.clear();
            mesh.vertices.clear();
            return mesh;
        }
    }

    mesh.valid = true;
    return mesh;
}

// ─── Fallback heuristic parser (scans for bounding box + vertex buffer) ─────
// Used when spec-based parser fails (e.g. due to SkipProperties issues)
static ParsedMesh ParseStaticMeshHeuristic(const uint8_t* serialData, int serialSize)
{
    ParsedMesh mesh;
    mesh.valid = false;
    if (serialSize < 200) return mesh;

    static const int VERTEX_STRIDE = 48;
    size_t endPos = (size_t)serialSize;

    // Scan for the StaticMesh class header pattern: INT32(4) + INT32(8)
    // followed by a valid section count and then a bounding box
    for (size_t scan = 8; scan + 100 < endPos; scan++) {
        int32_t v1, v2;
        memcpy(&v1, serialData + scan, 4);
        memcpy(&v2, serialData + scan + 4, 4);
        if (v1 != 4 || v2 != 8) continue;

        // Try reading sections at scan+8
        size_t tpos = scan + 8;
        size_t br2;
        int nSec = ReadCompactIndex(serialData + tpos, endPos - tpos, br2);
        tpos += br2;
        if (nSec < 0 || nSec > 64) continue;
        tpos += (size_t)nSec * 14;
        if (tpos + 25 > endPos) continue;

        // Read FBox and validate
        float bmin[3], bmax[3];
        memcpy(bmin, serialData + tpos, 12);
        memcpy(bmax, serialData + tpos + 12, 12);
        uint8_t isValid = serialData[tpos + 24];
        if (isValid != 1) continue;
        bool boxOk = true;
        for (int i = 0; i < 3; i++) {
            if (bmin[i] != bmin[i] || bmax[i] != bmax[i]) { boxOk = false; break; }
            if (bmin[i] < -1e7f || bmin[i] > 1e7f) { boxOk = false; break; }
            if (bmax[i] < -1e7f || bmax[i] > 1e7f) { boxOk = false; break; }
            if (bmin[i] > bmax[i]) { boxOk = false; break; }
        }
        if (!boxOk) continue;

        tpos += 25; // skip FBox

        // Read numVerts
        if (tpos + 1 > endPos) continue;
        int numVerts = ReadCompactIndex(serialData + tpos, endPos - tpos, br2);
        tpos += br2;
        if (numVerts < 3 || numVerts > 500000) continue;
        size_t vertBytes = (size_t)numVerts * VERTEX_STRIDE;
        if (tpos + vertBytes > endPos) continue;

        // Validate first few vertex positions are within bounding box (with margin)
        float margin = 100.0f;
        bool vertsOk = true;
        for (int i = 0; i < 3 && i < numVerts; i++) {
            float vx, vy, vz;
            memcpy(&vx, serialData + tpos + (size_t)i * VERTEX_STRIDE, 4);
            memcpy(&vy, serialData + tpos + (size_t)i * VERTEX_STRIDE + 4, 4);
            memcpy(&vz, serialData + tpos + (size_t)i * VERTEX_STRIDE + 8, 4);
            if (vx < bmin[0] - margin || vx > bmax[0] + margin ||
                vy < bmin[1] - margin || vy > bmax[1] + margin ||
                vz < bmin[2] - margin || vz > bmax[2] + margin) {
                vertsOk = false; break;
            }
        }
        if (!vertsOk) continue;

        // Extract vertices
        mesh.boundsMin = {bmin[0], bmin[1], bmin[2]};
        mesh.boundsMax = {bmax[0], bmax[1], bmax[2]};
        mesh.vertices.resize(numVerts);
        for (int i = 0; i < numVerts; i++) {
            size_t voff = tpos + (size_t)i * VERTEX_STRIDE;
            memcpy(&mesh.vertices[i].x, serialData + voff, 4);
            memcpy(&mesh.vertices[i].y, serialData + voff + 4, 4);
            memcpy(&mesh.vertices[i].z, serialData + voff + 8, 4);
            memcpy(&mesh.vertices[i].nx, serialData + voff + 12, 4);
            memcpy(&mesh.vertices[i].ny, serialData + voff + 16, 4);
            memcpy(&mesh.vertices[i].nz, serialData + voff + 20, 4);
            mesh.vertices[i].u = 0;
            mesh.vertices[i].v = 0;
        }
        tpos += vertBytes;

        // Try to read UV stream
        if (tpos + 1 < endPos) {
            int uvStreams = ReadCompactIndex(serialData + tpos, endPos - tpos, br2);
            tpos += br2;
            if (uvStreams >= 1 && uvStreams <= 8) {
                int numUVs = ReadCompactIndex(serialData + tpos, endPos - tpos, br2);
                tpos += br2;
                if (numUVs == numVerts && tpos + (size_t)numUVs * 8 <= endPos) {
                    for (int i = 0; i < numUVs; i++) {
                        memcpy(&mesh.vertices[i].u, serialData + tpos + (size_t)i * 8, 4);
                        memcpy(&mesh.vertices[i].v, serialData + tpos + (size_t)i * 8 + 4, 4);
                    }
                    tpos += (size_t)numUVs * 8;
                    tpos += 4; // CoordinateIndex
                }
            }
        }

        // Try to read indices
        if (tpos + 1 < endPos) {
            int numIdx = ReadCompactIndex(serialData + tpos, endPos - tpos, br2);
            tpos += br2;
            if (numIdx >= 3 && numIdx <= 5000000 && tpos + (size_t)numIdx * 2 <= endPos) {
                int numTris = numIdx / 3;
                mesh.triangles.resize(numTris);
                bool idxOk = true;
                for (int i = 0; i < numTris; i++) {
                    size_t ioff = tpos + (size_t)i * 6;
                    memcpy(&mesh.triangles[i].i0, serialData + ioff, 2);
                    memcpy(&mesh.triangles[i].i1, serialData + ioff + 2, 2);
                    memcpy(&mesh.triangles[i].i2, serialData + ioff + 4, 2);
                    if (mesh.triangles[i].i0 >= (uint16_t)numVerts ||
                        mesh.triangles[i].i1 >= (uint16_t)numVerts ||
                        mesh.triangles[i].i2 >= (uint16_t)numVerts) {
                        idxOk = false; break;
                    }
                }
                if (!idxOk) { mesh.triangles.clear(); mesh.vertices.clear(); return mesh; }
            }
        }

        if (!mesh.triangles.empty()) {
            mesh.valid = true;
            return mesh;
        }
    }
    return mesh;
}

// ─── Public API: try spec-based first, then fallback heuristic ──────────────
ParsedMesh ParseStaticMesh(const uint8_t* serialData, int serialSize,
                           const std::vector<std::string>& names)
{
    ParsedMesh result = ParseStaticMeshSpec(serialData, serialSize, names);
    if (result.valid) return result;

    // Fallback: scan for the SM class header pattern and extract geometry
    result = ParseStaticMeshHeuristic(serialData, serialSize);
    return result;
}
