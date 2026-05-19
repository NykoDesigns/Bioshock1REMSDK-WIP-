#include "bsm_document.h"
#include <fstream>
#include <filesystem>
#include <algorithm>

// ─── Inline BSM parser (extracted from bsm_tool for self-contained build) ───
// This is a minimal version that parses enough to extract actor positions.

static constexpr uint32_t UE_MAGIC = 0x9E2A83C1;

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

static int ReadCI(const uint8_t* d, size_t& pos)
{
    uint8_t b0 = d[pos++];
    bool sign = (b0 & 0x80) != 0;
    bool more = (b0 & 0x40) != 0;
    int val = b0 & 0x3F;
    if (more) {
        uint8_t b1 = d[pos++]; val |= (b1 & 0x7F) << 6;
        if (b1 & 0x80) { uint8_t b2 = d[pos++]; val |= (b2 & 0x7F) << 13;
            if (b2 & 0x80) { uint8_t b3 = d[pos++]; val |= (b3 & 0x7F) << 20;
                if (b3 & 0x80) { uint8_t b4 = d[pos++]; val |= (b4 & 0x7F) << 27; }
            }
        }
    }
    return sign ? -val : val;
}

struct FNameRef { int32_t index; int32_t number; };

static FNameRef ReadFNameRef(const uint8_t* d, size_t maxLen, size_t& pos)
{
    FNameRef fn;
    size_t br;
    fn.index = ReadCompactIndex(d + pos, maxLen - pos, br); pos += br;
    fn.number = *reinterpret_cast<const int32_t*>(d + pos) - 1; pos += 4;
    return fn;
}

enum PropType {
    PT_None=0, PT_Byte=1, PT_Int=2, PT_Bool=3, PT_Float=4,
    PT_Object=5, PT_Name=6, PT_String=7, PT_Class=8, PT_Array=9,
    PT_Struct=10, PT_Vector=11, PT_Rotator=12
};

static int DecodePackedSize(const uint8_t* d, size_t& pos, int sizeBits)
{
    switch (sizeBits) {
        case 0: return 1;   case 1: return 2;
        case 2: return 4;   case 3: return 12;
        case 4: return 16;
        case 5: return d[pos++];
        case 6: { uint16_t v; memcpy(&v, d+pos, 2); pos += 2; return v; }
        case 7: { uint32_t v; memcpy(&v, d+pos, 4); pos += 4; return (int)v; }
        default: return 0;
    }
}

struct SimpleProp {
    std::string name;
    int type;
    int size;
    size_t valueOffset;
    std::string structName;
};

static std::vector<SimpleProp> ParsePropsMinimal(const uint8_t* data, size_t pos,
                                                  const std::vector<std::string>& names,
                                                  size_t endPos)
{
    std::vector<SimpleProp> props;
    while (pos < endPos - 5) {
        SimpleProp p;
        size_t br;
        int nameIdx = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
        int32_t nameNum = *reinterpret_cast<const int32_t*>(data + pos) - 1; pos += 4;

        if (nameIdx == 0) break; // "None" sentinel
        if (nameIdx < 0 || nameIdx >= (int)names.size()) break;
        p.name = names[nameIdx];

        if (pos >= endPos) break;
        uint8_t info = data[pos++];
        p.type = info & 0x0F;
        int sizeBits = (info >> 4) & 0x07;
        int arrayFlag = (info >> 7) & 1;

        if (p.type == PT_Struct) {
            int sIdx = ReadCompactIndex(data + pos, endPos - pos, br); pos += br;
            int32_t sNum = *reinterpret_cast<const int32_t*>(data + pos); pos += 4;
            if (sIdx >= 0 && sIdx < (int)names.size()) p.structName = names[sIdx];
        }

        p.size = DecodePackedSize(data, pos, sizeBits);

        if (p.type == PT_Bool) {
            // no payload
        } else if (arrayFlag) {
            uint8_t b = data[pos++];
            if ((b & 0xC0) == 0x80) pos++;
            else if ((b & 0xC0) == 0xC0) pos += 3;
        }

        p.valueOffset = pos;
        pos += p.size;
        props.push_back(p);
    }
    return props;
}

