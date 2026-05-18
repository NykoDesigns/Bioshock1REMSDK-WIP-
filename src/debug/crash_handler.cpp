#include "crash_handler.h"
#include "../core/log.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <atomic>

#pragma comment(lib, "dbghelp.lib")

namespace bs1sdk {

// ═══════════════════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int    MAX_CRUMBS       = 128;
static constexpr int    STACK_DUMP_BYTES = 2048;
static constexpr int    MAX_STACK_FRAMES = 40;
static constexpr int    CODE_DUMP_BYTES  = 64;    // bytes before+after EIP
static constexpr int    MAX_REG_DEREF    = 3;     // pointer-chase depth

// ═══════════════════════════════════════════════════════════════════════════
//  Breadcrumb Ring Buffer (lock-free)
// ═══════════════════════════════════════════════════════════════════════════

struct Breadcrumb {
    char     Label[192];
    DWORD    ThreadId;
    DWORD    Timestamp;     // GetTickCount() for human-readable relative timing
    uint64_t TSC;           // raw TSC for ordering
};

static Breadcrumb        s_Crumbs[MAX_CRUMBS];
static std::atomic<int>  s_CrumbHead{0};
static char              s_Context[256] = "init";
static PVOID             s_VEH = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER s_PrevUEF = nullptr;
static char              s_CrashDir[MAX_PATH] = {0};
static char              s_CrashReportPath[MAX_PATH] = {0};
static char              s_MiniDumpPath[MAX_PATH] = {0};
static char              s_FullDumpPath[MAX_PATH] = {0};
static DWORD             s_InitTick = 0;          // GetTickCount() at init
static std::atomic<bool> s_ReportWritten{false};

// ═══════════════════════════════════════════════════════════════════════════
//  Path Setup
// ═══════════════════════════════════════════════════════════════════════════

static void InitCrashPaths()
{
    HMODULE hGame = GetModuleHandleA(NULL);
    char exePath[MAX_PATH];
    GetModuleFileNameA(hGame, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0';

    snprintf(s_CrashDir, MAX_PATH, "%s\\BS1SDK_dumps", exePath);
    CreateDirectoryA(s_CrashDir, NULL);

    snprintf(s_CrashReportPath, MAX_PATH, "%s\\crash_report.txt", s_CrashDir);
    snprintf(s_MiniDumpPath, MAX_PATH, "%s\\crash.dmp", s_CrashDir);
    snprintf(s_FullDumpPath, MAX_PATH, "%s\\crash_full.dmp", s_CrashDir);

    CreateDirectoryA("Z:\\Bioshock1SDK\\debug_dumps", NULL);

    char markerPath[MAX_PATH];
    snprintf(markerPath, MAX_PATH, "%s\\sdk_loaded.txt", s_CrashDir);
    FILE* marker = fopen(markerPath, "w");
    if (marker) {
        fprintf(marker, "BS1SDK crash handler initialized\n");
        fprintf(marker, "Crash reports will be written to: %s\n", s_CrashDir);
        fprintf(marker, "EXE path: %s\n", exePath);
        fclose(marker);
    }

    s_InitTick = GetTickCount();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Public Breadcrumb / Context API
// ═══════════════════════════════════════════════════════════════════════════

void CrashBreadcrumb(const char* label)
{
    int idx = s_CrumbHead.fetch_add(1) % MAX_CRUMBS;
    strncpy(s_Crumbs[idx].Label, label, sizeof(s_Crumbs[idx].Label) - 1);
    s_Crumbs[idx].Label[sizeof(s_Crumbs[idx].Label) - 1] = '\0';
    s_Crumbs[idx].ThreadId = GetCurrentThreadId();
    s_Crumbs[idx].Timestamp = GetTickCount();
    s_Crumbs[idx].TSC = __rdtsc();
}

void CrashBreadcrumbf(const char* fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    CrashBreadcrumb(buf);
}

void CrashSetContext(const char* context)
{
    strncpy(s_Context, context, sizeof(s_Context) - 1);
    s_Context[sizeof(s_Context) - 1] = '\0';
}

const char* GetCrashReportPath() { return s_CrashReportPath; }

// ─── Scoped Context ──────────────────────────────────────────────────────

ScopedCrashContext::ScopedCrashContext(const char* ctx)
{
    strncpy(prev, s_Context, sizeof(prev));
    prev[sizeof(prev) - 1] = '\0';
    CrashSetContext(ctx);
}

ScopedCrashContext::~ScopedCrashContext()
{
    CrashSetContext(prev);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Crash Info Provider Registry
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int MAX_PROVIDERS = 16;
struct ProviderEntry {
    const char*         name;
    CrashInfoProviderFn fn;
};
static ProviderEntry s_Providers[MAX_PROVIDERS] = {};
static int           s_ProviderCount = 0;

void RegisterCrashInfoProvider(const char* name, CrashInfoProviderFn fn)
{
    if (s_ProviderCount >= MAX_PROVIDERS) return;
    s_Providers[s_ProviderCount].name = name;
    s_Providers[s_ProviderCount].fn   = fn;
    s_ProviderCount++;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Safe Memory Probing
// ═══════════════════════════════════════════════════════════════════════════

// Thread-local guard: when > 0, VEH knows we're inside IsSafeToRead and
// the exception will be caught by SEH — don't write a crash report.
static __declspec(thread) int t_ProbingDepth = 0;

// Runtime max user-mode address — respects LARGEADDRESSAWARE on WoW64
static uintptr_t GetMaxUserAddress()
{
    static uintptr_t s_Max = 0;
    if (!s_Max) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        s_Max = (uintptr_t)si.lpMaximumApplicationAddress;
    }
    return s_Max;
}

bool IsSafeToRead(const void* ptr, size_t len)
{
    if (!ptr) return false;
    // Reject addresses outside valid user-mode range (LAA-aware)
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < 0x10000u || addr > GetMaxUserAddress()) return false;
    t_ProbingDepth++;
    __try {
        volatile char c;
        const char* p = (const char*)ptr;
        c = p[0];
        if (len > 1) c = p[len - 1];
        (void)c;
        t_ProbingDepth--;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        t_ProbingDepth--;
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Helpers for the crash report writer (all self-contained, no STL)
// ═══════════════════════════════════════════════════════════════════════════

static const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
    case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
    case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
    case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
    case 0xE06D7363:                         return "CPP_EXCEPTION (throw)";
    default:                                 return "UNKNOWN";
    }
}

// Resolve an address to "module+0xOFFSET" — writes into buf, returns buf
static const char* ResolveAddr(uintptr_t addr, char* buf, size_t bufSize)
{
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hMod)) {
        char modFull[MAX_PATH];
        GetModuleFileNameA(hMod, modFull, sizeof(modFull));
        // Extract just the filename
        const char* name = strrchr(modFull, '\\');
        name = name ? name + 1 : modFull;
        snprintf(buf, bufSize, "%s+0x%08X", name, (unsigned)(addr - (uintptr_t)hMod));
    } else {
        snprintf(buf, bufSize, "0x%08X", (unsigned)addr);
    }
    return buf;
}

// Try to dereference a register value and print what it points to
static void DumpRegPointer(FILE* f, const char* regName, uintptr_t val)
{
    fprintf(f, "  %s = 0x%08X", regName, (unsigned)val);
    if (!val) { fprintf(f, "  (NULL)\n"); return; }

    // Check what module it's in
    char addrBuf[256];
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)val, &hMod)) {
        char modFull[MAX_PATH];
        GetModuleFileNameA(hMod, modFull, sizeof(modFull));
        const char* name = strrchr(modFull, '\\');
        name = name ? name + 1 : modFull;
        fprintf(f, "  -> %s+0x%X", name, (unsigned)(val - (uintptr_t)hMod));
    }

    // Pointer chase: dereference up to MAX_REG_DEREF levels
    uintptr_t cur = val;
    for (int depth = 0; depth < MAX_REG_DEREF; depth++) {
        if (!IsSafeToRead((void*)cur, 4)) break;
        uintptr_t next = *(uintptr_t*)cur;
        fprintf(f, "  -> [0x%08X]", (unsigned)next);
        cur = next;
        if (!cur) break;
    }
    fprintf(f, "\n");

    // If readable, dump first 32 bytes as hex
    if (IsSafeToRead((void*)val, 32)) {
        fprintf(f, "         hex: ");
        const uint8_t* p = (const uint8_t*)val;
        for (int i = 0; i < 32; i++) fprintf(f, "%02X ", p[i]);
        fprintf(f, "\n");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  DbgHelp Stack Walk
// ═══════════════════════════════════════════════════════════════════════════

static void WriteDbgHelpStackTrace(FILE* f, CONTEXT* ctx)
{
    HANDLE hProc = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();

    // Initialize DbgHelp (safe to call multiple times)
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(hProc, NULL, TRUE);

    STACKFRAME frame = {};
    frame.AddrPC.Offset    = ctx->Eip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Ebp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Esp;
    frame.AddrStack.Mode   = AddrModeFlat;

    fprintf(f, "--- DbgHelp Stack Trace ---\n");

    char symBuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    for (int i = 0; i < MAX_STACK_FRAMES; i++) {
        if (!StackWalk(IMAGE_FILE_MACHINE_I386, hProc, hThread, &frame,
                       ctx, NULL, SymFunctionTableAccess, SymGetModuleBase, NULL)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;

        uintptr_t pc = (uintptr_t)frame.AddrPC.Offset;
        char addrBuf[256];
        ResolveAddr(pc, addrBuf, sizeof(addrBuf));

        // Try to get symbol name
        DWORD64 displacement = 0;
        if (SymFromAddr(hProc, pc, &displacement, sym)) {
            // Try to get source line info
            IMAGEHLP_LINE line = { sizeof(IMAGEHLP_LINE) };
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr(hProc, pc, &lineDisp, &line)) {
                fprintf(f, "  #%2d  %s  %s+0x%X  (%s:%lu)\n",
                        i, addrBuf, sym->Name, (unsigned)displacement,
                        line.FileName, line.LineNumber);
            } else {
                fprintf(f, "  #%2d  %s  %s+0x%X\n",
                        i, addrBuf, sym->Name, (unsigned)displacement);
            }
        } else {
            fprintf(f, "  #%2d  %s\n", i, addrBuf);
        }
    }
    fprintf(f, "\n");

    SymCleanup(hProc);
}

// ═══════════════════════════════════════════════════════════════════════════
//  All-Thread Stack Traces
// ═══════════════════════════════════════════════════════════════════════════

static void DumpThreadStack(FILE* f, DWORD tid, HANDLE hProc)
{
    HANDLE hThread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, tid);
    if (!hThread) {
        fprintf(f, "  (could not open thread %lu)\n", tid);
        return;
    }

    SuspendThread(hThread);

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(hThread, &ctx)) {
        STACKFRAME frame = {};
        frame.AddrPC.Offset    = ctx.Eip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Ebp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Esp;
        frame.AddrStack.Mode   = AddrModeFlat;

        fprintf(f, "  EIP=0x%08X ESP=0x%08X EBP=0x%08X\n",
                (unsigned)ctx.Eip, (unsigned)ctx.Esp, (unsigned)ctx.Ebp);

        char symBuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;

        for (int i = 0; i < 20; i++) {
            if (!StackWalk(IMAGE_FILE_MACHINE_I386, hProc, hThread, &frame,
                           &ctx, NULL, SymFunctionTableAccess, SymGetModuleBase, NULL))
                break;
            if (frame.AddrPC.Offset == 0) break;

            uintptr_t pc = (uintptr_t)frame.AddrPC.Offset;
            char addrBuf[256];
            ResolveAddr(pc, addrBuf, sizeof(addrBuf));

            DWORD64 displacement = 0;
            if (SymFromAddr(hProc, pc, &displacement, sym))
                fprintf(f, "    #%2d  %s  %s+0x%X\n", i, addrBuf, sym->Name, (unsigned)displacement);
            else
                fprintf(f, "    #%2d  %s\n", i, addrBuf);
        }
    } else {
        fprintf(f, "  (could not get thread context)\n");
    }

    ResumeThread(hThread);
    CloseHandle(hThread);
}

