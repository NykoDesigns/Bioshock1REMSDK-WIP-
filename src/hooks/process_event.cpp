#include "process_event.h"
#include "../core/log.h"
#include "../debug/crash_handler.h"

#include <Windows.h>
#include <Psapi.h>
#include <MinHook.h>
#include <vector>
#include <mutex>
#include <chrono>
#include <algorithm>

namespace bs1sdk {

// ─── State ──────────────────────────────────────────────────────────────

static ProcessEventFn s_OriginalProcessEvent = nullptr;
static bool s_Hooked = false;
static std::vector<std::pair<int, ProcessEventHook>> s_Hooks;
static std::mutex s_HookMutex;
static int s_NextHookId = 1;
static ProcessEventStats s_Stats;
static uintptr_t s_ProcessEventAddr = 0;

// Vtable hook fallback state
static bool s_VtableHooked = false;
static uintptr_t* s_VtableLocation = nullptr;  // pointer to the vtable entry we patched
static int s_VtableIndex = -1;

// ─── SEH helper: safely resolve a single name (no C++ objects) ──────────

static int SafeGetOneName(UObject* obj, char* outBuf, int outBufLen)
{
    __try {
        // Read the FName index from UObject (offset 0x18 for NameIndex)
        // Then index into GNames to get the string
        // GetName() returns std::string which we can't use in __try
        // So we probe the object memory first, then let the caller use GetName()
        volatile uint32_t nameIdx = *(uint32_t*)((uint8_t*)obj + 0x18);
        (void)nameIdx;
        outBuf[0] = '?';
        outBuf[1] = '\0';
        return 0; // Pointer is safe to dereference
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashBreadcrumb("PE: name probe crashed (SEH caught)");
        outBuf[0] = '\0';
        return 1;
    }
}

// ─── SEH helper: safely call a hook callback ────────────────────────────

static int SafeCallHook(const std::function<bool(UObject*, UFunction*, void*)>& cb,
                         UObject* obj, UFunction* func, void* parms)
{
    __try {
        return cb(obj, func, parms) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashBreadcrumb("PE: Hook callback crashed (SEH caught)");
        return 0;
    }
}

// ─── Our replacement ProcessEvent ───────────────────────────────────────
// x86 __thiscall: 'this' in ECX. We use __fastcall to capture ECX + EDX.

static void __fastcall HookedProcessEvent(UObject* thisObj, void* /*edx*/,
                                           UFunction* function, void* parms, void* result)
{
    s_Stats.TotalCalls++;

    // Safety: validate pointers before any dereference
    if (!thisObj || !function) {
        if (s_OriginalProcessEvent) {
            s_OriginalProcessEvent(thisObj, function, parms, result);
        }
        return;
    }

    // Validate pointers are actually readable memory
    if (!IsSafeToRead(thisObj, 8) || !IsSafeToRead(function, 8)) {
        if (s_OriginalProcessEvent) {
            s_OriginalProcessEvent(thisObj, function, parms, result);
        }
        return;
    }

    // Only resolve names if we have hooks or every Nth call for stats display
    bool needNames = !s_Hooks.empty() || (s_Stats.TotalCalls % 64 == 0);
    std::string funcName;

    if (needNames) {
        // Probe that name memory is readable before calling GetName()
        char probeBuf[4];
        if (SafeGetOneName(reinterpret_cast<UObject*>(function), probeBuf, 4) != 0 ||
            SafeGetOneName(thisObj, probeBuf, 4) != 0) {
            // Memory not readable — skip hooks, call original
            if (s_OriginalProcessEvent) {
                s_OriginalProcessEvent(thisObj, function, parms, result);
            }
            return;
        }
        // Memory is safe, call GetName() normally
        funcName = function->GetName();
        s_Stats.LastFunctionName = funcName;
        s_Stats.LastObjectName = thisObj->GetName();
    }

    bool blocked = false;

    if (!s_Hooks.empty() && !funcName.empty()) {
        std::lock_guard<std::mutex> lock(s_HookMutex);
        for (auto& [id, hook] : s_Hooks) {
            if (!hook.FunctionFilter.empty() && hook.FunctionFilter != funcName)
                continue;

            if (hook.Callback) {
                if (SafeCallHook(hook.Callback, thisObj, function, parms)) {
                    blocked = true;
                    s_Stats.BlockedCalls++;
                    break;
                }
            }
        }
    }

    // Call original unless blocked
    if (!blocked && s_OriginalProcessEvent) {
        s_OriginalProcessEvent(thisObj, function, parms, result);
    }
}

// ─── ProcessEvent Address Discovery ─────────────────────────────────────
// We find the actual function address via vtable, then use MinHook to
// create an inline detour. This hooks ALL calls to the function regardless
// of which vtable is used.

static uintptr_t DiscoverProcessEventAddress()
{
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) return 0;

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Get a valid UObject to read its vtable
    UObject* testObj = nullptr;
    for (int i = 0; i < objCount && i < 100; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        testObj = reinterpret_cast<UObject*>(ptr);
        break;
    }
    if (!testObj) return 0;

