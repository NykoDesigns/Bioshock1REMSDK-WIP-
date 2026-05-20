#include "app.h"
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include "gl_funcs.h"
#include <cmath>
#include <cstdio>
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#endif

extern void LogMsg(const char* msg);

bool App::Init(const char* mapPath)
{
    LogMsg("[App] Init start");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) { LogMsg("[App] SDL_Init failed"); return false; }
    LogMsg("[App] SDL_Init OK");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    m_Window = SDL_CreateWindow(
        "BS1 Level Editor v0.1",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1600, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED
    );
    if (!m_Window) { LogMsg("[App] SDL_CreateWindow failed"); return false; }
    LogMsg("[App] Window created");

    m_GLContext = SDL_GL_CreateContext(m_Window);
    SDL_GL_MakeCurrent(m_Window, m_GLContext);
    SDL_GL_SetSwapInterval(1); // vsync
    LogMsg("[App] GL context created");

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.1f, 0.12f, 1.0f);

    ImGui_ImplSDL2_InitForOpenGL(m_Window, m_GLContext);
    ImGui_ImplOpenGL3_Init("#version 330");

    LogMsg("[App] ImGui init done");
    // Viewport
    if (!m_Viewport.Init()) { LogMsg("[App] Viewport init FAILED"); return false; }

    m_Running = true;
    LogMsg("[App] Viewport init done, loading map...");

    // Load map: use command-line path if provided, otherwise default to Medical
    const char* defaultMap = mapPath ? mapPath : "D:\\SteamLibrary\\steamapps\\common\\BioShock Remastered\\ContentBaked\\pc\\Maps\\1-Medical.bsm";
    printf("[App] Loading map: %s\n", defaultMap);
    LogMsg("[App] About to load BSM file");
    {
        std::string loadMsg = std::string("Loading ") + defaultMap + "...";
        RenderLoadingFrame(loadMsg.c_str());
    }
    if (m_Document.Load(defaultMap)) {
        LogMsg("[App] BSM loaded successfully");
        auto& actors = m_Document.GetActors();
        if (!actors.empty()) {
            double cx = 0, cy = 0, cz = 0;
            int validCount = 0;
            for (auto& a : actors) {
                if (std::abs(a.location.x) < 1e6f && std::abs(a.location.y) < 1e6f && std::abs(a.location.z) < 1e6f) {
                    cx += a.location.x; cy += a.location.y; cz += a.location.z;
                    validCount++;
                }
            }
            Vec3 center = {0, 0, 0};
            if (validCount > 0)
                center = {(float)(cx / validCount), (float)(cy / validCount), (float)(cz / validCount)};
            m_Viewport.GetCamera().FocusOn(center);
            m_Viewport.GetCamera().distance = 20000.0f;
        }
        std::string mapName = m_Document.GetMapName();
        { size_t dp = mapName.find_last_of('.'); if (dp != std::string::npos) mapName = mapName.substr(0, dp); }
        m_ExportDir = "Z:\\UEViewer\\export\\" + mapName;
        LogMsg("[App] Resolving textures...");
        m_Document.ResolveTextures(m_ExportDir);
        std::string texDir = m_ExportDir + "\\Texture";
        LogMsg("[App] Uploading meshes to GPU...");
        m_Viewport.UploadMeshes(m_Document.GetMeshes(), texDir);
        LogMsg("[App] Meshes uploaded");
        if (m_Document.HasBSP())
            m_Viewport.UploadBSP(m_Document.GetBSPMeshes(), texDir);
        m_ContentBrowser.ScanDirectory(m_ExportDir);
    }

    return true;
}

void App::Run()
{
    Uint64 lastTime = SDL_GetPerformanceCounter();

    while (m_Running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - lastTime) / (float)SDL_GetPerformanceFrequency();
        lastTime = now;

        ProcessEvents();
        Update(dt);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();

        int w, h;
        SDL_GetWindowSize(m_Window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update camera zone for zone-based BSP visibility
        if (m_Viewport.m_ZoneFilterEnabled && m_Document.IsLoaded()) {
            m_Viewport.m_CameraZone = m_Document.FindCameraZone(m_Viewport.GetCamera().GetPosition());
        }

        // Render 3D viewport (full background)
        m_Viewport.Render(m_Document, m_SelectedActor);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(m_Window);
    }
}

