#include "overlay.h"
#include "../core/log.h"

#include "../engine/uobject.h"
#include "../engine/function_caller.h"
#include "../engine/world.h"
#include "../sdk/sdk_generator.h"
#include "../debug/coop_debug.h"
#include "../network/coop_true.h"
#include "../network/coop_world_sync.h"
#include "../network/coop_p2.h"
#include "../network/coop_inventory.h"
#include "../network/coop_transitions.h"
#include "../network/coop_testing.h"
#include "../hooks/process_event.h"
#include "../scripting/lua_bridge.h"
#include "../gameplay/teleport_plasmid.h"
#include "../gameplay/gameplay_mods.h"
#include "../core/mod_config.h"
#include "../network/coop_bridge.h"
#include "../network/coop_puppet.h"
#include "../network/coop_render.h"
#include "../network/coop_save.h"
#include "../network/net_manager.h"

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
#include <cmath>

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

    // Tick co-op network (approx 16ms at 60fps)
    if (IsCoopActive()) {
        CoopTick(1.0f / 60.0f);
        RenderCoopOverlay(); // Draw remote player marker
    }

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
        // Proper bitmask-aware bool toggle
        uint32_t bitMask = 1;
        if (p.PropertyObj) {
            bitMask = p.PropertyObj->GetField<uint32_t>(0x78);
            if (bitMask == 0) bitMask = 1;
        }
        uint32_t rawVal = *reinterpret_cast<const uint32_t*>(base + p.Offset);
        bool isSet = (rawVal & bitMask) != 0;
        if (ImGui::Checkbox(("##" + p.Name).c_str(), &isSet)) {
            uint32_t* target = reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(base) + p.Offset);
            if (isSet) *target |= bitMask;
            else *target &= ~bitMask;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(bit 0x%X)", bitMask);
    } else if (p.TypeName == "ByteProperty") {
        uint8_t val = *(base + p.Offset);
        int ival = val;
        ImGui::SetNextItemWidth(60);
        if (ImGui::InputInt(("##" + p.Name).c_str(), &ival, 1, 5)) {
            if (ival >= 0 && ival <= 255)
                const_cast<uint8_t*>(base)[p.Offset] = (uint8_t)ival;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", val);
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
        // Detect FVector/FRotator (12 bytes = 3 floats/ints)
        if (p.ElementSize == 12) {
            // Determine struct name from the UStructProperty's inner Struct pointer
            std::string structName;
            if (p.PropertyObj) {
                // UStructProperty has Struct* at +0x78 (after UProperty fields)
                UObject* inner = p.PropertyObj->GetField<UObject*>(0x78);
                if (inner && (uintptr_t)inner > 0x10000) structName = inner->GetName();
            }
            if (structName == "Rotator") {
                int32_t* rot = reinterpret_cast<int32_t*>(const_cast<uint8_t*>(base) + p.Offset);
                ImGui::SetNextItemWidth(70); ImGui::InputInt(("P##" + p.Name).c_str(), &rot[0], 0, 0);
                ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::InputInt(("Y##" + p.Name).c_str(), &rot[1], 0, 0);
                ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::InputInt(("R##" + p.Name).c_str(), &rot[2], 0, 0);
            } else {
                // Assume FVector (or Color)
                float* vec = reinterpret_cast<float*>(const_cast<uint8_t*>(base) + p.Offset);
                ImGui::SetNextItemWidth(70); ImGui::InputFloat(("X##" + p.Name).c_str(), &vec[0], 0, 0, "%.1f");
                ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::InputFloat(("Y##" + p.Name).c_str(), &vec[1], 0, 0, "%.1f");
                ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::InputFloat(("Z##" + p.Name).c_str(), &vec[2], 0, 0, "%.1f");
            }
        } else if (p.ElementSize == 16) {
            // FQuat or FPlane (4 floats)
            float* v = reinterpret_cast<float*>(const_cast<uint8_t*>(base) + p.Offset);
            ImGui::Text("%.2f %.2f %.2f %.2f", v[0], v[1], v[2], v[3]);
        } else if (p.ElementSize <= 16) {
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
    
    // Log history (static so it persists and is accessible from chat callback)
    static std::vector<std::pair<ImVec4, std::string>> logHistory;
    static bool chatCallbackRegistered = false;
    if (!chatCallbackRegistered) {
        chatCallbackRegistered = true;
        SetCoopChatCallback([](const std::string& sender, const std::string& msg) {
            logHistory.push_back({ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "[" + sender + "] " + msg});
            if (logHistory.size() > 200) logHistory.erase(logHistory.begin());
        });
    }
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
            LogYellow("=== Gameplay Mods ===");
            LogInfo("  initmods                - Activate all gameplay mods");
            LogInfo("  mods                    - Show gameplay mods status");
            LogInfo("  decoytp [on|off]        - Toggle Decoy→Teleport");
            LogInfo("  bots [on|off|limit N]   - Toggle friendly bots / set limit");
            LogInfo("  rivets [on|off]         - Toggle Rivet Pistol");
            LogInfo("  factions [on|off]       - Toggle splicer factions");
            LogInfo("  tag <1|2>               - Tag nearest splicer to faction");
            LogInfo("  chain [on|off|r N|j N]  - Chain lightning config");
            LogInfo("  saveconfig              - Save current mod settings to JSON");
            LogYellow("=== Co-op Multiplayer ===");
            LogInfo("  host [port]             - Host a co-op session");
            LogInfo("  join <ip> [port]        - Join a co-op session");
            LogInfo("  netstatus               - Show network status");
            LogInfo("  disconnect              - Leave co-op session");
            LogInfo("  chat <msg>              - Send chat to co-op partner");
            LogInfo("  syncsave                - Send your save to partner (world sync)");
            LogYellow("=== Lua Scripting ===");
            LogInfo("  lua <code>              - Execute Lua code inline");
            LogInfo("  luafile <path>          - Execute a Lua script file");
            LogInfo("  reload                  - Hot-reload autorun.lua");
            LogYellow("=== World / Actors ===");
            LogInfo("  actors [class]          - List actors (optionally filter by class)");
            LogInfo("  worldinfo               - Show level info + actor count");
            LogInfo("  nearby <radius> [class] - Find actors within radius");
            LogInfo("  tickrate                - Show current engine tick rate");
            LogYellow("=== Spawning / Movement ===");
            LogInfo("  spawn <class>           - Spawn actor at your position");
            LogInfo("  tp <x> <y> <z>          - Teleport to coordinates");
            LogInfo("  noclip                  - Toggle fly + no collision");
            LogYellow("=== Engine Internals ===");
            LogInfo("  cdo <class>             - Show ClassDefaultObject address");
            LogInfo("  setdefault <c> <p> <v>  - Set a CDO property value");
            LogInfo("  natives                 - Show GNatives info");
            LogInfo("  gensdk                  - Regenerate SDK headers");
            LogInfo("  assets [filter]         - Dump all loaded assets to file");
            LogYellow("=== Reverse Engineering ===");
            LogInfo("  dumpsdk                 - Generate full SDK (all classes/props/funcs)");
            LogInfo("  inspect <class|0xAddr>  - Live inspect object properties+values");
            LogInfo("  hierarchy               - Dump full class inheritance tree");
            LogInfo("  functions               - Dump all UFunction objects");
            LogYellow("=== Co-op Debug ===");
            LogInfo("  snapshot [label]        - Dump full engine state to file");
            LogInfo("  census                  - Dump actor class census");
            LogInfo("  offsets                 - Dump property offsets for key classes");
            LogInfo("  petrack [secs]          - Track PE frequency (default 10s)");
            LogInfo("  pestop                  - Stop PE tracker early and dump");
            LogInfo("  aifuncs                 - Catalog all AI/tick/spawn functions");
            LogInfo("  snapa / snapb           - Mark actor snapshot A/B");
            LogInfo("  snapdiff                - Diff snapshots A vs B");
            LogYellow("=== Co-op Analysis ===");
            LogInfo("  catalog [secs]          - Record ALL PE events (default 30s)");
            LogInfo("  interactions            - Dump all interaction event param layouts");
            LogInfo("  statediff [class] [sec] - Track property changes over time");
            LogYellow("=== True Co-op ===");
            LogInfo("  truehost [port]         - HOST: start co-op session");
            LogInfo("  truejoin <ip> [port]    - CLIENT: join a host");
            LogInfo("  freeze / unfreeze       - Toggle client sim freeze");
            LogInfo("  truecoop                - Show true co-op status");
            LogInfo("  coopdiag                - Full co-op diagnostics");
            LogInfo("  netstatus               - Network connection details");
            LogYellow("--- Quick Start Guide ---");
            LogInfo("  HOST:   truehost");
            LogInfo("  CLIENT: truejoin <host_ip>");
            LogInfo("  (default port: 27015)");
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
        // ─── initmods ─── activate all gameplay mods
        else if (tokens[0] == "initmods") {
            if (InitGameplayMods())
                LogGreen("All gameplay mods activated!");
            else
                LogRed("Failed to init gameplay mods");
        }
        // ─── mods ─── show status
        else if (tokens[0] == "mods") {
            LogInfo(GetGameplayModsStatus());
        }
        // ─── decoytp [on|off] ───
        else if (tokens[0] == "decoytp") {
            if (tokens.size() >= 2) {
                SetDecoyTeleportEnabled(tokens[1] == "on" || tokens[1] == "1");
            }
            LogInfo(std::string("Decoy Teleport: ") + (IsDecoyTeleportEnabled() ? "ON" : "OFF"));
        }
        // ─── bots [on|off|limit N] ───
        else if (tokens[0] == "bots") {
            if (tokens.size() >= 2) {
                if (tokens[1] == "limit" && tokens.size() >= 3) {
                    SetFriendlyBotLimit(std::stoi(tokens[2]));
                    LogGreen("Bot limit set to " + tokens[2]);
                } else {
                    SetFriendlyBotsEnabled(tokens[1] == "on" || tokens[1] == "1");
                }
            }
            LogInfo(std::string("Friendly Bots: ") + (IsFriendlyBotsEnabled() ? "ON" : "OFF") +
                    " (" + std::to_string(GetFriendlyBotCount()) + " alive)");
        }
        // ─── rivets [on|off] ───
        else if (tokens[0] == "rivets") {
            if (tokens.size() >= 2) {
                SetRivetPistolEnabled(tokens[1] == "on" || tokens[1] == "1");
            }
            LogInfo(std::string("Rivet Pistol: ") + (IsRivetPistolEnabled() ? "ON" : "OFF"));
        }
        // ─── factions [on|off] ───
        else if (tokens[0] == "factions") {
            if (tokens.size() >= 2) {
                SetSplicerFactionsEnabled(tokens[1] == "on" || tokens[1] == "1");
            }
            LogInfo(std::string("Splicer Factions: ") + (IsSplicerFactionsEnabled() ? "ON" : "OFF"));
        }
        // ─── tag <1|2> ─── tag nearest splicer
        else if (tokens[0] == "tag" && tokens.size() >= 2) {
            int fid = std::stoi(tokens[1]);
            TagSplicerFaction(fid);
        }
        // ─── chain [on|off|r N|j N] ───
        else if (tokens[0] == "chain") {
            if (tokens.size() >= 2) {
                if (tokens[1] == "on" || tokens[1] == "1") SetChainLightningEnabled(true);
                else if (tokens[1] == "off" || tokens[1] == "0") SetChainLightningEnabled(false);
                else if (tokens[1] == "r" && tokens.size() >= 3) SetChainLightningRadius(std::stof(tokens[2]));
                else if (tokens[1] == "j" && tokens.size() >= 3) SetChainLightningJumps(std::stoi(tokens[2]));
                else if (tokens[1] == "f" && tokens.size() >= 3) SetChainLightningDamageFalloff(std::stof(tokens[2]));
            }
            LogInfo(std::string("Chain Lightning: ") + (IsChainLightningEnabled() ? "ON" : "OFF"));
        }
        // ─── saveconfig ─── save current settings to mod_config.json
        else if (tokens[0] == "saveconfig") {
            ModConfig cfg;
            cfg.decoyTeleport = IsDecoyTeleportEnabled();
            cfg.friendlyBots = IsFriendlyBotsEnabled();
            cfg.friendlyBotLimit = 3; // TODO: expose getter
            cfg.rivetPistol = IsRivetPistolEnabled();
            cfg.splicerFactions = IsSplicerFactionsEnabled();
            cfg.chainLightning = IsChainLightningEnabled();
            SaveModConfig(cfg);
            LogGreen("Config saved to mod_config.json");
        }
        // ─── host [port] ─── host co-op session
        else if (tokens[0] == "host") {
            uint16_t port = 27015;
            if (tokens.size() >= 2) port = (uint16_t)std::stoi(tokens[1]);
            if (CoopHost(port)) LogGreen("Hosting on port " + std::to_string(port) + " - waiting for player...");
            else LogRed("Failed to start host");
        }
        // ─── join <ip> [port] ─── join co-op session
        else if (tokens[0] == "join" && tokens.size() >= 2) {
            std::string ip = tokens[1];
            uint16_t port = 27015;
            if (tokens.size() >= 3) port = (uint16_t)std::stoi(tokens[2]);
            if (CoopJoin(ip, port)) LogGreen("Connecting to " + ip + ":" + std::to_string(port) + "...");
            else LogRed("Failed to join");
        }
        // ─── netstatus ─── show co-op network status
        else if (tokens[0] == "netstatus") {
            LogInfo(GetCoopStatus());
        }
        // ─── disconnect ─── leave co-op session
        else if (tokens[0] == "disconnect") {
            CoopDisconnect();
            LogInfo("Disconnected from co-op session");
        }
        // ─── syncsave ─── send your save to partner so they load into your world
        else if (tokens[0] == "syncsave") {
            if (GetNetRole() == NetRole::None) {
                LogRed("Not connected. Host or join first.");
            } else if (IsSaveTransferActive()) {
                LogYellow("Save transfer already in progress (" +
                          std::to_string((int)(GetSaveTransferProgress() * 100)) + "%)");
            } else if (StartSaveTransfer()) {
                LogGreen("Sending your save to partner...");
                LogInfo("They will need to load 'CoopSync' from the menu after receiving.");
            } else {
                LogRed("Failed to start save transfer. Check logs.");
            }
        }
        // ─── chat <msg> ─── send chat message
        else if (tokens[0] == "chat" && tokens.size() >= 2) {
            std::string msg = cmd.substr(cmd.find(' ') + 1);
            NetSendChat(msg);
            LogInfo("[You] " + msg);
        }
        // ─── coopdump ─── dump spawnable classes, actors, meshes for co-op debugging
        else if (tokens[0] == "coopdump") {
            const auto& g = GetEngineGlobals();
            if (!g.IsValid()) { LogRed("Engine not ready"); }
            else {
                std::ofstream df("Z:\\Bioshock1SDK\\coop_dump.txt", std::ios::out | std::ios::trunc);
                if (!df.is_open()) { LogRed("Can't write coop_dump.txt"); }
                else {
                    uintptr_t od = *reinterpret_cast<uintptr_t*>(g.GObjects);
                    int32_t oc = *reinterpret_cast<int32_t*>(g.GObjects + 4);
                    df << "=== Co-op Debug Dump ===\n";
                    df << "GObjects count: " << oc << "\n\n";

                    df << "--- CLASSES (UClass objects) ---\n";
                    for (int i = 0; i < oc && i < 100000; i++) {
                        uintptr_t p = *reinterpret_cast<uintptr_t*>(od + i * 4);
                        if (!p) continue;
                        UObject* o = reinterpret_cast<UObject*>(p);
                        if (o->GetObjClassName() == "Class") {
                            df << "  [" << i << "] " << o->GetName() << "\n";
                        }
                    }

                    df << "\n--- ACTORS (instances in world) ---\n";
                    std::map<std::string, int> actorCounts;
                    for (int i = 0; i < oc && i < 100000; i++) {
                        uintptr_t p = *reinterpret_cast<uintptr_t*>(od + i * 4);
                        if (!p) continue;
                        UObject* o = reinterpret_cast<UObject*>(p);
                        std::string cn = o->GetObjClassName();
                        // Skip pure data objects
                        if (cn == "Class" || cn == "Function" || cn == "Package" ||
                            cn == "Property" || cn == "Struct" || cn == "Const" ||
                            cn == "Enum" || cn == "State" || cn == "ScriptStruct") continue;
                        actorCounts[cn]++;
                    }
                    for (auto& kv : actorCounts) {
                        df << "  " << kv.first << " x" << kv.second << "\n";
                    }

                    df << "\n--- MESH OBJECTS (StaticMesh/SkeletalMesh) ---\n";
                    for (int i = 0; i < oc && i < 100000; i++) {
                        uintptr_t p = *reinterpret_cast<uintptr_t*>(od + i * 4);
                        if (!p) continue;
                        UObject* o = reinterpret_cast<UObject*>(p);
                        std::string cn = o->GetObjClassName();
                        if (cn == "SkeletalMesh" || cn == "StaticMesh") {
                            df << "  [" << i << "] " << cn << ": " << o->GetName() << "\n";
                        }
                    }

                    df << "\n--- SPAWNER-RELATED ---\n";
                    for (int i = 0; i < oc && i < 100000; i++) {
                        uintptr_t p = *reinterpret_cast<uintptr_t*>(od + i * 4);
                        if (!p) continue;
                        UObject* o = reinterpret_cast<UObject*>(p);
                        std::string cn = o->GetObjClassName();
                        std::string nm = o->GetName();
                        if (cn.find("Spawn") != std::string::npos || cn.find("Decoy") != std::string::npos ||
                            cn.find("Puppet") != std::string::npos || nm.find("Spawn") != std::string::npos ||
                            cn.find("Attachment") != std::string::npos) {
                            df << "  [" << i << "] " << cn << ": " << nm << "\n";
                        }
                    }

                    df.close();
                    LogGreen("Co-op dump saved to coop_dump.txt (" + std::to_string(oc) + " objects scanned)");
                }
            }
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
        // ─── actors [class] ───
        else if (tokens[0] == "actors") {
            if (!IsWorldSystemReady()) { LogRed("World system not ready"); }
            else {
                std::string filter = (tokens.size() >= 2) ? tokens[1] : "";
                std::vector<UObject*> actors;
                if (filter.empty())
                    actors = GetAllActors();
                else
                    actors = GetActorsOfClass(filter);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Found %d actors%s", (int)actors.size(),
                             filter.empty() ? "" : (" of class " + filter).c_str());
                LogYellow(buf);
                int shown = 0;
                for (auto* a : actors) {
                    if (shown++ >= 50) { LogInfo("... (" + std::to_string(actors.size() - 50) + " more)"); break; }
                    LogInfo("  [" + a->GetObjClassName() + "] " + a->GetName());
                }
            }
        }
        // ─── worldinfo ───
        else if (tokens[0] == "worldinfo") {
            if (!IsWorldSystemReady()) { LogRed("World system not ready"); }
            else {
                LevelInfo info = GetCurrentLevel();
                char buf[256];
                std::snprintf(buf, sizeof(buf), "Level: %s\nActors: %d\nLevel ptr: 0x%08X",
                             info.LevelName.c_str(), info.ActorCount, (uint32_t)info.LevelPtr);
                LogGreen(buf);
                std::snprintf(buf, sizeof(buf), "Tick rate: %.1f FPS", GetTickRate());
                LogInfo(buf);
                std::snprintf(buf, sizeof(buf), "GNatives: %d functions", GetNativeCount());
                LogInfo(buf);
            }
        }
        // ─── nearby <radius> [class] ───
        else if (tokens[0] == "nearby" && tokens.size() >= 2) {
            float radius = std::strtof(tokens[1].c_str(), nullptr);
            std::string classFilter = (tokens.size() >= 3) ? tokens[2] : "";
            FVec3 playerPos;
            if (!GetPlayerPosition(playerPos)) { LogRed("Can't get player position"); }
            else {
                auto actors = GetActorsInRadius(playerPos, radius, classFilter);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%d actors within %.0f units", (int)actors.size(), radius);
                LogYellow(buf);
                for (auto* a : actors) {
                    float dist = GetActorDistance(FindObjectByClassName("ShockPlayer"), a);
                    std::snprintf(buf, sizeof(buf), "  [%s] %s (%.0f units)",
                                 a->GetObjClassName().c_str(), a->GetName().c_str(), dist);
                    LogInfo(buf);
                }
            }
        }
        // ─── tickrate ───
        else if (tokens[0] == "tickrate") {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Engine tick: %.1f FPS (hook %s)",
                         GetTickRate(), IsTickHookActive() ? "active" : "inactive");
            LogInfo(buf);
        }
        // ─── cdo <class> ───
        else if (tokens[0] == "cdo" && tokens.size() >= 2) {
            UObject* cdo = GetDefaultObject(tokens[1]);
            if (!cdo) { LogRed("No CDO for class: " + tokens[1]); }
            else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "CDO for %s: 0x%08X (%s)",
                             tokens[1].c_str(), (uint32_t)(uintptr_t)cdo, cdo->GetName().c_str());
                LogGreen(buf);
            }
        }
        // ─── setdefault <class> <prop> <value> ───
        else if (tokens[0] == "setdefault" && tokens.size() >= 4) {
            std::string className = tokens[1];
            std::string propName = tokens[2];
            std::string valStr = tokens[3];
            // Try int first, then float
            bool ok = false;
            if (valStr.find('.') != std::string::npos) {
                ok = SetDefaultProperty(className, propName, std::strtof(valStr.c_str(), nullptr));
            } else {
                ok = SetDefaultPropertyInt(className, propName, std::atoi(valStr.c_str()));
                if (!ok) ok = SetDefaultProperty(className, propName, std::strtof(valStr.c_str(), nullptr));
            }
            if (ok) LogGreen("Set " + className + "." + propName + " = " + valStr + " (CDO)");
            else LogRed("Failed to set CDO property");
        }
        // ─── natives ───
        else if (tokens[0] == "natives") {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "GNatives: %d functions at 0x%08X",
                         GetNativeCount(), (uint32_t)GetNativesTableAddress());
            LogGreen(buf);
            LogInfo("Full dump at Z:\\Bioshock1SDK\\gnatives_dump.txt");
        }
        // ─── gensdk ───
        else if (tokens[0] == "gensdk") {
            LogYellow("Regenerating SDK headers...");
            SDKGenerator gen;
            int count = gen.Generate("Z:\\Bioshock1SDK\\sdk_gen");
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SDK generated: %d classes", count);
            LogGreen(buf);
        }
        // ─── spawn <className> ─── spawn an actor at the player's position
        else if (tokens[0] == "spawn" && tokens.size() >= 2) {
            std::string clsName = tokens[1];
            UObject* player = FindObjectByClassName("ShockPlayer");
            if (!player) { LogRed("No ShockPlayer found"); }
            else {
                UStruct* cls = FindClass(clsName);
                if (!cls) { LogRed("Class '" + clsName + "' not found. Try: classes " + clsName); }
                else {
                    // Get player position for spawn location
                    FVec3 pos;
                    GetActorPosition(player, pos);
                    // Offset slightly in front (use Rotation to get forward direction)
                    const uint8_t* pBase = reinterpret_cast<const uint8_t*>(player);
                    // Rotation at 0x01E4 (FRotator: pitch, yaw, roll as int32)
                    int32_t yaw = *reinterpret_cast<const int32_t*>(pBase + 0x01E8); // Yaw component
                    float yawRad = (float)yaw * 3.14159265f / 32768.0f;
                    pos.X += cosf(yawRad) * 200.0f;
                    pos.Y += sinf(yawRad) * 200.0f;
                    pos.Z += 10.0f; // Slight lift

                    // Find the GameInfo to call SpawnActor via ProcessEvent
                    // Alternative: use the class factory approach
                    // For now, we look for an existing actor of that class and clone location
                    // or use native SpawnActor
                    UObject* level = FindObjectByClassName("Level");
                    if (!level) { LogRed("No Level found"); }
                    else {
                        // Try to find SpawnActor function on Level
                        UStruct* levelCls = reinterpret_cast<UStruct*>(level->GetClass());
                        UFunction* spawnFunc = FindFunction(levelCls, "SpawnActor");
                        if (!spawnFunc) {
                            // Fallback: look on GameInfo
                            UObject* gi = FindObjectByClassName("ShockGameInfo");
                            if (gi) {
                                levelCls = reinterpret_cast<UStruct*>(gi->GetClass());
                                spawnFunc = FindFunction(levelCls, "Spawn");
                                if (spawnFunc) level = gi;
                            }
                        }
                        if (!spawnFunc) {
                            LogRed("Cannot find SpawnActor/Spawn function - spawn not supported yet");
                            LogInfo("Tip: Use 'summon " + clsName + "' in engine console (tilde ~)");
                        } else {
                            std::vector<std::string> args;
                            args.push_back(clsName);
                            std::string result = CallFunction(level, spawnFunc, args);
                            LogGreen("Spawn " + clsName + " -> " + result);
                        }
                    }
                }
            }
        }
        // ─── assets [filter] ─── dump loaded assets (textures, meshes, sounds, materials)
        else if (tokens[0] == "assets") {
            std::string filter = (tokens.size() >= 2) ? tokens[1] : "";
            auto& globals = GetEngineGlobals();
            if (!globals.IsValid()) { LogRed("Engine not initialized"); }
            else {
                uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
                int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);
                // Asset types we care about
                std::map<std::string, int> assetCounts;
                std::ofstream out("Z:\\Bioshock1SDK\\sdk_gen\\SDK_Assets.txt");
                out << "// BioShock Remastered - Loaded Asset Browser\n";
                out << "// Generated by BS1SDK\n\n";

                std::vector<std::pair<std::string, std::string>> entries; // {type, name}
                for (int i = 0; i < objCount && i < 200000; i++) {
                    uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
                    if (!ptr) continue;
                    UObject* obj = reinterpret_cast<UObject*>(ptr);
                    std::string cn = obj->GetObjClassName();
                    // Filter to asset types
                    bool isAsset = (cn == "Texture" || cn == "StaticMesh" || cn == "SkeletalMesh" ||
                                    cn == "Sound" || cn == "Material" || cn == "Shader" ||
                                    cn == "Combiner" || cn == "FinalBlend" || cn == "TexModifier" ||
                                    cn == "TexOscillator" || cn == "TexPanner" || cn == "TexScaler" ||
                                    cn == "Palette" || cn == "Font" || cn == "Level" ||
                                    cn == "Animation" || cn == "MeshAnimation" ||
                                    cn == "Package" || cn == "Music");
                    if (!isAsset) continue;

                    std::string name = obj->GetFullPath();
                    if (!filter.empty()) {
                        bool match = false;
                        for (size_t c = 0; c + filter.size() <= name.size(); c++) {
                            if (_strnicmp(name.c_str() + c, filter.c_str(), filter.size()) == 0) {
                                match = true; break;
                            }
                        }
                        if (!match) continue;
                    }
                    assetCounts[cn]++;
                    entries.push_back({cn, name});
                }
                // Sort by type then name
                std::sort(entries.begin(), entries.end());
                std::string currentType;
                for (auto& e : entries) {
                    if (e.first != currentType) {
                        currentType = e.first;
                        out << "\n// ═══ " << currentType << " ═══\n";
                    }
                    out << "  " << e.second << "\n";
                }
                out << "\n// ═══ Summary ═══\n";
                int total = 0;
                for (auto& kv : assetCounts) {
                    out << "// " << kv.first << ": " << kv.second << "\n";
                    total += kv.second;
                }
                out << "// Total: " << total << " assets\n";
                out.close();

                // Console output
                for (auto& kv : assetCounts) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "  %-20s %d", kv.first.c_str(), kv.second);
                    LogInfo(buf);
                }
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Total: %d assets -> sdk_gen/SDK_Assets.txt", total);
                LogGreen(buf);
            }
        }
        // ─── tp <x> <y> <z> ─── teleport player to coordinates
        else if (tokens[0] == "tp" && tokens.size() >= 4) {
            UObject* player = FindObjectByClassName("ShockPlayer");
            if (!player) { LogRed("No ShockPlayer found"); }
            else {
                FVec3 dest;
                dest.X = std::strtof(tokens[1].c_str(), nullptr);
                dest.Y = std::strtof(tokens[2].c_str(), nullptr);
                dest.Z = std::strtof(tokens[3].c_str(), nullptr);
                SetActorPosition(player, dest);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Teleported to (%.0f, %.0f, %.0f)", dest.X, dest.Y, dest.Z);
                LogGreen(buf);
            }
        }
        // ─── noclip ─── toggle noclip mode (fly through walls)
        else if (tokens[0] == "noclip") {
            static bool noclipOn = false;
            UObject* player = FindObjectByClassName("ShockPlayer");
            if (!player) { LogRed("No ShockPlayer found"); }
            else {
                noclipOn = !noclipOn;
                if (noclipOn) {
                    // PHYS_Flying = 6, disable collision
                    uint8_t physFly = 6;
                    int32_t bFalse = 0;
                    SetActorProperty(player, "Physics", &physFly, 1);
                    SetActorProperty(player, "bCollideWorld", &bFalse, 4);
                    SetActorProperty(player, "bBlockActors", &bFalse, 4);
                    SetActorProperty(player, "bBlockPlayers", &bFalse, 4);
                    LogGreen("Noclip ON - PHYS_Flying, collision disabled");
                } else {
                    // PHYS_Walking = 2, re-enable collision
                    uint8_t physWalk = 2;
                    int32_t bTrue = 1;
                    SetActorProperty(player, "Physics", &physWalk, 1);
                    SetActorProperty(player, "bCollideWorld", &bTrue, 4);
                    SetActorProperty(player, "bBlockActors", &bTrue, 4);
                    SetActorProperty(player, "bBlockPlayers", &bTrue, 4);
                    LogYellow("Noclip OFF - restored walking + collision");
                }
            }
        }
        // ─── snapshot [label] ───
        else if (tokens[0] == "snapshot") {
            std::string label = (tokens.size() >= 2) ? tokens[1] : "";
            DumpFullSnapshot(label);
            LogGreen("Full snapshot dumped to debug_dumps/");
        }
        // ─── census ───
        else if (tokens[0] == "census") {
            DumpActorCensus();
            LogGreen("Actor census dumped to debug_dumps/actor_census.txt");
        }
        // ─── offsets ───
        else if (tokens[0] == "offsets") {
            DumpPropertyOffsets();
            LogGreen("Property offsets dumped to debug_dumps/property_offsets.txt");
        }
        // ─── petrack [seconds] ───
        else if (tokens[0] == "petrack") {
            if (IsPETrackerRunning()) {
                LogYellow("PE tracker already running");
            } else {
                float secs = (tokens.size() >= 2) ? std::strtof(tokens[1].c_str(), nullptr) : 10.0f;
                StartPETracker(secs);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "PE tracker started (%.0fs)", secs);
                LogGreen(buf);
            }
        }
        // ─── pestop ───
        else if (tokens[0] == "pestop") {
            if (!IsPETrackerRunning()) {
                LogYellow("PE tracker not running");
            } else {
                StopPETracker();
                DumpPEFrequency();
                LogGreen("PE tracker stopped, dumped to debug_dumps/pe_frequency.txt");
            }
        }
        // ─── aifuncs ───
        else if (tokens[0] == "aifuncs") {
            DumpAIFunctions();
            LogGreen("AI function catalog dumped to debug_dumps/ai_functions.txt");
        }
        // ─── snapa / snapb ───
        else if (tokens[0] == "snapa") {
            MarkSnapshotA();
            LogGreen("Snapshot A marked");
        }
        else if (tokens[0] == "snapb") {
            MarkSnapshotB();
            LogGreen("Snapshot B marked");
        }
        // ─── snapdiff ───
        else if (tokens[0] == "snapdiff") {
            DumpSnapshotDiff();
            LogGreen("Snapshot diff dumped to debug_dumps/snapshot_diff.txt");
        }
        // ─── catalog [seconds] ─── record all PE events for co-op sync analysis
        else if (tokens[0] == "catalog") {
            if (IsEventCatalogRunning()) {
                StopEventCatalog();
                DumpEventCatalog();
                LogGreen("Event catalog stopped and dumped");
            } else {
                float secs = (tokens.size() >= 2) ? std::strtof(tokens[1].c_str(), nullptr) : 30.0f;
                StartEventCatalog(secs);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Event catalog started (%.0fs) - play normally!", secs);
                LogGreen(buf);
                LogInfo("Open doors, pick up items, fight enemies, etc.");
                LogInfo("Run 'catalog' again to stop early. Auto-dumps after timeout.");
            }
        }
        // ─── interactions ─── dump all interaction event layouts
        else if (tokens[0] == "interactions") {
            DumpInteractionEvents();
            LogGreen("Interaction events dumped to debug_dumps/interaction_events.txt");
        }
        // ─── statediff [class] [seconds] ─── track property changes over time
        else if (tokens[0] == "statediff") {
            if (IsStateDiffRunning()) {
                StopStateDiff();
                DumpStateDiff();
                LogGreen("State diff stopped and dumped");
            } else {
                std::string filter = (tokens.size() >= 2) ? tokens[1] : "";
                float secs = (tokens.size() >= 3) ? std::strtof(tokens[2].c_str(), nullptr) : 10.0f;
                StartStateDiff(filter, secs);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "State diff started: filter='%s' (%.0fs)", filter.c_str(), secs);
                LogGreen(buf);
                LogInfo("Interact with world (open door, pick up item, kill enemy)");
                LogInfo("Run 'statediff' again to stop early.");
            }
        }
        // ─── truehost [port] ───
        else if (tokens[0] == "truehost") {
            uint16_t port = (tokens.size() >= 2) ? (uint16_t)std::atoi(tokens[1].c_str()) : 27015;
            EnsureSubsystemsReady();

            // Set current level name for host/client matching
            auto level = GetCurrentLevel();
            if (level.LevelName[0]) {
                NetSetLocalLevel(level.LevelName);
                LogInfo("Level: " + std::string(level.LevelName));
            }

            SetTrueCoopRole(TrueCoopRole::TrueHost);
            if (CoopHost(port, "TrueHost")) {
                LogGreen("True co-op HOST started on port " + std::to_string(port));
                LogInfo("Waiting for client to join...");
                LogInfo("Tick hook: " + std::string(IsTickHookActive() ? "active" : "NOT active"));
                LogInfo("PE hook: " + std::string(IsProcessEventHooked() ? "active" : "NOT active"));
                LogYellow("────────────────────────────────────────");
                LogYellow("Tell the CLIENT to run:");
                LogGreen("  truejoin <YOUR_IP> " + std::to_string(port));
                LogYellow("(Use Hamachi/ZeroTier IP or LAN IP)");
                LogYellow("────────────────────────────────────────");

                // P2 splicer pawn disabled for now — use ghost puppet only
                // Use 'p2spawn' command to manually test splicer commandeering
                LogInfo("Ghost puppet will track remote player position");
            } else {
                LogRed("Failed to start true co-op host");
                LogRed("Check: Is port " + std::to_string(port) + " already in use?");
            }
        }
        // ─── truejoin <ip> [port] ───
        else if (tokens[0] == "truejoin" && tokens.size() >= 2) {
            std::string ip = tokens[1];
            uint16_t port = (tokens.size() >= 3) ? (uint16_t)std::atoi(tokens[2].c_str()) : 27015;
            EnsureSubsystemsReady();

            // Set current level name for matching
            auto level = GetCurrentLevel();
            if (level.LevelName[0]) {
                NetSetLocalLevel(level.LevelName);
                LogInfo("Level: " + std::string(level.LevelName));
            }

            SetTrueCoopRole(TrueCoopRole::TrueClient);
            if (CoopJoin(ip, port, "TrueClient")) {
                LogGreen("True co-op CLIENT joining " + ip + ":" + std::to_string(port));
                LogInfo("Tick hook: " + std::string(IsTickHookActive() ? "active" : "NOT active"));
                LogInfo("PE hook: " + std::string(IsProcessEventHooked() ? "active" : "NOT active"));
                if (FreezeClientSimulation()) {
                    LogYellow("Client simulation FROZEN (AI/physics/spawn blocked)");
                } else {
                    LogRed("WARNING: Failed to freeze simulation!");
                }
                LogInfo("Connecting... (will timeout in 30s if host unreachable)");
            } else {
                LogRed("Failed to join true co-op session");
                LogRed("Check: Is " + ip + ":" + std::to_string(port) + " reachable?");
                LogRed("       Host must run 'truehost' first!");
            }
        }
        // ─── freeze ───
        else if (tokens[0] == "freeze") {
            if (FreezeClientSimulation()) {
                LogGreen("Client simulation FROZEN (AI/physics/spawn blocked)");
            } else {
                LogRed("Failed to freeze (PE hook not active?)");
            }
        }
        // ─── unfreeze ───
        else if (tokens[0] == "unfreeze") {
            UnfreezeClientSimulation();
            LogGreen("Client simulation UNFROZEN");
        }
        // ─── truecoop ───
        else if (tokens[0] == "truecoop") {
            LogInfo(GetTrueCoopStatus());
            if (IsSimulationFrozen()) {
                auto frozen = GetFrozenFunctions();
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Blocking %d unique function+class combos", (int)frozen.size());
                LogYellow(buf);
            }
        }
        // ─── coopdiag ─── comprehensive co-op diagnostics
        else if (tokens[0] == "coopdiag") {
            LogYellow("═══ CO-OP DIAGNOSTICS ═══");

            // ProcessEvent hook state
            if (IsProcessEventHooked()) {
                const auto& st = GetProcessEventStats();
                char buf[128];
                std::snprintf(buf, sizeof(buf), "ProcessEvent: ACTIVE (calls=%llu, blocked=%llu)",
                             st.TotalCalls, st.BlockedCalls);
                LogGreen(buf);
            } else {
                LogRed("ProcessEvent: NOT HOOKED (co-op will NOT work!)");
            }

            // Tick hook
            LogInfo(std::string("Tick hook: ") + (IsTickHookActive() ? "ACTIVE" : "NOT active"));

            // Role & connection
            LogInfo(GetTrueCoopStatus());
            LogInfo(GetCoopStatus());

            // Puppet diagnostics
            std::string puppetInfo = GetPuppetDiagnostics();
            // Split multiline string into separate log entries
            size_t pos = 0;
            while (pos < puppetInfo.size()) {
                size_t nl = puppetInfo.find('\n', pos);
                if (nl == std::string::npos) nl = puppetInfo.size();
                std::string line = puppetInfo.substr(pos, nl - pos);
                if (!line.empty()) {
                    if (line.find("[BAD]") != std::string::npos)
                        LogRed(line);
                    else if (line.find("===") != std::string::npos)
                        LogYellow(line);
                    else
                        LogInfo(line);
                }
                pos = nl + 1;
            }

            // Simulation freeze state
            if (IsSimulationFrozen()) {
                auto frozen = GetFrozenFunctions();
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Simulation: FROZEN (%d func combos blocked)", (int)frozen.size());
                LogYellow(buf);
            } else {
                LogInfo("Simulation: NOT frozen");
            }

            LogYellow("═══════════════════════════");
        }
        // ─── cooptest [phase] ───
        else if (tokens[0] == "cooptest") {
            EnsureSubsystemsReady();
            std::string phase = (tokens.size() >= 2) ? tokens[1] : "all";
            LogYellow("Running co-op tests: " + phase);
            RunCoopTests(phase);
            auto results = GetCoopTestResults();
            int pass = 0, fail = 0, skip = 0;
            for (auto& r : results) {
                if (r.Result == TestResult::Pass) pass++;
                else if (r.Result == TestResult::Fail) fail++;
                else if (r.Result == TestResult::Skipped) skip++;
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Results: %d PASS, %d FAIL, %d SKIP", pass, fail, skip);
            if (fail > 0) LogRed(buf); else LogGreen(buf);
            LogInfo("Details in debug_dumps/coop_test_results.txt");
        }
        // ─── quicktest ───
        else if (tokens[0] == "quicktest") {
            EnsureSubsystemsReady();
            QuickValidate();
            LogGreen("Quick validation complete -> debug_dumps/coop_test_results.txt");
        }
        // ─── p2spawn ───
        else if (tokens[0] == "p2spawn") {
            EnsureSubsystemsReady();
            auto prevRole = GetTrueCoopRole();
            if (prevRole == TrueCoopRole::None) SetTrueCoopRole(TrueCoopRole::TrueHost);
            if (P2SpawnPawn()) {
                LogGreen("P2 pawn spawned: " + std::string(P2GetPawn() ? P2GetPawn()->GetName() : "unknown"));
            } else {
                LogRed("P2 pawn spawn failed (no candidate found)");
            }
        }
        // ─── p2kill / p2respawn ───
        else if (tokens[0] == "p2kill") {
            P2Kill();
            LogYellow("P2 killed");
        }
        else if (tokens[0] == "p2respawn") {
            P2Respawn();
            LogGreen("P2 respawned");
        }
        // ─── p2status ───
        else if (tokens[0] == "p2status") {
            LogInfo(GetP2Status());
        }
        // ─── worldsync ───
        else if (tokens[0] == "worldsync") {
            EnsureSubsystemsReady();
            auto stats = GetWorldSyncStats();
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "WorldSync: %d tracked, %d dirty, %d pkt/s sent, %d pkt/s recv\n"
                "  Bandwidth: %d B/s out, %d B/s in\n"
                "  Client: %d matched, %d unmatched",
                stats.TrackedActors, stats.DirtyActors,
                stats.PacketsSentThisSec, stats.PacketsRecvThisSec,
                stats.BytesSentThisSec, stats.BytesRecvThisSec,
                stats.ClientMatchedActors, stats.ClientUnmatchedActors);
            LogInfo(buf);
            DumpWorldSyncState();
        }
        // ─── p2inv ───
        else if (tokens[0] == "p2inv") {
            EnsureSubsystemsReady();
            LogInfo(GetP2InventoryStatus());
            DumpP2Inventory();
        }
        // ─── transitions ───
        else if (tokens[0] == "transitions") {
            LogInfo(GetTransitionsStatus());
        }
        // ─── stability ───
        else if (tokens[0] == "stability") {
            DumpStabilityReport();
            LogGreen("Stability report -> debug_dumps/stability_report.txt");
        }
        // ─── dumpall ───
        else if (tokens[0] == "dumpall") {
            DumpFullSnapshot("manual_dumpall");
            DumpActorCensus();
            DumpPropertyOffsets();
            DumpAIFunctions();
            DumpWorldSyncState();
            DumpP2State();
            DumpP2Inventory();
            DumpStabilityReport();
            DumpCoopTestResults();
            LogGreen("All debug data dumped to debug_dumps/");
        }
        // ─── dumpsdk ───
        else if (tokens[0] == "dumpsdk") {
            LogYellow("Generating FULL SDK (classes, enums, structs, defaults, states, natives)...");
            SDKGenerator gen;
            int count = gen.Generate("Z:\\Bioshock1SDK\\sdk_gen");
            DumpClassHierarchy();
            DumpAllFunctions();
            char buf2[128];
            std::snprintf(buf2, sizeof(buf2), "Full SDK generated: %d classes -> Z:\\Bioshock1SDK\\sdk_gen\\", count);
            LogGreen(buf2);
            LogInfo("  + Per-package .h headers (C++ style)");
            LogInfo("  + SDK_Enums.h (all enum values)");
            LogInfo("  + SDK_Structs.h (all struct layouts)");
            LogInfo("  + SDK_Defaults.txt (CDO property values)");
            LogInfo("  + SDK_States.txt (AI/game states)");
            LogInfo("  + SDK_NativeFunctions.txt (native C++ function pointers)");
            LogInfo("  + SDK_STATS.txt (summary)");
            LogGreen("  + debug_dumps/class_hierarchy.txt");
            LogGreen("  + debug_dumps/all_functions.txt");
        }
        // ─── inspect <class_or_addr> ───
        else if (tokens[0] == "inspect" && tokens.size() >= 2) {
            std::string target = tokens[1];
            std::string result = InspectObject(target);
            // Split into lines for console display
            std::istringstream stream(result);
            std::string line;
            int lineCount = 0;
            while (std::getline(stream, line) && lineCount < 100) {
                if (line.find("===") != std::string::npos) {
                    LogGreen(line);
                } else {
                    LogInfo(line);
                }
                lineCount++;
            }
            if (lineCount >= 100) LogYellow("(output truncated at 100 lines)");
        }
        // ─── hierarchy ───
        else if (tokens[0] == "hierarchy") {
            DumpClassHierarchy();
            LogGreen("Class hierarchy -> debug_dumps/class_hierarchy.txt");
        }
        // ─── functions [filter] ───
        else if (tokens[0] == "functions") {
            DumpAllFunctions();
            LogGreen("All functions -> debug_dumps/all_functions.txt");
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
