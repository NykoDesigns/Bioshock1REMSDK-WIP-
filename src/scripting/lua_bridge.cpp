#include "lua_bridge.h"
#include "../engine/uobject.h"
#include "../engine/function_caller.h"
#include "../engine/world.h"
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

// ─── World/Actor Lua bindings ────────────────────────────────────────────

// sdk.getActors([className]) -> table of {address, name, class}
static int lua_sdk_getActors(lua_State* L)
{
    const char* classFilter = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
    
    std::vector<UObject*> actors;
    if (classFilter)
        actors = GetActorsOfClass(classFilter);
    else
        actors = GetAllActors();
    
    lua_newtable(L);
    int idx = 1;
    for (auto* actor : actors) {
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)(uintptr_t)actor);
        lua_setfield(L, -2, "address");
        lua_pushstring(L, actor->GetName().c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, actor->GetObjClassName().c_str());
        lua_setfield(L, -2, "class");
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

// sdk.getActorPosition(address) -> x, y, z
static int lua_sdk_getActorPosition(lua_State* L)
{
    uintptr_t addr = (uintptr_t)luaL_checkinteger(L, 1);
    UObject* actor = reinterpret_cast<UObject*>(addr);
    
    FVec3 pos;
    if (GetActorPosition(actor, pos)) {
        lua_pushnumber(L, pos.X);
        lua_pushnumber(L, pos.Y);
        lua_pushnumber(L, pos.Z);
        return 3;
    }
    return 0;
}

// sdk.setActorPosition(address, x, y, z)
static int lua_sdk_setActorPosition(lua_State* L)
{
    uintptr_t addr = (uintptr_t)luaL_checkinteger(L, 1);
    UObject* actor = reinterpret_cast<UObject*>(addr);
    
    FVec3 pos;
    pos.X = (float)luaL_checknumber(L, 2);
    pos.Y = (float)luaL_checknumber(L, 3);
    pos.Z = (float)luaL_checknumber(L, 4);
    
    lua_pushboolean(L, SetActorPosition(actor, pos));
    return 1;
}

// sdk.getActorsInRadius(x, y, z, radius, [classFilter]) -> table
static int lua_sdk_getActorsInRadius(lua_State* L)
{
    FVec3 center;
    center.X = (float)luaL_checknumber(L, 1);
    center.Y = (float)luaL_checknumber(L, 2);
    center.Z = (float)luaL_checknumber(L, 3);
    float radius = (float)luaL_checknumber(L, 4);
    const char* classFilter = lua_isnoneornil(L, 5) ? "" : luaL_checkstring(L, 5);
    
    auto actors = GetActorsInRadius(center, radius, classFilter);
    
    lua_newtable(L);
    int idx = 1;
    for (auto* actor : actors) {
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)(uintptr_t)actor);
        lua_setfield(L, -2, "address");
        lua_pushstring(L, actor->GetName().c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, actor->GetObjClassName().c_str());
        lua_setfield(L, -2, "class");
        
        FVec3 pos;
        if (GetActorPosition(actor, pos)) {
            lua_pushnumber(L, pos.X);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, pos.Y);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, pos.Z);
            lua_setfield(L, -2, "z");
        }
        
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

// sdk.getPlayerPos() -> x, y, z
static int lua_sdk_getPlayerPos(lua_State* L)
{
    FVec3 pos;
    if (GetPlayerPosition(pos)) {
        lua_pushnumber(L, pos.X);
        lua_pushnumber(L, pos.Y);
        lua_pushnumber(L, pos.Z);
        return 3;
    }
    return 0;
}

// sdk.actorDistance(addr1, addr2) -> float
static int lua_sdk_actorDistance(lua_State* L)
{
    uintptr_t a1 = (uintptr_t)luaL_checkinteger(L, 1);
    uintptr_t a2 = (uintptr_t)luaL_checkinteger(L, 2);
    float dist = GetActorDistance(reinterpret_cast<UObject*>(a1), reinterpret_cast<UObject*>(a2));
    lua_pushnumber(L, dist);
    return 1;
}

// sdk.getLevelInfo() -> {actorCount, levelName}
static int lua_sdk_getLevelInfo(lua_State* L)
{
    LevelInfo info = GetCurrentLevel();
    lua_newtable(L);
    lua_pushinteger(L, info.ActorCount);
    lua_setfield(L, -2, "actorCount");
    lua_pushstring(L, info.LevelName.c_str());
    lua_setfield(L, -2, "name");
    return 1;
}

// ─── CDO (Class Default Object) Lua bindings ────────────────────────────

