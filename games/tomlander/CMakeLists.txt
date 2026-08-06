include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

add_picosystem_game(tomlander
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    MODELS
        ${tomlander_model_files}
)