void App::Shutdown()
{
    m_Viewport.Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(m_GLContext);
    SDL_DestroyWindow(m_Window);
    SDL_Quit();
}

void App::ProcessEvents()
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);

        if (e.type == SDL_QUIT) m_Running = false;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
            if (m_RMBDown) {
                m_RMBDown = false;
                m_Viewport.GetCamera().EndFly();
                SDL_SetRelativeMouseMode(SDL_FALSE);
            } else {
                m_Running = false;
            }
        }

        // Camera control (when viewport is hovered and ImGui doesn't want input)
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse) {
            if (e.type == SDL_MOUSEBUTTONDOWN) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    m_RMBDown = true;
                    m_Viewport.GetCamera().BeginFly();
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                }
                if (e.button.button == SDL_BUTTON_MIDDLE) m_MMBDown = true;
                if (e.button.button == SDL_BUTTON_LEFT && !m_RMBDown) {
                    // Pick actor
                    int w, h;
                    SDL_GetWindowSize(m_Window, &w, &h);
                    int picked = m_Viewport.PickActor(m_Document,
                        (float)e.button.x, (float)e.button.y, (float)w, (float)h);
                    if (picked >= 0) m_SelectedActor = picked;
                }
            }
            if (e.type == SDL_MOUSEBUTTONUP) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    m_RMBDown = false;
                    m_Viewport.GetCamera().EndFly();
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
                if (e.button.button == SDL_BUTTON_MIDDLE) m_MMBDown = false;
            }
            if (e.type == SDL_MOUSEMOTION) {
                float dx = (float)e.motion.xrel;
                float dy = (float)e.motion.yrel;
                if (m_RMBDown) {
                    m_Viewport.GetCamera().FlyLook(dx, dy);
                } else if (m_MMBDown) {
                    m_Viewport.GetCamera().Pan(-dx, dy);
                }
            }
            if (e.type == SDL_MOUSEWHEEL) {
                if (m_RMBDown) {
                    // Adjust fly speed with scroll while flying
                    m_Viewport.GetCamera().flySpeed *= (1.0f + e.wheel.y * 0.15f);
                    if (m_Viewport.GetCamera().flySpeed < 500.0f) m_Viewport.GetCamera().flySpeed = 500.0f;
                    if (m_Viewport.GetCamera().flySpeed > 100000.0f) m_Viewport.GetCamera().flySpeed = 100000.0f;
                } else {
                    m_Viewport.GetCamera().Zoom((float)e.wheel.y);
                }
            }
        }

        // Keyboard shortcuts
        if (!io.WantCaptureKeyboard && e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_o && (e.key.keysym.mod & KMOD_CTRL))
                OpenFileDialog();
            if (e.key.keysym.sym == SDLK_s && (e.key.keysym.mod & KMOD_CTRL)) {
                if (m_Document.IsLoaded())
                    m_Document.Save(m_Document.GetFilePath());
            }
            if (e.key.keysym.sym == SDLK_f && m_SelectedActor >= 0) {
                auto& a = m_Document.GetActors()[m_SelectedActor];
                m_Viewport.GetCamera().FocusOn(a.location);
            }
            if (e.key.keysym.sym == SDLK_DELETE && m_SelectedActor >= 0) {
                DeleteSelectedActor();
            }
            // Draw radius: Z toggles, [ and ] adjust radius
            if (e.key.keysym.sym == SDLK_z && !(e.key.keysym.mod & KMOD_CTRL)) {
                m_Viewport.m_DrawRadiusEnabled = !m_Viewport.m_DrawRadiusEnabled;
            }
            // Zone filter: V toggles zone-based BSP visibility
            if (e.key.keysym.sym == SDLK_v && !(e.key.keysym.mod & KMOD_CTRL)) {
                m_Viewport.m_ZoneFilterEnabled = !m_Viewport.m_ZoneFilterEnabled;
                printf("[App] Zone filter %s\n", m_Viewport.m_ZoneFilterEnabled ? "ON" : "OFF");
                fflush(stdout);
            }
            // Wireframe BSP: F1 toggles
            if (e.key.keysym.sym == SDLK_F1) {
                m_Viewport.m_WireframeBSP = !m_Viewport.m_WireframeBSP;
                printf("[App] BSP wireframe %s\n", m_Viewport.m_WireframeBSP ? "ON" : "OFF");
                fflush(stdout);
            }
            if (e.key.keysym.sym == SDLK_LEFTBRACKET) {
                m_Viewport.m_DrawRadius = (m_Viewport.m_DrawRadius - 1000.0f > 1000.0f) ? m_Viewport.m_DrawRadius - 1000.0f : 1000.0f;
                printf("[App] Draw radius: %.0f\n", m_Viewport.m_DrawRadius);
                fflush(stdout);
            }
            if (e.key.keysym.sym == SDLK_RIGHTBRACKET) {
                m_Viewport.m_DrawRadius += 1000.0f;
                printf("[App] Draw radius: %.0f\n", m_Viewport.m_DrawRadius);
                fflush(stdout);
            }
            // Section clip: C toggles, PageUp/PageDown adjust height
            if (e.key.keysym.sym == SDLK_c && !(e.key.keysym.mod & KMOD_CTRL)) {
                m_Viewport.m_ClipEnabled = !m_Viewport.m_ClipEnabled;
                if (m_Viewport.m_ClipEnabled) {
                    // Clip to camera Z ± 500 units (single floor)
                    float camZ = m_Viewport.GetCamera().GetPosition().z;
                    m_Viewport.m_ClipMinZ = camZ - 500.0f;
                    m_Viewport.m_ClipMaxZ = camZ + 500.0f;
                }
                printf("[App] Section clip %s (Z: %.0f to %.0f)\n",
                       m_Viewport.m_ClipEnabled ? "ON" : "OFF",
                       m_Viewport.m_ClipMinZ, m_Viewport.m_ClipMaxZ);
                fflush(stdout);
            }
            if (e.key.keysym.sym == SDLK_PAGEUP) {
                m_Viewport.m_ClipEnabled = true;
                m_Viewport.m_ClipMaxZ += 1000.0f;
                m_Viewport.m_ClipMinZ += 1000.0f;
            }
            if (e.key.keysym.sym == SDLK_PAGEDOWN) {
                m_Viewport.m_ClipEnabled = true;
                m_Viewport.m_ClipMaxZ -= 1000.0f;
                m_Viewport.m_ClipMinZ -= 1000.0f;
            }
        }
    }
}

