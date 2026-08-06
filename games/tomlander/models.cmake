# The models Tom Lander renders, named once.
#
# src/render.cpp is compiled twice: into the game, and into the host preview
# harness under tests/. A model listed for one and not the other is not a
# missing picture, it is a missing header and a broken build. Add a model here
# and both builds see it. There is no second list.
#
# The pad used to be left out of here as a draw_box, on the grounds that rule
# 11 exempts the trivial shapes the engine already generates. It is not one:
# it is a plated deck on a block, it is a fixed object, and the moment it
# needed to stop being a box it would have become a vertex table in C++, which
# is the thing the rule exists to prevent. It is a model.
set(tomlander_model_files
    block.obj
    cargo.obj
    pad.obj
    tom.obj
    tower.obj
)
