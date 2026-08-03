#include "game.hpp"
#include <cstdlib>
#include <cmath>

using namespace blit;

// Screen dimensions - 240x240 for PicoSystem
const int SCREEN_W = 240;
const int SCREEN_H = 240;

// Tile size
const int TILE_SIZE = 12;
const int TILES_Y = SCREEN_H / TILE_SIZE;  // 20 tiles vertically

// Chunk-based infinite level system
const int CHUNK_WIDTH = 10;  // tiles per chunk
const int NUM_CHUNKS = 5;    // chunks to keep in memory
const int LEVEL_WIDTH = CHUNK_WIDTH * NUM_CHUNKS;  // 50 tiles wide buffer

// Level data - circular buffer of chunks
uint8_t level[LEVEL_WIDTH * TILES_Y];
int chunk_offset = 0;  // which chunk index is leftmost in buffer
int world_chunk_offset = 0;  // world position of leftmost chunk

// Camera position (in world pixels)
float camera_x = 0;

// Player state (world coordinates)
struct Player {
    float x, y;
    float vx, vy;
    bool on_ground;
    bool facing_right;
} player;

// Gem structure (world coordinates)
struct Gem {
    float x, y;
    bool collected;
    bool active;
    uint8_t type;  // 0=red, 1=green, 2=blue
};

const int MAX_GEMS = 50;
Gem gems[MAX_GEMS];
int score = 0;

// Physics constants
const float GRAVITY = 0.3f;
const float JUMP_FORCE = -5.5f;
const float MOVE_SPEED = 2.0f;
const float FRICTION = 0.85f;

// Random seed
uint32_t seed = 34125;
uint32_t chunk_seed = 34125;

uint32_t random_next() {
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) & 0x7FFF;
}

// Seeded random for deterministic chunk generation
uint32_t chunk_random(int chunk_id) {
    uint32_t s = chunk_seed + chunk_id * 7919;
    s = s * 1103515245 + 12345;
    return (s >> 16) & 0x7FFF;
}

uint32_t chunk_random_next(uint32_t &s) {
    s = s * 1103515245 + 12345;
    return (s >> 16) & 0x7FFF;
}

