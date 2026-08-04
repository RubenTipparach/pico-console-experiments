cmake_minimum_required(VERSION 3.20)

# 3.20 rather than the 3.12 this repo started on: pico-sdk 2.x needs 3.13, and
# CMP0118 (cross directory GENERATED properties) needs 3.20. The generated
# model sources depend on that policy behaving predictably.

project(pico-console-experiments C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

# BUILD_ENGINE_TESTS builds only the host test binary, with no SDK at all. CI
# runs this first so a broken renderer fails in seconds rather than after a
# full cross compile.
option(BUILD_ENGINE_TESTS "Build host tests for the SDK free engine" OFF)

add_subdirectory(engine)

if(BUILD_ENGINE_TESTS)
    enable_testing()
    add_executable(engine_tests engine/tests/test_engine.cpp)
    target_link_libraries(engine_tests pse-core)
    add_test(NAME engine_tests COMMAND engine_tests)

    # Game test suites are discovered, never listed, same as the games
    # themselves: a games/<slug>/tests/CMakeLists.txt is picked up here and its
    # tests run in the same ctest pass CI already executes.
    file(GLOB game_test_dirs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/games/*/tests)
    foreach(test_dir ${game_test_dirs})
        if(EXISTS ${test_dir}/CMakeLists.txt)
            add_subdirectory(${test_dir})
        endif()
    endforeach()

    # The launcher is not a game, but its library scan and its boot vector
    # checks are the parts of this repo with the least room to be wrong.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/launcher/tests/CMakeLists.txt)
        add_subdirectory(launcher/tests)
    endif()
    return()
endif()

find_package(32BLIT CONFIG REQUIRED PATHS ../32blit-sdk $ENV{PATH_32BLIT_SDK})

include(picosystem_game)
include(slot)

# The launcher owns the base of flash and hands the machine to a game in a
# slot. Device only, so it is skipped for the web and desktop builds where
# there is nothing to hand over to. It is built on its own
# (-DPICO_LAUNCHER_ONLY) so a game build does not drag it along.
if(PICO_ON_DEVICE AND (NOT DEFINED PICO_ONLY_GAME OR PICO_LAUNCHER_ONLY))
    add_subdirectory(launcher)
endif()
if(PICO_LAUNCHER_ONLY)
    return()
endif()

# Games are discovered, never listed. Adding a directory with a game.yml and a
# CMakeLists.txt is the whole procedure for adding a game.
file(GLOB game_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/games CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/games/*)
foreach(game ${game_dirs})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/CMakeLists.txt)
        if(NOT DEFINED PICO_ONLY_GAME OR PICO_ONLY_GAME STREQUAL game)
            add_subdirectory(games/${game})
            # A slot build links the game where the launcher expects to find
            # it, for bundling. Without PICO_SLOT the game links at the base
            # of flash and is the standalone .uf2 it has always been.
            if(DEFINED PICO_SLOT AND PICO_ONLY_GAME STREQUAL game)
                pico_link_at_slot(${game} ${PICO_SLOT})
            endif()
        endif()
    endif()
endforeach()
