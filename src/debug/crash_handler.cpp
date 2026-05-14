#include "crash_handler.h"
#include "../core/log.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <chrono>
#include <mutex>
#include <atomic>

#pragma comment(lib, "dbghelp.lib")

namespace bs1sdk {

// ─── Breadcrumb Ring Buffer ──────────────────────────────────────────────

static constexpr int MAX_CRUMBS = 64;

struct Breadcrumb {
    char Label[128];
    DWORD ThreadId;
    uint64_t Tick;
};

static Breadcrumb s_Crumbs[MAX_CRUMBS];
static std::atomic<int> s_CrumbHead{0};
static char s_Context[256] = "init";
static PVOID s_VEH = nullptr;
static const char* s_CrashPath = "Z:\\Bioshock1SDK\\debug_dumps\\crash_report.txt";

void CrashBreadcrumb(const char* label)
{
    int idx = s_CrumbHead.fetch_add(1) % MAX_CRUMBS;
    strncpy(s_Crumbs[idx].Label, label, sizeof(s_Crumbs[idx].Label) - 1);
    s_Crumbs[idx].Label[sizeof(s_Crumbs[idx].Label) - 1] = '\0';
    s_Crumbs[idx].ThreadId = GetCurrentThreadId();
    s_Crumbs[idx].Tick = __rdtsc();
}

void CrashSetContext(const char* context)
{
    strncpy(s_Context, context, sizeof(s_Context) - 1);
    s_Context[sizeof(s_Context) - 1] = '\0';
}

const char* GetCrashReportPath() { return s_CrashPath; }

// ─── Safe Read ───────────────────────────────────────────────────────────

bool IsSafeToRead(const void* ptr, size_t len)
{
    if (!ptr) return false;
    __try {
        volatile char c;
        const char* p = (const char*)ptr;
        c = p[0];
        if (len > 1) c = p[len - 1];
        (void)c;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ─── Exception Name ──────────────────────────────────────────────────────

static const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
    case EXCEPTION_SINGLE_STEP:           return "SINGLE_STEP";
    default:                              return "UNKNOWN";
    }
}

// ─── Crash Dump Writer ───────────────────────────────────────────────────

static void WriteCrashReport(EXCEPTION_POINTERS* ep)
{
    FILE* f = fopen(s_CrashPath, "w");
    if (!f) return;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    PVOID addr = ep->ExceptionRecord->ExceptionAddress;
    CONTEXT* ctx = ep->ContextRecord;

    fprintf(f, "==========================================================\n");
    fprintf(f, "  BS1SDK CRASH REPORT\n");
    fprintf(f, "==========================================================\n\n");

    // Get current time
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt;
    localtime_s(&lt, &t);
    fprintf(f, "Time: %04d-%02d-%02d %02d:%02d:%02d\n",
            lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
            lt.tm_hour, lt.tm_min, lt.tm_sec);
    fprintf(f, "Thread: %lu\n", GetCurrentThreadId());
    fprintf(f, "Context: %s\n\n", s_Context);

    // Exception info
    fprintf(f, "--- Exception ---\n");
    fprintf(f, "Code:    0x%08lX (%s)\n", code, ExceptionName(code));
    fprintf(f, "Address: 0x%08IX\n", (uintptr_t)addr);

    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR target = ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(f, "Attempted %s at 0x%08IX\n",
                rw == 0 ? "READ" : rw == 1 ? "WRITE" : "EXECUTE",
                target);
    }
    fprintf(f, "\n");

