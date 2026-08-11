include(${CMAKE_CURRENT_LIST_DIR}/sprites.cmake)

add_picosystem_game(catcoin
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    SPRITES
        ${catcoin_sprite_files}
)
