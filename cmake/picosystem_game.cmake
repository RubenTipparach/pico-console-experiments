# add_picosystem_game(<name> SOURCES ... [MODELS ...] [ASSETS <file>] [DEFINES ...])
#
# Registers one game. This is the only thing a game's CMakeLists.txt needs to
# call: adding a game must never require editing the top level build, the
# workflow, or the gallery.
#
# The 32blit SDK decides what a target actually becomes. The same call produces
# a .uf2 for the PicoSystem, a native binary for desktop, and an .html/.js/.wasm
# triple for Emscripten, depending only on the toolchain the build was
# configured with.

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/obj_model.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/game_meta.cmake)

# Our Emscripten page, used instead of the SDK's, and the tool that fills in a
# game's tutorial panels.
#
# Both are resolved here, at file scope, and never inside the function below.
# CMAKE_CURRENT_LIST_DIR is the directory of the listfile being processed, and
# inside a function that is the caller's file, not this one: a path built from
# it in add_picosystem_game() came out as games/<slug>/../tools/gen_shell.py
# and broke every web build. At file scope it is this directory, which is what
# these paths are relative to.
set(PICO_WEB_SHELL ${CMAKE_CURRENT_LIST_DIR}/../web/shell.html
    CACHE INTERNAL "Emscripten shell with touch controls")
set(PICO_GEN_SHELL ${CMAKE_CURRENT_LIST_DIR}/../tools/gen_shell.py
    CACHE INTERNAL "Builds one game's page from the shell and its game.yml")

# Checked on every configure, not just Emscripten ones. The bad path above
# only failed on the web build, which is the one configuration a host build
# and the test suite never run, so it reached main green. A missing tool is
# now a hard error everywhere, and a laptop finds it in seconds.
foreach(required ${PICO_WEB_SHELL} ${PICO_GEN_SHELL})
    if(NOT EXISTS ${required})
        message(FATAL_ERROR "picosystem_game.cmake: missing ${required}")
    endif()
endforeach()

function(add_picosystem_game NAME)
    cmake_parse_arguments(GAME "" "ASSETS" "SOURCES;MODELS;DEFINES" ${ARGN})

    if(NOT GAME_SOURCES)
        message(FATAL_ERROR "add_picosystem_game(${NAME}): SOURCES is required")
    endif()

    set(generated_sources "")
    foreach(model ${GAME_MODELS})
        add_obj_model(${NAME} ${model} generated_sources)
    endforeach()

    # The name and icon block, for the launcher and the desktop tool. Device
    # builds only: on the web the gallery already says what a game is, and the
    # icon would just be 4.7 KB more wasm to download.
    if(NOT EMSCRIPTEN)
        add_game_meta(${NAME} generated_sources)
    endif()

    # Swap in our own Emscripten page.
    #
    # The SDK's shell is keyboard only, so a phone can load a game and then not
    # play it. Ours adds a touch gamepad and a fullscreen button.
    #
    # blit_executable() bakes `--shell-file ${EMSCRIPTEN_SHELL}` into the
    # target's LINK_FLAGS, so overriding that variable is the seam to use.
    # CMake functions inherit the caller's variable scope, so setting it here
    # reaches blit_executable without leaking to anything else. Rewriting
    # LINK_FLAGS after the fact would clobber the SDK's other link flags,
    # including the SDL2 ports and the IndexedDB filesystem.
    #
    # The shell is shared, so anything game specific has to be put into it
    # here: the mini tutorial panels come from this game's own game.yml, which
    # is why the page each game ships is generated rather than the file in
    # web/. Generating at configure time keeps it out of the link's way, and
    # game.yml is a configure dependency so editing it reconfigures.
    if(EMSCRIPTEN)
        set(shell_out ${CMAKE_CURRENT_BINARY_DIR}/generated/shell.html)
        set(game_yml ${CMAKE_CURRENT_SOURCE_DIR}/game.yml)
        set_property(DIRECTORY APPEND PROPERTY
                     CMAKE_CONFIGURE_DEPENDS ${game_yml} ${PICO_WEB_SHELL})
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env python3 ${PICO_GEN_SHELL}
                    --game ${CMAKE_CURRENT_SOURCE_DIR}
                    --shell ${PICO_WEB_SHELL}
                    --out ${shell_out}
            RESULT_VARIABLE shell_result)
        if(NOT shell_result EQUAL 0)
            message(FATAL_ERROR
                    "gen_shell.py failed for ${NAME}: a game's web page cannot "
                    "ship without its tutorial")
        endif()
        set(EMSCRIPTEN_SHELL ${shell_out})
    endif()

    blit_executable(${NAME} ${GAME_SOURCES} ${generated_sources})

    target_link_libraries(${NAME} pse-engine)

    if(NOT EMSCRIPTEN)
        game_meta_keep(${NAME})
    endif()

    # Generated model headers land in the build tree, next to nothing else.
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    if(GAME_DEFINES)
        target_compile_definitions(${NAME} PRIVATE ${GAME_DEFINES})
    endif()

    if(GAME_ASSETS)
        blit_assets_yaml(${NAME} ${GAME_ASSETS})
    endif()

    blit_metadata(${NAME} ${CMAKE_CURRENT_SOURCE_DIR}/metadata.yml)

    # Emscripten names its output after the target, but GitHub Pages has no
    # directory index, so the page has to be called index.html to be reachable
    # at /<slug>/. Renaming here keeps tools/publish.py from having to guess.
    if(EMSCRIPTEN)
        set_target_properties(${NAME} PROPERTIES OUTPUT_NAME "index")
    endif()
endfunction()
