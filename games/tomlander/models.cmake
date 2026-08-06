# The models Tom Lander renders, named once.
#
# src/render.cpp is compiled twice: into the game, and into the host preview
# harness under tests/. A model listed for one and not the other is not a
# missing picture, it is a missing header and a broken build. Add a model here
# and both builds see it. There is no second list.
#
# The pads are not here on purpose. They are boxes, and draw_box is the
# engine's own primitive: rule 11 keeps hand written vertex tables out of C++
# and exempts exactly the trivial shapes the engine already generates.
set(tomlander_model_files
    tom.obj
)
