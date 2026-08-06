#include "render.hpp"

#include <cmath>

#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"

#include "sprites.hpp"

#include "picomon/ball.hpp"
#include "picomon/counter.hpp"
#include "picomon/desk.hpp"
#include "picomon/emberkit.hpp"
#include "picomon/house.hpp"
#include "picomon/machine.hpp"
#include "picomon/mossling.hpp"
#include "picomon/mothlet.hpp"
#include "picomon/pebblin.hpp"
#include "picomon/pine.hpp"
#include "picomon/plant.hpp"
#include "picomon/rock.hpp"
#include "picomon/sign.hpp"
#include "picomon/sparklet.hpp"
#include "picomon/tidepup.hpp"
#include "picomon/treeleaf.hpp"
#include "picomon/treepine.hpp"
#include "picomon/wall.hpp"

namespace pmr {
namespace {

using namespace pm;
// The generated meshes. Namespaced by game because the console links every
// game into one binary, and this one shares a tree with kingfisher and a rock
// with dustrider.
using namespace models::picomon;

// Rendering state. Static because dynamic allocation is banned, and this is
// the documented RAM cost of drawing the game:
//   Rasterizer  14,400 bytes of depth buffer
//   the rest    under 200 bytes
pse::Rasterizer g_raster;
pse::Renderer3D g_renderer(g_raster);

// ---- the camera, measured out of Black and White ------------------------
//
// 31 degrees below horizontal through a 16.18 degree lens, yaw locked to the
// tile grid. The lens is the part that matters: the engine's default 90
// degrees looks 45 degrees further down at the bottom of the frame than in
// the middle, so a camera pitched for a three quarter view still reads as top
// down near the player. A long lens sees every row at nearly the same angle,
// and that uniformity is most of what makes it read as a world.
//
// Twelve tiles across a 120 pixel screen puts a tile at 10 pixels, which is
// what lands a 20 pixel character sprite at exactly two tiles and 1.0 scale.
// The height and setback then fall out of the lens rather than out of taste.
constexpr float k_fov = 16.183f;
constexpr float k_pitch = -31.0f * 3.14159265f / 180.0f;
constexpr float k_tiles_across = 12.0f;
constexpr float k_cam_q = 42.18f;                       // view depth at the player
constexpr float k_cam_height = 21.72f;
constexpr float k_cam_back = 36.16f;
constexpr int k_tile_px = 10;

// ---- a 3 by 5 font, the same metric as the SDK's minimal_font -----------
//
// The renderer carries its own because it must not include the SDK: this file
// is compiled into the preview harness and the host tests as well as the game.
// Each glyph is five rows of three bits.
struct Glyph { uint8_t row[5]; };

const Glyph k_font[] = {
    {{0, 0, 0, 0, 0}},          // space
    {{2, 5, 7, 5, 5}},          // A
    {{6, 5, 6, 5, 6}}, {{3, 4, 4, 4, 3}}, {{6, 5, 5, 5, 6}}, {{7, 4, 6, 4, 7}},
    {{7, 4, 6, 4, 4}}, {{3, 4, 5, 5, 3}}, {{5, 5, 7, 5, 5}}, {{7, 2, 2, 2, 7}},
    {{1, 1, 1, 5, 2}}, {{5, 5, 6, 5, 5}}, {{4, 4, 4, 4, 7}}, {{5, 7, 7, 5, 5}},
    {{6, 5, 5, 5, 5}}, {{2, 5, 5, 5, 2}}, {{6, 5, 6, 4, 4}}, {{2, 5, 5, 2, 1}},
    {{6, 5, 6, 5, 5}}, {{3, 4, 2, 1, 6}}, {{7, 2, 2, 2, 2}}, {{5, 5, 5, 5, 7}},
    {{5, 5, 5, 2, 2}}, {{5, 5, 7, 7, 5}}, {{5, 5, 2, 5, 5}}, {{5, 5, 2, 2, 2}},
    {{7, 1, 2, 4, 7}},          // Z
    {{7, 5, 5, 5, 7}}, {{2, 6, 2, 2, 7}}, {{6, 1, 2, 4, 7}}, {{6, 1, 2, 1, 6}},
    {{5, 5, 7, 1, 1}}, {{7, 4, 6, 1, 6}}, {{2, 4, 7, 5, 7}}, {{7, 1, 2, 2, 2}},
    {{7, 5, 7, 5, 7}}, {{7, 5, 7, 1, 2}},   // 9
    {{0, 0, 0, 0, 2}},          // .
    {{0, 0, 0, 2, 4}},          // ,
    {{2, 2, 2, 0, 2}},          // !
    {{6, 1, 2, 0, 2}},          // ?
    {{0, 2, 0, 2, 0}},          // :
    {{0, 0, 7, 0, 0}},          // -
    {{1, 1, 2, 4, 4}},          // /
    {{2, 2, 0, 0, 0}},          // '
    {{0, 5, 2, 5, 0}},          // x
    {{4, 2, 1, 2, 4}},          // >
    {{1, 2, 4, 2, 1}},          // <
    // $ has to be an S with a stem through it, and there is no fourth column
    // to put the stem in. The middle column carries both: it is the stem on
    // the top and bottom rows and the S's own turns in between.
    {{2, 7, 4, 7, 2}},          // $
};

int glyph_index(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (c == ' ') return 0;
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    switch (c) {
        case '.': return 37;
        case ',': return 38;
        case '!': return 39;
        case '?': return 40;
        case ':': return 41;
        case '-': return 42;
        case '/': return 43;
        case '\'': return 44;
        case 'x': return 45;
        case '>': return 46;
        case '<': return 47;
        case '$': return 48;
        default: return 40;      // anything unmapped shows as a question mark
    }
}

int text_width(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n * 4 - 1;
}

// ---- 2D primitives, straight to the target ------------------------------

void plot(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    g_raster.plot(x, y, r, g, b);
}

void fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) plot(i, j, r, g, b);
}

void text(const char* s, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; s[i]; i++) {
        const Glyph& gl = k_font[glyph_index(s[i])];
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (gl.row[row] & (4 >> col)) plot(x + i * 4 + col, y + row, r, g, b);
            }
        }
    }
}

void text_centred(const char* s, int cx, int y, uint8_t r, uint8_t g, uint8_t b) {
    text(s, cx - text_width(s) / 2, y, r, g, b);
}

// A panel: filled, edged, with the corners knocked out so it reads as rounded
// at this size.
void panel(int x, int y, int w, int h) {
    fill_rect(x, y, w, h, 0x22, 0x22, 0x33);
    for (int i = x; i < x + w; i++) {
        plot(i, y, 0xEE, 0xEE, 0xEE);
        plot(i, y + h - 1, 0xEE, 0xEE, 0xEE);
    }
    for (int j = y; j < y + h; j++) {
        plot(x, j, 0xEE, 0xEE, 0xEE);
        plot(x + w - 1, j, 0xEE, 0xEE, 0xEE);
    }
    plot(x, y, 0x22, 0x22, 0x33);
    plot(x + w - 1, y, 0x22, 0x22, 0x33);
    plot(x, y + h - 1, 0x22, 0x22, 0x33);
    plot(x + w - 1, y + h - 1, 0x22, 0x22, 0x33);
}

