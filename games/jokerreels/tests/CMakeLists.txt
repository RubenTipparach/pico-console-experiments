# Host side tests for Joker Reels, discovered generically by the top level
# BUILD_ENGINE_TESTS block. The sim is pure integer C++, so what it promises
# (the scored facet is the one under the payline, a facet only ever changes
# symbol out of sight, a hands off spin ends) is proven here rather than
# asserted in a comment.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/texture.cmake)

add_executable(jokerreels_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(jokerreels_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(jokerreels_sim_tests pse-core)
add_test(NAME jokerreels_sim_tests COMMAND jokerreels_sim_tests)

# The preview harness renders real frames through the real engine, the real
# models and the real textures, with no SDK anywhere. Not a test of the rules:
# a tool for looking at the machine, which is the only way the geometry gets
# checked at all.
#
# It builds the game's own render.cpp, so it needs the game's own model and
# texture lists. Keeping a second copy here is what left a header ungenerated
# and main red the last time somebody tried.
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../textures.cmake)

set(jokerreels_generated "")
foreach(model ${jokerreels_model_files})
    add_obj_model(jokerreels
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} jokerreels_generated)
endforeach()
foreach(picture ${jokerreels_texture_files})
    add_texture(jokerreels
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/${picture} jokerreels_generated)
endforeach()

add_executable(jokerreels_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${jokerreels_generated}
)
target_include_directories(jokerreels_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jokerreels_preview pse-core)