static void WriteAllThreadStacks(FILE* f, DWORD crashTid)
{
    HANDLE hProc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(hProc, NULL, TRUE);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        fprintf(f, "  (could not enumerate threads)\n");
        SymCleanup(hProc);
        return;
    }

    THREADENTRY32 te = { sizeof(THREADENTRY32) };
    DWORD pid = GetCurrentProcessId();
    int count = 0;

    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == crashTid) {
                fprintf(f, "  --- TID %lu (CRASH THREAD — see stack trace above) ---\n\n", te.th32ThreadID);
                continue;
            }

            fprintf(f, "  --- TID %lu (priority=%ld) ---\n", te.th32ThreadID, te.tpBasePri);
            __try {
                DumpThreadStack(f, te.th32ThreadID, hProc);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(f, "    (exception during stack walk)\n");
            }
            fprintf(f, "\n");
            count++;

            if (count >= 30) {
                fprintf(f, "  (truncated after 30 threads)\n");
                break;
            }
        } while (Thread32Next(hSnap, &te));
    }

    CloseHandle(hSnap);
    SymCleanup(hProc);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Memory Region Dump (around an address)
// ═══════════════════════════════════════════════════════════════════════════

static void DumpMemoryRegion(FILE* f, uintptr_t centerAddr, int bytesAround)
{
    uintptr_t start = (centerAddr > (uintptr_t)bytesAround) ? centerAddr - bytesAround : 0;
    uintptr_t end   = centerAddr + bytesAround;

    for (uintptr_t row = start & ~0xFULL; row < end; row += 16) {
        if (!IsSafeToRead((void*)row, 16)) {
            fprintf(f, "    %08X: ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??\n", (unsigned)row);
            continue;
        }
        fprintf(f, "    %08X: ", (unsigned)row);
        const uint8_t* p = (const uint8_t*)row;
        for (int j = 0; j < 16; j++) {
            if (row + j == centerAddr)
                fprintf(f, "[%02X]", p[j]);
            else
                fprintf(f, " %02X ", p[j]);
        }
        fprintf(f, "  ");
        for (int j = 0; j < 16; j++) {
            char c = (p[j] >= 0x20 && p[j] < 0x7F) ? (char)p[j] : '.';
            fprintf(f, "%c", c);
        }
        fprintf(f, "\n");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Main Crash Report Writer
// ═══════════════════════════════════════════════════════════════════════════

static void WriteCrashReport(EXCEPTION_POINTERS* ep)
{
    // Open report file — try game dir, then fallback
    FILE* f = fopen(s_CrashReportPath, "w");
    if (!f) f = fopen("crash_report.txt", "w");
    if (!f) return;

    FILE* fDev = fopen("Z:\\Bioshock1SDK\\debug_dumps\\crash_report.txt", "w");

    // Use a lambda-like macro to write to both files at once
    #define EMIT(...) do { fprintf(f, __VA_ARGS__); if (fDev) fprintf(fDev, __VA_ARGS__); } while(0)

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    PVOID addr = ep->ExceptionRecord->ExceptionAddress;
    CONTEXT* ctx = ep->ContextRecord;

    EMIT("╔══════════════════════════════════════════════════════════════╗\n");
    EMIT("║              BS1SDK CRASH REPORT v3.0                      ║\n");
    EMIT("╚══════════════════════════════════════════════════════════════╝\n\n");

    // ─── Timestamp & Identity ─────────────────────────────────────────
    SYSTEMTIME st;
    GetLocalTime(&st);
    EMIT("Time:       %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    EMIT("Uptime:     %.1f seconds (since SDK init)\n",
         (GetTickCount() - s_InitTick) / 1000.0f);
    EMIT("Thread:     %lu (0x%lX)\n", GetCurrentThreadId(), GetCurrentThreadId());
    EMIT("Process:    %lu\n", GetCurrentProcessId());
    EMIT("Context:    %s\n\n", s_Context);

    // ─── Exception Details ────────────────────────────────────────────
    EMIT("=== EXCEPTION ===\n");
    EMIT("Code:       0x%08lX (%s)\n", code, ExceptionName(code));

    char addrBuf[256];
    EMIT("Address:    0x%08X  [%s]\n", (unsigned)(uintptr_t)addr,
         ResolveAddr((uintptr_t)addr, addrBuf, sizeof(addrBuf)));

    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR target = ep->ExceptionRecord->ExceptionInformation[1];
        EMIT("Operation:  Attempted %s at 0x%08X\n",
             rw == 0 ? "READ" : rw == 1 ? "WRITE" : "DEP/EXECUTE",
             (unsigned)target);
        // Check if target is near-null (common this-> dereference)
        if (target < 0x10000)
            EMIT("            *** NEAR-NULL dereference (likely null pointer + offset 0x%X) ***\n", (unsigned)target);
        else if (target >= 0x80000000u)
            EMIT("            *** KERNEL ADDRESS (corruption / wild pointer) ***\n");
    }
    if (code == EXCEPTION_IN_PAGE_ERROR && ep->ExceptionRecord->NumberParameters >= 3) {
        EMIT("NTSTATUS:   0x%08X\n", (unsigned)ep->ExceptionRecord->ExceptionInformation[2]);
    }
    // Chained exceptions
    if (ep->ExceptionRecord->ExceptionRecord) {
        EMIT("Chained:    0x%08lX (%s)\n",
             ep->ExceptionRecord->ExceptionRecord->ExceptionCode,
             ExceptionName(ep->ExceptionRecord->ExceptionRecord->ExceptionCode));
    }
    EMIT("\n");

    // ─── Access Violation Target Memory ───────────────────────────────
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR avTarget = ep->ExceptionRecord->ExceptionInformation[1];
        if (avTarget >= 0x10000 && avTarget < 0x80000000u) {
            EMIT("=== MEMORY AROUND AV TARGET (0x%08X ± 64 bytes) ===\n", (unsigned)avTarget);
            DumpMemoryRegion(f, (uintptr_t)avTarget, 64);
            if (fDev) DumpMemoryRegion(fDev, (uintptr_t)avTarget, 64);
            EMIT("\n");
        } else if (avTarget >= 0x10000) {
            EMIT("=== AV TARGET 0x%08X (kernel/unmapped — cannot dump) ===\n\n", (unsigned)avTarget);
        } else {
            // Near-null — dump what the base pointer (register) SHOULD have been
            // Try all registers to see which one was the base
            EMIT("=== NULL POINTER ANALYSIS (target offset 0x%X) ===\n", (unsigned)avTarget);
            struct { const char* name; DWORD val; } regs[] = {
                {"EAX", ctx->Eax}, {"EBX", ctx->Ebx}, {"ECX", ctx->Ecx}, {"EDX", ctx->Edx},
                {"ESI", ctx->Esi}, {"EDI", ctx->Edi}
            };
            for (auto& r : regs) {
                if (r.val == 0) {
                    EMIT("  %s = NULL  *** LIKELY BASE REGISTER (NULL + 0x%X = 0x%08X) ***\n",
                         r.name, (unsigned)avTarget, (unsigned)avTarget);
                } else if (IsSafeToRead((void*)(r.val + avTarget), 4)) {
                    uint32_t deref = *(uint32_t*)(r.val + avTarget);
                    EMIT("  %s = 0x%08X  [%s+0x%X] = 0x%08X\n",
                         r.name, r.val, r.name, (unsigned)avTarget, deref);
                }
            }
            EMIT("\n");
        }
    }

    // ─── Crash Module ─────────────────────────────────────────────────
    EMIT("=== CRASH MODULE ===\n");
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hMod)) {
        char modName[MAX_PATH];
        GetModuleFileNameA(hMod, modName, sizeof(modName));
        uintptr_t modBase = (uintptr_t)hMod;
        uintptr_t offset = (uintptr_t)addr - modBase;

        MODULEINFO mi = {};
        GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi));

        EMIT("Module:     %s\n", modName);
        EMIT("Base:       0x%08X\n", (unsigned)modBase);
        EMIT("Size:       0x%08X (%u KB)\n", mi.SizeOfImage, mi.SizeOfImage / 1024);
        EMIT("Offset:     0x%08X\n", (unsigned)offset);

        // Check for PDB next to module
        char pdbPath[MAX_PATH];
        strncpy(pdbPath, modName, MAX_PATH);
        char* dot = strrchr(pdbPath, '.');
        if (dot) { strcpy(dot, ".pdb"); }
        DWORD pdbAttr = GetFileAttributesA(pdbPath);
        if (pdbAttr != INVALID_FILE_ATTRIBUTES) {
            EMIT("PDB:        %s (AVAILABLE)\n", pdbPath);
        } else {
            EMIT("PDB:        NOT FOUND (place .pdb next to DLL for symbols)\n");
        }
    } else {
        EMIT("Module:     UNKNOWN (0x%08X not in any loaded module!)\n", (unsigned)(uintptr_t)addr);
    }
    EMIT("\n");

    // ─── Registers + Pointer Chase ────────────────────────────────────
    EMIT("=== REGISTERS ===\n");
    EMIT("EAX=%08lX  EBX=%08lX  ECX=%08lX  EDX=%08lX\n",
         ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    EMIT("ESI=%08lX  EDI=%08lX  EBP=%08lX  ESP=%08lX\n",
         ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    EMIT("EIP=%08lX  EFLAGS=%08lX\n\n", ctx->Eip, ctx->EFlags);

    EMIT("=== REGISTER POINTER ANALYSIS ===\n");
    DumpRegPointer(f, "EAX", ctx->Eax); if (fDev) DumpRegPointer(fDev, "EAX", ctx->Eax);
    DumpRegPointer(f, "EBX", ctx->Ebx); if (fDev) DumpRegPointer(fDev, "EBX", ctx->Ebx);
    DumpRegPointer(f, "ECX", ctx->Ecx); if (fDev) DumpRegPointer(fDev, "ECX", ctx->Ecx);
    DumpRegPointer(f, "EDX", ctx->Edx); if (fDev) DumpRegPointer(fDev, "EDX", ctx->Edx);
    DumpRegPointer(f, "ESI", ctx->Esi); if (fDev) DumpRegPointer(fDev, "ESI", ctx->Esi);
    DumpRegPointer(f, "EDI", ctx->Edi); if (fDev) DumpRegPointer(fDev, "EDI", ctx->Edi);
    DumpRegPointer(f, "EBP", ctx->Ebp); if (fDev) DumpRegPointer(fDev, "EBP", ctx->Ebp);
    EMIT("\n");

    // ─── Code bytes around EIP ────────────────────────────────────────
    EMIT("=== CODE BYTES (EIP-0x%X .. EIP+0x%X) ===\n", CODE_DUMP_BYTES, CODE_DUMP_BYTES);
    uintptr_t codeStart = ctx->Eip - CODE_DUMP_BYTES;
    for (int row = 0; row < CODE_DUMP_BYTES * 2; row += 16) {
        uintptr_t rowAddr = codeStart + row;
        if (!IsSafeToRead((void*)rowAddr, 16)) {
            EMIT("  %08X: ????????????????????????????????\n", (unsigned)rowAddr);
            continue;
        }
        EMIT("  %08X: ", (unsigned)rowAddr);
        const uint8_t* p = (const uint8_t*)rowAddr;
        for (int j = 0; j < 16; j++) {
            if (rowAddr + j == ctx->Eip) EMIT("[%02X]", p[j]); // highlight crash byte
            else EMIT(" %02X ", p[j]);
        }
        EMIT("  ");
        for (int j = 0; j < 16; j++) {
            char c = (p[j] >= 0x20 && p[j] < 0x7F) ? (char)p[j] : '.';
            EMIT("%c", c);
        }
        EMIT("\n");
    }
    EMIT("\n");

    // ─── DbgHelp Stack Trace ──────────────────────────────────────────
    // Write to both files manually since WriteDbgHelpStackTrace takes single FILE*
    WriteDbgHelpStackTrace(f, ctx);
    if (fDev) WriteDbgHelpStackTrace(fDev, ctx);

    // ─── Raw Stack Dump ───────────────────────────────────────────────
    EMIT("=== RAW STACK (%d bytes from ESP) ===\n", STACK_DUMP_BYTES);
    uintptr_t esp = ctx->Esp;
    for (int i = 0; i < STACK_DUMP_BYTES; i += 16) {
        if (!IsSafeToRead((void*)(esp + i), 16)) break;
        EMIT("  %08X: ", (unsigned)(esp + i));
        for (int j = 0; j < 4; j++) {
            uint32_t val = *(uint32_t*)(esp + i + j * 4);
            EMIT("%08X ", val);
        }
        // ASCII column
        EMIT(" ");
        const uint8_t* p = (const uint8_t*)(esp + i);
        for (int j = 0; j < 16; j++) {
            char c = (p[j] >= 0x20 && p[j] < 0x7F) ? (char)p[j] : '.';
            EMIT("%c", c);
        }
        EMIT("\n");
    }
    EMIT("\n");

    // ─── Return Address Scan ──────────────────────────────────────────
    EMIT("=== RETURN ADDRESS SCAN (all modules) ===\n");
    int found = 0;
    for (int i = 0; i < STACK_DUMP_BYTES && found < 30; i += 4) {
        if (!IsSafeToRead((void*)(esp + i), 4)) break;
        uintptr_t val = *(uintptr_t*)(esp + i);
        if (val < 0x10000 || val >= 0x80000000u) continue;

        HMODULE hValMod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)val, &hValMod)) {
            char modFull[MAX_PATH];
            GetModuleFileNameA(hValMod, modFull, sizeof(modFull));
            const char* name = strrchr(modFull, '\\');
            name = name ? name + 1 : modFull;
            uintptr_t base = (uintptr_t)hValMod;
            EMIT("  ESP+%04X: 0x%08X  [%s+0x%08X]\n",
                 i, (unsigned)val, name, (unsigned)(val - base));
            found++;
        }
    }
    EMIT("\n");

    // ─── Breadcrumbs ──────────────────────────────────────────────────
    EMIT("=== BREADCRUMBS (last %d, newest first) ===\n", MAX_CRUMBS);
    int head = s_CrumbHead.load();
    int printed = 0;
    for (int i = 0; i < MAX_CRUMBS; i++) {
        int idx = ((head - 1 - i) % MAX_CRUMBS + MAX_CRUMBS) % MAX_CRUMBS;
        if (s_Crumbs[idx].Label[0] == '\0') continue;
        float secAgo = (GetTickCount() - s_Crumbs[idx].Timestamp) / 1000.0f;
        EMIT("  [%2d] -%7.3fs  tid=%5lu  %s\n",
             printed, secAgo, s_Crumbs[idx].ThreadId, s_Crumbs[idx].Label);
        printed++;
    }
    if (printed == 0) EMIT("  (none recorded)\n");
    EMIT("\n");

    // ─── Thread List ──────────────────────────────────────────────────
    EMIT("=== THREADS ===\n");
    HANDLE hSnapT = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapT != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = { sizeof(THREADENTRY32) };
        DWORD pid = GetCurrentProcessId();
        DWORD crashTid = GetCurrentThreadId();
        if (Thread32First(hSnapT, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                const char* marker = (te.th32ThreadID == crashTid) ? " <<<< CRASH" : "";
                EMIT("  TID=%5lu  Priority=%2ld%s\n", te.th32ThreadID, te.tpBasePri, marker);
            } while (Thread32Next(hSnapT, &te));
        }
        CloseHandle(hSnapT);
    }
    EMIT("\n");

    // ─── Loaded Modules ───────────────────────────────────────────────
    EMIT("=== LOADED MODULES ===\n");
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me = { sizeof(MODULEENTRY32) };
        if (Module32First(hSnap, &me)) {
            do {
                EMIT("  0x%08X - 0x%08X  %6u KB  %s\n",
                     (unsigned)(uintptr_t)me.modBaseAddr,
                     (unsigned)((uintptr_t)me.modBaseAddr + me.modBaseSize),
                     me.modBaseSize / 1024,
                     me.szModule);
            } while (Module32Next(hSnap, &me));
        }
        CloseHandle(hSnap);
    }
    EMIT("\n");

    // ─── Memory Info ──────────────────────────────────────────────────
    EMIT("=== MEMORY ===\n");
    MEMORYSTATUSEX memStat = { sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memStat)) {
        EMIT("Physical:   %llu MB used / %llu MB total (%lu%% load)\n",
             (memStat.ullTotalPhys - memStat.ullAvailPhys) / (1024*1024),
             memStat.ullTotalPhys / (1024*1024),
             memStat.dwMemoryLoad);
        EMIT("Virtual:    %llu MB used / %llu MB total\n",
             (memStat.ullTotalVirtual - memStat.ullAvailVirtual) / (1024*1024),
             memStat.ullTotalVirtual / (1024*1024));
    }

    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        EMIT("Working:    %lu MB  (peak %lu MB)\n",
             (unsigned long)(pmc.WorkingSetSize / (1024*1024)),
             (unsigned long)(pmc.PeakWorkingSetSize / (1024*1024)));
        EMIT("Private:    %lu MB\n", (unsigned long)(pmc.PrivateUsage / (1024*1024)));
    }
    EMIT("\n");

    // ─── All-Thread Stack Traces ─────────────────────────────────────
    EMIT("=== ALL THREAD STACKS ===\n");
    __try {
        WriteAllThreadStacks(f, GetCurrentThreadId());
        if (fDev) WriteAllThreadStacks(fDev, GetCurrentThreadId());
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        EMIT("  (exception during all-thread stack walk)\n");
    }
    EMIT("\n");

    // ─── Crash Info Providers ────────────────────────────────────────
    for (int i = 0; i < s_ProviderCount; i++) {
        EMIT("=== %s ===\n", s_Providers[i].name);
        __try {
            s_Providers[i].fn(f, fDev);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            EMIT("  (exception in provider '%s')\n", s_Providers[i].name);
        }
        EMIT("\n");
    }

    // ─── Dump Paths ───────────────────────────────────────────────────
    EMIT("╔══════════════════════════════════════════════════════════════╗\n");
    EMIT("║  Report:     %-46s ║\n", s_CrashReportPath);
    EMIT("║  Minidump:   %-46s ║\n", s_MiniDumpPath);
    EMIT("║  Full dump:  %-46s ║\n", s_FullDumpPath);
    EMIT("╚══════════════════════════════════════════════════════════════╝\n");

    #undef EMIT

    fclose(f);
    if (fDev) fclose(fDev);

    // ─── Minidump (normal — small, fast) ──────────────────────────────
    HANDLE hFile = CreateFileA(s_MiniDumpPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          hFile, MiniDumpNormal, &mei, NULL, NULL);
        CloseHandle(hFile);
    }

    // ─── Full minidump (larger — includes heap, handles, thread info) ─
    hFile = CreateFileA(s_FullDumpPath, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules |
            MiniDumpWithIndirectlyReferencedMemory);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          hFile, dumpType, &mei, NULL, NULL);
        CloseHandle(hFile);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  VEH + UEF Handlers
