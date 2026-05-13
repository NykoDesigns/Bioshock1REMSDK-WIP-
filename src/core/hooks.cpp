#include "hooks.h"
#include "log.h"
#include "../hooks/d3d9_hook.h"

#include <MinHook.h>
#include <vector>
#include <mutex>

namespace bs1sdk {

static std::vector<Hooks::TickCallback> g_TickCallbacks;
static std::vector<Hooks::RenderCallback> g_RenderCallbacks;
static std::mutex g_CallbackMutex;

bool Hooks::Initialize()
{
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        LOG_ERROR("MH_Initialize failed: {}", MH_StatusToString(status));
        return false;
    }
    return true;
}

void Hooks::Shutdown()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    
    std::lock_guard lock(g_CallbackMutex);
    g_TickCallbacks.clear();
    g_RenderCallbacks.clear();
}

bool Hooks::CreateHook(void* target, void* detour, void** original)
{
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        LOG_ERROR("MH_CreateHook failed at 0x{:08X}: {}", 
                  reinterpret_cast<uintptr_t>(target), MH_StatusToString(status));
        return false;
    }
    
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        LOG_ERROR("MH_EnableHook failed at 0x{:08X}: {}", 
                  reinterpret_cast<uintptr_t>(target), MH_StatusToString(status));
        return false;
    }
    
    return true;
}

bool Hooks::EnableHook(void* target)
{
    return MH_EnableHook(target) == MH_OK;
}

bool Hooks::DisableHook(void* target)
{
    return MH_DisableHook(target) == MH_OK;
}

bool Hooks::RemoveHook(void* target)
{
    MH_DisableHook(target);
    return MH_RemoveHook(target) == MH_OK;
}

bool Hooks::EnableAll()
{
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

bool Hooks::DisableAll()
{
    return MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
}

void Hooks::RegisterTickCallback(TickCallback callback)
{
    std::lock_guard lock(g_CallbackMutex);
    g_TickCallbacks.push_back(std::move(callback));
}

void Hooks::RegisterRenderCallback(RenderCallback callback)
{
    std::lock_guard lock(g_CallbackMutex);
    g_RenderCallbacks.push_back(std::move(callback));
}

// ─── D3D9 Hook Implementation ─────────────────────────────────────────────

// D3D9 hooks are implemented in hooks/d3d9_hook.cpp
// This stub exists to dispatch from the central hook manager
bool Hooks::InstallD3D9Hook()
{
    return bs1sdk::InstallD3D9HookImpl();
}

bool Hooks::InstallTickHook()
{
    // TODO: Implement once tick function address is discovered
    LOG_WARN("Tick hook not yet implemented - requires RE of game loop");
    return false;
}

bool Hooks::InstallProcessEventHook()
{
    // TODO: Implement once ProcessEvent address is discovered
    LOG_WARN("ProcessEvent hook not yet implemented - requires RE of UObject vtable");
    return false;
}

} // namespace bs1sdk
