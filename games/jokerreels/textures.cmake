# The eight symbols, named once, for the same reason models.cmake exists: the
# game and the host preview harness compile the same render.cpp, so a texture
# listed for one and not the other is a missing header rather than a missing
# picture.
#
# Drawn by tools/gen_jokerreels_symbols.py and committed as PNGs. The order
# here is only the build's; what the game indexes them by is sim.hpp's Symbol
# enum, and tools/tests/test_jokerreels_art.py is what keeps those in step.
set(jokerreels_texture_files
    cherry.png
    bell.png
    plum.png
    bar.png
    clover.png
    seven.png
    diamond.png
    crown.png
)
