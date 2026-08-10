# Host side tests for Dumb Lander, discovered generically by the top level
# BUILD_ENGINE_TESTS block.
#
# The sim is pure integer C++ with no SDK and no renderer, so its promises are
# proven rather than asserted in comments. The preview renders real frames
# through the real engine and the real sprites, which is both the tool for
# looking at the game without a device and the only place the render side
# promises can be checked.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/texture.cmake)

add_executable(dumblander_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(dumblander_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME dumblander_sim_tests COMMAND dumblander_sim_tests)

# The sprite list comes from the game's own file, never a second copy: the
# preview compiles the same render.cpp the device does, so a picture named for
# one and not the other is a missing header. Rule 11's reasoning, applied to
# 2D art.
include(${CMAKE_CURRENT_LIST_DIR}/../sprites.cmake)

set(dumblander_generated "")
foreach(picture ${dumblander_sprite_files})
    # The SLUG, not the target: render.cpp names models::dumblander::lander, so
    # the generated table has to land in that namespace here too.
    add_sprite(dumblander
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/${picture} dumblander_generated)
endforeach()

add_executable(dumblander_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${dumblander_generated}
)
target_include_directories(dumblander_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(dumblander_preview pse-core)

# Registered, so the render side checks run in CI rather than only when
# somebody happens to run the binary.
add_test(NAME dumblander_preview COMMAND dumblander_preview)
