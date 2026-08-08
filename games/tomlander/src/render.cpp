#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"
#include "pse/text.hpp"

#include "tomlander/block.hpp"
#include "tomlander/facade.hpp"
#include "tomlander/cargo.hpp"
#include "tomlander/crate.hpp"
#include "tomlander/hull.hpp"
#include "tomlander/pad.hpp"
#include "tomlander/segment.hpp"
#include "tomlander/tom.hpp"
#include "tomlander/tower.hpp"

namespace tlr {
namespace {

using tl::World;

// The Rasterizer and the FrameQueue come from the engine rather than being
// declared here: on the console every game is linked into one binary, and a
// 14 KB depth buffer plus a 15 KB triangle queue per game is RAM spent on
// scenes nothing is rendering. A standalone build gets exactly one instance.
//   Rasterizer  ~14.4 KB (depth buffer)   shared
//   FrameQueue  ~15.4 KB (640 triangles)  shared
//   the rest    a few hundred bytes
pse::Rasterizer& g_raster = pse::shared_rasterizer();
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue& g_queue = pse::shared_queue();

FrameStats g_stats{};

// The textures a ScreenTriangle's index resolves against. One entry, so the
// tower's facade is index 1. The table is const and lives in flash; the
// Rasterizer only holds a pointer to it.
//
// The tower used to be a banded facade: five bands on four walls, 42
// triangles, 32 of them slivers covering about twenty pixels each. A sliver is
// the worst case for a scanline rasterizer, paying a full bounding box, three
// edge setups and three divides to fill almost nothing. As a texture it is one
// quad per wall, 10 triangles, and the detail went UP, because a picture can
// have windows in columns as well as rows.
const pse::Texture k_textures[] = {
    models::tomlander::facade,
    models::tomlander::crate,
    models::tomlander::hull,
};
constexpr uint8_t k_tex_facade = 1;   // 1 based, 0 means untextured
constexpr uint8_t k_tex_crate = 2;
constexpr uint8_t k_tex_hull = 3;

constexpr float k_pi = 3.14159265f;

// The camera sits 45 degrees above the horizontal, which is the brief. Equal
// distance and height is what makes it exactly 45; the two move together.
constexpr float k_cam_reach = 16.3f;
constexpr float k_cam_fov = 58.0f;

// Bracketed to the scene rather than left at the engine default. The depth
// buffer is one byte and a perspective curve spends nearly all of it near the
// near plane, so a 0.25 to 400 range puts the ship and the ground it is
// standing on in the same step. See Renderer3D::set_depth_range.
constexpr float k_z_near = 6.0f;
constexpr float k_z_far = 170.0f;

// Model units to world units for the ship. Its bounding box is 9.5 across, so
// it flies about 5.9 world units wide.
constexpr float k_ship_scale = 0.62f;

// The ground, as cells around the ship. This was a checkerboard of small
// quads once: 12x12 of them is 288 triangles against the ship's 204, so over
// half the frame queue went on a flat pattern whose only job was to show that
// the ship was moving. Shading does that for free. Larger cells, fewer of
// them, each one lit off its own normal.
constexpr float k_cell = 20.0f;
constexpr int k_cells = 4;          // either side: 8x8 cells, 128 triangles

constexpr float k_fp16 = 1.0f / 65536.0f;

float to_f(int32_t fp) { return fp * k_fp16; }

// Nearest integer. A plain cast truncates toward zero, which biases a plume
// walking up and left by half a pixel per step against one walking down and
// right, and a flame that is symmetric in the maths is then not symmetric on
// screen.
int round_i(float v) {
    return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
}

struct Rgb { uint8_t r, g, b; };

Rgb lerp_rgb(Rgb a, Rgb b, int t256) {
    return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * t256 / 256),
               static_cast<uint8_t>(a.g + (b.g - a.g) * t256 / 256),
               static_cast<uint8_t>(a.b + (b.b - a.b) * t256 / 256)};
}

