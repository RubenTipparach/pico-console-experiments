include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The level data and the character art are text and .aseprite files in this
# directory, compiled to const C++ at build time and never committed. Doing it
# here rather than by hand is what makes a typo in a map a failed build with a
# file and a line, instead of an event that silently never fires.
set(picomon_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)

file(GLOB picomon_data_files CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/data/*.txt
     ${CMAKE_CURRENT_SOURCE_DIR}/data/zones/*.zone)

add_custom_command(
    OUTPUT ${picomon_gen}/picomon_data.cpp ${picomon_gen}/picomon_data.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${picomon_gen}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/../../tools/picomon_data.py
            ${CMAKE_CURRENT_SOURCE_DIR}/data
            --out-hpp ${picomon_gen}/picomon_data.hpp
            --out-cpp ${picomon_gen}/picomon_data.cpp
    DEPENDS ${picomon_data_files}
            ${CMAKE_CURRENT_LIST_DIR}/../../tools/picomon_data.py
    COMMENT "picomon_data: compiling the level data"
    VERBATIM
)

add_custom_command(
    OUTPUT ${picomon_gen}/sprites.cpp ${picomon_gen}/sprites.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${picomon_gen}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/art/build_art.py
            --out-dir ${picomon_gen}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/art/build_art.py
            ${CMAKE_CURRENT_SOURCE_DIR}/art/aseprite.py
    COMMENT "picomon art: packing the sprite sheets"
    VERBATIM
)

# No target_include_directories(picomon ...) after this call, however much it
# looks like one is needed for the generated headers above. add_picosystem_game
# already puts this game's generated directory on the include path, and in a
# console build there is no picomon target to name: the sources are compiled
# into the console binary, so asking for include directories on it was a hard
# configure error the moment picomon went on the menu.
add_picosystem_game(picomon
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
        ${picomon_gen}/picomon_data.cpp
        ${picomon_gen}/sprites.cpp
    MODELS
        ${picomon_model_files}
)
