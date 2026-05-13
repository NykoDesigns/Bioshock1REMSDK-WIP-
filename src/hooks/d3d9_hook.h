#pragma once

#include <d3d9.h>
#include <cstdint>

namespace bs1sdk {

/// D3D9 hook implementation.
/// Hooks IDirect3DDevice9::Present and EndScene to render our overlay.
/// Uses a dummy device creation approach to get vtable addresses.

/// Install the D3D9 hooks (called from Hooks::InstallD3D9Hook)
bool InstallD3D9HookImpl();

/// Get the hooked D3D9 device (available after first Present call)
IDirect3DDevice9* GetD3D9Device();

/// Check if D3D9 hooks are active
bool IsD3D9Hooked();

} // namespace bs1sdk
