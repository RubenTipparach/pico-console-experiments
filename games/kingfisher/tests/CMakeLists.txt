# Host side tests for Kingfisher, discovered generically by the top level
# BUILD_ENGINE_TESTS block. The sim is pure integer C++, so its promises
# (every fish landable, greedy reeling punished, no allocation, determinism)
# are proven here rather than asserted in comments.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)

add_executable(kingfisher_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(kingfisher_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME kingfisher_sim_tests COMMAND kingfisher_sim_tests)

# The preview harness renders real frames through the real engine and the real
# models, no SDK anywhere, and writes them as PPM images. Not a test: a tool
# for looking at the game without hardware.
# It builds the game's own render.cpp, so it needs the game's own model list.
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)

set(kingfisher_models "")
foreach(model ${kingfisher_model_files})
    add_obj_model(kingfisher_preview
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} kingfisher_models)
endforeach()

add_executable(kingfisher_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${kingfisher_models}
)
target_include_directories(kingfisher_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(kingfisher_preview pse-core)
