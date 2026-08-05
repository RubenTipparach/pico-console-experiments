# The models Dust Rider renders, named once.
#
# src/render.cpp is compiled twice: into the game, and into the host preview
# harness under tests/. A model listed for one and not the other is not a
# missing picture, it is a missing header and a broken build. That is exactly
# how rock.obj reached main: it was added to the game and the preview still
# had a list of its own.
#
# Add a model here and both builds see it. There is no second list.
set(dustrider_model_files
    bike.obj
    cactus.obj
    rock.obj
)
