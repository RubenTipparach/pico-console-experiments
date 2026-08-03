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

add_library(pse-core STATIC
    src/raster.cpp
    src/renderer3d.cpp
)

target_include_directories(pse-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(pse-core PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

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
