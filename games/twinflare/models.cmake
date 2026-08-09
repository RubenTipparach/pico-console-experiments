# The models Twin Flare renders, named once.
#
# src/render.cpp is compiled twice: into the game, and into the host preview
# harness under tests/. A model listed for one and not the other is not a
# missing picture, it is a missing header and a broken build. Add a model here
# and both builds see it. There is no second list.
#
# One engine and one cockpit PER RACER. There used to be two engine meshes and
# one cockpit between six racers, recoloured, which made the choice a colour
# swatch: the stat bars said the pods differed and the only thing visible was
# paint. Twelve meshes cost about 3.3 KB more of flash, against a 12 MB program
# region, and the triangle budget has not moved because the side count varies
# with the shape: five for the slim pods, seven for the heavy ones, and the
# roster averages the thirty six a single shared engine used to cost.
set(twinflare_model_files
    engine_scarab.obj
    engine_wisp.obj
    engine_anvil.obj
    engine_needle.obj
    engine_nightjar.obj
    engine_fang.obj
    cockpit_scarab.obj
    cockpit_wisp.obj
    cockpit_anvil.obj
    cockpit_needle.obj
    cockpit_nightjar.obj
    cockpit_fang.obj
    rock.obj
)
