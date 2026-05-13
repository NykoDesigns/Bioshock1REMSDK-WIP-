#pragma once

#include <string>
#include <functional>

namespace bs1sdk {

/// Initialize the Lua scripting engine.
/// Creates a Lua state and registers all SDK bindings.
bool InitLuaEngine();

/// Shutdown and destroy the Lua state.
void ShutdownLuaEngine();

/// Execute a Lua string (returns error message or empty on success).
std::string LuaExec(const std::string& code);

/// Execute a Lua file (returns error message or empty on success).
std::string LuaExecFile(const std::string& path);

/// Check if Lua engine is initialized.
bool IsLuaInitialized();

/// Hot-reload: re-execute the autorun script if it exists.
std::string LuaReload();

/// Get the scripts directory path.
std::string GetScriptsDir();

} // namespace bs1sdk