    // Module info — which DLL/EXE the crash address belongs to
    fprintf(f, "--- Crash Module ---\n");
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hMod)) {
        char modName[MAX_PATH];
        GetModuleFileNameA(hMod, modName, sizeof(modName));
        uintptr_t modBase = (uintptr_t)hMod;
        uintptr_t offset = (uintptr_t)addr - modBase;
        fprintf(f, "Module: %s\n", modName);
        fprintf(f, "Base:   0x%08IX\n", modBase);
        fprintf(f, "Offset: 0x%08IX (crash address - base)\n", offset);
    } else {
        fprintf(f, "Module: UNKNOWN (address not in any loaded module)\n");
    }
    fprintf(f, "\n");

    // Registers
    fprintf(f, "--- Registers ---\n");
    fprintf(f, "EAX=%08lX  EBX=%08lX  ECX=%08lX  EDX=%08lX\n",
            ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    fprintf(f, "ESI=%08lX  EDI=%08lX  EBP=%08lX  ESP=%08lX\n",
            ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    fprintf(f, "EIP=%08lX  EFLAGS=%08lX\n", ctx->Eip, ctx->EFlags);
    fprintf(f, "\n");

    // Stack trace (raw — dump stack memory)
    fprintf(f, "--- Stack (raw, top 256 bytes) ---\n");
    uintptr_t esp = ctx->Esp;
    for (int i = 0; i < 256; i += 16) {
        if (!IsSafeToRead((void*)(esp + i), 16)) break;
        fprintf(f, "  %08IX: ", esp + i);
        for (int j = 0; j < 4; j++) {
            uint32_t val = *(uint32_t*)(esp + i + j * 4);
            fprintf(f, "%08X ", val);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n");

    // Try to identify stack return addresses (poor man's stack trace)
    fprintf(f, "--- Likely Return Addresses (stack scan) ---\n");
    HMODULE hGame = GetModuleHandleA(NULL);
    HMODULE hSDK = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&WriteCrashReport, &hSDK);

    MODULEINFO gameInfo = {}, sdkInfo = {};
    if (hGame) GetModuleInformation(GetCurrentProcess(), hGame, &gameInfo, sizeof(gameInfo));
    if (hSDK) GetModuleInformation(GetCurrentProcess(), hSDK, &sdkInfo, sizeof(sdkInfo));

    int found = 0;
    for (int i = 0; i < 512 && found < 20; i += 4) {
        if (!IsSafeToRead((void*)(esp + i), 4)) break;
        uintptr_t val = *(uintptr_t*)(esp + i);

        // Check if it looks like it's in a code section
        bool inGame = hGame && val >= (uintptr_t)gameInfo.lpBaseOfDll &&
                      val < (uintptr_t)gameInfo.lpBaseOfDll + gameInfo.SizeOfImage;
        bool inSDK = hSDK && val >= (uintptr_t)sdkInfo.lpBaseOfDll &&
                     val < (uintptr_t)sdkInfo.lpBaseOfDll + sdkInfo.SizeOfImage;

        if (inGame || inSDK) {
            const char* which = inSDK ? "BS1SDK.dll" : "Bioshock.exe";
            uintptr_t base = inSDK ? (uintptr_t)sdkInfo.lpBaseOfDll : (uintptr_t)gameInfo.lpBaseOfDll;
            fprintf(f, "  ESP+%03X: 0x%08IX  [%s+0x%08IX]\n",
                    i, val, which, val - base);
            found++;
        }
    }
    fprintf(f, "\n");

    // Breadcrumbs
    fprintf(f, "--- Last %d Breadcrumbs (newest first) ---\n", MAX_CRUMBS);
    int head = s_CrumbHead.load();
    for (int i = 0; i < MAX_CRUMBS; i++) {
        int idx = ((head - 1 - i) % MAX_CRUMBS + MAX_CRUMBS) % MAX_CRUMBS;
        if (s_Crumbs[idx].Label[0] == '\0') continue;
        fprintf(f, "  [%2d] tid=%lu  %s\n", i, s_Crumbs[idx].ThreadId, s_Crumbs[idx].Label);
    }
    fprintf(f, "\n");

    // Loaded modules
    fprintf(f, "--- Loaded Modules ---\n");
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me = { sizeof(MODULEENTRY32) };
        if (Module32First(hSnap, &me)) {
            do {
                fprintf(f, "  0x%08IX - 0x%08IX  %s\n",
                        (uintptr_t)me.modBaseAddr,
                        (uintptr_t)me.modBaseAddr + me.modBaseSize,
                        me.szModule);
            } while (Module32Next(hSnap, &me));
        }
        CloseHandle(hSnap);
    }

    fprintf(f, "\n==========================================================\n");
    fprintf(f, "END OF CRASH REPORT\n");
    fprintf(f, "==========================================================\n");
    fclose(f);
}

// ─── VEH Handler ─────────────────────────────────────────────────────────

static LONG WINAPI CrashVEH(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Only handle fatal exceptions
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Write crash report
    WriteCrashReport(ep);

    // Let it crash normally after writing report
    return EXCEPTION_CONTINUE_SEARCH;
}

// ─── Install / Remove ────────────────────────────────────────────────────

void InstallCrashHandler()
{
    memset(s_Crumbs, 0, sizeof(s_Crumbs));
    s_VEH = AddVectoredExceptionHandler(1, CrashVEH);
    if (s_VEH) {
        LOG_INFO("[CrashHandler] Installed VEH crash handler");
    }
}

void RemoveCrashHandler()
{
    if (s_VEH) {
        RemoveVectoredExceptionHandler(s_VEH);
        s_VEH = nullptr;
    }
}

} // namespace bs1sdk
