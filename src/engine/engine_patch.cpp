#include "engine_patch.h"
#include "../core/log.h"
#include "../debug/crash_handler.h"

#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <atomic>

namespace bs1sdk {

// ═══════════════════════════════════════════════════════════════════════════
//  Trampoline Memory Allocator
// ═══════════════════════════════════════════════════════════════════════════

// Single page of executable memory for all trampolines
static uint8_t* s_TrampolinePage = nullptr;
static size_t   s_TrampolineUsed = 0;
static constexpr size_t TRAMPOLINE_PAGE_SIZE = 4096;

static uint8_t* AllocTrampoline(size_t size)
{
    if (!s_TrampolinePage) {
        s_TrampolinePage = (uint8_t*)VirtualAlloc(
            nullptr, TRAMPOLINE_PAGE_SIZE,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s_TrampolinePage) {
            LOG_ERROR("[EnginePatch] Failed to allocate trampoline page");
            return nullptr;
        }
        memset(s_TrampolinePage, 0xCC, TRAMPOLINE_PAGE_SIZE); // fill with INT3
    }

    if (s_TrampolineUsed + size > TRAMPOLINE_PAGE_SIZE) {
        LOG_ERROR("[EnginePatch] Trampoline page full");
        return nullptr;
    }

    uint8_t* ptr = s_TrampolinePage + s_TrampolineUsed;
    s_TrampolineUsed += size;
    return ptr;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Patch Helpers
// ═══════════════════════════════════════════════════════════════════════════

static bool WriteBytes(uintptr_t addr, const void* data, size_t len)
{
    DWORD oldProtect;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("[EnginePatch] VirtualProtect failed at 0x{:08X}", (uint32_t)addr);
        return false;
    }
    memcpy((void*)addr, data, len);
    VirtualProtect((void*)addr, len, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Patch 1: Null dereference at BioshockHD.exe+0x763F9B
//  
//  Original:  8B 83 34 02 00 00   ; mov eax, [ebx+0x234]
//  Problem:   EBX can be NULL → ACCESS_VIOLATION reading 0x234
//  Context:   Havok/render worker thread iterating actors
//  Fix:       Add null check — if EBX is null, set EAX=0 and skip
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uintptr_t PATCH1_RVA = 0x763F9B;
static constexpr uint8_t   PATCH1_ORIGINAL[] = { 0x8B, 0x83, 0x34, 0x02, 0x00, 0x00 };
static constexpr size_t    PATCH1_SIZE = sizeof(PATCH1_ORIGINAL);

static uint8_t  s_Patch1_SavedBytes[PATCH1_SIZE] = {};
static uintptr_t s_Patch1_Addr = 0;
static bool      s_Patch1_Installed = false;

// Counter: how many times the null path was taken (atomic for thread safety)
static std::atomic<uint32_t> s_Patch1_NullHits{0};

// The trampoline is built dynamically because we need to compute relative JMPs.
static bool InstallPatch1()
{
    // Find BioshockHD.exe base
    HMODULE hGame = GetModuleHandleA("BioshockHD.exe");
    if (!hGame) hGame = GetModuleHandleA(NULL);
    if (!hGame) {
        LOG_ERROR("[EnginePatch] Cannot find game module");
        return false;
    }

    uintptr_t base = (uintptr_t)hGame;
    s_Patch1_Addr = base + PATCH1_RVA;

    // Verify the bytes at the patch site match what we expect
    if (!IsSafeToRead((void*)s_Patch1_Addr, PATCH1_SIZE)) {
        LOG_ERROR("[EnginePatch] Patch1 address 0x{:08X} not readable", (uint32_t)s_Patch1_Addr);
        return false;
    }

    if (memcmp((void*)s_Patch1_Addr, PATCH1_ORIGINAL, PATCH1_SIZE) != 0) {
        // Bytes don't match — might be a different game version or already patched
        LOG_WARN("[EnginePatch] Patch1: bytes at 0x{:08X} don't match expected pattern", 
                 (uint32_t)s_Patch1_Addr);
        LOG_WARN("[EnginePatch]   Expected: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                 PATCH1_ORIGINAL[0], PATCH1_ORIGINAL[1], PATCH1_ORIGINAL[2],
                 PATCH1_ORIGINAL[3], PATCH1_ORIGINAL[4], PATCH1_ORIGINAL[5]);
        uint8_t* actual = (uint8_t*)s_Patch1_Addr;
        LOG_WARN("[EnginePatch]   Actual:   {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                 actual[0], actual[1], actual[2], actual[3], actual[4], actual[5]);
        return false;
    }

    // Save original bytes for restoration
    memcpy(s_Patch1_SavedBytes, (void*)s_Patch1_Addr, PATCH1_SIZE);

    // Build trampoline:
    //   offset 0:  test ebx, ebx           ; 85 DB           (2 bytes)
    //   offset 2:  jz .null_ebx            ; 74 0B           (2 bytes, skip 11)
    //   offset 4:  mov eax, [ebx+0x234]    ; 8B 83 34 02 00 00 (6 bytes, original)
    //   offset 10: jmp return_addr          ; E9 xx xx xx xx  (5 bytes)
    //   offset 15: xor eax, eax            ; 33 C0           (2 bytes)  [.null_ebx]
    //   offset 17: inc [counter]            ; FF 05 xx xx xx xx (6 bytes)
    //   offset 23: nop                       ; 90 (1 byte)
    //   offset 24: jmp return_addr          ; E9 xx xx xx xx  (5 bytes)
    //   Total: 29 bytes
    //
    //   NOTE: No LOCK prefix! 8 worker threads all hitting this path would
    //   serialize on the bus lock and tank FPS. Non-atomic inc is fine here.

    constexpr size_t TRAMP_SIZE = 29;
    uint8_t* tramp = AllocTrampoline(TRAMP_SIZE);
    if (!tramp) return false;

    uintptr_t returnAddr = s_Patch1_Addr + PATCH1_SIZE; // instruction after the patched site
    uintptr_t counterAddr = (uintptr_t)&s_Patch1_NullHits;

    uint8_t code[TRAMP_SIZE];
    size_t p = 0;

    // test ebx, ebx
    code[p++] = 0x85; code[p++] = 0xDB;

    // jz .null_ebx (offset 15 - (offset 2 + 2) = 11)
    code[p++] = 0x74; code[p++] = 0x0B;

    // mov eax, [ebx+0x234] (original instruction)
    code[p++] = 0x8B; code[p++] = 0x83;
    code[p++] = 0x34; code[p++] = 0x02; code[p++] = 0x00; code[p++] = 0x00;

    // jmp return_addr
    code[p++] = 0xE9;
    int32_t rel1 = (int32_t)(returnAddr - ((uintptr_t)tramp + p + 4));
    memcpy(&code[p], &rel1, 4); p += 4;

    // .null_ebx: xor eax, eax
    code[p++] = 0x33; code[p++] = 0xC0;

    // inc dword ptr [counter] (NO LOCK — avoids bus lock contention across worker threads)
    code[p++] = 0xFF; code[p++] = 0x05; // INC dword ptr [addr32]
    memcpy(&code[p], &counterAddr, 4); p += 4;
    code[p++] = 0x90; // NOP (fills the byte where LOCK prefix was)

    // jmp return_addr
    code[p++] = 0xE9;
    int32_t rel2 = (int32_t)(returnAddr - ((uintptr_t)tramp + p + 4));
    memcpy(&code[p], &rel2, 4); p += 4;

    // Write trampoline
    memcpy(tramp, code, TRAMP_SIZE);

    // Patch the original site: JMP trampoline + NOP
    uint8_t patch[PATCH1_SIZE];
    patch[0] = 0xE9; // JMP rel32
    int32_t relPatch = (int32_t)((uintptr_t)tramp - (s_Patch1_Addr + 5));
    memcpy(&patch[1], &relPatch, 4);
    patch[5] = 0x90; // NOP

    if (!WriteBytes(s_Patch1_Addr, patch, PATCH1_SIZE)) {
        LOG_ERROR("[EnginePatch] Failed to write patch at 0x{:08X}", (uint32_t)s_Patch1_Addr);
        return false;
    }

    s_Patch1_Installed = true;
    LOG_INFO("[EnginePatch] Patch1 installed: null-check at 0x{:08X} (BioshockHD.exe+0x{:08X})",
             (uint32_t)s_Patch1_Addr, (uint32_t)PATCH1_RVA);
    LOG_INFO("[EnginePatch]   Trampoline at 0x{:08X} ({} bytes)", (uint32_t)(uintptr_t)tramp, TRAMP_SIZE);
    return true;
}

static void RemovePatch1()
{
    if (!s_Patch1_Installed) return;
    WriteBytes(s_Patch1_Addr, s_Patch1_SavedBytes, PATCH1_SIZE);
    s_Patch1_Installed = false;
    LOG_INFO("[EnginePatch] Patch1 removed (original bytes restored)");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════════════

static void CrashProvider_EnginePatches(FILE* f1, FILE* f2)
{
    auto emit = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt); if (f1) vfprintf(f1, fmt, ap); va_end(ap);
        va_start(ap, fmt); if (f2) vfprintf(f2, fmt, ap); va_end(ap);
    };

    EnginePatchInfo patches[8];
    int count = GetEnginePatches(patches, 8);
    emit("  Installed patches: %d\n", count);
    for (int i = 0; i < count; i++) {
        emit("  [%d] %s: addr=0x%08X installed=%s triggered=%s (%lu hits)\n",
             i, patches[i].name, (unsigned)patches[i].address,
             patches[i].installed ? "YES" : "NO",
             patches[i].triggered ? "YES" : "no",
             (unsigned long)s_Patch1_NullHits.load());
        emit("      %s\n", patches[i].description);
    }
}

void InstallEnginePatches()
{
    LOG_INFO("[EnginePatch] Installing engine patches...");

    if (InstallPatch1()) {
        LOG_INFO("[EnginePatch] Patch1 (null-deref +0x{:08X}): INSTALLED", (uint32_t)PATCH1_RVA);
    } else {
        LOG_WARN("[EnginePatch] Patch1 (null-deref +0x{:08X}): FAILED", (uint32_t)PATCH1_RVA);
    }

    RegisterCrashInfoProvider("ENGINE PATCHES", CrashProvider_EnginePatches);
}

void RemoveEnginePatches()
{
    RemovePatch1();

    if (s_TrampolinePage) {
        VirtualFree(s_TrampolinePage, 0, MEM_RELEASE);
        s_TrampolinePage = nullptr;
        s_TrampolineUsed = 0;
    }
}

int GetEnginePatches(EnginePatchInfo* out, int maxCount)
{
    if (maxCount < 1 || !out) return 0;

    out[0].name = "NullDeref_763F9B";
    out[0].description = "Null-check EBX before mov eax,[ebx+0x234] on worker thread";
    out[0].address = s_Patch1_Addr;
    out[0].installed = s_Patch1_Installed;
    out[0].triggered = s_Patch1_NullHits.load() > 0;

    return 1;
}

} // namespace bs1sdk
