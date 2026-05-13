#include "overlay.h"
#include "../core/log.h"

#include "../engine/uobject.h"
#include "../engine/function_caller.h"
#include "../sdk/sdk_generator.h"
#include "../hooks/process_event.h"
#include "../scripting/lua_bridge.h"
#include "../gameplay/teleport_plasmid.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <algorithm>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace bs1sdk {

bool Overlay::s_Initialized = false;
bool Overlay::s_Visible = true;
ID3D11Device* Overlay::s_Device = nullptr;
ID3D11DeviceContext* Overlay::s_Context = nullptr;
ID3D11RenderTargetView* Overlay::s_RenderTarget = nullptr;
IDXGISwapChain* Overlay::s_SwapChain = nullptr;

static HWND g_GameWindow = nullptr;
static WNDPROC g_OriginalWndProc = nullptr;

// WndProc hook to forward input to ImGui
static LRESULT WINAPI WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Toggle overlay with INSERT key
    if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
        Overlay::Toggle();
        return 0;
    }

    // When overlay is visible, let ImGui consume input
    if (Overlay::IsVisible()) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 0;
    }

    return CallWindowProcW(g_OriginalWndProc, hWnd, msg, wParam, lParam);
}

static bool CreateRenderTarget(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11RenderTargetView** rtv)
{
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || !backBuffer) return false;
    
    hr = dev->CreateRenderTargetView(backBuffer, nullptr, rtv);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

void Overlay::InitializeDXGI(IDXGISwapChain* swapChain, ID3D11Device* device, HWND hwnd)
{
    if (s_Initialized) return;

    s_SwapChain = swapChain;
    s_Device = device;
    s_Device->AddRef(); // Keep a reference
    s_Device->GetImmediateContext(&s_Context);
    g_GameWindow = hwnd;

    // Create render target view
    if (!CreateRenderTarget(swapChain, device, &s_RenderTarget)) {
        LOG_ERROR("Failed to create render target view");
        return;
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.Alpha = 0.95f;

    // Initialize backends
    ImGui_ImplWin32_Init(g_GameWindow);
    ImGui_ImplDX11_Init(s_Device, s_Context);

    // Hook the window procedure for input
    g_OriginalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_GameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook))
    );

    s_Initialized = true;
    LOG_INFO("Overlay initialized (D3D11, HWND: 0x{:X})", reinterpret_cast<uintptr_t>(g_GameWindow));
}

void Overlay::Render()
{
    if (!s_Initialized || !s_Visible) return;

    // Start ImGui frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Render our content
    RenderMainMenu();

    // Finish frame
    ImGui::EndFrame();
    ImGui::Render();
    
    // Set render target and draw
    s_Context->OMSetRenderTargets(1, &s_RenderTarget, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Overlay::OnDeviceLost()
{
    if (s_RenderTarget) {
        s_RenderTarget->Release();
        s_RenderTarget = nullptr;
    }
}

void Overlay::OnDeviceReset()
{
    if (s_Initialized && s_SwapChain && s_Device) {
        CreateRenderTarget(s_SwapChain, s_Device, &s_RenderTarget);
    }
}

void Overlay::Shutdown()
{
    if (!s_Initialized) return;

    // Restore original WndProc
    if (g_OriginalWndProc && g_GameWindow) {
        SetWindowLongPtrW(g_GameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OriginalWndProc));
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (s_RenderTarget) { s_RenderTarget->Release(); s_RenderTarget = nullptr; }
    if (s_Context) { s_Context->Release(); s_Context = nullptr; }
    if (s_Device) { s_Device->Release(); s_Device = nullptr; }

    s_Initialized = false;
    LOG_INFO("Overlay shutdown");
}

void Overlay::Toggle()
{
    s_Visible = !s_Visible;
    LOG_INFO("Overlay toggled: {}", s_Visible ? "visible" : "hidden");
}

bool Overlay::IsVisible()
{
    return s_Visible;
}

static void DumpObjectsToFile()
{
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;
    
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    
    std::ofstream file("Z:\\Bioshock1SDK\\object_dump.txt");
    if (!file.is_open()) return;
    
    file << "=== BioShock Remastered Object Dump ===" << std::endl;
    file << "Total Objects: " << objCount << std::endl;
    file << "GObjects: 0x" << std::hex << globals.GObjects << std::endl;
    file << "GNames: 0x" << globals.GNames << std::endl;
    file << std::dec << std::endl;
    
    // Collect classes for summary
    std::map<std::string, int> classCounts;
    
    file << "=== ALL OBJECTS ===" << std::endl;
    file << "Index | Address    | Class                | Name (Full Path)" << std::endl;
    file << "------+------------+----------------------+------------------" << std::endl;
    
    for (int i = 0; i < objCount; i++) {
        uintptr_t objPtr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!objPtr) continue;
        
        UObject* obj = reinterpret_cast<UObject*>(objPtr);
        std::string name = obj->GetName();
        std::string className = obj->GetObjClassName();
        std::string fullPath = obj->GetFullPath();
        
        classCounts[className]++;
        
        char line[512];
        std::snprintf(line, sizeof(line), "%5d | 0x%08X | %-20s | %s",
                      i, static_cast<unsigned>(objPtr), className.c_str(), fullPath.c_str());
        file << line << std::endl;
    }
    
    // Class summary at the end
    file << std::endl << "=== CLASS SUMMARY (sorted by count) ===" << std::endl;
    
    // Sort by count descending
    std::vector<std::pair<std::string, int>> sorted(classCounts.begin(), classCounts.end());
    std::sort(sorted.begin(), sorted.end(), 
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    for (const auto& [cls, count] : sorted) {
        char line[256];
        std::snprintf(line, sizeof(line), "%6d x %s", count, cls.c_str());
        file << line << std::endl;
    }
    
    file.close();
    LOG_INFO("Object dump saved: {} objects, {} unique classes", objCount, classCounts.size());
}

void Overlay::RenderMainMenu()
{
    // Main SDK window
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    
    ImGui::Begin("BS1SDK v0.1.0 - BioShock Remastered", nullptr, ImGuiWindowFlags_MenuBar);

    static bool showObjectInspector = false;
    static bool showPlayerProps = false;
    static bool showConsole = false;
    static bool showModMenu = false;

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("Object Inspector", "F2", &showObjectInspector);
            ImGui::MenuItem("Player Properties", "F4", &showPlayerProps);
            ImGui::MenuItem("Console", "~", &showConsole);
            ImGui::MenuItem("Memory Viewer", nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mods")) {
            ImGui::MenuItem("Gameplay Mods", "F7", &showModMenu);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F2)) showObjectInspector = !showObjectInspector;
    if (ImGui::IsKeyPressed(ImGuiKey_F4)) showPlayerProps = !showPlayerProps;
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) showConsole = !showConsole;
    if (ImGui::IsKeyPressed(ImGuiKey_F7)) showModMenu = !showModMenu;

    ImGui::Text("Status: SDK Active");
    ImGui::Separator();
    
    ImGui::Text("Game Module: 0x%p", GetModuleHandleA(nullptr));
    ImGui::Text("D3D11 Device: 0x%p", s_Device);
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    
    ImGui::Separator();
    const auto& globals = GetEngineGlobals();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "DXGI Hook: Active");
    
    if (globals.GObjects) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "GObjects: 0x%08X (%d objects)",
            globals.GObjects, *reinterpret_cast<int32_t*>(globals.GObjects + 4));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "GObjects: Not Found");
    }
    
    if (globals.GNames) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "GNames: 0x%08X (%d names)",
            globals.GNames, *reinterpret_cast<int32_t*>(globals.GNames + 4));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "GNames: Not Found");
    }
    
    ImGui::Separator();
    
    static int dumpState = 0; // 0=ready, 1=done
    if (ImGui::IsKeyPressed(ImGuiKey_F3) && globals.IsValid()) {
        DumpObjectsToFile();
        dumpState = 1;
    }
    if (dumpState == 0) {
        ImGui::Text("Press F3 to dump all objects to file");
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Dump saved! Press F3 again to re-dump.");
    }
    
    // SDK Generation
    static int sdkState = 0; // 0=ready, 1=generating, 2=done
    static int sdkClassCount = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_F5) && globals.IsValid() && sdkState != 1) {
        sdkState = 1;
        SDKGenerator gen;
        sdkClassCount = gen.Generate("Z:\\Bioshock1SDK\\sdk_gen");
        sdkState = 2;
    }
    if (sdkState == 0) {
        ImGui::Text("Press F5 to generate SDK headers");
    } else if (sdkState == 1) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Generating SDK...");
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "SDK generated! %d classes -> sdk_gen/", sdkClassCount);
    }
    
    // ProcessEvent Hook
    ImGui::Separator();
    if (!IsProcessEventHooked()) {
        if (ImGui::Button("Hook ProcessEvent (F6)") || ImGui::IsKeyPressed(ImGuiKey_F6)) {
            InitProcessEventHook();
        }
    } else {
        const auto& peStats = GetProcessEventStats();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "ProcessEvent: HOOKED");
        ImGui::Text("Calls: %llu  Blocked: %llu", peStats.TotalCalls, peStats.BlockedCalls);
        if (!peStats.LastFunctionName.empty()) {
            ImGui::Text("Last: %s.%s", peStats.LastObjectName.c_str(), peStats.LastFunctionName.c_str());
        }
        if (ImGui::Button("Unhook ProcessEvent")) {
            ShutdownProcessEventHook();
        }
    }
    
    ImGui::Separator();
    ImGui::TextWrapped("Press INSERT to toggle this overlay.");
    ImGui::TextWrapped("Press F12 to unload SDK.");

    ImGui::End();

    // Object Inspector window
    if (showObjectInspector) RenderObjectInspector();
    
    // Player Properties window
    if (showPlayerProps) RenderPlayerProperties();
    
    // Console window
    if (showConsole) RenderConsole();

    // Mod Menu window
    if (showModMenu) RenderModMenu();

    // ImGui demo window for testing
    static bool showDemo = false;
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) showDemo = !showDemo;
    if (showDemo) ImGui::ShowDemoWindow(&showDemo);
}

