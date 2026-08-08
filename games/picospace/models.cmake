# The models Pico Space Program renders, named once.
#
# src/render.cpp is compiled twice: into the game, and into the host preview
# harness under tests/. A model listed for one and not the other is not a
# missing picture, it is a missing header and a broken build. Add a model here
# and both builds see it. There is no second list.
set(picospace_model_files
    booster.obj
    lander.obj
    legs.obj
    pad.obj
)