// ═══════════════════════════════════════════════════════════════════════════

static bool IsFatalException(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ||
           code == EXCEPTION_STACK_OVERFLOW ||
           code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
           code == EXCEPTION_FLT_DIVIDE_BY_ZERO ||
           code == EXCEPTION_FLT_INVALID_OPERATION ||
           code == EXCEPTION_PRIV_INSTRUCTION ||
           code == EXCEPTION_IN_PAGE_ERROR ||
           code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
           code == EXCEPTION_GUARD_PAGE;
}

static LONG WINAPI CrashVEH(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsFatalException(code)) return EXCEPTION_CONTINUE_SEARCH;

    // Skip if we're inside IsSafeToRead — those exceptions are SEH-handled
    if (t_ProbingDepth > 0) return EXCEPTION_CONTINUE_SEARCH;

    // Skip during intentional memory probing phases (FindEngineGlobals etc.)
    if (strncmp(s_Context, "init:", 5) == 0) return EXCEPTION_CONTINUE_SEARCH;

    // Write crash report once
    if (!s_ReportWritten.exchange(true)) {
        WriteCrashReport(ep);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// Backstop: SetUnhandledExceptionFilter catches what VEH might miss
static LONG WINAPI CrashUEF(EXCEPTION_POINTERS* ep)
{
    if (!s_ReportWritten.exchange(true)) {
        WriteCrashReport(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Install / Remove
// ═══════════════════════════════════════════════════════════════════════════

void InstallCrashHandler()
{
    InitCrashPaths();
    memset(s_Crumbs, 0, sizeof(s_Crumbs));

    // VEH — first responder, fires before SEH
    s_VEH = AddVectoredExceptionHandler(1, CrashVEH);

    // UEF — backstop, fires if nothing else handles the exception
    s_PrevUEF = SetUnhandledExceptionFilter(CrashUEF);

    if (s_VEH) {
        LOG_INFO("[CrashHandler] VEH + UEF crash handler installed");
        LOG_INFO("[CrashHandler] Reports → {}", s_CrashDir);
        LOG_INFO("[CrashHandler] Breadcrumb buffer: {} slots", MAX_CRUMBS);
    }
}

void RemoveCrashHandler()
{
    if (s_VEH) {
        RemoveVectoredExceptionHandler(s_VEH);
        s_VEH = nullptr;
    }
    if (s_PrevUEF) {
        SetUnhandledExceptionFilter(s_PrevUEF);
        s_PrevUEF = nullptr;
    }
}

} // namespace bs1sdk
