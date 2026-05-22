#include "nearby_scanner.h"
#include "coop_debug.h"
#include "../engine/uobject.h"
#include "../engine/world.h"
#include "../core/log.h"

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>

namespace bs1sdk {

// ─── Helpers ────────────────────────────────────────────────────────────────

static void EnsureDumpDir()
{
    CreateDirectoryA(GetDebugDir(), NULL);
}

// ─── Configuration ──────────────────────────────────────────────────────────

static float s_ScanRadius = 3000.0f;

void SetScanRadius(float radius) { s_ScanRadius = radius; }
float GetScanRadius() { return s_ScanRadius; }

// ─── Property Reading Helpers ───────────────────────────────────────────────

struct FVector { float X, Y, Z; };
struct FRotator { int Pitch, Yaw, Roll; }; // UE2 rotator = int32 units (65536 = 360 deg)

static bool ReadVectorProp(UObject* actor, const char* name, FVector& out)
{
    return GetActorProperty(actor, name, &out, sizeof(FVector));
}

static bool ReadRotatorProp(UObject* actor, const char* name, FRotator& out)
{
    return GetActorProperty(actor, name, &out, sizeof(FRotator));
}

static bool ReadFloatProp(UObject* actor, const char* name, float& out)
{
    return GetActorProperty(actor, name, &out, sizeof(float));
}

static bool ReadIntProp(UObject* actor, const char* name, int32_t& out)
{
    return GetActorProperty(actor, name, &out, sizeof(int32_t));
}

static bool ReadBoolProp(UObject* actor, const char* name, bool& out)
{
    int32_t val = 0;
    if (!GetActorProperty(actor, name, &val, sizeof(int32_t))) return false;
    out = (val != 0);
    return true;
}

static bool ReadByteProp(UObject* actor, const char* name, uint8_t& out)
{
    return GetActorProperty(actor, name, &out, sizeof(uint8_t));
}

// Pointer safety check
static bool IsValidPtr(const void* ptr)
{
    if (!ptr) return false;
    return !IsBadReadPtr(ptr, 4);
}

// Safe object access wrappers
static bool SafeGetName(UObject* obj, char* buf, int bufSize)
{
    if (!IsValidPtr(obj)) { strncpy(buf, "<null>", bufSize - 1); return false; }
    try {
        std::string n = obj->GetName();
        strncpy(buf, n.c_str(), bufSize - 1);
        buf[bufSize - 1] = '\0';
        return true;
    } catch (...) {
        strncpy(buf, "<invalid_ptr>", bufSize - 1);
        return false;
    }
}

static bool SafeGetFullPath(UObject* obj, char* buf, int bufSize)
{
    if (!IsValidPtr(obj)) { strncpy(buf, "<null>", bufSize - 1); return false; }
    try {
        std::string p = obj->GetFullPath();
        strncpy(buf, p.c_str(), bufSize - 1);
        buf[bufSize - 1] = '\0';
        return true;
    } catch (...) {
        strncpy(buf, "<invalid_ptr>", bufSize - 1);
        return false;
    }
}

static bool SafeGetOuter(UObject* obj, UObject** out)
{
    if (!IsValidPtr(obj)) { *out = nullptr; return false; }
    try {
        *out = obj->GetOuter();
        return true;
    } catch (...) {
        *out = nullptr;
        return false;
    }
}

static bool SafeGetClassName(UObject* obj, char* buf, int bufSize)
{
    if (!IsValidPtr(obj)) { strncpy(buf, "<null>", bufSize - 1); return false; }
    try {
        std::string cn = obj->GetObjClassName();
        strncpy(buf, cn.c_str(), bufSize - 1);
        buf[bufSize - 1] = '\0';
        return true;
    } catch (...) {
        strncpy(buf, "<error>", bufSize - 1);
        return false;
    }
}

// Read an object reference property and return the object's name
static std::string ReadObjRefName(UObject* actor, const char* propName)
{
    UObject* ref = nullptr;
    if (!GetActorProperty(actor, propName, &ref, sizeof(UObject*))) return "";
    if (!ref) return "";
    char buf[256];
    SafeGetName(ref, buf, sizeof(buf));
    return buf;
}

// Read an object reference property and return full path
static std::string ReadObjRefPath(UObject* actor, const char* propName)
{
    UObject* ref = nullptr;
    if (!GetActorProperty(actor, propName, &ref, sizeof(UObject*))) return "";
    if (!ref) return "";
    char buf[512];
    SafeGetFullPath(ref, buf, sizeof(buf));
    return buf;
}

// Build full outer chain for an object: Package.Group.Name
static std::string GetOuterChain(UObject* obj)
{
    if (!obj) return "";
    std::string result;
    std::vector<std::string> parts;
    UObject* cur = obj;
    int depth = 0;
    while (cur && depth < 20) {
        char nameBuf[256];
        if (!SafeGetName(cur, nameBuf, sizeof(nameBuf))) break;
        parts.push_back(nameBuf);
        UObject* outer = nullptr;
        if (!SafeGetOuter(cur, &outer)) break;
        cur = outer;
        depth++;
    }
    for (int i = (int)parts.size() - 1; i >= 0; i--) {
        if (!result.empty()) result += ".";
        result += parts[i];
    }
    return result;
}

// ─── JSON Helpers ───────────────────────────────────────────────────────────

static std::string EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

// ─── Main Scanner ───────────────────────────────────────────────────────────

int ExecuteNearbyScan()
{
    if (!IsWorldSystemReady()) return -1;

    FVec3 playerPos;
    if (!GetPlayerPosition(playerPos)) return -1;

    auto level = GetCurrentLevel();
    auto allActors = GetActorsInRadius(playerPos, s_ScanRadius);

    // Sort by distance
    struct ActorDist { UObject* actor; float dist; };
    std::vector<ActorDist> sorted;
    sorted.reserve(allActors.size());
    for (auto* a : allActors) {
        FVec3 aPos;
        float dist = 0;
        if (GetActorPosition(a, aPos)) {
            float dx = aPos.X - playerPos.X;
            float dy = aPos.Y - playerPos.Y;
            float dz = aPos.Z - playerPos.Z;
            dist = sqrtf(dx*dx + dy*dy + dz*dz);
        }
        sorted.push_back({a, dist});
    }
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.dist < b.dist; });

    // Build output path
    EnsureDumpDir();
    char outPath[MAX_PATH];
    snprintf(outPath, sizeof(outPath), "%s\\runtime_nearby_actors.json", GetDebugDir());

    std::ofstream f(outPath);
    if (!f.is_open()) return -1;

    // Header
    f << "{\n";
    f << "  \"mapName\": \"" << EscapeJson(level.LevelName) << "\",\n";
    f << "  \"playerLocation\": [" << playerPos.X << ", " << playerPos.Y << ", " << playerPos.Z << "],\n";
    f << "  \"scanRadius\": " << s_ScanRadius << ",\n";
    f << "  \"actorCount\": " << sorted.size() << ",\n";

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt; localtime_s(&lt, &t);
    char tsBuf[64];
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    f << "  \"timestamp\": \"" << tsBuf << "\",\n";
    f << "  \"actors\": [\n";

    int exported = 0;
    for (int idx = 0; idx < (int)sorted.size(); idx++) {
        auto* actor = sorted[idx].actor;
        if (!actor) continue;

        char classNameBuf[256], actorNameBuf[256];
        if (!SafeGetClassName(actor, classNameBuf, sizeof(classNameBuf))) continue;
        if (!SafeGetName(actor, actorNameBuf, sizeof(actorNameBuf))) continue;
        std::string className(classNameBuf);
        std::string actorName(actorNameBuf);
        std::string fullPath = GetOuterChain(actor);

        if (exported > 0) f << ",\n";
        f << "    {\n";
        f << "      \"name\": \"" << EscapeJson(actorName) << "\",\n";
        f << "      \"class\": \"" << EscapeJson(className) << "\",\n";
        f << "      \"fullPath\": \"" << EscapeJson(fullPath) << "\",\n";
        f << "      \"distance\": " << sorted[idx].dist << ",\n";

        // Location
        FVector loc = {0, 0, 0};
        ReadVectorProp(actor, "Location", loc);
        f << "      \"location\": [" << loc.X << ", " << loc.Y << ", " << loc.Z << "],\n";

        // Rotation (convert from Unreal units to degrees)
        FRotator rot = {0, 0, 0};
        ReadRotatorProp(actor, "Rotation", rot);
        float pitchDeg = rot.Pitch * (360.0f / 65536.0f);
        float yawDeg = rot.Yaw * (360.0f / 65536.0f);
        float rollDeg = rot.Roll * (360.0f / 65536.0f);
        f << "      \"rotation\": [" << pitchDeg << ", " << yawDeg << ", " << rollDeg << "],\n";

        // DrawScale / DrawScale3D
        float drawScale = 1.0f;
        ReadFloatProp(actor, "DrawScale", drawScale);
        f << "      \"drawScale\": " << drawScale << ",\n";

        FVector drawScale3D = {1, 1, 1};
        ReadVectorProp(actor, "DrawScale3D", drawScale3D);
        f << "      \"drawScale3D\": [" << drawScale3D.X << ", " << drawScale3D.Y << ", " << drawScale3D.Z << "],\n";

        // Visibility flags
        bool bHidden = false, bDeleteMe = false;
        ReadBoolProp(actor, "bHidden", bHidden);
        ReadBoolProp(actor, "bDeleteMe", bDeleteMe);
        f << "      \"bHidden\": " << (bHidden ? "true" : "false") << ",\n";
        f << "      \"bDeleteMe\": " << (bDeleteMe ? "true" : "false") << ",\n";

        // DrawType (enum — byte value)
        uint8_t drawType = 0;
        ReadByteProp(actor, "DrawType", drawType);
        const char* drawTypeNames[] = {"DT_None","DT_Sprite","DT_Mesh","DT_Brush",
                                        "DT_RopeSprite","DT_VerticalSprite","DT_Terraform",
                                        "DT_SpriteAnimOnce","DT_StaticMesh","DT_DrawType",
                                        "DT_Particle","DT_AntiPortal","DT_FluidSurface"};
        const char* dtName = (drawType < 13) ? drawTypeNames[drawType] : "Unknown";
        f << "      \"drawType\": \"" << dtName << "\",\n";
        f << "      \"drawTypeValue\": " << (int)drawType << ",\n";

        // Mesh references
        std::string staticMeshRef = ReadObjRefPath(actor, "StaticMesh");
        std::string meshRef = ReadObjRefPath(actor, "Mesh");
        f << "      \"staticMesh\": \"" << EscapeJson(staticMeshRef) << "\",\n";
        f << "      \"mesh\": \"" << EscapeJson(meshRef) << "\",\n";

        // Collision
        float collisionRadius = 0, collisionHeight = 0;
        ReadFloatProp(actor, "CollisionRadius", collisionRadius);
        ReadFloatProp(actor, "CollisionHeight", collisionHeight);
        f << "      \"collisionRadius\": " << collisionRadius << ",\n";
        f << "      \"collisionHeight\": " << collisionHeight << ",\n";

        // Owner / Base / AttachParent
        std::string ownerRef = ReadObjRefName(actor, "Owner");
        std::string baseRef = ReadObjRefName(actor, "Base");
        f << "      \"owner\": \"" << EscapeJson(ownerRef) << "\",\n";
        f << "      \"base\": \"" << EscapeJson(baseRef) << "\",\n";

        // Archetype
        std::string archetypeName;
        {
            UObject* arch = actor->GetField<UObject*>(UObject::OFFSET_ARCHETYPE);
            if (arch) {
                char archBuf[512];
                if (SafeGetFullPath(arch, archBuf, sizeof(archBuf)))
                    archetypeName = archBuf;
            }
        }
        f << "      \"archetype\": \"" << EscapeJson(archetypeName) << "\",\n";

        // LifeSpan (>0 means spawned at runtime / temporary)
        float lifeSpan = 0;
        ReadFloatProp(actor, "LifeSpan", lifeSpan);
        f << "      \"lifeSpan\": " << lifeSpan << ",\n";

        // ─── Special handling per class type ─────────────────────────────────

        // Pickup / Weapon actors
        if (className.find("Pickup") != std::string::npos ||
            className.find("Weapon") != std::string::npos ||
            className.find("Ammo") != std::string::npos ||
            className.find("Item") != std::string::npos) {
            f << "      \"_type\": \"pickup\",\n";
            std::string invType = ReadObjRefPath(actor, "InventoryType");
            std::string pickupClass = ReadObjRefPath(actor, "PickupClass");
            std::string pickupMesh = ReadObjRefPath(actor, "PickupMesh");
            std::string thirdPersonMesh = ReadObjRefPath(actor, "ThirdPersonMesh");
            std::string pickupViewMesh = ReadObjRefPath(actor, "PickupViewMesh");
            std::string glowEffect = ReadObjRefName(actor, "PickupGlow");
            f << "      \"inventoryType\": \"" << EscapeJson(invType) << "\",\n";
            f << "      \"pickupClass\": \"" << EscapeJson(pickupClass) << "\",\n";
            f << "      \"pickupMesh\": \"" << EscapeJson(pickupMesh) << "\",\n";
            f << "      \"thirdPersonMesh\": \"" << EscapeJson(thirdPersonMesh) << "\",\n";
            f << "      \"pickupViewMesh\": \"" << EscapeJson(pickupViewMesh) << "\",\n";
            f << "      \"pickupGlow\": \"" << EscapeJson(glowEffect) << "\",\n";
        }
        // Corpse / Body / Decoration
        else if (className.find("Corpse") != std::string::npos ||
                 className.find("Body") != std::string::npos ||
                 className.find("DeadBody") != std::string::npos ||
                 className.find("Decoration") != std::string::npos ||
                 className.find("Ragdoll") != std::string::npos ||
                 className.find("KActor") != std::string::npos) {
            f << "      \"_type\": \"corpse_debris\",\n";
            std::string skelMesh = ReadObjRefPath(actor, "SkeletalMesh");
            if (skelMesh.empty()) skelMesh = ReadObjRefPath(actor, "Mesh");
            f << "      \"skeletalMesh\": \"" << EscapeJson(skelMesh) << "\",\n";
        }
        // Projector / Decal
        else if (className.find("Projector") != std::string::npos ||
                 className.find("Decal") != std::string::npos) {
            f << "      \"_type\": \"decal_projector\",\n";
            std::string projTex = ReadObjRefPath(actor, "ProjTexture");
            if (projTex.empty()) projTex = ReadObjRefPath(actor, "DecalMaterial");
            float fov = 0;
            ReadFloatProp(actor, "FOV", fov);
            float maxDist = 0;
            ReadFloatProp(actor, "MaxTraceDistance", maxDist);
            f << "      \"projTexture\": \"" << EscapeJson(projTex) << "\",\n";
            f << "      \"projFOV\": " << fov << ",\n";
            f << "      \"projMaxDist\": " << maxDist << ",\n";
        }
        // Emitter / Particle
        else if (className.find("Emitter") != std::string::npos ||
                 className.find("Particle") != std::string::npos) {
            f << "      \"_type\": \"emitter\",\n";
            std::string emitterTemplate = ReadObjRefPath(actor, "Template");
            if (emitterTemplate.empty()) emitterTemplate = ReadObjRefPath(actor, "ParticleSystemComponent");
            f << "      \"emitterTemplate\": \"" << EscapeJson(emitterTemplate) << "\",\n";
        }
        // Light actors
        else if (className.find("Light") != std::string::npos) {
            f << "      \"_type\": \"light\",\n";
            uint8_t brightness = 0, hue = 0, saturation = 0;
            float lightRadius = 0;
            ReadByteProp(actor, "LightBrightness", brightness);
            ReadByteProp(actor, "LightHue", hue);
            ReadByteProp(actor, "LightSaturation", saturation);
            ReadFloatProp(actor, "LightRadius", lightRadius);
            // If LightRadius is 0, try reading as int (some versions)
            if (lightRadius == 0) {
                int32_t iRadius = 0;
                ReadIntProp(actor, "LightRadius", iRadius);
                lightRadius = (float)iRadius;
            }
            uint8_t lightType = 0, lightEffect = 0;
            ReadByteProp(actor, "LightType", lightType);
            ReadByteProp(actor, "LightEffect", lightEffect);
            f << "      \"lightBrightness\": " << (int)brightness << ",\n";
            f << "      \"lightHue\": " << (int)hue << ",\n";
            f << "      \"lightSaturation\": " << (int)saturation << ",\n";
            f << "      \"lightRadius\": " << lightRadius << ",\n";
            f << "      \"lightType\": " << (int)lightType << ",\n";
            f << "      \"lightEffect\": " << (int)lightEffect << ",\n";
        }
        // Default
        else {
            f << "      \"_type\": \"generic\",\n";
        }

        // Skins array (up to 8 material slots)
        f << "      \"skins\": [";
        bool firstSkin = true;
        for (int s = 0; s < 8; s++) {
            char skinProp[32];
            snprintf(skinProp, sizeof(skinProp), "Skins(%d)", s);
            // Try array-style access
            UObject* skinRef = nullptr;
            if (GetActorProperty(actor, skinProp, &skinRef, sizeof(UObject*)) && skinRef) {
                char skinNameBuf[256];
                SafeGetName(skinRef, skinNameBuf, sizeof(skinNameBuf));
                if (!firstSkin) f << ", ";
                f << "\"" << EscapeJson(std::string(skinNameBuf)) << "\"";
                firstSkin = false;
            }
        }
        f << "],\n";

        // Tag / event
        // Try reading Tag as FName (8 bytes)
        FName tag = {0, 0};
        if (GetActorProperty(actor, "Tag", &tag, sizeof(FName))) {
            std::string tagStr = tag.ToString();
            f << "      \"tag\": \"" << EscapeJson(tagStr) << "\",\n";
        } else {
            f << "      \"tag\": \"\",\n";
        }

        // Appears spawned at runtime? Heuristic: lifeSpan > 0 or bDeleteMe or certain flags
        uint64_t flags = actor->GetObjectFlags();
        bool appearsSpawned = (lifeSpan > 0.0f) || ((flags & 0x0001) == 0); // RF_Transactional off = spawned
        f << "      \"appearsSpawned\": " << (appearsSpawned ? "true" : "false") << "\n";

        f << "    }";
        exported++;
    }

    f << "\n  ]\n";
    f << "}\n";
    f.close();

    return exported;
}

