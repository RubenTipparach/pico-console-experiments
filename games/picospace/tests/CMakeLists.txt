# Host side tests for Pico Space Program, discovered generically by the top
# level BUILD_ENGINE_TESTS block.
#
# The flight model is pure integer C++, so the claims the game makes are proven
# here rather than asserted in comments: that a circular orbit stays circular,
# that the apoapsis the HUD prints is the apoapsis the ship reaches, that a
# Hohmann burn taken at the window the map lights actually arrives at the moon,
# and that the whole mission can be flown from the pad to a landing.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)

add_executable(picospace_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(picospace_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(picospace_sim_tests pse-core)
add_test(NAME picospace_sim_tests COMMAND picospace_sim_tests)

# The preview harness renders real frames through the real engine and the real
# models, no SDK anywhere, and writes them as PPM images. It builds the game's
# own render.cpp, so it needs the game's own model list: keeping a second copy
# here is what left a header ungenerated and main red once already.
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)

set(picospace_models "")
foreach(model ${picospace_model_files})
    # The SLUG, not the target: the preview compiles the same render.cpp the
    # device does, so the model has to land in the namespace that source names.
    add_obj_model(picospace
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} picospace_models)
endforeach()

add_executable(picospace_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${picospace_models}
)
target_include_directories(picospace_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(picospace_preview pse-core)

# It is a tool AND it is the only thing that checks the render side promises:
# that the horizon sits where the altitude says it should at every scale from a
# pad to an orbit, that the legs appear exactly when the booster is gone, that
# the prograde marker points where the sim says the ship is going, and that a
# frame never overflows the triangle queue. It returns non zero when any of
# those breaks, and until it is registered here none of them run in CI, so they
# would be checks that only fire when somebody happens to run the binary.
add_test(NAME picospace_preview COMMAND picospace_preview)
