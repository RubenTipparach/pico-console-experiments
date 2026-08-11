include(${CMAKE_CURRENT_LIST_DIR}/sprites.cmake)

add_picosystem_game(dumblander
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    SPRITES
        ${dumblander_sprite_files}
)
