include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

add_picosystem_game(picospace
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    MODELS
        ${picospace_model_files}
)
