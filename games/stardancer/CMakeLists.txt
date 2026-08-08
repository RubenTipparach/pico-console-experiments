include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

add_picosystem_game(stardancer
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    MODELS
        ${stardancer_model_files}
)