void App::Update(float dt)
{
    // FPS fly movement (WASD + QE for up/down, Shift for boost)
    if (m_RMBDown && m_Viewport.GetCamera().flyMode) {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        float fwd = 0, right = 0, up = 0;
        if (keys[SDL_SCANCODE_W]) fwd += 1.0f;
        if (keys[SDL_SCANCODE_S]) fwd -= 1.0f;
        if (keys[SDL_SCANCODE_D]) right += 1.0f;
        if (keys[SDL_SCANCODE_A]) right -= 1.0f;
        if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_SPACE]) up += 1.0f;
        if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_LCTRL]) up -= 1.0f;
        float speedMul = keys[SDL_SCANCODE_LSHIFT] ? 3.0f : 1.0f;
        if (fwd != 0 || right != 0 || up != 0) {
            m_Viewport.GetCamera().FlyMove(fwd * speedMul, right * speedMul, up * speedMul, dt);
        }
    }
}

void App::RenderUI()
{
    // Main menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) OpenFileDialog();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, m_Document.IsLoaded()))
                m_Document.Save(m_Document.GetFilePath());
            if (ImGui::MenuItem("Save As...", nullptr, false, m_Document.IsLoaded())) {
                // TODO: save-as dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) m_Running = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Draw Radius", "Z", &m_Viewport.m_DrawRadiusEnabled);
            if (m_Viewport.m_DrawRadiusEnabled) {
                ImGui::SliderFloat("Radius", &m_Viewport.m_DrawRadius, 1000.0f, 50000.0f);
            }
            ImGui::MenuItem("Zone Filter", "V", &m_Viewport.m_ZoneFilterEnabled);
            ImGui::MenuItem("BSP Wireframe", "F1", &m_Viewport.m_WireframeBSP);
            if (m_Viewport.m_ZoneFilterEnabled) {
                ImGui::Text("Camera Zone: %d", m_Viewport.m_CameraZone);
            }
            ImGui::Separator();
            ImGui::MenuItem("Section Clip", "C", &m_Viewport.m_ClipEnabled);
            if (m_Viewport.m_ClipEnabled) {
                ImGui::SliderFloat("Clip Max Z", &m_Viewport.m_ClipMaxZ, -50000.0f, 50000.0f);
                ImGui::SliderFloat("Clip Min Z", &m_Viewport.m_ClipMinZ, -50000.0f, 50000.0f);
                if (ImGui::Button("Reset Clip")) {
                    m_Viewport.m_ClipMinZ = -500000.0f;
                    m_Viewport.m_ClipMaxZ = 500000.0f;
                }
            }
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowDemo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("BS1 Level Editor v0.2");
            ImGui::Separator();
            ImGui::Text("Hold RMB + WASD: Fly camera");
            ImGui::Text("  Shift: Move faster | Scroll: Adjust speed");
            ImGui::Text("  E/Space: Up | Q/Ctrl: Down");
            ImGui::Text("MMB drag: Pan | Scroll: Zoom (orbit)");
            ImGui::Separator();
            ImGui::Text("F: Focus on selected");
            ImGui::Text("Delete: Remove selected actor");
            ImGui::Text("Double-click mesh in Content Browser: Spawn");
            ImGui::Text("Ctrl+O: Open | Ctrl+S: Save");
            ImGui::Separator();
            ImGui::Text("C: Toggle section clip (show one floor)");
            ImGui::Text("PageUp/Down: Move clip plane up/down");
            ImGui::Text("View menu: Fine-tune clip range");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Dockspace
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_PassthruCentralNode);

    // Panels
    m_SceneTree.Render(m_Document, m_SelectedActor);
    m_PropertiesPanel.Render(m_Document, m_SelectedActor);
    m_ContentBrowser.Render();

    // Handle spawn request from content browser
    if (m_ContentBrowser.WantsSpawn()) {
        SpawnMeshActor(m_ContentBrowser.GetSpawnMesh());
        m_ContentBrowser.ConsumeSpawn();
    }

    // Status bar
    ImGui::Begin("Status");
    if (m_Document.IsLoaded()) {
        Camera& cam = m_Viewport.GetCamera();
        Vec3 pos = cam.GetPosition();
        ImGui::Text("Map: %s | Actors: %d | Selected: %d",
                    m_Document.GetMapName().c_str(), m_Document.GetActorCount(), m_SelectedActor);
        ImGui::SameLine(0, 20);
        if (cam.flyMode) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "FLY (%.0f u/s)", cam.flySpeed);
        }
        ImGui::Text("Pos: %.0f, %.0f, %.0f", pos.x, pos.y, pos.z);
        if (m_Viewport.m_DrawRadiusEnabled) {
            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "RADIUS:%.0f", m_Viewport.m_DrawRadius);
        }
        if (m_Viewport.m_ZoneFilterEnabled) {
            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "ZONE:%d", m_Viewport.m_CameraZone);
        }
        if (m_Viewport.m_ClipEnabled) {
            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "CLIP Z:[%.0f..%.0f]",
                               m_Viewport.m_ClipMinZ, m_Viewport.m_ClipMaxZ);
        }
        if (m_Viewport.m_WireframeBSP) {
            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 1.0f, 1.0f), "WIRE");
        }
    } else {
        ImGui::Text("No map loaded. Press Ctrl+O to open a .bsm file.");
    }
    ImGui::End();

    if (m_ShowDemo) ImGui::ShowDemoWindow(&m_ShowDemo);
}

