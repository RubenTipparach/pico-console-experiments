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
    src/parallel.cpp
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