// Ground colour by height. Per VERTEX, not per face, because ScreenTriangle
// already carries three colours and the rasterizer already interpolates them:
// a gradient across the landscape is free, and it is what stops eight metre
// cells reading as eight metre cells.
// Mars, so the whole ramp is one rusted hue and only its value moves. Iron
// oxide dust is the same mineral everywhere, and a real Martian view varies by
// how much light a slope catches rather than by changing colour, which is also
// what makes the ramp read as one planet instead of three biomes: the palette
// this replaced ran teal to brown to near white, so a basin, a plain and a
// ridge looked like different worlds bolted together.
//
// The hue is held slightly toward orange rather than pushed to pure red.
// Mars is butterscotch and rust in true colour, and the saturated red everyone
// pictures is a contrast stretched press image, so a screen full of it would
// look like a filter rather than a place.
Rgb ground_colour(float height) {
    const Rgb low{58, 30, 26};        // basins, deep in shadow
    const Rgb mid{142, 76, 47};       // the general regolith, iron oxide
    const Rgb high{206, 152, 106};    // ridge tops catching the light, dust
    if (height < 0.0f) {
        int t = static_cast<int>((height + 9.0f) * 256.0f / 9.0f);
        t = t < 0 ? 0 : (t > 256 ? 256 : t);
        return lerp_rgb(low, mid, t);
    }
    int t = static_cast<int>(height * 256.0f / 10.0f);
    t = t < 0 ? 0 : (t > 256 ? 256 : t);
    return lerp_rgb(mid, high, t);
}

// Standing water. One flat tone rather than a ramp: a clamped surface has no
// height variation left to shade by, and the lambert the ground triangles get
// is what stops it reading as a painted hole.
const Rgb k_water{34, 58, 92};

// One ground triangle: three projected corners with their own colours.
void ground_tri(const int sx[3], const int sy[3], const int sz[3],
                const Rgb c[3]) {
    pse::ScreenTriangle tri;
    tri.x0 = static_cast<int16_t>(sx[0]);
    tri.y0 = static_cast<int16_t>(sy[0]);
    tri.z0 = static_cast<uint16_t>(sz[0]);
    tri.x1 = static_cast<int16_t>(sx[1]);
    tri.y1 = static_cast<int16_t>(sy[1]);
    tri.z1 = static_cast<uint16_t>(sz[1]);
    tri.x2 = static_cast<int16_t>(sx[2]);
    tri.y2 = static_cast<int16_t>(sy[2]);
    tri.z2 = static_cast<uint16_t>(sz[2]);
    tri.r0 = c[0].r; tri.g0 = c[0].g; tri.b0 = c[0].b;
    tri.r1 = c[1].r; tri.g1 = c[1].g; tri.b1 = c[1].b;
    tri.r2 = c[2].r; tri.g2 = c[2].g; tri.b2 = c[2].b;
    g_raster.draw(tri);
}