// Helper: render live property values for any UObject instance
static void RenderPropertyValue(UObject* obj, const PropertyInfo& p)
{
    const uint8_t* base = reinterpret_cast<const uint8_t*>(obj);
    
    if (p.TypeName == "IntProperty") {
        int32_t val = *reinterpret_cast<const int32_t*>(base + p.Offset);
        int32_t edit = val;
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt(("##" + p.Name).c_str(), &edit, 1, 10)) {
            obj->SetField<int32_t>(p.Offset, edit);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", val);
    } else if (p.TypeName == "FloatProperty") {
        float val = *reinterpret_cast<const float*>(base + p.Offset);
        float edit = val;
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputFloat(("##" + p.Name).c_str(), &edit, 0.1f, 1.0f, "%.2f")) {
            obj->SetField<float>(p.Offset, edit);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%.2f)", val);
    } else if (p.TypeName == "BoolProperty") {
        uint32_t val = *reinterpret_cast<const uint32_t*>(base + p.Offset);
        ImGui::Text("%s (0x%X)", val ? "true" : "false", val);
    } else if (p.TypeName == "ByteProperty") {
        uint8_t val = *(base + p.Offset);
        ImGui::Text("%d (0x%02X)", val, val);
    } else if (p.TypeName == "NameProperty") {
        FName val = *reinterpret_cast<const FName*>(base + p.Offset);
        ImGui::Text("%s", val.ToString().c_str());
    } else if (p.TypeName == "StrProperty") {
        // FString = TArray<wchar_t>: Data, Count, Max
        struct FStr { wchar_t* Data; int32_t Count; int32_t Max; };
        FStr val = *reinterpret_cast<const FStr*>(base + p.Offset);
        if (val.Data && val.Count > 0 && val.Count < 4096) {
            std::string s;
            for (int c = 0; c < val.Count && val.Data[c]; c++)
                s += (char)(val.Data[c] & 0x7F);
            ImGui::Text("\"%s\"", s.c_str());
        } else {
            ImGui::TextDisabled("<empty>");
        }
    } else if (p.TypeName == "ObjectProperty" || p.TypeName == "ClassProperty" ||
               p.TypeName == "ComponentProperty") {
        uintptr_t val = *reinterpret_cast<const uintptr_t*>(base + p.Offset);
        if (val && val > 0x10000) {
            UObject* ref = reinterpret_cast<UObject*>(val);
            ImGui::Text("-> %s (%s)", ref->GetName().c_str(), ref->GetObjClassName().c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("0x%08X", (uint32_t)val);
        } else {
            ImGui::TextDisabled("NULL");
        }
    } else if (p.TypeName == "ArrayProperty") {
        struct Arr { void* Data; int32_t Count; int32_t Max; };
        Arr val = *reinterpret_cast<const Arr*>(base + p.Offset);
        ImGui::Text("TArray [%d/%d]", val.Count, val.Max);
    } else if (p.TypeName == "StructProperty") {
        // Show raw hex for small structs
        if (p.ElementSize <= 16) {
            std::string hex;
            for (int b = 0; b < p.ElementSize && b < 16; b++) {
                char tmp[4];
                std::snprintf(tmp, sizeof(tmp), "%02X ", base[p.Offset + b]);
                hex += tmp;
            }
            ImGui::TextDisabled("%s", hex.c_str());
        } else {
            ImGui::TextDisabled("struct (%dB)", p.ElementSize);
        }
    } else {
        ImGui::TextDisabled("(%s, %dB)", p.TypeName.c_str(), p.ElementSize);
    }
}

