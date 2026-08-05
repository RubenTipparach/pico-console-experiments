# Host tests and the screenshot harness for the console's menu.
#
# Both build the real generated library, so the names and icons here are the
# ones the device shows. What they do not build is the games: the generated
# stubs stand in for them, because proving the menu draws does not need four
# games and the 3D renderer compiled first. That keeps this pass in seconds,
# which is what makes it something to run rather than something to skip.

set(CONSOLE_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
console_generate_library(${CONSOLE_GEN})

add_executable(console_menu_tests
    test_menu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/menu.cpp
    ${CONSOLE_GEN}/console_library.cpp
    ${CONSOLE_GEN}/console_game_stubs.cpp
)
target_include_directories(console_menu_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(console_menu_tests pse-core)
add_test(NAME console_menu_tests COMMAND console_menu_tests)

# Not a test: a way to look at the menu. Writes PPM frames, which
# tools/make_thumbnail.py --scale 1 turns into PNGs.
add_executable(console_menu_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/menu.cpp
    ${CONSOLE_GEN}/console_library.cpp
    ${CONSOLE_GEN}/console_game_stubs.cpp
)
target_include_directories(console_menu_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(console_menu_preview pse-core)
