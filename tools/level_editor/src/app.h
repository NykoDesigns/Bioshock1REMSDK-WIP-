#pragma once

#include "bsm_document.h"
#include "viewport.h"
#include "scene_tree.h"
#include "properties_panel.h"
#include "content_browser.h"
#include <set>

struct SDL_Window;
typedef void* SDL_GLContext;

// Main application class
class App {
public:
    bool Init(const char* mapPath = nullptr);
    void Run();
    void Shutdown();

    // Runtime scan data types (public for helper access)
    struct RuntimeActor {
        std::string name;
        std::string className;
        std::string fullPath;
        float location[3] = {0,0,0};
        bool hasLocation = false;
        float rotation[3] = {0,0,0};
        float drawScale = 1.0f;
        float drawScale3D[3] = {1,1,1};
        bool bHidden = false;
        std::string drawType;
        std::string staticMesh;
        std::string mesh;
        std::string skeletalMesh;
        std::string type;
        float distance = 0;
        bool appearsSpawned = false;
        int matchedEditorActor = -1;
        std::vector<std::string> skins;
    };
    struct RuntimeMeshEntry { std::string name; std::string path; };
    struct RuntimeMaterialEntry { std::string name; std::string className; std::string path; };

private:
    void ProcessEvents();
    void Update(float dt);
    void RenderUI();
    void OpenFileDialog();
    void SpawnMeshActor(const std::string& meshName);
    void SpawnActorByClass(const std::string& className);
    void DeleteSelectedActor();
    void DuplicateSelectedActor();
    void ReloadMap();
    void RenderLoadingFrame(const char* message, float progress = -1.0f);
    void SaveLayoutJSON(const std::string& path);
    void LoadLayoutJSON(const std::string& path);
    void SaveLayoutDialog();
    void LoadLayoutDialog();
    void CopySelection();
    void PasteSelection();
    void SelectAll();
    void AlignSelectionToGrid();

    SDL_Window* m_Window = nullptr;
    SDL_GLContext m_GLContext = nullptr;
    bool m_Running = false;

    BSMDocument m_Document;
    Viewport m_Viewport;
    SceneTree m_SceneTree;
    PropertiesPanel m_PropertiesPanel;
    ContentBrowser m_ContentBrowser;

    int m_SelectedActor = -1;
    std::set<int> m_SelectedActors; // multi-select set

    bool m_ShowDemo = false;
    bool m_ShowShortcuts = false;
    bool m_ShowSpawner = false; // actor spawner palette
    bool m_ShowActorReport = false; // actor render classification report
    bool m_ShowNearbyInspector = false; // nearby actor inspector
    float m_NearbyRadius = 2000.0f; // inspection radius
    bool m_ShowTargetAreaReport = false; // runtime scan comparison report

    // Runtime scan data
    std::vector<RuntimeActor> m_RuntimeActors;
    std::vector<RuntimeMeshEntry> m_RuntimeMeshes;
    std::vector<RuntimeMaterialEntry> m_RuntimeTextures;
    std::vector<RuntimeMaterialEntry> m_RuntimeMaterials;
    std::string m_RuntimeScanMap;
    float m_RuntimeScanPos[3] = {0,0,0};
    float m_RuntimeScanRadius = 0;
    int m_RuntimeTotalGObjects = 0;
    bool LoadRuntimeScanJSON(const std::string& path);
    bool LoadRuntimeFullScanJSON(const std::string& path);
    void MatchRuntimeToEditor();
    int ImportUnmatchedActors(); // returns count of imported actors
    std::string m_ExportDir;
    float m_SnapGrid = 0.0f;
    std::string m_LayoutPath;

    // Camera bookmarks (Ctrl+0-9 to save, 0-9 to recall)
    struct CameraBookmark {
        Vec3 pos = {0,0,0};
        float yaw = 0, pitch = 0, dist = 5000;
        bool valid = false;
    };
    CameraBookmark m_Bookmarks[10];

    // Copy/paste buffer
    std::vector<EditorActor> m_CopyBuffer;

    // Recent files
    std::vector<std::string> m_RecentFiles;
    void AddRecentFile(const std::string& path);

    // Autosave
    float m_AutosaveTimer = 0;
    float m_AutosaveInterval = 300.0f; // 5 minutes
    bool m_AutosaveEnabled = true;

    // Box select state
    bool m_BoxSelecting = false;
    float m_BoxStartX = 0, m_BoxStartY = 0;
    float m_BoxEndX = 0, m_BoxEndY = 0;

    // Performance stats
    int m_DrawCalls = 0;
    int m_TriCount = 0;

    // Viewport input state
    bool m_ViewportHovered = false;
    bool m_Loading = false;
    std::string m_LoadingMessage;
    float m_LoadingProgress = -1.0f;
    bool m_RMBDown = false;
    bool m_MMBDown = false;
    bool m_LMBDown = false;
    float m_LastMouseX = 0;
    float m_LastMouseY = 0;
    float m_FPS = 0.0f;

    // Undo/Redo system
    struct UndoAction {
        int actorIndex;
        Vec3 oldPos, newPos;
    };
    std::vector<UndoAction> m_UndoStack;
    int m_UndoIndex = -1;
    void PushUndo(int actorIdx, const Vec3& oldPos, const Vec3& newPos);
    void Undo();
    void Redo();

    // Gizmo dragging state
    bool m_GizmoWasDragging = false;
    float m_ViewportW = 1, m_ViewportH = 1;
};
