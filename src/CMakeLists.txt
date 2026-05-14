# BS1SDK Main DLL
add_library(BS1SDK SHARED
    core/dllmain.cpp
    core/hooks.cpp
    core/memory.cpp
    core/pattern.cpp
    core/config.cpp
    core/log.cpp
    core/mod_config.cpp
    hooks/d3d9_hook.cpp
    hooks/process_event.cpp
    render/overlay.cpp
    engine/engine_globals.cpp
    engine/function_caller.cpp
    engine/class_factory.cpp
    sdk/sdk_generator.cpp
    scripting/lua_bridge.cpp
    gameplay/teleport_plasmid.cpp
    gameplay/gameplay_mods.cpp
    network/udp_socket.cpp
    network/net_manager.cpp
    network/coop_bridge.cpp
    network/coop_render.cpp
    network/coop_sync.cpp
    network/coop_puppet.cpp
    network/coop_economy.cpp
)

target_include_directories(BS1SDK PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(BS1SDK PRIVATE
    minhook
    imgui
    lua_static
    d3d9
    psapi
    ws2_32
)

target_compile_definitions(BS1SDK PRIVATE
    BS1SDK_EXPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Disable SAFESEH for hook compatibility
if(MSVC)
    target_link_options(BS1SDK PRIVATE /SAFESEH:NO)
endif()
