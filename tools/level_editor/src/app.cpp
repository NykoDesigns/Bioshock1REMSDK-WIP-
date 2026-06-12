#include "app.h"
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include "gl_funcs.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#endif

extern void LogMsg(const char* msg);

// Write raw DXT1 data as a .dds file (for lightmap verification)
static void WriteDXT1DDS(const std::string& path, const uint8_t* data, int w, int h)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return;
    // DDS magic
    uint32_t magic = 0x20534444; // "DDS "
    f.write((char*)&magic, 4);
    // DDS_HEADER (124 bytes)
    uint8_t hdr[124] = {};
    auto* h32 = (uint32_t*)hdr;
    h32[0] = 124;                          // dwSize
    h32[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000; // flags: CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE
    h32[2] = (uint32_t)h;                  // dwHeight
    h32[3] = (uint32_t)w;                  // dwWidth
    h32[4] = ((w + 3) / 4) * ((h + 3) / 4) * 8; // dwPitchOrLinearSize
    h32[5] = 0;                            // dwDepth
    h32[6] = 1;                            // dwMipMapCount
    // DDS_PIXELFORMAT at offset 76 (19 dwords from start)
    auto* pf = (uint32_t*)(hdr + 76);
    pf[0] = 32;                            // pfSize
    pf[1] = 0x4;                           // DDPF_FOURCC
    pf[2] = 0x31545844;                    // 'DXT1'
    // dwCaps at offset 108
    h32[27] = 0x1000;                      // DDSCAPS_TEXTURE
    f.write((char*)hdr, 124);
    // Mip0 data
    int mip0Size = ((w + 3) / 4) * ((h + 3) / 4) * 8;
    f.write((char*)data, mip0Size);
}

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

    // Thumbnail renderer for content browser mesh previews
    if (!m_ThumbnailRenderer.Init(128)) { LogMsg("[App] Thumbnail renderer init failed (non-fatal)"); }
    m_ContentBrowser.SetThumbnailRenderer(&m_ThumbnailRenderer);

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
        m_Viewport.GetTextureCache().AddAllMapTextureDirs("Z:\\UEViewer\\export");
        m_Viewport.UploadMeshes(m_Document.GetMeshes(), texDir);
        LogMsg("[App] Meshes uploaded");

        // Upload lightmap textures AFTER UploadMeshes (which clears cache) but BEFORE UploadBSP
        {
            std::string mapPath = m_Document.GetFilePath();
            size_t mapsPos = mapPath.find("Maps");
            if (mapsPos != std::string::npos) {
                std::string baseDir = mapPath.substr(0, mapsPos);
                std::string bdcPath = baseDir + "BulkContent\\Catalog.bdc";
                if (m_Catalog.Load(bdcPath)) {
                    printf("[App] Catalog loaded: %d entries\n", m_Catalog.GetEntryCount());
                    std::string mapShort = m_Document.GetMapName();
                    { size_t dp = mapShort.find_last_of('.'); if (dp != std::string::npos) mapShort = mapShort.substr(0, dp); }
                    auto lmEntries = m_Catalog.FindByPackage(mapShort);
                    printf("[App] Lightmap textures in '%s': %d\n", mapShort.c_str(), (int)lmEntries.size());
                    int uploaded = 0;
                    for (auto* entry : lmEntries) {
                        auto data = m_Catalog.ReadBulkData(entry->objectName, m_Catalog.GetBulkDir());
                        if (!data.empty() && data.size() >= 524288) {
                            std::string lmName = "LM_" + entry->objectName;
                            if (m_Viewport.GetTextureCache().UploadDXT1(lmName, data.data(), 1024, 1024))
                                uploaded++;
                        }
                    }
                    printf("[App] Uploaded %d/%d lightmap textures to GPU\n", uploaded, (int)lmEntries.size());

                    // ─── Bulk texture loading: load ALL textures from .blk ─────────────
                    // For textures that don't have TGA exports, load directly from bulk
                    // using metadata parsed from UTexture exports (Format + mip0 dims)
                    auto& texMeta = m_Document.GetTextureMetadata();
                    int bulkLoaded = 0, bulkSkipped = 0, bulkMissing = 0;
                    for (int ei = 0; ei < m_Catalog.GetEntryCount(); ei++) {
                        auto* entry = m_Catalog.GetEntry(ei);
                        if (!entry) continue;
                        // Skip lightmap textures (already handled above)
                        if (entry->objectName.find("Texture_") == 0 &&
                            entry->packageName.find("LightMaps") != std::string::npos) continue;

                        // Check if already in texture cache (TGA or prior upload)
                        if (m_Viewport.GetTextureCache().GetTexture(entry->objectName)) {
                            bulkSkipped++;
                            continue;
                        }

                        // Look up metadata for format + dimensions
                        auto mit = texMeta.find(entry->objectName);
                        if (mit == texMeta.end()) {
                            bulkMissing++;
                            continue;
                        }
                        auto& tm = mit->second;
                        // Only load formats we support
                        if (tm.format != 3 && tm.format != 7 && tm.format != 8 && tm.format != 12) continue;
                        if (tm.width <= 0 || tm.height <= 0) continue;

                        auto data = m_Catalog.ReadBulkData(entry->objectName, m_Catalog.GetBulkDir());
                        if (data.empty()) continue;
                        // Bulk data = all externalized mips concatenated largest-first
                        // We only need mip0 (first tm.mip0Size bytes)
                        if ((int)data.size() < tm.mip0Size) continue;

                        m_Viewport.GetTextureCache().UploadCompressed(
                            entry->objectName, data.data(), tm.width, tm.height, tm.format);
                        bulkLoaded++;
                    }
                    printf("[App] Bulk textures: %d loaded, %d already cached, %d no metadata\n",
                           bulkLoaded, bulkSkipped, bulkMissing);

                    // Re-link meshes that were untextured during initial upload
                    // (their textures may now be available from bulk loading)
                    if (bulkLoaded > 0) {
                        int relinked = m_Viewport.RelinkMeshTextures(m_Document.GetMeshes());
                        if (relinked > 0)
                            printf("[App] Re-linked %d meshes to bulk-loaded textures\n", relinked);
                    }
                }
            }
        }

        if (m_Document.HasBSP())
            m_Viewport.UploadBSP(m_Document.GetBSPMeshes(), texDir);
        m_ContentBrowser.ScanDirectory(m_ExportDir);
        m_ThumbnailRenderer.SetTextureCache(&m_Viewport.GetTextureCache());
        m_ThumbnailRenderer.SetTextureDir(texDir);
        m_ThumbnailRenderer.SetMeshes(m_Document.GetMeshes());

        // Extract point lights from BSM Light actors for viewport rendering
        m_Viewport.m_SceneLights.clear();
        for (auto& a : m_Document.GetActors()) {
            if (!a.isLight || !a.hasLocation) continue;
            SceneLight sl;
            sl.pos[0] = a.location.x;
            sl.pos[1] = a.location.y;
            sl.pos[2] = a.location.z;
            sl.color[0] = a.lightColorR / 255.0f;
            sl.color[1] = a.lightColorG / 255.0f;
            sl.color[2] = a.lightColorB / 255.0f;
            sl.radius = a.lightRadius;
            sl.brightness = a.lightBrightness;
            m_Viewport.m_SceneLights.push_back(sl);
        }
        printf("[App] Extracted %d point lights from BSM\n", (int)m_Viewport.m_SceneLights.size());
        fflush(stdout);
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
        if (dt > 0.0001f) m_FPS = m_FPS * 0.95f + (1.0f / dt) * 0.05f;

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

        // Mark multi-selected actors for rendering highlights
        if (m_Document.IsLoaded()) {
            auto& actors = m_Document.GetActors();
            for (int i = 0; i < (int)actors.size(); i++)
                actors[i].selected = m_SelectedActors.count(i) > 0;
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
                    m_LMBDown = true;
                    int w, h;
                    SDL_GetWindowSize(m_Window, &w, &h);
                    m_ViewportW = (float)w; m_ViewportH = (float)h;
                    // Try gizmo interaction first
                    bool gizmoHit = false;
                    if (m_SelectedActor >= 0 && m_Document.IsLoaded()) {
                        auto& actors = m_Document.GetActors();
                        if (m_SelectedActor < (int)actors.size() && actors[m_SelectedActor].hasLocation) {
                            Mat4 view = m_Viewport.GetCamera().GetViewMatrix();
                            float aspect = m_ViewportW / m_ViewportH;
                            Mat4 proj = m_Viewport.GetCamera().GetProjectionMatrix(aspect);
                            Mat4 vp_mat;
                            // Inline MulMat4 for vp
                            for (int ii = 0; ii < 4; ii++)
                                for (int jj = 0; jj < 4; jj++) {
                                    vp_mat.m[jj*4+ii] = 0;
                                    for (int kk = 0; kk < 4; kk++)
                                        vp_mat.m[jj*4+ii] += proj.m[kk*4+ii] * view.m[jj*4+kk];
                                }
                            gizmoHit = m_Viewport.BeginGizmoDrag(
                                actors[m_SelectedActor].location,
                                (float)e.button.x, (float)e.button.y,
                                m_ViewportW, m_ViewportH, vp_mat);
                        }
                    }
                    if (!gizmoHit) {
                        // Pick actor
                        int picked = m_Viewport.PickActor(m_Document,
                            (float)e.button.x, (float)e.button.y, m_ViewportW, m_ViewportH);
                        if (picked >= 0) {
                            bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
                            if (ctrl) {
                                // Toggle in multi-select
                                if (m_SelectedActors.count(picked))
                                    m_SelectedActors.erase(picked);
                                else
                                    m_SelectedActors.insert(picked);
                                m_SelectedActor = picked;
                            } else {
                                // Single select — clear multi-select
                                m_SelectedActors.clear();
                                m_SelectedActors.insert(picked);
                                m_SelectedActor = picked;
                            }
                        } else if (!(SDL_GetModState() & KMOD_CTRL)) {
                            // Clicked empty space — deselect
                            m_SelectedActor = -1;
                            m_SelectedActors.clear();
                        }
                    }
                }
            }
            if (e.type == SDL_MOUSEBUTTONUP) {
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    m_RMBDown = false;
                    m_Viewport.GetCamera().EndFly();
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
                if (e.button.button == SDL_BUTTON_MIDDLE) m_MMBDown = false;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    m_LMBDown = false;
                    if (m_Viewport.m_GizmoDragging && m_SelectedActor >= 0) {
                        // Finalize gizmo drag — push undo
                        auto& actors = m_Document.GetActors();
                        if (m_SelectedActor < (int)actors.size()) {
                            Vec3 oldPos = m_Viewport.m_GizmoDragStartPos;
                            Vec3 newPos = actors[m_SelectedActor].location;
                            if (oldPos.x != newPos.x || oldPos.y != newPos.y || oldPos.z != newPos.z) {
                                PushUndo(m_SelectedActor, oldPos, newPos);
                            }
                        }
                        m_Viewport.EndGizmoDrag();
                    }
                }
            }
            if (e.type == SDL_MOUSEMOTION) {
                float dx = (float)e.motion.xrel;
                float dy = (float)e.motion.yrel;
                if (m_RMBDown) {
                    m_Viewport.GetCamera().FlyLook(dx, dy);
                } else if (m_MMBDown) {
                    m_Viewport.GetCamera().Pan(-dx, dy);
                } else if (m_Viewport.m_GizmoDragging && m_SelectedActor >= 0 && m_Document.IsLoaded()) {
                    // Update gizmo drag
                    auto& actors = m_Document.GetActors();
                    if (m_SelectedActor < (int)actors.size()) {
                        Mat4 view = m_Viewport.GetCamera().GetViewMatrix();
                        float aspect = m_ViewportW / m_ViewportH;
                        Mat4 proj = m_Viewport.GetCamera().GetProjectionMatrix(aspect);
                        Mat4 vp_mat;
                        for (int ii = 0; ii < 4; ii++)
                            for (int jj = 0; jj < 4; jj++) {
                                vp_mat.m[jj*4+ii] = 0;
                                for (int kk = 0; kk < 4; kk++)
                                    vp_mat.m[jj*4+ii] += proj.m[kk*4+ii] * view.m[jj*4+kk];
                            }
                        Vec3 oldPos = actors[m_SelectedActor].location;
                        Vec3 newPos = m_Viewport.UpdateGizmoDrag(
                            actors[m_SelectedActor].location,
                            (float)e.motion.x, (float)e.motion.y,
                            m_ViewportW, m_ViewportH, vp_mat);
                        // Apply snap-to-grid
                        if (m_SnapGrid > 0) {
                            newPos.x = roundf(newPos.x / m_SnapGrid) * m_SnapGrid;
                            newPos.y = roundf(newPos.y / m_SnapGrid) * m_SnapGrid;
                            newPos.z = roundf(newPos.z / m_SnapGrid) * m_SnapGrid;
                        }
                        // Compute delta and move all selected actors
                        Vec3 delta = {newPos.x - oldPos.x, newPos.y - oldPos.y, newPos.z - oldPos.z};
                        actors[m_SelectedActor].location = newPos;
                        for (int si : m_SelectedActors) {
                            if (si != m_SelectedActor && si >= 0 && si < (int)actors.size()) {
                                actors[si].location.x += delta.x;
                                actors[si].location.y += delta.y;
                                actors[si].location.z += delta.z;
                            }
                        }
                    }
                } else if (!m_LMBDown && m_SelectedActor >= 0 && m_Document.IsLoaded()) {
                    // Update gizmo hover highlight
                    auto& actors = m_Document.GetActors();
                    if (m_SelectedActor < (int)actors.size() && actors[m_SelectedActor].hasLocation) {
                        Mat4 view = m_Viewport.GetCamera().GetViewMatrix();
                        float aspect = m_ViewportW / m_ViewportH;
                        Mat4 proj = m_Viewport.GetCamera().GetProjectionMatrix(aspect);
                        Mat4 vp_mat;
                        for (int ii = 0; ii < 4; ii++)
                            for (int jj = 0; jj < 4; jj++) {
                                vp_mat.m[jj*4+ii] = 0;
                                for (int kk = 0; kk < 4; kk++)
                                    vp_mat.m[jj*4+ii] += proj.m[kk*4+ii] * view.m[jj*4+kk];
                            }
                        m_Viewport.m_GizmoHoverAxis = m_Viewport.HitTestGizmo(
                            actors[m_SelectedActor].location,
                            (float)e.motion.x, (float)e.motion.y,
                            m_ViewportW, m_ViewportH, vp_mat);
                    }
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
            if (e.key.keysym.sym == SDLK_s && (e.key.keysym.mod & KMOD_CTRL) && !(e.key.keysym.mod & KMOD_SHIFT)) {
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
            // Escape: cancel gizmo drag or deselect
            if (e.key.keysym.sym == SDLK_ESCAPE) {
                if (m_Viewport.m_GizmoDragging && m_SelectedActor >= 0) {
                    // Cancel drag: restore original position
                    auto& actors = m_Document.GetActors();
                    if (m_SelectedActor < (int)actors.size())
                        actors[m_SelectedActor].location = m_Viewport.m_GizmoDragStartPos;
                    m_Viewport.EndGizmoDrag();
                } else {
                    m_SelectedActor = -1;
                }
            }
            // Ctrl+D: Duplicate selected actor
            if (e.key.keysym.sym == SDLK_d && (e.key.keysym.mod & KMOD_CTRL)) {
                DuplicateSelectedActor();
            }
            // F3: Toggle keyboard shortcuts overlay
            if (e.key.keysym.sym == SDLK_F3) {
                m_ShowShortcuts = !m_ShowShortcuts;
            }
            // G: Cycle snap-to-grid (off → 10 → 50 → 100 → 500 → off)
            if (e.key.keysym.sym == SDLK_g && !(e.key.keysym.mod & KMOD_CTRL) && !m_RMBDown) {
                if (m_SnapGrid <= 0) m_SnapGrid = 10;
                else if (m_SnapGrid < 25) m_SnapGrid = 50;
                else if (m_SnapGrid < 75) m_SnapGrid = 100;
                else if (m_SnapGrid < 250) m_SnapGrid = 500;
                else m_SnapGrid = 0;
            }
            // Ctrl+Shift+S: Save layout JSON
            if (e.key.keysym.sym == SDLK_s && (e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.mod & KMOD_SHIFT)) {
                SaveLayoutDialog();
            }
            // Ctrl+Shift+L: Load layout JSON
            if (e.key.keysym.sym == SDLK_l && (e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.mod & KMOD_SHIFT)) {
                LoadLayoutDialog();
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
            // Gizmo mode: W=Translate, E=Rotate, R=Scale (only when not flying)
            if (!m_RMBDown) {
                if (e.key.keysym.sym == SDLK_w && !(e.key.keysym.mod & KMOD_CTRL))
                    m_Viewport.m_GizmoMode = GizmoMode::Translate;
                if (e.key.keysym.sym == SDLK_e && !(e.key.keysym.mod & KMOD_CTRL))
                    m_Viewport.m_GizmoMode = GizmoMode::Rotate;
                if (e.key.keysym.sym == SDLK_r && !(e.key.keysym.mod & KMOD_CTRL))
                    m_Viewport.m_GizmoMode = GizmoMode::Scale;
            }
            // Undo: Ctrl+Z
            if (e.key.keysym.sym == SDLK_z && (e.key.keysym.mod & KMOD_CTRL)) {
                Undo();
            }
            // Redo: Ctrl+Y
            if (e.key.keysym.sym == SDLK_y && (e.key.keysym.mod & KMOD_CTRL)) {
                Redo();
            }
            // View mode cycle: F2 (Lit → Unlit → Wireframe → LitNoTexture → SurfaceID → ZoneColor → LMScale)
            if (e.key.keysym.sym == SDLK_F2) {
                int mode = ((int)m_Viewport.m_ViewMode + 1) % (int)ViewMode::COUNT;
                m_Viewport.m_ViewMode = (ViewMode)mode;
                const char* modeNames[] = {"Lit", "Unlit", "Wireframe", "Lit (No Texture)",
                                           "Surface ID", "Zone Color", "Lightmap Scale"};
                printf("[App] View mode: %s\n", modeNames[mode]);
                fflush(stdout);
            }
            // Game Preview mode: P key
            if (e.key.keysym.sym == SDLK_p && !(SDL_GetModState() & KMOD_CTRL) && !m_RMBDown) {
                m_Viewport.m_GamePreview = !m_Viewport.m_GamePreview;
                printf("[App] Game Preview: %s\n", m_Viewport.m_GamePreview ? "ON" : "OFF");
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
            // Copy: Ctrl+C
            if (e.key.keysym.sym == SDLK_c && (e.key.keysym.mod & KMOD_CTRL)) {
                CopySelection();
            }
            // Paste: Ctrl+V
            if (e.key.keysym.sym == SDLK_v && (e.key.keysym.mod & KMOD_CTRL)) {
                PasteSelection();
            }
            // Select All: Ctrl+A
            if (e.key.keysym.sym == SDLK_a && (e.key.keysym.mod & KMOD_CTRL)) {
                SelectAll();
            }
            // Orthographic toggle: T (not while flying)
            if (e.key.keysym.sym == SDLK_t && !m_RMBDown && !(e.key.keysym.mod & KMOD_CTRL)) {
                m_Viewport.m_Orthographic = !m_Viewport.m_Orthographic;
                printf("[App] %s projection\n", m_Viewport.m_Orthographic ? "Orthographic" : "Perspective");
                fflush(stdout);
            }
            // Align to grid: Ctrl+G
            if (e.key.keysym.sym == SDLK_g && (e.key.keysym.mod & KMOD_CTRL)) {
                AlignSelectionToGrid();
            }
            // Actor spawner palette: F4
            if (e.key.keysym.sym == SDLK_F4) {
                m_ShowSpawner = !m_ShowSpawner;
            }
            // F5: Toggle game bridge connection
            if (e.key.keysym.sym == SDLK_F5) {
                if (m_BridgeClient.IsConnected())
                    m_BridgeClient.Disconnect();
                else
                    m_BridgeClient.Connect();
            }
            // Camera bookmarks: Ctrl+0-9 to save, 0-9 to recall (not while flying)
            if (!m_RMBDown) {
                for (int bk = 0; bk <= 9; bk++) {
                    SDL_Keycode numKey = (bk == 0) ? SDLK_0 : (SDL_Keycode)(SDLK_1 + bk - 1);
                    if (e.key.keysym.sym == numKey) {
                        if (e.key.keysym.mod & KMOD_CTRL) {
                            // Save bookmark
                            auto& cam = m_Viewport.GetCamera();
                            m_Bookmarks[bk].pos = cam.GetPosition();
                            m_Bookmarks[bk].yaw = cam.yaw;
                            m_Bookmarks[bk].pitch = cam.pitch;
                            m_Bookmarks[bk].dist = cam.distance;
                            m_Bookmarks[bk].valid = true;
                            printf("[App] Bookmark %d saved\n", bk);
                        } else if (m_Bookmarks[bk].valid && !(e.key.keysym.mod & KMOD_SHIFT)) {
                            // Recall bookmark
                            auto& cam = m_Viewport.GetCamera();
                            cam.FocusOn(m_Bookmarks[bk].pos);
                            cam.yaw = m_Bookmarks[bk].yaw;
                            cam.pitch = m_Bookmarks[bk].pitch;
                            cam.distance = m_Bookmarks[bk].dist;
                        }
                        break;
                    }
                }
            }
        }
    }
}

void App::Update(float dt)
{
    // Poll live game bridge
    m_BridgeClient.Poll();

    // Progressive thumbnail generation (4 per frame)
    m_ThumbnailRenderer.GenerateBatch(4);

    // Autosave timer
    if (m_AutosaveEnabled && m_Document.IsLoaded() && !m_LayoutPath.empty()) {
        m_AutosaveTimer += dt;
        if (m_AutosaveTimer >= m_AutosaveInterval) {
            m_AutosaveTimer = 0;
            std::string autoPath = m_LayoutPath + ".autosave.json";
            SaveLayoutJSON(autoPath);
            printf("[App] Autosaved to %s\n", autoPath.c_str());
        }
    }

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
            if (!m_RecentFiles.empty() && ImGui::BeginMenu("Recent Files")) {
                for (auto& rf : m_RecentFiles) {
                    // Show just filename
                    size_t slash = rf.find_last_of("\\/");
                    const char* fname = (slash != std::string::npos) ? rf.c_str() + slash + 1 : rf.c_str();
                    if (ImGui::MenuItem(fname)) {
                        // Will load in next frame
                        m_Document.Load(rf);
                        m_SelectedActor = -1;
                        if (m_Document.IsLoaded()) AddRecentFile(rf);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) m_Running = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_UndoIndex >= 0)) Undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_UndoIndex < (int)m_UndoStack.size() - 1)) Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, m_SelectedActor >= 0)) CopySelection();
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_CopyBuffer.empty())) PasteSelection();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_SelectedActor >= 0)) DuplicateSelectedActor();
            if (ImGui::MenuItem("Delete", "Del", false, m_SelectedActor >= 0)) DeleteSelectedActor();
            if (ImGui::MenuItem("Select All", "Ctrl+A", false, m_Document.IsLoaded())) SelectAll();
            if (ImGui::MenuItem("Deselect", "Esc", false, m_SelectedActor >= 0)) { m_SelectedActor = -1; m_SelectedActors.clear(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Translate (W)", nullptr, m_Viewport.m_GizmoMode == GizmoMode::Translate))
                m_Viewport.m_GizmoMode = GizmoMode::Translate;
            if (ImGui::MenuItem("Rotate (E)", nullptr, m_Viewport.m_GizmoMode == GizmoMode::Rotate))
                m_Viewport.m_GizmoMode = GizmoMode::Rotate;
            if (ImGui::MenuItem("Scale (R)", nullptr, m_Viewport.m_GizmoMode == GizmoMode::Scale))
                m_Viewport.m_GizmoMode = GizmoMode::Scale;
            ImGui::Separator();
            {
                const char* snapLabels[] = {"Off", "10", "50", "100", "500"};
                float snapVals[] = {0, 10, 50, 100, 500};
                for (int i = 0; i < 5; i++) {
                    char label[64];
                    snprintf(label, sizeof(label), "Snap: %s", snapLabels[i]);
                    if (ImGui::MenuItem(label, i == 0 ? "G" : nullptr, m_SnapGrid == snapVals[i]))
                        m_SnapGrid = snapVals[i];
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layout")) {
            if (ImGui::MenuItem("Save Layout...", "Ctrl+Shift+S", false, m_Document.IsLoaded()))
                SaveLayoutDialog();
            if (ImGui::MenuItem("Load Layout...", "Ctrl+Shift+L", false, m_Document.IsLoaded()))
                LoadLayoutDialog();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            // View mode radio buttons
            const char* modeNames[] = {"Lit", "Unlit", "Wireframe", "Lit (No Texture)",
                                       "Surface ID", "Zone Color", "Lightmap Scale"};
            const char* modeKeys[] = {"F2", "F2", "F2", "F2", "F2", "F2", "F2"};
            for (int i = 0; i < (int)ViewMode::COUNT; i++) {
                if (i == 4) ImGui::Separator(); // separator before debug modes
                bool selected = (m_Viewport.m_ViewMode == (ViewMode)i);
                if (ImGui::MenuItem(modeNames[i], modeKeys[i], selected)) {
                    m_Viewport.m_ViewMode = (ViewMode)i;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Game Preview", "P", m_Viewport.m_GamePreview)) {
                m_Viewport.m_GamePreview = !m_Viewport.m_GamePreview;
            }
            ImGui::MenuItem("Dark Preview (BioShock)", nullptr, &m_Viewport.m_DarkPreview);
            ImGui::MenuItem("Missing Material Debug", nullptr, &m_Viewport.m_MissingMaterialDebug);
            ImGui::MenuItem("Show Decals", nullptr, &m_Viewport.m_ShowDecals);
            ImGui::Separator();
            ImGui::MenuItem("Draw Radius", "Z", &m_Viewport.m_DrawRadiusEnabled);
            if (m_Viewport.m_DrawRadiusEnabled) {
                ImGui::SliderFloat("Radius", &m_Viewport.m_DrawRadius, 1000.0f, 50000.0f);
            }
            ImGui::MenuItem("Zone Filter", "V", &m_Viewport.m_ZoneFilterEnabled);
            ImGui::MenuItem("BSP Wireframe", "F1", &m_Viewport.m_WireframeBSP);
            ImGui::MenuItem("Show Light Icons", nullptr, &m_Viewport.m_ShowLightIcons);
            ImGui::MenuItem("Show Actor Icons", nullptr, &m_Viewport.m_ShowActorIcons);
            ImGui::MenuItem("Shadows", nullptr, &m_Viewport.m_ShadowEnabled);
            ImGui::MenuItem("Orthographic", "T", &m_Viewport.m_Orthographic);
            if (m_Viewport.m_Orthographic) {
                ImGui::SliderFloat("Ortho Size", &m_Viewport.m_OrthoSize, 1000.0f, 100000.0f);
            }
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
            ImGui::MenuItem("Actor Spawner", "F4", &m_ShowSpawner);
            ImGui::MenuItem("Actor Report", nullptr, &m_ShowActorReport);
            ImGui::MenuItem("Nearby Actor Inspector", nullptr, &m_ShowNearbyInspector);
            ImGui::MenuItem("Target Area Report", nullptr, &m_ShowTargetAreaReport);
            ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowDemo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Game")) {
            bool connected = m_BridgeClient.IsConnected();
            if (!connected) {
                if (ImGui::MenuItem("Connect to Game", "F5"))
                    m_BridgeClient.Connect();
            } else {
                char buf[128];
                snprintf(buf, sizeof(buf), "Connected (%.0fms) | tx=%d rx=%d",
                         m_BridgeClient.GetLatencyMs(),
                         m_BridgeClient.GetMessagesSent(),
                         m_BridgeClient.GetMessagesReceived());
                ImGui::TextDisabled("%s", buf);
                if (ImGui::MenuItem("Disconnect"))
                    m_BridgeClient.Disconnect();
                ImGui::Separator();
                if (ImGui::MenuItem("Sync Player Position"))
                    m_BridgeClient.SendQueryPlayer();
                if (ImGui::MenuItem("Ping"))
                    m_BridgeClient.SendPing();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("BS1 Level Editor v0.6");
            ImGui::Separator();
            if (ImGui::MenuItem("Keyboard Shortcuts", "F3"))
                m_ShowShortcuts = !m_ShowShortcuts;
            ImGui::Separator();
            ImGui::TextDisabled("Quick Reference:");
            ImGui::Text("RMB+WASD: Fly | MMB: Pan | Scroll: Zoom");
            ImGui::Text("W/E/R: Translate/Rotate/Scale");
            ImGui::Text("Ctrl+C/V: Copy/Paste | Ctrl+D: Duplicate");
            ImGui::Text("Ctrl+A: Select All | Del: Delete | Esc: Deselect");
            ImGui::Text("Ctrl+Z/Y: Undo/Redo | G: Snap grid");
            ImGui::Text("T: Ortho | Ctrl+0-9: Save bookmark | 0-9: Recall");
            ImGui::Text("F4: Spawner | Ctrl+G: Align to grid");
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

    // Lighting panel
    {
        auto& L = m_Viewport.m_Lighting;
        ImGui::Begin("Lighting");
        if (ImGui::CollapsingHeader("Directional Light (Sun)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##sun", &L.sunEnabled);
            ImGui::SliderFloat("Yaw##sun", &L.sunYaw, -180.0f, 180.0f);
            ImGui::SliderFloat("Pitch##sun", &L.sunPitch, -90.0f, 90.0f);
            ImGui::ColorEdit3("Color##sun", L.sunColor);
            ImGui::SliderFloat("Intensity##sun", &L.sunIntensity, 0.0f, 3.0f);
        }
        if (ImGui::CollapsingHeader("Fill Light")) {
            ImGui::ColorEdit3("Color##fill", L.fillColor);
            ImGui::SliderFloat("Intensity##fill", &L.fillIntensity, 0.0f, 2.0f);
        }
        if (ImGui::CollapsingHeader("Ambient", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::ColorEdit3("Sky##amb", L.ambientSkyColor);
            ImGui::ColorEdit3("Ground##amb", L.ambientGroundColor);
            ImGui::SliderFloat("Intensity##amb", &L.ambientIntensity, 0.0f, 3.0f);
        }
        if (ImGui::CollapsingHeader("Fog")) {
            ImGui::Checkbox("Enabled##fog", &L.fogEnabled);
            ImGui::ColorEdit3("Color##fog", L.fogColor);
            ImGui::DragFloat("Start##fog", &L.fogStart, 100.0f, 0.0f, 50000.0f);
            ImGui::DragFloat("End##fog", &L.fogEnd, 100.0f, 100.0f, 100000.0f);
        }
        if (ImGui::CollapsingHeader("Post Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Exposure", &L.exposure, 0.1f, 5.0f);
            ImGui::SliderFloat("Gamma", &L.gamma, 1.0f, 3.0f);
            ImGui::Checkbox("ACES Tone Mapping", &L.toneMapping);
        }
        if (ImGui::CollapsingHeader("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Use Level Lights", &L.useLevelLights);
            ImGui::SliderFloat("Multiplier##pl", &L.pointLightMultiplier, 0.0f, 5.0f);
            ImGui::Text("%d lights in scene", (int)m_Viewport.m_SceneLights.size());
        }
        if (ImGui::CollapsingHeader("Shadow Mapping")) {
            ImGui::Checkbox("Enabled##shadow", &m_Viewport.m_ShadowEnabled);
            int sizes[] = {512, 1024, 2048, 4096};
            const char* sizeLabels[] = {"512", "1024", "2048", "4096"};
            int curSize = 2; // default 2048
            for (int s = 0; s < 4; s++) if (sizes[s] == m_Viewport.m_ShadowMapSize) curSize = s;
            if (ImGui::Combo("Resolution##shadow", &curSize, sizeLabels, 4)) {
                m_Viewport.m_ShadowMapSize = sizes[curSize];
                // Resize shadow map texture
                glBindTexture(GL_TEXTURE_2D, m_Viewport.m_ShadowDepthTex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                    m_Viewport.m_ShadowMapSize, m_Viewport.m_ShadowMapSize,
                    0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            }
        }
        if (ImGui::CollapsingHeader("Bloom")) {
            ImGui::Checkbox("Enabled##bloom", &m_Viewport.m_BloomEnabled);
            ImGui::SliderFloat("Threshold", &m_Viewport.m_BloomThreshold, 0.1f, 3.0f);
            ImGui::SliderFloat("Intensity##bloom", &m_Viewport.m_BloomIntensity, 0.0f, 2.0f);
        }
        if (ImGui::CollapsingHeader("Post-Processing")) {
            ImGui::Checkbox("FXAA (Anti-Aliasing)", &m_Viewport.m_FXAAEnabled);
            ImGui::Checkbox("SSAO", &m_Viewport.m_SSAOEnabled);
            if (m_Viewport.m_SSAOEnabled)
                ImGui::SliderFloat("AO Intensity", &m_Viewport.m_SSAOIntensity, 0.0f, 2.0f);
            ImGui::Checkbox("Vignette", &m_Viewport.m_VignetteEnabled);
            if (m_Viewport.m_VignetteEnabled)
                ImGui::SliderFloat("Vignette Strength", &m_Viewport.m_VignetteStrength, 0.5f, 4.0f);
            ImGui::Checkbox("Film Grain", &m_Viewport.m_FilmGrainEnabled);
            if (m_Viewport.m_FilmGrainEnabled)
                ImGui::SliderFloat("Grain Strength", &m_Viewport.m_FilmGrainStrength, 0.01f, 0.1f);
            ImGui::Checkbox("God Rays", &m_Viewport.m_GodRaysEnabled);
            if (m_Viewport.m_GodRaysEnabled) {
                ImGui::SliderFloat("Ray Density", &m_Viewport.m_GodRayDensity, 0.1f, 2.0f);
                ImGui::SliderFloat("Ray Intensity", &m_Viewport.m_GodRayIntensity, 0.0f, 3.0f);
                ImGui::SliderFloat("Ray Decay", &m_Viewport.m_GodRayDecay, 0.9f, 1.0f);
            }
            ImGui::Checkbox("Water Surfaces", &m_Viewport.m_WaterEnabled);
        }
        ImGui::Separator();
        // Presets
        if (ImGui::Button("BioShock Interior")) {
            L.sunEnabled = true; L.sunYaw = 45; L.sunPitch = -25;
            L.sunColor[0]=0.3f; L.sunColor[1]=0.35f; L.sunColor[2]=0.4f; L.sunIntensity=0.1f;
            L.fillColor[0]=0.1f; L.fillColor[1]=0.15f; L.fillColor[2]=0.25f; L.fillIntensity=0.05f;
            L.ambientSkyColor[0]=0.02f; L.ambientSkyColor[1]=0.03f; L.ambientSkyColor[2]=0.05f;
            L.ambientGroundColor[0]=0.01f; L.ambientGroundColor[1]=0.01f; L.ambientGroundColor[2]=0.015f;
            L.ambientIntensity=1.0f; L.fogEnabled=true;
            L.fogColor[0]=0.005f; L.fogColor[1]=0.01f; L.fogColor[2]=0.02f;
            L.fogStart=2000; L.fogEnd=15000; L.exposure=1.8f; L.gamma=2.2f; L.toneMapping=true;
            L.useLevelLights=true; L.pointLightMultiplier=3.0f;
            m_Viewport.m_BloomEnabled=true; m_Viewport.m_BloomThreshold=0.8f; m_Viewport.m_BloomIntensity=0.5f;
            m_Viewport.m_FXAAEnabled=true; m_Viewport.m_SSAOEnabled=true; m_Viewport.m_SSAOIntensity=1.0f;
            m_Viewport.m_VignetteEnabled=true; m_Viewport.m_VignetteStrength=2.0f;
            m_Viewport.m_FilmGrainEnabled=true; m_Viewport.m_FilmGrainStrength=0.02f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Bright Editor")) {
            L.sunEnabled = true; L.sunYaw = 45; L.sunPitch = -35;
            L.sunColor[0]=1; L.sunColor[1]=0.95f; L.sunColor[2]=0.85f; L.sunIntensity=0.7f;
            L.fillColor[0]=0.3f; L.fillColor[1]=0.35f; L.fillColor[2]=0.5f; L.fillIntensity=0.25f;
            L.ambientSkyColor[0]=0.12f; L.ambientSkyColor[1]=0.15f; L.ambientSkyColor[2]=0.22f;
            L.ambientGroundColor[0]=0.06f; L.ambientGroundColor[1]=0.05f; L.ambientGroundColor[2]=0.04f;
            L.ambientIntensity=1.0f; L.fogEnabled=false;
            L.exposure=1.2f; L.gamma=2.2f; L.toneMapping=true;
            L.useLevelLights=true; L.pointLightMultiplier=1.0f;
        }
        if (ImGui::Button("Noir")) {
            L.sunEnabled = true; L.sunYaw = -30; L.sunPitch = -50;
            L.sunColor[0]=0.7f; L.sunColor[1]=0.75f; L.sunColor[2]=1.0f; L.sunIntensity=0.5f;
            L.fillColor[0]=0.1f; L.fillColor[1]=0.1f; L.fillColor[2]=0.15f; L.fillIntensity=0.1f;
            L.ambientSkyColor[0]=0.03f; L.ambientSkyColor[1]=0.04f; L.ambientSkyColor[2]=0.06f;
            L.ambientGroundColor[0]=0.01f; L.ambientGroundColor[1]=0.01f; L.ambientGroundColor[2]=0.01f;
            L.ambientIntensity=1.0f; L.fogEnabled=true;
            L.fogColor[0]=0.005f; L.fogColor[1]=0.005f; L.fogColor[2]=0.01f;
            L.fogStart=200; L.fogEnd=5000; L.exposure=1.8f; L.gamma=2.2f; L.toneMapping=true;
            L.useLevelLights=true; L.pointLightMultiplier=3.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Flat Unlit")) {
            L.sunEnabled=false; L.fillIntensity=0;
            L.ambientSkyColor[0]=0.5f; L.ambientSkyColor[1]=0.5f; L.ambientSkyColor[2]=0.5f;
            L.ambientGroundColor[0]=0.5f; L.ambientGroundColor[1]=0.5f; L.ambientGroundColor[2]=0.5f;
            L.ambientIntensity=2.0f; L.fogEnabled=false;
            L.exposure=1.0f; L.gamma=2.2f; L.toneMapping=false;
            L.useLevelLights=false; L.pointLightMultiplier=0;
        }
        ImGui::End();
    }

    // Handle drag-and-drop from content browser into viewport
    {
        // Check if an ImGui drag-drop is active with our payload type
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        if (payload && payload->IsDataType("MESH_ASSET")) {
            // Show visual feedback: tinted overlay when dragging over viewport area
            ImGuiIO& io = ImGui::GetIO();
            if (!ImGui::IsAnyItemHovered() && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                // Mouse is over the viewport (not over any ImGui window)
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddRectFilled(ImVec2(io.MousePos.x - 30, io.MousePos.y - 10),
                                  ImVec2(io.MousePos.x + 120, io.MousePos.y + 14),
                                  IM_COL32(40, 40, 40, 200), 4.0f);
                const char* meshName = (const char*)payload->Data;
                fg->AddText(ImVec2(io.MousePos.x - 24, io.MousePos.y - 8),
                           IM_COL32(100, 220, 100, 255), meshName);

                // On mouse release, spawn the mesh
                if (ImGui::IsMouseReleased(0)) {
                    std::string name((const char*)payload->Data);
                    SpawnMeshActor(name);
                    printf("[App] Drag-dropped '%s' into viewport\n", name.c_str());
                }
            }
        }
    }

    // Handle spawn request from content browser (double-click fallback)
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
        // Show current view mode
        {
            const char* modeNames[] = {"LIT", "UNLIT", "WIREFRAME", "LIT(noTex)",
                                       "SURFACE ID", "ZONE COLOR", "LM SCALE"};
            ImVec4 modeColors[] = {
                {0.8f, 0.8f, 0.2f, 1.0f},
                {0.2f, 0.9f, 0.9f, 1.0f},
                {1.0f, 0.5f, 1.0f, 1.0f},
                {0.9f, 0.6f, 0.2f, 1.0f},
                {0.2f, 1.0f, 0.4f, 1.0f},
                {0.4f, 0.6f, 1.0f, 1.0f},
                {1.0f, 0.3f, 0.3f, 1.0f},
            };
            int mi = (int)m_Viewport.m_ViewMode;
            if (mi >= 0 && mi < 7) {
                ImGui::SameLine(0, 20);
                ImGui::TextColored(modeColors[mi], "[F2] %s", modeNames[mi]);
            }
        }
        // Game Preview indicator
        if (m_Viewport.m_GamePreview) {
            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "[P] GAME PREVIEW");
        }
        // Gizmo mode
        {
            const char* gizmoNames[] = {"[W] MOVE", "[E] ROTATE", "[R] SCALE"};
            ImVec4 gizmoColor = {0.4f, 0.8f, 1.0f, 1.0f};
            ImGui::SameLine(0, 20);
            ImGui::TextColored(gizmoColor, "%s", gizmoNames[(int)m_Viewport.m_GizmoMode]);
        }
        // FPS + light count
        ImGui::SameLine(0, 20);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%.0f FPS", m_FPS);
        if (!m_Viewport.m_SceneLights.empty()) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%d lights",
                (int)m_Viewport.m_SceneLights.size());
        }
        // Snap-to-grid indicator
        if (m_SnapGrid > 0) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.6f, 1.0f), "SNAP:%.0f", m_SnapGrid);
        }
        // Undo stack indicator
        if (!m_UndoStack.empty()) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.7f, 1.0f), "Undo:%d/%d",
                m_UndoIndex + 1, (int)m_UndoStack.size());
        }
        // Orthographic indicator
        if (m_Viewport.m_Orthographic) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 1.0f, 1.0f), "ORTHO");
        }
        // Shadow indicator
        if (m_Viewport.m_ShadowEnabled) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.6f, 1.0f), "SHADOW");
        }
        // Performance stats
        ImGui::SameLine(0, 12);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%dK tri %d draw",
            m_Viewport.m_TriCount / 1000, m_Viewport.m_DrawCalls);
        // Bookmarks
        {
            int bkCount = 0;
            for (int i = 0; i < 10; i++) if (m_Bookmarks[i].valid) bkCount++;
            if (bkCount > 0) {
                ImGui::SameLine(0, 12);
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "BK:%d", bkCount);
            }
        }
        // Multi-select count
        if (m_SelectedActors.size() > 1) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f), "SEL:%d", (int)m_SelectedActors.size());
        }
    } else {
        ImGui::Text("No map loaded. Press Ctrl+O to open a .bsm file.");
    }
    ImGui::End();

    if (m_ShowDemo) ImGui::ShowDemoWindow(&m_ShowDemo);

    // Actor Spawner Palette (F4)
    if (m_ShowSpawner) {
        ImGui::SetNextWindowSize(ImVec2(260, 400), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Actor Spawner", &m_ShowSpawner)) {
            ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "Spawn Actor by Class");
            ImGui::Separator();
            struct SpawnEntry { const char* label; const char* className; ImVec4 color; };
            SpawnEntry entries[] = {
                {"Point Light",   "PointLight",     {1.0f, 0.9f, 0.3f, 1.0f}},
                {"Spot Light",    "SpotLight",      {1.0f, 0.7f, 0.2f, 1.0f}},
                {"Player Start",  "PlayerStart",    {0.2f, 0.8f, 1.0f, 1.0f}},
                {"Trigger",       "Trigger",        {0.8f, 0.3f, 0.8f, 1.0f}},
                {"Emitter",       "Emitter",        {0.4f, 1.0f, 0.4f, 1.0f}},
                {"Sound",         "AmbientSound",   {0.5f, 0.5f, 1.0f, 1.0f}},
                {"Note",          "Note",           {0.9f, 0.9f, 0.5f, 1.0f}},
                {"BlockingVolume","BlockingVolume",  {0.7f, 0.3f, 0.3f, 1.0f}},
            };
            for (auto& se : entries) {
                ImGui::PushStyleColor(ImGuiCol_Text, se.color);
                if (ImGui::Button(se.label, ImVec2(-1, 28))) {
                    SpawnActorByClass(se.className);
                }
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();
    }

    // Actor Render Classification Report
    if (m_ShowActorReport && m_Document.IsLoaded()) {
        ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Actor Report", &m_ShowActorReport)) {
            auto& actors = m_Document.GetActors();
            int total = (int)actors.size();

            // Count by render type
            const char* rtNames[] = {"StaticMesh","Mover","Pickup","Decoration",
                                     "Emitter","Decal","Light","Collision",
                                     "Trigger","EditorOnly","Unknown"};
            ImVec4 rtColors[] = {
                {0.5f,0.8f,1.0f,1}, {0.3f,0.6f,1.0f,1}, {0.2f,1.0f,0.4f,1}, {0.8f,0.7f,0.3f,1},
                {0.2f,1.0f,1.0f,1}, {1.0f,0.5f,1.0f,1}, {1.0f,0.8f,0.2f,1}, {0.5f,0.5f,0.5f,1},
                {0.8f,0.8f,0.2f,1}, {0.4f,0.4f,0.4f,1}, {0.6f,0.3f,0.3f,1},
            };
            int counts[11] = {};
            int withMesh = 0, missingMesh = 0;
            for (auto& a : actors) {
                int idx = (int)a.renderType;
                if (idx >= 0 && idx < 11) counts[idx]++;
                bool hasMesh = a.meshIndex >= 0 && a.meshIndex < (int)m_Document.GetMeshes().size();
                if (IsVisibleInGame(a.renderType)) {
                    if (hasMesh) withMesh++;
                    else if (a.renderType != ActorRenderType::LightOnly &&
                             a.renderType != ActorRenderType::VisibleEmitter &&
                             a.renderType != ActorRenderType::VisibleDecal) missingMesh++;
                }
            }

            ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "Summary");
            ImGui::Text("Total actors: %d", total);
            ImGui::TextColored(ImVec4(0.3f,1,0.3f,1), "  Visible + mesh resolved: %d", withMesh);
            ImGui::TextColored(ImVec4(1,0.5f,0.2f,1), "  Visible but missing mesh: %d", missingMesh);
            ImGui::Separator();

            // Type breakdown
            ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "By Render Type");
            for (int i = 0; i < 11; i++) {
                if (counts[i] == 0) continue;
                ImGui::TextColored(rtColors[i], "  %-16s %d", rtNames[i], counts[i]);
            }
            ImGui::Separator();

            // Export buttons
            if (ImGui::Button("Export missing_material_meshes.csv")) {
                std::string csvPath = m_Document.GetFilePath();
                size_t dot = csvPath.rfind('.');
                if (dot != std::string::npos) csvPath = csvPath.substr(0, dot);
                csvPath += "_missing_material_meshes.csv";
                FILE* f = fopen(csvPath.c_str(), "w");
                if (f) {
                    fprintf(f, "MeshIndex,MeshName,TextureName,HasGPUTexture,UsedByActorCount\n");
                    // Count actor usage per mesh
                    std::unordered_map<int, int> meshUsage;
                    for (auto& a : actors) {
                        if (a.meshIndex >= 0) meshUsage[a.meshIndex]++;
                    }
                    for (int i = 0; i < (int)m_Document.GetMeshes().size(); i++) {
                        bool hasTex = m_Viewport.HasMeshTexture(i);
                        if (hasTex) continue; // only export missing
                        auto& mesh = m_Document.GetMeshes()[i];
                        fprintf(f, "%d,\"%s\",\"%s\",no,%d\n",
                                i, mesh.name.c_str(), mesh.textureName.c_str(),
                                meshUsage.count(i) ? meshUsage[i] : 0);
                    }
                    fclose(f);
                    printf("[App] Exported: %s\n", csvPath.c_str());
                    fflush(stdout);
                }
            }
            ImGui::Separator();

            // Filter controls
            static int filterType = -1; // -1 = all
            ImGui::Text("Filter: ");
            ImGui::SameLine();
            if (ImGui::SmallButton("All")) filterType = -1;
            ImGui::SameLine();
            if (ImGui::SmallButton("Missing Mesh")) filterType = 99;
            ImGui::SameLine();
            if (ImGui::SmallButton("Unknown")) filterType = 10;

            // Actor table
            if (ImGui::BeginChild("ActorTable", ImVec2(0, 0), true)) {
                for (int i = 0; i < (int)actors.size(); i++) {
                    auto& a = actors[i];
                    int rti = (int)a.renderType;
                    bool hasMesh = a.meshIndex >= 0 && a.meshIndex < (int)m_Document.GetMeshes().size();

                    if (filterType == 99) {
                        // Missing mesh: visible actors without mesh
                        if (!IsVisibleInGame(a.renderType)) continue;
                        if (hasMesh) continue;
                        if (a.renderType == ActorRenderType::LightOnly ||
                            a.renderType == ActorRenderType::VisibleEmitter ||
                            a.renderType == ActorRenderType::VisibleDecal) continue;
                    } else if (filterType >= 0 && filterType < 11) {
                        if (rti != filterType) continue;
                    }

                    ImVec4 col = (rti >= 0 && rti < 11) ? rtColors[rti] : ImVec4(0.5f,0.5f,0.5f,1);
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    char label[512];
                    const char* meshName = hasMesh ? m_Document.GetMeshes()[a.meshIndex].name.c_str() : 
                                           (!a.meshRefName.empty() ? a.meshRefName.c_str() : "—");
                    snprintf(label, sizeof(label), "[%d] %-24s %-20s %s %s",
                             i, a.className.c_str(), a.objectName.c_str(),
                             rtNames[rti], meshName);
                    if (ImGui::Selectable(label, m_SelectedActor == i)) {
                        m_SelectedActor = i;
                    }
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    // Nearby Actor Inspector
    if (m_ShowNearbyInspector && m_Document.IsLoaded()) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Nearby Actor Inspector", &m_ShowNearbyInspector)) {
            Vec3 camPos = m_Viewport.GetCamera().GetPosition();
            ImGui::Text("Center: (%.0f, %.0f, %.0f)", camPos.x, camPos.y, camPos.z);
            ImGui::SliderFloat("Radius", &m_NearbyRadius, 500.0f, 20000.0f);
            static char nearbyFilter[128] = "";
            ImGui::InputText("Filter (class/name)", nearbyFilter, sizeof(nearbyFilter));
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) nearbyFilter[0] = '\0';

            auto& actors = m_Document.GetActors();
            float rSq = m_NearbyRadius * m_NearbyRadius;

            // Gather nearby actors sorted by distance
            struct NearbyEntry { int idx; float dist; };
            std::vector<NearbyEntry> nearby;
            std::string filterStr(nearbyFilter);
            // Convert filter to lowercase for case-insensitive matching
            for (auto& c : filterStr) c = (char)tolower((unsigned char)c);
            for (int i = 0; i < (int)actors.size(); i++) {
                auto& a = actors[i];
                if (!a.hasLocation) continue;
                float dx = a.location.x - camPos.x;
                float dy = a.location.y - camPos.y;
                float dz = a.location.z - camPos.z;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > rSq) continue;
                // Apply text filter
                if (!filterStr.empty()) {
                    std::string cn = a.className; for (auto& c : cn) c = (char)tolower((unsigned char)c);
                    std::string on = a.objectName; for (auto& c : on) c = (char)tolower((unsigned char)c);
                    if (cn.find(filterStr) == std::string::npos && on.find(filterStr) == std::string::npos) continue;
                }
                nearby.push_back({i, sqrtf(d2)});
            }
            std::sort(nearby.begin(), nearby.end(), [](auto& a, auto& b) { return a.dist < b.dist; });

            ImGui::Text("Found %d actors within %.0f units", (int)nearby.size(), m_NearbyRadius);

            // Export CSV button
            if (ImGui::Button("Export nearby_actors_report.csv")) {
                std::string csvPath = m_Document.GetFilePath();
                size_t dot = csvPath.rfind('.');
                if (dot != std::string::npos) csvPath = csvPath.substr(0, dot);
                csvPath += "_nearby_actors_report.csv";
                FILE* f = fopen(csvPath.c_str(), "w");
                if (f) {
                    fprintf(f, "Index,Name,Class,LocationX,LocationY,LocationZ,Distance,RenderType,MeshResolved,MeshName,TextureResolved,HiddenReason,GamePreviewVisible\n");
                    const char* rtNames[] = {"VisibleStaticMesh","VisibleMover","VisiblePickup","VisibleDecoration",
                                             "VisibleEmitter","VisibleDecal","LightOnly","CollisionOnly",
                                             "TriggerOnly","EditorOnly","UnknownPlaceholder"};
                    for (auto& ne : nearby) {
                        auto& a = actors[ne.idx];
                        bool hasMesh = a.meshIndex >= 0 && a.meshIndex < (int)m_Document.GetMeshes().size();
                        const char* meshName = hasMesh ? m_Document.GetMeshes()[a.meshIndex].name.c_str() : "";
                        bool hasTex = hasMesh && m_Viewport.HasMeshTexture(a.meshIndex);
                        const char* hiddenReason = "";
                        if (!a.visible) hiddenReason = "user_hidden";
                        else if (!IsVisibleInGame(a.renderType)) {
                            if (a.renderType == ActorRenderType::TriggerOnly) hiddenReason = "trigger";
                            else if (a.renderType == ActorRenderType::EditorOnly) hiddenReason = "editor_only";
                            else if (a.renderType == ActorRenderType::CollisionOnly) hiddenReason = "collision";
                            else if (a.renderType == ActorRenderType::UnknownPlaceholder) hiddenReason = "unknown";
                        }
                        bool gpVisible = IsVisibleInGame(a.renderType) && a.visible;
                        int rti = (int)a.renderType;
                        fprintf(f, "%d,\"%s\",\"%s\",%.1f,%.1f,%.1f,%.1f,%s,%s,\"%s\",%s,%s,%s\n",
                                ne.idx, a.objectName.c_str(), a.className.c_str(),
                                a.location.x, a.location.y, a.location.z, ne.dist,
                                (rti >= 0 && rti < 11) ? rtNames[rti] : "?",
                                hasMesh ? "yes" : "no", meshName,
                                hasTex ? "yes" : "no", hiddenReason,
                                gpVisible ? "yes" : "no");
                    }
                    fclose(f);
                    printf("[App] Exported: %s (%d actors)\n", csvPath.c_str(), (int)nearby.size());
                    fflush(stdout);
                }
            }

            ImGui::Separator();
            // Table
            const char* rtNames[] = {"StaticMesh","Mover","Pickup","Decoration",
                                     "Emitter","Decal","Light","Collision",
                                     "Trigger","EditorOnly","Unknown"};
            if (ImGui::BeginChild("NearbyTable", ImVec2(0, 0), true)) {
                for (auto& ne : nearby) {
                    auto& a = actors[ne.idx];
                    bool hasMesh = a.meshIndex >= 0 && a.meshIndex < (int)m_Document.GetMeshes().size();
                    bool gpVisible = IsVisibleInGame(a.renderType) && a.visible;
                    int rti = (int)a.renderType;

                    ImVec4 col = gpVisible ? ImVec4(0.8f, 0.9f, 1.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, col);

                    char label[512];
                    snprintf(label, sizeof(label), "%.0f  %-22s %-20s %s %s %s",
                             ne.dist, a.className.c_str(), a.objectName.c_str(),
                             (rti >= 0 && rti < 11) ? rtNames[rti] : "?",
                             hasMesh ? "[mesh]" : "",
                             gpVisible ? "" : "[hidden]");
                    if (ImGui::Selectable(label, m_SelectedActor == ne.idx)) {
                        m_SelectedActor = ne.idx;
                    }
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    // Target Area Report (runtime vs editor comparison)
    if (m_ShowTargetAreaReport) {
        ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Target Area Report", &m_ShowTargetAreaReport)) {
            // Import button
            if (ImGui::Button("Import Scan JSON...")) {
#ifdef _WIN32
                char szFile[MAX_PATH] = "";
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) {
                    // Auto-detect format: full scan has "staticMeshes"
                    std::ifstream peek(szFile);
                    std::string first512(512, '\0');
                    peek.read(&first512[0], 512);
                    peek.close();
                    if (first512.find("staticMeshes") != std::string::npos ||
                        first512.find("totalGObjects") != std::string::npos) {
                        LoadRuntimeFullScanJSON(szFile);
                    } else {
                        LoadRuntimeScanJSON(szFile);
                    }
                }
#endif
            }
            ImGui::SameLine();
            if (ImGui::Button("Re-match")) MatchRuntimeToEditor();
            ImGui::SameLine();
            if (ImGui::Button("Import Unmatched")) {
                int count = ImportUnmatchedActors();
                printf("[App] Imported %d actors from runtime scan\n", count);
            }

            if (!m_RuntimeActors.empty() || !m_RuntimeMeshes.empty()) {
                ImGui::Separator();
                ImGui::Text("Map: %s | Player: (%.0f, %.0f, %.0f) | GObjects: %d",
                    m_RuntimeScanMap.c_str(), m_RuntimeScanPos[0], m_RuntimeScanPos[1], m_RuntimeScanPos[2],
                    m_RuntimeTotalGObjects);

                int matched = 0, unmatched = 0, hidden = 0, noLoc = 0;
                for (auto& ra : m_RuntimeActors) {
                    if (!ra.hasLocation) noLoc++;
                    else if (ra.matchedEditorActor >= 0) matched++;
                    else unmatched++;
                    if (ra.bHidden) hidden++;
                }
                ImGui::Text("Actors: %d | Matched: %d | Unmatched: %d | No-Loc: %d | Hidden: %d",
                    (int)m_RuntimeActors.size(), matched, unmatched, noLoc, hidden);
                if (!m_RuntimeMeshes.empty() || !m_RuntimeTextures.empty())
                    ImGui::Text("Meshes: %d | Textures: %d | Materials: %d",
                        (int)m_RuntimeMeshes.size(), (int)m_RuntimeTextures.size(), (int)m_RuntimeMaterials.size());

                // Filter
                static char tarFilter[128] = "";
                ImGui::InputText("Filter", tarFilter, sizeof(tarFilter));
                static bool showOnlyUnmatched = false;
                ImGui::SameLine();
                ImGui::Checkbox("Unmatched only", &showOnlyUnmatched);

                ImGui::Separator();

                // Tabs: Actors | Meshes | Textures | Materials
                if (ImGui::BeginTabBar("TARTabs")) {
                    // ─── Actors Tab ───
                    if (ImGui::BeginTabItem("Actors")) {
                        if (ImGui::BeginChild("TARActors", ImVec2(0, 0), true)) {
                            std::string filterStr(tarFilter);
                            for (auto& c : filterStr) c = (char)tolower((unsigned char)c);
                            bool docLoaded = m_Document.IsLoaded();
                            auto* edActorsPtr = docLoaded ? &m_Document.GetActors() : nullptr;

                            for (int i = 0; i < (int)m_RuntimeActors.size(); i++) {
                                auto& ra = m_RuntimeActors[i];
                                if (!filterStr.empty()) {
                                    std::string cn = ra.className; for (auto& c : cn) c = (char)tolower((unsigned char)c);
                                    std::string nm = ra.name; for (auto& c : nm) c = (char)tolower((unsigned char)c);
                                    std::string sm = ra.staticMesh; for (auto& c : sm) c = (char)tolower((unsigned char)c);
                                    if (cn.find(filterStr) == std::string::npos &&
                                        nm.find(filterStr) == std::string::npos &&
                                        sm.find(filterStr) == std::string::npos) continue;
                                }
                                if (showOnlyUnmatched && ra.matchedEditorActor >= 0) continue;

                                ImVec4 col;
                                if (!ra.hasLocation) col = ImVec4(0.6f, 0.6f, 0.3f, 1.0f);
                                else if (ra.bHidden) col = ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
                                else if (ra.matchedEditorActor >= 0) col = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
                                else col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                                ImGui::PushStyleColor(ImGuiCol_Text, col);

                                char label[512];
                                const char* matchStr = ra.hasLocation ? "NO MATCH" : "NO LOC";
                                std::string edInfo;
                                if (ra.matchedEditorActor >= 0 && edActorsPtr) {
                                    auto& ea = (*edActorsPtr)[ra.matchedEditorActor];
                                    matchStr = "MATCHED";
                                    bool meshOk = ea.meshIndex >= 0;
                                    bool texOk = meshOk && m_Viewport.HasMeshTexture(ea.meshIndex);
                                    edInfo = meshOk ? (texOk ? " [mesh+tex]" : " [mesh]") : " [no mesh]";
                                }
                                const char* meshBrief = "";
                                if (!ra.staticMesh.empty()) meshBrief = " SM";
                                else if (!ra.mesh.empty()) meshBrief = " M";
                                else if (!ra.skeletalMesh.empty()) meshBrief = " SK";

                                snprintf(label, sizeof(label), "%-22s %-16s %s%s%s%s",
                                         ra.className.c_str(), ra.name.c_str(),
                                         matchStr, edInfo.c_str(), meshBrief,
                                         ra.bHidden ? " (hid)" : "");

                                if (ImGui::Selectable(label, false)) {
                                    if (ra.hasLocation) {
                                        Vec3 pos = {ra.location[0], ra.location[1], ra.location[2]};
                                        m_Viewport.GetCamera().FocusOn(pos);
                                    }
                                    if (ra.matchedEditorActor >= 0)
                                        m_SelectedActor = ra.matchedEditorActor;
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::BeginTooltip();
                                    ImGui::Text("Class: %s", ra.className.c_str());
                                    ImGui::Text("Path: %s", ra.fullPath.c_str());
                                    if (ra.hasLocation)
                                        ImGui::Text("Pos: (%.1f, %.1f, %.1f)", ra.location[0], ra.location[1], ra.location[2]);
                                    else
                                        ImGui::TextDisabled("No location data");
                                    ImGui::Text("Scale: %.2f | 3D: (%.2f,%.2f,%.2f)", ra.drawScale, ra.drawScale3D[0], ra.drawScale3D[1], ra.drawScale3D[2]);
                                    ImGui::Text("StaticMesh: %s", ra.staticMesh.empty() ? "(none)" : ra.staticMesh.c_str());
                                    ImGui::Text("Mesh: %s", ra.mesh.empty() ? "(none)" : ra.mesh.c_str());
                                    ImGui::Text("SkelMesh: %s", ra.skeletalMesh.empty() ? "(none)" : ra.skeletalMesh.c_str());
                                    if (!ra.skins.empty()) {
                                        ImGui::Text("Skins:");
                                        for (auto& sk : ra.skins) ImGui::Text("  %s", sk.c_str());
                                    }
                                    if (ra.matchedEditorActor >= 0 && edActorsPtr) {
                                        auto& ea = (*edActorsPtr)[ra.matchedEditorActor];
                                        ImGui::Separator();
                                        ImGui::TextColored(ImVec4(0.5f,1,0.5f,1), "Editor Match:");
                                        ImGui::Text("  %s (%s)", ea.objectName.c_str(), ea.className.c_str());
                                        ImGui::Text("  MeshRef: %s", ea.meshRefName.c_str());
                                    }
                                    ImGui::EndTooltip();
                                }
                                ImGui::PopStyleColor();
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    // ─── Meshes Tab ───
                    if (ImGui::BeginTabItem("Meshes")) {
                        if (ImGui::BeginChild("TARMeshes", ImVec2(0, 0), true)) {
                            std::string filterStr(tarFilter);
                            for (auto& c : filterStr) c = (char)tolower((unsigned char)c);
                            for (auto& me : m_RuntimeMeshes) {
                                if (!filterStr.empty()) {
                                    std::string nm = me.name; for (auto& c : nm) c = (char)tolower((unsigned char)c);
                                    std::string pt = me.path; for (auto& c : pt) c = (char)tolower((unsigned char)c);
                                    if (nm.find(filterStr) == std::string::npos &&
                                        pt.find(filterStr) == std::string::npos) continue;
                                }
                                char label[512];
                                snprintf(label, sizeof(label), "%-30s %s", me.name.c_str(), me.path.c_str());
                                if (ImGui::Selectable(label, false)) {}
                                if (ImGui::IsItemHovered()) {
                                    ImGui::BeginTooltip();
                                    ImGui::Text("Path: %s", me.path.c_str());
                                    ImGui::EndTooltip();
                                }
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    // ─── Textures Tab ───
                    if (ImGui::BeginTabItem("Textures")) {
                        if (ImGui::BeginChild("TARTextures", ImVec2(0, 0), true)) {
                            std::string filterStr(tarFilter);
                            for (auto& c : filterStr) c = (char)tolower((unsigned char)c);
                            for (auto& te : m_RuntimeTextures) {
                                if (!filterStr.empty()) {
                                    std::string nm = te.name; for (auto& c : nm) c = (char)tolower((unsigned char)c);
                                    std::string pt = te.path; for (auto& c : pt) c = (char)tolower((unsigned char)c);
                                    if (nm.find(filterStr) == std::string::npos &&
                                        pt.find(filterStr) == std::string::npos) continue;
                                }
                                char label[512];
                                snprintf(label, sizeof(label), "[%s] %-30s %s", te.className.c_str(), te.name.c_str(), te.path.c_str());
                                ImGui::Selectable(label, false);
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    // ─── Materials Tab ───
                    if (ImGui::BeginTabItem("Materials")) {
                        if (ImGui::BeginChild("TARMaterials", ImVec2(0, 0), true)) {
                            std::string filterStr(tarFilter);
                            for (auto& c : filterStr) c = (char)tolower((unsigned char)c);
                            for (auto& mt : m_RuntimeMaterials) {
                                if (!filterStr.empty()) {
                                    std::string nm = mt.name; for (auto& c : nm) c = (char)tolower((unsigned char)c);
                                    std::string pt = mt.path; for (auto& c : pt) c = (char)tolower((unsigned char)c);
                                    if (nm.find(filterStr) == std::string::npos &&
                                        pt.find(filterStr) == std::string::npos) continue;
                                }
                                char label[512];
                                snprintf(label, sizeof(label), "[%s] %-30s %s", mt.className.c_str(), mt.name.c_str(), mt.path.c_str());
                                ImGui::Selectable(label, false);
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            } else {
                ImGui::TextDisabled("No runtime scan loaded. Click Import to load a scan JSON.");
            }
        }
        ImGui::End();
    }

    // Keyboard shortcuts overlay (F3)
    if (m_ShowShortcuts) {
        ImGui::SetNextWindowSize(ImVec2(440, 620), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Keyboard Shortcuts", &m_ShowShortcuts)) {
            ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "CAMERA");
            ImGui::BulletText("RMB + WASD    Fly camera");
            ImGui::BulletText("RMB + Shift   Fly faster");
            ImGui::BulletText("RMB + Scroll  Adjust fly speed");
            ImGui::BulletText("RMB + E/Space Up | Q/Ctrl Down");
            ImGui::BulletText("MMB drag      Pan");
            ImGui::BulletText("Scroll        Zoom (orbit mode)");
            ImGui::BulletText("F             Focus on selected actor");
            ImGui::BulletText("Ctrl+0-9      Save camera bookmark");
            ImGui::BulletText("0-9           Recall camera bookmark");
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "GIZMO / TRANSFORM");
            ImGui::BulletText("W             Translate mode");
            ImGui::BulletText("E             Rotate mode");
            ImGui::BulletText("R             Scale mode");
            ImGui::BulletText("LMB drag      Drag axis on gizmo");
            ImGui::BulletText("Escape        Cancel current drag");
            ImGui::BulletText("G             Cycle snap grid (off/10/50/100/500)");
            ImGui::BulletText("Ctrl+G        Align selection to grid");
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "EDITING");
            ImGui::BulletText("LMB           Select actor (click in viewport)");
            ImGui::BulletText("Ctrl+Click    Add/remove from multi-selection");
            ImGui::BulletText("Ctrl+A        Select all actors");
            ImGui::BulletText("Escape        Deselect (when not dragging)");
            ImGui::BulletText("Ctrl+C        Copy selection");
            ImGui::BulletText("Ctrl+V        Paste at camera");
            ImGui::BulletText("Ctrl+D        Duplicate selected actor");
            ImGui::BulletText("Delete        Delete selected actor");
            ImGui::BulletText("Ctrl+Z        Undo");
            ImGui::BulletText("Ctrl+Y        Redo");
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0.5f,1,1), "VIEW");
            ImGui::BulletText("F1            Toggle BSP wireframe");
            ImGui::BulletText("F2            Cycle view mode");
            ImGui::BulletText("F3            This shortcuts window");
            ImGui::BulletText("F4            Actor spawner palette");
            ImGui::BulletText("P             Toggle Game Preview mode");
            ImGui::BulletText("T             Toggle orthographic/perspective");
            ImGui::BulletText("Z             Toggle draw radius");
            ImGui::BulletText("[  /  ]       Decrease / increase draw radius");
            ImGui::BulletText("V             Toggle zone filter");
            ImGui::BulletText("C             Toggle section clip");
            ImGui::BulletText("PageUp/Down   Move clip plane");
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "FILE");
            ImGui::BulletText("Ctrl+O        Open BSM file");
            ImGui::BulletText("Ctrl+S        Save BSM file");
            ImGui::BulletText("Ctrl+Shift+S  Save layout (JSON)");
            ImGui::BulletText("Ctrl+Shift+L  Load layout (JSON)");
            ImGui::BulletText("Double-click mesh in Content Browser: Spawn");
        }
        ImGui::End();
    }
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
        m_SelectedActors.clear();
        if (m_Document.IsLoaded()) {
            AddRecentFile(filename);
            printf("[App] Loaded: %d actors\n", m_Document.GetActorCount());
            // Focus camera on center of valid actors
            auto& actors = m_Document.GetActors();
            if (!actors.empty()) {
                double cx = 0, cy = 0, cz = 0;
                int validCount = 0;
                for (auto& a : actors) {
                    // Only use actors with parsed Location for centering
                    if (!a.hasLocation) continue;
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

            m_Viewport.GetTextureCache().AddAllMapTextureDirs("Z:\\UEViewer\\export");
            m_Viewport.UploadMeshes(m_Document.GetMeshes(), texDir2);
            if (m_Document.HasBSP())
                m_Viewport.UploadBSP(m_Document.GetBSPMeshes(), texDir2);
            m_ContentBrowser.ScanDirectory(m_ExportDir);
            m_ThumbnailRenderer.SetTextureCache(&m_Viewport.GetTextureCache());
            m_ThumbnailRenderer.SetTextureDir(texDir2);
            m_ThumbnailRenderer.SetMeshes(m_Document.GetMeshes());
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

void App::DuplicateSelectedActor()
{
    if (!m_Document.IsLoaded() || m_SelectedActor < 0) return;
    auto& actors = m_Document.GetActors();
    if (m_SelectedActor >= (int)actors.size()) return;

    EditorActor copy = actors[m_SelectedActor];
    copy.exportIndex = -1; // user-created
    copy.objectName += "_copy";
    // Offset slightly so it's visibly separate
    copy.location.x += 100.0f;
    copy.location.y += 100.0f;

    actors.push_back(copy);
    m_SelectedActor = (int)actors.size() - 1;
    printf("[App] Duplicated actor -> '%s'\n", copy.objectName.c_str());
}

void App::SaveLayoutJSON(const std::string& path)
{
    if (!m_Document.IsLoaded()) return;
    auto& actors = m_Document.GetActors();

    std::ofstream f(path);
    if (!f.is_open()) {
        printf("[App] ERROR: Could not write %s\n", path.c_str());
        return;
    }

    f << "{\n";
    f << "  \"map\": \"" << m_Document.GetMapName() << "\",\n";
    f << "  \"actorCount\": " << actors.size() << ",\n";
    f << "  \"actors\": [\n";

    for (int i = 0; i < (int)actors.size(); i++) {
        auto& a = actors[i];
        if (a.exportIndex >= 0 && !a.hasLocation) continue; // skip non-positioned BSM actors
        f << "    {\n";
        f << "      \"class\": \"" << a.className << "\",\n";
        f << "      \"name\": \"" << a.objectName << "\",\n";
        f << "      \"exportIndex\": " << a.exportIndex << ",\n";
        f << "      \"meshIndex\": " << a.meshIndex << ",\n";
        f << "      \"visible\": " << (a.visible ? "true" : "false") << ",\n";
        f << "      \"location\": [" << a.location.x << ", " << a.location.y << ", " << a.location.z << "],\n";
        f << "      \"rotation\": [" << a.rotation.x << ", " << a.rotation.y << ", " << a.rotation.z << "],\n";
        f << "      \"scale\": [" << a.scale.x << ", " << a.scale.y << ", " << a.scale.z << "]";
        if (a.isLight) {
            f << ",\n      \"light\": {\n";
            f << "        \"brightness\": " << a.lightBrightness << ",\n";
            f << "        \"radius\": " << a.lightRadius << ",\n";
            f << "        \"color\": [" << (int)a.lightColorR << ", " << (int)a.lightColorG << ", " << (int)a.lightColorB << "]\n";
            f << "      }";
        }
        f << "\n    }";
        if (i < (int)actors.size() - 1) f << ",";
        f << "\n";
    }

    f << "  ]\n}\n";
    f.close();
    m_LayoutPath = path;
    printf("[App] Saved layout to %s (%d actors)\n", path.c_str(), (int)actors.size());
}

// Simple JSON value parser (handles numbers, strings, arrays of numbers)
static std::string ParseJSONString(const std::string& line, const std::string& key) {
    size_t p = line.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = line.find(":", p);
    if (p == std::string::npos) return "";
    p++;
    while (p < line.size() && (line[p] == ' ' || line[p] == '"')) p++;
    if (p >= line.size()) return "";
    // Check if it's a quoted string
    size_t qStart = line.find('"', line.find(":", line.find("\"" + key + "\"")));
    if (qStart != std::string::npos) {
        qStart++;
        size_t qEnd = line.find('"', qStart);
        if (qEnd != std::string::npos) return line.substr(qStart, qEnd - qStart);
    }
    return "";
}

static float ParseJSONFloat(const std::string& line, const std::string& key) {
    size_t p = line.find("\"" + key + "\"");
    if (p == std::string::npos) return 0;
    p = line.find(":", p);
    if (p == std::string::npos) return 0;
    return (float)atof(line.c_str() + p + 1);
}

static int ParseJSONInt(const std::string& line, const std::string& key) {
    size_t p = line.find("\"" + key + "\"");
    if (p == std::string::npos) return 0;
    p = line.find(":", p);
    if (p == std::string::npos) return 0;
    return atoi(line.c_str() + p + 1);
}

static bool ParseJSONBool(const std::string& line, const std::string& key) {
    size_t p = line.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    return line.find("true", p) != std::string::npos;
}

static void ParseJSONVec3(const std::string& line, const std::string& key, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    size_t p = line.find("\"" + key + "\"");
    if (p == std::string::npos) return;
    size_t bracket = line.find("[", p);
    if (bracket == std::string::npos) return;
    // read 3 floats separated by commas
    const char* s = line.c_str() + bracket + 1;
    out[0] = (float)atof(s);
    const char* c1 = strchr(s, ',');
    if (c1) { out[1] = (float)atof(c1+1); const char* c2 = strchr(c1+1, ','); if (c2) out[2] = (float)atof(c2+1); }
}

void App::LoadLayoutJSON(const std::string& path)
{
    if (!m_Document.IsLoaded()) return;

    std::ifstream f(path);
    if (!f.is_open()) {
        printf("[App] ERROR: Could not open %s\n", path.c_str());
        return;
    }

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Clear user-created actors, keep BSM-originals
    auto& actors = m_Document.GetActors();
    // We'll update existing BSM actors by export index, and append new ones
    int loaded = 0;

    // Simple line-by-line parsing
    // Find each actor block between { and }
    size_t pos = content.find("\"actors\"");
    if (pos == std::string::npos) { printf("[App] No actors array in layout\n"); return; }

    size_t searchStart = pos;
    while (true) {
        size_t blockStart = content.find("{", searchStart);
        if (blockStart == std::string::npos) break;
        // Make sure we're inside the actors array
        size_t blockEnd = content.find("}", blockStart);
        if (blockEnd == std::string::npos) break;
        // Check for nested { (light block)
        size_t nextOpen = content.find("{", blockStart + 1);
        if (nextOpen != std::string::npos && nextOpen < blockEnd) {
            // Skip past nested block
            size_t nestedEnd = content.find("}", nextOpen);
            if (nestedEnd != std::string::npos) blockEnd = content.find("}", nestedEnd + 1);
            if (blockEnd == std::string::npos) break;
        }

        std::string block = content.substr(blockStart, blockEnd - blockStart + 1);
        searchStart = blockEnd + 1;

        // Parse actor fields
        std::string cls = ParseJSONString(block, "class");
        std::string name = ParseJSONString(block, "name");
        if (cls.empty()) continue;

        int expIdx = ParseJSONInt(block, "exportIndex");
        int meshIdx = ParseJSONInt(block, "meshIndex");
        bool visible = ParseJSONBool(block, "visible");

        float loc[3], rot[3], sc[3];
        ParseJSONVec3(block, "location", loc);
        ParseJSONVec3(block, "rotation", rot);
        ParseJSONVec3(block, "scale", sc);

        // Find existing actor by exportIndex or append new
        int targetIdx = -1;
        if (expIdx >= 0) {
            for (int i = 0; i < (int)actors.size(); i++) {
                if (actors[i].exportIndex == expIdx) { targetIdx = i; break; }
            }
        }

        if (targetIdx >= 0) {
            // Update existing BSM actor
            actors[targetIdx].location = {loc[0], loc[1], loc[2]};
            actors[targetIdx].rotation = {rot[0], rot[1], rot[2]};
            actors[targetIdx].scale = {sc[0], sc[1], sc[2]};
            actors[targetIdx].visible = visible;
        } else {
            // New user-created actor
            EditorActor a;
            a.exportIndex = -1;
            a.className = cls;
            a.objectName = name;
            a.location = {loc[0], loc[1], loc[2]};
            a.rotation = {rot[0], rot[1], rot[2]};
            a.scale = {sc[0], sc[1], sc[2]};
            a.hasLocation = true;
            a.visible = visible;
            a.meshIndex = meshIdx;
            actors.push_back(a);
        }
        loaded++;
    }

    m_LayoutPath = path;
    m_SelectedActor = -1;
    printf("[App] Loaded layout from %s (%d actor entries)\n", path.c_str(), loaded);
}

void App::SaveLayoutDialog()
{
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "JSON Layout (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "json";
    if (GetSaveFileNameA(&ofn)) {
        SaveLayoutJSON(filename);
    }
#endif
}

void App::LoadLayoutDialog()
{
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "JSON Layout (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) {
        LoadLayoutJSON(filename);
    }
#endif
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
        m_Viewport.GetTextureCache().AddAllMapTextureDirs("Z:\\UEViewer\\export");
        m_Viewport.UploadMeshes(m_Document.GetMeshes(), m_ExportDir + "\\Texture");
        if (m_Document.HasBSP())
            m_Viewport.UploadBSP(m_Document.GetBSPMeshes());
        // Re-extract lights
        m_Viewport.m_SceneLights.clear();
        for (auto& a : m_Document.GetActors()) {
            if (!a.isLight || !a.hasLocation) continue;
            SceneLight sl;
            sl.pos[0] = a.location.x; sl.pos[1] = a.location.y; sl.pos[2] = a.location.z;
            sl.color[0] = a.lightColorR / 255.0f;
            sl.color[1] = a.lightColorG / 255.0f;
            sl.color[2] = a.lightColorB / 255.0f;
            sl.radius = a.lightRadius;
            sl.brightness = a.lightBrightness;
            m_Viewport.m_SceneLights.push_back(sl);
        }
    }
}

void App::PushUndo(int actorIdx, const Vec3& oldPos, const Vec3& newPos)
{
    // Truncate any redo history
    if (m_UndoIndex < (int)m_UndoStack.size() - 1) {
        m_UndoStack.resize(m_UndoIndex + 1);
    }
    m_UndoStack.push_back({actorIdx, oldPos, newPos});
    m_UndoIndex = (int)m_UndoStack.size() - 1;
}

void App::Undo()
{
    if (m_UndoIndex < 0 || m_UndoStack.empty()) return;
    auto& action = m_UndoStack[m_UndoIndex];
    if (m_Document.IsLoaded()) {
        auto& actors = m_Document.GetActors();
        if (action.actorIndex < (int)actors.size()) {
            actors[action.actorIndex].location = action.oldPos;
            m_SelectedActor = action.actorIndex;
        }
    }
    m_UndoIndex--;
}

void App::Redo()
{
    if (m_UndoIndex >= (int)m_UndoStack.size() - 1) return;
    m_UndoIndex++;
    auto& action = m_UndoStack[m_UndoIndex];
    if (m_Document.IsLoaded()) {
        auto& actors = m_Document.GetActors();
        if (action.actorIndex < (int)actors.size()) {
            actors[action.actorIndex].location = action.newPos;
            m_SelectedActor = action.actorIndex;
        }
    }
}

void App::SpawnActorByClass(const std::string& className)
{
    if (!m_Document.IsLoaded()) return;
    Vec3 camPos = m_Viewport.GetCamera().GetPosition();
    Vec3 camFwd = m_Viewport.GetCamera().GetForward();
    // Spawn 500 units in front of camera
    EditorActor a;
    a.exportIndex = -1;
    a.className = className;
    a.objectName = className + "_spawned";
    a.location = {camPos.x + camFwd.x * 500.0f, camPos.y + camFwd.y * 500.0f, camPos.z + camFwd.z * 500.0f};
    a.rotation = {0, 0, 0};
    a.scale = {1, 1, 1};
    a.hasLocation = true;
    a.visible = true;
    // Check if it's a light
    if (className.find("Light") != std::string::npos) {
        a.isLight = true;
        a.lightBrightness = 1.0f;
        a.lightRadius = 1024.0f;
        a.lightColorR = 255;
        a.lightColorG = 255;
        a.lightColorB = 255;
    }
    auto& actors = m_Document.GetActors();
    actors.push_back(a);
    m_SelectedActor = (int)actors.size() - 1;
    m_SelectedActors.clear();
    m_SelectedActors.insert(m_SelectedActor);
    printf("[App] Spawned %s at (%.0f, %.0f, %.0f)\n", className.c_str(),
        a.location.x, a.location.y, a.location.z);
}

void App::CopySelection()
{
    if (!m_Document.IsLoaded() || m_SelectedActor < 0) return;
    m_CopyBuffer.clear();
    auto& actors = m_Document.GetActors();
    if (m_SelectedActors.empty()) {
        // Single selection
        if (m_SelectedActor < (int)actors.size())
            m_CopyBuffer.push_back(actors[m_SelectedActor]);
    } else {
        for (int idx : m_SelectedActors) {
            if (idx >= 0 && idx < (int)actors.size())
                m_CopyBuffer.push_back(actors[idx]);
        }
    }
    printf("[App] Copied %d actors\n", (int)m_CopyBuffer.size());
}

void App::PasteSelection()
{
    if (!m_Document.IsLoaded() || m_CopyBuffer.empty()) return;
    auto& actors = m_Document.GetActors();
    Vec3 camPos = m_Viewport.GetCamera().GetPosition();
    Vec3 camFwd = m_Viewport.GetCamera().GetForward();
    // Place pasted actors in front of camera
    Vec3 pastePos = {camPos.x + camFwd.x * 500.0f, camPos.y + camFwd.y * 500.0f, camPos.z + camFwd.z * 500.0f};
    // Calculate centroid of copy buffer
    Vec3 centroid = {0,0,0};
    for (auto& a : m_CopyBuffer) {
        centroid.x += a.location.x;
        centroid.y += a.location.y;
        centroid.z += a.location.z;
    }
    float n = (float)m_CopyBuffer.size();
    centroid.x /= n; centroid.y /= n; centroid.z /= n;

    m_SelectedActors.clear();
    for (auto& src : m_CopyBuffer) {
        EditorActor copy = src;
        copy.exportIndex = -1;
        copy.objectName += "_paste";
        // Offset relative to centroid, place at paste position
        copy.location.x = pastePos.x + (src.location.x - centroid.x);
        copy.location.y = pastePos.y + (src.location.y - centroid.y);
        copy.location.z = pastePos.z + (src.location.z - centroid.z);
        actors.push_back(copy);
        m_SelectedActors.insert((int)actors.size() - 1);
    }
    m_SelectedActor = (int)actors.size() - 1;
    printf("[App] Pasted %d actors\n", (int)m_CopyBuffer.size());
}

void App::SelectAll()
{
    if (!m_Document.IsLoaded()) return;
    auto& actors = m_Document.GetActors();
    m_SelectedActors.clear();
    for (int i = 0; i < (int)actors.size(); i++) {
        if (actors[i].visible && actors[i].hasLocation) {
            m_SelectedActors.insert(i);
        }
    }
    if (!m_SelectedActors.empty())
        m_SelectedActor = *m_SelectedActors.begin();
    printf("[App] Selected all: %d actors\n", (int)m_SelectedActors.size());
}

void App::AlignSelectionToGrid()
{
    if (!m_Document.IsLoaded() || m_SnapGrid <= 0) return;
    auto& actors = m_Document.GetActors();
    auto snapVal = [this](float v) {
        return roundf(v / m_SnapGrid) * m_SnapGrid;
    };
    if (!m_SelectedActors.empty()) {
        for (int idx : m_SelectedActors) {
            if (idx >= 0 && idx < (int)actors.size()) {
                actors[idx].location.x = snapVal(actors[idx].location.x);
                actors[idx].location.y = snapVal(actors[idx].location.y);
                actors[idx].location.z = snapVal(actors[idx].location.z);
            }
        }
    } else if (m_SelectedActor >= 0 && m_SelectedActor < (int)actors.size()) {
        actors[m_SelectedActor].location.x = snapVal(actors[m_SelectedActor].location.x);
        actors[m_SelectedActor].location.y = snapVal(actors[m_SelectedActor].location.y);
        actors[m_SelectedActor].location.z = snapVal(actors[m_SelectedActor].location.z);
    }
    printf("[App] Aligned selection to grid (%.0f)\n", m_SnapGrid);
}

void App::AddRecentFile(const std::string& path)
{
    // Remove if already exists
    for (auto it = m_RecentFiles.begin(); it != m_RecentFiles.end(); ) {
        if (*it == path) it = m_RecentFiles.erase(it);
        else ++it;
    }
    // Add to front
    m_RecentFiles.insert(m_RecentFiles.begin(), path);
    // Keep max 8
    if (m_RecentFiles.size() > 8) m_RecentFiles.resize(8);
}

// ─── Runtime Scan JSON Import ───────────────────────────────────────────────

// Minimal JSON value extraction helpers (no external JSON library needed)
static std::string JsonExtractString(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\": \"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\":\"";
        pos = json.find(search);
    }
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    // Handle escaped quotes
    while (end > 0 && json[end-1] == '\\') end = json.find('"', end+1);
    return json.substr(pos, end - pos);
}

static float JsonExtractFloat(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\": ";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\":";
        pos = json.find(search);
    }
    if (pos == std::string::npos) return 0;
    pos += search.size();
    return std::strtof(json.c_str() + pos, nullptr);
}

static bool JsonExtractBool(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\": ";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\":";
        pos = json.find(search);
    }
    if (pos == std::string::npos) return false;
    pos += search.size();
    return json.substr(pos, 4) == "true";
}

static void JsonExtractFloatArray(const std::string& json, const std::string& key, float* out, int count)
{
    std::string search = "\"" + key + "\": [";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\":[";
        pos = json.find(search);
    }
    if (pos == std::string::npos) return;
    pos += search.size();
    for (int i = 0; i < count; i++) {
        out[i] = std::strtof(json.c_str() + pos, nullptr);
        size_t comma = json.find_first_of(",]", pos);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

bool App::LoadRuntimeScanJSON(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string json((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();

    m_RuntimeActors.clear();
    m_RuntimeScanMap = JsonExtractString(json, "mapName");
    JsonExtractFloatArray(json, "playerLocation", m_RuntimeScanPos, 3);
    m_RuntimeScanRadius = JsonExtractFloat(json, "scanRadius");

    // Split into actor blocks (between { and })
    size_t actorsStart = json.find("\"actors\": [");
    if (actorsStart == std::string::npos) actorsStart = json.find("\"actors\":[");
    if (actorsStart == std::string::npos) return false;

    size_t pos = json.find('{', actorsStart + 10);
    while (pos != std::string::npos) {
        size_t end = json.find("\n    }", pos);
        if (end == std::string::npos) end = json.find("}", pos + 1);
        if (end == std::string::npos) break;
        end += 1; // include the }

        std::string block = json.substr(pos, end - pos + 5);

        RuntimeActor ra;
        ra.name = JsonExtractString(block, "name");
        ra.className = JsonExtractString(block, "class");
        ra.fullPath = JsonExtractString(block, "fullPath");
        ra.distance = JsonExtractFloat(block, "distance");
        JsonExtractFloatArray(block, "location", ra.location, 3);
        JsonExtractFloatArray(block, "rotation", ra.rotation, 3);
        ra.drawScale = JsonExtractFloat(block, "drawScale");
        JsonExtractFloatArray(block, "drawScale3D", ra.drawScale3D, 3);
        ra.bHidden = JsonExtractBool(block, "bHidden");
        ra.drawType = JsonExtractString(block, "drawType");
        ra.staticMesh = JsonExtractString(block, "staticMesh");
        ra.mesh = JsonExtractString(block, "mesh");
        ra.type = JsonExtractString(block, "_type");
        ra.appearsSpawned = JsonExtractBool(block, "appearsSpawned");
        ra.matchedEditorActor = -1;

        m_RuntimeActors.push_back(ra);
        pos = json.find('{', end);
    }

    printf("[App] Loaded runtime scan: %d actors from %s (map: %s)\n",
           (int)m_RuntimeActors.size(), path.c_str(), m_RuntimeScanMap.c_str());
    fflush(stdout);

    // Match against editor
    MatchRuntimeToEditor();
    return true;
}

// ─── Full Scan JSON Import (runtime_full_scan.json) ─────────────────────────

// Extract all simple objects from a JSON array like: [{"name":"X","path":"Y"}, ...]
static void ParseSimpleObjArray(const std::string& json, const std::string& arrayKey,
                                std::vector<App::RuntimeMeshEntry>& out)
{
    out.clear();
    std::string search = "\"" + arrayKey + "\": [";
    size_t start = json.find(search);
    if (start == std::string::npos) { search = "\"" + arrayKey + "\":["; start = json.find(search); }
    if (start == std::string::npos) return;
    start += search.size();

    size_t arrEnd = json.find("\n  ],", start);
    if (arrEnd == std::string::npos) arrEnd = json.find("\n  ]", start);
    if (arrEnd == std::string::npos) return;

    std::string arr = json.substr(start, arrEnd - start);
    size_t pos = 0;
    while (true) {
        size_t objStart = arr.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = arr.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string obj = arr.substr(objStart, objEnd - objStart + 1);

        App::RuntimeMeshEntry entry;
        entry.name = JsonExtractString(obj, "name");
        entry.path = JsonExtractString(obj, "path");
        out.push_back(entry);
        pos = objEnd + 1;
    }
}

static void ParseMaterialArray(const std::string& json, const std::string& arrayKey,
                               std::vector<App::RuntimeMaterialEntry>& out)
{
    out.clear();
    std::string search = "\"" + arrayKey + "\": [";
    size_t start = json.find(search);
    if (start == std::string::npos) { search = "\"" + arrayKey + "\":["; start = json.find(search); }
    if (start == std::string::npos) return;
    start += search.size();

    size_t arrEnd = json.find("\n  ],", start);
    if (arrEnd == std::string::npos) arrEnd = json.find("\n  ]", start);
    if (arrEnd == std::string::npos) return;

    std::string arr = json.substr(start, arrEnd - start);
    size_t pos = 0;
    while (true) {
        size_t objStart = arr.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = arr.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string obj = arr.substr(objStart, objEnd - objStart + 1);

        App::RuntimeMaterialEntry entry;
        entry.name = JsonExtractString(obj, "name");
        entry.className = JsonExtractString(obj, "class");
        entry.path = JsonExtractString(obj, "path");
        out.push_back(entry);
        pos = objEnd + 1;
    }
}

bool App::LoadRuntimeFullScanJSON(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string json((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();

    m_RuntimeActors.clear();
    m_RuntimeMeshes.clear();
    m_RuntimeTextures.clear();
    m_RuntimeMaterials.clear();

    m_RuntimeScanMap = JsonExtractString(json, "mapName");
    JsonExtractFloatArray(json, "playerLocation", m_RuntimeScanPos, 3);
    m_RuntimeTotalGObjects = (int)JsonExtractFloat(json, "totalGObjects");

    // Parse actors array using brace-counting for proper nesting
    size_t actorsStart = json.find("\"actors\": [");
    if (actorsStart == std::string::npos) actorsStart = json.find("\"actors\":[");

    if (actorsStart != std::string::npos) {
        size_t arrStart = json.find('[', actorsStart);
        // Find matching ] using brace counting
        int bracketDepth = 0;
        size_t actorsEnd = arrStart;
        for (size_t i = arrStart; i < json.size(); i++) {
            if (json[i] == '[') bracketDepth++;
            else if (json[i] == ']') { bracketDepth--; if (bracketDepth == 0) { actorsEnd = i; break; } }
        }

        // Find each actor object using brace counting
        size_t pos = arrStart + 1;
        int actorsParsed = 0, actorsWithLoc = 0;
        while (pos < actorsEnd) {
            // Find start of next object
            size_t objStart = json.find('{', pos);
            if (objStart == std::string::npos || objStart >= actorsEnd) break;

            // Find matching } using brace counting
            int depth = 0;
            size_t objEnd = objStart;
            for (size_t i = objStart; i < actorsEnd; i++) {
                if (json[i] == '{') depth++;
                else if (json[i] == '}') { depth--; if (depth == 0) { objEnd = i; break; } }
            }
            if (objEnd <= objStart) break;

            std::string block = json.substr(objStart, objEnd - objStart + 1);

            // Find where "properties" starts to limit key search to top-level fields only
            size_t propsStart = block.find("\"properties\":");
            std::string header = (propsStart != std::string::npos) ? block.substr(0, propsStart) : block;

            RuntimeActor ra;
            ra.name = JsonExtractString(header, "name");
            ra.className = JsonExtractString(header, "class");
            ra.fullPath = JsonExtractString(header, "fullPath");
            ra.distance = JsonExtractFloat(header, "distFromPlayer");

            // Check location - search only in header (before properties)
            if (header.find("\"location\": null") != std::string::npos ||
                header.find("\"location\":null") != std::string::npos) {
                ra.hasLocation = false;
            } else {
                JsonExtractFloatArray(header, "location", ra.location, 3);
                // hasLocation = true if we found a location array (even at 0,0,0 is valid)
                ra.hasLocation = (header.find("\"location\": [") != std::string::npos ||
                                  header.find("\"location\":[") != std::string::npos);
            }

            JsonExtractFloatArray(header, "rotation", ra.rotation, 3);
            ra.drawScale = JsonExtractFloat(header, "drawScale");
            if (ra.drawScale == 0) ra.drawScale = 1.0f;
            JsonExtractFloatArray(header, "drawScale3D", ra.drawScale3D, 3);
            ra.bHidden = JsonExtractBool(header, "bHidden");
            ra.drawType = std::to_string((int)JsonExtractFloat(header, "drawType"));
            ra.staticMesh = JsonExtractString(header, "staticMesh");
            ra.mesh = JsonExtractString(header, "mesh");
            ra.skeletalMesh = JsonExtractString(header, "skeletalMesh");
            ra.matchedEditorActor = -1;

            // Parse skins array from header
            std::string skinsSearch = "\"skins\": [";
            size_t skinsPos = header.find(skinsSearch);
            if (skinsPos != std::string::npos) {
                skinsPos += skinsSearch.size();
                size_t skinsEnd = header.find(']', skinsPos);
                if (skinsEnd != std::string::npos) {
                    std::string skinsStr = header.substr(skinsPos, skinsEnd - skinsPos);
                    size_t sq = 0;
                    while (true) {
                        size_t qStart = skinsStr.find('"', sq);
                        if (qStart == std::string::npos) break;
                        size_t qEnd = skinsStr.find('"', qStart + 1);
                        if (qEnd == std::string::npos) break;
                        ra.skins.push_back(skinsStr.substr(qStart + 1, qEnd - qStart - 1));
                        sq = qEnd + 1;
                    }
                }
            }

            m_RuntimeActors.push_back(ra);
            actorsParsed++;
            if (ra.hasLocation) actorsWithLoc++;
            pos = objEnd + 1;
        }
        printf("[App] Parsed %d actors (%d with location)\n", actorsParsed, actorsWithLoc);
    }

    // Parse static meshes, textures, materials
    ParseSimpleObjArray(json, "staticMeshes", m_RuntimeMeshes);
    ParseMaterialArray(json, "textures", m_RuntimeTextures);
    ParseMaterialArray(json, "materials", m_RuntimeMaterials);

    printf("[App] Full scan loaded: %d actors, %d meshes, %d textures, %d materials (map: %s)\n",
           (int)m_RuntimeActors.size(), (int)m_RuntimeMeshes.size(),
           (int)m_RuntimeTextures.size(), (int)m_RuntimeMaterials.size(),
           m_RuntimeScanMap.c_str());
    fflush(stdout);

    MatchRuntimeToEditor();
    return true;
}

int App::ImportUnmatchedActors()
{
    if (!m_Document.IsLoaded()) return 0;

    auto& edActors = m_Document.GetActors();
    auto& meshes = const_cast<std::vector<ParsedMesh>&>(m_Document.GetMeshes());

    // Build mesh name lookup from existing BSM meshes
    std::unordered_map<std::string, int> meshNameMap;
    for (int i = 0; i < (int)meshes.size(); i++) {
        std::string mn = meshes[i].name;
        for (auto& c : mn) c = (char)tolower((unsigned char)c);
        meshNameMap[mn] = i;
    }

    // === Phase 1: Sync runtime locations into matched BSM actors that lack location ===
    int synced = 0;
    for (auto& ra : m_RuntimeActors) {
        if (!ra.hasLocation) continue;
        if (ra.matchedEditorActor < 0 || ra.matchedEditorActor >= (int)edActors.size()) continue;
        auto& ea = edActors[ra.matchedEditorActor];
        if (!ea.hasLocation) {
            // Copy runtime location into BSM actor
            ea.location = {ra.location[0], ra.location[1], ra.location[2]};
            ea.rotation = {ra.rotation[0], ra.rotation[1], ra.rotation[2]};
            ea.hasLocation = true;
            // Also try to resolve mesh if BSM didn't find one
            if (ea.meshIndex < 0 && !ra.staticMesh.empty()) {
                std::string meshName = ra.staticMesh;
                size_t lastDot = meshName.rfind('.');
                if (lastDot != std::string::npos) meshName = meshName.substr(lastDot + 1);
                ea.meshRefName = meshName;
                std::string mnLower = meshName;
                for (auto& c : mnLower) c = (char)tolower((unsigned char)c);
                auto it = meshNameMap.find(mnLower);
                if (it != meshNameMap.end()) {
                    ea.meshIndex = it->second;
                    bool hasMesh = ea.meshIndex >= 0;
                    ea.renderType = ResolveActorRenderType(ea.className, hasMesh, ea.isLight);
                }
            }
            // Sync scale from runtime
            if (ra.drawScale != 1.0f || ra.drawScale3D[0] != 1.0f ||
                ra.drawScale3D[1] != 1.0f || ra.drawScale3D[2] != 1.0f) {
                ea.scale = {ra.drawScale * ra.drawScale3D[0],
                            ra.drawScale * ra.drawScale3D[1],
                            ra.drawScale * ra.drawScale3D[2]};
            }
            synced++;
        }
    }

    // === Phase 2: Import truly unmatched runtime actors as new editor actors ===
    int imported = 0;
    int skipMatched = 0, skipNoLoc = 0, skipHidden = 0, skipClass = 0;
    for (auto& ra : m_RuntimeActors) {
        if (ra.matchedEditorActor >= 0) { skipMatched++; continue; }
        if (!ra.hasLocation) { skipNoLoc++; continue; }
        if (ra.bHidden) { skipHidden++; continue; }
        std::string cn = ra.className;
        if (cn.find("Controller") != std::string::npos ||
            (cn.find("Info") != std::string::npos && cn.find("Volume") == std::string::npos) ||
            cn.find("Pawn") != std::string::npos ||
            (cn.find("Volume") != std::string::npos && cn.find("Blocking") == std::string::npos) ||
            cn.find("EventResponse") != std::string::npos ||
            cn.find("Specification") != std::string::npos ||
            cn.find("SoundGroup") != std::string::npos ||
            cn.find("ObjectPool") != std::string::npos ||
            cn.find("HavokRigidBody") != std::string::npos) { skipClass++; continue; }

        EditorActor ea;
        ea.exportIndex = -1; // not from BSM
        ea.className = ra.className;
        ea.objectName = ra.name;
        ea.location = {ra.location[0], ra.location[1], ra.location[2]};
        ea.rotation = {ra.rotation[0], ra.rotation[1], ra.rotation[2]};
        ea.hasLocation = true;
        ea.scale = {ra.drawScale * ra.drawScale3D[0],
                    ra.drawScale * ra.drawScale3D[1],
                    ra.drawScale * ra.drawScale3D[2]};
        ea.meshIndex = -1;
        if (!ra.staticMesh.empty()) {
            std::string meshName = ra.staticMesh;
            size_t lastDot = meshName.rfind('.');
            if (lastDot != std::string::npos) meshName = meshName.substr(lastDot + 1);
            ea.meshRefName = meshName;
            std::string mnLower = meshName;
            for (auto& c : mnLower) c = (char)tolower((unsigned char)c);
            auto it = meshNameMap.find(mnLower);
            if (it != meshNameMap.end()) ea.meshIndex = it->second;
        }
        bool hasMesh = ea.meshIndex >= 0;
        ea.isLight = cn.find("Light") != std::string::npos;
        ea.renderType = ResolveActorRenderType(cn, hasMesh, ea.isLight);

        int newIdx = (int)edActors.size();
        edActors.push_back(ea);
        ra.matchedEditorActor = newIdx;
        imported++;
    }

    printf("[App] Sync: %d actors got runtime locations. Import: %d new actors added.\n", synced, imported);
    printf("[App]   Skip: %d matched, %d no-loc, %d hidden, %d non-visual\n",
           skipMatched, skipNoLoc, skipHidden, skipClass);
    fflush(stdout);
    return synced + imported;
}

void App::MatchRuntimeToEditor()
{
    if (!m_Document.IsLoaded()) return;
    auto& edActors = m_Document.GetActors();

    // Build name lookup for editor actors (lowercase objectName → index)
    std::unordered_map<std::string, std::vector<int>> edNameMap;
    std::unordered_map<std::string, std::vector<int>> edMeshMap; // meshRefName → indices
    for (int i = 0; i < (int)edActors.size(); i++) {
        std::string nm = edActors[i].objectName;
        for (auto& c : nm) c = (char)tolower((unsigned char)c);
        edNameMap[nm].push_back(i);
        if (!edActors[i].meshRefName.empty()) {
            std::string mn = edActors[i].meshRefName;
            for (auto& c : mn) c = (char)tolower((unsigned char)c);
            edMeshMap[mn].push_back(i);
        }
    }

    // Track which editor actors have already been claimed (1:1 match)
    std::vector<bool> edClaimed(edActors.size(), false);

    for (auto& ra : m_RuntimeActors) {
        ra.matchedEditorActor = -1;

        // === Pass 1: Location proximity (500 unit threshold) ===
        if (ra.hasLocation) {
            float bestDist = 500.0f;
            int bestIdx = -1;
            for (int i = 0; i < (int)edActors.size(); i++) {
                if (edClaimed[i]) continue;
                auto& ea = edActors[i];
                if (!ea.hasLocation) continue;
                float dx = ea.location.x - ra.location[0];
                float dy = ea.location.y - ra.location[1];
                float dz = ea.location.z - ra.location[2];
                float d = sqrtf(dx*dx + dy*dy + dz*dz);
                if (d < bestDist) {
                    bestDist = d;
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0) {
                ra.matchedEditorActor = bestIdx;
                edClaimed[bestIdx] = true;
                continue;
            }
        }

        // === Pass 2: Name-based matching ===
        // Try matching by objectName (runtime "name" field)
        {
            std::string rn = ra.name;
            for (auto& c : rn) c = (char)tolower((unsigned char)c);
            auto it = edNameMap.find(rn);
            if (it != edNameMap.end()) {
                for (int idx : it->second) {
                    if (!edClaimed[idx]) {
                        ra.matchedEditorActor = idx;
                        edClaimed[idx] = true;
                        break;
                    }
                }
                if (ra.matchedEditorActor >= 0) continue;
            }
        }

        // === Pass 3: Mesh name cross-reference ===
        // If runtime actor references a StaticMesh, find an editor actor using the same mesh
        if (!ra.staticMesh.empty()) {
            // Extract mesh name from path like "StaticMesh Med_Arch.med_pillar_tall"
            std::string meshName = ra.staticMesh;
            size_t lastDot = meshName.rfind('.');
            if (lastDot != std::string::npos) meshName = meshName.substr(lastDot + 1);
            for (auto& c : meshName) c = (char)tolower((unsigned char)c);

            auto it = edMeshMap.find(meshName);
            if (it != edMeshMap.end()) {
                // Among unclaimed editor actors with this mesh, pick closest by location if possible
                int bestIdx = -1;
                float bestDist = 1e9f;
                for (int idx : it->second) {
                    if (edClaimed[idx]) continue;
                    if (ra.hasLocation && edActors[idx].hasLocation) {
                        float dx = edActors[idx].location.x - ra.location[0];
                        float dy = edActors[idx].location.y - ra.location[1];
                        float dz = edActors[idx].location.z - ra.location[2];
                        float d = sqrtf(dx*dx + dy*dy + dz*dz);
                        if (d < bestDist) { bestDist = d; bestIdx = idx; }
                    } else if (bestIdx < 0) {
                        bestIdx = idx;
                    }
                }
                if (bestIdx >= 0) {
                    ra.matchedEditorActor = bestIdx;
                    edClaimed[bestIdx] = true;
                    continue;
                }
            }
        }
    }

    int matched = 0, unmatched = 0, noLoc = 0, unmatchedWithLoc = 0;
    for (auto& ra : m_RuntimeActors) {
        if (ra.matchedEditorActor >= 0) matched++;
        else if (!ra.hasLocation) noLoc++;
        else unmatchedWithLoc++;
    }
    unmatched = unmatchedWithLoc;
    printf("[App] Runtime match: %d matched, %d unmatched (with loc), %d no-location\n",
           matched, unmatched, noLoc);
    fflush(stdout);
}
