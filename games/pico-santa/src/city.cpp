#include "city.hpp"

#include <cmath>

namespace santa {
namespace {

// Deterministic LCG. The stock rand() is not guaranteed to match between the
// device build and the web build, and a street that differs per platform makes
// bug reports useless.
uint32_t next_random(uint32_t& state) {
    state = state * 1103515245u + 12345u;
    return (state >> 16) & 0x7FFF;
}

const uint8_t k_wall_colours[6][3] = {
    {180, 100, 100}, {100, 130, 180}, {150, 150, 120},
    {120, 160, 120}, {180, 150, 100}, {140, 140, 160},
};

const uint8_t k_roof_colours[6][3] = {
    {120, 60, 60}, {60, 80, 120}, {100, 100, 80},
    {80, 110, 80}, {130, 100, 60}, {100, 100, 120},
};

const uint8_t k_gem_colours[3][3] = {
    {255, 60, 60}, {60, 255, 90}, {70, 150, 255},
};

constexpr float k_chunk_span = City::k_chunk_tiles * City::k_tile_size;

}  // namespace

void City::reset(uint32_t seed) {
    seed_ = seed;
    active_buildings_ = 0;

    for (auto& building : buildings_) building.active = false;
    for (auto& gem : gems_) gem.active = false;

    chunk_left_ = -1;
    chunk_right_ = 2;
    for (int chunk = chunk_left_; chunk <= chunk_right_; chunk++) {
        generate_chunk(chunk);
    }
}

int City::free_building_slot() const {
    for (int i = 0; i < k_max_buildings; i++) {
        if (!buildings_[i].active) return i;
    }
    return -1;
}

int City::free_gem_slot() const {
    for (int i = 0; i < k_max_gems; i++) {
        if (!gems_[i].active) return i;
    }
    return -1;
}

void City::generate_chunk(int chunk) {
    // Seeded per chunk, so a chunk regenerates identically after being unloaded
    // and revisited.
    uint32_t state = seed_ + static_cast<uint32_t>(chunk) * 7919u;
    const float chunk_start = chunk * k_chunk_span;

    for (int tile = 0; tile < k_chunk_tiles; tile++) {
        if (next_random(state) % 4 == 0) continue;

        const float world_x = chunk_start + tile * k_tile_size;

        for (int side = 0; side < 2; side++) {
            if (next_random(state) % 3 == 0) continue;

            const int slot = free_building_slot();
            if (slot < 0) continue;

            Building& building = buildings_[slot];
            const float offset = 4.0f + static_cast<float>(next_random(state) % 3);
            building.x = world_x;
            building.z = side == 0 ? -offset : offset;
            building.width = 1.5f + (next_random(state) % 100) / 100.0f;
            building.depth = 1.5f + (next_random(state) % 100) / 100.0f;
            building.height = 2.0f + static_cast<float>(next_random(state) % 8);

            const int palette = next_random(state) % 6;
            building.wall_r = k_wall_colours[palette][0];
            building.wall_g = k_wall_colours[palette][1];
            building.wall_b = k_wall_colours[palette][2];
            building.roof_r = k_roof_colours[palette][0];
            building.roof_g = k_roof_colours[palette][1];
            building.roof_b = k_roof_colours[palette][2];

            building.active = true;
            building.chunk = chunk;
            active_buildings_++;
        }

        if (next_random(state) % 5 == 0) {
            const int slot = free_gem_slot();
            if (slot < 0) continue;

            Gem& gem = gems_[slot];
            gem.x = world_x + (next_random(state) % 100) / 50.0f - 1.0f;
            gem.y = 0.6f;
            gem.z = (next_random(state) % 100) / 50.0f - 1.0f;
            gem.type = static_cast<uint8_t>(next_random(state) % 3);
            gem.active = true;
            gem.chunk = chunk;
        }
    }
}

void City::remove_chunk(int chunk) {
    for (auto& building : buildings_) {
        if (building.active && building.chunk == chunk) {
            building.active = false;
            active_buildings_--;
        }
    }
    for (auto& gem : gems_) {
        if (gem.active && gem.chunk == chunk) gem.active = false;
    }
}

void City::update(float camera_x) {
    const int centre = static_cast<int>(floorf(camera_x / k_chunk_span));
    const int wanted_left = centre - 1;
    const int wanted_right = centre + 2;

    while (chunk_left_ < wanted_left) remove_chunk(chunk_left_++);
    while (chunk_right_ > wanted_right) remove_chunk(chunk_right_--);
    while (chunk_left_ > wanted_left) generate_chunk(--chunk_left_);
    while (chunk_right_ < wanted_right) generate_chunk(++chunk_right_);
}

void City::render(pse::Renderer3D& renderer) const {
    for (const auto& building : buildings_) {
        if (!building.active) continue;
        renderer.draw_box(building.x, 0.0f, building.z,
                          building.width, building.height, building.depth,
                          building.roof_r, building.roof_g, building.roof_b,
                          building.wall_r, building.wall_g, building.wall_b);
    }
}

void City::render_gems(pse::Renderer3D& renderer, const pse::MeshData& gem_model,
                       uint32_t time_ms) const {
    // One spin and bob phase for every gem. Per gem phases would look better
    // and cost a sinf per gem per frame on a chip with no FPU, which is not a
    // trade worth making for a collectible.
    const float phase = time_ms * 0.003f;
    const float bob = sinf(phase) * 0.15f;
    const float spin = phase;

    for (const auto& gem : gems_) {
        if (!gem.active) continue;
        // One gem mesh in flash, tinted per type. Three separate models would
        // be three times the flash for the same silhouette.
        const uint8_t* colour = k_gem_colours[gem.type];
        renderer.draw_mesh(gem_model, gem.x, gem.y + bob, gem.z,
                           spin, 0.55f, colour[0], colour[1], colour[2]);
    }
}

bool City::collides(float x, float z, float radius) const {
    for (const auto& building : buildings_) {
        if (!building.active) continue;
        const float half_width = building.width * 0.5f + radius;
        const float half_depth = building.depth * 0.5f + radius;
        if (fabsf(x - building.x) < half_width &&
            fabsf(z - building.z) < half_depth) {
            return true;
        }
    }
    return false;
}

int City::collect_gems(float x, float z, float radius) {
    int points = 0;
    const float radius_squared = radius * radius;

    for (auto& gem : gems_) {
        if (!gem.active) continue;
        const float dx = x - gem.x;
        const float dz = z - gem.z;
        if (dx * dx + dz * dz < radius_squared) {
            gem.active = false;
            points += (gem.type + 1) * 10;
        }
    }
    return points;
}

}  // namespace santa