void Overlay::RenderObjectInspector()
{
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;

    ImGui::SetNextWindowPos(ImVec2(430, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(650, 700), ImGuiCond_FirstUseEver);
    
    static bool open = true;
    ImGui::Begin("Object Inspector (F2)", &open);
    
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
    
    // Filter bar
    static char filterBuf[128] = {};
    static char classFilter[64] = {};
    ImGui::Text("Objects: %d", objCount);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("Name##filt", filterBuf, sizeof(filterBuf));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("Class##cfilt", classFilter, sizeof(classFilter));
    
    static int maxDisplay = 500;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("Max##max", &maxDisplay, 0, 0);
    if (maxDisplay < 10) maxDisplay = 10;
    if (maxDisplay > 5000) maxDisplay = 5000;
    
    ImGui::Separator();
    
    // Require at least a filter to avoid rendering 107k items
    if (filterBuf[0] == '\0' && classFilter[0] == '\0') {
        ImGui::TextWrapped("Type a name or class filter to browse objects. "
                          "Try: ShockPlayer, Pistol, Wrench, BigDaddy, Splicer...");
        ImGui::End();
        return;
    }
    
    // Property cache: map object address -> cached properties
    static std::map<uintptr_t, std::vector<PropertyInfo>> propCache;
    
    if (ImGui::BeginChild("ObjList", ImVec2(0, 0), true)) {
        int displayed = 0;
        
        for (int i = 0; i < objCount && displayed < maxDisplay; i++) {
            uintptr_t objPtr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
            if (!objPtr) continue;
            
            UObject* obj = reinterpret_cast<UObject*>(objPtr);
            std::string name = obj->GetName();
            std::string className = obj->GetObjClassName();
            
            // Apply name filter
            if (filterBuf[0]) {
                bool match = false;
                for (size_t c = 0; c + strlen(filterBuf) <= name.size(); c++) {
                    if (_strnicmp(name.c_str() + c, filterBuf, strlen(filterBuf)) == 0) {
                        match = true; break;
                    }
                }
                if (!match) continue;
            }
            
            // Apply class filter
            if (classFilter[0]) {
                bool match = false;
                for (size_t c = 0; c + strlen(classFilter) <= className.size(); c++) {
                    if (_strnicmp(className.c_str() + c, classFilter, strlen(classFilter)) == 0) {
                        match = true; break;
                    }
                }
                if (!match) continue;
            }
            
            displayed++;
            
            ImGui::PushID((int)objPtr);
            if (ImGui::TreeNode("", "%s  (%s)  [%d]", name.c_str(), className.c_str(), i)) {
                // Object header
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                    "0x%08X | %s", (uint32_t)objPtr, obj->GetFullPath().c_str());
                
                UObject* outer = obj->GetOuter();
                if (outer) {
                    ImGui::Text("Outer: %s (0x%08X)", outer->GetName().c_str(),
                                (uint32_t)(uintptr_t)outer);
                }
                ImGui::Text("Flags: 0x%016llX  Index: %d", obj->GetObjectFlags(), obj->GetInternalIndex());
                
                ImGui::Separator();
                
                // Live Properties section
                if (ImGui::TreeNode("Properties")) {
                    // Get or cache properties for this object's class
                    auto it = propCache.find(objPtr);
                    if (it == propCache.end()) {
                        UStruct* cls = reinterpret_cast<UStruct*>(obj->GetClass());
                        if (cls) {
                            propCache[objPtr] = WalkProperties(cls);
                        }
                        it = propCache.find(objPtr);
                    }
                    
                    if (it != propCache.end() && !it->second.empty()) {
                        // Property filter
                        static char propFilter[64] = {};
                        ImGui::SetNextItemWidth(160);
                        ImGui::InputText("Filter##propf", propFilter, sizeof(propFilter));
                        ImGui::SameLine();
                        ImGui::Text("(%d props)", (int)it->second.size());
                        
                        for (auto& p : it->second) {
                            if (propFilter[0]) {
                                bool match = false;
                                for (size_t c = 0; c + strlen(propFilter) <= p.Name.size(); c++) {
                                    if (_strnicmp(p.Name.c_str() + c, propFilter, strlen(propFilter)) == 0) {
                                        match = true; break;
                                    }
                                }
                                if (!match) continue;
                            }
                            
                            // Property row: offset | name | value
                            ImGui::TextDisabled("+%04X", p.Offset);
                            ImGui::SameLine(60);
                            ImGui::Text("%-24s", p.Name.c_str());
                            ImGui::SameLine(260);
                            RenderPropertyValue(obj, p);
                        }
                    } else {
                        ImGui::TextDisabled("No properties found");
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        
        if (displayed >= maxDisplay) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                "Showing %d / more results. Narrow your filter or increase Max.", displayed);
        } else if (displayed == 0) {
            ImGui::TextDisabled("No objects match filter.");
        }
    }
    ImGui::EndChild();
    
    // Clear prop cache periodically (every 10 seconds)
    static float cacheTimer = 0.0f;
    cacheTimer += ImGui::GetIO().DeltaTime;
    if (cacheTimer > 10.0f) {
        propCache.clear();
        cacheTimer = 0.0f;
    }
    
    ImGui::End();
}

void Overlay::RenderPlayerProperties()
{
    const auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return;
    
    ImGui::SetNextWindowPos(ImVec2(440, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 600), ImGuiCond_FirstUseEver);
    
    static bool open = true;
    ImGui::Begin("Player Properties (F4)", &open);
    
    // Find the live ShockPlayer instance
    static UObject* cachedPlayer = nullptr;
    static UStruct* cachedPlayerClass = nullptr;
    static std::vector<PropertyInfo> cachedProps;
    static float refreshTimer = 0.0f;
    
    // Re-find player every 2 seconds (handles level transitions)
    refreshTimer += ImGui::GetIO().DeltaTime;
    if (!cachedPlayer || refreshTimer > 2.0f) {
        refreshTimer = 0.0f;
        cachedPlayer = FindObjectByClassName("ShockPlayer");
        if (cachedPlayer && !cachedPlayerClass) {
            cachedPlayerClass = FindClass("ShockPlayer");
            if (cachedPlayerClass)
                cachedProps = WalkProperties(cachedPlayerClass);
        }
    }
    
    if (!cachedPlayer) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No ShockPlayer instance found.");
        ImGui::Text("Load into a level first.");
        ImGui::End();
        return;
    }
    
    ImGui::Text("ShockPlayer: 0x%08X  (%s)", (uint32_t)(uintptr_t)cachedPlayer,
                cachedPlayer->GetFullPath().c_str());
    ImGui::Text("Properties: %d total (class + inherited)", (int)cachedProps.size());
    ImGui::Separator();
    
    // Key properties section - hand-picked from ShockPlayer.uc
    if (ImGui::CollapsingHeader("Key Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto ReadInt = [&](const char* propName) -> int32_t {
            for (auto& p : cachedProps) {
                if (p.Name == propName && p.TypeName == "IntProperty")
                    return cachedPlayer->GetField<int32_t>(p.Offset);
            }
            return -1;
        };
        auto ReadFloat = [&](const char* propName) -> float {
            for (auto& p : cachedProps) {
                if (p.Name == propName && p.TypeName == "FloatProperty")
                    return cachedPlayer->GetField<float>(p.Offset);
            }
            return -1.0f;
        };
        auto WriteInt = [&](const char* propName, int32_t val) {
            for (auto& p : cachedProps) {
                if (p.Name == propName && p.TypeName == "IntProperty") {
                    cachedPlayer->SetField<int32_t>(p.Offset, val);
                    return;
                }
            }
        };
        auto WriteFloat = [&](const char* propName, float val) {
            for (auto& p : cachedProps) {
                if (p.Name == propName && p.TypeName == "FloatProperty") {
                    cachedPlayer->SetField<float>(p.Offset, val);
                    return;
                }
            }
        };
        
        // ADAM
        int adam = ReadInt("ADAM");
        if (adam >= 0) {
            ImGui::Text("ADAM:"); ImGui::SameLine(120);
            int editAdam = adam;
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("##adam", &editAdam, 10, 100)) WriteInt("ADAM", editAdam);
        }
        
        // Credits
        int credits = ReadInt("Credits");
        if (credits >= 0) {
            ImGui::Text("Credits:"); ImGui::SameLine(120);
            int editCredits = credits;
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("##credits", &editCredits, 10, 100)) WriteInt("Credits", editCredits);
        }
        
        // BioAmmo (EVE)
        float bioAmmo = ReadFloat("BioAmmo");
        if (bioAmmo >= 0.0f) {
            ImGui::Text("EVE:"); ImGui::SameLine(120);
            float editBio = bioAmmo;
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("##bioammo", &editBio, 1.0f, 10.0f, "%.1f")) WriteFloat("BioAmmo", editBio);
        }
        
        // MaxBioAmmo
        float maxBio = ReadFloat("MaxBioAmmo");
        if (maxBio >= 0.0f) {
            ImGui::Text("Max EVE:"); ImGui::SameLine(120);
            float editMaxBio = maxBio;
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("##maxbioammo", &editMaxBio, 1.0f, 10.0f, "%.1f")) WriteFloat("MaxBioAmmo", editMaxBio);
        }
        
        // Health (from Pawn)
        float health = ReadFloat("Health");
        if (health >= 0.0f) {
            ImGui::Text("Health:"); ImGui::SameLine(120);
            float editHealth = health;
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("##health", &editHealth, 10.0f, 50.0f, "%.1f")) WriteFloat("Health", editHealth);
        }
        
        // HealthMax
        float healthMax = ReadFloat("HealthMax");
        if (healthMax >= 0.0f) {
            ImGui::Text("Max Health:"); ImGui::SameLine(120);
            float editHMax = healthMax;
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("##healthmax", &editHMax, 10.0f, 50.0f, "%.1f")) WriteFloat("HealthMax", editHMax);
        }
    }
    
    ImGui::Separator();
    
    // Full property browser
    if (ImGui::CollapsingHeader("All Properties")) {
        static char propFilter[64] = {};
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Filter##props", propFilter, sizeof(propFilter));
        
        if (ImGui::BeginChild("PropList", ImVec2(0, 0), true)) {
            for (auto& p : cachedProps) {
                // Filter
                if (propFilter[0]) {
                    bool match = false;
                    for (size_t c = 0; c + strlen(propFilter) <= p.Name.size(); c++) {
                        if (_strnicmp(p.Name.c_str() + c, propFilter, strlen(propFilter)) == 0) {
                            match = true; break;
                        }
                    }
                    if (!match) continue;
                }
                
                const uint8_t* base = reinterpret_cast<const uint8_t*>(cachedPlayer);
                
                if (p.TypeName == "IntProperty") {
                    int32_t val = *reinterpret_cast<const int32_t*>(base + p.Offset);
                    ImGui::Text("[+%04X] %-30s = %d", p.Offset, p.Name.c_str(), val);
                } else if (p.TypeName == "FloatProperty") {
                    float val = *reinterpret_cast<const float*>(base + p.Offset);
                    ImGui::Text("[+%04X] %-30s = %.3f", p.Offset, p.Name.c_str(), val);
                } else if (p.TypeName == "BoolProperty") {
                    // BoolProperty uses bitmask - read the DWORD and check
                    uint32_t val = *reinterpret_cast<const uint32_t*>(base + p.Offset);
                    ImGui::Text("[+%04X] %-30s = %s (0x%X)", p.Offset, p.Name.c_str(),
                               val ? "true" : "false", val);
                } else if (p.TypeName == "NameProperty") {
                    FName val = *reinterpret_cast<const FName*>(base + p.Offset);
                    ImGui::Text("[+%04X] %-30s = %s", p.Offset, p.Name.c_str(), val.ToString().c_str());
                } else if (p.TypeName == "ObjectProperty" || p.TypeName == "ClassProperty") {
                    uintptr_t val = *reinterpret_cast<const uintptr_t*>(base + p.Offset);
                    if (val) {
                        UObject* ref = reinterpret_cast<UObject*>(val);
                        ImGui::Text("[+%04X] %-30s = 0x%08X (%s)", p.Offset, p.Name.c_str(),
                                    (uint32_t)val, ref->GetName().c_str());
                    } else {
                        ImGui::Text("[+%04X] %-30s = NULL", p.Offset, p.Name.c_str());
                    }
                } else if (p.TypeName == "StrProperty") {
                    ImGui::Text("[+%04X] %-30s (string)", p.Offset, p.Name.c_str());
                } else {
                    ImGui::TextDisabled("[+%04X] %-30s (%s, %dB)", p.Offset, p.Name.c_str(),
                                       p.TypeName.c_str(), p.ElementSize);
                }
            }
        }
        ImGui::EndChild();
    }
    
    ImGui::End();
}

