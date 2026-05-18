#pragma once

#include <cstdint>
#include <cstdio>

namespace bs1sdk {

// ─── Crash Handler ───────────────────────────────────────────────────────
// Installs a Vectored Exception Handler (VEH) that catches access violations
// and other fatal exceptions. Dumps diagnostic info to BS1SDK_dumps/.
// Also installs SetUnhandledExceptionFilter as a backstop.

/// Install the crash handler. Call early in init.
void InstallCrashHandler();

/// Remove the crash handler.
void RemoveCrashHandler();

/// Log a breadcrumb — when crash happens, last N breadcrumbs are dumped.
/// Call this from hot paths (tick, PE hook) to trace what was happening.
void CrashBreadcrumb(const char* label);

/// Formatted breadcrumb (printf-style)
void CrashBreadcrumbf(const char* fmt, ...);

/// Set the current context for crash reports
void CrashSetContext(const char* context);

/// Get crash report path
const char* GetCrashReportPath();

/// Check if a pointer is safely readable (no exception)
bool IsSafeToRead(const void* ptr, size_t len = 4);

/// RAII context guard — sets context on construction, restores on destruction
struct ScopedCrashContext {
    char prev[256];
    ScopedCrashContext(const char* ctx);
    ~ScopedCrashContext();
};

#define CRASH_SCOPE(name) ::bs1sdk::ScopedCrashContext _crash_scope_##__LINE__(name)

// ─── Crash Info Providers ────────────────────────────────────────────────
// Subsystems (co-op, engine patches, etc.) register callbacks that dump
// diagnostic state into the crash report. This avoids the crash handler
// needing headers for every subsystem.

/// Callback that writes diagnostic info to two FILE* streams (game dir + dev dir).
/// Either file may be NULL. Use the CRASH_EMIT macro inside providers.
using CrashInfoProviderFn = void(*)(FILE* f1, FILE* f2);

/// Register a named crash info provider. Call during subsystem init.
/// Max 16 providers. name is NOT copied — must be a static string.
void RegisterCrashInfoProvider(const char* name, CrashInfoProviderFn fn);

/// Macro for use inside CrashInfoProviderFn implementations
#define CRASH_EMIT(f1, f2, ...) do { \
    if (f1) fprintf(f1, __VA_ARGS__); \
    if (f2) fprintf(f2, __VA_ARGS__); \
} while(0)

} // namespace bs1sdk
