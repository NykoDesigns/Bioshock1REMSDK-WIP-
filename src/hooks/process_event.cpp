#include "process_event.h"
#include "../core/log.h"
#include "../debug/crash_handler.h"

#include <Windows.h>
#include <Psapi.h>
#include <MinHook.h>
#include <vector>
#include <mutex>
#include <atomic>
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
//
// PERFORMANCE CRITICAL: This runs for every UFunction call in the engine
// (~3000-5000 per frame). Must be as lean as possible.
//   - No std::string allocations
//   - No mutex on the hot path
//   - No SEH / IsSafeToRead probes (engine pointers are valid)
//   - Zero-allocation FName lookup via raw wchar_t*
//   - Cached snapshot of hooks (copy-on-write) to avoid lock

// Lock-free hook snapshot: updated when hooks change, read without locking
static std::vector<std::pair<int, ProcessEventHook>> s_HookSnapshot;
static std::atomic<bool> s_HooksDirty{false};

static void RefreshHookSnapshot()
{
    std::lock_guard<std::mutex> lock(s_HookMutex);
    s_HookSnapshot = s_Hooks;
    s_HooksDirty = false;
}

static void __fastcall HookedProcessEvent(UObject* thisObj, void* /*edx*/,
                                           UFunction* function, void* parms, void* result)
{
    s_Stats.TotalCalls++;

    // Null check only — no IsSafeToRead (engine pointers are always valid)
    if (!thisObj || !function) {
        if (s_OriginalProcessEvent)
            s_OriginalProcessEvent(thisObj, function, parms, result);
        return;
    }

    // Fast path: no hooks → just call original
    if (s_HookSnapshot.empty() && !s_HooksDirty) {
        s_OriginalProcessEvent(thisObj, function, parms, result);
        return;
    }

    // Refresh snapshot if hooks changed (rare — only during init)
    if (s_HooksDirty)
        RefreshHookSnapshot();

    // Read function FName index directly (zero allocation — just a uint32 read)
    // UFunction inherits UObject: FName at +0x28, Index is first int32
    int32_t funcNameIdx = *reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(function) + UObject::OFFSET_NAME);

    // Look up raw wide string from GNames (no allocation)
    const wchar_t* funcNameW = GetFNameRaw(funcNameIdx);
    if (!funcNameW) {
        s_OriginalProcessEvent(thisObj, function, parms, result);
        return;
    }

    // Stats name tracking removed — GetName() does heap allocations which
    // add up at 5000 PE calls/frame. TotalCalls counter still increments above.

    // Dispatch to hooks — NO mutex lock (we use the snapshot)
    bool blocked = false;
    for (auto& [id, hook] : s_HookSnapshot) {
        // Fast filter check using raw wide string compare (no std::string)
        if (!hook.FunctionFilter.empty()) {
            if (!WideMatchesAscii(funcNameW, hook.FunctionFilter.c_str()))
                continue;
        }

        if (hook.Callback) {
            if (hook.Callback(thisObj, function, parms)) {
                blocked = true;
                s_Stats.BlockedCalls++;
                break;
            }
        }
    }

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

            for (int b = 0; b < 8192; b++) {
                // cmp eax, 0xFA (3D FA000000) — most common encoding
                if (code[b] == 0x3D && *reinterpret_cast<const uint32_t*>(code + b + 1) == 250) {
                    found = true; break;
                }
                // 81 XX imm32: cmp reg/mem, 250 — check at multiple offsets
                // ModRM variants: +2 (reg), +3 (reg+disp8), +6 (reg+disp32)
                if (code[b] == 0x81 && b + 7 < 8192) {
                    if (*reinterpret_cast<const uint32_t*>(code + b + 2) == 250) { found = true; break; }
                    if (*reinterpret_cast<const uint32_t*>(code + b + 3) == 250) { found = true; break; }
                    if (*reinterpret_cast<const uint32_t*>(code + b + 6) == 250) { found = true; break; }
                }
            }

            if (found) {
                // Validate function starts with a valid prologue (avoids false positives in data)
                bool validStart = false;
                if (code[0] == 0x55 && code[1] == 0x8B && code[2] == 0xEC) validStart = true;
                if (code[0] == 0x53 || code[0] == 0x56 || code[0] == 0x57) validStart = true;
                if (code[0] == 0x83 && code[1] == 0xEC) validStart = true;
                if (code[0] == 0x81 && code[1] == 0xEC) validStart = true;
                if (code[0] == 0x6A) validStart = true;
                if (code[0] == 0x8B) validStart = true;
                if (code[0] == 0xE9) validStart = true;
                if (!validStart) {
                    LOG_INFO("ProcessEvent: vtable[{}] has recursion pattern but bad prologue ({:02X} {:02X} {:02X}), skipping",
                             idx, code[0], code[1], code[2]);
                    continue;
                }
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

    for (int idx = 60; idx < 73; idx++) {
        __try {
            uintptr_t funcAddr = vtableEntries[idx];
            if (funcAddr < modBase || funcAddr >= modEnd) continue;

            const uint8_t* code = reinterpret_cast<const uint8_t*>(funcAddr);

            // Validate this looks like actual code, not data/zeros.
            // NOTE: Do NOT use VirtualQuery here — Steam DRM can report code
            // pages as non-executable during early init. Prologue check is
            // sufficient: data regions (zeros, pointers) never start with
            // valid x86 function prologues.
            bool validPrologue = false;
            if (code[0] == 0x55 && code[1] == 0x8B && code[2] == 0xEC) validPrologue = true; // push ebp; mov ebp, esp
            if (code[0] == 0x8B && code[1] == 0xFF && code[2] == 0x55) validPrologue = true; // mov edi,edi; push ebp
            if (code[0] == 0x53 || code[0] == 0x56 || code[0] == 0x57) validPrologue = true; // push ebx/esi/edi
            if (code[0] == 0x83 && code[1] == 0xEC) validPrologue = true; // sub esp, imm8
            if (code[0] == 0x81 && code[1] == 0xEC) validPrologue = true; // sub esp, imm32
            if (code[0] == 0x6A) validPrologue = true; // push imm8
            if (code[0] == 0x8B && (code[1] == 0x44 || code[1] == 0x4C)) validPrologue = true; // mov reg, [esp+x]
            if (code[0] == 0x8B && code[1] == 0xD1) validPrologue = true; // mov edx, ecx (thiscall)
            if (code[0] == 0xE9) validPrologue = true; // jmp rel32 (hotpatch trampoline)

            if (!validPrologue) {
                LOG_INFO("ProcessEvent: vtable[{}] = 0x{:08X} — no valid prologue (bytes: {:02X} {:02X} {:02X}), skipping",
                         idx, funcAddr, code[0], code[1], code[2]);
                continue;
            }

            // Estimate function size by scanning for the next function prologue
            // (push ebp / mov ebp, esp = 55 8B EC) or next vtable entry
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

            if (estSize > 300 && estSize > bestSize) {
                // ProcessEvent is a large function — pick the biggest candidate
                bestSize = estSize;
                bestAddr = funcAddr;
                bestIdx = idx;
            }

            LOG_INFO("ProcessEvent: vtable[{}] = 0x{:08X}, ~{}B", idx, funcAddr, estSize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }

    if (bestAddr && bestSize > 300) {
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
    // NOTE: Do NOT check VirtualQuery here — Steam DRM can report valid code
    // pages as non-executable during early initialization.
    uintptr_t origAddr = *vtableSlot;
    s_OriginalProcessEvent = reinterpret_cast<ProcessEventFn>(origAddr);
    LOG_INFO("ProcessEvent: vtable fallback — original at 0x{:08X}", origAddr);

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
    s_HooksDirty = true;
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
    s_HooksDirty = true;
}

ProcessEventFn GetOriginalProcessEvent()
{
    return s_OriginalProcessEvent;
}

bool IsProcessEventHooked()
{
    return s_Hooked;
}

uintptr_t GetProcessEventAddress()
{
    return s_ProcessEventAddr;
}

const ProcessEventStats& GetProcessEventStats()
{
    return s_Stats;
}

} // namespace bs1sdk