// Greedy word wrap at the panel's width, the same one the data compiler runs
// at build time so a line that would not fit fails there instead of here.
int wrap_into(const char* s, char out[3][32]) {
    int line = 0, col = 0;
    out[0][0] = 0;
    for (int i = 0; s[i] && line < 3; ) {
        int word = 0;
        while (s[i + word] && s[i + word] != ' ') word++;
        if (col && col + 1 + word > 28) {
            out[line][col] = 0;
            if (++line >= 3) break;
            col = 0;
            out[line][0] = 0;
        }
        if (col) out[line][col++] = ' ';
        for (int k = 0; k < word && col < 31; k++) out[line][col++] = s[i + k];
        out[line][col] = 0;
        i += word;
        while (s[i] == ' ') i++;
    }
    return line + 1;
}

void number(char* out, int value, int width = 0) {
    char tmp[8];
    int n = 0;
    if (value <= 0) tmp[n++] = '0';
    while (value > 0) { tmp[n++] = char('0' + value % 10); value /= 10; }
    int w = 0;
    while (w + n < width) out[w++] = ' ';
    for (int i = 0; i < n; i++) out[w + i] = tmp[n - 1 - i];
    out[w + n] = 0;
}

void append(char* out, const char* s) {
    int n = 0;
    while (out[n]) n++;
    for (int i = 0; s[i]; i++) out[n++] = s[i];
    out[n] = 0;
}

// ---- sprites -------------------------------------------------------------

// Draw one 4 bit indexed frame, depth tested so scenery in front still wins.
void draw_sprite(int sheet, int frame, int cx, int top, uint8_t depth,
                 bool flip, int white) {
    const SpriteSheet& sh = k_sheets[sheet];
    if (frame >= sh.frame_count) return;
    const int stride = sh.w * sh.h / 2;
    const uint8_t* px = sh.pixels + frame * stride;
    const int x0 = cx - sh.w / 2;
    for (int y = 0; y < sh.h; y++) {
        for (int x = 0; x < sh.w; x++) {
            const int sx = flip ? sh.w - 1 - x : x;
            const int i = y * sh.w + sx;
            const uint8_t pair = px[i >> 1];
            const uint8_t idx = (i & 1) ? (pair & 0x0F) : (pair >> 4);
            if (idx == 0) continue;
            const uint8_t* c = sh.palette + idx * 3;
            int r = c[0], g = c[1], b = c[2];
            if (white) {
                r += (255 - r) * white / 255;
                g += (255 - g) * white / 255;
                b += (255 - b) * white / 255;
            }
            const int px_x = x0 + x, px_y = top + y;
            if (!g_raster.test_and_set_depth(px_x, px_y, depth)) continue;
            plot(px_x, px_y, uint8_t(r), uint8_t(g), uint8_t(b));
        }
    }
}

// A tree, as a sprite standing on its tile.
//
// It used to be a mesh, and a screenful of Route 1 is up to 87 tree tiles: at
// twenty triangles each that is more triangles spent on trees than on the
// whole rest of the frame, on a chip with no FPU. A sprite is one blit.
//
// It is also the better picture here. The lens is long enough that a tile is
// ten pixels across near the player and eight at the far edge of the window,
// so one fixed size is very nearly right everywhere, and two sizes cover the
// rest of the falloff.
//
// A zone says which species grows in it and the shape varies only within that
// species, so an area is one kind of forest rather than a mixture. The
// variation is a hash of the tile, which keeps a forest from being one tree
// stamped in a grid, and the hash is the same every frame, which is what
// stops the treeline crawling as the player walks.
static_assert(int(TreeKind::Count) == k_tree_kind_count,
              "the tree species in the level data and the runs of frames in "
              "the art have to be the same list, in the same order");

// An NPC's sheet is an index straight into the art's k_sheets, so the data
// compiler's Sheet enum and the art's art_* constants are one list written
// in two files. They drifted, and the result was not a crash or a blank
// sprite: every villager in the game was drawn as the player and every nurse
// as a villager, animated perfectly, from a real sheet, just not theirs.
static_assert(sheet_hero == art_hero && sheet_villager == art_villager &&
                  sheet_trainer == art_trainer && sheet_healer == art_healer,
              "the sprite sheet order in tools/picomon_data.py and in "
              "art/build_art.py have to match, because an NPC's sheet is an "
              "index into the art's table");

// The mesh a zone's trees are made of, for the ones standing inside the
// playable area. The species is the zone's, never the tile's, so a route is
// one forest and not a mixture, and it is the same species the border's
// sprites are drawn from.
const pse::MeshData& tree_mesh(uint8_t kind) {
    switch (TreeKind(kind)) {
        case TreeKind::Broadleaf: return treeleaf;
        default:                  return treepine;
    }
}

void draw_tree(int tx, int ty, float wx, float wz, bool far, uint8_t kind) {
    int px = 0, py = 0, depth = 0;
    if (!g_renderer.project(wx, 0.0f, wz, px, py, depth)) return;
    const int sheet = far ? art_treefar : art_treenear;
    const SpriteSheet& sh = k_sheets[sheet];
    const TreeKindFrames& k = k_tree_kinds[kind < k_tree_kind_count ? kind : 0];
    const int frame = k.first + (tx * 37 + ty * 11) % (k.count ? k.count : 1);
    // Depth one step nearer than the ground it stands on, the same as every
    // other billboard here, so a tree wins against its own tile and loses to
    // anything genuinely in front of it.
    const uint8_t d = uint8_t(depth * 255 / pse::k_fixed_one - 1);
    draw_sprite(sheet, frame, px, py - sh.h, d, false, 0);
}

// The colour a hit throws off, by the attacking move's type. One rule and no
// table of effect sprites: at 120 pixels a coloured burst reads instantly and
// a six frame animation reads as noise.
void type_colour(uint8_t type, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (Type(type)) {
        case Type::Ember: r = 0xFF; g = 0x88; b = 0x22; return;
        case Type::Tide:  r = 0x55; g = 0xBB; b = 0xFF; return;
        case Type::Leaf:  r = 0x66; g = 0xDD; b = 0x55; return;
        case Type::Spark: r = 0xFF; g = 0xEE; b = 0x44; return;
        case Type::Stone: r = 0xCC; g = 0xAA; b = 0x66; return;
        default:          r = 0xEE; g = 0x88; b = 0xEE; return;
    }
}