void Overlay::RenderConsole()
{
    ImGui::SetNextWindowPos(ImVec2(20, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_FirstUseEver);
    
    static bool open = true;
    ImGui::Begin("Console (~)", &open);
    
    // Log history
    static std::vector<std::pair<ImVec4, std::string>> logHistory;
    auto Log = [&](const ImVec4& col, const std::string& msg) {
        logHistory.push_back({col, msg});
        if (logHistory.size() > 200) logHistory.erase(logHistory.begin());
    };
    auto LogInfo = [&](const std::string& msg) { Log(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), msg); };
    auto LogGreen = [&](const std::string& msg) { Log(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), msg); };
    auto LogYellow = [&](const std::string& msg) { Log(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), msg); };
    auto LogRed = [&](const std::string& msg) { Log(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), msg); };
    
    // Scroll area for log output
    if (ImGui::BeginChild("ConsoleLog", ImVec2(0, -30), true)) {
        for (auto& [col, msg] : logHistory) {
            ImGui::TextColored(col, "%s", msg.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    
    // Command input
    static char cmdBuf[256] = {};
    static bool focusInput = false;
    
    ImGui::SetNextItemWidth(-1);
    bool entered = ImGui::InputText("##cmd", cmdBuf, sizeof(cmdBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
    if (focusInput) {
        ImGui::SetKeyboardFocusHere(-1);
        focusInput = false;
    }
    
    if (entered && cmdBuf[0]) {
        std::string cmd = cmdBuf;
        LogInfo("> " + cmd);
        cmdBuf[0] = '\0';
        focusInput = true;
        
        // Parse command
        // Tokenize
        std::vector<std::string> tokens;
        std::istringstream iss(cmd);
        std::string tok;
        while (iss >> tok) tokens.push_back(tok);
        
        if (tokens.empty()) {
            // nothing
        }
        // ─── help ───
        else if (tokens[0] == "help") {
            LogYellow("=== BS1SDK Console Commands ===");
            LogInfo("  set <prop> <value>      - Set player property (int/float)");
            LogInfo("  get <prop>              - Read player property");
            LogInfo("  setobj <class> <prop> <value> - Set on first instance of class");
            LogInfo("  god                     - Toggle god mode (freeze health)");
            LogInfo("  give adam <N>           - Set ADAM");
            LogInfo("  give credits <N>        - Set Credits");
            LogInfo("  give eve <N>            - Set EVE (BioAmmo)");
            LogInfo("  give health <N>         - Set Health");
            LogInfo("  list <filter>           - Search objects (name or class)");
            LogInfo("  props [filter]          - List player properties");
            LogInfo("  classes [filter]        - List class names");
            LogYellow("=== ProcessEvent ===");
            LogInfo("  hookpe                  - Toggle ProcessEvent hook");
            LogInfo("  logpe [filter]          - Log unique function calls (toggle)");
            LogInfo("  pestats                 - Show ProcessEvent stats");
            LogInfo("  dumpevents              - Dump all events to file (toggle)");
            LogYellow("=== Functions ===");
            LogInfo("  call <class> <func> [args] - Call a UFunction");
            LogInfo("  funcs <class> [filter]     - List functions on a class");
            LogYellow("=== Teleport Plasmid ===");
            LogInfo("  teleport [dist]         - Blink forward (default 800)");
            LogInfo("  initplasmid             - Hijack Security Bullseye → teleport");
            LogInfo("  tpdist <dist>           - Set teleport distance");
            LogYellow("=== Lua Scripting ===");
            LogInfo("  lua <code>              - Execute Lua code inline");
            LogInfo("  luafile <path>          - Execute a Lua script file");
            LogInfo("  reload                  - Hot-reload autorun.lua");
        }
        // ─── set <prop> <value> ───
        else if (tokens[0] == "set" && tokens.size() >= 3) {
            std::string propName = tokens[1];
            std::string valStr = tokens[2];
            
            UObject* player = FindObjectByClassName("ShockPlayer");
            if (!player) { LogRed("No ShockPlayer found"); }
            else {
                UStruct* cls = FindClass("ShockPlayer");
                if (!cls) { LogRed("ShockPlayer class not found"); }
                else {
                    auto props = WalkProperties(cls);
                    PropertyInfo* pi = nullptr;
                    for (auto& p : props) {
                        if (p.Name == propName) { pi = &p; break; }
                    }
                    if (!pi) { LogRed("Property '" + propName + "' not found"); }
                    else if (pi->TypeName == "IntProperty") {
                        int32_t val = std::atoi(valStr.c_str());
                        player->SetField<int32_t>(pi->Offset, val);
                        char buf[128]; std::snprintf(buf, sizeof(buf), "%s = %d (at +0x%04X)", propName.c_str(), val, pi->Offset);
                        LogGreen(buf);
                    } else if (pi->TypeName == "FloatProperty") {
                        float val = std::strtof(valStr.c_str(), nullptr);
                        player->SetField<float>(pi->Offset, val);
                        char buf[128]; std::snprintf(buf, sizeof(buf), "%s = %.2f (at +0x%04X)", propName.c_str(), val, pi->Offset);
                        LogGreen(buf);
                    } else if (pi->TypeName == "BoolProperty") {
                        uint32_t val = (valStr == "true" || valStr == "1") ? 1 : 0;
                        player->SetField<uint32_t>(pi->Offset, val);
                        char buf[128]; std::snprintf(buf, sizeof(buf), "%s = %s (at +0x%04X)", propName.c_str(), val ? "true" : "false", pi->Offset);
                        LogGreen(buf);
                    } else {
                        LogRed("Can't set " + pi->TypeName + " from console (only int/float/bool)");
                    }
                }
            }
        }
        // ─── get <prop> ───
        else if (tokens[0] == "get" && tokens.size() >= 2) {
            std::string propName = tokens[1];
            UObject* player = FindObjectByClassName("ShockPlayer");
            if (!player) { LogRed("No ShockPlayer found"); }
            else {
                UStruct* cls = FindClass("ShockPlayer");
                auto props = WalkProperties(cls);
                PropertyInfo* pi = nullptr;
                for (auto& p : props) {
                    if (p.Name == propName) { pi = &p; break; }
                }
                if (!pi) { LogRed("Property '" + propName + "' not found"); }
                else {
                    const uint8_t* base = reinterpret_cast<const uint8_t*>(player);
                    char buf[256];
                    if (pi->TypeName == "IntProperty") {
                        int32_t val = *reinterpret_cast<const int32_t*>(base + pi->Offset);
                        std::snprintf(buf, sizeof(buf), "%s = %d (+0x%04X)", propName.c_str(), val, pi->Offset);
                    } else if (pi->TypeName == "FloatProperty") {
                        float val = *reinterpret_cast<const float*>(base + pi->Offset);
                        std::snprintf(buf, sizeof(buf), "%s = %.2f (+0x%04X)", propName.c_str(), val, pi->Offset);
                    } else if (pi->TypeName == "BoolProperty") {
                        uint32_t val = *reinterpret_cast<const uint32_t*>(base + pi->Offset);
                        std::snprintf(buf, sizeof(buf), "%s = %s (0x%X) (+0x%04X)", propName.c_str(), val?"true":"false", val, pi->Offset);
                    } else if (pi->TypeName == "ObjectProperty" || pi->TypeName == "ClassProperty") {
                        uintptr_t val = *reinterpret_cast<const uintptr_t*>(base + pi->Offset);
                        if (val > 0x10000) {
                            UObject* ref = reinterpret_cast<UObject*>(val);
                            std::snprintf(buf, sizeof(buf), "%s = %s (%s) 0x%08X", propName.c_str(),
                                         ref->GetName().c_str(), ref->GetObjClassName().c_str(), (uint32_t)val);
                        } else {
                            std::snprintf(buf, sizeof(buf), "%s = NULL", propName.c_str());
                        }
                    } else {
                        std::snprintf(buf, sizeof(buf), "%s = (%s, %dB at +0x%04X)", propName.c_str(),
                                     pi->TypeName.c_str(), pi->ElementSize, pi->Offset);
                    }
                    LogGreen(buf);
                }
            }
        }
        // ─── give shortcuts ───
        else if (tokens[0] == "give" && tokens.size() >= 3) {
            UObject* player = FindObjectByClassName("ShockPlayer");
            if (!player) { LogRed("No ShockPlayer found"); }
            else {
                UStruct* cls = FindClass("ShockPlayer");
                auto props = WalkProperties(cls);
                std::string what = tokens[1];
                std::string valStr = tokens[2];
                
                std::string propName;
                bool isFloat = false;
                if (what == "adam") { propName = "ADAM"; }
                else if (what == "credits" || what == "money") { propName = "Credits"; }
                else if (what == "eve" || what == "bioammo") { propName = "BioAmmo"; isFloat = true; }
                else if (what == "health" || what == "hp") { propName = "Health"; isFloat = true; }
                else if (what == "maxhealth" || what == "maxhp") { propName = "HealthMax"; isFloat = true; }
                else if (what == "maxeve") { propName = "MaxBioAmmo"; isFloat = true; }
                else { LogRed("Unknown: give " + what); propName = ""; }
                
                if (!propName.empty()) {
                    for (auto& p : props) {
                        if (p.Name == propName) {
                            if (isFloat) {
                                float val = std::strtof(valStr.c_str(), nullptr);
                                player->SetField<float>(p.Offset, val);
                                char buf[128]; std::snprintf(buf, sizeof(buf), "%s = %.1f", propName.c_str(), val);
                                LogGreen(buf);
                            } else {
                                int32_t val = std::atoi(valStr.c_str());
                                player->SetField<int32_t>(p.Offset, val);
                                char buf[128]; std::snprintf(buf, sizeof(buf), "%s = %d", propName.c_str(), val);
                                LogGreen(buf);
                            }
                            break;
                        }
                    }
                }
            }
        }
        // ─── god mode ───
        else if (tokens[0] == "god") {
            static bool godMode = false;
            godMode = !godMode;
            if (godMode) LogGreen("God mode ON - health frozen each frame");
            else LogYellow("God mode OFF");
            // Store state for frame update
            static float* godHealthPtr = nullptr;
            static float godHealthVal = 0;
            if (godMode) {
                UObject* player = FindObjectByClassName("ShockPlayer");
                UStruct* cls = FindClass("ShockPlayer");
                if (player && cls) {
                    auto props = WalkProperties(cls);
                    for (auto& p : props) {
                        if (p.Name == "Health" && p.TypeName == "FloatProperty") {
                            godHealthPtr = reinterpret_cast<float*>(
                                reinterpret_cast<uint8_t*>(player) + p.Offset);
                            godHealthVal = *godHealthPtr;
                            break;
                        }
                    }
                    for (auto& p : props) {
                        if (p.Name == "HealthMax" && p.TypeName == "FloatProperty") {
                            float maxH = player->GetField<float>(p.Offset);
                            player->SetField<float>(p.Offset, 99999.0f);
                            char buf[128]; std::snprintf(buf, sizeof(buf), "HealthMax set to 99999 (was %.0f)", maxH);
                            LogInfo(buf);
                            break;
                        }
                    }
                }
            }
        }
        // ─── list <filter> ─── (case-insensitive, matches class or object name)
        else if (tokens[0] == "list" && tokens.size() >= 2) {
            std::string filter = tokens[1];
            auto& globals = GetEngineGlobals();
            uintptr_t od = *reinterpret_cast<uintptr_t*>(globals.GObjects);
            int32_t oc = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
            int shown = 0;
            for (int i = 0; i < oc && shown < 30; i++) {
                uintptr_t ptr = *reinterpret_cast<uintptr_t*>(od + i * 4);
                if (!ptr) continue;
                UObject* obj = reinterpret_cast<UObject*>(ptr);
                std::string cn = obj->GetObjClassName();
                std::string on = obj->GetName();
                // Case-insensitive substring check on class name or object name
                bool match = false;
                for (size_t c = 0; c + filter.size() <= cn.size(); c++) {
                    if (_strnicmp(cn.c_str() + c, filter.c_str(), filter.size()) == 0) {
                        match = true; break;
                    }
                }
                if (!match) {
                    for (size_t c = 0; c + filter.size() <= on.size(); c++) {
                        if (_strnicmp(on.c_str() + c, filter.c_str(), filter.size()) == 0) {
                            match = true; break;
                        }
                    }
                }
                if (!match) continue;
                shown++;
                char buf[256];
                std::snprintf(buf, sizeof(buf), "  [%d] %s (%s) 0x%08X", i, on.c_str(), cn.c_str(), (uint32_t)ptr);
                LogInfo(buf);
            }
            char buf[128]; std::snprintf(buf, sizeof(buf), "Found %d matches for '%s'", shown, filter.c_str());
            LogYellow(buf);
        }
        // ─── props [filter] ───
        else if (tokens[0] == "props") {
            UObject* player = FindObjectByClassName("ShockPlayer");
            UStruct* cls = FindClass("ShockPlayer");
            if (!player || !cls) { LogRed("No ShockPlayer found"); }
            else {
                auto props = WalkProperties(cls);
                std::string filter = tokens.size() >= 2 ? tokens[1] : "";
                int shown = 0;
                for (auto& p : props) {
                    if (!filter.empty()) {
                        bool match = false;
                        for (size_t c = 0; c + filter.size() <= p.Name.size(); c++) {
                            if (_strnicmp(p.Name.c_str() + c, filter.c_str(), filter.size()) == 0) {
                                match = true; break;
                            }
                        }
                        if (!match) continue;
                    }
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "  +%04X %-28s %s (%dB)",
                                 p.Offset, p.Name.c_str(), p.TypeName.c_str(), p.ElementSize);
                    LogInfo(buf);
                    if (++shown >= 50) { LogInfo("  ... (use filter to narrow)"); break; }
                }
                char buf[64]; std::snprintf(buf, sizeof(buf), "Total: %d props (showing %d)", (int)props.size(), shown);
                LogYellow(buf);
            }
        }
        // ─── classes [filter] ───
        else if (tokens[0] == "classes") {
            auto& globals = GetEngineGlobals();
            uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
            int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
            std::string filter = tokens.size() >= 2 ? tokens[1] : "";
            std::set<std::string> seen;
            for (int i = 0; i < objCount; i++) {
                uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
                if (!ptr) continue;
                UObject* obj = reinterpret_cast<UObject*>(ptr);
                if (obj->GetObjClassName() != "Class") continue;
                std::string n = obj->GetName();
                if (!filter.empty()) {
                    bool match = false;
                    for (size_t c = 0; c + filter.size() <= n.size(); c++) {
                        if (_strnicmp(n.c_str() + c, filter.c_str(), filter.size()) == 0) {
                            match = true; break;
                        }
                    }
                    if (!match) continue;
                }
                seen.insert(n);
            }
            for (auto& n : seen) LogInfo("  " + n);
            char buf[64]; std::snprintf(buf, sizeof(buf), "Found %d classes", (int)seen.size());
            LogYellow(buf);
        }
        // ─── setobj <class> <prop> <value> ───
        else if (tokens[0] == "setobj" && tokens.size() >= 4) {
            std::string clsName = tokens[1];
            std::string propName = tokens[2];
            std::string valStr = tokens[3];
            
            UObject* obj = FindObjectByClassName(clsName);
            if (!obj) { LogRed("No instance of '" + clsName + "' found"); }
            else {
                UStruct* cls = FindClass(clsName);
                if (!cls) { LogRed("Class '" + clsName + "' not found"); }
                else {
                    auto props = WalkProperties(cls);
                    PropertyInfo* pi = nullptr;
                    for (auto& p : props) {
                        if (p.Name == propName) { pi = &p; break; }
                    }
                    if (!pi) { LogRed("Property '" + propName + "' not found on " + clsName); }
                    else if (pi->TypeName == "IntProperty") {
                        int32_t val = std::atoi(valStr.c_str());
                        obj->SetField<int32_t>(pi->Offset, val);
                        char buf[128]; std::snprintf(buf, sizeof(buf), "%s.%s = %d", clsName.c_str(), propName.c_str(), val);
                        LogGreen(buf);
                    } else if (pi->TypeName == "FloatProperty") {
                        float val = std::strtof(valStr.c_str(), nullptr);
                        obj->SetField<float>(pi->Offset, val);
                        char buf[128]; std::snprintf(buf, sizeof(buf), "%s.%s = %.2f", clsName.c_str(), propName.c_str(), val);
                        LogGreen(buf);
                    } else {
                        LogRed("Can't set " + pi->TypeName + " from console");
                    }
                }
            }
        }
        // ─── hookpe ─── toggle ProcessEvent hook
        else if (tokens[0] == "hookpe") {
            if (IsProcessEventHooked()) {
                ShutdownProcessEventHook();
                LogYellow("ProcessEvent hook disabled");
            } else {
                if (InitProcessEventHook())
                    LogGreen("ProcessEvent hook enabled");
                else
                    LogRed("Failed to hook ProcessEvent");
            }
        }
        // ─── logpe [filter] ─── log ProcessEvent calls for N seconds
        else if (tokens[0] == "logpe") {
            if (!IsProcessEventHooked()) {
                LogRed("ProcessEvent not hooked. Run 'hookpe' first.");
            } else {
                std::string filter = tokens.size() >= 2 ? tokens[1] : "";
                static int logHookId = -1;
                if (logHookId >= 0) {
                    UnregisterProcessEventHook(logHookId);
                    logHookId = -1;
                    LogYellow("Stopped logging ProcessEvent calls");
                } else {
                    // Register a logging hook
                    struct LogState {
                        std::vector<std::pair<ImVec4, std::string>>* logHistory;
                        std::set<std::string> seen;
                        int count;
                    };
                    static LogState state;
                    state = {&logHistory, {}, 0};
                    
                    ProcessEventHook hook;
                    hook.Name = "ConsoleLog";
                    hook.FunctionFilter = filter;
                    hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
                        if (state.count >= 200) return false;
                        std::string fn = func->GetName();
                        std::string on = obj->GetName();
                        std::string key = on + "." + fn;
                        if (state.seen.count(key)) return false;
                        state.seen.insert(key);
                        state.count++;
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "  PE: %s.%s", on.c_str(), fn.c_str());
                        state.logHistory->push_back({ImVec4(0.5f, 0.8f, 1.0f, 1.0f), buf});
                        return false; // don't block
                    };
                    logHookId = RegisterProcessEventHook(hook);
                    if (filter.empty())
                        LogGreen("Logging ALL ProcessEvent calls (unique only, max 200). Run 'logpe' again to stop.");
                    else
                        LogGreen("Logging ProcessEvent calls for '" + filter + "'. Run 'logpe' to stop.");
                }
            }
        }
        // ─── pestats ─── show ProcessEvent statistics
        else if (tokens[0] == "pestats") {
            if (!IsProcessEventHooked()) {
                LogRed("ProcessEvent not hooked");
            } else {
                const auto& st = GetProcessEventStats();
                char buf[256];
                std::snprintf(buf, sizeof(buf), "Total calls: %llu  Blocked: %llu", st.TotalCalls, st.BlockedCalls);
                LogInfo(buf);
                std::snprintf(buf, sizeof(buf), "Last: %s.%s", st.LastObjectName.c_str(), st.LastFunctionName.c_str());
                LogInfo(buf);
            }
        }
        // ─── call <class> <function> [args...] ─── invoke a UFunction
        else if (tokens[0] == "call" && tokens.size() >= 3) {
            std::string clsName = tokens[1];
            std::string funcName = tokens[2];
            
            UObject* obj = FindObjectByClassName(clsName);
            if (!obj) { LogRed("No instance of '" + clsName + "'"); }
            else {
                UStruct* cls = reinterpret_cast<UStruct*>(obj->GetClass());
                UFunction* func = FindFunction(cls, funcName);
                if (!func) { LogRed("Function '" + funcName + "' not found on " + clsName); }
                else {
                    // Gather remaining tokens as args
                    std::vector<std::string> args;
                    for (size_t a = 3; a < tokens.size(); a++)
                        args.push_back(tokens[a]);
                    
                    std::string result = CallFunction(obj, func, args);
                    LogGreen(clsName + "." + funcName + "() -> " + result);
                }
            }
        }
        // ─── funcs <class> [filter] ─── list functions on a class
        else if (tokens[0] == "funcs" && tokens.size() >= 2) {
            std::string clsName = tokens[1];
            std::string filter = tokens.size() >= 3 ? tokens[2] : "";
            UStruct* cls = FindClass(clsName);
            if (!cls) { LogRed("Class '" + clsName + "' not found"); }
            else {
                auto funcs = GetClassFunctions(cls);
                int shown = 0;
                for (auto& fi : funcs) {
                    if (!filter.empty()) {
                        bool match = false;
                        for (size_t c = 0; c + filter.size() <= fi.Name.size(); c++) {
                            if (_strnicmp(fi.Name.c_str() + c, filter.c_str(), filter.size()) == 0) {
                                match = true; break;
                            }
                        }
                        if (!match) continue;
                    }
                    // Build param string
                    std::string paramStr;
                    for (auto& p : fi.Params) {
                        if (p.IsReturnParam) continue;
                        if (!paramStr.empty()) paramStr += ", ";
                        paramStr += p.TypeName.substr(0, p.TypeName.find("Property")) + " " + p.Name;
                    }
                    // Check for return type
                    std::string retStr = "void";
                    for (auto& p : fi.Params) {
                        if (p.IsReturnParam) {
                            retStr = p.TypeName.substr(0, p.TypeName.find("Property"));
                            break;
                        }
                    }
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "  %s %s(%s)", retStr.c_str(), fi.Name.c_str(), paramStr.c_str());
                    LogInfo(buf);
                    if (++shown >= 50) { LogInfo("  ... (use filter)"); break; }
                }
                char buf[64]; std::snprintf(buf, sizeof(buf), "Found %d functions", shown);
                LogYellow(buf);
            }
        }
        // ─── dumpevents [seconds] ─── log all PE calls to file
        else if (tokens[0] == "dumpevents") {
            if (!IsProcessEventHooked()) {
                LogRed("ProcessEvent not hooked. Run 'hookpe' first.");
            } else {
                static int dumpHookId = -1;
                if (dumpHookId >= 0) {
                    UnregisterProcessEventHook(dumpHookId);
                    dumpHookId = -1;
                    LogYellow("Event dump stopped. File: event_dump.txt");
                } else {
                    // Open file and register hook
                    static std::ofstream dumpFile;
                    dumpFile.open("Z:\\Bioshock1SDK\\event_dump.txt", std::ios::out | std::ios::trunc);
                    if (!dumpFile.is_open()) {
                        LogRed("Failed to open event_dump.txt");
                    } else {
                        dumpFile << "=== ProcessEvent Dump ===\n";
                        dumpFile << "Format: Object.Function (ParamsSize)\n\n";
                        
                        ProcessEventHook hook;
                        hook.Name = "EventDump";
                        hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
                            if (dumpFile.is_open()) {
                                std::string line = obj->GetName() + "." + func->GetName();
                                line += " (" + std::to_string(func->GetPropertiesSize()) + "B)";
                                line += " [" + obj->GetObjClassName() + "]\n";
                                dumpFile << line;
                                dumpFile.flush();
                            }
                            return false;
                        };
                        dumpHookId = RegisterProcessEventHook(hook);
                        LogGreen("Dumping events to event_dump.txt. Run 'dumpevents' again to stop.");
                    }
                }
            }
        }
        // ─── teleport [distance] ─── blink-teleport forward
        else if (tokens[0] == "teleport" || tokens[0] == "tp") {
            float dist = 800.0f;
            if (tokens.size() >= 2) dist = std::stof(tokens[1]);
            if (DoTeleport(dist))
                LogGreen("Teleported " + std::to_string((int)dist) + " units forward");
            else
                LogRed("Teleport failed (player not found?)");
        }
        // ─── initplasmid ─── hijack Security Bullseye → teleport
        else if (tokens[0] == "initplasmid") {
            if (InitTeleportPlasmid())
                LogGreen("Security Bullseye hijacked → Teleport! Fire it to blink forward.");
            else
                LogRed("Failed to init teleport plasmid");
        }
        // ─── tpdist <distance> ─── set teleport distance
        else if (tokens[0] == "tpdist" && tokens.size() >= 2) {
            float d = std::stof(tokens[1]);
            SetTeleportDistance(d);
            LogGreen("Teleport distance: " + std::to_string((int)d));
        }
        // ─── lua <code> ─── execute Lua code
        else if (tokens[0] == "lua" && tokens.size() >= 2) {
            if (!IsLuaInitialized()) InitLuaEngine();
            // Rejoin all tokens after "lua" into one string
            std::string code = cmd.substr(cmd.find(' ') + 1);
            std::string result = LuaExec(code);
            if (result.empty()) {
                LogGreen("OK");
            } else if (result[0] == '=') {
                LogGreen(result);
            } else {
                LogRed("[Lua] " + result);
            }
        }
        // ─── luafile <path> ─── execute a Lua file
        else if (tokens[0] == "luafile" && tokens.size() >= 2) {
            if (!IsLuaInitialized()) InitLuaEngine();
            std::string path = tokens[1];
            // If relative, prepend scripts dir
            if (path.find(':') == std::string::npos && path[0] != '\\' && path[0] != '/') {
                path = GetScriptsDir() + "\\" + path;
            }
            std::string result = LuaExecFile(path);
            if (result.empty()) LogGreen("Executed: " + path);
            else LogRed("[Lua] " + result);
        }
        // ─── reload ─── hot-reload autorun.lua
        else if (tokens[0] == "reload") {
            if (!IsLuaInitialized()) InitLuaEngine();
            std::string result = LuaReload();
            if (result.empty()) LogGreen("autorun.lua reloaded");
            else LogRed(result);
        }
        // ─── unknown ───
        else {
            LogRed("Unknown command: " + tokens[0] + " (type 'help' for commands)");
        }
    }
    
    ImGui::End();
}

