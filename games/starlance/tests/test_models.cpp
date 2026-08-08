// The ships, checked against the real tables tools/obj2cpp.py generates.
//
// Two things are checked, and neither can be seen by looking at the game.
//
// Winding. A face listed the wrong way round is culled from the outside and
// drawn from the inside, so the hull it belongs to has a hole in it that moves
// as you fly around it. At twelve pixels across nobody spots that, and the
// signed volume of a closed solid is negative exactly when its faces are wound
// inward, so one number per model settles it.
//
// Hardpoint seats. The subsystem offsets in sim.cpp are written in thousandths
// of the model and the models themselves are authored a thousand lines away,
// so nothing but a test connects the turret in the code to the sponson in the
// picture. Checking the seat is inside the mesh's own bounding box is the
// cheap half of that, and it is the half that catches a decimal point.

#include <cstdint>
#include <cstdio>

#include "pse/mesh.hpp"
#include "pse/raster.hpp"

#include "sim.hpp"

#include "starlance/bomber.hpp"
#include "starlance/fighter.hpp"
#include "starlance/frigate.hpp"
#include "starlance/gunship.hpp"
#include "starlance/interceptor.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, int line) {
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL line %d: %s\n", line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

struct Bounds {
    double lo[3];
    double hi[3];
};

// Signed volume by the divergence theorem, in the winding this repo's models
// use. obj2cpp takes a face normal as (c - a) x (b - a), which is the
// opposite handedness to the usual convention, so the sum below carries a
// minus sign and an outward wound solid comes out positive.
double signed_volume(const pse::MeshData& mesh) {
    double total = 0.0;
    const double scale = static_cast<double>(mesh.scale);
    for (uint16_t f = 0; f < mesh.face_count; f++) {
        const pse::MeshFace& face = mesh.faces[f];
        const pse::MeshVertex& a = mesh.vertices[face.i0];
        const pse::MeshVertex& b = mesh.vertices[face.i1];
        const pse::MeshVertex& c = mesh.vertices[face.i2];
        const double ax = a.x / scale, ay = a.y / scale, az = a.z / scale;
        const double bx = b.x / scale, by = b.y / scale, bz = b.z / scale;
        const double cx = c.x / scale, cy = c.y / scale, cz = c.z / scale;
        const double cross_x = by * cz - bz * cy;
        const double cross_y = bz * cx - bx * cz;
        const double cross_z = bx * cy - by * cx;
        total -= ax * cross_x + ay * cross_y + az * cross_z;
    }
    return total / 6.0;
}

Bounds bounds_of(const pse::MeshData& mesh) {
    Bounds out{{1e9, 1e9, 1e9}, {-1e9, -1e9, -1e9}};
    const double scale = static_cast<double>(mesh.scale);
    for (uint16_t v = 0; v < mesh.vertex_count; v++) {
        const double p[3] = {mesh.vertices[v].x / scale,
                             mesh.vertices[v].y / scale,
                             mesh.vertices[v].z / scale};
        for (int axis = 0; axis < 3; axis++) {
            if (p[axis] < out.lo[axis]) out.lo[axis] = p[axis];
            if (p[axis] > out.hi[axis]) out.hi[axis] = p[axis];
        }
    }
    return out;
}

void report(const char* name, const pse::MeshData& mesh) {
    const double volume = signed_volume(mesh);
    const Bounds b = bounds_of(mesh);
    std::printf("%-12s %3u verts %3u tris  volume %+0.5f  "
                "x[%+.2f %+.2f] y[%+.2f %+.2f] z[%+.2f %+.2f]\n",
                name, static_cast<unsigned>(mesh.vertex_count),
                static_cast<unsigned>(mesh.face_count), volume, b.lo[0],
                b.hi[0], b.lo[1], b.hi[1], b.lo[2], b.hi[2]);
    CHECK(volume > 0.0);
    CHECK(mesh.face_count > 0);
}