void draw_ground(const World& world) {
    const int base_x = static_cast<int>(std::floor(to_f(world.x) / k_cell));
    const int base_z = static_cast<int>(std::floor(to_f(world.z) / k_cell));

    for (int cz = base_z - k_cells; cz < base_z + k_cells; cz++) {
        for (int cx = base_x - k_cells; cx < base_x + k_cells; cx++) {
            const float x0 = cx * k_cell, x1 = x0 + k_cell;
            const float z0 = cz * k_cell, z1 = z0 + k_cell;
            const float wx[4] = {x0, x1, x1, x0};
            const float wz[4] = {z0, z0, z1, z1};

            int sx[4], sy[4], sz[4];
            float hy[4];
            Rgb col[4];
            bool visible = true;
            const float sea = to_f(world.sea);
            for (int i = 0; i < 4; i++) {
                hy[i] = to_f(tl::terrain_height(
                    world, static_cast<int32_t>(wx[i] * 65536.0f),
                    static_cast<int32_t>(wz[i] * 65536.0f)));
                // The sea is the same clamp the collision floor uses, so the
                // surface the ship splashes on is the surface that is drawn.
                // Costs no triangles: the ground mesh already had vertices
                // here and they simply stop going down.
                if (hy[i] < sea) {
                    hy[i] = sea;
                    col[i] = k_water;
                } else {
                    col[i] = ground_colour(hy[i]);
                }
                if (!g_renderer.project(wx[i], hy[i], wz[i],
                                        sx[i], sy[i], sz[i])) {
                    visible = false;
                    break;
                }
            }
            if (!visible) continue;

            // Alternate which way the cell splits. Cut every one the same way
            // and the shading lines up into stripes, and the surface reads as
            // a grid again, which is the thing being got rid of.
            const int a[2][3] = {{0, 1, 2}, {0, 2, 3}};
            const int b[2][3] = {{1, 2, 3}, {1, 3, 0}};
            const int (*split)[3] = ((cx + cz) & 1) ? b : a;
            for (int t = 0; t < 2; t++) {
                const int i0 = split[t][0], i1 = split[t][1], i2 = split[t][2];

                // Height alone made the landscape mush: without a slope term
                // a hillside is the same colour as the flat beside it, and at
                // twenty unit cells the gradient just reads as fog. So each
                // triangle gets a lambert off its own normal, applied to all
                // three of its vertex colours. The colours still vary per
                // vertex, so the shape reads AND the facet does.
                const float hx0 = wx[i0], hz0 = wz[i0];
                const float e1x = wx[i1] - hx0, e1z = wz[i1] - hz0;
                const float e2x = wx[i2] - hx0, e2z = wz[i2] - hz0;
                const float e1y = hy[i1] - hy[i0], e2y = hy[i2] - hy[i0];
                float nx = e1y * e2z - e1z * e2y;
                float ny = e1z * e2x - e1x * e2z;
                float nz = e1x * e2y - e1y * e2x;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
                if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }  // faces up
                float lambert = nx * 0.42f + ny * 0.82f + nz * -0.39f;
                if (lambert < 0.0f) lambert = 0.0f;
                const int shade256 =
                    static_cast<int>((0.52f + 0.48f * lambert) * 256.0f);

                Rgb tc[3] = {col[i0], col[i1], col[i2]};
                for (int c = 0; c < 3; c++) {
                    tc[c].r = static_cast<uint8_t>(tc[c].r * shade256 / 256);
                    tc[c].g = static_cast<uint8_t>(tc[c].g * shade256 / 256);
                    tc[c].b = static_cast<uint8_t>(tc[c].b * shade256 / 256);
                }
                const int tx[3] = {sx[i0], sx[i1], sx[i2]};
                const int ty[3] = {sy[i0], sy[i1], sy[i2]};
                const int tz[3] = {sz[i0], sz[i1], sz[i2]};
                ground_tri(tx, ty, tz, tc);
            }
        }
    }
}

// The pads, as the model they are.
//
// This was a draw_box with a hand laid deck over its lid, and that lid is what
// made the pad mottle. draw_box always paints a top face, so the pad carried
// two horizontal surfaces 0.048 units apart, and this depth buffer cannot tell
// them apart: one byte over the whole 6 to 170 range means that at the 23
// units the camera works at, 0.048 of vertical separation is about a fifth of
// a single depth step. The two tied nearly everywhere, ties go to whoever drew
// first, the lid drew first, and the deck lost most of its own surface to the
// grey underneath it.
//
// Widening the gap would not have fixed it. A gap big enough to win at 23
// units is still too small at 40, and the pads are looked at from across the
// valley. pad.obj has one surface up there and nothing under it, so there is
// nothing left to tie with.
//
// The deck standing proud of its apron is a different question and it still
// earns its 2.4 units: apron to deck is about ten depth steps, which separates
// cleanly. That is why k_pad_rise exists and it has not moved.
//
// Model space is world space here. pad.obj is authored at the real 7 by 2.4,
// so this draws at scale 1.0 and preview.cpp holds the model to k_pad_half and
// k_pad_rise rather than trusting the two to stay in step on their own.
void draw_pads(const World& world) {
    for (int i = 0; i < tl::k_pad_count; i++) {
        const tl::Pad& pad = world.pads[i];
        // A deck out past the waterline is not a deck, it is the rocket
        // section the salvage is about. Same landing square either way, so the
        // sim never has to know which mesh got drawn.
        const bool floating =
            world.sea > tl::k_no_sea && pad.y <= world.sea + tl::k_float_rise;
        if (floating) {
            // Mission three duplicates pad 0 into slot 2, so the shore deck
            // does not get drawn twice.
            if (i > 0 && pad.x == world.pads[0].x &&
                pad.z == world.pads[0].z) {
                continue;
            }
            g_renderer.draw_mesh(models::tomlander::segment,
                                 to_f(pad.x), to_f(pad.y), to_f(pad.z),
                                 0.0f, 1.0f, 255, 255, 255, 0.0f, 0, 0.0f,
                                 k_tex_hull);
            continue;
        }
        if (i > 0 && pad.x == world.pads[0].x && pad.z == world.pads[0].z) {
            continue;
        }
        g_renderer.draw_mesh(models::tomlander::pad,
                             to_f(pad.x), to_f(pad.y), to_f(pad.z),
                             0.0f, 1.0f);
    }
}

