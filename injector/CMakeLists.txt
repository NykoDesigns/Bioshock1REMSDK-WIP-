# Simple DLL Injector
add_executable(BS1Injector injector.cpp)

target_compile_definitions(BS1Injector PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Injector can be either 32 or 64 bit, but target process is 32-bit
# For simplicity, build as 32-bit alongside the SDK