// Every hull is authored exactly one unit long in Z so the renderer can scale
// it by hull_length() and nothing else has to know how big a frigate is.
void check_unit_length(const char* name, const pse::MeshData& mesh) {
    const Bounds b = bounds_of(mesh);
    const double length = b.hi[2] - b.lo[2];
    if (length < 0.98 || length > 1.02) {
        std::printf("FAIL %s is %.3f long in Z, not 1.0\n", name, length);
        g_failures++;
    }
    g_checks++;
}

void test_every_hull_is_wound_outward() {
    report("interceptor", models::starlance::interceptor);
    report("fighter", models::starlance::fighter);
    report("bomber", models::starlance::bomber);
    report("gunship", models::starlance::gunship);
    report("frigate", models::starlance::frigate);

    check_unit_length("interceptor", models::starlance::interceptor);
    check_unit_length("fighter", models::starlance::fighter);
    check_unit_length("bomber", models::starlance::bomber);
    check_unit_length("gunship", models::starlance::gunship);
    check_unit_length("frigate", models::starlance::frigate);
}

// The whole frame's worth of geometry, at the heaviest the game can make it,
// against the queue that has to hold it. The queue drops the overflow
// silently, so the frame this would break is the one with the capital ship in
// it, which is the frame nobody is looking at when they call the budget fine.
void test_the_heaviest_wave_fits_the_queue() {
    const int worst = models::starlance::frigate.face_count +
                      models::starlance::gunship.face_count +
                      2 * models::starlance::fighter.face_count;
    std::printf("heaviest wave: %d triangles into a %d triangle queue\n", worst,
                pse::FrameQueue::k_capacity);
    // Half the budget, not all of it: the queue also has to hold whatever a
    // later wave grows into, and a scene that exactly fits today has no room
    // to be edited.
    CHECK(worst < pse::FrameQueue::k_capacity / 2);
}

// The seats in sim.cpp against the hulls in models/. Both are in the model's
// own coordinates, so the comparison is direct: a seat of 388 is 0.388 along a
// hull that runs from -0.5 to +0.5.
void check_seats(const char* name, sl::Hull cls, const pse::MeshData& mesh) {
    sl::World world;
    sl::world_init(world);

    // Reach a ship of this class the only way there is: put the wave that
    // carries it on the field. Building a Ship by hand here would be testing
    // a layout this file wrote rather than the one the game spawns.
    const uint8_t wave = cls == sl::Hull::Frigate ? 5 : 4;
    world.wave = wave;
    world.phase = sl::Phase::Briefing;
    world.wave_timer = 0;
    sl::Input none{};
    sl::world_tick(world, none);

    const sl::Ship* found = nullptr;
    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        if (world.ships[i].active && world.ships[i].cls == cls) {
            found = &world.ships[i];
            break;
        }
    }
    CHECK(found != nullptr);
    if (found == nullptr) return;
    CHECK(found->sub_count > 0);

    const Bounds b = bounds_of(mesh);

    for (uint8_t s = 0; s < found->sub_count; s++) {
        const sl::Subsystem& sub = found->subs[s];
        // Seats are in thousandths of the model, which is what the .obj is
        // authored in, so this is a direct comparison with no scale in it.
        const double seat[3] = {sub.ox / 1000.0, sub.oy / 1000.0,
                                sub.oz / 1000.0};
        const bool inside = seat[0] >= b.lo[0] && seat[0] <= b.hi[0] &&
                            seat[1] >= b.lo[1] && seat[1] <= b.hi[1] &&
                            seat[2] >= b.lo[2] && seat[2] <= b.hi[2];
        if (!inside) {
            std::printf("FAIL %s %s seat (%.3f %.3f %.3f) is outside the hull\n",
                        name, sl::sub_name(sub.kind), seat[0], seat[1], seat[2]);
            g_failures++;
        }
        g_checks++;
    }
    std::printf("%-12s %u hardpoints, all seated\n", name,
                static_cast<unsigned>(found->sub_count));
}

void test_hardpoints_sit_on_the_hull() {
    check_seats("gunship", sl::Hull::Gunship, models::starlance::gunship);
    check_seats("frigate", sl::Hull::Frigate, models::starlance::frigate);
}

}  // namespace

int main() {
    test_every_hull_is_wound_outward();
    test_the_heaviest_wave_fits_the_queue();
    test_hardpoints_sit_on_the_hull();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
