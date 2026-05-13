# Unit tests
# Using a minimal test approach initially (no external test framework)
# Will add GoogleTest or Catch2 later

# Compile pattern scanner sources directly into test (avoids DLL link issues)
add_executable(test_pattern 
    test_pattern.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pattern.cpp
    ${CMAKE_SOURCE_DIR}/src/core/memory.cpp
)
target_include_directories(test_pattern PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_pattern PRIVATE psapi)
target_compile_definitions(test_pattern PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME PatternScanner COMMAND test_pattern)
