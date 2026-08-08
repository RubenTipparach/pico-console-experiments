# Host side tests for Twin Flare, discovered generically by the top level
# BUILD_ENGINE_TESTS block.
#
# The sim is pure integer C++ with no SDK in it, so its promises are proven
# here rather than asserted in comments. Most of these tests are about UNITS
# rather than gameplay, because the fixed point port of a flight model that
# already worked in floating point shipped four separate scale errors and
# every one of them presented as "the pod behaves oddly" rather than as
# anything a compiler or a crash would report.

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/obj_model.cmake)

add_executable(twinflare_sim_tests
    test_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tracks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fixed.cpp
)
target_include_directories(twinflare_sim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(twinflare_sim_tests pse-core)
add_test(NAME twinflare_sim_tests COMMAND twinflare_sim_tests)

# The models are listed in exactly one place, and both the game and this
# harness read that list. A model added to one and not the other is a missing
# generated header, not a missing picture.
include(${CMAKE_CURRENT_LIST_DIR}/../models.cmake)

set(twinflare_models "")
foreach(model ${twinflare_model_files})
    add_obj_model(twinflare
        ${CMAKE_CURRENT_SOURCE_DIR}/../models/${model} twinflare_models)
endforeach()

# The renderer, for the parts a screenshot cannot show: that the floating
# origin really is bounding what the projector sees, that near geometry is cut
# rather than dropped, that there is ground beside the road, and that the pod's
# parts move relative to each other. All four were reported from playing it.
add_executable(twinflare_render_tests
    test_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tracks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fixed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${twinflare_models}
)
target_include_directories(twinflare_render_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(twinflare_render_tests pse-core)
add_test(NAME twinflare_render_tests COMMAND twinflare_render_tests)

# The preview harness renders real frames through the real engine and the real
# models, no SDK anywhere, and writes them as PPM. Twin Flare draws its whole
# HUD and every menu through pse::draw_text rather than the SDK's screen.text,
# so these frames show the instruments and the menus too, and the committed
# thumbnail comes out of exactly the code the device runs.
add_executable(twinflare_preview
    preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tracks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fixed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render.cpp
    ${twinflare_models}
)
target_include_directories(twinflare_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(twinflare_preview pse-core)
add_test(NAME twinflare_preview COMMAND twinflare_preview)