    uintptr_t vtable = *reinterpret_cast<uintptr_t*>(testObj);
    uintptr_t* vtableEntries = reinterpret_cast<uintptr_t*>(vtable);

    // Get module bounds for validation
    HMODULE gameModule = GetModuleHandleA(nullptr);
    MODULEINFO modInfo = {};
    GetModuleInformation(GetCurrentProcess(), gameModule, &modInfo, sizeof(modInfo));
    uintptr_t modBase = reinterpret_cast<uintptr_t>(gameModule);
    uintptr_t modEnd = modBase + modInfo.SizeOfImage;

    // Log vtable info for remote debugging
    LOG_INFO("ProcessEvent: vtable=0x{:08X}, module=0x{:08X}-0x{:08X}", vtable, modBase, modEnd);

    // Strategy 1: scan vtable entries for code containing the recursion check
    // ProcessEvent checks `if(++Recurse > 250)` which generates cmp with 0xFA
    for (int idx = 50; idx < 120; idx++) {
        uintptr_t funcAddr = vtableEntries[idx];
        if (funcAddr < modBase || funcAddr >= modEnd) continue;

        __try {
            const uint8_t* code = reinterpret_cast<const uint8_t*>(funcAddr);
            bool found = false;

            for (int b = 0; b < 2048; b++) {
                // cmp eax, 0xFA (3D FA000000)
                if (code[b] == 0x3D && *reinterpret_cast<const uint32_t*>(code + b + 1) == 250) {
                    found = true; break;
                }
                // cmp [reg], 0xFA various encodings
                if (code[b] == 0x81 && b + 6 < 2048) {
                    uint32_t imm = *reinterpret_cast<const uint32_t*>(code + b + 2);
                    if (imm == 250) { found = true; break; }
                }
                // cmp byte 0xFA
                if (code[b] == 0x83 && b + 3 < 2048) {
                    if (code[b + 2] == 0xFA) { found = true; break; }
                }
            }

            if (found) {
                LOG_INFO("ProcessEvent: Found via heuristic at vtable[{}] = 0x{:08X}", idx, funcAddr);
                return funcAddr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue; // Skip unreadable vtable entries
        }
    }

    // Strategy 2: scan for the largest function in the typical PE index range
    // ProcessEvent is usually the biggest function in the 60-75 range
    LOG_WARN("ProcessEvent: Heuristic scan failed, trying size-based fallback...");

    uintptr_t bestAddr = 0;
    int bestIdx = -1;
    size_t bestSize = 0;

    for (int idx = 55; idx < 85; idx++) {
        __try {
            uintptr_t funcAddr = vtableEntries[idx];
            if (funcAddr < modBase || funcAddr >= modEnd) continue;

            // Estimate function size by scanning for the next function prologue
            // (push ebp / mov ebp, esp = 55 8B EC) or next vtable entry
            const uint8_t* code = reinterpret_cast<const uint8_t*>(funcAddr);
            size_t estSize = 0;

            for (size_t b = 16; b < 65536; b++) {
                // Look for typical function prologue
                if (code[b] == 0x55 && code[b+1] == 0x8B && code[b+2] == 0xEC) {
                    estSize = b;
                    break;
                }
                // Or int3 padding (CC CC)
                if (code[b] == 0xCC && code[b+1] == 0xCC) {
                    estSize = b;
                    break;
                }
            }

            if (estSize > 1000 && estSize > bestSize) {
                // ProcessEvent is always a large function (>1KB)
                bestSize = estSize;
                bestAddr = funcAddr;
                bestIdx = idx;
            }

            LOG_INFO("ProcessEvent: vtable[{}] = 0x{:08X}, ~{}B", idx, funcAddr, estSize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }

    if (bestAddr && bestSize > 1000) {
        LOG_INFO("ProcessEvent: Using size-based fallback vtable[{}] = 0x{:08X} (~{}B)",
                 bestIdx, bestAddr, bestSize);
        return bestAddr;
    }

    LOG_ERROR("ProcessEvent: Could not discover address (all strategies failed)");
    return 0;
}

// ─── Public API ─────────────────────────────────────────────────────────

// Helper: find the vtable entry for ProcessEvent on a test object
static bool FindVtableEntry(uintptr_t peAddr, uintptr_t*& outVtableSlot, int& outIndex)
{
    auto& globals = GetEngineGlobals();
    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    // Check multiple objects to find one whose vtable contains our PE address
    for (int i = 0; i < objCount && i < 200; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        if (!IsSafeToRead(obj, 4)) continue;

        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(obj);
        if (!IsSafeToRead(reinterpret_cast<void*>(vtable), 400)) continue;

        uintptr_t* entries = reinterpret_cast<uintptr_t*>(vtable);
        for (int idx = 50; idx < 90; idx++) {
            if (entries[idx] == peAddr) {
                outVtableSlot = &entries[idx];
                outIndex = idx;
                return true;
            }
        }
    }
    return false;
}

bool InitProcessEventHook()
{
    if (s_Hooked) return true;

    s_ProcessEventAddr = DiscoverProcessEventAddress();
    if (!s_ProcessEventAddr) {
        LOG_ERROR("ProcessEvent hook: Failed to discover function address");
        return false;
    }

    // Strategy 1: Use MinHook for inline detour
    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(s_ProcessEventAddr),
        reinterpret_cast<void*>(&HookedProcessEvent),
        reinterpret_cast<void**>(&s_OriginalProcessEvent)
    );

    if (status == MH_OK) {
        status = MH_EnableHook(reinterpret_cast<void*>(s_ProcessEventAddr));
        if (status == MH_OK) {
            s_Hooked = true;
            LOG_INFO("ProcessEvent hooked via MinHook detour at 0x{:08X}", s_ProcessEventAddr);
            return true;
        }
        LOG_ERROR("ProcessEvent: MH_EnableHook failed: {}", static_cast<int>(status));
        MH_RemoveHook(reinterpret_cast<void*>(s_ProcessEventAddr));
    } else {
        LOG_WARN("ProcessEvent: MH_CreateHook failed: {} — trying vtable hook fallback",
                 static_cast<int>(status));
    }

    // Strategy 2: vtable hook fallback
    // Find the vtable slot pointing to ProcessEvent and swap it
    uintptr_t* vtableSlot = nullptr;
    int vtableIdx = -1;
    if (!FindVtableEntry(s_ProcessEventAddr, vtableSlot, vtableIdx)) {
        LOG_ERROR("ProcessEvent: vtable hook fallback failed — could not find vtable entry");
        return false;
    }

    // Store original function pointer
    s_OriginalProcessEvent = reinterpret_cast<ProcessEventFn>(*vtableSlot);

    // Make vtable writable and patch it
    DWORD oldProtect;
    if (!VirtualProtect(vtableSlot, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
        LOG_ERROR("ProcessEvent: VirtualProtect failed for vtable patch");
        return false;
    }

    *vtableSlot = reinterpret_cast<uintptr_t>(&HookedProcessEvent);
    VirtualProtect(vtableSlot, sizeof(uintptr_t), oldProtect, &oldProtect);

    s_VtableHooked = true;
    s_VtableLocation = vtableSlot;
    s_VtableIndex = vtableIdx;
    s_Hooked = true;

    LOG_INFO("ProcessEvent hooked via VTABLE patch at index {} (0x{:08X})",
             vtableIdx, s_ProcessEventAddr);
    return true;
}

void ShutdownProcessEventHook()
{
    if (!s_Hooked) return;

    if (s_VtableHooked && s_VtableLocation && s_OriginalProcessEvent) {
        // Restore original vtable entry
        DWORD oldProtect;
        if (VirtualProtect(s_VtableLocation, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
            *s_VtableLocation = reinterpret_cast<uintptr_t>(s_OriginalProcessEvent);
            VirtualProtect(s_VtableLocation, sizeof(uintptr_t), oldProtect, &oldProtect);
        }
        s_VtableHooked = false;
        s_VtableLocation = nullptr;
    } else {
        MH_DisableHook(reinterpret_cast<void*>(s_ProcessEventAddr));
        MH_RemoveHook(reinterpret_cast<void*>(s_ProcessEventAddr));
    }

    s_Hooked = false;
    s_OriginalProcessEvent = nullptr;
    s_ProcessEventAddr = 0;

    std::lock_guard<std::mutex> lock(s_HookMutex);
    s_Hooks.clear();

    LOG_INFO("ProcessEvent hook removed");
}

int RegisterProcessEventHook(const ProcessEventHook& hook)
{
    std::lock_guard<std::mutex> lock(s_HookMutex);
    int id = s_NextHookId++;
    s_Hooks.push_back({id, hook});
    LOG_INFO("ProcessEvent hook registered: '{}' (filter: '{}')", hook.Name,
             hook.FunctionFilter.empty() ? "*" : hook.FunctionFilter);
    return id;
}

void UnregisterProcessEventHook(int id)
{
    std::lock_guard<std::mutex> lock(s_HookMutex);
    s_Hooks.erase(
        std::remove_if(s_Hooks.begin(), s_Hooks.end(),
                       [id](const auto& p) { return p.first == id; }),
        s_Hooks.end());
}

ProcessEventFn GetOriginalProcessEvent()
{
    return s_OriginalProcessEvent;
}

bool IsProcessEventHooked()
{
    return s_Hooked;
}

const ProcessEventStats& GetProcessEventStats()
{
    return s_Stats;
}

} // namespace bs1sdk