// ─── Mod Menu ─────────────────────────────────────────────────────────────

void Overlay::RenderModMenu()
{
    ImGui::SetNextWindowPos(ImVec2(440, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Gameplay Mods (F7)", nullptr);

    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) {
        ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Engine globals not found");
        ImGui::End();
        return;
    }

    UObject* player = FindObjectByClassName("ShockPlayer");

    // ═══════════════════════════════════════════════════════
    if (ImGui::CollapsingHeader("Player Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!player) {
            ImGui::TextColored(ImVec4(1, 1, 0.4f, 1), "Player not spawned");
        } else {
            UStruct* cls = FindClass("ShockPlayer");
            if (cls) {
                auto props = WalkProperties(cls);
                auto FindProp = [&](const char* name) -> PropertyInfo* {
                    for (auto& p : props) if (p.Name == name) return &p;
                    return nullptr;
                };

                auto EditInt = [&](const char* label, const char* propName) {
                    if (auto* p = FindProp(propName)) {
                        int32_t val = player->GetField<int32_t>(p->Offset);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::InputInt(label, &val, 10, 100))
                            player->SetField<int32_t>(p->Offset, val);
                    }
                };
                auto EditFloat = [&](const char* label, const char* propName) {
                    if (auto* p = FindProp(propName)) {
                        float val = player->GetField<float>(p->Offset);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::InputFloat(label, &val, 10.0f, 100.0f, "%.1f"))
                            player->SetField<float>(p->Offset, val);
                    }
                };

                EditInt("ADAM##mod", "ADAM");
                EditInt("Credits##mod", "Credits");
                EditFloat("Health##mod", "Health");
                EditFloat("EVE (BioAmmo)##mod", "BioAmmo");
                EditFloat("Max EVE##mod", "MaxBioAmmo");

                ImGui::Separator();
                if (ImGui::Button("Max Resources")) {
                    if (auto* p = FindProp("ADAM")) player->SetField<int32_t>(p->Offset, 9999);
                    if (auto* p = FindProp("Credits")) player->SetField<int32_t>(p->Offset, 9999);
                    if (auto* p = FindProp("Health")) player->SetField<float>(p->Offset, 9999.0f);
                    if (auto* p = FindProp("BioAmmo")) player->SetField<float>(p->Offset, 100.0f);
                }
                ImGui::SameLine();
                if (ImGui::Button("Full Heal")) {
                    if (auto* p = FindProp("Health")) {
                        auto* pMax = FindProp("HealthMax");
                        float maxH = pMax ? player->GetField<float>(pMax->Offset) : 500.0f;
                        player->SetField<float>(p->Offset, maxH);
                    }
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════
    if (ImGui::CollapsingHeader("Combat Mods", ImGuiTreeNodeFlags_DefaultOpen)) {
        // God Mode (block TakeDamage via ProcessEvent)
        static int godHookId = -1;
        static bool godActive = false;
        if (ImGui::Checkbox("God Mode (Block Damage)", &godActive)) {
            if (godActive) {
                if (!IsProcessEventHooked()) InitProcessEventHook();
                ProcessEventHook hook;
                hook.Name = "GodMode";
                hook.FunctionFilter = "TakeDamage";
                hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
                    // Block TakeDamage on the player pawn
                    std::string cn = obj->GetObjClassName();
                    if (cn == "ShockPlayer" || cn.find("ShockPawn") != std::string::npos)
                        return true;
                    return false;
                };
                godHookId = RegisterProcessEventHook(hook);
            } else if (godHookId >= 0) {
                UnregisterProcessEventHook(godHookId);
                godHookId = -1;
            }
        }
        if (godActive) ImGui::SameLine(), ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "ACTIVE");

        // One-Hit Kill (multiply damage)
        static int ohkHookId = -1;
        static bool ohkActive = false;
        if (ImGui::Checkbox("One-Hit Kill Enemies", &ohkActive)) {
            if (ohkActive) {
                if (!IsProcessEventHooked()) InitProcessEventHook();
                ProcessEventHook hook;
                hook.Name = "OneHitKill";
                hook.FunctionFilter = "TakeDamage";
                hook.Callback = [](UObject* obj, UFunction* func, void* parms) -> bool {
                    // If not player, set health to 0
                    std::string cn = obj->GetObjClassName();
                    if (cn != "ShockPlayer") {
                        UStruct* ocls = reinterpret_cast<UStruct*>(obj->GetClass());
                        if (ocls) {
                            auto oprops = WalkProperties(ocls);
                            for (auto& p : oprops) {
                                if (p.Name == "Health" && p.TypeName == "FloatProperty") {
                                    obj->SetField<float>(p.Offset, 0.0f);
                                    break;
                                }
                            }
                        }
                    }
                    return false;
                };
                ohkHookId = RegisterProcessEventHook(hook);
            } else if (ohkHookId >= 0) {
                UnregisterProcessEventHook(ohkHookId);
                ohkHookId = -1;
            }
        }
        if (ohkActive) ImGui::SameLine(), ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "ACTIVE");
    }

    // ═══════════════════════════════════════════════════════
    if (ImGui::CollapsingHeader("Weapon Editor")) {
        // Find all weapon instances
        static std::vector<std::pair<UObject*, std::string>> weapons;
        static int selectedWeapon = -1;
        static float refreshTimer = 0;

        refreshTimer += ImGui::GetIO().DeltaTime;
        if (refreshTimer > 2.0f || weapons.empty()) {
            refreshTimer = 0;
            weapons.clear();
            uintptr_t od = *reinterpret_cast<uintptr_t*>(globals.GObjects);
            int32_t oc = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
            for (int i = 0; i < oc; i++) {
                uintptr_t ptr = *reinterpret_cast<uintptr_t*>(od + i * 4);
                if (!ptr) continue;
                UObject* obj = reinterpret_cast<UObject*>(ptr);
                std::string cn = obj->GetObjClassName();
                // Weapons: Pistol, Shotgun, Wrench, MachineGun, ChemThrower, Crossbow, Grenade, RPG
                if (cn == "Pistol" || cn == "Shotgun" || cn == "Wrench" ||
                    cn == "MachineGun" || cn == "ChemThrower" || cn == "Crossbow" ||
                    cn == "GrenadeLauncher" || cn == "RPG") {
                    weapons.push_back({obj, obj->GetName() + " (" + cn + ")"});
                }
            }
        }

        if (weapons.empty()) {
            ImGui::TextColored(ImVec4(1, 1, 0.4f, 1), "No weapons found");
        } else {
            // Weapon selector dropdown
            const char* preview = selectedWeapon >= 0 && selectedWeapon < (int)weapons.size()
                ? weapons[selectedWeapon].second.c_str() : "Select weapon...";
            if (ImGui::BeginCombo("##WeaponSelect", preview)) {
                for (int i = 0; i < (int)weapons.size(); i++) {
                    bool selected = (i == selectedWeapon);
                    if (ImGui::Selectable(weapons[i].second.c_str(), selected))
                        selectedWeapon = i;
                }
                ImGui::EndCombo();
            }

            if (selectedWeapon >= 0 && selectedWeapon < (int)weapons.size()) {
                UObject* wep = weapons[selectedWeapon].first;
                UStruct* wcls = reinterpret_cast<UStruct*>(wep->GetClass());
                if (wcls) {
                    auto wprops = WalkProperties(wcls);
                    auto FindWProp = [&](const char* name) -> PropertyInfo* {
                        for (auto& p : wprops) if (p.Name == name) return &p;
                        return nullptr;
                    };

                    auto WepFloat = [&](const char* label, const char* propName) {
                        if (auto* p = FindWProp(propName)) {
                            float val = wep->GetField<float>(p->Offset);
                            ImGui::SetNextItemWidth(120);
                            if (ImGui::InputFloat(label, &val, 0.1f, 1.0f, "%.3f"))
                                wep->SetField<float>(p->Offset, val);
                        }
                    };
                    auto WepInt = [&](const char* label, const char* propName) {
                        if (auto* p = FindWProp(propName)) {
                            int32_t val = wep->GetField<int32_t>(p->Offset);
                            ImGui::SetNextItemWidth(120);
                            if (ImGui::InputInt(label, &val, 1, 10))
                                wep->SetField<int32_t>(p->Offset, val);
                        }
                    };

                    WepFloat("Fire Rate##wep", "BaseFireRate");
                    WepFloat("Accuracy##wep", "BaseAccuracy");
                    WepFloat("Reload Rate##wep", "BaseReloadRate");
                    WepFloat("Ammo Consumption##wep", "BaseAmmoConsumptionRate");
                    WepInt("Magazine Size##wep", "BaseMagazineSize");
                    WepInt("Rounds Remaining##wep", "RoundsRemaining");

                    ImGui::Separator();
                    if (ImGui::Button("Infinite Ammo##wep")) {
                        if (auto* p = FindWProp("RoundsRemaining"))
                            wep->SetField<int32_t>(p->Offset, 9999);
                        if (auto* p = FindWProp("BaseMagazineSize"))
                            wep->SetField<int32_t>(p->Offset, 9999);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Rapid Fire##wep")) {
                        if (auto* p = FindWProp("BaseFireRate"))
                            wep->SetField<float>(p->Offset, 10.0f);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset##wep")) {
                        if (auto* p = FindWProp("BaseFireRate"))
                            wep->SetField<float>(p->Offset, 1.0f);
                        if (auto* p = FindWProp("BaseAccuracy"))
                            wep->SetField<float>(p->Offset, 1.0f);
                        if (auto* p = FindWProp("BaseReloadRate"))
                            wep->SetField<float>(p->Offset, 1.0f);
                    }
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════
    if (ImGui::CollapsingHeader("Plasmid Hijacks", ImGuiTreeNodeFlags_DefaultOpen)) {
        static bool hijacksActive = false;

        if (!hijacksActive) {
            if (ImGui::Button("Activate Plasmid Hijacks")) {
                hijacksActive = InitPlasmidHijacks();
            }
            ImGui::TextWrapped("Replaces Security Bullseye and Hypnotize Big Daddy with custom effects.");
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "%s",
                               GetPlasmidHijackStatus().c_str());
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1, 1), "Security Bullseye -> Teleport");
        ImGui::TextWrapped("Throw the beacon — you teleport to where it lands.");

        float tpDist = GetTeleportDistance();
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderFloat("Fallback Dist##tp", &tpDist, 100.0f, 5000.0f, "%.0f")) {
            SetTeleportDistance(tpDist);
        }
        ImGui::SameLine();
        if (ImGui::Button("Blink Fwd")) {
            DoTeleport(tpDist);
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Hypnotize Big Daddy -> Summon");
        ImGui::TextWrapped("Throw the dart — a friendly Big Daddy appears where it hits.");

        if (hijacksActive) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0.4f, 1),
                "Buy these at any Gatherer's Garden, equip, and fire!");
        }
    }

    // ═══════════════════════════════════════════════════════
    if (ImGui::CollapsingHeader("ProcessEvent Monitor")) {
        if (!IsProcessEventHooked()) {
            if (ImGui::Button("Enable ProcessEvent Hook"))
                InitProcessEventHook();
        } else {
            const auto& st = GetProcessEventStats();
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "HOOKED");
            ImGui::SameLine();
            ImGui::Text("Calls: %llu", st.TotalCalls);
            if (!st.LastFunctionName.empty())
                ImGui::Text("Last: %s.%s", st.LastObjectName.c_str(), st.LastFunctionName.c_str());
        }
    }

    // ═══════════════════════════════════════════════════════
    if (ImGui::CollapsingHeader("Lua Scripting")) {
        if (!IsLuaInitialized()) {
            if (ImGui::Button("Initialize Lua Engine"))
                InitLuaEngine();
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Lua Active");
            ImGui::SameLine();
            if (ImGui::Button("Reload autorun.lua")) {
                std::string err = LuaReload();
                if (!err.empty()) LOG_ERROR("Lua reload: {}", err);
            }
            ImGui::Text("Scripts: %s", GetScriptsDir().c_str());

            static char luaBuf[512] = {};
            ImGui::SetNextItemWidth(-60);
            bool exec = ImGui::InputText("##luainput", luaBuf, sizeof(luaBuf),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Run") || exec) {
                if (luaBuf[0]) {
                    std::string result = LuaExec(luaBuf);
                    if (!result.empty()) LOG_INFO("[Lua] {}", result);
                    luaBuf[0] = '\0';
                }
            }
        }
    }

    ImGui::End();
}

void Overlay::RenderMemoryViewer()
{
    // TODO: Hex editor style memory view
}

} // namespace bs1sdk
