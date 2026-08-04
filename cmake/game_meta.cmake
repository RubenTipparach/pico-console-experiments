# add_game_meta(<target> <out_sources_var>)
#
# Compiles the game's name and icon into the binary itself, in a `.pse_meta`
# section, so an on device launcher and the desktop tool can both read them
# straight out of the `.uf2`. See tools/game_meta.py for the layout and
# LAUNCHER.md for why the block exists at all.
#
# Device builds only. A web build has the gallery for its name and picture,
# and 4.7 KB of icon in a wasm download buys nothing.

include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Same caching reason as obj_model.cmake: include_guard(GLOBAL) means only the
# first directory to include this file runs the find, so the path has to
# outlive that directory's scope.
set(PICO_META_PYTHON3 ${Python3_EXECUTABLE}
    CACHE INTERNAL "python interpreter for game_meta")

set(PICO_GAME_META ${CMAKE_CURRENT_LIST_DIR}/../tools/game_meta.py
    CACHE INTERNAL "game metadata generator")

function(add_game_meta TARGET OUT_SOURCES)
    set(generated_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(generated_cpp ${generated_dir}/${TARGET}_meta.cpp)

    # The icon is whatever the game ships, so both are inputs: editing either
    # has to regenerate. Neither is required, and a game with no picture gets
    # a generated placeholder rather than a build failure.
    set(icon_inputs "")
    foreach(candidate icon.png thumbnail.png)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${candidate})
            list(APPEND icon_inputs ${CMAKE_CURRENT_SOURCE_DIR}/${candidate})
        endif()
    endforeach()

    add_custom_command(
        OUTPUT ${generated_cpp}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_dir}
        COMMAND ${PICO_META_PYTHON3} ${PICO_GAME_META} emit
                --game ${CMAKE_CURRENT_SOURCE_DIR}
                --out ${generated_cpp}
                --version "${PSE_BUILD_VERSION}"
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/game.yml ${icon_inputs}
                ${PICO_GAME_META}
        COMMENT "game_meta ${TARGET}"
        VERBATIM
    )

    set(${OUT_SOURCES} ${${OUT_SOURCES}} ${generated_cpp} PARENT_SCOPE)
endfunction()
