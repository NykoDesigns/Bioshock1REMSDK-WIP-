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

        if (p.type == PT_Bool) {
            // Bool value is in the arrayFlag bit; no size bytes, no payload
            p.size = 0;
            p.valueOffset = pos;
        } else {
            p.size = DecodePackedSize(data, pos, sizeBits);
            if (arrayFlag) {
                uint8_t b = data[pos++];
                if ((b & 0xC0) == 0x80) pos++;
                else if ((b & 0xC0) == 0xC0) pos += 3;
            }
            p.valueOffset = pos;
            pos += p.size;
        }
        props.push_back(p);
    }
    return props;
}

// Known UE2 actor property names — used to detect where property serialization starts
static bool IsKnownPropertyName(const std::string& s)
{
    static const char* known[] = {
        "Tag","Group","Event","Location","Rotation","DrawScale","DrawScale3D",
        "CollisionRadius","CollisionHeight","bCollideActors","bCollideWorld",
        "bBlockActors","bBlockPlayers","bHidden","bStatic","bNoDelete",
        "Physics","DrawType","AmbientGlow","bShadowCast","StaticMesh","Mesh",
        "StaticMeshComponent","Texture","Skins","LightBrightness","LightColor",
        "LightHue","LightRadius","LightSaturation","LightType","LightEffect",
        "bDirectionalCorona","bCorona","Level","Region","PhysicsVolume",
        "Label","RelativeLocation","RelativeRotation","Base","Owner",
        "bStasis","bWorldGeometry","bAcceptsProjectors","bLightChanged",
        "bDeleteMe","bDisableTick","bNoSmooth","Mass","Buoyancy",
        "LifeSpan","InitialState","NetPriority","bAlwaysRelevant",
        "bNetTemporary","RemoteRole","Role","NetUpdateFrequency",
        "PrePivot","bUseDynamicLights","bUnlit","MaxLights",
        "ScaleGlow","bMovable","LightEffect","bSpecialLit",
        "AmbientSound","SoundRadius","SoundVolume","SoundPitch",
        "ForcedVisibilityZoneTag","bForceStaticLighting",
        // StaticMesh-export properties (so DetectHeaderSkip anchors on the mesh prop block)
        "Materials","UseSimpleLineCollision","UseSimpleBoxCollision",
        "UseSimpleKarmaCollision","UseVertexColor","InternalVersion",
    };
    for (auto& k : known)
        if (s == k) return true;
    return false;
}