// The valley's buildings. Placement comes from tl::k_buildings, the same table
// ground_at collides against, so a building can never be drawn somewhere the
// ship would fly straight through it.
//
// Both meshes are one unit half width, so `half` is the uniform scale, and the
// tint darkens some of them so a row does not read as one repeated object.
// Culling is the renderer's: a building behind the camera or past the far
// plane costs a handful of rejected vertices and no triangles.
void draw_buildings(const World& world) {
    for (int i = 0; i < tl::k_building_count; i++) {
        const tl::Building& b = tl::k_buildings[i];
        const float x = static_cast<float>(b.x);
        const float z = static_cast<float>(b.z);
        // Founded on the untouched landscape, the same height ground_at uses
        // for its roof, so the mesh and the collision share a floor.
        const float y = to_f(tl::terrain_height(
            world, b.x * 65536, b.z * 65536));
        g_renderer.draw_mesh(b.tower ? models::tomlander::tower
                                     : models::tomlander::block,
                             x, y, z, 0.0f, static_cast<float>(b.half),
                             b.tint, b.tint, b.tint, 0.0f, 0, 0.0f,
                             b.tower ? k_tex_facade : 0);
    }
}

// The crate, wherever it currently is: sitting on the deck it was left on, or
// slung under the hull once the ship has it. Slung means it turns with the
// hull, which is why it goes through the same basis the ship does.
void draw_cargo(const World& world) {
    if (world.cargo == tl::kCargoNone || world.cargo == tl::kCargoDone) return;

    if (tl::carrying(world)) {
        const pse::Basis b = pse::quat_basis(world.q);
        // Tucked under the belly, between the legs. The hull's feet reach
        // 1.85 below its origin and the crate is 1.7 tall, so a base at -1.72
        // puts the whole crate inside the leg span: it never reaches through
        // the deck on touchdown, and it never has to be drawn at a different
        // size from the one sitting on the pad.
        const float my = -1.72f;
        g_renderer.draw_mesh(models::tomlander::cargo,
                             to_f(world.x) + b.m[1] * my,
                             to_f(world.y) + b.m[4] * my,
                             to_f(world.z) + b.m[7] * my,
                             b, 1.0f, 255, 255, 255, 0, k_tex_crate);
        return;
    }

    const tl::Pad& pad = world.pads[world.cargo];
    g_renderer.draw_mesh(models::tomlander::cargo,
                         to_f(pad.x), to_f(pad.y) + to_f(tl::k_pad_rise),
                         to_f(pad.z), 0.0f, 1.0f,
                         255, 255, 255, 0.0f, 0, 0.0f, k_tex_crate);
}