static int DetectHeaderSkip(const uint8_t* data, size_t offset, int size,
                            const std::vector<std::string>& names)
{
    int bestSkip = 57;
    int bestScore = 0;
    static const char* known[] = {"Tag","Location","Rotation","Label","Region","Level",
                                   "PhysicsVolume","StaticMesh","CollisionRadius"};
    for (int skip = 4; skip < 80 && skip < size; skip++) {
        auto props = ParsePropsMinimal(data, offset + skip, names, offset + size);
        int score = 0;
        for (auto& p : props)
            for (auto& k : known)
                if (p.name == k) score++;
        if (score > bestScore) { bestScore = score; bestSkip = skip; }
    }
    return bestSkip;
}

// ─── BSMDocument Implementation ─────────────────────────────────────────────

bool BSMDocument::Load(const std::string& filepath)
{
    m_Loaded = false;
    m_Actors.clear();
    m_ParsedExports.clear();
    m_Meshes.clear();
    m_MeshNameToIndex.clear();
    m_BSPMeshes.clear();
    m_FilePath = filepath;
    m_MapName = std::filesystem::path(filepath).filename().string();

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    size_t fileSize = file.tellg();
    file.seekg(0);
    m_RawData.resize(fileSize);
    file.read(reinterpret_cast<char*>(m_RawData.data()), fileSize);

    if (fileSize < 36) return false;
    auto* d = m_RawData.data();

    // Header
    uint32_t magic = *reinterpret_cast<uint32_t*>(d);
    if (magic != UE_MAGIC) return false;

    m_NameCount   = *reinterpret_cast<int32_t*>(d + 12);
    int nameOff   = *reinterpret_cast<int32_t*>(d + 16);
    m_ExportCount = *reinterpret_cast<int32_t*>(d + 20);
    int expOff    = *reinterpret_cast<int32_t*>(d + 24);
    m_ImportCount = *reinterpret_cast<int32_t*>(d + 28);
    int impOff    = *reinterpret_cast<int32_t*>(d + 32);

    // Parse name table
    std::vector<std::string> names;
    size_t pos = nameOff;
    for (int i = 0; i < m_NameCount && pos < fileSize; i++) {
        size_t br = 0;
        int rawLen = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        int charCount = (rawLen < 0) ? -rawLen : rawLen;
        if (charCount <= 0 || charCount > 65536) break;

        std::string name;
        for (int c = 0; c < charCount && pos + 1 < fileSize; c++) {
            uint16_t wc = *reinterpret_cast<uint16_t*>(d + pos); pos += 2;
            if (wc == 0) break;
            name += (wc < 128) ? (char)wc : '?';
        }
        if (pos + 1 < fileSize && *reinterpret_cast<uint16_t*>(d + pos) == 0) pos += 2;
        pos += 8; // flags
        names.push_back(name);
    }

    // Parse import table (just names for class resolution)
    struct ImportInfo { std::string className; std::string objectName; };
    std::vector<ImportInfo> imports;
    pos = impOff;
    for (int i = 0; i < m_ImportCount && pos < fileSize; i++) {
        size_t br;
        ReadCompactIndex(d + pos, fileSize - pos, br); pos += br; pos += 4; // classPackage
        int clsIdx = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        pos += 4; // className number
        pos += 4; // outerIndex
        int objIdx = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        pos += 4; // objectName number

        ImportInfo ii;
        ii.className = (clsIdx >= 0 && clsIdx < (int)names.size()) ? names[clsIdx] : "?";
        ii.objectName = (objIdx >= 0 && objIdx < (int)names.size()) ? names[objIdx] : "?";
        imports.push_back(ii);
    }

    // Parse export table
    pos = expOff;
    for (int i = 0; i < m_ExportCount && pos < fileSize; i++) {
        size_t br;
        int classIdx = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        ReadCompactIndex(d + pos, fileSize - pos, br); pos += br; // superIndex
        pos += 4; // outerIndex
        pos += 4; // unknownBS1
        int objNameIdx = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        int32_t objNameNum = *reinterpret_cast<int32_t*>(d + pos) - 1; pos += 4;
        pos += 8; // objectFlags (uint64)
        int serialSize = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        int serialOffset = 0;
        if (serialSize > 0) {
            serialOffset = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        }
        pos += 4; // unknownBS2

        // Resolve class name
        std::string className;
        if (classIdx == 0) className = "Class";
        else if (classIdx < 0) {
            int idx = -classIdx - 1;
            if (idx < (int)imports.size()) className = imports[idx].objectName;
            else className = "?";
        } else {
            // export ref — rare for class
            className = "ExportClass";
        }

        // Resolve object name
        std::string objName = (objNameIdx >= 0 && objNameIdx < (int)names.size()) ? names[objNameIdx] : "?";
        if (objNameNum >= 0) objName += "_" + std::to_string(objNameNum);

        ParsedExport pe;
        pe.classIndex = classIdx;
        pe.className = className;
        pe.objectName = objName;
        pe.serialSize = serialSize;
        pe.serialOffset = serialOffset;
        pe.locationValueFileOffset = -1;
        pe.rotationValueFileOffset = -1;
        m_ParsedExports.push_back(pe);
    }

    // Extract actors (exports with Location property)
    // Skip structural classes
    static const char* skipClasses[] = {
        "Class","Function","Struct","State","Enum","Const","ScriptStruct",
        "ByteProperty","IntProperty","FloatProperty","StrProperty","NameProperty",
        "BoolProperty","ObjectProperty","ClassProperty","ArrayProperty",
        "StructProperty","DelegateProperty","Package","TextBuffer",
        "ComponentProperty","InterfaceProperty"
    };

    for (int i = 0; i < (int)m_ParsedExports.size(); i++) {
        auto& pe = m_ParsedExports[i];
        if (pe.serialSize <= 0) continue;

        bool skip = false;
        for (auto& sc : skipClasses)
            if (pe.className == sc) { skip = true; break; }
        if (skip) continue;

        // Try to parse properties and find Location
        if (pe.serialOffset + pe.serialSize > (int)fileSize) continue;

        int headerSkip = DetectHeaderSkip(d, pe.serialOffset, pe.serialSize, names);
        auto props = ParsePropsMinimal(d, pe.serialOffset + headerSkip, names,
                                        pe.serialOffset + pe.serialSize);

        EditorActor actor;
        actor.exportIndex = i;
        actor.className = pe.className;
        actor.objectName = pe.objectName;
        actor.serialSize = pe.serialSize;
        actor.serialOffset = pe.serialOffset;

        for (auto& p : props) {
            if (p.name == "Location" && p.size == 12) {
                memcpy(&actor.location.x, d + p.valueOffset, 4);
                memcpy(&actor.location.y, d + p.valueOffset + 4, 4);
                memcpy(&actor.location.z, d + p.valueOffset + 8, 4);
                actor.hasLocation = true;
                pe.locationValueFileOffset = (int)p.valueOffset;
            }
            if (p.name == "Rotation" && p.size == 12) {
                int32_t pitch, yaw, roll;
                memcpy(&pitch, d + p.valueOffset, 4);
                memcpy(&yaw, d + p.valueOffset + 4, 4);
                memcpy(&roll, d + p.valueOffset + 8, 4);
                actor.rotation.x = pitch * (360.0f / 65536.0f);
                actor.rotation.y = yaw * (360.0f / 65536.0f);
                actor.rotation.z = roll * (360.0f / 65536.0f);
                pe.rotationValueFileOffset = (int)p.valueOffset;
            }
            if (p.name == "DrawScale" && p.size == 4) {
                float ds;
                memcpy(&ds, d + p.valueOffset, 4);
                if (ds > 0.001f && ds < 1000.0f) {
                    actor.scale = {ds, ds, ds};
                }
            }
            if (p.name == "DrawScale3D" && p.size == 12) {
                float sx, sy, sz;
                memcpy(&sx, d + p.valueOffset, 4);
                memcpy(&sy, d + p.valueOffset + 4, 4);
                memcpy(&sz, d + p.valueOffset + 8, 4);
                if (sx > 0.001f && sx < 1000.0f) actor.scale.x *= sx;
                if (sy > 0.001f && sy < 1000.0f) actor.scale.y *= sy;
                if (sz > 0.001f && sz < 1000.0f) actor.scale.z *= sz;
            }
        }

        // Check for StaticMesh property (object reference to a StaticMesh export)
        for (auto& p : props) {
            if (p.name == "StaticMesh" && p.size >= 1) {
                // Read compact index from value
                size_t vpos = 0;
                size_t remaining = p.size;
                int ref = ReadCompactIndex(d + p.valueOffset, remaining, vpos);
                if (ref > 0 && ref <= (int)m_ParsedExports.size()) {
                    // Export reference (1-based) — store name for later linking
                    actor.meshIndex = ref - 1; // will be resolved to mesh index later
                }
            }
        }

        if (actor.hasLocation) {
            m_Actors.push_back(actor);
        }
    }

    // ─── Parse StaticMesh geometry directly from BSM (C.4 spec) ────────────────
    printf("[BSM] Parsing StaticMesh exports directly from BSM...\n");
    int meshesFound = 0, meshesParsed = 0;
    for (int i = 0; i < (int)m_ParsedExports.size(); i++) {
        auto& pe = m_ParsedExports[i];
        if (pe.className != "StaticMesh") continue;
        if (pe.serialSize < 100) continue;
        if (pe.serialOffset + pe.serialSize > (int)fileSize) continue;
        meshesFound++;
        ParsedMesh mesh = ParseStaticMesh(d + pe.serialOffset, pe.serialSize, names);
        if (mesh.valid) {
            mesh.name = pe.objectName;
            m_MeshNameToIndex[pe.objectName] = (int)m_Meshes.size();
            m_Meshes.push_back(std::move(mesh));
            meshesParsed++;
        }
    }
    printf("[BSM] StaticMesh parsing: %d found, %d successfully parsed (%d verts, %d tris)\n",
           meshesFound, meshesParsed,
           [&]() { int v = 0; for (auto& m : m_Meshes) v += (int)m.vertices.size(); return v; }(),
           [&]() { int t = 0; for (auto& m : m_Meshes) t += (int)m.triangles.size(); return t; }());

    // ─── Parse BSP geometry from UModel exports (C.1 spec) ─────────────────────
    m_BSPMeshes.clear();
    {
        // Find the largest Model export (that's the main level BSP)
        int bestModelIdx = -1;
        int bestModelSize = 0;
        for (int i = 0; i < (int)m_ParsedExports.size(); i++) {
            auto& pe = m_ParsedExports[i];
            if (pe.className != "Model") continue;
            if (pe.serialSize > bestModelSize) {
                bestModelSize = pe.serialSize;
                bestModelIdx = i;
            }
        }
        if (bestModelIdx >= 0) {
            auto& pe = m_ParsedExports[bestModelIdx];
            if (pe.serialOffset + pe.serialSize <= (int)fileSize) {
                printf("[BSM] Parsing BSP from Model export '%s' (%d bytes)...\n",
                       pe.objectName.c_str(), pe.serialSize);
                m_BSPMeshes = ParseBSPGeometry(d + pe.serialOffset, pe.serialSize, names);
                if (!m_BSPMeshes.empty()) {
                    int tv = 0, tt = 0;
                    for (auto& c : m_BSPMeshes) { tv += (int)c.vertices.size(); tt += (int)c.triangles.size(); }
                    printf("[BSM] BSP loaded: %d verts, %d tris in %d chunks\n", tv, tt, (int)m_BSPMeshes.size());
                } else {
                    printf("[BSM] BSP parsing failed\n");
                }
            }
        } else {
            printf("[BSM] No Model export found for BSP\n");
        }
    }

    // If BSM parsing got nothing, try glTF exports as fallback
    if (m_Meshes.empty()) {
        std::string gltfMapName = m_MapName;
        { size_t dp = gltfMapName.find_last_of('.'); if (dp != std::string::npos) gltfMapName = gltfMapName.substr(0, dp); }
        std::string exportDir = "Z:\\UEViewer\\export\\" + gltfMapName;
        printf("[BSM] No meshes from BSM, trying glTF fallback: %s\n", exportDir.c_str());
        m_Meshes = LoadAllMeshesFromExportDir(exportDir);
        for (int i = 0; i < (int)m_Meshes.size(); i++)
            m_MeshNameToIndex[m_Meshes[i].name] = i;
    }

    // Build normalized name map for fuzzy matching
    auto normalizeName = [](const std::string& s) -> std::string {
        std::string r;
        r.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            char lc = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            if (lc == '_') {
                if (i + 1 < s.size() && s[i+1] >= '0' && s[i+1] <= '9') continue;
                if (!r.empty() && r.back() == '_') continue;
                r += '_';
            } else {
                r += lc;
            }
        }
        return r;
    };
    std::unordered_map<std::string, int> normalizedNameMap;
    for (int i = 0; i < (int)m_Meshes.size(); i++)
        normalizedNameMap[normalizeName(m_Meshes[i].name)] = i;

    // Link actors to their meshes by export index (try exact then normalized name)
    int linked = 0, noRef = 0, badRef = 0, noMatch = 0;
    int unmatchedLogged = 0;
    for (auto& actor : m_Actors) {
        if (actor.meshIndex >= 0 && actor.meshIndex < (int)m_ParsedExports.size()) {
            auto& meshExport = m_ParsedExports[actor.meshIndex];
            // Try exact match first
            auto it = m_MeshNameToIndex.find(meshExport.objectName);
            if (it != m_MeshNameToIndex.end()) {
                actor.meshIndex = it->second;
                linked++;
            } else {
                // Try normalized name (collapse underscores, case insensitive)
                auto it2 = normalizedNameMap.find(normalizeName(meshExport.objectName));
                if (it2 != normalizedNameMap.end()) {
                    actor.meshIndex = it2->second;
                    linked++;
                } else {
                    if (unmatchedLogged < 10) {
                        printf("[BSM] Unmatched mesh: '%s' (norm: '%s')\n",
                               meshExport.objectName.c_str(), normalizeName(meshExport.objectName).c_str());
                        unmatchedLogged++;
                    }
                    noMatch++;
                    actor.meshIndex = -1;
                }
            }
        } else if (actor.meshIndex >= 0) {
            badRef++;
            actor.meshIndex = -1;
        } else {
            noRef++;
            actor.meshIndex = -1;
        }
    }
    printf("[BSM] Linked %d actors to mesh geometry (%d no ref, %d bad ref, %d unmatched)\n", linked, noRef, badRef, noMatch);

    m_Loaded = true;
    return true;
}