void App::OpenFileDialog()
{
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "BioShock Maps (*.bsm)\0*.bsm\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Open BioShock Map";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrInitialDir = "D:\\SteamLibrary\\steamapps\\common\\BioShock Remastered\\ContentBaked\\pc\\Maps";

    if (GetOpenFileNameA(&ofn)) {
        printf("[App] Loading: %s\n", filename);

        // Show loading overlay
        std::string shortName = filename;
        { size_t sl = shortName.find_last_of("\\/"); if (sl != std::string::npos) shortName = shortName.substr(sl + 1); }
        char loadMsg[256];
        snprintf(loadMsg, sizeof(loadMsg), "Loading %s...", shortName.c_str());
        RenderLoadingFrame(loadMsg);

        m_Document.Load(filename);
        m_SelectedActor = -1;
        if (m_Document.IsLoaded()) {
            printf("[App] Loaded: %d actors\n", m_Document.GetActorCount());
            // Focus camera on center of valid actors
            auto& actors = m_Document.GetActors();
            if (!actors.empty()) {
                double cx = 0, cy = 0, cz = 0;
                int validCount = 0;
                for (auto& a : actors) {
                    // Skip actors with bogus coords
                    if (std::isnan(a.location.x) || std::isnan(a.location.y) || std::isnan(a.location.z)) continue;
                    if (std::abs(a.location.x) > 500000.0f || std::abs(a.location.y) > 500000.0f || std::abs(a.location.z) > 500000.0f) continue;
                    cx += a.location.x;
                    cy += a.location.y;
                    cz += a.location.z;
                    validCount++;
                }
                Vec3 center;
                if (validCount > 0) {
                    center = {(float)(cx/validCount), (float)(cy/validCount), (float)(cz/validCount)};
                } else {
                    center = actors[0].location;
                }
                printf("[App] Center: (%.0f, %.0f, %.0f) from %d valid actors\n",
                       center.x, center.y, center.z, validCount);
                printf("[App] First actor: '%s' at (%.0f, %.0f, %.0f)\n",
                       actors[0].className.c_str(),
                       actors[0].location.x, actors[0].location.y, actors[0].location.z);
                m_Viewport.GetCamera().FocusOn(center);
                m_Viewport.GetCamera().distance = 20000.0f;
            }
            // Upload meshes to GPU
            m_ExportDir = "Z:\\UEViewer\\export\\" + m_Document.GetMapName();
            { size_t dp = m_ExportDir.find_last_of('.'); if (dp != std::string::npos) m_ExportDir = m_ExportDir.substr(0, dp); }

            snprintf(loadMsg, sizeof(loadMsg), "Resolving textures for %s...", shortName.c_str());
            RenderLoadingFrame(loadMsg, 0.5f);

            m_Document.ResolveTextures(m_ExportDir);
            std::string texDir2 = m_ExportDir + "\\Texture";

            snprintf(loadMsg, sizeof(loadMsg), "Uploading to GPU...");
            RenderLoadingFrame(loadMsg, 0.8f);

            m_Viewport.UploadMeshes(m_Document.GetMeshes(), texDir2);
            if (m_Document.HasBSP())
                m_Viewport.UploadBSP(m_Document.GetBSPMeshes(), texDir2);
            m_ContentBrowser.ScanDirectory(m_ExportDir);
        } else {
            printf("[App] FAILED to load!\n");
        }
    }
#endif
}

