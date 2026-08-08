include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

add_picosystem_game(starlance
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    MODELS
        ${starlance_model_files}
)