// The fuel crates, as green cubes hanging in the air.
//
// draw_box rather than a model, which is rule 11's stated exception: a cube is
// a primitive the engine already generates parametrically, and an .obj of one
// would be six quads nobody will ever open in Blender. Twelve triangles each,
// and a mission never places more than three.
//
// Green because nothing else in the scene is. The ground runs one rusted hue
// from basin to ridge, the decks are grey steel and the sky is butterscotch,
// so the one saturated green thing on Mars is unmistakably an object rather
// than scenery. The lid is the brighter of the two tones, which is the light
// direction every other box in this game is shaded by.
void draw_crates(const World& world) {
    for (int i = 0; i < world.crate_count; i++) {
        const tl::FuelCrate& c = world.crates[i];
        if (c.state != tl::Crate::Out) continue;
        const float half = 1.6f;
        // draw_box's y is the BASE of the box, not its middle: the unit cube
        // it builds from runs 0 to 1 in y so that a building sits on the
        // ground. A crate hangs in the air and its middle is the point the
        // pickup is measured from, so the base has to be dropped by a half or
        // the cube is drawn 1.6 units above the thing you fly into.
        g_renderer.draw_box(to_f(c.x), to_f(c.y) - half, to_f(c.z),
                            half * 2.0f, half * 2.0f, half * 2.0f,
                            96, 240, 112,       // lid
                            34, 168, 68);       // sides
    }
}

void draw_ship(const World& world) {
    // The attitude straight through, as the orientation it is. No angles are
    // extracted on the way, which is the point: extracting them would put the
    // singularity back in the one place it would be hardest to spot, the
    // picture.
    g_renderer.draw_mesh(models::tomlander::tom,
                         to_f(world.x), to_f(world.y), to_f(world.z),
                         pse::quat_basis(world.q), k_ship_scale);
}

// How far below the nozzle the plume's aim is sampled, in world units. Only
// its direction is kept, so the number just has to be far enough that the two
// projected points are several pixels apart and the direction is not noise.
constexpr float k_plume_probe = 3.0f;

// Thruster flames, as billboards drawn after the geometry with a depth test,
// which is how the engine expects sprites to go on.
//
// A plume leaves along the hull's own down axis, so it has to be aimed, not
// just placed. The nozzle mouth was rotated with the hull from the start and
// the flame was then drawn straight down the screen from it, which reads as
// correct only while the ship is level: lean it over and the exhaust still
// falls vertically out of a tilted nozzle. Both ends of the plume go through
// the projection now, and the pixels walk the line between them.
void draw_flames(const World& world, const pse::RenderTarget& target,
                 uint32_t time_ms) {
    // One basis for the nozzle mouths and the plume direction both, the same
    // one the hull is drawn with. When these were rebuilt out of the sim's
    // angles instead, the flames were a second copy of the attitude
    // convention, and a second copy is a thing to get out of step.
    const pse::Basis b = pse::quat_basis(world.q);

    // The hull's own down, in the world.
    const float dx = -b.m[1], dy = -b.m[4], dz = -b.m[7];

    for (int i = 0; i < tl::kPodCount; i++) {
        g_stats.flame_ax[i] = 0;
        g_stats.flame_ay[i] = 0;
        const uint8_t throttle = world.throttle[i];
        if (throttle < 8) continue;

        // The nozzle mouth in model space, through the hull's basis.
        const float mx = tl::k_pods[i].ox * 4.0f * k_ship_scale;
        const float mz = tl::k_pods[i].oz * 4.0f * k_ship_scale;
        const float my = -3.1f * k_ship_scale;

        const float wx = to_f(world.x) + b.m[0]*mx + b.m[1]*my + b.m[2]*mz;
        const float wy = to_f(world.y) + b.m[3]*mx + b.m[4]*my + b.m[5]*mz;
        const float wz = to_f(world.z) + b.m[6]*mx + b.m[7]*my + b.m[8]*mz;

        int px, py;
        float scale;
        uint8_t depth;
        if (!g_renderer.project_billboard(wx, wy, wz, 1.0f, px, py, scale,
                                          depth)) {
            continue;
        }

        // Down the screen is the fallback, for the one case the aim point has
        // nothing to say: a nozzle pointing at the camera projects both ends
        // to nearly the same pixel, and normalising that is dividing noise.
        float ax = 0.0f, ay = 1.0f;
        int qx, qy;
        float qscale;
        uint8_t qdepth;
        if (g_renderer.project_billboard(wx + dx * k_plume_probe,
                                         wy + dy * k_plume_probe,
                                         wz + dz * k_plume_probe,
                                         1.0f, qx, qy, qscale, qdepth)) {
            const float vx = static_cast<float>(qx - px);
            const float vy = static_cast<float>(qy - py);
            const float mag = std::sqrt(vx * vx + vy * vy);
            if (mag > 1.5f) {
                ax = vx / mag;
                ay = vy / mag;
            }
        }
        // The plume's width runs across it, whichever way it is pointing.
        const float bx = -ay, by = ax;
        g_stats.flame_ax[i] = static_cast<int8_t>(round_i(ax * 64.0f));
        g_stats.flame_ay[i] = static_cast<int8_t>(round_i(ay * 64.0f));

        const int flick = static_cast<int>((time_ms / 40 + i * 3) % 3);
        const int len = 2 + (throttle * 7) / 255 + flick;
        for (int k = 0; k < len; k++) {
            const int half = 1 + (2 * (len - k)) / (len * 2 + 1);
            // White at the throat, then yellow, then orange.
            const uint8_t r = 255;
            const uint8_t g = k * 3 < len ? 255 : (k * 3 < len * 2 ? 236 : 163);
            const uint8_t b = k * 3 < len ? 232 : (k * 3 < len * 2 ? 39 : 0);
            const float cxf = px + ax * static_cast<float>(k);
            const float cyf = py + ay * static_cast<float>(k);
            for (int w = -half; w <= half; w++) {
                const float fw = static_cast<float>(w);
                const int x = round_i(cxf + bx * fw);
                const int yy = round_i(cyf + by * fw);
                if (x < 0 || yy < 0 || x >= target.width ||
                    yy >= target.height) {
                    continue;
                }
                if (g_raster.test_and_set_depth(x, yy, depth)) {
                    g_raster.plot(x, yy, r, g, b);
                }
            }
        }
    }
}

