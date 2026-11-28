cmake_minimum_required(VERSION 3.20)
project(input_sdl_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(input_sdl_test test_main.c)
target_link_libraries(input_sdl_test PRIVATE xbox_input)
enable_testing()
add_test(NAME input_sdl_keyboard COMMAND input_sdl_test)
add_test(NAME input_sdl_keyboard_disabled COMMAND input_sdl_test --no-keyboard)
set_tests_properties(input_sdl_keyboard input_sdl_keyboard_disabled PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
add_test(NAME input_sdl_kbm COMMAND input_sdl_test --kbm)
set_tests_properties(input_sdl_kbm PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
