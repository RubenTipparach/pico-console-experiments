# Host side tests for Picomon, discovered generically by the top level
# BUILD_ENGINE_TESTS block. No SDK anywhere in here.
#
# The data is compiled the same way it is for the device: tools/picomon_data.py
# reads games/picomon/data and emits const C++ into the build tree. That is
# deliberate. If a level file names a species that does not exist, or warps
# somewhere that is not walkable, the tests fail to build rather than failing
# mysteriously at run time, and the failure names the file and the line.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(picomon_root ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(picomon_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)

file(GLOB picomon_data_files CONFIGURE_DEPENDS
     ${picomon_root}/data/*.txt ${picomon_root}/data/zones/*.zone)

add_custom_command(
    OUTPUT ${picomon_gen}/picomon_data.cpp ${picomon_gen}/picomon_data.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${picomon_gen}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/picomon_data.py
            ${picomon_root}/data
            --out-hpp ${picomon_gen}/picomon_data.hpp
            --out-cpp ${picomon_gen}/picomon_data.cpp
    DEPENDS ${picomon_data_files}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/picomon_data.py
    COMMENT "picomon_data: compiling the level data"
    VERBATIM
)

add_executable(picomon_sim_tests
    test_sim.cpp
    ${picomon_root}/src/sim.cpp
    ${picomon_gen}/picomon_data.cpp
)
target_include_directories(picomon_sim_tests PRIVATE
    ${picomon_root}/src
    ${picomon_gen}
)
add_test(NAME picomon_sim_tests COMMAND picomon_sim_tests)

# The preview harness renders real frames through the real engine and the real
# models, no SDK anywhere, and writes them as PPM images. Not a test: a tool for
# looking at the game without hardware, and the way the thumbnail is taken.
#
# It builds the game's own render.cpp, so it needs the game's own model list and
# the same generated sprite tables the device gets.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)

set(picomon_models "")
foreach(model ${picomon_model_files})
    add_obj_model(picomon_preview
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} picomon_models)
endforeach()

# The character art is committed as .aseprite and .png; the device tables are
# not, so they are regenerated into the build tree like everything else.
add_custom_command(
    OUTPUT ${picomon_gen}/sprites.cpp ${picomon_gen}/sprites.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${picomon_gen}
    COMMAND ${Python3_EXECUTABLE} ${picomon_root}/art/build_art.py
            --out-dir ${picomon_gen}
    DEPENDS ${picomon_root}/art/build_art.py ${picomon_root}/art/aseprite.py
    COMMENT "picomon art: packing the sprite sheets"
    VERBATIM
)

add_executable(picomon_preview
    preview.cpp
    ${picomon_root}/src/sim.cpp
    ${picomon_root}/src/render.cpp
    ${picomon_gen}/picomon_data.cpp
    ${picomon_gen}/sprites.cpp
    ${picomon_models}
)
target_include_directories(picomon_preview PRIVATE
    ${picomon_root}/src
    ${picomon_gen}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(picomon_preview pse-core)

# What a frame contains, asserted by rendering one. Three bugs on this branch
# were invisible to the sim tests and obvious in a picture, so these render real
# frames and fail on the pixels.
add_executable(picomon_render_tests
    test_render.cpp
    ${picomon_root}/src/sim.cpp
    ${picomon_root}/src/render.cpp
    ${picomon_gen}/picomon_data.cpp
    ${picomon_gen}/sprites.cpp
    ${picomon_models}
)
target_include_directories(picomon_render_tests PRIVATE
    ${picomon_root}/src
    ${picomon_gen}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(picomon_render_tests pse-core)
add_test(NAME picomon_render_tests COMMAND picomon_render_tests)
