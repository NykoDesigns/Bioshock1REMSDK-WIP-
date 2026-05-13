# BSM file analysis and editing tool
add_executable(bsm_tool main.cpp)

target_include_directories(bsm_tool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
