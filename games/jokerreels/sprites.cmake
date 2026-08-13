# This game's 2D art, listed once.
#
# Both the game and the host preview harness include this file, for the reason
# rule 11 gives about models: they compile the same render.cpp, so a picture
# named for one and not the other is a missing header.
#
# One sheet, eight cells, drawn by tools/gen_jokerreels_jokers.py. The cell is
# the Joker enum value, so there is no table anywhere naming the icons in an
# order that could drift from the rules; tools/tests/test_jokerreels_art.py is
# what keeps the sheet and the enum in step.
set(jokerreels_sprite_files
    jokers.png
    items.png
    extras.png
    hands.png
)
