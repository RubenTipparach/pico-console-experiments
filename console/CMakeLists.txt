# The console: one binary, the menu and every game console.yaml lists.
#
# The target is created here and the games append themselves to it, which is
# why it is added before them in the top level CMakeLists. That order is not
# an accident: a game's CMakeLists.txt says nothing about the console, so the
# console has to already exist for add_picosystem_game() to hand it sources.
#
# Not published to the web and not a game: it is never discovered by the
# gallery, never listed in build.yaml, and has no page of its own.

blit_executable(console
    src/main.cpp
    src/menu.cpp
    ${CONSOLE_GEN_DIR}/console_library.cpp
)

# Reading the cell is a device thing: an ADC channel and two GPIOs on the
# PicoSystem, and nothing at all on a desktop that has no battery. Split the
# same way the engine splits run_split, for the same reason: PICO_BOARD is
# only defined when configuring with the pico toolchain.
if(PICO_BOARD OR PICO_ADDON)
    target_sources(console PRIVATE src/battery_pico.cpp)
else()
    target_sources(console PRIVATE src/battery_host.cpp)
endif()

target_include_directories(console PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CONSOLE_GEN_DIR}
)

target_link_libraries(console pse-engine)
if(PICO_BOARD OR PICO_ADDON)
    target_link_libraries(console hardware_adc)
endif()

blit_metadata(console ${CMAKE_CURRENT_SOURCE_DIR}/metadata.yml)