// The burst itself: a ring of sparks thrown out from where the blow landed,
// expanding and thinning across the strike. Drawn straight to the target with
// no depth test, because it is in front of everything by definition and a
// depth tested spark that loses to the creature it just hit is a spark nobody
// sees.
//
// The spread is a fixed pattern rather than a random one on purpose: it costs
// no state, it is identical on every device, and it cannot flicker.
void impact_burst(int cx, int cy, float k, int strength, uint8_t type) {
    if (k < 0.0f || k > 1.0f) return;
    uint8_t r, g, b;
    type_colour(type, r, g, b);
    static const int8_t k_dir[12][2] = {
        {4, 0}, {3, 3}, {0, 4}, {-3, 3}, {-4, 0}, {-3, -3},
        {0, -4}, {3, -3}, {5, 2}, {-5, 2}, {2, -5}, {-2, -5},
    };
    const int reach = 3 + strength * 5 / 4;
    const int n = strength > 4 ? 12 : 8;
    // Fades as it flies, and stops rather than darkening to black: a spark
    // scaled toward zero is a black pixel, and a ring of black pixels on
    // green grass is not a burst ending, it is soot.
    const int fade = int(255.0f * (1.0f - k));
    if (fade < 70) return;
    for (int i = 0; i < n; i++) {
        const float d = 0.25f + k * 1.15f;
        const int x = cx + int(float(k_dir[i][0]) * d * float(reach) / 4.0f);
        const int y = cy + int(float(k_dir[i][1]) * d * float(reach) / 4.0f);
        const uint8_t rr = uint8_t(r * fade / 255);
        const uint8_t gg = uint8_t(g * fade / 255);
        const uint8_t bb = uint8_t(b * fade / 255);
        plot(x, y, rr, gg, bb);
        plot(x + 1, y, rr, gg, bb);
        plot(x, y + 1, rr, gg, bb);
        if (strength > 4) plot(x + 1, y + 1, rr, gg, bb);
    }
}

// A darkened ellipse of ground under a billboard. No geometry and no art, and
// it is the single thing that stops a sprite reading as a sticker on screen.
void ground_shadow(float wx, float wz, float radius) {
    int px = 0, py = 0, depth = 0;
    if (!g_renderer.project(wx, 0.02f, wz, px, py, depth)) return;
    const int rx = int(radius * k_tile_px);
    const int ry = rx > 1 ? rx / 2 : 1;
    const uint8_t d = uint8_t(depth * 255 / pse::k_fixed_one);
    for (int j = -ry; j <= ry; j++) {
        for (int i = -rx; i <= rx; i++) {
            if (i * i * ry * ry + j * j * rx * rx > rx * rx * ry * ry) continue;
            if (!g_raster.test_and_set_depth(px + i, py + j, uint8_t(d - 1))) continue;
            // Reading the framebuffer back is not worth the format handling
            // here, so the shadow is a flat dark tint rather than a multiply.
            plot(px + i, py + j, 0x33, 0x55, 0x33);
        }
    }
}

// ---- the ground ----------------------------------------------------------

void ground_quad(float x0, float z0, float x1, float z1,
                 uint8_t r, uint8_t g, uint8_t b, float y = 0.0f) {
    int sx[4], sy[4], sz[4];
    bool ok = g_renderer.project(x0, y, z0, sx[0], sy[0], sz[0]);
    ok &= g_renderer.project(x1, y, z0, sx[1], sy[1], sz[1]);
    ok &= g_renderer.project(x1, y, z1, sx[2], sy[2], sz[2]);
    ok &= g_renderer.project(x0, y, z1, sx[3], sy[3], sz[3]);
    if (!ok) return;

    pse::ScreenTriangle t;
    t.r0 = t.r1 = t.r2 = r;
    t.g0 = t.g1 = t.g2 = g;
    t.b0 = t.b1 = t.b2 = b;
    t.x0 = int16_t(sx[0]); t.y0 = int16_t(sy[0]); t.z0 = uint16_t(sz[0]);
    t.x1 = int16_t(sx[1]); t.y1 = int16_t(sy[1]); t.z1 = uint16_t(sz[1]);
    t.x2 = int16_t(sx[2]); t.y2 = int16_t(sy[2]); t.z2 = uint16_t(sz[2]);
    g_raster.draw(t);
    t.x1 = int16_t(sx[2]); t.y1 = int16_t(sy[2]); t.z1 = uint16_t(sz[2]);
    t.x2 = int16_t(sx[3]); t.y2 = int16_t(sy[3]); t.z2 = uint16_t(sz[3]);
    g_raster.draw(t);
}

// Outside the map, read the nearest edge tile. A window that runs past the
// border is the cheapest way to fill the frame, and the border is trees.
uint8_t tile_clamped(const Zone& z, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= z.w) x = z.w - 1;
    if (y >= z.h) y = z.h - 1;
    return z.tiles[y * z.w + x];
}

const pse::MeshData* mesh_for(uint8_t mesh) {
    switch (mesh) {
        case mesh_emberkit: return &emberkit;
        case mesh_mossling: return &mossling;
        case mesh_tidepup: return &tidepup;
        case mesh_sparklet: return &sparklet;
        case mesh_pebblin: return &pebblin;
        case mesh_mothlet: return &mothlet;
        default: return &emberkit;
    }
}

// ---- the overworld -------------------------------------------------------

// A map row is not a world z. The camera looks along +z and the tile grid
// counts rows southward from row 0, so laying one straight onto the other
// renders the world back to front: the top of the frame is south, and
// pressing up walks the player toward the bottom of the screen. The map is
// read from the far end instead, which turns the camera round without
// mirroring x the way a 180 degree yaw would.
//
// wz(y) is the far edge of map row y. The row spans z from wz(y) - 1 to
// wz(y), and anything standing in the middle of it is at wz(y) - 0.5.
inline float wz_of(const Zone& z, float map_y) { return float(z.h) - map_y; }

