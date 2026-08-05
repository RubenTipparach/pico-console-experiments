#include "render.hpp"

#include <cmath>

#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"

#include "sprites.hpp"

#include "ball.hpp"
#include "emberkit.hpp"
#include "house.hpp"
#include "mossling.hpp"
#include "mothlet.hpp"
#include "pebblin.hpp"
#include "rock.hpp"
#include "sign.hpp"
#include "sparklet.hpp"
#include "tidepup.hpp"
#include "tree.hpp"
#include "tree_far.hpp"

namespace pmr {
namespace {

using namespace pm;
using namespace models;   // the generated meshes

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

void draw_overworld(const World& w, uint32_t t) {
    const Zone& z = zone_of(w);
    int16_t ox = 0, oy = 0;
    player_offset(w, ox, oy);
    const float px = float(w.tx) + 0.5f + float(ox) / 256.0f;
    const float pz = float(w.ty) + 0.5f + float(oy) / 256.0f;

    g_renderer.set_fov(k_fov);
    // Bracket the depth range to what this scene occupies. The engine's
    // default 0.25 to 400 puts an entire tree inside one step of the one byte
    // depth buffer at this camera distance, so every prop ties with the ground
    // it stands on and loses the tie. That is not a theoretical risk: it is
    // what made the first version of this scene a field with no trees in it.
    g_renderer.set_depth_range(12.0f, 84.0f);
    g_renderer.set_camera(px, k_cam_height, pz - k_cam_back, 0.0f, k_pitch);

    g_raster.begin_frame(g_raster.target());
    g_raster.clear_gradient(0x66, 0xAA, 0xEE, 0xAA, 0xDD, 0xEE);

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
    const int z0 = int(pz) - 12;
    const int z1 = int(pz) + 20;

    // Rows merge runs of the same material, and deepen with distance: a tile
    // eleven rows out is a few pixels tall and never needed its own quad.
    for (int y = z0; y <= z1; ) {
        const int step = (y - int(pz)) > 14 ? 4 : (y - int(pz)) > 6 ? 2 : 1;
        const int y_end = (y + step > z1 + 1) ? z1 + 1 : y + step;
        int x = x0;
        while (x <= x1) {
            const uint8_t tile = tile_clamped(z, x, y);
            int e = x;
            while (e + 1 <= x1 && tile_clamped(z, e + 1, y) == tile) e++;
            const TileDef& td = k_tiles[tile];
            uint8_t r = td.r, g = td.g, b = td.b;
            if (td.flags & k_tile_water) {
                const int wave = int(8.0f * sinf(float(t) * 0.002f + float(y)));
                r = uint8_t(r + wave); g = uint8_t(g + wave); b = uint8_t(b + wave);
            }
            ground_quad(float(x), float(y), float(e + 1), float(y_end), r, g, b);
            x = e + 1;
        }
        y = y_end;
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
            ground_quad(float(tx) + 0.2f, float(ty) + 0.2f,
                        float(tx) + 0.8f, float(ty) + 0.8f, 0xCC, 0x99, 0x55);
        }
    }

