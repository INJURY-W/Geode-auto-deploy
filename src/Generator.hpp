#pragma once
// Pure C++ (no Geode / cocos) so the placement logic can be unit-tested on a PC.

#include <cstdint>
#include <vector>

namespace pb {

// Player gamemode at a recorded frame.
enum class Mode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

struct Sample {
    float x = 0.f;          // player center (world units, 30 = 1 block)
    float y = 0.f;
    float size = 1.f;       // 1 = normal, ~0.6 = mini
    Mode mode = Mode::Cube;
    bool upsideDown = false;
    bool onGround = false;
    bool holding = false;   // jump button held this frame
};

enum class Style : uint8_t { Basic, Dense, BlocksOnly };

struct Settings {
    Style style = Style::Basic;
    bool cube = true, ship = true, wave = true, ball = true, ufo = true;
    int maxSpikes = 2;
    float spikeClearance = 4.f;
    float shipMargin = 30.f;
    float waveMargin = 12.f;
    int blockID = 1;
    int spikeID = 8;
};

struct Placement {
    int objectID = 1;
    float x = 0.f;          // object center
    float y = 0.f;
    float rotation = 0.f;   // 180 for ceiling spikes
    bool isSpike = false;
};

// Objects already in the level, used so we don't stack on top of them.
struct ExistingCell { int ix; int iy; };

// Grid helpers. Ground top is y = 90, so the first block row has center y = 105 (iy = 3).
constexpr float kCell = 30.f;
constexpr float kGroundTop = 90.f;
constexpr int kFirstRow = 3;

std::vector<Placement> generate(
    std::vector<Sample> const& samples,
    Settings const& settings,
    std::vector<ExistingCell> const& existing
);

} // namespace pb
