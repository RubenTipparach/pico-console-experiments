include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/textures.cmake)

add_picosystem_game(tomlander
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    MODELS
        ${tomlander_model_files}
    TEXTURES
        ${tomlander_texture_files}
)
