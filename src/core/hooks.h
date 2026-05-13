#pragma once

#include <cstdint>
#include <functional>

namespace bs1sdk {

/// Central hook management. Wraps MinHook for inline hooks and provides
/// additional vtable hooking utilities.
class Hooks {
public:
    /// Initialize the hooking framework (calls MH_Initialize)
    static bool Initialize();
    
    /// Shutdown and remove all hooks (calls MH_Uninitialize)
    static void Shutdown();

    /// Create an inline hook at target, redirecting to detour.
    /// Stores the original function pointer in *original.
    static bool CreateHook(void* target, void* detour, void** original);

    /// Enable a previously created hook
    static bool EnableHook(void* target);

    /// Disable a hook without removing it
    static bool DisableHook(void* target);

    /// Remove a hook entirely
    static bool RemoveHook(void* target);

    /// Enable all created hooks
    static bool EnableAll();

    /// Disable all hooks
    static bool DisableAll();

    // ─── High-level game hooks ───

    /// Install D3D9 Present/EndScene hook for overlay rendering
    static bool InstallD3D9Hook();

    /// Install game tick hook (called every frame by the engine)
    static bool InstallTickHook();

    /// Install ProcessEvent hook (intercepts all UnrealScript events)
    static bool InstallProcessEventHook();

    // ─── Callbacks ───
    
    using TickCallback = std::function<void(float deltaTime)>;
    using RenderCallback = std::function<void()>;

    /// Register a function to be called every game tick
    static void RegisterTickCallback(TickCallback callback);

    /// Register a function to be called every render frame (for ImGui, etc.)
    static void RegisterRenderCallback(RenderCallback callback);
};

} // namespace bs1sdk
