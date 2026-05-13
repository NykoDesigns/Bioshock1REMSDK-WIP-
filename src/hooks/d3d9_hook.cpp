#include "d3d9_hook.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../render/overlay.h"

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace bs1sdk {

// ─── DXGI/D3D11 hooking (primary for BioShock Remastered) ────────────────
// The remastered version uses D3D11 internally even though the engine code
// still references D3D9. We hook IDXGISwapChain::Present to render overlay.

constexpr int DXGI_PRESENT_VTABLE_IDX = 8;
constexpr int DXGI_RESIZE_VTABLE_IDX = 13;

using DXGIPresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using DXGIResizeFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static DXGIPresentFn g_OriginalPresent = nullptr;
static DXGIResizeFn g_OriginalResize = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static bool g_Hooked = false;
static bool g_OverlayInitialized = false;

// ─── DXGI Present Hook ───────────────────────────────────────────────────

HRESULT WINAPI HookedDXGIPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    if (!g_OverlayInitialized) {
        g_SwapChain = swapChain;
        
        // Get the D3D11 device from the swap chain so we can get the HWND
        ID3D11Device* device = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
        
        if (device) {
            // Get HWND from swap chain description
            DXGI_SWAP_CHAIN_DESC desc{};
            swapChain->GetDesc(&desc);
            
            LOG_INFO("DXGI Present hooked! HWND=0x{:X}, {}x{}", 
                     reinterpret_cast<uintptr_t>(desc.OutputWindow),
                     desc.BufferDesc.Width, desc.BufferDesc.Height);
            
            // For ImGui, we need the D3D9 device OR switch overlay to D3D11.
            // For now, just render a proof-of-life via D3D11.
            Overlay::InitializeDXGI(swapChain, device, desc.OutputWindow);
            device->Release();
        }
        
        g_OverlayInitialized = true;
    }

    Overlay::Render();

    return g_OriginalPresent(swapChain, syncInterval, flags);
}

HRESULT WINAPI HookedDXGIResize(IDXGISwapChain* swapChain, UINT bufferCount, 
                                  UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    LOG_INFO("ResizeBuffers: {}x{}", width, height);
    Overlay::OnDeviceLost();
    HRESULT hr = g_OriginalResize(swapChain, bufferCount, width, height, format, flags);
    if (SUCCEEDED(hr)) {
        Overlay::OnDeviceReset();
    }
    return hr;
}

// ─── DXGI VTable Extraction ─────────────────────────────────────────────

static bool GetDXGIVTable(uintptr_t* outVtable, size_t count)
{
    // Create a temporary D3D11 device + swap chain to get DXGI vtable
    HWND tempWnd = CreateWindowExA(0, "STATIC", "BS1SDK_dxgi", WS_OVERLAPPEDWINDOW,
                                    0, 0, 100, 100, nullptr, nullptr,
                                    GetModuleHandleA(nullptr), nullptr);
    if (!tempWnd) {
        LOG_ERROR("Failed to create temp window for DXGI");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 2;
    sd.BufferDesc.Height = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = tempWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &swapChain, &device, &featureLevel, &context
    );

    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDeviceAndSwapChain failed: 0x{:08X}", static_cast<unsigned>(hr));
        DestroyWindow(tempWnd);
        return false;
    }

    // Copy swap chain vtable
    uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(swapChain);
    memcpy(outVtable, vtable, count * sizeof(uintptr_t));

    LOG_INFO("DXGI vtable extracted. Present=0x{:08X}, ResizeBuffers=0x{:08X}",
             outVtable[DXGI_PRESENT_VTABLE_IDX], outVtable[DXGI_RESIZE_VTABLE_IDX]);

    swapChain->Release();
    context->Release();
    device->Release();
    DestroyWindow(tempWnd);

    return true;
}

// ─── Public Interface ──────────────────────────────────────────────────────

bool InstallD3D9HookImpl()
{
    // BioShock Remastered uses D3D11 internally — hook DXGI swap chain
    uintptr_t vtable[20] = {};
    if (!GetDXGIVTable(vtable, 20)) {
        LOG_ERROR("Could not extract DXGI vtable");
        return false;
    }

    // Hook IDXGISwapChain::Present
    if (!Hooks::CreateHook(
            reinterpret_cast<void*>(vtable[DXGI_PRESENT_VTABLE_IDX]),
            reinterpret_cast<void*>(&HookedDXGIPresent),
            reinterpret_cast<void**>(&g_OriginalPresent))) {
        LOG_ERROR("Failed to hook DXGI Present");
        return false;
    }

    // Hook IDXGISwapChain::ResizeBuffers
    if (!Hooks::CreateHook(
            reinterpret_cast<void*>(vtable[DXGI_RESIZE_VTABLE_IDX]),
            reinterpret_cast<void*>(&HookedDXGIResize),
            reinterpret_cast<void**>(&g_OriginalResize))) {
        LOG_WARN("Failed to hook DXGI ResizeBuffers (non-fatal)");
    }

    g_Hooked = true;
    LOG_INFO("DXGI hooks installed successfully — waiting for Present call...");
    return true;
}

IDirect3DDevice9* GetD3D9Device()
{
    return nullptr; // No D3D9 device — game uses D3D11
}

bool IsD3D9Hooked()
{
    return g_Hooked;
}

} // namespace bs1sdk
