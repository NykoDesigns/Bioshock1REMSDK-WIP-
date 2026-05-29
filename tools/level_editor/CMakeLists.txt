cmake_minimum_required(VERSION 3.20)
project(BS1LevelEditor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ─── Dependencies ────────────────────────────────────────────────────────────
# SDL2
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.28.5
    GIT_SHALLOW ON
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# Dear ImGui (docking branch)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
    GIT_SHALLOW ON
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# Build ImGui as a static library
set(IMGUI_DIR ${imgui_SOURCE_DIR})
add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_include_directories(imgui_lib PUBLIC ${sdl2_SOURCE_DIR}/include)
target_link_libraries(imgui_lib PUBLIC SDL2-static opengl32)

# ─── Editor Sources ──────────────────────────────────────────────────────────
set(EDITOR_SOURCES
    src/main.cpp
    src/app.cpp
    src/app.h
    src/bsm_document.cpp
    src/bsm_document.h
    src/viewport.cpp
    src/viewport.h
    src/scene_tree.cpp
    src/scene_tree.h
    src/properties_panel.cpp
    src/properties_panel.h
    src/camera.cpp
    src/camera.h
    src/gl_funcs.cpp
    src/gl_funcs.h
    src/mesh_parser.cpp
    src/mesh_parser.h
    src/gltf_loader.cpp
    src/bsp_parser.h
    src/bsp_parser.cpp
    src/texture_loader.h
    src/texture_loader.cpp
    src/content_browser.h
    src/content_browser.cpp
    src/thumbnail_renderer.h
    src/thumbnail_renderer.cpp
    src/catalog_parser.h
    src/catalog_parser.cpp
)

add_executable(BS1LevelEditor ${EDITOR_SOURCES})

target_link_libraries(BS1LevelEditor PRIVATE
    imgui_lib
    SDL2-static
    SDL2main
    opengl32
)

target_include_directories(BS1LevelEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${sdl2_SOURCE_DIR}/include
)

# CLI test for BSM parsing (no GUI, no SDL)
add_executable(ParseTest
    src/parse_test.cpp
    src/bsm_document.cpp
    src/mesh_parser.cpp
    src/gltf_loader.cpp
    src/bsp_parser.cpp
)
target_include_directories(ParseTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
