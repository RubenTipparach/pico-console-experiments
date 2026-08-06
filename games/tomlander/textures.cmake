# The textures Tom Lander renders, named once.
#
# Same discipline as models.cmake and for the same reason: src/render.cpp is
# compiled into the game AND into the host preview harness, so a texture listed
# for one and not the other is a missing header rather than a missing picture.
set(tomlander_texture_files
    crate.png
    facade.png
    hull.png
)
