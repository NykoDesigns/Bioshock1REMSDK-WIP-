#include "lua_bridge.h"
#include "../engine/uobject.h"
#include "../engine/function_caller.h"
#include "../hooks/process_event.h"
#include "../gameplay/teleport_plasmid.h"
#include "../core/log.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <Windows.h>
#include <vector>
#include <string>
#include <mutex>
#include <filesystem>

namespace bs1sdk {

static lua_State* L = nullptr;
static bool s_LuaInit = false;

// ─── Script directory ────────────────────────────────────────────────────

std::string GetScriptsDir()
{
    // First check: scripts dir next to the DLL itself (development)
    HMODULE hSelf = nullptr;
    char dllPath[MAX_PATH] = {};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&GetScriptsDir), &hSelf);
    if (hSelf) {
        GetModuleFileNameA(hSelf, dllPath, MAX_PATH);
        std::string dllDir = std::filesystem::path(dllPath).parent_path().string();
        std::string devDir = dllDir + "\\BS1SDK_Scripts";
        if (std::filesystem::exists(devDir)) return devDir;
    }

    // Second: next to game executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir = std::filesystem::path(exePath).parent_path().string();
    return exeDir + "\\BS1SDK_Scripts";
}

// ─── Lua API: sdk.log(msg) ───────────────────────────────────────────────

static int lua_sdk_log(lua_State* L)
{
    const char* msg = luaL_checkstring(L, 1);
    LOG_INFO("[Lua] {}", msg);
    return 0;
}

// ─── Lua API: sdk.getProperty(className, propName) ───────────────────────

static int lua_sdk_getProperty(lua_State* L)
{
    const char* className = luaL_checkstring(L, 1);
    const char* propName = luaL_checkstring(L, 2);

    UObject* obj = FindObjectByClassName(className);
    if (!obj) {
        lua_pushnil(L);
        lua_pushstring(L, "object not found");
        return 2;
    }

    UStruct* cls = FindClass(className);
    if (!cls) {
        lua_pushnil(L);
        lua_pushstring(L, "class not found");
        return 2;
    }

    auto props = WalkProperties(cls);
    for (auto& p : props) {
        if (p.Name == propName) {
            const uint8_t* base = reinterpret_cast<const uint8_t*>(obj);
            if (p.TypeName == "IntProperty") {
                lua_pushinteger(L, *reinterpret_cast<const int32_t*>(base + p.Offset));
            } else if (p.TypeName == "FloatProperty") {
                lua_pushnumber(L, *reinterpret_cast<const float*>(base + p.Offset));
            } else if (p.TypeName == "BoolProperty") {
                lua_pushboolean(L, *reinterpret_cast<const uint32_t*>(base + p.Offset) != 0);
            } else if (p.TypeName == "ByteProperty") {
                lua_pushinteger(L, *(base + p.Offset));
            } else {
                lua_pushnil(L);
            }
            return 1;
        }
    }

    lua_pushnil(L);
    lua_pushstring(L, "property not found");
    return 2;
}

// ─── Lua API: sdk.setProperty(className, propName, value) ────────────────

static int lua_sdk_setProperty(lua_State* L)
{
    const char* className = luaL_checkstring(L, 1);
    const char* propName = luaL_checkstring(L, 2);

    UObject* obj = FindObjectByClassName(className);
    if (!obj) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "object not found");
        return 2;
    }

    UStruct* cls = FindClass(className);
    if (!cls) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "class not found");
        return 2;
    }

    auto props = WalkProperties(cls);
    for (auto& p : props) {
        if (p.Name == propName) {
            if (p.TypeName == "IntProperty") {
                int32_t val = (int32_t)luaL_checkinteger(L, 3);
                obj->SetField<int32_t>(p.Offset, val);
            } else if (p.TypeName == "FloatProperty") {
                float val = (float)luaL_checknumber(L, 3);
                obj->SetField<float>(p.Offset, val);
            } else if (p.TypeName == "BoolProperty") {
                uint32_t val = lua_toboolean(L, 3) ? 1 : 0;
                obj->SetField<uint32_t>(p.Offset, val);
            } else {
                lua_pushboolean(L, 0);
                lua_pushstring(L, "unsupported type for set");
                return 2;
            }
            lua_pushboolean(L, 1);
            return 1;
        }
    }

    lua_pushboolean(L, 0);
    lua_pushstring(L, "property not found");
    return 2;
}

// ─── Lua API: sdk.callFunction(className, funcName, ...) ─────────────────

