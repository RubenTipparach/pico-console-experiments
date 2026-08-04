# Host tests for the launcher's two dangerous parts: what it believes is
# installed, and what it is willing to jump into. Neither can be debugged on
# the device without a wire, so both are proven here against synthetic images.

add_executable(launcher_tests
    test_library.cpp
    ../src/library.cpp
    ../src/menu.cpp
)

target_include_directories(launcher_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_test(NAME launcher_tests COMMAND launcher_tests)
