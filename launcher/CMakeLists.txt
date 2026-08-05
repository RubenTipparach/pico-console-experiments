# The games launcher. Device only: it exists to hand the machine to another
# image, which is not a thing a browser tab can do.
#
# It is not a game and does not live under games/, so it is never discovered by
# the gallery, never published, and never listed in build.yaml.

blit_executable(launcher src/main.cpp src/library.cpp src/menu.cpp
    src/override_table.cpp)

target_include_directories(launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

blit_metadata(launcher metadata.yml)

# The launcher owns the base of flash, which is where the SDK links by default,
# so it needs no slot script of its own. What it does need is to stay under the
# first slot; tools/make_bundle.py refuses a bundle where it does not.
