# External dependencies — fetched automatically via FetchContent
include(FetchContent)

# ─── MinHook ─────────────────────────────────────────────────────────────
# Inline hooking library for x86/x64
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(minhook)

# ─── Dear ImGui ──────────────────────────────────────────────────────────
# Immediate mode GUI for debug overlay
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ImGui doesn't have a CMakeLists.txt, so we build it manually
if(NOT TARGET imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui PUBLIC 
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PRIVATE d3d9 d3d11 dxgi)
endif()

# ─── Lua 5.4 ─────────────────────────────────────────────────────────────
# Embedded scripting for mod support
FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
)
FetchContent_MakeAvailable(lua)

# Lua doesn't have CMakeLists.txt - build it as a static library
if(NOT TARGET lua_static)
    file(GLOB LUA_CORE_SRCS "${lua_SOURCE_DIR}/src/*.c")
    # Remove lua.c and luac.c (standalone interpreters)
    list(FILTER LUA_CORE_SRCS EXCLUDE REGEX "lua\\.c$")
    list(FILTER LUA_CORE_SRCS EXCLUDE REGEX "luac\\.c$")
    
    add_library(lua_static STATIC ${LUA_CORE_SRCS})
    target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)
    # Compile as C
    set_target_properties(lua_static PROPERTIES LINKER_LANGUAGE C)
endif()

# ─── Future dependencies ─────────────────────────────────────────────────
# toml++ — will be added when config system is expanded