void draw_overworld(const World& w, uint32_t t) {
    const Zone& z = zone_of(w);
    int16_t ox = 0, oy = 0;
    player_offset(w, ox, oy);
    const float px = float(w.tx) + 0.5f + float(ox) / 256.0f;
    const float pz = wz_of(z, float(w.ty) + 0.5f + float(oy) / 256.0f);

    g_renderer.set_fov(k_fov);
    // Bracket the depth range to what this scene occupies. The engine's
    // default 0.25 to 400 puts an entire tree inside one step of the one byte
    // depth buffer at this camera distance, so every prop ties with the ground
    // it stands on and loses the tie. That is not a theoretical risk: it is
    // what made the first version of this scene a field with no trees in it.
    g_renderer.set_depth_range(12.0f, 84.0f);
    g_renderer.set_camera(px, k_cam_height, pz - k_cam_back, 0.0f, k_pitch);

    g_raster.begin_frame(g_raster.target());
    // Indoors there is no sky. What shows above the far wall is the dark the
    // room is standing in, and the map's own border tile is wall, so the
    // window running past the edge draws wall rather than a field.
    if (z.indoor) g_raster.clear_gradient(0x22, 0x1D, 0x22, 0x33, 0x2C, 0x33);
    else g_raster.clear_gradient(0x66, 0xAA, 0xEE, 0xAA, 0xDD, 0xEE);

    // At this pitch the horizon sits far above the frame, so ground has to
    // cover every row of it. The obvious way is one big quad underneath, and
    // that does not work: a quad a few centimetres below the tiles lands on
    // the same step of an eight bit depth buffer, the tiles tie with it, and
    // ties go to whoever drew first. The whole map comes out one flat colour.
    //
    // So there is no underneath. The window simply runs past the edge of the
    // map and reads the border tile out there, which is trees, which is what
    // the edge of the map is anyway.
    const int x0 = int(px) - 10;
    const int x1 = int(px) + 10;
    // In world z, not in map rows: the near edge is behind the player and the
    // far edge is up the frame, whichever way round the map runs.
    const int z0 = int(pz) - 12;
    const int z1 = int(pz) + 20;

    // Rows merge runs of the same material, and deepen with distance: a tile
    // eleven rows out is a few pixels tall and never needed its own quad.
    for (int zi = z0; zi <= z1; ) {
        const int step = (zi - int(pz)) > 14 ? 4 : (zi - int(pz)) > 6 ? 2 : 1;
        const int z_end = (zi + step > z1 + 1) ? z1 + 1 : zi + step;
        // The row of map that occupies world z from zi to zi + 1.
        const int y = z.h - zi - 1;
        // A room stops at its walls. Outdoors the window runs past the border
        // and reads the edge tile, which is trees and is what the edge of the
        // map is anyway; indoors that same trick paints the floor colour out
        // into the void beyond the room, which is worse than showing nothing.
        if (z.indoor && (y < 0 || y >= z.h)) { zi = z_end; continue; }
        int x = x0;
        while (x <= x1) {
            if (z.indoor && (x < 0 || x >= z.w)) { x++; continue; }
            const int x_last = z.indoor && x1 >= z.w ? z.w - 1 : x1;
            const uint8_t tile = tile_clamped(z, x, y);
            int e = x;
            while (e + 1 <= x_last && tile_clamped(z, e + 1, y) == tile) e++;
            const TileDef& td = k_tiles[tile];
            uint8_t r = td.r, g = td.g, b = td.b;
            if (td.flags & k_tile_water) {
                const int wave = int(8.0f * sinf(float(t) * 0.002f + float(y)));
                r = uint8_t(r + wave); g = uint8_t(g + wave); b = uint8_t(b + wave);
            }
            ground_quad(float(x), float(zi), float(e + 1), float(z_end), r, g, b);
            x = e + 1;
        }
        zi = z_end;
    }

    // A trainer's sight line, while it is still unbeaten. The trap is meant to
    // be visible and avoidable rather than a gotcha.
    static const int8_t k_dx[4] = {0, 1, 0, -1};
    static const int8_t k_dy[4] = {-1, 0, 1, 0};
    for (int i = 0; i < z.npc_count; i++) {
        const NpcDef& n = z.npcs[i];
        if (NpcKind(n.kind) != NpcKind::Trainer || !n.sight) continue;
        if (n.flag != k_no_flag && flag_get(w, n.flag)) continue;
        if (!npc_present(w, n)) continue;
        for (int s = 1; s <= n.sight; s++) {
            const int tx = n.x + k_dx[n.facing] * s;
            const int ty = n.y + k_dy[n.facing] * s;
            if (!tile_walkable(z, tx, ty)) break;
            const float tz = wz_of(z, float(ty));
            ground_quad(float(tx) + 0.2f, tz - 0.8f,
                        float(tx) + 0.8f, tz - 0.2f, 0xCC, 0x99, 0x55);
        }
    }

    // Props, far to near so the sprites layered on top stay sane. These read
    // the real map, not the clamped one: scenery outside it is the flat border
    // colour and nothing stands on it.
    const int px0 = x0 < 0 ? 0 : x0, px1 = x1 >= z.w ? z.w - 1 : x1;
    // The window's near and far edges in world z, turned back into the map
    // rows they cover. Far is the smaller row number, which is why these look
    // crossed over.
    const int my_far = z.h - 1 - (z1 >= z.h ? z.h - 1 : z1);
    const int my_near = z.h - 1 - (z0 < 0 ? 0 : z0);
    for (int y = my_far; y <= my_near; y++) {
        for (int x = px0; x <= px1; x++) {
            const uint8_t tile = tile_at(z, x, y);
            const float wx = float(x) + 0.5f, wz = wz_of(z, float(y) + 0.5f);
            const float far = wz - pz > 4.0f ? 1.0f : 0.0f;
            switch (tile) {
                case tile_tree:
                    // The border. Sprites, not meshes: see draw_tree.
                    draw_tree(x, y, wx, wz, far != 0.0f, z.trees);
                    break;
                case tile_treecore:
                    // Inside the playable area, where the player walks round
                    // a tree and sees more than one side of it. Twenty
                    // triangles each and under twenty of them in a zone,
                    // which the border could never afford.
                    g_renderer.draw_mesh(tree_mesh(z.trees), wx, 0.0f, wz,
                                         0.0f, 1.0f);
                    break;
                case tile_rock:
                    g_renderer.draw_mesh(rock, wx, 0.0f, wz, 0.0f, 1.0f);
                    break;
                case tile_wall:
                    // Nothing south of the player. A wall between the camera
                    // and the player can only stand in the way, and at two
                    // units tall the near wall of a room ate the bottom third
                    // of the frame including the doorway the player had just
                    // walked through. The room reads as a cutaway instead,
                    // which is how these games have always drawn interiors.
                    if (wz < pz) break;
                    g_renderer.draw_mesh(wall, wx, 0.0f, wz, 0.0f, 1.0f);
                    break;
                case tile_counter:
                    // A bench is a counter in a different room. One mesh, and
                    // the tile colour under it does the rest.
                case tile_bench:
                    g_renderer.draw_mesh(counter, wx, 0.0f, wz, 0.0f, 1.0f);
                    break;
                case tile_desk:
                    g_renderer.draw_mesh(desk, wx, 0.0f, wz, 0.0f, 1.0f);
                    break;
                case tile_machine:
                    g_renderer.draw_mesh(machine, wx, 0.0f, wz, 0.0f, 1.0f);
                    break;
                case tile_plant:
                    g_renderer.draw_mesh(plant, wx, 0.0f, wz, 0.0f, 1.0f);
                    break;
                case tile_house:
                    // One mesh per building, on its own north west corner
                    // only, or a four tile house draws four times. The z
                    // offset is southward, which is now the near direction.
                    if (tile_at(z, x - 1, y) != tile_house &&
                        tile_at(z, x, y - 1) != tile_house) {
                        g_renderer.draw_mesh(house, wx + 1.0f, 0.0f, wz - 0.5f,
                                             0.0f, 1.0f);
                    }
                    break;
                default:
                    break;
            }
        }
    }
    for (int i = 0; i < z.event_count; i++) {
        const EventDef& e = z.events[i];
        const float ez = wz_of(z, float(e.y) + 0.5f);
        if (EventKind(e.kind) == EventKind::Sign) {
            g_renderer.draw_mesh(sign, float(e.x) + 0.5f, 0.0f, ez, 0.0f, 1.0f);
        } else if (EventKind(e.kind) == EventKind::Item &&
                   (e.flag == k_no_flag || !flag_get(w, e.flag))) {
            g_renderer.draw_mesh(ball, float(e.x) + 0.5f, 0.2f, ez, 0.0f, 1.0f);
        }
    }

    // The cast. Shadows first so a character never darkens the one behind it.
    for (int i = 0; i < z.npc_count; i++) {
        const NpcDef& n = z.npcs[i];
        if (!npc_present(w, n)) continue;
        ground_shadow(float(n.x) + 0.5f, wz_of(z, float(n.y) + 0.5f), 0.45f);
    }
    ground_shadow(px, pz, 0.45f);

    for (int i = 0; i < z.npc_count; i++) {
        const NpcDef& n = z.npcs[i];
        if (!npc_present(w, n)) continue;
        int sx = 0, sy = 0, sd = 0;
        if (!g_renderer.project(float(n.x) + 0.5f, 0.0f,
                                wz_of(z, float(n.y) + 0.5f), sx, sy, sd)) continue;
        const SpriteSheet& sh = k_sheets[n.sheet < k_sheet_art_count ? n.sheet : 0];
        const bool back = n.facing == 0 && sh.frame_count > 4;
        draw_sprite(n.sheet < k_sheet_art_count ? n.sheet : 1,
                    back ? 5 : 1, sx, sy - sh.h,
                    uint8_t(sd * 255 / pse::k_fixed_one - 1), false, 0);
    }
    {
        int sx = 0, sy = 0, sd = 0;
        if (g_renderer.project(px, 0.0f, pz, sx, sy, sd)) {
            const int dir = w.facing;
            const int tag = dir == 0 ? 4 : dir == 2 ? 0 : 8;
            const int phase = w.step ? (w.anim_phase & 3) : 1;
            draw_sprite(art_hero, tag + phase, sx, sy - k_sheets[art_hero].h,
                        uint8_t(sd * 255 / pse::k_fixed_one - 1), dir == 3, 0);
        }
    }

    // The banner, and the dialogue panel when something is being said.
    panel(2, 2, 4 * int(text_width(z.name) / 4) + 12, 13);
    text(z.name, 6, 6, 0xEE, 0xEE, 0xEE);
}

