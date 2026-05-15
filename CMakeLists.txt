cmake_minimum_required(VERSION 3.20)
project(BS1SDK VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static runtime so users don't need VC++ Redistributable installed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Force 32-bit build (BioShock Remastered is x86)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BS1SDK must be built as 32-bit (x86). Use -A Win32 with CMake generator.")
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Game directory for auto-deploy after build (set to your local install)
set(BS1SDK_GAME_DIR "D:/SteamLibrary/steamapps/common/BioShock Remastered/Build/Final"
    CACHE PATH "BioShock Remastered game directory for auto-deploy")

# Options
option(BS1SDK_BUILD_INJECTOR "Build the DLL injector" ON)
option(BS1SDK_BUILD_TOOLS "Build standalone tools (bsm_tool, etc.)" ON)
option(BS1SDK_BUILD_TESTS "Build unit tests" ON)

# External dependencies
add_subdirectory(external)

# Main SDK DLL
add_subdirectory(src)

# Proxy loader (winmm.dll — drop-in auto-loader, no injector needed)
add_subdirectory(src/loader)

# Injector
if(BS1SDK_BUILD_INJECTOR)
    add_subdirectory(injector)
endif()

# Tools
if(BS1SDK_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# Tests
if(BS1SDK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
