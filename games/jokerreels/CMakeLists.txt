include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/textures.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/sprites.cmake)

add_picosystem_game(jokerreels
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    MODELS
        ${jokerreels_model_files}
    TEXTURES
        ${jokerreels_texture_files}
    SPRITES
        ${jokerreels_sprite_files}
)
