#pragma once

#include <string>
#include <cstdint>

namespace bs1sdk {

// ─── Crash Handler ───────────────────────────────────────────────────────
// Installs a Vectored Exception Handler (VEH) that catches access violations
// and other fatal exceptions. Dumps diagnostic info to debug_dumps/crash_report.txt
// before the process dies.

/// Install the crash handler. Call early in init.
void InstallCrashHandler();

/// Remove the crash handler.
void RemoveCrashHandler();

/// Log a breadcrumb — when crash happens, last N breadcrumbs are dumped.
/// Call this from hot paths (tick, PE hook) to trace what was happening.
void CrashBreadcrumb(const char* label);

/// Set the current context for crash reports
void CrashSetContext(const char* context);

/// Get crash report path
const char* GetCrashReportPath();

/// Check if a pointer is safely readable (no exception)
bool IsSafeToRead(const void* ptr, size_t len = 4);

} // namespace bs1sdk
