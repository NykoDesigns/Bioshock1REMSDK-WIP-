#include "bsm_document.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>

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

// Forward declaration
static const char* RenderTypeName(ActorRenderType rt);

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
    // Skip classes that use binary serialization (not tagged properties).
    // Parsing their raw data as properties produces false matches.
    static const char* skipClasses[] = {
        // UObject infrastructure
        "Class","Function","Struct","State","Enum","Const","ScriptStruct",
        "ByteProperty","IntProperty","FloatProperty","StrProperty","NameProperty",
        "BoolProperty","ObjectProperty","ClassProperty","ArrayProperty",
        "StructProperty","DelegateProperty","Package","TextBuffer",
        "ComponentProperty","InterfaceProperty",
        // Mesh/Model data (binary vertex/index buffers)
        "StaticMesh","StaticMeshComponent","Model","Polys","Brush",
        "SkeletalMesh","AnimSequence","AnimTree","AnimNodeBlendList",
        "AnimNodeSequence","AnimSet","MorphTarget","PhysicsAsset",
        // Textures & Materials (binary pixel data / shader trees)
        "Texture","Texture2D","TextureCube","TextureMovie","TextureRenderTarget2D",
        "Shader","Material","MaterialInstance","MaterialInstanceConstant",
        "MaterialInstanceTimeVarying","MaterialExpression",
        // Sound (binary audio data)
        "SoundCue","SoundNodeWave","SoundNodeConcatenator","SoundNodeMixer",
        "SoundNodeAmbient","SoundNodeAttenuation","SoundNodeLooping",
        "SoundNodeModulator","SoundNodeRandom","SoundNodeWavePlayer",
        "SoundEffectSpecification","SoundNodeDelay","SoundNodeEnveloper",
        // Havok physics (binary collision data)
        "HavokRigidBodyComponent","HavokConstraint","RB_BodySetup",
        "HkMeshProxy","HkMesh","PhysicsVolume",
        // FX/Particles (binary emitter data)
        "ParticleSystem","ParticleEmitter","ParticleSpriteEmitter",
        "ParticleModuleColor","ParticleModuleSize","ParticleModuleLifetime",
        // Level infrastructure
        "Level","LevelStreaming","LevelStreamingDistance","LevelStreamingPersistent",
        // Sequences / Kismet (binary graph data)
        "Sequence","SequenceAction","SequenceEvent","SequenceCondition",
        "SequenceVariable","SequenceOp","SequenceFrame",
        // Event/Effect subsystems
        "EventResponse_VisualEffectsSubsystem",
        "EventResponse_SoundEffectsSubsystem",
        "EventResponse_ModEffectsSubsystem",
        "VisualEffectsSubsystemSpecification",
        "SoundEffectsSubsystemSpecification",
        "ObjectPool","SoundGroup",
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

        // Collect scale properties independently to avoid ordering dependency
        float drawScale = 1.0f;
        bool hasDrawScale = false;
        Vec3 drawScale3D = {1.0f, 1.0f, 1.0f};
        bool hasDrawScale3D = false;

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
                memcpy(&drawScale, d + p.valueOffset, 4);
                hasDrawScale = true;
            }
            if (p.name == "DrawScale3D" && p.size == 12) {
                memcpy(&drawScale3D.x, d + p.valueOffset, 4);
                memcpy(&drawScale3D.y, d + p.valueOffset + 4, 4);
                memcpy(&drawScale3D.z, d + p.valueOffset + 8, 4);
                hasDrawScale3D = true;
            }
        }

        // Combine: final scale = DrawScale * DrawScale3D (order-independent)
        if (hasDrawScale || hasDrawScale3D) {
            float absDS = std::abs(drawScale);
            if (absDS > 0.0001f && absDS < 10000.0f) {
                actor.scale.x = drawScale * drawScale3D.x;
                actor.scale.y = drawScale * drawScale3D.y;
                actor.scale.z = drawScale * drawScale3D.z;
            }
        }

        // Extract light properties for Light actors
        if (pe.className.find("Light") != std::string::npos) {
            actor.isLight = true;
            for (auto& p : props) {
                if (p.name == "LightBrightness" && p.size == 1) {
                    actor.lightBrightness = d[p.valueOffset] / 255.0f;
                } else if (p.name == "LightBrightness" && p.size == 4) {
                    memcpy(&actor.lightBrightness, d + p.valueOffset, 4);
                    // Normalize if stored as 0-255 float
                    if (actor.lightBrightness > 1.5f) actor.lightBrightness /= 255.0f;
                }
                if (p.name == "LightRadius" && p.size == 1) {
                    actor.lightRadius = d[p.valueOffset] * 25.0f; // UE2 byte radius * 25
                } else if (p.name == "LightRadius" && p.size == 4) {
                    memcpy(&actor.lightRadius, d + p.valueOffset, 4);
                    if (actor.lightRadius < 10.0f) actor.lightRadius *= 25.0f;
                }
                if (p.name == "LightColor" && p.size >= 3) {
                    // UE2 Color struct: B,G,R,A
                    actor.lightColorB = d[p.valueOffset];
                    actor.lightColorG = d[p.valueOffset + 1];
                    actor.lightColorR = d[p.valueOffset + 2];
                }
                if (p.name == "LightHue" && p.size == 1) {
                    // HSB hue byte (UE2 legacy) — convert to approximate RGB
                    uint8_t hue = d[p.valueOffset];
                    float h = hue / 255.0f * 6.0f;
                    float r = 0, g = 0, b = 0;
                    int hi = (int)h % 6;
                    float f = h - (int)h;
                    switch (hi) {
                        case 0: r=1; g=f; b=0; break;
                        case 1: r=1-f; g=1; b=0; break;
                        case 2: r=0; g=1; b=f; break;
                        case 3: r=0; g=1-f; b=1; break;
                        case 4: r=f; g=0; b=1; break;
                        case 5: r=1; g=0; b=1-f; break;
                    }
                    actor.lightColorR = (uint8_t)(r * 255);
                    actor.lightColorG = (uint8_t)(g * 255);
                    actor.lightColorB = (uint8_t)(b * 255);
                }
            }
        }

        // Check for mesh properties: multiple property names may reference mesh exports
        // Priority: StaticMesh > Mesh > StaticMeshComponent > Display
        static const char* meshPropNames[] = {
            "StaticMesh", "Mesh", "StaticMeshComponent",
            "PickupMesh", "ThirdPersonMesh", "PickupViewMesh",
        };
        for (auto& propName : meshPropNames) {
            if (actor.meshIndex >= 0) break; // already found one
            for (auto& p : props) {
                if (actor.meshIndex >= 0) break;
                if (p.name != propName) continue;
                if (p.size < 1 || p.size > 5) continue;
                size_t vpos = 0;
                size_t remaining = p.size;
                int ref = ReadCompactIndex(d + p.valueOffset, remaining, vpos);
                if (ref > 0 && ref <= (int)m_ParsedExports.size()) {
                    auto& refExport = m_ParsedExports[ref - 1];
                    if (actor.meshRefName.empty()) actor.meshRefName = refExport.objectName;
                    if (refExport.className == "StaticMesh") {
                        actor.meshIndex = ref - 1;
                    } else if (refExport.className == "StaticMeshComponent" ||
                               refExport.className == "StaticMeshActor") {
                        // Follow indirect: parse the component's properties to find its StaticMesh
                        if (refExport.serialOffset + refExport.serialSize <= (int)fileSize) {
                            int compSkip = DetectHeaderSkip(d, refExport.serialOffset, refExport.serialSize, names);
                            auto compProps = ParsePropsMinimal(d, refExport.serialOffset + compSkip, names,
                                                               refExport.serialOffset + refExport.serialSize);
                            for (auto& cp : compProps) {
                                if (cp.name == "StaticMesh" && cp.size >= 1 && cp.size <= 5) {
                                    size_t cvpos = 0;
                                    size_t cremaining = cp.size;
                                    int cref = ReadCompactIndex(d + cp.valueOffset, cremaining, cvpos);
                                    if (cref > 0 && cref <= (int)m_ParsedExports.size() &&
                                        m_ParsedExports[cref - 1].className == "StaticMesh") {
                                        actor.meshIndex = cref - 1;
                                        actor.meshRefName = m_ParsedExports[cref - 1].objectName;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Parse Projector/Decal properties
        if (actor.className.find("Projector") != std::string::npos ||
            actor.className.find("Decal") != std::string::npos) {
            actor.isProjector = true;
            for (auto& p : props) {
                if (p.name == "ProjTexture" && p.size >= 1 && p.size <= 5) {
                    size_t vpos = 0;
                    int ref = ReadCompactIndex(d + p.valueOffset, p.size, vpos);
                    if (ref > 0 && ref <= (int)m_ParsedExports.size())
                        actor.projTextureName = m_ParsedExports[ref - 1].objectName;
                }
                if (p.name == "FOV" && p.size == 4) {
                    float val;
                    memcpy(&val, d + p.valueOffset, 4);
                    if (val > 0 && val < 180) actor.projFOV = val;
                }
                if ((p.name == "MaxTraceDistance" || p.name == "ProjectDistance") && p.size == 4) {
                    float val;
                    memcpy(&val, d + p.valueOffset, 4);
                    if (val > 0 && val < 100000) actor.projMaxDist = val;
                }
            }
        }

        // Classify render type early to decide if this is a real actor
        bool hasMesh = actor.meshIndex >= 0;
        actor.renderType = ResolveActorRenderType(pe.className, hasMesh, actor.isLight);
        // Include all actors — even without Location — for matching against runtime scan
        m_Actors.push_back(actor);
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

                // Build export/import info for BSP material resolution
                std::vector<BSPExportInfo> bspExports;
                bspExports.reserve(m_ParsedExports.size());
                for (auto& ex : m_ParsedExports)
                    bspExports.push_back({ex.className, ex.objectName});
                std::vector<std::string> importNames;
                importNames.reserve(imports.size());
                for (auto& im : imports)
                    importNames.push_back(im.objectName);

                std::vector<BSPTreeNodeOut> treeOut;
                m_BSPMeshes = ParseBSPGeometry(d + pe.serialOffset, pe.serialSize, names,
                                                bspExports, importNames, &treeOut);
                // Store BSP tree for zone traversal
                m_BSPTree.resize(treeOut.size());
                for (size_t ti = 0; ti < treeOut.size(); ti++) {
                    m_BSPTree[ti].planeX = treeOut[ti].planeX;
                    m_BSPTree[ti].planeY = treeOut[ti].planeY;
                    m_BSPTree[ti].planeZ = treeOut[ti].planeZ;
                    m_BSPTree[ti].planeW = treeOut[ti].planeW;
                    m_BSPTree[ti].iFront = treeOut[ti].iFront;
                    m_BSPTree[ti].iBack = treeOut[ti].iBack;
                    memcpy(m_BSPTree[ti].zoneMask, treeOut[ti].zoneMask, 16);
                    m_BSPTree[ti].iZone = treeOut[ti].iZone;
                }
                if (!m_BSPMeshes.empty()) {
                    int tv = 0, tt = 0;
                    for (auto& c : m_BSPMeshes) { tv += (int)c.vertices.size(); tt += (int)c.triangles.size(); }
                    printf("[BSM] BSP loaded: %d verts, %d tris in %d chunks, tree: %d nodes\n",
                           tv, tt, (int)m_BSPMeshes.size(), (int)m_BSPTree.size());
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
    {
        int withLoc = 0, withoutLoc = 0;
        for (auto& a : m_Actors) { if (a.hasLocation) withLoc++; else withoutLoc++; }
        printf("[BSM] Actor location: %d with Location, %d without (total %d)\n", withLoc, withoutLoc, (int)m_Actors.size());
    }

    // Diagnostic: count actors per mesh to find over-represented meshes
    {
        std::unordered_map<int, int> meshCounts;
        for (auto& a : m_Actors)
            if (a.meshIndex >= 0) meshCounts[a.meshIndex]++;
        std::vector<std::pair<int,int>> sorted(meshCounts.begin(), meshCounts.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
        printf("[MESH-COUNTS] Top 15 meshes by actor count:\n");
        for (int i = 0; i < (int)sorted.size() && i < 15; i++) {
            auto& m = m_Meshes[sorted[i].first];
            printf("  %3d actors -> mesh[%d] '%s'\n", sorted[i].second, sorted[i].first, m.name.c_str());
        }
    }

    // Diagnostic: show actors linked to sign_surgery
    {
        int cnt = 0;
        for (auto& a : m_Actors) {
            if (a.meshIndex >= 0 && a.meshIndex < (int)m_Meshes.size() &&
                m_Meshes[a.meshIndex].name.find("sign_surgery") != std::string::npos) {
                if (cnt < 20)
                    printf("[SURGERY-DIAG] class='%s' name='%s' exportIdx=%d\n",
                           a.className.c_str(), a.objectName.c_str(), a.exportIndex);
                cnt++;
            }
        }
        if (cnt > 0) printf("[SURGERY-DIAG] Total actors with sign_surgery mesh: %d\n", cnt);
    }

    // ─── Assign render types and build diagnostic ─────────────────────────────
    {
        int totalActors = (int)m_Actors.size();
        std::unordered_map<std::string, int> typeCounts; // RenderType → count
        std::unordered_map<std::string, int> placeholderClasses; // class → count (missing mesh)
        int visibleResolved = 0, visibleMissing = 0, hiddenCount = 0, unknownCount = 0;

        for (auto& a : m_Actors) {
            bool hasMesh = a.meshIndex >= 0 && a.meshIndex < (int)m_Meshes.size();
            a.renderType = ResolveActorRenderType(a.className, hasMesh, a.isLight);
            typeCounts[RenderTypeName(a.renderType)]++;

            if (IsVisibleInGame(a.renderType)) {
                if (hasMesh || a.renderType == ActorRenderType::LightOnly ||
                    a.renderType == ActorRenderType::VisibleEmitter ||
                    a.renderType == ActorRenderType::VisibleDecal) {
                    visibleResolved++;
                } else {
                    visibleMissing++;
                }
            } else if (a.renderType == ActorRenderType::UnknownPlaceholder) {
                unknownCount++;
                placeholderClasses[a.className]++;
            } else {
                hiddenCount++;
            }
        }

        printf("\n");
        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║              ACTOR RENDER CLASSIFICATION                ║\n");
        printf("╠══════════════════════════════════════════════════════════╣\n");
        printf("║ Total actors:               %5d                       ║\n", totalActors);
        printf("║ Visible + resolved:         %5d (3D geometry / light)  ║\n", visibleResolved);
        printf("║ Visible but missing mesh:   %5d (placeholder)         ║\n", visibleMissing);
        printf("║ Hidden (editor/trigger/col): %4d                       ║\n", hiddenCount);
        printf("║ Unknown placeholder:        %5d                       ║\n", unknownCount);
        printf("╠══════════════════════════════════════════════════════════╣\n");
        // Sort and print type counts
        std::vector<std::pair<std::string,int>> sorted(typeCounts.begin(), typeCounts.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return b.second < a.second; });
        for (auto& [name, cnt] : sorted)
            printf("║   %-20s  %5d                       ║\n", name.c_str(), cnt);
        printf("╚══════════════════════════════════════════════════════════╝\n");

        if (!placeholderClasses.empty()) {
            printf("\n[UNKNOWN PLACEHOLDERS] Unclassified actor classes:\n");
            std::vector<std::pair<std::string,int>> pSorted(placeholderClasses.begin(), placeholderClasses.end());
            std::sort(pSorted.begin(), pSorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (auto& [cn, cnt] : pSorted)
                printf("  %4d  %s\n", cnt, cn.c_str());
        }

        // Show actors that should have meshes but failed to resolve
        printf("\n[MISSING MESH] Visible actors with unresolved mesh (first 20):\n");
        int shown = 0;
        for (auto& a : m_Actors) {
            if (shown >= 20) break;
            if (a.renderType == ActorRenderType::UnknownPlaceholder) continue; // not expected to have mesh
            bool hasMesh = a.meshIndex >= 0 && a.meshIndex < (int)m_Meshes.size();
            if (hasMesh) continue;
            if (!IsVisibleInGame(a.renderType)) continue;
            if (a.renderType == ActorRenderType::LightOnly) continue;
            if (a.renderType == ActorRenderType::VisibleEmitter) continue;
            if (a.renderType == ActorRenderType::VisibleDecal) continue;
            printf("  [%d] class='%s' name='%s' meshRef='%s' type=%s\n",
                   a.exportIndex, a.className.c_str(), a.objectName.c_str(),
                   a.meshRefName.c_str(), RenderTypeName(a.renderType));
            shown++;
        }

        // Projector/Decal actors summary
        int projCount = 0;
        for (auto& a : m_Actors) {
            if (a.isProjector) projCount++;
        }
        if (projCount > 0) {
            printf("\n[PROJECTORS/DECALS] Found %d projector/decal actors:\n", projCount);
            int pshown = 0;
            for (auto& a : m_Actors) {
                if (!a.isProjector) continue;
                if (pshown++ >= 30) { printf("  ... (%d more)\n", projCount - 30); break; }
                printf("  [%d] class='%s' name='%s' tex='%s' fov=%.1f dist=%.0f loc=(%.0f,%.0f,%.0f)\n",
                       a.exportIndex, a.className.c_str(), a.objectName.c_str(),
                       a.projTextureName.c_str(), a.projFOV, a.projMaxDist,
                       a.location.x, a.location.y, a.location.z);
            }
        }
        printf("\n");
    }

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
    // Player starts / navigation (check BEFORE "Spawn" to avoid false match)
    if (cn.find("PlayerStart") != std::string::npos || cn.find("NavigationPoint") != std::string::npos ||
        cn.find("PathNode") != std::string::npos || cn.find("Teleporter") != std::string::npos)
        return ActorCategory::PlayerStart;

    // Spawners / enemies / AI
    if (cn.find("Spawner") != std::string::npos || cn.find("Spawn") != std::string::npos) return ActorCategory::Spawner;
    if (cn.find("BigDaddy") != std::string::npos || cn.find("Splicer") != std::string::npos ||
        cn.find("Turret") != std::string::npos || cn.find("Security") != std::string::npos ||
        cn.find("Enemy") != std::string::npos || cn.find("AI") != std::string::npos ||
        cn.find("Bot") != std::string::npos || cn.find("NPC") != std::string::npos)
        return ActorCategory::Spawner;

    // Triggers / volumes
    if (cn.find("Trigger") != std::string::npos || cn.find("Volume") != std::string::npos ||
        cn.find("Zone") != std::string::npos || cn.find("PhysicsVolume") != std::string::npos)
        return ActorCategory::Trigger;

    // Lights
    if (cn.find("Light") != std::string::npos) return ActorCategory::Light;

    // Doors / movers
    if (cn.find("Door") != std::string::npos || cn.find("Mover") != std::string::npos ||
        cn.find("Lift") != std::string::npos || cn.find("Elevator") != std::string::npos)
        return ActorCategory::Door;

    // Pickups / items / loot / weapons / ammo
    if (cn.find("Pickup") != std::string::npos || cn.find("Loot") != std::string::npos ||
        cn.find("Weapon") != std::string::npos || cn.find("Ammo") != std::string::npos ||
        cn.find("HealthStation") != std::string::npos || cn.find("VendingMachine") != std::string::npos ||
        cn.find("Item") != std::string::npos || cn.find("Audio") != std::string::npos ||
        cn.find("Diary") != std::string::npos || cn.find("Plasmid") != std::string::npos ||
        cn.find("Tonic") != std::string::npos || cn.find("Gene") != std::string::npos)
        return ActorCategory::Pickup;

    // Effects / emitters / sounds / particles
    if (cn.find("Emitter") != std::string::npos || cn.find("Effect") != std::string::npos ||
        cn.find("Particle") != std::string::npos || cn.find("Sound") != std::string::npos ||
        cn.find("Ambient") != std::string::npos || cn.find("Fog") != std::string::npos ||
        cn.find("Smoke") != std::string::npos || cn.find("Fire") != std::string::npos ||
        cn.find("Water") != std::string::npos || cn.find("Steam") != std::string::npos ||
        cn.find("Beam") != std::string::npos || cn.find("Decal") != std::string::npos ||
        cn.find("Camera") != std::string::npos || cn.find("Projector") != std::string::npos)
        return ActorCategory::Effect;

    // Static meshes
    if (cn.find("StaticMesh") != std::string::npos || cn.find("Brush") != std::string::npos)
        return ActorCategory::StaticMesh;

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

// ─── Actor Render Type Resolution ──────────────────────────────────────────

ActorRenderType ResolveActorRenderType(const std::string& cn, bool hasMesh, bool isLight)
{
    // Lights → lighting contribution only
    if (isLight) return ActorRenderType::LightOnly;

    // Editor-only / navigation helpers
    if (cn.find("PlayerStart") != std::string::npos ||
        cn.find("PathNode") != std::string::npos ||
        cn.find("NavigationPoint") != std::string::npos ||
        cn.find("Teleporter") != std::string::npos ||
        cn.find("Keypoint") != std::string::npos ||
        cn.find("AIController") != std::string::npos ||
        cn.find("GameReplicationInfo") != std::string::npos ||
        cn == "LevelInfo" || cn == "ZoneInfo" ||
        cn == "DefaultPhysicsVolume" ||
        cn.find("Bookmark") != std::string::npos ||
        cn.find("Note") != std::string::npos)
        return ActorRenderType::EditorOnly;

    // Triggers
    if (cn.find("Trigger") != std::string::npos && cn.find("TriggerLight") == std::string::npos)
        return ActorRenderType::TriggerOnly;

    // Collision-only volumes
    if (cn.find("BlockingVolume") != std::string::npos ||
        cn.find("PhysicsVolume") != std::string::npos ||
        cn.find("Volume") != std::string::npos)
        return ActorRenderType::CollisionOnly;

    // Zone actors (not visible geometry)
    if (cn.find("ZoneInfo") != std::string::npos)
        return ActorRenderType::EditorOnly;

    // Emitters / particles
    if (cn.find("Emitter") != std::string::npos ||
        cn.find("Particle") != std::string::npos ||
        cn.find("Smoke") != std::string::npos ||
        cn.find("Fire") != std::string::npos ||
        cn.find("Steam") != std::string::npos ||
        cn.find("Beam") != std::string::npos)
        return ActorRenderType::VisibleEmitter;

    // Decals
    if (cn.find("Decal") != std::string::npos ||
        cn.find("Projector") != std::string::npos)
        return ActorRenderType::VisibleDecal;

    // Doors / movers
    if (cn.find("Mover") != std::string::npos ||
        cn.find("Door") != std::string::npos ||
        cn.find("Lift") != std::string::npos ||
        cn.find("Elevator") != std::string::npos ||
        cn.find("InterpActor") != std::string::npos) {
        return hasMesh ? ActorRenderType::VisibleMover : ActorRenderType::UnknownPlaceholder;
    }

    // Pickups / weapons / items
    if (cn.find("Pickup") != std::string::npos ||
        cn.find("Weapon") != std::string::npos ||
        cn.find("Ammo") != std::string::npos ||
        cn.find("Item") != std::string::npos ||
        cn.find("Plasmid") != std::string::npos ||
        cn.find("Tonic") != std::string::npos ||
        cn.find("Gene") != std::string::npos ||
        cn.find("HealthStation") != std::string::npos ||
        cn.find("VendingMachine") != std::string::npos) {
        return hasMesh ? ActorRenderType::VisiblePickup : ActorRenderType::UnknownPlaceholder;
    }

    // Decorations / debris / bodies
    if (cn.find("Decoration") != std::string::npos ||
        cn.find("DestroyableObject") != std::string::npos ||
        cn.find("KActor") != std::string::npos ||
        cn.find("Corpse") != std::string::npos ||
        cn.find("Body") != std::string::npos ||
        cn.find("Ragdoll") != std::string::npos) {
        return hasMesh ? ActorRenderType::VisibleDecoration : ActorRenderType::UnknownPlaceholder;
    }

    // Static meshes (most common visible actor)
    if (cn.find("StaticMesh") != std::string::npos) {
        return hasMesh ? ActorRenderType::VisibleStaticMesh : ActorRenderType::UnknownPlaceholder;
    }

    // Sound / ambient (invisible in-game)
    if (cn.find("Sound") != std::string::npos ||
        cn.find("Ambient") != std::string::npos ||
        cn.find("Audio") != std::string::npos ||
        cn.find("Diary") != std::string::npos)
        return ActorRenderType::EditorOnly;

    // Spawners / AI enemies (have skeletal meshes in-game, but we can't render them yet)
    if (cn.find("Spawner") != std::string::npos ||
        cn.find("Spawn") != std::string::npos ||
        cn.find("BigDaddy") != std::string::npos ||
        cn.find("Splicer") != std::string::npos ||
        cn.find("Turret") != std::string::npos ||
        cn.find("Security") != std::string::npos ||
        cn.find("Enemy") != std::string::npos ||
        cn.find("NPC") != std::string::npos ||
        cn.find("Bot") != std::string::npos)
        return ActorRenderType::EditorOnly;

    // Camera actors
    if (cn.find("Camera") != std::string::npos)
        return ActorRenderType::EditorOnly;

    // Sequence / Kismet (logic only)
    if (cn.find("Sequence") != std::string::npos)
        return ActorRenderType::EditorOnly;

    // Fallback: if it has a mesh, it's probably visible
    if (hasMesh) return ActorRenderType::VisibleStaticMesh;

    return ActorRenderType::UnknownPlaceholder;
}

bool IsVisibleInGame(ActorRenderType rt)
{
    switch (rt) {
        case ActorRenderType::VisibleStaticMesh:
        case ActorRenderType::VisibleMover:
        case ActorRenderType::VisiblePickup:
        case ActorRenderType::VisibleDecoration:
        case ActorRenderType::VisibleEmitter:
        case ActorRenderType::VisibleDecal:
        case ActorRenderType::LightOnly:
            return true;
        default:
            return false;
    }
}

static const char* RenderTypeName(ActorRenderType rt)
{
    switch (rt) {
        case ActorRenderType::VisibleStaticMesh: return "StaticMesh";
        case ActorRenderType::VisibleMover:      return "Mover";
        case ActorRenderType::VisiblePickup:     return "Pickup";
        case ActorRenderType::VisibleDecoration: return "Decoration";
        case ActorRenderType::VisibleEmitter:    return "Emitter";
        case ActorRenderType::VisibleDecal:      return "Decal";
        case ActorRenderType::LightOnly:         return "Light";
        case ActorRenderType::CollisionOnly:     return "Collision";
        case ActorRenderType::TriggerOnly:       return "Trigger";
        case ActorRenderType::EditorOnly:        return "EditorOnly";
        case ActorRenderType::UnknownPlaceholder:return "Unknown";
    }
    return "???";
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

    // Step 3: Resolve BSP chunk texture names (already set by BSP parser from surf Material refs)
    int bspResolved = 0;
    std::string texDir = umodelExportDir + "\\Texture";
    for (auto& chunk : m_BSPMeshes) {
        if (chunk.textureName.empty()) continue;
        std::string candidate = chunk.textureName;

        // Check if it directly matches a TGA
        if (fs::exists(texDir + "\\" + candidate + ".tga")) {
            bspResolved++;
            continue;
        }

        // Try shader→diffuse lookup via .mat files
        // The chunk.textureName was already stripped of _Shader by the BSP parser,
        // but the .mat file uses the full shader name. Try both.
        std::string shaderName = candidate + "_Shader";
        auto it = shaderToDiffuse.find(shaderName);
        if (it == shaderToDiffuse.end()) {
            shaderName = candidate + "_shader";
            it = shaderToDiffuse.find(shaderName);
        }
        if (it == shaderToDiffuse.end()) {
            // Try the candidate itself as a shader name
            it = shaderToDiffuse.find(candidate);
        }
        if (it != shaderToDiffuse.end() && fs::exists(texDir + "\\" + it->second + ".tga")) {
            chunk.textureName = it->second;
            bspResolved++;
        } else if (fs::exists(texDir + "\\" + candidate + ".tga")) {
            bspResolved++;
        }
        // else leave textureName as-is; TextureCache will try suffixes
    }
    // Diagnostic: show first 10 unresolved BSP texture names
    int unresLog = 0;
    for (auto& chunk : m_BSPMeshes) {
        if (chunk.textureName.empty()) continue;
        if (!fs::exists(texDir + "\\" + chunk.textureName + ".tga") && unresLog < 10) {
            printf("[BSP-TEX] Unresolved: '%s'\n", chunk.textureName.c_str());
            unresLog++;
        }
    }
    printf("[BSM] BSP textures: %d/%d chunks resolved\n", bspResolved, (int)m_BSPMeshes.size());
}

int BSMDocument::FindCameraZone(Vec3 pos) const
{
    if (m_BSPTree.empty()) return -1;

    // Traverse BSP tree from root (node 0) to find the camera's zone
    int nodeIdx = 0;
    int lastValidNode = 0;
    int depth = 0;
    const int maxDepth = 128;

    while (nodeIdx >= 0 && nodeIdx < (int)m_BSPTree.size() && depth < maxDepth) {
        auto& node = m_BSPTree[nodeIdx];
        float side = pos.x * node.planeX + pos.y * node.planeY + pos.z * node.planeZ - node.planeW;
        lastValidNode = nodeIdx;
        if (side >= 0.0f) {
            nodeIdx = node.iFront;
        } else {
            nodeIdx = node.iBack;
        }
        depth++;
    }

    return (int)m_BSPTree[lastValidNode].iZone;
}