static int lua_sdk_callFunction(lua_State* L)
{
    const char* className = luaL_checkstring(L, 1);
    const char* funcName = luaL_checkstring(L, 2);

    UObject* obj = FindObjectByClassName(className);
    if (!obj) {
        lua_pushnil(L);
        lua_pushstring(L, "object not found");
        return 2;
    }

    UStruct* cls = reinterpret_cast<UStruct*>(obj->GetClass());
    UFunction* func = FindFunction(cls, funcName);
    if (!func) {
        lua_pushnil(L);
        lua_pushstring(L, "function not found");
        return 2;
    }

    // Gather args from Lua stack (index 3+)
    int nargs = lua_gettop(L) - 2;
    std::vector<std::string> args;
    for (int i = 0; i < nargs; i++) {
        if (lua_isnumber(L, i + 3)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", lua_tonumber(L, i + 3));
            args.push_back(buf);
        } else if (lua_isstring(L, i + 3)) {
            args.push_back(lua_tostring(L, i + 3));
        } else if (lua_isboolean(L, i + 3)) {
            args.push_back(lua_toboolean(L, i + 3) ? "true" : "false");
        } else {
            args.push_back("0");
        }
    }

    std::string result = CallFunction(obj, func, args);
    lua_pushstring(L, result.c_str());
    return 1;
}

// ─── Lua API: sdk.findObjects(filter) ────────────────────────────────────

static int lua_sdk_findObjects(lua_State* L)
{
    const char* filter = luaL_checkstring(L, 1);
    auto& globals = GetEngineGlobals();
    if (!globals.IsValid()) { lua_newtable(L); return 1; }

    uintptr_t objData = *reinterpret_cast<uintptr_t*>(globals.GObjects);
    int32_t objCount = *reinterpret_cast<int32_t*>(globals.GObjects + 4);

    lua_newtable(L);
    int idx = 1;
    std::string f = filter;

    for (int i = 0; i < objCount && idx <= 50; i++) {
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(objData + i * 4);
        if (!ptr) continue;
        UObject* obj = reinterpret_cast<UObject*>(ptr);
        std::string cn = obj->GetObjClassName();
        std::string on = obj->GetName();

        bool match = false;
        for (size_t c = 0; c + f.size() <= cn.size(); c++) {
            if (_strnicmp(cn.c_str() + c, f.c_str(), f.size()) == 0) { match = true; break; }
        }
        if (!match) {
            for (size_t c = 0; c + f.size() <= on.size(); c++) {
                if (_strnicmp(on.c_str() + c, f.c_str(), f.size()) == 0) { match = true; break; }
            }
        }
        if (!match) continue;

        lua_newtable(L);
        lua_pushstring(L, on.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, cn.c_str()); lua_setfield(L, -2, "class");
        lua_pushinteger(L, (lua_Integer)ptr); lua_setfield(L, -2, "address");
        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

// ─── Lua API: sdk.on(funcName, callback) ─── ProcessEvent hook ───────────

struct LuaEventHook {
    int callbackRef;
    int hookId;
};
static std::vector<LuaEventHook> s_LuaEventHooks;

static int lua_sdk_on(lua_State* L)
{
    const char* funcFilter = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (!IsProcessEventHooked()) {
        InitProcessEventHook();
    }

    // Store the callback reference
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Register ProcessEvent hook
    ProcessEventHook hook;
    hook.Name = std::string("Lua:") + funcFilter;
    hook.FunctionFilter = funcFilter;
    lua_State* luaState = L;  // capture-friendly local copy
    hook.Callback = [ref, luaState](UObject* obj, UFunction* func, void* parms) -> bool {
        if (!luaState) return false;
        lua_rawgeti(luaState, LUA_REGISTRYINDEX, ref);
        lua_pushstring(luaState, obj->GetName().c_str());
        lua_pushstring(luaState, func->GetName().c_str());
        
        int err = lua_pcall(luaState, 2, 1, 0);
        if (err != LUA_OK) {
            LOG_ERROR("[Lua] on() callback error: {}", lua_tostring(luaState, -1));
            lua_pop(luaState, 1);
            return false;
        }
        
        bool block = lua_toboolean(luaState, -1) != 0;
        lua_pop(luaState, 1);
        return block;
    };

    int hookId = RegisterProcessEventHook(hook);
    s_LuaEventHooks.push_back({ref, hookId});

    lua_pushinteger(L, hookId);
    return 1;
}

// ─── Lua API: sdk.off(hookId) ── remove event hook ──────────────────────

static int lua_sdk_off(lua_State* L)
{
    int hookId = (int)luaL_checkinteger(L, 1);
    UnregisterProcessEventHook(hookId);

    // Remove from our tracking and unref
    for (auto it = s_LuaEventHooks.begin(); it != s_LuaEventHooks.end(); ++it) {
        if (it->hookId == hookId) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->callbackRef);
            s_LuaEventHooks.erase(it);
            break;
        }
    }

    return 0;
}

