# winmm.dll proxy loader — auto-loads BS1SDK.dll when placed in game folder
# No injector needed: Windows loads winmm.dll from app dir first.
add_library(winmm_proxy SHARED
    winmm_proxy.cpp
    winmm_proxy.def
)

set_target_properties(winmm_proxy PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
)

target_compile_definitions(winmm_proxy PRIVATE
    NOMINMAX
)

if(MSVC)
    target_link_options(winmm_proxy PRIVATE /SAFESEH:NO)
endif()

# Auto-deploy to game directory after build
if(BS1SDK_GAME_DIR)
    add_custom_command(TARGET winmm_proxy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:winmm_proxy>"
            "${BS1SDK_GAME_DIR}/winmm.dll"
        COMMENT "Deploying winmm.dll -> ${BS1SDK_GAME_DIR}"
    )
endif()
