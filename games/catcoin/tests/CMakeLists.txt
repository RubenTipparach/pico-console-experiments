# Host side tests for Cat Coin Pusher, discovered generically by the top level
# BUILD_ENGINE_TESTS block.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/texture.cmake)

add_executable(catcoin_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
)
target_include_directories(catcoin_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME catcoin_sim_tests COMMAND catcoin_sim_tests)

# The sprite list comes from the game's own file, never a second copy.
include(${CMAKE_CURRENT_LIST_DIR}/../sprites.cmake)

set(catcoin_generated "")
foreach(picture ${catcoin_sprite_files})
    add_sprite(catcoin
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/${picture} catcoin_generated)
endforeach()

add_executable(catcoin_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${catcoin_generated}
)
target_include_directories(catcoin_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(catcoin_preview pse-core)

add_test(NAME catcoin_preview COMMAND catcoin_preview)