// sdk.getDefaultObject(className) -> address or nil
static int lua_sdk_getDefaultObject(lua_State* L)
{
    const char* className = luaL_checkstring(L, 1);
    UObject* cdo = GetDefaultObject(className);
    if (cdo) {
        lua_pushinteger(L, (lua_Integer)(uintptr_t)cdo);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// sdk.setDefault(className, propName, value) -> bool
static int lua_sdk_setDefault(lua_State* L)
{
    const char* className = luaL_checkstring(L, 1);
    const char* propName = luaL_checkstring(L, 2);
    
    if (lua_isinteger(L, 3)) {
        int32_t val = (int32_t)lua_tointeger(L, 3);
        lua_pushboolean(L, SetDefaultPropertyInt(className, propName, val));
    } else {
        float val = (float)lua_tonumber(L, 3);
        lua_pushboolean(L, SetDefaultProperty(className, propName, val));
    }
    return 1;
}

// ─── Function introspection Lua bindings ─────────────────────────────────

// sdk.getFunctions(className) -> table of {name, flags, isNative, nativeIdx}
static int lua_sdk_getFunctions(lua_State* L)
{
    const char* className = luaL_checkstring(L, 1);
    UStruct* cls = FindClass(className);
    if (!cls) { lua_pushnil(L); return 1; }
    
    auto funcs = GetClassFunctions(cls);
    
    lua_newtable(L);
    int idx = 1;
    for (auto& fi : funcs) {
        lua_newtable(L);
        lua_pushstring(L, fi.Name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, fi.FunctionFlags);
        lua_setfield(L, -2, "flags");
        
        UFunction* func = fi.Function;
        lua_pushboolean(L, func->IsNative());
        lua_setfield(L, -2, "isNative");
        lua_pushinteger(L, func->GetNativeIndex());
        lua_setfield(L, -2, "nativeIndex");
        lua_pushboolean(L, func->IsEvent());
        lua_setfield(L, -2, "isEvent");
        lua_pushboolean(L, func->IsExec());
        lua_setfield(L, -2, "isExec");
        lua_pushinteger(L, func->GetParmsSize());
        lua_setfield(L, -2, "paramsSize");
        
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

// sdk.getEnum(enumName) -> table of strings or nil
static int lua_sdk_getEnum(lua_State* L)
{
    const char* enumName = luaL_checkstring(L, 1);
    UEnum* uenum = FindEnum(enumName);
    if (!uenum) { lua_pushnil(L); return 1; }
    
    auto names = uenum->GetAllEnumNames();
    lua_newtable(L);
    for (int i = 0; i < (int)names.size(); i++) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ─── Tick callback Lua binding ───────────────────────────────────────────

static int s_LuaTickRef = LUA_NOREF;

// sdk.onTick(function(dt) ... end) -> tickId
static int lua_sdk_onTick(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    
    // Store the function reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    
    // Register tick callback that calls the Lua function
    int tickId = RegisterTickCallback([L, ref](float dt) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            LOG_ERROR("Lua tick error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    });
    
    lua_pushinteger(L, tickId);
    return 1;
}

// sdk.offTick(tickId)
static int lua_sdk_offTick(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    UnregisterTickCallback(id);
    return 0;
}

// sdk.getTickRate() -> float
static int lua_sdk_getTickRate(lua_State* L)
{
    lua_pushnumber(L, GetTickRate());
    return 1;
}

// ─── Native function access Lua bindings ─────────────────────────────────

// sdk.getNativeCount() -> int
static int lua_sdk_getNativeCount(lua_State* L)
{
    lua_pushinteger(L, GetNativeCount());
    return 1;
}

// sdk.getNativeAddress(index) -> integer address or nil
static int lua_sdk_getNativeAddress(lua_State* L)
{
    int idx = (int)luaL_checkinteger(L, 1);
    NativeFunc fn = GetNative((uint16_t)idx);
    if (fn) {
        lua_pushinteger(L, (lua_Integer)(uintptr_t)fn);
    } else {
        lua_pushnil(L);
    }
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
    // World/Actor
    {"getActors", lua_sdk_getActors},
    {"getActorPosition", lua_sdk_getActorPosition},
    {"setActorPosition", lua_sdk_setActorPosition},
    {"getActorsInRadius", lua_sdk_getActorsInRadius},
    {"getPlayerPos", lua_sdk_getPlayerPos},
    {"actorDistance", lua_sdk_actorDistance},
    {"getLevelInfo", lua_sdk_getLevelInfo},
    // CDO
    {"getDefaultObject", lua_sdk_getDefaultObject},
    {"setDefault", lua_sdk_setDefault},
    // Functions / Enums
    {"getFunctions", lua_sdk_getFunctions},
    {"getEnum", lua_sdk_getEnum},
    // Tick
    {"onTick", lua_sdk_onTick},
    {"offTick", lua_sdk_offTick},
    {"getTickRate", lua_sdk_getTickRate},
    // Natives
    {"getNativeCount", lua_sdk_getNativeCount},
    {"getNativeAddress", lua_sdk_getNativeAddress},
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
