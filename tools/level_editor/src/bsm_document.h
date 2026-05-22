#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include "camera.h"
#include "mesh_parser.h"
#include "bsp_parser.h"

// How an actor should be rendered in the viewport
enum class ActorRenderType {
    VisibleStaticMesh,   // Has a resolved StaticMesh → render as 3D geometry
    VisibleMover,        // Door/Mover with mesh → render as 3D geometry
    VisiblePickup,       // Pickup/weapon with mesh → render as 3D geometry
    VisibleDecoration,   // Decoration/debris/corpse with mesh
    VisibleEmitter,      // Particle emitter (future: render particles)
    VisibleDecal,        // Decal projector (future: project texture)
    LightOnly,           // Light actor → contributes lighting, no geometry
    CollisionOnly,       // BlockingVolume/collision → invisible in-game
    TriggerOnly,         // Trigger/volume → invisible in-game
    EditorOnly,          // PathNode/PlayerStart/nav → editor helper only
    UnknownPlaceholder,  // Unclassified or missing mesh
};

ActorRenderType ResolveActorRenderType(const std::string& className, bool hasMesh, bool isLight);
bool IsVisibleInGame(ActorRenderType rt);

// Lightweight actor representation for the editor
struct EditorActor {
    int exportIndex;
    std::string className;
    std::string objectName;
    Vec3 location = {0, 0, 0};
    Vec3 rotation = {0, 0, 0}; // pitch, yaw, roll in degrees
    Vec3 scale = {1, 1, 1};
    int serialSize = 0;
    int serialOffset = 0;
    int meshIndex = -1; // index into BSMDocument::m_Meshes, -1 if no mesh
    std::string meshRefName; // name of the referenced mesh export (even if unresolved)
    ActorRenderType renderType = ActorRenderType::UnknownPlaceholder;
    bool hasLocation = false;
    bool selected = false;
    bool visible = true;

    // Light actor properties (only valid when class contains "Light")
    float lightBrightness = 1.0f;  // LightBrightness (byte 0-255 → 0-1)
    float lightRadius = 1024.0f;   // LightRadius (Unreal units)
    uint8_t lightColorR = 255;
    uint8_t lightColorG = 255;
    uint8_t lightColorB = 255;
    bool isLight = false;

    // Projector/Decal properties
    bool isProjector = false;
    std::string projTextureName; // ProjTexture material/texture name
    float projFOV = 30.0f;       // projection cone half-angle
    float projMaxDist = 256.0f;  // MaxTraceDistance
};

// Actor category for color coding in viewport
enum class ActorCategory {
    Spawner,      // Red (0)
    Trigger,      // Yellow (1)
    Light,        // Orange (2)
    Door,         // Blue (3)
    Pickup,       // Green (4)
    Effect,       // Cyan (5)
    PlayerStart,  // White (6)
    StaticMesh,   // Gray (7)
    Other         // Dark gray (8)
    // NOTE: icon textures array is [9], matching enum order 0-8
};

ActorCategory CategorizeActor(const std::string& className);
Vec3 CategoryColor(ActorCategory cat);

// BSM Document — loads a .bsm file and provides editor-friendly access
class BSMDocument {
public:
    bool Load(const std::string& filepath);
    bool Save(const std::string& filepath);
    bool IsLoaded() const { return m_Loaded; }

    const std::string& GetFilePath() const { return m_FilePath; }
    const std::string& GetMapName() const { return m_MapName; }
    int GetActorCount() const { return (int)m_Actors.size(); }
    
    std::vector<EditorActor>& GetActors() { return m_Actors; }
    const std::vector<EditorActor>& GetActors() const { return m_Actors; }

    // Modify actor location in the underlying BSM data
    bool SetActorLocation(int actorIdx, Vec3 newLoc);
    bool SetActorRotation(int actorIdx, Vec3 newRot);

    // Meshes
    const std::vector<ParsedMesh>& GetMeshes() const { return m_Meshes; }
    int GetMeshCount() const { return (int)m_Meshes.size(); }

    // BSP geometry (level shell - walls, floors, ceilings)
    const std::vector<ParsedMesh>& GetBSPMeshes() const { return m_BSPMeshes; }
    bool HasBSP() const { return !m_BSPMeshes.empty(); }

    // BSP tree traversal for zone detection
    struct BSPTreeNode {
        float planeX, planeY, planeZ, planeW;
        int32_t iFront, iBack;
        uint8_t zoneMask[16];
        uint8_t iZone;
    };
    // Returns the zone index (0-127) for the camera position, or -1 if no tree
    int FindCameraZone(Vec3 pos) const;
    const std::vector<BSPTreeNode>& GetBSPTree() const { return m_BSPTree; }

    // Resolve texture names using UEViewer export directory
    void ResolveTextures(const std::string& umodelExportDir);

    // Stats
    int GetNameCount() const { return m_NameCount; }
    int GetImportCount() const { return m_ImportCount; }
    int GetExportCount() const { return m_ExportCount; }

private:
    bool m_Loaded = false;
    std::string m_FilePath;
    std::string m_MapName;
    int m_NameCount = 0;
    int m_ImportCount = 0;
    int m_ExportCount = 0;
    std::vector<EditorActor> m_Actors;
    std::vector<ParsedMesh> m_Meshes;
    std::vector<ParsedMesh> m_BSPMeshes;
    std::vector<BSPTreeNode> m_BSPTree;
    std::unordered_map<std::string, int> m_MeshNameToIndex;
    std::vector<uint8_t> m_RawData;

    // Internal parse state
    struct ParsedExport {
        int classIndex;
        std::string className;
        std::string objectName;
        int serialSize;
        int serialOffset;
        int locationValueFileOffset; // -1 if no location
        int rotationValueFileOffset; // -1 if no rotation
    };
    std::vector<ParsedExport> m_ParsedExports;
};