// ─── Full Comprehensive Scan ────────────────────────────────────────────────
// Iterates ALL GObjects and exports:
//   - Every actor with all readable properties
//   - All UStaticMesh objects (name + path)
//   - All UTexture/UTexture2D objects (name + path + dimensions)
//   - All UMaterial/UShader/UMaterialInstanceConstant objects (name + path)
// This gives the level editor everything it needs to match BSM data.

static void WritePropertyValue(std::ofstream& f, UObject* obj, UProperty* prop,
                                const std::string& propName, const std::string& typeName)
{
    const uint8_t* base = reinterpret_cast<const uint8_t*>(obj);
    int32_t offset = prop->GetPropertyOffset();
    int32_t elemSize = prop->GetElementSize();

    if (typeName == "FloatProperty") {
        float val = *reinterpret_cast<const float*>(base + offset);
        f << val;
    } else if (typeName == "IntProperty") {
        int32_t val = *reinterpret_cast<const int32_t*>(base + offset);
        f << val;
    } else if (typeName == "ByteProperty") {
        uint8_t val = *(base + offset);
        f << (int)val;
    } else if (typeName == "BoolProperty") {
        uint32_t bitMask = prop->GetField<uint32_t>(0x78);
        if (bitMask == 0) bitMask = 1;
        uint32_t raw = *reinterpret_cast<const uint32_t*>(base + offset);
        f << ((raw & bitMask) ? "true" : "false");
    } else if (typeName == "NameProperty") {
        FName nm = *reinterpret_cast<const FName*>(base + offset);
        f << "\"" << EscapeJson(nm.ToString()) << "\"";
    } else if (typeName == "StrProperty") {
        // FString: int32 Length, then chars
        f << "\"<fstring>\"";
    } else if (typeName == "ObjectProperty" || typeName == "ComponentProperty") {
        UObject* ref = *reinterpret_cast<UObject* const*>(base + offset);
        if (ref && IsValidPtr(ref)) {
            char refBuf[512];
            SafeGetFullPath(ref, refBuf, sizeof(refBuf));
            f << "\"" << EscapeJson(std::string(refBuf)) << "\"";
        } else {
            f << "null";
        }
    } else if (typeName == "ClassProperty") {
        UObject* ref = *reinterpret_cast<UObject* const*>(base + offset);
        if (ref && IsValidPtr(ref)) {
            char refBuf[256];
            SafeGetFullPath(ref, refBuf, sizeof(refBuf));
            f << "\"" << EscapeJson(std::string(refBuf)) << "\"";
        } else {
            f << "null";
        }
    } else if (typeName == "StructProperty") {
        // Check if it's a known struct (FVector, FRotator, FColor)
        if (elemSize == 12) {
            // Likely FVector
            float x = *reinterpret_cast<const float*>(base + offset);
            float y = *reinterpret_cast<const float*>(base + offset + 4);
            float z = *reinterpret_cast<const float*>(base + offset + 8);
            f << "[" << x << ", " << y << ", " << z << "]";
        } else if (elemSize == 4) {
            // Likely FColor
            uint32_t col = *reinterpret_cast<const uint32_t*>(base + offset);
            f << col;
        } else {
            f << "\"<struct_" << elemSize << "B>\"";
        }
    } else if (typeName == "ArrayProperty") {
        // TArray: pointer, count, max
        int32_t count = *reinterpret_cast<const int32_t*>(base + offset + 4);
        f << "\"<array[" << count << "]>\"";
    } else {
        f << "\"<" << typeName << ">\"";
    }
}

