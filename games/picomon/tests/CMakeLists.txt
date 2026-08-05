# Host side tests for Picomon, discovered generically by the top level
# BUILD_ENGINE_TESTS block. No SDK anywhere in here.
#
# The data is compiled the same way it is for the device: tools/picomon_data.py
# reads games/picomon/data and emits const C++ into the build tree. That is
# deliberate. If a level file names a species that does not exist, or warps
# somewhere that is not walkable, the tests fail to build rather than failing
# mysteriously at run time, and the failure names the file and the line.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(picomon_root ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(picomon_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)

file(GLOB picomon_data_files CONFIGURE_DEPENDS
     ${picomon_root}/data/*.txt ${picomon_root}/data/zones/*.zone)

add_custom_command(
    OUTPUT ${picomon_gen}/picomon_data.cpp ${picomon_gen}/picomon_data.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${picomon_gen}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/picomon_data.py
            ${picomon_root}/data
            --out-hpp ${picomon_gen}/picomon_data.hpp
            --out-cpp ${picomon_gen}/picomon_data.cpp
    DEPENDS ${picomon_data_files}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/picomon_data.py
    COMMENT "picomon_data: compiling the level data"
    VERBATIM
)

add_executable(picomon_sim_tests
    test_sim.cpp
    ${picomon_root}/src/sim.cpp
    ${picomon_gen}/picomon_data.cpp
)
target_include_directories(picomon_sim_tests PRIVATE
    ${picomon_root}/src
    ${picomon_gen}
)
add_test(NAME picomon_sim_tests COMMAND picomon_sim_tests)