// An arrow at the edge of the screen for the deck you cannot see, and only
// then: a marker stuck on top of something already in frame is the decorative
// status line rule 9 exists to keep off this screen. With the pads no longer
// marking themselves, the HUD's own range readout is what names the target
// while it is on screen.
//
// Direction comes from the world bearing rotated into the camera's own frame,
// NOT from the projection, because a pad behind the camera has no valid
// projection at all and behind the camera is when an arrow earns its pixels.
void draw_target_arrow(const World& world, const pse::RenderTarget& target,
                       float yaw) {
    g_stats.arrow_x = -1;
    g_stats.arrow_y = -1;
    if (world.state != tl::Flight::Flying) return;

    const tl::Pad& pad = world.pads[world.target];
    const float deck_y = to_f(pad.y) + to_f(tl::k_pad_rise);

    int px, py, pz;
    const bool on_screen =
        g_renderer.project(to_f(pad.x), deck_y, to_f(pad.z), px, py, pz);
    g_stats.pad_visible = on_screen;
    g_stats.pad_x = static_cast<int16_t>(on_screen ? px : -1);
    g_stats.pad_y = static_cast<int16_t>(on_screen ? py : -1);

    const int margin = 13;
    if (on_screen && px >= margin && px <= target.width - margin &&
        py >= margin && py <= target.height - margin) {
        return;
    }

    const float dx = to_f(pad.x) - to_f(world.x);
    const float dz = to_f(pad.z) - to_f(world.z);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float right = dx * cy - dz * sy;
    const float fwd = dx * sy + dz * cy;
    float ax = right, ay = -fwd;             // screen y counts downward
    const float len = std::sqrt(ax * ax + ay * ay);
    if (len < 1e-4f) return;
    ax /= len;
    ay /= len;

    const float half_w = target.width * 0.5f - 9.0f;
    const float half_h = target.height * 0.5f - 9.0f;
    const float tx = std::fabs(ax) > 1e-4f ? half_w / std::fabs(ax) : 1e9f;
    const float tz = std::fabs(ay) > 1e-4f ? half_h / std::fabs(ay) : 1e9f;
    const float t = tx < tz ? tx : tz;
    const float ex = target.width * 0.5f + ax * t;
    const float ey = target.height * 0.5f + ay * t;
    g_stats.arrow_x = static_cast<int16_t>(ex);
    g_stats.arrow_y = static_cast<int16_t>(ey);

    // A small solid triangle, filled by walking out from the tip. No depth
    // test: HUD furniture sits on top of the world by definition.
    //
    // Drawn TWICE, a dark halo then the bright core, and that is not
    // decoration. On a rust planet no single flat colour is readable against
    // everything: the ground ramp alone spans luminance 58 to 206 and the sky
    // meets it at the horizon, so a colour with contrast against the shadowed
    // basins has almost none against a lit ridge. Measured against the five
    // tones in this scene, the best flat colour available managed a worst case
    // contrast ratio of 1.48, and the orange this used to be managed 1.06
    // against the horizon, which is invisible.
    //
    // An outline sidesteps the whole problem. Whatever the arrow is over, one
    // of the two tones contrasts with it, so the shape reads against every
    // background instead of against most of them. It costs one more pass over
    // about forty pixels, once a frame, and only when the arrow is drawn at
    // all.
    const float perp_x = -ay, perp_y = ax;
    struct Pass { int grow; uint8_t r, g, b; };
    const Pass passes[2] = {
        {1, 24, 12, 10},        // the halo, near black
        {0, 255, 214, 92},      // the core, bright warm
    };
    for (const Pass& pass : passes) {
        for (int step = 0; step <= 7; step++) {
            const float cx = ex + ax * (4.0f - step);
            const float cyp = ey + ay * (4.0f - step);
            const int spread = step / 2 + pass.grow;
            for (int w = -spread; w <= spread; w++) {
                // The halo also steps one pixel along the arrow's own axis at
                // each end, so the tip and the tail are outlined too rather
                // than just the flanks.
                for (int along = -pass.grow; along <= pass.grow; along++) {
                    pse::plot_pixel(
                        target,
                        static_cast<int>(cx + perp_x * w + ax * along),
                        static_cast<int>(cyp + perp_y * w + ay * along),
                        pass.r, pass.g, pass.b);
                }
            }
        }
    }
}

}  // namespace

