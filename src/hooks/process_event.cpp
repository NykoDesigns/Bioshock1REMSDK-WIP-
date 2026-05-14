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

    // Strategy 1: scan vtable entries for code containing the recursion check
    // ProcessEvent checks `if(++Recurse > 250)` which generates cmp with 0xFA
    for (int idx = 50; idx < 120; idx++) {
        uintptr_t funcAddr = vtableEntries[idx];
        if (funcAddr < modBase || funcAddr >= modEnd) continue;

        const uint8_t* code = reinterpret_cast<const uint8_t*>(funcAddr);
        bool found = false;

        for (int b = 0; b < 2048; b++) {
            // cmp eax, 0xFA (3D FA000000)
            if (code[b] == 0x3D && *reinterpret_cast<const uint32_t*>(code + b + 1) == 250) {
                found = true; break;
            }
            // cmp [mem], 0xFA (81 3D/7D ... FA000000)
            if (code[b] == 0x81 && b + 6 < 2048) {
                uint32_t imm = *reinterpret_cast<const uint32_t*>(code + b + 2);
                if (imm == 250) { found = true; break; }
            }
        }

        if (found) {
            LOG_INFO("ProcessEvent: Found via heuristic at vtable[{}] = 0x{:08X}", idx, funcAddr);
            return funcAddr;
        }
    }

    // Strategy 2: fallback - try common UE2 indices, pick largest function
    LOG_WARN("ProcessEvent: Heuristic scan failed, trying fallback indices...");

    for (int idx : {67, 68, 69, 70, 66, 65, 71, 72, 73, 64, 63}) {
        uintptr_t funcAddr = vtableEntries[idx];
        if (funcAddr >= modBase && funcAddr < modEnd) {
            uintptr_t nextFunc = vtableEntries[idx + 1];
            if (nextFunc >= modBase && nextFunc < modEnd && nextFunc != funcAddr) {
                size_t funcSize = nextFunc - funcAddr;
                if (funcSize > 256 && funcSize < 65536) {
                    LOG_INFO("ProcessEvent: Using fallback index {} (0x{:08X}, ~{}B)",
                             idx, funcAddr, funcSize);
                    return funcAddr;
                }
            }
        }
    }

    LOG_ERROR("ProcessEvent: Could not discover address");
    return 0;
}

// ─── Public API ─────────────────────────────────────────────────────────

bool InitProcessEventHook()
{
    if (s_Hooked) return true;

    s_ProcessEventAddr = DiscoverProcessEventAddress();
    if (!s_ProcessEventAddr) {
        LOG_ERROR("ProcessEvent hook: Failed to discover function address");
        return false;
    }

    // Use MinHook for inline detour - this hooks ALL calls to the function
    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(s_ProcessEventAddr),
        reinterpret_cast<void*>(&HookedProcessEvent),
        reinterpret_cast<void**>(&s_OriginalProcessEvent)
    );

    if (status != MH_OK) {
        LOG_ERROR("ProcessEvent: MH_CreateHook failed: {}", static_cast<int>(status));
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(s_ProcessEventAddr));
    if (status != MH_OK) {
        LOG_ERROR("ProcessEvent: MH_EnableHook failed: {}", static_cast<int>(status));
        MH_RemoveHook(reinterpret_cast<void*>(s_ProcessEventAddr));
        return false;
    }

    s_Hooked = true;
    LOG_INFO("ProcessEvent hooked via MinHook detour at 0x{:08X}", s_ProcessEventAddr);
    return true;
}

void ShutdownProcessEventHook()
{
    if (!s_Hooked) return;

    MH_DisableHook(reinterpret_cast<void*>(s_ProcessEventAddr));
    MH_RemoveHook(reinterpret_cast<void*>(s_ProcessEventAddr));

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
