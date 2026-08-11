# The models Joker Reels renders, named once.
#
# src/render.cpp is compiled twice, into the game and into the host preview
# harness under tests/, and a model listed for one and not the other is a
# missing header rather than a missing picture. There is no second list.
set(jokerreels_model_files
    facet.obj
    cap.obj
)
