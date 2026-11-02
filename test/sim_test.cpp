// Host-side test: simulates a rough playtest path and checks the generator never
// places anything that would touch the recorded player. Not part of the mod build.
#include "../src/Generator.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace pb;

int main(int argc, char** argv) {
    std::string style = argc > 1 ? argv[1] : "basic";
    std::vector<Sample> path;
    const float dt = 1.f / 60.f, speed = 311.58f;
    float x = 0.f, y = 105.f, vy = 0.f;
    const float g = 2721.f, jumpV = 571.f;
    float floorY = 105.f;

    auto push = [&](Mode m, bool ground, bool hold, bool upside = false) {
        path.push_back({x, y, 1.f, m, upside, ground, hold});
    };

    // Cube: run, 3 jumps on the floor, then a jump onto a 1-block step and back down.
    std::vector<float> jumpAt = {300, 600, 900, 1200};
    size_t nextJump = 0;
    while (x < 1800) {
        bool hold = false;
        bool ground = (y <= floorY + 0.01f && vy <= 0);
        if (ground && nextJump < jumpAt.size() && x >= jumpAt[nextJump]) {
            vy = jumpV; hold = true; nextJump++; ground = false;
            if (nextJump == 4) floorY = 135.f; // the 4th jump lands on a step
        }
        if (x > 1500) floorY = 105.f;          // walk off the step
        if (!ground) { vy -= g * dt; y += vy * dt; if (y <= floorY) { y = floorY; vy = 0; } }
        push(Mode::Cube, y <= floorY + 0.01f && vy == 0, hold);
        x += speed * dt;
    }
    // Ship: sine wave flight.
    float x0 = x;
    while (x < x0 + 900) {
        y = 200.f + 60.f * std::sin((x - x0) / 150.f);
        push(Mode::Ship, false, false);
        x += speed * dt;
    }
    // Wave: 45-degree zigzag.
    x0 = x; y = 200.f; float dir = 1;
    while (x < x0 + 900) {
        y += dir * speed * dt;
        if (y > 260 || y < 140) dir = -dir;
        push(Mode::Wave, false, dir > 0);
        x += speed * dt;
    }
    // Ball: roll, flip to a 6-block-high ceiling, flip back.
    x0 = x; y = 105.f; vy = 0; float gdir = -1; bool up = false;
    float ceilY = 105.f + 5 * 30.f;
    std::vector<float> flips = {x0 + 150, x0 + 450};
    size_t nf = 0;
    while (x < x0 + 750) {
        bool hold = false;
        bool ground = (!up && y <= 105.f) || (up && y >= ceilY);
        if (ground && nf < flips.size() && x >= flips[nf]) { up = !up; gdir = -gdir; hold = true; nf++; ground = false; vy = 0; }
        if (!ground) {
            vy += gdir * g * dt; y += vy * dt;
            if (!up && y <= 105.f) { y = 105.f; vy = 0; }
            if (up && y >= ceilY) { y = ceilY; vy = 0; }
        }
        bool g2 = (!up && y <= 105.f) || (up && y >= ceilY);
        push(Mode::Ball, g2, hold, up);
        x += speed * dt;
    }

    Settings st;
    if (style == "dense") st.style = Style::Dense;
    if (style == "blocks") st.style = Style::BlocksOnly;
    auto out = generate(path, st, {});

    // Safety check: no block cell or spike hazard may overlap the player at any frame.
    int bad = 0;
    for (auto const& p : out) {
        float l, b, r, t;
        if (!p.isSpike) { l = p.x - 15; r = p.x + 15; b = p.y - 15; t = p.y + 15; }
        else if (p.rotation == 0) { l = p.x - 6; r = p.x + 6; b = p.y - 15; t = p.y + 5; }
        else { l = p.x - 6; r = p.x + 6; b = p.y - 5; t = p.y + 15; }
        for (auto const& s : path) {
            float h = 15 * s.size;
            if (l < s.x + h - 0.5f && s.x - h < r - 0.5f && b < s.y + h - 0.5f && s.y - h < t - 0.5f) { bad++; break; }
        }
    }
    int blocks = 0, spikes = 0;
    for (auto const& p : out) (p.isSpike ? spikes : blocks)++;
    std::printf("style=%s samples=%zu blocks=%d spikes=%d collisions=%d\n",
                style.c_str(), path.size(), blocks, spikes, bad);

    // ASCII render: '#' block, '^' / 'v' spike, '.' player path.
    std::map<std::pair<int, int>, char> grid;
    int maxX = 0, maxY = 0;
    for (auto const& s : path) {
        int cx = (int)std::floor(s.x / 30), cy = (int)std::floor(s.y / 30);
        grid[{cx, cy}] = '.'; maxX = std::max(maxX, cx); maxY = std::max(maxY, cy);
    }
    for (auto const& p : out) {
        int cx = (int)std::floor(p.x / 30), cy = (int)std::floor(p.y / 30);
        grid[{cx, cy}] = p.isSpike ? (p.rotation == 0 ? '^' : 'v') : '#';
        maxY = std::max(maxY, cy);
    }
    for (int row = maxY; row >= 2; row--) {
        std::string line;
        for (int c = 0; c <= maxX; c++) {
            auto it = grid.find({c, row});
            line += it == grid.end() ? (row == 2 ? '=' : ' ') : it->second;
        }
        std::printf("%s\n", line.c_str());
    }
    return bad == 0 ? 0 : 1;
}