// Generate a single chunk of level
void generate_chunk(int chunk_id, int buffer_chunk_index) {
    int base_x = buffer_chunk_index * CHUNK_WIDTH;
    
    // Use deterministic seed for this chunk
    uint32_t cseed = chunk_seed + chunk_id * 7919;
    
    // Clear chunk
    for (int y = 0; y < TILES_Y; y++) {
        for (int x = 0; x < CHUNK_WIDTH; x++) {
            level[y * LEVEL_WIDTH + base_x + x] = 0;
        }
    }
    
    // Bottom floor (always present)
    for (int x = 0; x < CHUNK_WIDTH; x++) {
        level[(TILES_Y - 1) * LEVEL_WIDTH + base_x + x] = 1;
    }
    
    // Generate platforms for this chunk
    int platform_y = TILES_Y - 4;
    while (platform_y > 2) {
        int platform_x = chunk_random_next(cseed) % (CHUNK_WIDTH - 2);
        int platform_len = chunk_random_next(cseed) % 4 + 2;
        
        for (int i = 0; i < platform_len && platform_x + i < CHUNK_WIDTH; i++) {
            level[platform_y * LEVEL_WIDTH + base_x + platform_x + i] = 1;
        }
        
        platform_y -= chunk_random_next(cseed) % 2 + 2;
    }
    
    // Add gems on platforms
    int world_base_x = chunk_id * CHUNK_WIDTH * TILE_SIZE;
    for (int y = 0; y < TILES_Y - 1; y++) {
        for (int x = 0; x < CHUNK_WIDTH; x++) {
            // Check if there's a platform below and empty space here
            if (level[y * LEVEL_WIDTH + base_x + x] == 0 &&
                level[(y + 1) * LEVEL_WIDTH + base_x + x] == 1) {
                    if (chunk_random_next(cseed) % 4 == 0) {
                        // Find empty gem slot
                        for (int i = 0; i < MAX_GEMS; i++) {
                            if (!gems[i].active) {
                                gems[i].x = world_base_x + x * TILE_SIZE + TILE_SIZE / 2;
                                gems[i].y = y * TILE_SIZE + TILE_SIZE / 2;
                                gems[i].collected = false;
                                gems[i].active = true;
                                gems[i].type = chunk_random_next(cseed) % 3;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Get tile at world coordinates
    bool is_solid_world(int world_tx, int ty) {
        if (ty < 0 || ty >= TILES_Y) {
            return ty >= TILES_Y;  // solid below screen, empty above
        }
        
        // Convert world tile X to buffer position
        int chunk_id = world_tx / CHUNK_WIDTH;
        int local_x = world_tx % CHUNK_WIDTH;
        if (local_x < 0) {
            local_x += CHUNK_WIDTH;
            chunk_id--;
        }
        
        // Check if chunk is in our buffer
        int buffer_chunk = chunk_id - world_chunk_offset;
        if (buffer_chunk < 0 || buffer_chunk >= NUM_CHUNKS) {
            return false;  // chunk not loaded, assume empty
        }
        
        int buffer_x = ((chunk_offset + buffer_chunk) % NUM_CHUNKS) * CHUNK_WIDTH + local_x;
        return level[ty * LEVEL_WIDTH + buffer_x] == 1;
    }
    
    // Update chunks based on camera position
    void update_chunks() {
        int camera_chunk = (int)(camera_x / (CHUNK_WIDTH * TILE_SIZE));
        
        // Generate chunks ahead of and behind the camera
        int desired_left_chunk = camera_chunk - 1;
        
        // Shift chunks if needed
        while (world_chunk_offset < desired_left_chunk) {
            // Remove leftmost chunk, add new one on right
            // Clear gems from old chunk
            int old_chunk_world_x = world_chunk_offset * CHUNK_WIDTH * TILE_SIZE;
            for (int i = 0; i < MAX_GEMS; i++) {
                if (gems[i].active && gems[i].x < old_chunk_world_x + CHUNK_WIDTH * TILE_SIZE) {
                    gems[i].active = false;
                }
            }
            
            world_chunk_offset++;
            chunk_offset = (chunk_offset + 1) % NUM_CHUNKS;
            
            // Generate new rightmost chunk
            int new_chunk_id = world_chunk_offset + NUM_CHUNKS - 1;
            int buffer_index = (chunk_offset + NUM_CHUNKS - 1) % NUM_CHUNKS;
            generate_chunk(new_chunk_id, buffer_index);
        }
        
        while (world_chunk_offset > desired_left_chunk && world_chunk_offset > 0) {
            // Remove rightmost chunk, add new one on left
            // Clear gems from old chunk
            int old_chunk_world_x = (world_chunk_offset + NUM_CHUNKS - 1) * CHUNK_WIDTH * TILE_SIZE;
            for (int i = 0; i < MAX_GEMS; i++) {
                if (gems[i].active && gems[i].x >= old_chunk_world_x) {
                    gems[i].active = false;
                }
            }
            
            world_chunk_offset--;
            chunk_offset = (chunk_offset + NUM_CHUNKS - 1) % NUM_CHUNKS;
            
            // Generate new leftmost chunk
            generate_chunk(world_chunk_offset, chunk_offset);
        }
    }
    
    // Draw a chicken sprite (8x8 pixel art)
    void draw_chicken(int x, int y, bool flip, int frame, bool on_ground) {
        Pen white(255, 255, 255);
        Pen orange(255, 165, 0);
        Pen red(255, 50, 50);
        Pen black(0, 0, 0);
        Pen yellow(255, 220, 100);
        
        auto put_pixel = [&](int px, int py, Pen color) {
            int draw_x = flip ? x + (7 - px) : x + px;
            if (draw_x >= 0 && draw_x < SCREEN_W && y + py >= 0 && y + py < SCREEN_H) {
                screen.pen = color;
                screen.pixel(Point(draw_x, y + py));
            }
        };
        
        // Head (red comb on top)
        put_pixel(2, 0, red);
        put_pixel(3, 0, red);
        
        // Head body
        put_pixel(1, 1, white);
        put_pixel(2, 1, white);
        put_pixel(3, 1, white);
        put_pixel(4, 1, white);
        
        // Eye and beak row
        put_pixel(1, 2, white);
        put_pixel(2, 2, black);
        put_pixel(3, 2, white);
        put_pixel(0, 2, orange);
        
        // Neck/wattle
        put_pixel(1, 3, red);
        put_pixel(2, 3, white);
        put_pixel(3, 3, white);
        put_pixel(4, 3, yellow);
        
        // Body
        put_pixel(2, 4, white);
        put_pixel(3, 4, white);
        put_pixel(4, 4, white);
        put_pixel(5, 4, white);
        put_pixel(6, 4, yellow);
        
        put_pixel(2, 5, white);
        put_pixel(3, 5, white);
        put_pixel(4, 5, white);
        put_pixel(5, 5, white);
        put_pixel(6, 5, yellow);
        put_pixel(7, 5, yellow);
        
        // Legs (animated)
        if (frame == 0 || !on_ground) {
            put_pixel(3, 6, orange);
            put_pixel(4, 6, orange);
            put_pixel(3, 7, orange);
            put_pixel(4, 7, orange);
        } else {
            put_pixel(2, 6, orange);
            put_pixel(5, 6, orange);
            put_pixel(2, 7, orange);
            put_pixel(5, 7, orange);
        }
    }
    
    // Draw a gem
    void draw_gem(int x, int y, uint8_t type, uint32_t time) {
        if (x < -5 || x > SCREEN_W + 5 || y < -5 || y > SCREEN_H + 5) return;
        
        Pen colors[3] = {
            Pen(255, 50, 50),
            Pen(50, 255, 50),
            Pen(50, 150, 255)
        };
        
        Pen color = colors[type];
        int bob = (int)(std::sin(time / 200.0f) * 2);
        y += bob;
        
        screen.pen = color;
        screen.pixel(Point(x, y - 3));
        screen.pixel(Point(x - 1, y - 2));
        screen.pixel(Point(x, y - 2));
        screen.pixel(Point(x + 1, y - 2));
        screen.pixel(Point(x - 2, y - 1));
        screen.pixel(Point(x - 1, y - 1));
        screen.pixel(Point(x, y - 1));
        screen.pixel(Point(x + 1, y - 1));
        screen.pixel(Point(x + 2, y - 1));
        screen.pixel(Point(x - 2, y));
        screen.pixel(Point(x - 1, y));
        screen.pixel(Point(x, y));
        screen.pixel(Point(x + 1, y));
        screen.pixel(Point(x + 2, y));
        screen.pixel(Point(x - 1, y + 1));
        screen.pixel(Point(x, y + 1));
        screen.pixel(Point(x + 1, y + 1));
        screen.pixel(Point(x, y + 2));
        
        screen.pen = Pen(255, 255, 255);
        screen.pixel(Point(x - 1, y - 2));
        screen.pixel(Point(x - 1, y - 1));
    }
    
    void init() {
        set_screen_mode(ScreenMode::hires);
        
        // Initialize gems
        for (int i = 0; i < MAX_GEMS; i++) {
            gems[i].active = false;
        }
        
        // Generate initial chunks
        chunk_offset = 0;
        world_chunk_offset = 0;
        for (int i = 0; i < NUM_CHUNKS; i++) {
            generate_chunk(i, i);
        }
        
        // Initialize player
        player.x = CHUNK_WIDTH * TILE_SIZE;  // Start in second chunk
        player.y = (TILES_Y - 3) * TILE_SIZE + 10;
        player.vx = 0;
        player.vy = 0;
        player.on_ground = false;
        player.facing_right = true;
        
        camera_x = player.x - SCREEN_W / 2;
        if (camera_x < 0) camera_x = 0;
    }
    
    void render(uint32_t time) {
        // Clear screen with sky gradient
        for (int y = 0; y < SCREEN_H; y++) {
            int r = 30 + y / 4;
            int g = 40 + y / 3;
            int b = 80 + y / 2;
            screen.pen = Pen(r, g, b);
            screen.rectangle(Rect(0, y, SCREEN_W, 1));
        }
        
        // Calculate visible tile range
        int start_tile_x = (int)(camera_x / TILE_SIZE);
        int end_tile_x = start_tile_x + (SCREEN_W / TILE_SIZE) + 2;
        
        // Draw tiles
        for (int world_tx = start_tile_x; world_tx <= end_tile_x; world_tx++) {
            for (int ty = 0; ty < TILES_Y; ty++) {
                if (is_solid_world(world_tx, ty)) {
                    int px = world_tx * TILE_SIZE - (int)camera_x;
                    int py = ty * TILE_SIZE;
                    
                    // Main ground color
                    screen.pen = Pen(100, 70, 50);
                    screen.rectangle(Rect(px, py, TILE_SIZE, TILE_SIZE));
                    
                    // Top grass
                    screen.pen = Pen(80, 160, 60);
                    screen.rectangle(Rect(px, py, TILE_SIZE, 3));
                    
                    // Texture
                    screen.pen = Pen(80, 55, 40);
                    if ((world_tx + ty) % 2 == 0) {
                        screen.pixel(Point(px + 2, py + 5));
                        screen.pixel(Point(px + 7, py + 8));
                    }
                }
            }
        }
        
        // Draw gems
        for (int i = 0; i < MAX_GEMS; i++) {
            if (gems[i].active && !gems[i].collected) {
                int screen_x = (int)(gems[i].x - camera_x);
                int screen_y = (int)gems[i].y;
                draw_gem(screen_x, screen_y, gems[i].type, time);
            }
        }
        
        // Draw player
        int player_screen_x = (int)(player.x - camera_x);
        int player_screen_y = (int)player.y;
        int anim_frame = 0;
        if (player.on_ground && (player.vx > 0.5f || player.vx < -0.5f)) {
            anim_frame = (time / 100) % 2;
        }
        draw_chicken(player_screen_x - 4, player_screen_y - 4, player.facing_right, anim_frame, player.on_ground);
        
        // Draw UI
        screen.pen = Pen(0, 0, 0, 150);
        screen.rectangle(Rect(0, 0, SCREEN_W, 16));
        
        screen.pen = Pen(255, 255, 255);
        screen.text("Score: " + std::to_string(score), minimal_font, Point(5, 5));
        
        int distance = (int)(player.x / TILE_SIZE);
        screen.text("Dist: " + std::to_string(distance), minimal_font, Point(180, 5));
    }
    
    void update(uint32_t time) {
        // Player input
        if (buttons & Button::DPAD_LEFT) {
            player.vx -= 0.5f;
            player.facing_right = false;
        }
        if (buttons & Button::DPAD_RIGHT) {
            player.vx += 0.5f;
            player.facing_right = true;
        }
        
        // Jump
        if ((buttons.pressed & Button::A) && player.on_ground) {
            player.vy = JUMP_FORCE;
            player.on_ground = false;
        }
        
        // Apply friction
        player.vx *= FRICTION;
        
        // Clamp horizontal speed
        if (player.vx > MOVE_SPEED) player.vx = MOVE_SPEED;
        if (player.vx < -MOVE_SPEED) player.vx = -MOVE_SPEED;
        
        // Apply gravity
        player.vy += GRAVITY;
        if (player.vy > 8.0f) player.vy = 8.0f;
        
        // Move horizontally
        float new_x = player.x + player.vx;
        int tx1 = (int)(new_x - 3) / TILE_SIZE;
        int tx2 = (int)(new_x + 3) / TILE_SIZE;
        int ty1 = (int)(player.y - 3) / TILE_SIZE;
        int ty2 = (int)(player.y + 3) / TILE_SIZE;
        
        bool h_collision = false;
        for (int ty = ty1; ty <= ty2; ty++) {
            if (is_solid_world(tx1, ty) || is_solid_world(tx2, ty)) {
                h_collision = true;
                break;
            }
        }
        
        if (!h_collision) {
            player.x = new_x;
        } else {
            player.vx = 0;
        }
        
        // Prevent going left of world start
        if (player.x < TILE_SIZE) {
            player.x = TILE_SIZE;
            player.vx = 0;
        }
        
        // Move vertically
        float new_y = player.y + player.vy;
        tx1 = (int)(player.x - 3) / TILE_SIZE;
        tx2 = (int)(player.x + 3) / TILE_SIZE;
        ty1 = (int)(new_y - 3) / TILE_SIZE;
        ty2 = (int)(new_y + 3) / TILE_SIZE;
        
        bool v_collision = false;
        for (int tx = tx1; tx <= tx2; tx++) {
            if (is_solid_world(tx, ty1) || is_solid_world(tx, ty2)) {
                v_collision = true;
                break;
            }
        }
        
        if (!v_collision) {
            player.y = new_y;
            player.on_ground = false;
        } else {
            if (player.vy > 0) {
                player.on_ground = true;
            }
            player.vy = 0;
        }
        
        // Fall off bottom - respawn
        if (player.y > SCREEN_H + 20) {
            player.y = TILE_SIZE * 2;
            player.vy = 0;
        }
        
        // Update camera to follow player
        float target_camera_x = player.x - SCREEN_W / 2;
        if (target_camera_x < 0) target_camera_x = 0;
        camera_x += (target_camera_x - camera_x) * 0.1f;  // Smooth follow
        
        // Update chunks based on camera
        update_chunks();
        
        // Collect gems
        for (int i = 0; i < MAX_GEMS; i++) {
            if (gems[i].active && !gems[i].collected) {
                float dx = player.x - gems[i].x;
                float dy = player.y - gems[i].y;
                if (dx * dx + dy * dy < 100) {
                    gems[i].collected = true;
                    score += (gems[i].type + 1) * 10;
                }
            }
        }
    }
    