void draw_dialogue(const World& w, uint32_t t) {
    if (w.text_count == 0) return;
    panel(0, 84, 120, 36);
    char lines[3][32];
    const int n = wrap_into(k_text[w.text_first + w.text_page], lines);
    for (int i = 0; i < n && i < 3; i++) {
        text(lines[i], 5, 89 + i * 9, 0xEE, 0xEE, 0xEE);
    }
    if ((t / 350) & 1) text(">", 112, 111, 0xFF, 0xBB, 0x33);
}

// ---- battle --------------------------------------------------------------

// `shown` is the HP the bar draws at, which is not always the HP the creature
// has: across the strike it walks down from what it had to what it has, so a
// player sees the damage happen rather than finding it already applied. The
// number beside it walks with the bar, because a bar and a number disagreeing
// is worse than either being late.
void hp_plate(int x, int y, const char* name, int level, int hp, int max_hp,
              bool show_numbers, int shown = -1) {
    if (shown < 0) shown = hp;
    hp = shown;
    panel(x, y, 56, show_numbers ? 24 : 19);
    text(name, x + 3, y + 3, 0xEE, 0xEE, 0xEE);
    char lv[8] = "L";
    char num[8];
    number(num, level);
    append(lv, num);
    text(lv, x + 53 - text_width(lv), y + 3, 0xFF, 0xBB, 0x33);
    fill_rect(x + 3, y + 11, 50, 5, 0x44, 0x44, 0x55);
    const int frac = max_hp > 0 ? hp * 48 / max_hp : 0;
    uint8_t r = 0x55, g = 0xDD, b = 0x66;
    if (hp * 4 <= max_hp) { r = 0xEE; g = 0x44; b = 0x55; }
    else if (hp * 2 <= max_hp) { r = 0xEE; g = 0xCC; b = 0x33; }
    fill_rect(x + 4, y + 12, frac, 3, r, g, b);
    if (!show_numbers) return;
    char line[16];
    number(line, hp);
    append(line, "/");
    number(num, max_hp);
    append(line, num);
    text(line, x + 53 - text_width(line), y + 18, 0xEE, 0xEE, 0xEE);
}

void message_text(const World& w, const Message& m, char* out) {
    out[0] = 0;
    switch (m.kind) {
        case Msg::WildAppeared: append(out, "A WILD "); append(out, k_species[m.a].name);
                                append(out, " APPEARED!"); break;
        case Msg::TrainerSent:  append(out, "FOE SENT OUT "); append(out, k_species[m.a].name);
                                append(out, "!"); break;
        case Msg::YouUsed:      append(out, "YOU USED "); append(out, k_moves[m.a].name);
                                append(out, "!"); break;
        case Msg::FoeUsed:      append(out, "FOE USED "); append(out, k_moves[m.a].name);
                                append(out, "!"); break;
        case Msg::SuperEffective: append(out, "IT IS SUPER EFFECTIVE!"); break;
        case Msg::NotVery:      append(out, "IT IS NOT VERY EFFECTIVE."); break;
        case Msg::FoeFainted:   append(out, k_species[m.a].name); append(out, " FAINTED!"); break;
        case Msg::YouFainted:   append(out, k_species[m.a].name); append(out, " FAINTED!"); break;
        case Msg::GotAway:      append(out, "GOT AWAY SAFELY!"); break;
        case Msg::CouldNotRun:  append(out, "COULD NOT GET AWAY!"); break;
        case Msg::NoRunning:    append(out, "NO RUNNING FROM A TRAINER!"); break;
        case Msg::CaughtIt:     append(out, "GOTCHA! "); append(out, k_species[m.a].name);
                                append(out, " WAS CAUGHT!"); break;
        case Msg::BrokeFree:    append(out, k_species[m.a].name); append(out, " BROKE FREE!"); break;
        case Msg::LevelUp: {
            char num[8];
            append(out, k_species[m.a].name);
            append(out, " GREW TO LEVEL ");
            number(num, m.b);
            append(out, num);
            append(out, "!");
            break;
        }
        case Msg::Evolved:      append(out, "IT EVOLVED INTO ");
                                append(out, k_species[m.a].name); append(out, "!"); break;
        case Msg::OutOfPP:      append(out, "NO POWER LEFT FOR THAT MOVE!"); break;
        case Msg::StatFell:     append(out, m.a ? "FOE DEFENCE FELL!" : "DEFENCE FELL!"); break;
        case Msg::StatRose:     append(out, m.a ? "FOE DEFENCE ROSE!" : "DEFENCE ROSE!"); break;
        default: break;
    }
}

