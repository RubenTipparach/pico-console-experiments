// Dump every track's world geometry as JSON, for tools/gen_twinflare_viewer.py
// to build the browser viewer out of.
//
// It links the GAME'S OWN sim and renderer and asks them where the ground is,
// rather than reimplementing the cross section in the exporter or in the page.
// That is the whole point of the thing: a viewer built from a second opinion
// about the geometry cannot audit the first one. ground_slice() hands back the
// boundary points draw_road builds its strips between, surface_at answers where
// the hover field would hold a pod, and both come from the binary the device
// runs.
//
// Usage: twinflare_export > tracks.json

#include <cstdio>
#include <string>

#include "fixed.hpp"
#include "render.hpp"
#include "sim.hpp"

using namespace twinflare;

namespace {

float w(int32_t v) { return v * (1.0f / 65536.0f); }

void colour(const char* name, const uint8_t c[3], bool comma = true) {
    std::printf("\"%s\":[%d,%d,%d]%s", name, c[0], c[1], c[2], comma ? "," : "");
}

void point(const float p[3]) {
    std::printf("[%.3f,%.3f,%.3f]", p[0], p[1], p[2]);
}

}  // namespace

int main() {
    std::printf("{\n\"units\":{\"spacing\":%.1f,\"shoulder_run\":%.1f,"
                "\"shoulder_drop\":%.1f,\"wall\":%.1f,\"tunnel\":%.1f,"
                "\"chasm\":%.1f,\"verge\":%.1f,\"rail\":%.1f,\"hover\":%.2f,"
                "\"crash_floor\":%.1f},\n",
                w(k_node_spacing), w(k_shoulder_run), w(k_shoulder_drop),
                w(k_wall_height), w(k_tunnel_height), w(k_chasm_depth),
                w(k_verge), w(k_rail_height), w(k_hover_height),
                w(k_crash_floor));
    std::printf("\"tracks\":[\n");
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        const Palette& p = t.palette;
        std::printf("{\"name\":\"%s\",\"laps\":%d,", t.name, t.laps);
        std::printf("\"water\":%s,",
                    has_water(t) ? std::to_string(w(water_level(t))).c_str() : "null");
        std::printf("\"world\":{\"gravity\":%d,\"grip\":%d,\"cooling\":%d,"
                    "\"air\":%d},",
                    t.world.gravity, t.world.grip, t.world.cooling, t.world.air);
        std::printf("\"palette\":{");
        colour("sky_top", p.sky_top);
        colour("sky_bottom", p.sky_bottom);
        colour("ground0", p.ground[0]);
        colour("ground1", p.ground[1]);
        colour("road0", p.road[0]);
        colour("road1", p.road[1]);
        colour("edge", p.edge);
        colour("wall", p.wall);
        colour("rock0", p.rock[0]);
        colour("rock1", p.rock[1]);
        colour("water0", p.water[0]);
        colour("water1", p.water[1]);
        colour("shallow0", p.shallow[0]);
        colour("shallow1", p.shallow[1]);
        colour("foam", p.foam, false);
        std::printf("},\n\"nodes\":[\n");
        for (uint16_t i = 0; i < t.node_count; ++i) {
            const TrackNode& n = t.nodes[i];
            GroundSlice l, r;
            ground_slice(t, i, -1.0f, l);
            ground_slice(t, i, 1.0f, r);
            std::printf("{\"i\":%u,\"flags\":%u,\"half\":%.2f,"
                        "\"c\":[%.3f,%.3f,%.3f],",
                        i, n.flags, w(node_half_width(n)),
                        w(node_x(n)), w(node_y(n)), w(node_z(n)));
            const GroundSlice* sides[2] = {&l, &r};
            const char* names[2] = {"l", "r"};
            for (int s = 0; s < 2; ++s) {
                const GroundSlice& g = *sides[s];
                std::printf("\"%s\":{\"railed\":%s,\"p\":[", names[s],
                            g.railed ? "true" : "false");
                point(g.base); std::printf(",");
                point(g.lip); std::printf(",");
                point(g.shoulder); std::printf(",");
                point(g.verge); std::printf(",");
                point(g.rail); std::printf(",");
                point(g.plain);
                std::printf("]}%s", s == 0 ? "," : "");
            }
            std::printf("}%s\n", i + 1 < t.node_count ? "," : "");
        }
        std::printf("]}%s\n", ti + 1 < k_track_count ? "," : "");
    }
    std::printf("]}\n");
    return 0;
}
