#include "tracks.hpp"

#include "track_data.hpp"

namespace twinflare {
namespace {

// A palette must separate the ROAD from the GROUND by more than one 4-bit step
// in at least two channels. It did not, on the first pass: road 198,170,124
// against desert 206,178,128 is the same colour once the panel has had it, and
// the result was a beautiful dune sea with no visible track in it. The only
// way to find the road was to drive off it.
constexpr Palette k_dune = {
    {128, 176, 222}, {226, 206, 170},
    {{220, 190, 138}, {190, 160, 112}},
    {{160, 132, 94}, {140, 114, 80}},
    {236, 214, 160},
    {196, 166, 120},
    {{122, 94, 66}, {86, 64, 46}},
    {{40, 96, 128}, {30, 76, 108}}, {{70, 132, 168}, {58, 116, 152}},
    {200, 224, 236},
};

constexpr Palette k_tide = {
    {40, 104, 140}, {126, 196, 206},
    {{38, 96, 110}, {20, 60, 78}},
    {{132, 196, 190}, {104, 166, 166}},
    {226, 246, 244},
    {86, 146, 150},
    {{52, 92, 96}, {28, 54, 64}},
    // The sea has to separate from the road above it by more than one 4-bit
    // step or the causeway disappears into it, which is the same mistake the
    // desert made with its road. Road is 132,196,190; sea is a third of that
    // in red and half in green, which is four steps apart in both.
    {{26, 74, 124}, {16, 56, 104}}, {{74, 156, 196}, {58, 134, 178}},
    {228, 244, 252},
};

constexpr Palette k_ash = {
    {8, 8, 16}, {34, 32, 44},
    {{78, 76, 86}, {52, 50, 60}},
    {{158, 156, 168}, {126, 124, 138}},
    {212, 196, 120},
    {126, 124, 136},
    {{86, 84, 94}, {44, 42, 52}},
    {{30, 34, 52}, {20, 22, 38}}, {{58, 62, 84}, {46, 50, 70}},
    {150, 152, 170},
};

constexpr Palette k_frost = {
    {168, 182, 200}, {220, 230, 240},
    {{238, 244, 252}, {214, 226, 240}},
    {{150, 174, 204}, {126, 152, 186}},
    {86, 120, 160},
    {222, 234, 248},
    // Pale, not navy. Dark rock on a snow planet gave the whole frame to the
    // scenery: the eye went to the black wedges and had to hunt for the road,
    // which is exactly backwards on the track that is hardest to stay on.
    {{190, 206, 226}, {152, 172, 200}},
    {{86, 132, 176}, {66, 108, 152}}, {{132, 178, 218}, {112, 158, 200}},
    {240, 248, 255},
};

// The four numbers are why these are four games rather than four palettes.
// Every one is a thousandths multiplier the sim applies everywhere.
constexpr Track k_tracks[k_track_count] = {
    {"DUNE SEA", generated::dune_nodes,
     sizeof(generated::dune_nodes) / sizeof(TrackNode), 3, k_no_water,
     {1000, 1000, 1000, 1000}, k_dune},

    // Thick water: slower to turn and slower to stop, but the heat vanishes
    // and a jump hangs.
    //
    // The sea sits at -4, and the number is measured rather than chosen for
    // looks: the circuit's road runs from -18 to +12, so -4 puts about a third
    // of the lap under the surface and about two fifths of the plain beside
    // it. That is the track the brief asked for, half causeway and half open
    // water, and it is the reason the hover field treats water as ground.
    {"TIDEBREAK", generated::tide_nodes,
     sizeof(generated::tide_nodes) / sizeof(TrackNode), 3, -4 * 16,
     {720, 780, 1350, 1450}, k_tide},

    // Half the desert gravity and half its air. Jumps go a long way, the air
    // brake barely bites, and the boost gauge never really cools.
    {"ASHFALL", generated::ash_nodes,
     sizeof(generated::ash_nodes) / sizeof(TrackNode), 2, k_no_water,
     {500, 1200, 450, 550}, k_ash},

    // Half the grip of the desert. The pod goes where it was going, not where
    // it is pointing, and the walls are close.
    {"HOARFROST", generated::frost_nodes,
     sizeof(generated::frost_nodes) / sizeof(TrackNode), 3, k_no_water,
     {1050, 520, 1550, 1050}, k_frost},
};

}  // namespace

const Track& track(int index) {
    return k_tracks[static_cast<unsigned>(index) % k_track_count];
}

}  // namespace twinflare
