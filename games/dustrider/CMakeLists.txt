include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

add_picosystem_game(dustrider
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
    MODELS
        ${dustrider_model_files}
)