void draw_battle(const World& w, uint32_t t) {
    const Battle& b = w.battle;
    const Mon& me = w.party[b.active];

    // A battle is a different shot from the overworld, so it gets a different
    // lens: wider and lower, the way these games have always staged them.
    g_renderer.set_fov(30.0f);
    // Both ends matter and neither is obvious. Too near a far plane drops the
    // band that covers the top of the frame; too far a near plane drops the one
    // under the player's feet. Either way ground_quad loses a whole quad,
    // because it drops the quad if any corner fails to project, and sky shows
    // through. These two were found by probing the projector rather than by
    // reasoning about it.
    g_renderer.set_depth_range(2.0f, 68.0f);
    g_renderer.set_camera(0.0f, 6.54f, -13.44f, 0.0f, -0.3735f);

    g_raster.begin_frame(g_raster.target());
    g_raster.clear_gradient(0x44, 0x77, 0xCC, 0xCC, 0xDD, 0xEE);

    ground_quad(-16.0f, -11.0f, 16.0f, 2.0f, 0x77, 0xAA, 0x55);
    ground_quad(-16.0f, 2.0f, 16.0f, 9.0f, 0x66, 0x99, 0x4A);
    ground_quad(-16.0f, 9.0f, 16.0f, 47.0f, 0x55, 0x88, 0x55);
    // The treeline, as real geometry. The overworld cannot afford it, with up
    // to 87 tree tiles on screen; the arena has five and nothing else in the
    // shot, so they can be lit, sit properly in the depth buffer, and pick up
    // the camera's angle the way a billboard never will.
    for (int i = 0; i < 5; i++) {
        g_renderer.draw_mesh(pine, -9.0f + float(i) * 4.5f, 0.0f,
                             17.0f + float(i % 3) * 2.5f,
                             float(i) * 1.3f, 2.4f);
    }

    // Where the two of them stand, and how big they are drawn.
    //
    // The foe is further out than it looks like it should be: it has to clear
    // the player's own plate, which starts at y 55, and standing it nearer put
    // it behind the plate rather than above it.
    //
    // The scales below are measured, not guessed. At this camera the ground is
    // 13 pixels per world unit where the player's creature stands and 10 where
    // the foe does, so an unscaled mesh of the same species comes out only 30
    // percent smaller at the back. That is not enough to read as distance, and
    // an earlier pass that paid the foe back for its distance overshot badly:
    // the same species came out 27 px at the back against 21 at the front, so
    // the creature further away was the bigger one on screen and the depth
    // read backwards.
    //
    // These put the front one at about 32 px and the back one at about 19,
    // which is the ratio the design mockup has and which is what makes one of
    // them look near.
    const float foe_x = 2.5f, foe_z = 6.0f;
    const float me_x = -1.8f, me_z = 0.2f;
    const float k_me_scale = 1.73f, k_foe_scale = 1.42f;

    // Each of them stands on a rock, which is the other half of the same
    // illusion: a creature on a mound has a ground plane under it and a
    // silhouette against the grass behind, where a creature standing on flat
    // green has neither. The mound's peak is where the feet go.
    // Standing a little into the mound rather than balanced on its peak: the
    // rock is a dome, so feet placed at the exact summit hang over the sides
    // and the creature reads as impaled on it rather than stood on it.
    const float k_me_mound = 1.45f, k_foe_mound = 1.05f;
    const float me_y = 0.44f * k_me_mound * 0.55f;
    const float foe_y = 0.44f * k_foe_mound * 0.55f;

    // The turn, played back.
    //
    // The sim resolves both moves before it reaches Attack, so this beat has
    // to replay something rather than watch it happen. It reads fx_dmg,
    // fx_mult and fx_type, which use_move wrote down as it went.
    //
    // Fourteen ticks, split in two: the first mover swings in the first half
    // and the second mover in the second, which is the order the messages
    // underneath will then explain. Before this the whole thing was one lunge
    // and a line of text, and a player could not tell a hit from a miss.
    const int k_beat = 14, k_half = 7;
    float lunge_me = 0.0f, lunge_foe = 0.0f;
    int hit_side = -1;              // who is being hit right now, -1 for nobody
    float hit_k = 0.0f;             // 0 at the moment of impact, 1 when spent
    if (b.state == BattleState::Attack) {
        const int elapsed = k_beat - int(b.timer);
        const bool second = elapsed >= k_half;
        const int phase = second ? elapsed - k_half : elapsed;
        const bool attacker_is_player = second ? !b.player_first : b.player_first;
        const float k = sinf(float(phase) / float(k_half) * 3.14159f);
        (attacker_is_player ? lunge_me : lunge_foe) = k * 0.9f;
        // The strike lands at the top of the lunge, not at the start of it.
        const int side = attacker_is_player ? Battle::k_foe : Battle::k_you;
        if (b.fx_dmg[side] > 0 && phase >= 3) {
            hit_side = side;
            hit_k = float(phase - 3) / float(k_half - 3);
        }
    }
    const float ux = 0.595f, uz = 0.803f;   // player to foe, normalised

    // A hit whitens the thing that took it and knocks it sideways. The white
    // is what reads at 120 pixels; the shake is what makes the white feel
    // like a blow rather than a lamp coming on.
    const int flash_you = hit_side == Battle::k_you
                              ? int(255.0f * (1.0f - hit_k)) : 0;
    const int flash_foe = hit_side == Battle::k_foe
                              ? int(255.0f * (1.0f - hit_k)) : 0;
    const float shake = hit_side < 0 ? 0.0f
        : 0.28f * (1.0f - hit_k) * sinf(hit_k * 22.0f) *
          (b.fx_mult[hit_side] > 4 ? 1.8f : 1.0f);
    const float shake_you = hit_side == Battle::k_you ? shake : 0.0f;
    const float shake_foe = hit_side == Battle::k_foe ? shake : 0.0f;

    const float bob = 0.04f * sinf(float(t) * 0.0024f);

    // Both shadows before either creature: they are ground level and the
    // meshes have to win the depth test against them, not the other way round.
    const bool foe_out = b.state != BattleState::Wobble &&
                         b.state != BattleState::Caught;
    if (foe_out && b.foe.hp > 0) {
        ground_shadow(foe_x - lunge_foe * ux, foe_z - lunge_foe * uz, 0.5f);
    }
    ground_shadow(me_x + lunge_me * ux, me_z + lunge_me * uz, 0.8f);

    if (b.foe.hp > 0 || b.state == BattleState::Throw ||
        b.state == BattleState::Wobble) {
        const Species& fs = k_species[b.foe.species];
        const float scale = float(fs.scale) / 100.0f;
        if (foe_out) {
            g_renderer.draw_mesh(rock, foe_x, 0.0f, foe_z + 0.35f, 2.1f, k_foe_mound);
            g_renderer.draw_mesh(*mesh_for(fs.mesh),
                                 foe_x - lunge_foe * ux + shake_foe,
                                 foe_y + bob, foe_z - lunge_foe * uz,
                                 3.14159f, scale * k_foe_scale,
                                 fs.tint_r, fs.tint_g, fs.tint_b, 0.0f,
                                 uint8_t(flash_foe));
        }
    }
    {
        const Species& ms = k_species[me.species];
        g_renderer.draw_mesh(rock, me_x, 0.0f, me_z + 0.45f, 0.7f, k_me_mound);
        g_renderer.draw_mesh(*mesh_for(ms.mesh),
                             me_x + lunge_me * ux + shake_you,
                             me_y - bob, me_z + lunge_me * uz, 0.15f,
                             float(ms.scale) / 100.0f * k_me_scale,
                             ms.tint_r, ms.tint_g, ms.tint_b, 0.0f,
                             uint8_t(flash_you));
    }
    if (b.state == BattleState::Throw || b.state == BattleState::Wobble ||
        b.state == BattleState::Caught) {
        const float arc = b.state == BattleState::Throw
                              ? 1.0f - float(b.timer) / 20.0f : 1.0f;
        const float bx = -1.6f + (foe_x + 1.6f) * arc;
        const float bz = -0.6f + (foe_z + 0.6f) * arc;
        const float by = 0.4f + sinf(arc * 3.14159f) * 1.4f + foe_y * arc;
        g_renderer.draw_mesh(ball, bx, by, bz, float(t) * 0.01f, 1.0f);
    }

    // The burst, over the creature that took the blow.
    if (hit_side >= 0) {
        const float hx = hit_side == Battle::k_foe
                             ? foe_x - lunge_foe * ux + shake_foe
                             : me_x + lunge_me * ux + shake_you;
        const float hz = hit_side == Battle::k_foe
                             ? foe_z - lunge_foe * uz : me_z + lunge_me * uz;
        int sx = 0, sy = 0, sd = 0;
        const float hy = hit_side == Battle::k_foe ? foe_y + 0.6f : me_y + 0.8f;
        if (g_renderer.project(hx, hy, hz, sx, sy, sd)) {
            impact_burst(sx, sy, hit_k, b.fx_mult[hit_side], b.fx_type[hit_side]);
        }
    }

    // The plates, with the bar of whoever is being hit still draining.
    int shown_foe = -1, shown_you = -1;
    if (hit_side == Battle::k_foe) {
        shown_foe = int(b.foe.hp) + int(float(b.fx_dmg[Battle::k_foe]) * (1.0f - hit_k));
    } else if (hit_side == Battle::k_you) {
        shown_you = int(me.hp) + int(float(b.fx_dmg[Battle::k_you]) * (1.0f - hit_k));
    }
    hp_plate(3, 4, k_species[b.foe.species].name, b.foe.level, b.foe.hp,
             b.foe.max_hp, false, shown_foe);
    hp_plate(61, 55, k_species[me.species].name, me.level, me.hp, me.max_hp,
             true, shown_you);

    panel(0, 84, 120, 36);
    char line[64];
    switch (b.state) {
        case BattleState::Menu: {
            static const char* const k_labels[4] = {"FIGHT", "BAG", "MON", "RUN"};
            for (int i = 0; i < 4; i++) {
                const int cx = 14 + (i & 1) * 56;
                const int cy = 91 + (i >> 1) * 15;
                const bool sel = i == b.cursor;
                if (sel) text(">", cx - 7, cy, 0xFF, 0xBB, 0x33);
                text(k_labels[i], cx, cy, sel ? 0xFF : 0xCC, sel ? 0xFF : 0xCC,
                     sel ? 0xFF : 0xDD);
            }
            break;
        }
        case BattleState::Moves: {
            for (int i = 0; i < 4; i++) {
                if (me.moves[i] == 0xFF) continue;
                const int cx = 12 + (i & 1) * 56;
                const int cy = 89 + (i >> 1) * 11;
                const bool sel = i == b.move_cursor;
                if (sel) text(">", cx - 7, cy, 0xFF, 0xBB, 0x33);
                text(k_moves[me.moves[i]].name, cx, cy, sel ? 0xFF : 0xBB,
                     sel ? 0xFF : 0xBB, sel ? 0xFF : 0xCC);
            }
            if (me.moves[b.move_cursor] != 0xFF) {
                char num[8];
                line[0] = 0;
                append(line, "PP ");
                number(num, me.pp[b.move_cursor]);
                append(line, num);
                append(line, "/");
                number(num, k_moves[me.moves[b.move_cursor]].pp);
                append(line, num);
                text(line, 70, 111, 0xEE, 0xEE, 0xEE);
            }
            break;
        }
        case BattleState::Throw:
        case BattleState::Wobble: {
            // The sim has nothing queued through the throw, and a blank panel
            // under a ball in mid air reads as a game that has stopped. Said
            // here rather than pushed as a message: a message would need
            // dismissing, and this beat runs on its own timer.
            line[0] = 0;
            append(line, "YOU THREW A ");
            append(line, k_items[b.ball_item].name);
            append(line, "!");
            text(line, 5, 90, 0xEE, 0xEE, 0xEE);
            break;
        }
        default: {
            if (b.msg_count) {
                message_text(w, b.msgq[b.msg_head], line);
                char lines[3][32];
                const int n = wrap_into(line, lines);
                for (int i = 0; i < n && i < 3; i++) {
                    text(lines[i], 5, 90 + i * 9, 0xEE, 0xEE, 0xEE);
                }
                if ((t / 350) & 1) text(">", 112, 112, 0xFF, 0xBB, 0x33);
            }
            break;
        }
    }
}