bool BSMDocument::Save(const std::string& filepath)
{
    if (!m_Loaded) return false;
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<char*>(m_RawData.data()), m_RawData.size());
    return true;
}

bool BSMDocument::SetActorLocation(int actorIdx, Vec3 newLoc)
{
    if (actorIdx < 0 || actorIdx >= (int)m_Actors.size()) return false;
    auto& actor = m_Actors[actorIdx];
    auto& pe = m_ParsedExports[actor.exportIndex];
    if (pe.locationValueFileOffset < 0) return false;

    // Update in-memory raw data
    memcpy(m_RawData.data() + pe.locationValueFileOffset, &newLoc.x, 4);
    memcpy(m_RawData.data() + pe.locationValueFileOffset + 4, &newLoc.y, 4);
    memcpy(m_RawData.data() + pe.locationValueFileOffset + 8, &newLoc.z, 4);
    actor.location = newLoc;
    return true;
}

bool BSMDocument::SetActorRotation(int actorIdx, Vec3 newRot)
{
    if (actorIdx < 0 || actorIdx >= (int)m_Actors.size()) return false;
    auto& actor = m_Actors[actorIdx];
    auto& pe = m_ParsedExports[actor.exportIndex];
    if (pe.rotationValueFileOffset < 0) return false;

    int32_t pitch = (int32_t)(newRot.x * (65536.0f / 360.0f));
    int32_t yaw = (int32_t)(newRot.y * (65536.0f / 360.0f));
    int32_t roll = (int32_t)(newRot.z * (65536.0f / 360.0f));
    memcpy(m_RawData.data() + pe.rotationValueFileOffset, &pitch, 4);
    memcpy(m_RawData.data() + pe.rotationValueFileOffset + 4, &yaw, 4);
    memcpy(m_RawData.data() + pe.rotationValueFileOffset + 8, &roll, 4);
    actor.rotation = newRot;
    return true;
}

