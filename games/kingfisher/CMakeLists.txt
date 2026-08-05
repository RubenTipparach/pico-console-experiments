include(${CMAKE_CURRENT_LIST_DIR}/models.cmake)

add_picosystem_game(kingfisher
    SOURCES
        src/game.cpp
        src/sim.cpp
        src/render.cpp
        src/sfx.cpp
    MODELS
        ${kingfisher_model_files}
)
