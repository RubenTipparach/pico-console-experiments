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
# that reuses its base mesh costs three bytes. tree_far.obj is tree.obj's
# distance form and both are always built, because the swap is a draw time
# choice and not a build time one.
set(picomon_model_files
    emberkit.obj
    mossling.obj
    tidepup.obj
    sparklet.obj
    pebblin.obj
    mothlet.obj
    tree.obj
    tree_far.obj
    house.obj
    sign.obj
    rock.obj
    ball.obj
    wall.obj
    counter.obj
)
