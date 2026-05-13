# INI configuration parser and patcher tool
add_executable(ini_tool main.cpp)

target_include_directories(ini_tool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
