#pragma once

#include <cstdint>

#include "pse/mesh.hpp"
#include "pse/renderer3d.hpp"

namespace santa {

// The endless street the player drives down.
//
// The picosystem version of this kept its buildings and gems in file scope
// arrays that game.cpp reached into directly. Everything is owned here now, so
// the world can be reset, tested, or instantiated twice without the rest of the
// game noticing.
class City {
public:
    static constexpr int k_chunk_tiles = 10;
    static constexpr int k_max_buildings = 32;
    static constexpr int k_max_gems = 50;
    static constexpr float k_tile_size = 2.0f;

    // Build the starting chunks. The same seed always gives the same street.
    void reset(uint32_t seed);

    // Load and unload chunks so the street exists around this position.
    void update(float camera_x);

    void render(pse::Renderer3D& renderer) const;

    // Gems are a mesh from models/gem.obj, spun and bobbed by elapsed time.
    void render_gems(pse::Renderer3D& renderer, const pse::MeshData& gem_model,
                     uint32_t time_ms) const;

    bool collides(float x, float z, float radius) const;

    // Collects everything in range and returns the points scored.
    int collect_gems(float x, float z, float radius);

    int active_buildings() const { return active_buildings_; }

private:
    struct Building {
        float x, z;
        float width, depth, height;
        uint8_t roof_r, roof_g, roof_b;
        uint8_t wall_r, wall_g, wall_b;
        bool active;
        int chunk;
    };

    struct Gem {
        float x, y, z;
        uint8_t type;
        bool active;
        int chunk;
    };

    void generate_chunk(int chunk);
    void remove_chunk(int chunk);
    int free_building_slot() const;
    int free_gem_slot() const;

    Building buildings_[k_max_buildings] = {};
    Gem gems_[k_max_gems] = {};
    int active_buildings_ = 0;
    uint32_t seed_ = 0;
    int chunk_left_ = 0;
    int chunk_right_ = 0;
};

}  // namespace santa