// ─── Actor Categorization ───────────────────────────────────────────────────

ActorCategory CategorizeActor(const std::string& cn)
{
    if (cn.find("Spawner") != std::string::npos) return ActorCategory::Spawner;
    if (cn.find("Trigger") != std::string::npos) return ActorCategory::Trigger;
    if (cn.find("Light") != std::string::npos) return ActorCategory::Light;
    if (cn.find("Door") != std::string::npos) return ActorCategory::Door;
    if (cn.find("Pickup") != std::string::npos || cn.find("Loot") != std::string::npos)
        return ActorCategory::Pickup;
    if (cn.find("Emitter") != std::string::npos || cn.find("Effect") != std::string::npos ||
        cn.find("Particle") != std::string::npos)
        return ActorCategory::Effect;
    if (cn.find("PlayerStart") != std::string::npos) return ActorCategory::PlayerStart;
    if (cn.find("StaticMesh") != std::string::npos) return ActorCategory::StaticMesh;
    return ActorCategory::Other;
}

Vec3 CategoryColor(ActorCategory cat)
{
    switch (cat) {
        case ActorCategory::Spawner:     return {1.0f, 0.2f, 0.2f};
        case ActorCategory::Trigger:     return {1.0f, 1.0f, 0.2f};
        case ActorCategory::Light:       return {1.0f, 0.7f, 0.0f};
        case ActorCategory::Door:        return {0.2f, 0.5f, 1.0f};
        case ActorCategory::Pickup:      return {0.2f, 1.0f, 0.2f};
        case ActorCategory::Effect:      return {0.2f, 1.0f, 1.0f};
        case ActorCategory::PlayerStart: return {1.0f, 1.0f, 1.0f};
        case ActorCategory::StaticMesh:  return {0.6f, 0.6f, 0.6f};
        case ActorCategory::Other:       return {0.4f, 0.4f, 0.4f};
    }
    return {0.5f, 0.5f, 0.5f};
}

