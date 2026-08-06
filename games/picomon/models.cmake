# The models Picomon renders, named once.
#
# src/render.cpp is compiled twice: into the game, and into the host preview
# harness under tests/. A model listed for one and not the other is not a
# missing picture, it is a missing header and a broken build.
#
# Add a model here and both builds see it. There is no second list.
#
# The six creature meshes are shared: species.txt gives sixteen species a
# `mesh` and a tint, so a mesh is roughly 1.2 KB of flash and an evolution
# that reuses its base mesh costs three bytes.
#
# pine.obj is the battle backdrop's tree and only that. The overworld's trees
# are sprites, authored in art/build_art.py: a screenful of Route 1 is up to
# 87 tree tiles, and geometry there costs more triangles than the whole rest
# of the frame. The arena has five, they are the only scenery in the shot,
# and geometry catches the light and sits in the depth buffer in a way a
# billboard cannot.
set(picomon_model_files
    emberkit.obj
    mossling.obj
    tidepup.obj
    sparklet.obj
    pebblin.obj
    mothlet.obj
    house.obj
    sign.obj
    rock.obj
    ball.obj
    wall.obj
    counter.obj
    desk.obj
    machine.obj
    plant.obj
    pine.obj
)
