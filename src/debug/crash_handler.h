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

} // namespace bs1sdk