// ---- flat menus ----------------------------------------------------------

void draw_bag(const World& w) {
    g_raster.begin_frame(g_raster.target());
    g_raster.clear_gradient(0x22, 0x22, 0x33, 0x11, 0x11, 0x22);
    static const char* const k_pockets[3] = {"BALLS", "MEDICINE", "KEY"};
    for (int i = 0; i < 3; i++) {
        const int x = 3 + i * 39;
        const bool on = i == w.menu_pocket;
        panel(x, 3, 37, 13);
        if (on) fill_rect(x + 1, 4, 35, 11, 0x44, 0x44, 0x66);
        text_centred(k_pockets[i], x + 18, 7, on ? 0xFF : 0x88,
                     on ? 0xBB : 0x88, on ? 0x33 : 0xAA);
    }
    panel(3, 19, 114, 76);
    int row = 0;
    int shown = -1;
    for (int i = 0; i < w.bag_count; i++) {
        if (k_items[w.bag[i].item].pocket != w.menu_pocket) continue;
        const int y = 24 + row * 10;
        const bool sel = row == w.menu_cursor;
        if (sel) {
            fill_rect(5, y - 2, 110, 9, 0x44, 0x44, 0x66);
            text(">", 6, y, 0xFF, 0xBB, 0x33);
            shown = i;
        }
        text(k_items[w.bag[i].item].name, 11, y, 0xEE, 0xEE, 0xEE);
        char cnt[8] = "x";
        char num[8];
        number(num, w.bag[i].count);
        append(cnt, num);
        text(cnt, 112 - text_width(cnt), y, 0xAA, 0xAA, 0xCC);
        row++;
    }
    panel(3, 97, 114, 20);
    if (shown >= 0) {
        char lines[3][32];
        const int n = wrap_into(k_items[w.bag[shown].item].desc, lines);
        for (int i = 0; i < n && i < 2; i++) {
            text(lines[i], 7, 101 + i * 8, 0xEE, 0xEE, 0xEE);
        }
    }
}

