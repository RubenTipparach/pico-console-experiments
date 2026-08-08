# Host side tests for Starlance, discovered generically by the top level
# BUILD_ENGINE_TESTS block.
#
# The sim is pure integer C++, so its promises are proven here rather than
# asserted in comments: that a rolled ship still pitches about its own nose,
# that the target cycle walks contacts in view first and then hardpoints, that
# a turret can be shot off a frigate without the hull beside it taking the
# damage, and that killing the navigation array is what stops the jump.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)

add_executable(starlance_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(starlance_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(starlance_sim_tests pse-core)
add_test(NAME starlance_sim_tests COMMAND starlance_sim_tests)

# The models are listed in exactly one place, and both the test binary below
# and the preview harness read that list.
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)

set(starlance_models "")
foreach(model ${starlance_model_files})
    add_obj_model(starlance
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} starlance_models)
endforeach()

# Winding and hardpoint seats, checked against the real generated tables. A
# face wound the wrong way round is invisible at 120 pixels and obvious as a
# negative volume.
add_executable(starlance_model_tests
    test_models.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${starlance_models}
)
target_include_directories(starlance_model_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(starlance_model_tests pse-core)
add_test(NAME starlance_model_tests COMMAND starlance_model_tests)

# The preview harness renders real frames through the real engine and the real
# models, no SDK anywhere, and writes them as PPM images. Starlance draws its
# whole HUD through pse::draw_text rather than the SDK's screen.text, so unlike
# the other games here these frames show the instruments too.
add_executable(starlance_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${starlance_models}
)
target_include_directories(starlance_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(starlance_preview pse-core)