void App::SpawnMeshActor(const std::string& meshName)
{
    if (!m_Document.IsLoaded()) return;

    // Place at camera target or fly position
    Vec3 spawnPos = m_Viewport.GetCamera().flyMode
        ? m_Viewport.GetCamera().GetPosition()
        : m_Viewport.GetCamera().target;
    // Offset forward a bit from camera in fly mode
    if (m_Viewport.GetCamera().flyMode) {
        Vec3 fwd = m_Viewport.GetCamera().GetForward();
        spawnPos.x += fwd.x * 1000.0f;
        spawnPos.y += fwd.y * 1000.0f;
        spawnPos.z += fwd.z * 1000.0f;
    }

    EditorActor actor;
    actor.exportIndex = -1; // New actor, not from BSM
    actor.className = "StaticMeshActor";
    actor.objectName = meshName;
    actor.location = spawnPos;
    actor.rotation = {0, 0, 0};
    actor.scale = {1, 1, 1};
    actor.hasLocation = true;
    actor.visible = true;

    // Find mesh index
    auto& meshes = m_Document.GetMeshes();
    for (int i = 0; i < (int)meshes.size(); i++) {
        if (meshes[i].name == meshName) {
            actor.meshIndex = i;
            break;
        }
    }

    m_Document.GetActors().push_back(actor);
    m_SelectedActor = (int)m_Document.GetActors().size() - 1;
    printf("[App] Spawned '%s' at (%.0f, %.0f, %.0f)\n",
           meshName.c_str(), spawnPos.x, spawnPos.y, spawnPos.z);
}

