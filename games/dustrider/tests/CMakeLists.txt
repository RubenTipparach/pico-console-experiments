# Host side tests for Dust Rider, discovered generically by the top level
# BUILD_ENGINE_TESTS block. The sim is pure integer C++, so its promises
# (the window never outruns the bike, generation stays dodgeable, runs are
# deterministic) are proven here rather than asserted in comments.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)

add_executable(dustrider_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(dustrider_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME dustrider_sim_tests COMMAND dustrider_sim_tests)

# The preview harness renders real frames through the real engine and the
# real models, no SDK anywhere, and writes them as PPM images. Not a test: a
# tool for looking at the game without hardware.
# It builds the game's own render.cpp, so it needs the game's own model list.
# Keeping a second copy here is what left rock.hpp ungenerated and main red.
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)

set(dustrider_models "")
foreach(model ${dustrider_model_files})
    add_obj_model(dustrider_preview
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} dustrider_models)
endforeach()

add_executable(dustrider_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${dustrider_models}
)
target_include_directories(dustrider_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(dustrider_preview pse-core)
