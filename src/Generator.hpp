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
    float halfW = 15.f;     // real hitbox half-size from the game (already includes mini/wave size)
    float halfH = 15.f;
    Mode mode = Mode::Cube;
    bool upsideDown = false;
    bool onGround = false;
    bool holding = false;   // jump button held this frame
};

// Tight = spikes only, placed off-grid right against the player's real hitbox.
enum class Style : uint8_t { Basic, Dense, BlocksOnly, Tight };

struct Settings {
    Style style = Style::Basic;
    bool cube = true, ship = true, wave = true, ball = true, ufo = true;
    int maxSpikes = 2;
    float spikeClearance = 4.f;
    float shipMargin = 30.f;
    float waveMargin = 12.f;
    float spikeSpacing = 15.f;  // tight style: horizontal distance between spikes (units)
    float tightGap = 1.f;       // tight style: gap between player and spike hitboxes (units)
    float portalGap = 1.f;      // tight style: gap used instead near portals/pads/mode changes
    // Spike hazard hitbox (read from the game when possible). Offset is the hitbox
    // center relative to the object position when unrotated.
    float spikeHalfW = 3.f, spikeHalfH = 6.f;
    float spikeOffX = 0.f, spikeOffY = 0.f;
    // Tight style mix: saws between the spikes, and random sizes for both.
    struct SawDef { int id; float radius; };   // radius at scale 1 (read from the game)
    std::vector<SawDef> saws;
    float sawChance = 0.f;      // 0..1, share of tight slots that become a saw
    float sizeMin = 1.f, sizeMax = 1.f;
    // Chain pattern (instead of random): same-size saws at an even spacing along
    // each side, top row shifted half a step, with same-size small spikes in between.
    bool chain = true;
    float sawSpacing = 60.f;    // units between saw centers on one side (0 = no saws)
    float sawScale = 1.f;
    float spikeScale = 0.5f;
    // Orbs used as obstacles inside the corridor: right on the path, only where the
    // recorded run isn't pressing while it overlaps them - a click there (one step
    // off) sets the orb off and throws the player into the spikes.
    struct OrbDef { int id; float halfW, halfH; };   // trigger box at scale 1 (from the game)
    std::vector<OrbDef> orbs;
    float orbChance = 0.f;      // 0..1, share of spike spots that become an orb
    float orbScale = 1.f;
    float orbSpacing = 60.f;    // units between orb spots along the path
    uint32_t seed = 1;
    int blockID = 1;
    int spikeID = 8;
};

struct Placement {
    int objectID = 1;
    float x = 0.f;          // object center
    float y = 0.f;
    float rotation = 0.f;   // 180 for ceiling spikes
    bool isSpike = false;   // any hazard (spike or saw)
    float scale = 1.f;      // object scale (1 = normal size)
    float radius = 0.f;     // saws: hazard circle radius after scaling (0 = not a saw)
    float boxW = 0.f, boxH = 0.f;   // orbs: half size of the trigger box after scaling (0 = not an orb)
};

// A solid block already in the level (world rect): tight hazards never go inside one.
struct SolidRect { float l, b, r, t; };

// Objects already in the level, used so we don't stack on top of them.
struct ExistingCell { int ix; int iy; };

// Grid helpers. Ground top is y = 90, so the first block row has center y = 105 (iy = 3).
constexpr float kCell = 30.f;
constexpr float kGroundTop = 90.f;
constexpr int kFirstRow = 3;

std::vector<Placement> generate(
    std::vector<Sample> const& samples,
    Settings const& settings,
    std::vector<ExistingCell> const& existing,
    std::vector<SolidRect> const& solids = {}
);

// Dual mode: places around each player's path separately, then drops anything that
// would hit (or, for blocks, sit inside) the other player's path.
std::vector<Placement> generateDual(
    std::vector<Sample> const& samples1,
    std::vector<Sample> const& samples2,
    Settings const& settings,
    std::vector<ExistingCell> const& existing,
    std::vector<SolidRect> const& solids = {}
);

// True if the placement touches the player's hitbox at any recorded step (with `gap`).
bool hitsPath(Placement const& p, std::vector<Sample> const& samples, Settings const& settings, float gap);

} // namespace pb
