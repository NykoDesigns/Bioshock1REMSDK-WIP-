#pragma once

#include "../engine/uobject.h"
#include <functional>
#include <string>
#include <vector>
#include <mutex>

namespace bs1sdk {

/// ProcessEvent hook - intercepts all UFunction calls in the engine.
/// In UE2.5, UObject::ProcessEvent(UFunction*, void* Parms, void* Result)
/// is a virtual function. We hook it by patching the vtable.

// UFunction is now fully defined in uobject.h with confirmed offsets

// Signature: void __thiscall UObject::ProcessEvent(UFunction* Function, void* Parms, void* Result)
// On x86 MSVC __thiscall: 'this' in ECX, args on stack
typedef void (__thiscall* ProcessEventFn)(UObject* thisObj, UFunction* function, void* parms, void* result);

/// Callback signature for ProcessEvent listeners.
/// Return true to block the original call, false to let it through.
struct ProcessEventHook {
    std::string Name;           // Human-readable hook name
    std::string FunctionFilter; // If non-empty, only fires for this function name ("" = all)
    std::function<bool(UObject* obj, UFunction* func, void* parms)> Callback;
};

/// Initialize the ProcessEvent hook by finding and patching the vtable.
/// Must be called after GObjects is valid and at least one UObject exists.
bool InitProcessEventHook();

/// Shutdown and restore original vtable entry.
void ShutdownProcessEventHook();

/// Register a hook callback. Returns an ID for removal.
int RegisterProcessEventHook(const ProcessEventHook& hook);

/// Remove a hook by ID.
void UnregisterProcessEventHook(int id);

/// Get the original ProcessEvent function pointer (for calling through).
ProcessEventFn GetOriginalProcessEvent();

/// Check if the hook is active.
bool IsProcessEventHooked();

/// Get stats about ProcessEvent calls.
struct ProcessEventStats {
    uint64_t TotalCalls = 0;
    uint64_t BlockedCalls = 0;
    uint64_t LastCallTime = 0;
    std::string LastFunctionName;
    std::string LastObjectName;
};
const ProcessEventStats& GetProcessEventStats();

} // namespace bs1sdk
