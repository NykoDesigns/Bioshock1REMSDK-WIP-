#pragma once

#include "bsm_document.h"
#include "viewport.h"
#include "scene_tree.h"
#include "properties_panel.h"
#include "content_browser.h"

struct SDL_Window;
typedef void* SDL_GLContext;

// Main application class
class App {
public:
    bool Init();
    void Run();
    void Shutdown();

private:
    void ProcessEvents();
    void Update(float dt);
    void RenderUI();
    void OpenFileDialog();
    void SpawnMeshActor(const std::string& meshName);
    void DeleteSelectedActor();
    void ReloadMap();
    void RenderLoadingFrame(const char* message, float progress = -1.0f);

    SDL_Window* m_Window = nullptr;
    SDL_GLContext m_GLContext = nullptr;
    bool m_Running = false;

    BSMDocument m_Document;
    Viewport m_Viewport;
    SceneTree m_SceneTree;
    PropertiesPanel m_PropertiesPanel;
    ContentBrowser m_ContentBrowser;

    int m_SelectedActor = -1;
    bool m_ShowDemo = false;
    std::string m_ExportDir; // UEViewer export base directory

    // Viewport input state
    bool m_ViewportHovered = false;
    bool m_Loading = false;
    std::string m_LoadingMessage;
    float m_LoadingProgress = -1.0f;
    bool m_RMBDown = false;
    bool m_MMBDown = false;
    float m_LastMouseX = 0;
    float m_LastMouseY = 0;
};
