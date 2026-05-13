#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace bs1sdk {

/// ImGui-based debug overlay rendered via DXGI Present hook.
class Overlay {
public:
    /// Initialize ImGui with DXGI swap chain (D3D11 path)
    static void InitializeDXGI(IDXGISwapChain* swapChain, ID3D11Device* device, HWND hwnd);

    /// Render one frame of the overlay
    static void Render();

    /// Cleanup before device reset / resize
    static void OnDeviceLost();

    /// Reinitialize after device reset / resize
    static void OnDeviceReset();

    /// Cleanup and shutdown
    static void Shutdown();

    /// Toggle overlay visibility
    static void Toggle();

    /// Check if overlay is visible
    static bool IsVisible();

private:
    static void RenderMainMenu();
    static void RenderObjectInspector();
    static void RenderPlayerProperties();
    static void RenderConsole();
    static void RenderModMenu();
    static void RenderMemoryViewer();

    static bool s_Initialized;
    static bool s_Visible;
    static ID3D11Device* s_Device;
    static ID3D11DeviceContext* s_Context;
    static ID3D11RenderTargetView* s_RenderTarget;
    static IDXGISwapChain* s_SwapChain;
};

} // namespace bs1sdk
