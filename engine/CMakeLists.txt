# The engine is split in two on purpose.
#
# `pse-core` is pure C++ with no SDK dependency: the rasterizer,
# the projector, the mesh format. It compiles for the device, for desktop, for
# Emscripten, and for a plain host compiler running the tests.
#
# `pse-engine` adds the single translation unit that knows the 32blit
# SDK exists. A 2D game that never touches the 3D renderer still links this, but
# it is one small file, and splitting further would be interface segregation
# taken past the point of being useful.

# The console's font is a picture file, not a hex table (rule 11's reasoning
# applied to glyphs). tools/gen_font.py packs it at configure time into the
# build tree, where every generated file in this repo lives.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/python.cmake)

set(PSE_FONT_ART ${CMAKE_CURRENT_SOURCE_DIR}/font/console5x7.txt)
set(PSE_FONT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PSE_FONT_ART})
execute_process(
    COMMAND ${PSE_PYTHON3}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_font.py
            --art ${PSE_FONT_ART} --out ${PSE_FONT_DIR}
    RESULT_VARIABLE pse_font_result)
if(NOT pse_font_result EQUAL 0)
    message(FATAL_ERROR "gen_font.py failed: the console cannot draw a name it "
                        "has no letters for")
endif()

add_library(pse-core STATIC
    src/raster.cpp
    src/renderer3d.cpp
    src/quat.cpp
    src/parallel.cpp
    src/draw2d.cpp
    src/text.cpp
    # Its own translation unit on purpose: a 2D game that never asks for the
    # shared rasterizer never links its 30 KB of scratch space, because an
    # archive member nothing references is never pulled in.
    src/shared_render.cpp
)

target_include_directories(pse-core PRIVATE ${PSE_FONT_DIR})

# run_split has one implementation per platform class. On the pico targets it
# hands the bottom half of the screen to core 1; everywhere else it runs both
# halves sequentially. PICO_BOARD is only defined when configuring with the
# pico toolchain, so host tests, desktop, and Emscripten all take the host
# path.
#
# Only the multicore HEADERS are linked. pico_multicore itself is an SDK
# INTERFACE library, so linking it into this static library makes CMake
# compile its multicore.c right here, where the pico_runtime_init include
# path does not exist: that path travels with pico_stdlib, which only the
# final executables link. Those executables already receive the multicore
# implementation through 32blit's BlitHalPico, which links pico_multicore,
# so the headers are genuinely all that is missing at this level.
if(PICO_BOARD OR PICO_ADDON)
    target_sources(pse-core PRIVATE src/parallel_pico.cpp)
    target_link_libraries(pse-core PRIVATE pico_multicore_headers)
else()
    target_sources(pse-core PRIVATE src/parallel_host.cpp)
endif()

target_include_directories(pse-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# How big the shared depth buffer is, for pse/shared_render.hpp.
#
# The largest window any game in THIS build draws, read from each game.yml's
# `render:` block. Per build and not per repo on purpose: a standalone game
# that draws the default square pays exactly what it always paid, and only a
# build that actually contains a wider game grows to fit it. That is still far
# less than the wider game carrying a second buffer of its own, which is what
# it did before.
if(PSE_CONSOLE)
    set(pse_arena_args --console)
elseif(DEFINED PICO_ONLY_GAME)
    set(pse_arena_args --only-game ${PICO_ONLY_GAME})
else()
    set(pse_arena_args "")
endif()
# Reconfigure when a game changes the window it asks for. console.yaml is
# deliberately NOT listed here: cmake/console.cmake already carries it, and
# naming the same file from two directories with two spellings makes ninja
# report two rules generating it and refuse to build at all.
file(GLOB pse_arena_inputs CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/../games/*/game.yml)
execute_process(
    COMMAND ${PSE_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/depth_arena.py
            ${pse_arena_args}
    OUTPUT_VARIABLE PSE_DEPTH_ARENA_BYTES
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE pse_arena_result)
if(NOT pse_arena_result EQUAL 0 OR NOT PSE_DEPTH_ARENA_BYTES MATCHES "^[0-9]+$")
    message(FATAL_ERROR "depth_arena.py did not report a size: a 3D game "
                        "would get no depth buffer and draw nothing")
endif()
message(STATUS "Shared depth buffer: ${PSE_DEPTH_ARENA_BYTES} bytes")
target_compile_definitions(pse-core PUBLIC
                           PSE_DEPTH_ARENA_BYTES=${PSE_DEPTH_ARENA_BYTES})

# Which board this is, for pse/board.hpp.
#
# Read from PICO_BOARD rather than from the SDK's own BLIT_BOARD_* define,
# because that one travels with BlitHalPico and the engine deliberately does
# not link it (see pico_multicore_headers above for the same distinction).
# PUBLIC so pse-engine and the games compile against the same answer: a header
# whose constants mean one thing in one translation unit and another in the
# next is worse than no header.
if(PICO_BOARD STREQUAL "pimoroni_tufty2350")
    target_compile_definitions(pse-core PUBLIC PSE_BOARD_TUFTY2350=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pse-core PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

# Host tests link the core alone and stop here.
if(BUILD_ENGINE_TESTS)
    return()
endif()

add_library(pse-engine STATIC
    src/blit_target.cpp
)

target_link_libraries(pse-engine PUBLIC pse-core BlitEngine)

target_include_directories(pse-engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