int ExecuteFullScan()
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return -1;

    EnsureDumpDir();
    char outPath[MAX_PATH];
    snprintf(outPath, sizeof(outPath), "%s\\runtime_full_scan.json", GetDebugDir());

    std::ofstream f(outPath);
    if (!f.is_open()) return -1;

    // Get player position for distance calculation
    FVec3 playerPos = {0, 0, 0};
    bool hasPlayer = GetPlayerPosition(playerPos);

    // Get level info
    auto level = GetCurrentLevel();

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt; localtime_s(&lt, &t);
    char tsBuf[64];
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);

    f << "{\n";
    f << "  \"timestamp\": \"" << tsBuf << "\",\n";
    f << "  \"mapName\": \"" << EscapeJson(level.LevelName) << "\",\n";
    if (hasPlayer)
        f << "  \"playerLocation\": [" << playerPos.X << ", " << playerPos.Y << ", " << playerPos.Z << "],\n";
    else
        f << "  \"playerLocation\": null,\n";

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    f << "  \"totalGObjects\": " << objCount << ",\n";

    // ─── Collect objects by category ───
    struct ObjEntry {
        UObject* obj;
        std::string name;
        std::string className;
        std::string fullPath;
    };

    std::vector<ObjEntry> actors;
    std::vector<ObjEntry> staticMeshes;
    std::vector<ObjEntry> textures;
    std::vector<ObjEntry> materials;

    for (int i = 0; i < objCount; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr || ptr < 0x10000) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (!IsValidPtr(obj)) continue;

        char cnBuf[256], nameBuf[256];
        if (!SafeGetClassName(obj, cnBuf, sizeof(cnBuf))) continue;
        if (!SafeGetName(obj, nameBuf, sizeof(nameBuf))) continue;
        std::string cn(cnBuf);
        std::string nm(nameBuf);

        // Categorize
        if (cn.find("StaticMeshActor") != std::string::npos ||
            cn.find("InterpActor") != std::string::npos ||
            cn.find("Pickup") != std::string::npos ||
            cn.find("Weapon") != std::string::npos ||
            cn.find("Ammo") != std::string::npos ||
            cn.find("KActor") != std::string::npos ||
            cn.find("Projector") != std::string::npos ||
            cn.find("Decal") != std::string::npos ||
            cn.find("Emitter") != std::string::npos ||
            cn.find("Light") != std::string::npos ||
            cn.find("Trigger") != std::string::npos ||
            cn.find("Mover") != std::string::npos ||
            cn.find("Decoration") != std::string::npos ||
            cn.find("Body") != std::string::npos ||
            cn.find("Corpse") != std::string::npos ||
            cn.find("Effect") != std::string::npos ||
            cn.find("Pawn") != std::string::npos ||
            cn.find("Controller") != std::string::npos ||
            cn.find("Info") != std::string::npos ||
            cn.find("Volume") != std::string::npos ||
            cn.find("Brush") != std::string::npos ||
            cn.find("Note") != std::string::npos ||
            cn.find("Actor") != std::string::npos) {
            char pathBuf[512];
            SafeGetFullPath(obj, pathBuf, sizeof(pathBuf));
            actors.push_back({obj, nm, cn, std::string(pathBuf)});
        }
        else if (cn == "StaticMesh") {
            char pathBuf[512];
            SafeGetFullPath(obj, pathBuf, sizeof(pathBuf));
            staticMeshes.push_back({obj, nm, cn, std::string(pathBuf)});
        }
        else if (cn.find("Texture") != std::string::npos) {
            char pathBuf[512];
            SafeGetFullPath(obj, pathBuf, sizeof(pathBuf));
            textures.push_back({obj, nm, cn, std::string(pathBuf)});
        }
        else if (cn.find("Material") != std::string::npos ||
                 cn == "Shader" ||
                 cn.find("Shader") != std::string::npos ||
                 cn == "Combiner" ||
                 cn == "ConstantMaterial" ||
                 cn == "FinalBlend" ||
                 cn == "TexModifier" ||
                 cn == "TexPanner" ||
                 cn == "TexRotator" ||
                 cn == "TexOscillator" ||
                 cn == "TexScaler") {
            char pathBuf[512];
            SafeGetFullPath(obj, pathBuf, sizeof(pathBuf));
            materials.push_back({obj, nm, cn, std::string(pathBuf)});
        }
    }

    // ─── Write actors section ───
    f << "  \"actors\": [\n";
    int actorCount = 0;
    for (auto& ae : actors) {
        if (actorCount > 0) f << ",\n";
        f << "    {\n";
        f << "      \"name\": \"" << EscapeJson(ae.name) << "\",\n";
        f << "      \"class\": \"" << EscapeJson(ae.className) << "\",\n";
        f << "      \"fullPath\": \"" << EscapeJson(ae.fullPath) << "\",\n";

        // Try reading location
        FVector loc = {0, 0, 0};
        bool hasLoc = ReadVectorProp(ae.obj, "Location", loc);
        if (hasLoc) {
            f << "      \"location\": [" << loc.X << ", " << loc.Y << ", " << loc.Z << "],\n";
            if (hasPlayer) {
                float dx = loc.X - playerPos.X;
                float dy = loc.Y - playerPos.Y;
                float dz = loc.Z - playerPos.Z;
                f << "      \"distFromPlayer\": " << sqrtf(dx*dx + dy*dy + dz*dz) << ",\n";
            }
        } else {
            f << "      \"location\": null,\n";
        }

        // Rotation
        FRotator rot = {0, 0, 0};
        if (ReadRotatorProp(ae.obj, "Rotation", rot)) {
            f << "      \"rotation\": [" << rot.Pitch * (360.0f/65536.0f) << ", "
              << rot.Yaw * (360.0f/65536.0f) << ", " << rot.Roll * (360.0f/65536.0f) << "],\n";
        }

        // DrawScale
        float drawScale = 1.0f;
        ReadFloatProp(ae.obj, "DrawScale", drawScale);
        FVector ds3d = {1,1,1};
        ReadVectorProp(ae.obj, "DrawScale3D", ds3d);
        f << "      \"drawScale\": " << drawScale << ",\n";
        f << "      \"drawScale3D\": [" << ds3d.X << ", " << ds3d.Y << ", " << ds3d.Z << "],\n";

        // Key object refs
        std::string staticMesh = ReadObjRefPath(ae.obj, "StaticMesh");
        std::string mesh = ReadObjRefPath(ae.obj, "Mesh");
        std::string skelMesh = ReadObjRefPath(ae.obj, "SkeletalMesh");
        f << "      \"staticMesh\": \"" << EscapeJson(staticMesh) << "\",\n";
        f << "      \"mesh\": \"" << EscapeJson(mesh) << "\",\n";
        f << "      \"skeletalMesh\": \"" << EscapeJson(skelMesh) << "\",\n";

        // Visibility
        bool bHidden = false;
        ReadBoolProp(ae.obj, "bHidden", bHidden);
        uint8_t drawType = 0;
        ReadByteProp(ae.obj, "DrawType", drawType);
        f << "      \"bHidden\": " << (bHidden ? "true" : "false") << ",\n";
        f << "      \"drawType\": " << (int)drawType << ",\n";

        // Collision
        float colRad = 0, colH = 0;
        ReadFloatProp(ae.obj, "CollisionRadius", colRad);
        ReadFloatProp(ae.obj, "CollisionHeight", colH);
        f << "      \"collisionRadius\": " << colRad << ",\n";
        f << "      \"collisionHeight\": " << colH << ",\n";

        // Skins/Materials array
        f << "      \"skins\": [";
        bool firstSkin = true;
        for (int s = 0; s < 8; s++) {
            char skinProp[32];
            snprintf(skinProp, sizeof(skinProp), "Skins(%d)", s);
            UObject* skinRef = nullptr;
            if (GetActorProperty(ae.obj, skinProp, &skinRef, sizeof(UObject*)) && skinRef && IsValidPtr(skinRef)) {
                char skinBuf[512];
                SafeGetFullPath(skinRef, skinBuf, sizeof(skinBuf));
                if (!firstSkin) f << ", ";
                f << "\"" << EscapeJson(std::string(skinBuf)) << "\"";
                firstSkin = false;
            }
        }
        f << "],\n";

        // Dump ALL properties for this actor
        f << "      \"properties\": {\n";
        UStruct* cls = reinterpret_cast<UStruct*>(ae.obj->GetClass());
        if (cls && IsValidPtr(cls)) {
            auto props = WalkProperties(cls);
            bool firstProp = true;
            int propLimit = 80; // max properties to dump per actor
            for (auto& pi : props) {
                if (propLimit-- <= 0) break;
                // Skip very large properties and internal ones
                if (pi.ElementSize > 64) continue;
                if (pi.Name.empty() || pi.Name[0] == '_') continue;
                if (pi.Offset < 0) continue;

                if (!firstProp) f << ",\n";
                f << "        \"" << EscapeJson(pi.Name) << "\": ";

                try {
                    WritePropertyValue(f, ae.obj, pi.PropertyObj, pi.Name, pi.TypeName);
                } catch (...) {
                    f << "null";
                }
                firstProp = false;
            }
        }
        f << "\n      }\n";
        f << "    }";
        actorCount++;
    }
    f << "\n  ],\n";

    // ─── Write static meshes section ───
    f << "  \"staticMeshes\": [\n";
    for (int i = 0; i < (int)staticMeshes.size(); i++) {
        if (i > 0) f << ",\n";
        f << "    {\"name\": \"" << EscapeJson(staticMeshes[i].name)
          << "\", \"path\": \"" << EscapeJson(staticMeshes[i].fullPath) << "\"}";
    }
    f << "\n  ],\n";

    // ─── Write textures section ───
    f << "  \"textures\": [\n";
    for (int i = 0; i < (int)textures.size(); i++) {
        if (i > 0) f << ",\n";
        f << "    {\"name\": \"" << EscapeJson(textures[i].name)
          << "\", \"class\": \"" << EscapeJson(textures[i].className)
          << "\", \"path\": \"" << EscapeJson(textures[i].fullPath) << "\"}";
    }
    f << "\n  ],\n";

    // ─── Write materials section ───
    f << "  \"materials\": [\n";
    for (int i = 0; i < (int)materials.size(); i++) {
        if (i > 0) f << ",\n";
        f << "    {\"name\": \"" << EscapeJson(materials[i].name)
          << "\", \"class\": \"" << EscapeJson(materials[i].className)
          << "\", \"path\": \"" << EscapeJson(materials[i].fullPath) << "\"}";
    }
    f << "\n  ],\n";

    // ─── Summary ───
    f << "  \"summary\": {\n";
    f << "    \"actors\": " << actorCount << ",\n";
    f << "    \"staticMeshes\": " << staticMeshes.size() << ",\n";
    f << "    \"textures\": " << textures.size() << ",\n";
    f << "    \"materials\": " << materials.size() << "\n";
    f << "  }\n";
    f << "}\n";
    f.close();

    return actorCount + (int)staticMeshes.size() + (int)textures.size() + (int)materials.size();
}

// ─── Console Command Handler ────────────────────────────────────────────────

void HandleScanCommand(const std::string& args)
{
    // Parse: "scan [radius]" or "scanradius <value>"
    if (args.empty() || args == "scan") {
        int count = ExecuteNearbyScan();
        if (count >= 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Exported %d actors (radius %.0f) -> BS1SDK_dumps/runtime_nearby_actors.json", count, s_ScanRadius);
            DebugSessionLog(buf);
        }
    } else {
        // Try to parse as radius
        float r = std::strtof(args.c_str(), nullptr);
        if (r > 0) {
            s_ScanRadius = r;
            char buf[128];
            snprintf(buf, sizeof(buf), "Scan radius set to %.0f", s_ScanRadius);
            DebugSessionLog(buf);
        }
    }
}

} // namespace bs1sdk
