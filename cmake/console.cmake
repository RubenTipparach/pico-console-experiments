# Turning console.yaml into the console's menu.
#
# One function, called from two places: the device build, which compiles the
# generated table into the console binary, and the host tests, which draw the
# same table without compiling a single game. Both get the list from the same
# generator run, so a screenshot cannot be of a menu the device does not have.

include_guard(GLOBAL)

# Captured while this file is being read, so it is the repository root and not
# whatever directory happens to be calling the function.
set(PSE_REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. CACHE INTERNAL "repository root")

# console_generate_library(<out_dir>)
#
# Writes console_library.cpp, console_game_stubs.cpp and console_games.cmake
# into <out_dir>, and sets CONSOLE_GAMES in the caller's scope to the slugs the
# menu lists.
function(console_generate_library out_dir)
    set(config ${PSE_REPO_ROOT}/console.yaml)

    # Reconfigure when the list or any game's name or picture changes. Without
    # this, adding a game to console.yaml would build a console that does not
    # have it, which is exactly the class of quiet wrongness this rewrite is
    # about.
    file(GLOB console_inputs CONFIGURE_DEPENDS
         ${PSE_REPO_ROOT}/games/*/game.yml
         ${PSE_REPO_ROOT}/games/*/thumbnail.png
         ${PSE_REPO_ROOT}/games/*/icon.png)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${config} ${console_inputs}
                 ${PSE_REPO_ROOT}/tools/gen_library.py)

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${PSE_REPO_ROOT}/tools/gen_library.py
                --config ${config}
                --games ${PSE_REPO_ROOT}/games
                --out ${out_dir}
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
                "gen_library.py refused console.yaml. The message above says "
                "which entry and why; the console is not built from a list it "
                "cannot draw.")
    endif()

    include(${out_dir}/console_games.cmake)
    set(CONSOLE_GAMES ${CONSOLE_GAMES} PARENT_SCOPE)
endfunction()