void App::DeleteSelectedActor()
{
    if (!m_Document.IsLoaded() || m_SelectedActor < 0) return;
    auto& actors = m_Document.GetActors();
    if (m_SelectedActor >= (int)actors.size()) return;

    printf("[App] Deleted actor '%s' at index %d\n",
           actors[m_SelectedActor].objectName.c_str(), m_SelectedActor);
    actors.erase(actors.begin() + m_SelectedActor);
    m_SelectedActor = -1;
}

void App::RenderLoadingFrame(const char* message, float progress)
{
    // Pump events so the window doesn't appear frozen
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) m_Running = false;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Full-screen darkened overlay
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##LoadingOverlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs);

    // Center the loading content
    float winW = ImGui::GetWindowWidth();
    float winH = ImGui::GetWindowHeight();
    float textW = ImGui::CalcTextSize(message).x;
    ImGui::SetCursorPos(ImVec2((winW - textW) * 0.5f, winH * 0.42f));
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", message);

    // Progress bar
    if (progress >= 0.0f) {
        float barW = 400.0f;
        ImGui::SetCursorPos(ImVec2((winW - barW) * 0.5f, winH * 0.48f));
        ImGui::PushItemWidth(barW);
        ImGui::ProgressBar(progress, ImVec2(barW, 24));
        ImGui::PopItemWidth();
    } else {
        // Animated dots
        int dots = ((int)(SDL_GetTicks() / 400)) % 4;
        char dotStr[8] = "";
        for (int i = 0; i < dots; i++) dotStr[i] = '.';
        dotStr[dots] = '\0';
        float dotW = ImGui::CalcTextSize("...").x;
        ImGui::SetCursorPos(ImVec2((winW - dotW) * 0.5f, winH * 0.48f));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", dotStr);
    }

    ImGui::End();
    ImGui::Render();

    int w, h;
    SDL_GetWindowSize(m_Window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_Window);
}

void App::ReloadMap()
{
    if (!m_Document.IsLoaded()) return;
    std::string path = m_Document.GetFilePath();
    m_Document.Load(path);
    m_SelectedActor = -1;
    if (m_Document.IsLoaded() && !m_ExportDir.empty()) {
        m_Document.ResolveTextures(m_ExportDir);
        m_Viewport.UploadMeshes(m_Document.GetMeshes(), m_ExportDir + "\\Texture");
        if (m_Document.HasBSP())
            m_Viewport.UploadBSP(m_Document.GetBSPMeshes());
    }
}
