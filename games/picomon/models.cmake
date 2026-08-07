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
# There are three tree meshes and a pair of tree sprite sheets, and which one
# draws is decided by where the tree stands, not by taste:
#
#   pine.obj                 the battle backdrop. Five trees are the only
#                            scenery in the shot, so it can afford 26
#                            triangles and a third cone.
#   treepine.obj             a tree inside the playable area. 20 triangles,
#   treeleaf.obj             the same silhouette as the backdrop's, one
#                            species per zone.
#   art/build_art.py sprites the wall of trees that frames the map. Over a
#                            hundred tiles of it, so geometry is out of the
#                            question: 120 x 20 triangles is more than
#                            everything else in the frame put together.
#
# tools/picomon_data.py draws that line by flooding inward from the map edge,
# which is why it is 19 mesh trees on Route 1 and 117 sprites.
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
    treepine.obj
    treeleaf.obj
)
