# Host side tests for Tom Lander, discovered generically by the top level
# BUILD_ENGINE_TESTS block. The sim is pure integer C++, so its promises (one
# pod cannot hover and four can, a pod lifts its own corner, the leveller
# converges) are proven here rather than asserted in comments.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)

add_executable(tomlander_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(tomlander_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME tomlander_sim_tests COMMAND tomlander_sim_tests)

# The preview harness renders real frames through the real engine and the real
# models, no SDK anywhere, and writes them as PPM images. It builds the game's
# own render.cpp, so it needs the game's own model list: keeping a second copy
# here is what left a header ungenerated and main red once already.
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)

set(tomlander_models "")
foreach(model ${tomlander_model_files})
    # The SLUG, not the target: the preview compiles the same render.cpp the
    # device does, so the model has to land in the namespace that source names.
    add_obj_model(tomlander
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} tomlander_models)
endforeach()

add_executable(tomlander_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${tomlander_models}
)
target_include_directories(tomlander_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(tomlander_preview pse-core)