    // Props, far to near so the sprites layered on top stay sane. These read
    // the real map, not the clamped one: scenery outside it is the flat border
    // colour and nothing stands on it.
    const int px0 = x0 < 0 ? 0 : x0, px1 = x1 >= z.w ? z.w - 1 : x1;
    const int pz0 = z0 < 0 ? 0 : z0, pz1 = z1 >= z.h ? z.h - 1 : z1;
    for (int y = pz1; y >= pz0; y--) {
        for (int x = px0; x <= px1; x++) {
            const uint8_t tile = tile_at(z, x, y);
            const float wx = float(x) + 0.5f, wz = float(y) + 0.5f;
            const float far = float(y - int(pz)) > 4.0f ? 1.0f : 0.0f;
            switch (tile) {
                case tile_tree:
                    g_renderer.draw_mesh(far ? tree_far : tree, wx, 0.0f, wz,
                                         float((x * 37 + y * 11) % 6), 1.0f);
                    break;
                case tile_rock:
                    g_renderer.draw_mesh(rock, wx, 0.0f, wz, 0.0f, 1.0f);
                    break;
                case tile_house:
                    // One mesh per building, on its own top left corner only,
                    // or a four tile house draws four times.
                    if (tile_at(z, x - 1, y) != tile_house &&
                        tile_at(z, x, y - 1) != tile_house) {
                        g_renderer.draw_mesh(house, wx + 1.0f, 0.0f, wz + 0.5f,
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
        if (EventKind(e.kind) == EventKind::Sign) {
            g_renderer.draw_mesh(sign, float(e.x) + 0.5f, 0.0f, float(e.y) + 0.5f,
                                 0.0f, 1.0f);
        } else if (EventKind(e.kind) == EventKind::Item &&
                   (e.flag == k_no_flag || !flag_get(w, e.flag))) {
            g_renderer.draw_mesh(ball, float(e.x) + 0.5f, 0.2f, float(e.y) + 0.5f,
                                 0.0f, 1.0f);
        }
    }

    // The cast. Shadows first so a character never darkens the one behind it.
    for (int i = 0; i < z.npc_count; i++) {
        const NpcDef& n = z.npcs[i];
        if (!npc_present(w, n)) continue;
        ground_shadow(float(n.x) + 0.5f, float(n.y) + 0.5f, 0.45f);
    }
    ground_shadow(px, pz, 0.45f);

    for (int i = 0; i < z.npc_count; i++) {
        const NpcDef& n = z.npcs[i];
        if (!npc_present(w, n)) continue;
        int sx = 0, sy = 0, sd = 0;
        if (!g_renderer.project(float(n.x) + 0.5f, 0.0f, float(n.y) + 0.5f,
                                sx, sy, sd)) continue;
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

void hp_plate(int x, int y, const char* name, int level, int hp, int max_hp,
              bool show_numbers) {
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
    for (int i = 0; i < 9; i++) {
        g_renderer.draw_mesh(tree, -10.0f + float(i) * 2.5f, 0.0f,
                             16.0f + float(i % 3) * 2.0f, float(i), 1.6f);
    }

    // Where the two of them stand. The foe is further out than it looks like
    // it should be: it has to clear the player's own plate, which starts at
    // y 55, and standing it nearer put it behind the plate rather than above
    // it. The extra distance is paid back in scale so it still reads as a
    // creature and not as a distant speck.
    const float foe_x = 2.5f, foe_z = 6.0f;
    const float me_x = -1.8f, me_z = 0.2f;

    // The lunge. The whole turn is already resolved by the time the sim
    // reaches Attack, so this beat covers both moves; it shows whoever went
    // first stepping in, which is the part a player is watching for.
    float lunge_me = 0.0f, lunge_foe = 0.0f;
    if (b.state == BattleState::Attack) {
        const float k = sinf((1.0f - float(b.timer) / 14.0f) * 3.14159f);
        (b.player_first ? lunge_me : lunge_foe) = k * 0.9f;
    }
    const float ux = 0.595f, uz = 0.803f;   // player to foe, normalised

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
            g_renderer.draw_mesh(*mesh_for(fs.mesh), foe_x - lunge_foe * ux,
                                 0.02f + bob, foe_z - lunge_foe * uz,
                                 3.14159f, scale * 2.15f,
                                 fs.tint_r, fs.tint_g, fs.tint_b);
        }
    }
    {
        const Species& ms = k_species[me.species];
        g_renderer.draw_mesh(*mesh_for(ms.mesh), me_x + lunge_me * ux,
                             0.02f - bob, me_z + lunge_me * uz, 0.15f,
                             float(ms.scale) / 100.0f * 1.25f,
                             ms.tint_r, ms.tint_g, ms.tint_b);
    }
    if (b.state == BattleState::Throw || b.state == BattleState::Wobble ||
        b.state == BattleState::Caught) {
        const float arc = b.state == BattleState::Throw
                              ? 1.0f - float(b.timer) / 20.0f : 1.0f;
        const float bx = -1.6f + (foe_x + 1.6f) * arc;
        const float bz = -0.6f + (foe_z + 0.6f) * arc;
        const float by = 0.4f + sinf(arc * 3.14159f) * 1.4f;
        g_renderer.draw_mesh(ball, bx, by, bz, float(t) * 0.01f, 1.0f);
    }

    hp_plate(3, 4, k_species[b.foe.species].name, b.foe.level, b.foe.hp,
             b.foe.max_hp, false);
    hp_plate(61, 55, k_species[me.species].name, me.level, me.hp, me.max_hp, true);

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
    for (int i = 0; i < 9; i++) {
        g_renderer.draw_mesh(tree, -10.0f + float(i) * 2.5f, 0.0f,
                             16.0f + float(i % 3) * 2.0f, float(i), 1.6f);
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