FrameStats last_frame_stats() { return g_stats; }

void render_scene(const World& world, const pse::RenderTarget& target,
                  float yaw, uint32_t time_ms) {
    g_queue.reset();
    g_raster.set_textures(k_textures,
                          static_cast<uint8_t>(sizeof(k_textures) /
                                               sizeof(k_textures[0])));
    g_raster.begin_frame_collect(target, g_queue);

    g_renderer.set_fov(k_cam_fov);
    g_renderer.set_depth_range(k_z_near, k_z_far);
    // Equal reach and height is what makes the camera exactly 45 degrees
    // above the horizontal. look_lift 0: this camera studies one object, and
    // the chase camera's default whole unit drops the ship out of frame.
    g_renderer.set_orbit_camera(to_f(world.x), to_f(world.y) - 2.0f,
                                to_f(world.z), yaw,
                                k_cam_reach, k_cam_reach, 0.0f);

    draw_ground(world);
    draw_buildings(world);
    draw_pads(world);
    draw_cargo(world);
    draw_crates(world);
    draw_ship(world);

    // A Martian sky, and it had to move when the ground did. The whole point
    // of this gradient is that the terrain FOGS into it rather than ending on
    // a line, and the dusk blue that was here stopped doing that the moment
    // the ground turned rust: bright orange meeting near black navy is a hard
    // horizon, not a haze.
    //
    // So the bottom of the sky is within a few counts of the ground's own lit
    // ridge tone, which is what closes the seam. Dust suspended in a thin
    // atmosphere is why a real Martian sky is butterscotch rather than blue,
    // and it darkens with altitude because there is less of it overhead than
    // along a sight line to the horizon.
    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{74, 56, 52, 190, 144, 108});
    g_raster.end_collect();

    g_stats.queued = g_queue.count;
    g_stats.dropped = g_queue.dropped;

    draw_flames(world, target, time_ms);
    draw_target_arrow(world, target, yaw);
}

}  // namespace tlr
