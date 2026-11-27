# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    if(XBOXRECOMP_GPU_NATIVE)
        target_compile_definitions(xbox_video PRIVATE RECOMP_GPU_NATIVE=1)
    endif()
    target_link_libraries(xbox_video PRIVATE xbox_input SDL2::SDL2 Threads::Threads)
endif()