// ─── Lua API: sdk.hookpe() ──────────────────────────────────────────────

static int lua_sdk_hookpe(lua_State* L)
{
    if (!IsProcessEventHooked()) {
        bool ok = InitProcessEventHook();
        lua_pushboolean(L, ok);
    } else {
        lua_pushboolean(L, 1);
    }
    return 1;
}

// ─── Lua API: sdk.teleport(distance) ─────────────────────────────────────

static int lua_sdk_teleport(lua_State* L)
{
    float dist = 800.0f;
    if (lua_gettop(L) >= 1) dist = (float)luaL_checknumber(L, 1);
    bool ok = DoTeleport(dist);
    lua_pushboolean(L, ok);
    return 1;
}

// ─── Registration ────────────────────────────────────────────────────────

static const luaL_Reg sdk_funcs[] = {
    {"log", lua_sdk_log},
    {"getProperty", lua_sdk_getProperty},
    {"setProperty", lua_sdk_setProperty},
    {"callFunction", lua_sdk_callFunction},
    {"findObjects", lua_sdk_findObjects},
    {"on", lua_sdk_on},
    {"off", lua_sdk_off},
    {"hookpe", lua_sdk_hookpe},
    {"teleport", lua_sdk_teleport},
    {nullptr, nullptr}
};

// ─── Public API ─────────────────────────────────────────────────────────

bool InitLuaEngine()
{
    if (s_LuaInit) return true;

    L = luaL_newstate();
    if (!L) {
        LOG_ERROR("Failed to create Lua state");
        return false;
    }

    luaL_openlibs(L);

    // Register sdk table
    luaL_newlib(L, sdk_funcs);
    lua_setglobal(L, "sdk");

    s_LuaInit = true;
    LOG_INFO("Lua engine initialized (Lua 5.4)");

    // Create scripts directory if it doesn't exist
    std::string scriptsDir = GetScriptsDir();
    std::filesystem::create_directories(scriptsDir);
    LOG_INFO("Scripts dir: {}", scriptsDir);

    // Auto-execute autorun.lua if it exists
    std::string autorun = scriptsDir + "\\autorun.lua";
    if (std::filesystem::exists(autorun)) {
        std::string err = LuaExecFile(autorun);
        if (!err.empty()) {
            LOG_ERROR("autorun.lua error: {}", err);
        } else {
            LOG_INFO("autorun.lua executed successfully");
        }
    }

    return true;
}

void ShutdownLuaEngine()
{
    if (!s_LuaInit) return;

    // Unregister all Lua event hooks
    for (auto& lh : s_LuaEventHooks) {
        UnregisterProcessEventHook(lh.hookId);
        luaL_unref(L, LUA_REGISTRYINDEX, lh.callbackRef);
    }
    s_LuaEventHooks.clear();

    lua_close(L);
    L = nullptr;
    s_LuaInit = false;
    LOG_INFO("Lua engine shut down");
}

std::string LuaExec(const std::string& code)
{
    if (!L) return "Lua not initialized";

    int err = luaL_dostring(L, code.c_str());
    if (err != LUA_OK) {
        std::string msg = lua_tostring(L, -1);
        lua_pop(L, 1);
        return msg;
    }

    // Check if there's a return value on the stack
    if (lua_gettop(L) > 0) {
        const char* result = lua_tostring(L, -1);
        std::string ret = result ? result : "";
        lua_pop(L, lua_gettop(L));
        return ret.empty() ? "" : "= " + ret;
    }

    return "";
}

std::string LuaExecFile(const std::string& path)
{
    if (!L) return "Lua not initialized";

    int err = luaL_dofile(L, path.c_str());
    if (err != LUA_OK) {
        std::string msg = lua_tostring(L, -1);
        lua_pop(L, 1);
        return msg;
    }
    return "";
}

bool IsLuaInitialized()
{
    return s_LuaInit;
}

std::string LuaReload()
{
    std::string autorun = GetScriptsDir() + "\\autorun.lua";
    if (!std::filesystem::exists(autorun))
        return "No autorun.lua found in " + GetScriptsDir();

    return LuaExecFile(autorun);
}

} // namespace bs1sdk