// The mart counter. Same shape as the bag on purpose: one list, one cursor,
// one description panel, so a player who has opened the bag already knows how
// to work this. What changes is the right hand column, which is the price
// rather than how many are held, and the money above it, which is the only
// thing that says why a purchase was refused.
void draw_shop(const World& w) {
    g_raster.begin_frame(g_raster.target());
    g_raster.clear_gradient(0x22, 0x22, 0x33, 0x11, 0x11, 0x22);

    const NpcDef* shop = shop_of(w);
    const int count = shop ? shop->stock_count : 0;

    panel(3, 3, 114, 14);
    text("MART", 7, 7, 0xEE, 0xEE, 0xEE);
    char money[12] = "$";
    char num[8];
    number(num, w.money);
    append(money, num);
    text(money, 113 - text_width(money), 7, 0xFF, 0xBB, 0x33);

    panel(3, 19, 114, 76);
    int shown = -1;
    for (int i = 0; i < count; i++) {
        const uint8_t item = k_stock[shop->stock_first + i];
        const Item& it = k_items[item];
        const int y = 24 + i * 10;
        const bool sel = i == w.menu_cursor;
        if (sel) {
            fill_rect(5, y - 2, 110, 9, 0x44, 0x44, 0x66);
            text(">", 6, y, 0xFF, 0xBB, 0x33);
            shown = int(item);
        }
        // Greyed when it cannot be bought, which is the whole explanation the
        // refusal ever gets and is on screen before the player presses A.
        const bool afford = w.money >= it.price;
        text(it.name, 11, y, afford ? 0xEE : 0x77, afford ? 0xEE : 0x77,
             afford ? 0xEE : 0x88);
        // How many are already held, in the row rather than under it: the
        // description panel below is two full lines of 27 and has no corner
        // to spare. Nothing is drawn at zero, so the column is quiet until
        // it has something to say.
        const int held = bag_count_of(w, item);
        if (held > 0) {
            char have[8] = "x";
            number(num, uint8_t(held));
            append(have, num);
            text(have, 72, y, 0x88, 0x88, 0xAA);
        }
        char price[12] = "$";
        number(num, it.price);
        append(price, num);
        text(price, 112 - text_width(price), y, afford ? 0xAA : 0x66,
             afford ? 0xAA : 0x66, afford ? 0xCC : 0x88);
    }

    panel(3, 97, 114, 20);
    if (shown >= 0) {
        char lines[3][32];
        const int n = wrap_into(k_items[shown].desc, lines);
        for (int i = 0; i < n && i < 2; i++) {
            text(lines[i], 7, 101 + i * 8, 0xEE, 0xEE, 0xEE);
        }
    }
}

void draw_party(const World& w) {
    g_raster.begin_frame(g_raster.target());
    g_raster.clear_gradient(0x22, 0x22, 0x33, 0x11, 0x11, 0x22);
    panel(2, 2, 116, 14);
    text("PARTY", 6, 6, 0xEE, 0xEE, 0xEE);
    for (int i = 0; i < k_max_party; i++) {
        const int y = 18 + i * 17;
        panel(2, y, 116, 16);
        if (i >= w.party_count) { text("- - -", 20, y + 5, 0x66, 0x66, 0x88); continue; }
        const Mon& m = w.party[i];
        if (i == w.menu_cursor) fill_rect(3, y + 1, 114, 14, 0x44, 0x44, 0x66);
        const Species& s = k_species[m.species];
        fill_rect(5, y + 3, 10, 10, s.tint_r / 2 + 0x33, s.tint_g / 2 + 0x33,
                  s.tint_b / 2 + 0x33);
        text(s.name, 20, y + 3, 0xEE, 0xEE, 0xEE);
        char lv[8] = "L";
        char num[8];
        number(num, m.level);
        append(lv, num);
        text(lv, 20, y + 10, 0xAA, 0xAA, 0xCC);
        fill_rect(66, y + 3, 48, 5, 0x44, 0x44, 0x55);
        const int frac = m.max_hp ? m.hp * 46 / m.max_hp : 0;
        uint8_t r = 0x55, g = 0xDD, b = 0x66;
        if (m.hp == 0) { r = 0x66; g = 0x66; b = 0x77; }
        else if (m.hp * 4 <= m.max_hp) { r = 0xEE; g = 0x44; b = 0x55; }
        else if (m.hp * 2 <= m.max_hp) { r = 0xEE; g = 0xCC; b = 0x33; }
        fill_rect(67, y + 4, frac, 3, r, g, b);
        char line[16];
        number(line, m.hp);
        append(line, "/");
        number(num, m.max_hp);
        append(line, num);
        text(line, 114 - text_width(line), y + 10, 0xDD, 0xDD, 0xEE);
    }
}

void draw_title(uint32_t t) {
    g_raster.begin_frame(g_raster.target());
    g_raster.clear_gradient(0x22, 0x33, 0x66, 0x66, 0x99, 0xDD);
    // The same camera the arena uses, because it is the one configuration in
    // this game whose depth range is known to separate a creature from the
    // ground it stands on. A title screen is not worth a second set of numbers
    // to keep in step with the first.
    g_renderer.set_fov(30.0f);
    g_renderer.set_depth_range(2.0f, 68.0f);
    g_renderer.set_camera(0.0f, 6.54f, -13.44f, 0.0f, -0.3735f);

    // Split into bands, and not for the colour. Depth is interpolated linearly
    // in screen space, and across one huge quad seen at this grazing angle that
    // is a poor approximation of the real perspective depth: the middle of the
    // quad reads far nearer than the ground actually is, and swallows anything
    // standing on it. The arena is banded for the same reason.
    ground_quad(-16.0f, -11.0f, 16.0f, 2.0f, 0x77, 0xAA, 0x55);
    ground_quad(-16.0f, 2.0f, 16.0f, 9.0f, 0x66, 0x99, 0x4A);
    ground_quad(-16.0f, 9.0f, 16.0f, 47.0f, 0x55, 0x88, 0x55);
    // The treeline, as real geometry. The overworld cannot afford it, with up
    // to 87 tree tiles on screen; the arena has five and nothing else in the
    // shot, so they can be lit, sit properly in the depth buffer, and pick up
    // the camera's angle the way a billboard never will.
    for (int i = 0; i < 5; i++) {
        g_renderer.draw_mesh(pine, -9.0f + float(i) * 4.5f, 0.0f,
                             17.0f + float(i % 3) * 2.5f,
                             float(i) * 1.3f, 2.4f);
    }
    g_renderer.draw_mesh(emberkit, -1.6f, 0.02f, 1.4f, 0.5f, 1.4f);
    g_renderer.draw_mesh(ball, 1.6f, 0.35f, 1.2f, float(t) * 0.003f, 1.2f);

    panel(14, 22, 92, 20);
    text_centred("PICOMON", 60, 28, 0xFF, 0xEE, 0x88);
    if ((t / 500) & 1) text_centred("PRESS ANY BUTTON", 60, 96, 0xEE, 0xEE, 0xEE);
}

}  // namespace

void render_scene(const pm::World& world, const pse::RenderTarget& target,
                  uint32_t time_ms) {
    g_raster.begin_frame(target);
    switch (world.mode) {
        case Mode::Title:
            draw_title(time_ms);
            return;
        case Mode::Bag:
            draw_bag(world);
            return;
        case Mode::Party:
            draw_party(world);
            return;
        case Mode::Shop:
            draw_shop(world);
            return;
        case Mode::Battle:
            draw_battle(world, time_ms);
            return;
        case Mode::Dialogue:
            draw_overworld(world, time_ms);
            draw_dialogue(world, time_ms);
            return;
        default:
            draw_overworld(world, time_ms);
            return;
    }
}

}  // namespace pmr