static int DetectHeaderSkip(const uint8_t* data, size_t offset, int size,
                            const std::vector<std::string>& names)
{
    // Strategy: scan byte-by-byte for the start of property serialization.
    // A valid property starts with a compact index (name index) followed by a 4-byte number,
    // then an info byte. We look for positions where:
    //   1. Reading a compact index gives a valid name table entry
    //   2. That name is a known UE2 property name
    //   3. Parsing properties from that offset yields multiple valid properties
    int maxScan = std::min(size - 5, 200); // Extend to 200 bytes to handle larger headers
    int bestSkip = -1;
    int bestScore = 0;

    for (int skip = 0; skip < maxScan; skip++) {
        size_t br = 0;
        int idx = ReadCompactIndex(data + offset + skip, size - skip, br);
        if (idx <= 0 || idx >= (int)names.size()) continue;
        if (br > 3) continue; // compact index shouldn't be huge for property names

        // Check if this name is a known property
        if (!IsKnownPropertyName(names[idx])) continue;

        // Found a candidate — verify by parsing properties from here
        auto props = ParsePropsMinimal(data, offset + skip, names, offset + size);
        int score = 0;
        for (auto& p : props) {
            if (IsKnownPropertyName(p.name)) score += 2;
            else if (p.name.size() > 1 && p.name.size() < 50) score++; // valid-looking name
        }
        if (score > bestScore) {
            bestScore = score;
            bestSkip = skip;
            if (score >= 6) break; // confident enough
        }
    }

    // Fallback: if no good candidate, try the classic 4-80 range
    if (bestSkip < 0 || bestScore < 2) {
        for (int skip = 4; skip < std::min(size - 5, 120); skip++) {
            auto props = ParsePropsMinimal(data, offset + skip, names, offset + size);
            if ((int)props.size() >= 3) {
                int score = 0;
                for (auto& p : props)
                    if (p.name.size() > 1 && p.name.size() < 50 &&
                        p.size > 0 && p.size < 10000) score++;
                if (score > bestScore) { bestScore = score; bestSkip = skip; }
            }
        }
    }

    return (bestSkip >= 0) ? bestSkip : 57;
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
        int32_t outerIdx = *reinterpret_cast<int32_t*>(d + pos); pos += 4; // outerIndex
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
            // export ref — local class defined in this package
            int idx = classIdx - 1; // 1-based to 0-based
            if (idx < (int)m_ParsedExports.size())
                className = m_ParsedExports[idx].objectName;
            else
                className = "ExportClass";
        }

        // Resolve object name
        std::string objName = (objNameIdx >= 0 && objNameIdx < (int)names.size()) ? names[objNameIdx] : "?";
        if (objNameNum >= 0) objName += "_" + std::to_string(objNameNum);

        ParsedExport pe;
        pe.classIndex = classIdx;
        pe.outerIndex = outerIdx;
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
            "StaticMesh", "Mesh", "StaticMeshComponent", "StaticMeshInstance",
            "PickupMesh", "ThirdPersonMesh", "PickupViewMesh",
        };
        bool isSMActor = (pe.className == "StaticMeshActor");
        for (auto& propName : meshPropNames) {
            if (actor.meshIndex >= 0) break; // already found one
            for (auto& p : props) {
                if (actor.meshIndex >= 0) break;
                if (p.name != propName) continue;
                // Object references are compact-index encoded; typically 1-5 bytes
                if (p.size < 1 || p.size > 8) continue;
                size_t vpos = 0;
                size_t remaining = p.size;
                int ref = ReadCompactIndex(d + p.valueOffset, remaining, vpos);
                if (ref > 0 && ref <= (int)m_ParsedExports.size()) {
                    auto& refExport = m_ParsedExports[ref - 1];
                    if (actor.meshRefName.empty()) actor.meshRefName = refExport.objectName;
                    if (refExport.className == "StaticMesh") {
                        actor.meshIndex = ref - 1;
                    } else if (refExport.className == "StaticMeshInstance") {
                        // StaticMeshInstance uses binary serialization — resolve via outer chain
                        int curIdx = ref - 1; // 0-based
                        bool found = false;
                        for (int depth = 0; depth < 5; depth++) {
                            int outer = m_ParsedExports[curIdx].outerIndex;
                            if (outer > 0 && outer <= (int)m_ParsedExports.size()) {
                                if (m_ParsedExports[outer - 1].className == "StaticMesh") {
                                    actor.meshIndex = outer - 1;
                                    actor.meshRefName = m_ParsedExports[outer - 1].objectName;
                                    found = true;
                                    break;
                                }
                                curIdx = outer - 1;
                            } else break;
                        }
                        // If outer chain didn't find it, scan binary data for StaticMesh references
                        if (!found) {
                            int scanLen = refExport.serialSize;
                            // Try compact index at every byte offset
                            for (int off = 0; off < scanLen - 2 && !found; off++) {
                                size_t tbr;
                                int tv = ReadCompactIndex(d + refExport.serialOffset + off, scanLen - off, tbr);
                                if (tv > 0 && tv <= (int)m_ParsedExports.size() &&
                                    m_ParsedExports[tv - 1].className == "StaticMesh") {
                                    actor.meshIndex = tv - 1;
                                    actor.meshRefName = m_ParsedExports[tv - 1].objectName;
                                    found = true;
                                }
                            }
                        }
                    } else if (refExport.className == "StaticMeshComponent" ||
                               refExport.className == "StaticMeshActor") {
                        // Follow indirect: parse the component's properties to find its StaticMesh
                        if (refExport.serialOffset + refExport.serialSize <= (int)fileSize) {
                            int compSkip = DetectHeaderSkip(d, refExport.serialOffset, refExport.serialSize, names);
                            auto compProps = ParsePropsMinimal(d, refExport.serialOffset + compSkip, names,
                                                               refExport.serialOffset + refExport.serialSize);
                            for (auto& cp : compProps) {
                                if (cp.name == "StaticMesh" && cp.size >= 1 && cp.size <= 8) {
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
                } else if (ref < 0) {
                    // Negative ref = import reference (external package mesh)
                    int impIdx = -ref - 1;
                    if (impIdx < (int)imports.size()) {
                        actor.meshRefName = imports[impIdx].objectName;
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

    // Resolve a package object ref (>0 export, <0 import) to its class + object name.
    auto resolveRef = [&](int ref, std::string& cls, std::string& nm) {
        if (ref > 0 && ref <= (int)m_ParsedExports.size()) {
            cls = m_ParsedExports[ref - 1].className;
            nm  = m_ParsedExports[ref - 1].objectName;
        } else if (ref < 0 && (-ref - 1) < (int)imports.size()) {
            cls = imports[-ref - 1].className;
            nm  = imports[-ref - 1].objectName;
        }
    };
    auto isMaterialClass = [](const std::string& c) -> bool {
        return c.find("Shader") != std::string::npos ||
               c.find("Material") != std::string::npos ||
               c.find("Modifier") != std::string::npos ||
               (c.size() >= 3 && c.compare(0, 3, "Tex") == 0) ||
               c == "FinalBlend" || c == "Combiner" || c == "Texture" ||
               c == "VertexColor" || c == "ConstantColor" || c == "ColorMaterial";
    };
    // Extract the first material/shader name from a StaticMesh export's "Materials"
    // tagged property — the authoritative binding straight from the .bsm. Recovers
    // meshes where UModel wrote a "dummy_material" placeholder into the glTF.
    auto extractBSMMaterial = [&](size_t soff, size_t ssize) -> std::string {
        if (ssize < 12 || soff + ssize > (size_t)fileSize) return "";
        int hdrSkip = DetectHeaderSkip(d, soff, (int)ssize, names);
        auto props = ParsePropsMinimal(d, soff + (size_t)hdrSkip, names, soff + ssize);
        for (auto& p : props) {
            if (p.name != "Materials" || p.size < 2) continue;
            // The Materials array element layout (native StaticMeshMaterial struct) varies,
            // so scan the whole payload for the first object ref that resolves to a material
            // class — robust to EnableCollision flags / struct framing in front of it.
            size_t scanEnd = p.valueOffset + (size_t)p.size;
            if (scanEnd > soff + ssize) scanEnd = soff + ssize;
            for (size_t sp = p.valueOffset; sp + 1 < scanEnd; sp++) {
                size_t br;
                int ref = ReadCompactIndex(d + sp, scanEnd - sp, br);
                if (ref == 0) continue;
                std::string cls, nm;
                resolveRef(ref, cls, nm);
                if (!nm.empty() && isMaterialClass(cls)) return nm;
            }
        }
        return "";
    };

    int meshesFound = 0, meshesParsed = 0, meshesWithBSMMat = 0;
    for (int i = 0; i < (int)m_ParsedExports.size(); i++) {
        auto& pe = m_ParsedExports[i];
        if (pe.className != "StaticMesh") continue;
        if (pe.serialSize < 100) continue;
        if (pe.serialOffset + pe.serialSize > (int)fileSize) continue;
        meshesFound++;
        ParsedMesh mesh = ParseStaticMesh(d + pe.serialOffset, pe.serialSize, names);
        if (mesh.valid) {
            mesh.name = pe.objectName;
            mesh.bsmMaterialName = extractBSMMaterial((size_t)pe.serialOffset, (size_t)pe.serialSize);
            if (!mesh.bsmMaterialName.empty()) meshesWithBSMMat++;
            m_MeshNameToIndex[pe.objectName] = (int)m_Meshes.size();
            m_Meshes.push_back(std::move(mesh));
            meshesParsed++;
        }
    }
    printf("[BSM] StaticMesh Materials: %d/%d meshes carry a BSM material ref\n",
           meshesWithBSMMat, meshesParsed);
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

                // Build lightmap texture name list before BSP parse (iLightMap uses 1-based index)
                m_LightMapNames.clear();
                for (int li = 0; li < (int)m_ParsedExports.size(); li++) {
                    auto& lpe = m_ParsedExports[li];
                    if (lpe.className == "Texture" && lpe.objectName.substr(0, 7) == "Texture"
                        && lpe.objectName.size() <= 10)
                        m_LightMapNames.push_back(lpe.objectName);
                }

                std::vector<BSPTreeNodeOut> treeOut;
                m_BSPMeshes = ParseBSPGeometry(d + pe.serialOffset, pe.serialSize, names,
                                                bspExports, importNames, &treeOut,
                                                &m_LightMapNames);
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

    // ─── Lightmap texture exports diagnostic ───
    {
        int texCount = 0;
        int lmTexCount = 0;
        for (int i = 0; i < (int)m_ParsedExports.size(); i++) {
            auto& pe = m_ParsedExports[i];
            if (pe.className != "Texture" && pe.className != "ShadowMap") continue;
            texCount++;
            // Lightmap textures are named "TextureNN" with small serial (stripped bulk data)
            if (pe.objectName.substr(0, 7) == "Texture" && pe.objectName.size() <= 10) {
                lmTexCount++;
                if (lmTexCount <= 5)
                    printf("[BSM-LM] Lightmap Texture export[%d]: '%s' serial=%d bytes at 0x%X\n",
                           i, pe.objectName.c_str(), pe.serialSize, pe.serialOffset);
            }
        }
        printf("[BSM-LM] Total Texture exports: %d, lightmap candidates: %d\n", texCount, lmTexCount);

        // Build lightmap texture name list (index 1=first, etc.)
        m_LightMapNames.clear();
        for (int i = 0; i < (int)m_ParsedExports.size(); i++) {
            auto& pe = m_ParsedExports[i];
            if (pe.className != "Texture") continue;
            if (pe.objectName.substr(0, 7) != "Texture" || pe.objectName.size() > 10) continue;
            m_LightMapNames.push_back(pe.objectName);
            printf("[BSM-LM] LM atlas %d → '%s'\n", (int)m_LightMapNames.size(), pe.objectName.c_str());
        }
    }

    // Load supplementary glTF meshes from UEViewer exports (adds meshes not in BSM)
    // First load from current map, then cross-map fallback from all other map exports
    {
        std::string gltfMapName = m_MapName;
        { size_t dp = gltfMapName.find_last_of('.'); if (dp != std::string::npos) gltfMapName = gltfMapName.substr(0, dp); }
        std::string exportRoot = "Z:\\UEViewer\\export";
        std::string exportDir = exportRoot + "\\" + gltfMapName;
        auto gltfMeshes = LoadAllMeshesFromExportDir(exportDir);
        int added = 0;
        for (auto& gm : gltfMeshes) {
            if (m_MeshNameToIndex.find(gm.name) == m_MeshNameToIndex.end()) {
                m_MeshNameToIndex[gm.name] = (int)m_Meshes.size();
                m_Meshes.push_back(std::move(gm));
                added++;
            }
        }
        printf("[BSM] Supplementary glTF: %d loaded, %d new (total meshes: %d)\n",
               (int)gltfMeshes.size(), added, (int)m_Meshes.size());

        // Cross-map mesh search: scan all other map export dirs for meshes we don't have yet
        namespace fs = std::filesystem;
        int crossMapAdded = 0;
        if (fs::is_directory(exportRoot)) {
            for (auto& entry : fs::directory_iterator(exportRoot)) {
                if (!entry.is_directory()) continue;
                std::string dirName = entry.path().filename().string();
                if (dirName == gltfMapName || dirName == "_BulkTextures") continue;
                std::vector<ParsedMesh> crossMeshes = LoadAllMeshesFromExportDir(entry.path().string());
                for (size_t mi = 0; mi < crossMeshes.size(); mi++) {
                    if (m_MeshNameToIndex.find(crossMeshes[mi].name) == m_MeshNameToIndex.end()) {
                        m_MeshNameToIndex[crossMeshes[mi].name] = (int)m_Meshes.size();
                        m_Meshes.push_back(std::move(crossMeshes[mi]));
                        crossMapAdded++;
                    }
                }
            }
            printf("[BSM] Cross-map mesh search: %d additional meshes (total: %d)\n",
                   crossMapAdded, (int)m_Meshes.size());
        }
    }

    // Build normalized name map for fuzzy matching (strip all underscores, lowercase)
    auto normalizeName = [](const std::string& s) -> std::string {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (c == '_') continue; // strip all underscores
            r += (c >= 'A' && c <= 'Z') ? (c + 32) : c;
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
                        printf("[BSM] Unmatched export mesh: '%s' (norm: '%s')\n",
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

    // Second pass: link import-referenced actors by meshRefName against glTF pool
    int importLinked = 0, importUnmatched = 0;
    int importUnmatchedLogged = 0;
    for (auto& actor : m_Actors) {
        if (actor.meshIndex >= 0) continue; // already linked
        if (actor.meshRefName.empty()) continue; // no reference at all
        // Try exact match
        auto it = m_MeshNameToIndex.find(actor.meshRefName);
        if (it != m_MeshNameToIndex.end()) {
            actor.meshIndex = it->second;
            importLinked++;
            continue;
        }
        // Try normalized match
        auto it2 = normalizedNameMap.find(normalizeName(actor.meshRefName));
        if (it2 != normalizedNameMap.end()) {
            actor.meshIndex = it2->second;
            importLinked++;
            continue;
        }
        if (importUnmatchedLogged < 15) {
            printf("[BSM] Unmatched import mesh: '%s' (class: %s)\n",
                   actor.meshRefName.c_str(), actor.className.c_str());
            importUnmatchedLogged++;
        }
        importUnmatched++;
    }
    printf("[BSM] Linked %d actors to mesh geometry (%d no ref, %d bad ref, %d unmatched)\n", linked, noRef, badRef, noMatch);
    printf("[BSM] Import mesh linking: %d linked, %d unmatched\n", importLinked, importUnmatched);

    // Third pass: class-to-default-mesh mapping for gameplay classes
    // These classes don't store per-instance mesh refs — the mesh is defined in class defaults
    static const struct { const char* className; const char* meshName; } classDefaults[] = {
        // Pickups
        {"MedHypoPickup",              "Health"},
        {"MedKitPickup",               "Health"},
        {"EVEHypoPickup",              "eve_hypo_ad"},
        {"PlasmidPickup",              "plasmid_pickup"},
        {"AmmoPickup",                 "Pickup"},
        {"WeaponPickup",               "Pickup"},
        // Gameplay stations (VendingWide mesh not in BSM, use ResStationBody as placeholder)
        {"PlaceableVendingStation",    "ResStationBody"},
        {"PlaceableVendingStationAlt", "ResStationBody"},
        {"PlaceableHealthStation",     "ResStationBody"},
        {"DoorKeypadControl",          "Pickup"},
        {"Lockbox",                    "Pickup"},
        // Spawners
        {"TurretSpawner",              "Turret_Cover"},
        {"SecurityCameraSpawner",      "SmCamWallBase"},
        {"SecurityBotSpawner",         "securytybot"},
        {"AggressorSpawner",           "securytybot"},
        // Containers
        {"Container",                  "Pickup"},
        {"DeadBodyContainer",          "Pickup"},
        {"FlowerVaseContainer",        "flower_vase"},
    };
    int defaultLinked = 0;
    for (auto& actor : m_Actors) {
        if (actor.meshIndex >= 0) continue;
        if (!actor.hasLocation) continue;
        for (auto& [cls, meshName] : classDefaults) {
            if (actor.className != cls) continue;
            auto it = m_MeshNameToIndex.find(meshName);
            if (it != m_MeshNameToIndex.end()) {
                actor.meshIndex = it->second;
                if (actor.meshRefName.empty()) actor.meshRefName = meshName;
                defaultLinked++;
            }
            break;
        }
    }
    if (defaultLinked > 0)
        printf("[BSM] Class-default mesh linking: %d actors\n", defaultLinked);

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

    // Pickups / weapons / items / containers / stations
    if (cn.find("Pickup") != std::string::npos ||
        cn.find("Weapon") != std::string::npos ||
        cn.find("Ammo") != std::string::npos ||
        cn.find("Item") != std::string::npos ||
        cn.find("Plasmid") != std::string::npos ||
        cn.find("Tonic") != std::string::npos ||
        cn.find("Gene") != std::string::npos ||
        cn.find("Container") != std::string::npos ||
        cn.find("Lockbox") != std::string::npos ||
        cn.find("Keypad") != std::string::npos ||
        cn.find("HealthStation") != std::string::npos ||
        cn.find("VendingStation") != std::string::npos ||
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

    // Spawners / AI enemies — render with placeholder mesh if available, else editor-only
    if (cn.find("Spawner") != std::string::npos ||
        cn.find("Spawn") != std::string::npos ||
        cn.find("BigDaddy") != std::string::npos ||
        cn.find("Splicer") != std::string::npos ||
        cn.find("Turret") != std::string::npos ||
        cn.find("Security") != std::string::npos ||
        cn.find("Enemy") != std::string::npos ||
        cn.find("NPC") != std::string::npos ||
        cn.find("Bot") != std::string::npos)
        return hasMesh ? ActorRenderType::VisibleDecoration : ActorRenderType::EditorOnly;

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

    // Normalize a shader/material name to a lookup key (strip trailing _shader/_Shader)
    auto stripShaderSuffix = [](std::string n) -> std::string {
        if (n.size() > 7) {
            std::string tail = n.substr(n.size() - 7);
            if (tail == "_shader" || tail == "_Shader") return n.substr(0, n.size() - 7);
        }
        return n;
    };
    // Parse a Diffuse value: handles both "0451_diffuse" (.mat) and
    // "Texture'Res_Decor.4_poster_bed_d'" (.props.txt) → returns bare texture name.
    auto parseDiffuseValue = [](std::string v) -> std::string {
        // Trim leading/trailing whitespace
        size_t a = v.find_first_not_of(" \t\r\n");
        size_t b = v.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        v = v.substr(a, b - a + 1);
        // Unwrap Texture'Pkg.name' form
        size_t q1 = v.find('\'');
        if (q1 != std::string::npos) {
            size_t q2 = v.find('\'', q1 + 1);
            if (q2 != std::string::npos) {
                std::string inner = v.substr(q1 + 1, q2 - q1 - 1); // Pkg.name
                size_t dot = inner.find_last_of('.');
                return dot != std::string::npos ? inner.substr(dot + 1) : inner;
            }
        }
        return v;
    };

    // Step 1: Load shader→diffuseTexture map from .mat AND .props.txt files.
    // Keys are normalized (no _shader suffix) so glTF material names match either source.
    std::unordered_map<std::string, std::string> shaderToDiffuse;
    if (fs::is_directory(shaderDir)) {
        for (auto& entry : fs::directory_iterator(shaderDir)) {
            std::string fname = entry.path().filename().string();
            std::string stem;
            bool isProps = false;
            if (entry.path().extension() == ".mat") {
                stem = entry.path().stem().string();
            } else if (fname.size() > 10 && fname.substr(fname.size() - 10) == ".props.txt") {
                stem = fname.substr(0, fname.size() - 10);
                isProps = true;
            } else {
                continue;
            }
            std::string key = stripShaderSuffix(stem);
            // .mat is concise; prefer it. Only let .props fill gaps.
            if (isProps && shaderToDiffuse.count(key)) continue;
            std::ifstream f(entry.path());
            std::string line;
            while (std::getline(f, line)) {
                // .mat:  "Diffuse=..."   .props.txt:  "Diffuse = Texture'...'"
                size_t dpos = line.find("Diffuse");
                if (dpos != 0) continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string diff = parseDiffuseValue(line.substr(eq + 1));
                if (!diff.empty() && diff != "None") {
                    shaderToDiffuse[key] = diff;
                }
                break;
            }
        }
    }

    // Step 2: Resolve each mesh's diffuse texture.
    // Primary source = the material named in the per-mesh glTF; authoritative fallback
    // = the Materials ref read straight from the .bsm (recovers dummy/missing glTF mats).
    std::string texDir = umodelExportDir + "\\Texture";

    // Build cross-map texture dirs for fallback resolution
    std::vector<std::string> allTexDirs;
    allTexDirs.push_back(texDir);
    {
        fs::path exportRoot = fs::path(umodelExportDir).parent_path();
        if (fs::is_directory(exportRoot)) {
            for (auto& entry : fs::directory_iterator(exportRoot)) {
                if (!entry.is_directory()) continue;
                std::string td = entry.path().string() + "\\Texture";
                if (fs::is_directory(td) && td != texDir)
                    allTexDirs.push_back(td);
            }
        }
        printf("[BSM] Cross-map texture search: %d directories\n", (int)allTexDirs.size());
    }

    // Check if a TGA exists in any of our texture directories
    auto findTGA = [&](const std::string& name) -> bool {
        for (auto& dir : allTexDirs)
            if (fs::exists(dir + "\\" + name + ".tga")) return true;
        return false;
    };

    // Strip the _shader suffix off a material name and resolve to a texture that exists
    // (direct .tga match -> .mat/.props Diffuse= mapping -> best-guess passthrough).
    auto matToTexName = [&](std::string m) -> std::string {
        if (m.empty()) return "";
        if (m.size() > 7) {
            std::string suf = m.substr(m.size() - 7);
            if (suf == "_shader" || suf == "_Shader") m = m.substr(0, m.size() - 7);
        }
        if (findTGA(m)) return m;
        auto it = shaderToDiffuse.find(m);
        if (it != shaderToDiffuse.end() && findTGA(it->second))
            return it->second;
        return m;
    };

    int resolved = 0, resolvedFromBSM = 0;
    for (auto& mesh : m_Meshes) {
        if (mesh.name.empty()) continue;

        // Primary: first non-dummy material name in the per-mesh glTF JSON.
        std::string matName;
        std::ifstream f(meshDir + "\\" + mesh.name + ".gltf");
        if (f.is_open()) {
            std::string line;
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
                            if (candidate.find("dummy_material") == std::string::npos &&
                                candidate != "None" && !candidate.empty()) {
                                matName = candidate;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Authoritative fallback straight from the .bsm Materials array.
        bool fromBSM = false;
        if (matName.empty() && !mesh.bsmMaterialName.empty()) {
            matName = mesh.bsmMaterialName;
            fromBSM = true;
        }
        if (matName.empty()) continue;

        mesh.textureName = matToTexName(matName);
        // If the glTF-derived texture is absent on disk but the authoritative BSM
        // material resolves to a real TGA, prefer the BSM binding.
        if (!fromBSM && !mesh.bsmMaterialName.empty() &&
            !findTGA(mesh.textureName)) {
            std::string bsmTex = matToTexName(mesh.bsmMaterialName);
            if (!bsmTex.empty() && findTGA(bsmTex)) {
                mesh.textureName = bsmTex;
                fromBSM = true;
            }
        }
        if (fromBSM) resolvedFromBSM++;
        resolved++;
    }
    printf("[BSM] Resolved %d/%d mesh texture names (%d via BSM Materials fallback)\n",
           resolved, (int)m_Meshes.size(), resolvedFromBSM);

    // Diagnostic: report meshes whose final texture name has no TGA on disk (any map)
    {
        int noName = 0, noFile = 0;
        std::ofstream dump("mesh_tex_missing.txt");
        for (auto& mesh : m_Meshes) {
            if (mesh.name.empty()) continue;
            if (mesh.textureName.empty()) {
                noName++;
                if (dump) dump << "NO-MAT\t" << mesh.name << "\tbsm='" << mesh.bsmMaterialName << "'\n";
                continue;
            }
            if (!findTGA(mesh.textureName)) {
                noFile++;
                if (dump) dump << "NO-TGA\t" << mesh.name << "\ttex='" << mesh.textureName
                               << "'\tbsm='" << mesh.bsmMaterialName << "'\n";
            }
        }
        printf("[MESH-TEX] untextured breakdown: %d no-material, %d missing-TGA "
               "(full list -> mesh_tex_missing.txt)\n", noName, noFile);
    }

    // Step 3: Resolve BSP chunk texture names (already set by BSP parser from surf Material refs)
    int bspResolved = 0;
    for (auto& chunk : m_BSPMeshes) {
        if (chunk.textureName.empty()) continue;
        std::string candidate = chunk.textureName;

        // Check if it directly matches a TGA (any map)
        if (findTGA(candidate)) {
            bspResolved++;
            continue;
        }

        // Shader→diffuse lookup. The map is keyed by normalized (stripped) shader
        // names, and chunk.textureName was already stripped by the BSP parser.
        auto it = shaderToDiffuse.find(candidate);
        if (it != shaderToDiffuse.end() && findTGA(it->second)) {
            chunk.textureName = it->second;
            bspResolved++;
        } else if (findTGA(candidate)) {
            bspResolved++;
        }
        // else leave textureName as-is; TextureCache will try suffixes
    }
    // Diagnostic: show first 10 unresolved BSP texture names
    int unresLog = 0;
    for (auto& chunk : m_BSPMeshes) {
        if (chunk.textureName.empty()) continue;
        if (!findTGA(chunk.textureName) && unresLog < 10) {
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