void BSMDocument::ResolveTextures(const std::string& umodelExportDir)
{
    namespace fs = std::filesystem;
    if (umodelExportDir.empty()) return;

    std::string shaderDir = umodelExportDir + "\\Shader";
    std::string meshDir = umodelExportDir + "\\StaticMesh";

    // Step 1: Load shader→diffuseTexture map from .mat files
    std::unordered_map<std::string, std::string> shaderToDiffuse;
    if (fs::is_directory(shaderDir)) {
        for (auto& entry : fs::directory_iterator(shaderDir)) {
            if (entry.path().extension() != ".mat") continue;
            std::string shaderName = entry.path().stem().string();
            std::ifstream f(entry.path());
            std::string line;
            while (std::getline(f, line)) {
                if (line.size() > 8 && line.substr(0, 8) == "Diffuse=") {
                    shaderToDiffuse[shaderName] = line.substr(8);
                    break;
                }
            }
        }
    }

    // Step 2: For each mesh, find its material from the corresponding glTF
    int resolved = 0;
    for (auto& mesh : m_Meshes) {
        if (mesh.name.empty()) continue;
        std::string gltfPath = meshDir + "\\" + mesh.name + ".gltf";
        std::ifstream f(gltfPath);
        if (!f.is_open()) continue;

        // Quick scan for first valid material name in the glTF JSON
        std::string line;
        std::string matName;
        bool inMaterials = false;
        while (std::getline(f, line)) {
            if (line.find("\"materials\"") != std::string::npos) inMaterials = true;
            if (inMaterials) {
                auto namePos = line.find("\"name\" : \"");
                if (namePos != std::string::npos) {
                    auto start = namePos + 10;
                    auto end = line.find("\"", start);
                    if (end != std::string::npos) {
                        std::string candidate = line.substr(start, end - start);
                        // Skip placeholder/dummy materials
                        if (candidate.find("dummy_material") == std::string::npos &&
                            candidate != "None" && !candidate.empty()) {
                            matName = candidate;
                            break;
                        }
                    }
                }
            }
        }
        if (matName.empty()) continue;

        // Strip _shader/_Shader suffix to get candidate texture name
        std::string texName = matName;
        if (texName.size() > 7 && texName.substr(texName.size() - 7) == "_shader") {
            texName = texName.substr(0, texName.size() - 7);
        } else if (texName.size() > 7 && texName.substr(texName.size() - 7) == "_Shader") {
            texName = texName.substr(0, texName.size() - 7);
        }

        // Check if stripped name directly matches a texture file
        std::string texDir = umodelExportDir + "\\Texture";
        std::string directPath = texDir + "\\" + texName + ".tga";
        if (fs::exists(directPath)) {
            mesh.textureName = texName;
        } else {
            // Fall back to .mat Diffuse= value if it exists on disk
            auto it = shaderToDiffuse.find(matName);
            if (it != shaderToDiffuse.end() && fs::exists(texDir + "\\" + it->second + ".tga")) {
                mesh.textureName = it->second;
            } else {
                mesh.textureName = texName; // best guess, TextureCache will try suffixes
            }
        }
        resolved++;
    }
    printf("[BSM] Resolved %d/%d mesh texture names\n", resolved, (int)m_Meshes.size());
}
