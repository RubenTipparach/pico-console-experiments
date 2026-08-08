include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

add_picosystem_game(twinflare
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/tracks.cpp
        src/fixed.cpp
        src/render.cpp
    MODELS
        ${twinflare_model_files}
)
