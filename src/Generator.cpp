#include "Generator.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace pb {
namespace {

struct Rect {
    float l, b, r, t;
    bool overlaps(Rect const& o, float eps = 0.5f) const {
        return l < o.r - eps && o.l < r - eps && b < o.t - eps && o.b < t - eps;
    }
    Rect expanded(float p) const { return {l - p, b - p, r + p, t + p}; }
};

int64_t key(int ix, int iy) {
    return (static_cast<int64_t>(ix) << 32) ^ static_cast<uint32_t>(iy);
}

int cellOf(float v) { return static_cast<int>(std::floor(v / kCell)); }

Rect cellRect(int ix, int iy) {
    return {ix * kCell, iy * kCell, ix * kCell + kCell, iy * kCell + kCell};
}

Rect playerRect(Sample const& s) {
    return {s.x - s.halfW, s.y - s.halfH, s.x + s.halfW, s.y + s.halfH};
}

// Hazard rect of a spike whose object position is (x, y); a 180-degree spike
// has its hitbox offset mirrored.
Rect freeSpikeRect(Settings const& st, float x, float y, bool ceiling) {
    float ox = ceiling ? -st.spikeOffX : st.spikeOffX;
    float oy = ceiling ? -st.spikeOffY : st.spikeOffY;
    float cx = x + ox, cy = y + oy;
    return {cx - st.spikeHalfW, cy - st.spikeHalfH, cx + st.spikeHalfW, cy + st.spikeHalfH};
}

// Recorded frames are ~5 units apart; GD checks collisions more often than that.
// Fill the gaps with 1-unit steps so nothing can slip between two frames.
std::vector<Sample> densify(std::vector<Sample> const& in) {
    std::vector<Sample> out;
    out.reserve(in.size() * 6);
    for (size_t i = 0; i < in.size(); i++) {
        if (i > 0) {
            auto const& a = in[i - 1];
            auto const& b = in[i];
            float dist = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y));
            if (dist > 1.f && dist <= 60.f) {   // > 60 = teleport, don't bridge it
                int steps = static_cast<int>(std::ceil(dist));
                for (int k = 1; k < steps; k++) {
                    float t = static_cast<float>(k) / steps;
                    Sample m = a;
                    m.x = a.x + (b.x - a.x) * t;
                    m.y = a.y + (b.y - a.y) * t;
                    m.halfW = std::max(a.halfW, b.halfW);
                    m.halfH = std::max(a.halfH, b.halfH);
                    out.push_back(m);
                }
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// Spike hazard area, a bit generous compared to GD's real spike hitbox.
Rect spikeHazard(int ix, int iy, bool ceiling) {
    float cx = ix * kCell + 15.f;
    if (!ceiling) {
        float base = iy * kCell;
        return {cx - 6.f, base, cx + 6.f, base + 20.f};
    }
    float top = iy * kCell + kCell;
    return {cx - 6.f, top - 20.f, cx + 6.f, top};
}

bool isPlatformMode(Mode m) {
    return m == Mode::Cube || m == Mode::Robot || m == Mode::Ball || m == Mode::Spider;
}

bool modeEnabled(Mode m, Settings const& st) {
    switch (m) {
        case Mode::Cube: case Mode::Robot: return st.cube;
        case Mode::Ship: case Mode::Swing: return st.ship;
        case Mode::Wave: return st.wave;
        case Mode::Ball: case Mode::Spider: return st.ball;
        case Mode::Ufo: return st.ufo;
    }
    return false;
}

// Snap a surface height to the grid; returns false if it isn't on a grid line (slopes etc).
bool snapSurface(float y, int& line) {
    line = static_cast<int>(std::lround(y / kCell));
    return std::fabs(y - line * kCell) <= 3.f;
}

class Builder {
public:
    Builder(std::vector<Sample> const& samples, Settings const& st,
            std::vector<ExistingCell> const& existing, std::vector<SolidRect> const& solids)
        : m_samples(samples), m_st(st), m_solids(solids), m_dense(densify(samples)) {
        for (auto const& c : existing) m_occupied.insert(key(c.ix, c.iy));
        // Index (densified) samples by the grid columns their padded hitbox touches.
        for (size_t i = 0; i < m_dense.size(); i++) {
            auto r = playerRect(m_dense[i]).expanded(kPad);
            for (int c = cellOf(r.l); c <= cellOf(r.r); c++) m_columns[c].push_back(i);
        }
        // Index path segments (consecutive recorded steps) for exact tight placement.
        for (size_t i = 0; i < m_samples.size(); i++) {
            size_t j = (i + 1 < m_samples.size()) ? i + 1 : i;
            auto const& a = m_samples[i];
            auto const& b = m_samples[j];
            if (std::fabs(b.x - a.x) > 60.f || std::fabs(b.y - a.y) > 60.f) j = i; // teleport
            float hw = std::max(a.halfW, m_samples[j].halfW) + kPad;
            float x0 = std::min(a.x, m_samples[j].x) - hw, x1 = std::max(a.x, m_samples[j].x) + hw;
            for (int c = cellOf(x0); c <= cellOf(x1); c++) m_segColumns[c].push_back(i);
        }
        markTransitions();
    }

    // Around portals and other mid-step changes (speed, gamemode, gravity, size,
    // ground push-out) GD may test positions between two recorded steps, so the
    // tight placement keeps clear of the whole straight path between them there.
    void markTransitions() {
        size_t n = m_samples.size();
        m_continuous.assign(n, false);
        auto stepDx = [&](size_t i) { return m_samples[i + 1].x - m_samples[i].x; };
        for (size_t i = 0; i + 1 < n; i++) {
            auto const& a = m_samples[i];
            auto const& b = m_samples[i + 1];
            bool change = a.mode != b.mode || a.upsideDown != b.upsideDown ||
                          std::fabs(a.halfW - b.halfW) > 0.01f || std::fabs(a.halfH - b.halfH) > 0.01f ||
                          a.onGround != b.onGround;
            if (!change && i > 0) {
                float d0 = stepDx(i - 1), d1 = stepDx(i);
                if (std::fabs(d1 - d0) > 0.02f + 0.02f * std::fabs(d0)) change = true;  // speed change
            }
            if (!change) continue;
            size_t lo = i >= kTransitionSteps ? i - kTransitionSteps : 0;
            size_t hi = std::min(n - 1, i + kTransitionSteps);
            for (size_t k = lo; k <= hi; k++) m_continuous[k] = true;
        }
    }

    // True if `area` (grown by `pad`) touches the player at any recorded frame.
    bool hitsPath(Rect const& area, float pad) const {
        Rect a = area.expanded(pad);
        for (int c = cellOf(a.l); c <= cellOf(a.r); c++) {
            auto it = m_columns.find(c);
            if (it == m_columns.end()) continue;
            for (size_t i : it->second) {
                if (playerRect(m_dense[i]).overlaps(a)) return true;
            }
        }
        return false;
    }

    bool isOccupied(int ix, int iy) const { return m_occupied.count(key(ix, iy)) > 0; }
    bool isOurBlock(int ix, int iy) const { return m_ourBlocks.count(key(ix, iy)) > 0; }

    bool canBlock(int ix, int iy) const {
        return iy >= kFirstRow && !isOccupied(ix, iy) && !hitsPath(cellRect(ix, iy), 0.f);
    }

    bool placeBlock(int ix, int iy) {
        if (!canBlock(ix, iy)) return false;
        m_occupied.insert(key(ix, iy));
        m_ourBlocks.insert(key(ix, iy));
        m_out.push_back({m_st.blockID, ix * kCell + 15.f, iy * kCell + 15.f, 0.f, false});
        return true;
    }

    bool canSpike(int ix, int iy, bool ceiling) const {
        if (iy < kFirstRow || isOccupied(ix, iy)) return false;
        return !hitsPath(spikeHazard(ix, iy, ceiling), m_st.spikeClearance);
    }

    // Spikes need something to sit on: the ground, an existing object or one of our blocks.
    bool hasSupport(int ix, int iy, bool ceiling) const {
        int sy = ceiling ? iy + 1 : iy - 1;
        if (!ceiling && sy < kFirstRow) return true; // sits on the ground
        return isOccupied(ix, sy);
    }

    bool placeSpike(int ix, int iy, bool ceiling, bool addSupport) {
        if (!canSpike(ix, iy, ceiling)) return false;
        if (!hasSupport(ix, iy, ceiling)) {
            if (!addSupport) return false;
            int sy = ceiling ? iy + 1 : iy - 1;
            if (!placeBlock(ix, sy)) return false;
        }
        m_occupied.insert(key(ix, iy));
        m_out.push_back({m_st.spikeID, ix * kCell + 15.f, iy * kCell + 15.f,
                         ceiling ? 180.f : 0.f, true});
        return true;
    }

    void groundBlocks() {
        for (auto const& s : m_samples) {
            if (!s.onGround || !isPlatformMode(s.mode) || !modeEnabled(s.mode, m_st)) continue;
            int line;
            int iy;
            if (!s.upsideDown) {
                if (!snapSurface(s.y - s.halfH, line)) continue;
                iy = line - 1;          // block whose top is the surface
            } else {
                if (!snapSurface(s.y + s.halfH, line)) continue;
                iy = line;              // block whose bottom is the surface
            }
            for (int ix = cellOf(s.x - s.halfW + 1.f); ix <= cellOf(s.x + s.halfW - 1.f); ix++) {
                placeBlock(ix, iy);
            }
        }
    }

    void jumpSpikes() {
        if (m_st.style == Style::BlocksOnly) return;
        size_t n = m_samples.size();
        for (size_t i = 1; i < n; i++) {
            auto const& d = m_samples[i - 1];
            if (!(d.onGround && !m_samples[i].onGround)) continue;
            if (!isPlatformMode(d.mode) || !modeEnabled(d.mode, m_st)) continue;

            // Find where the player lands again (or the segment ends).
            size_t l = i;
            bool samePlatformMode = true;
            while (l < n && !m_samples[l].onGround) {
                if (!isPlatformMode(m_samples[l].mode)) samePlatformMode = false;
                l++;
            }
            if (!samePlatformMode) continue;
            size_t lastAir = l - 1;

            // Only jumps / gravity switches, not walking off a ledge.
            bool clicked = false;
            for (size_t k = i - 1; k <= std::min(i + 4, lastAir); k++) {
                if (m_samples[k].holding) { clicked = true; break; }
            }
            if (!clicked) continue;

            bool ceiling = d.upsideDown;
            float h = d.halfH;
            int line;
            if (!snapSurface(ceiling ? d.y + h : d.y - h, line)) continue;
            int iy = ceiling ? line - 1 : line;
            bool onGround = !ceiling && line * kCell <= kGroundTop + 1.f;

            float endX = (l < n) ? m_samples[l].x : m_samples[lastAir].x;
            int c0 = cellOf(std::min(d.x, endX)) + 1;
            int c1 = cellOf(std::max(d.x, endX)) - 1;
            if (c1 < c0) continue;

            // Apex = highest point of the jump relative to the surface.
            float apexX = d.x;
            float best = -1e9f;
            for (size_t k = i; k <= lastAir; k++) {
                float dist = ceiling ? -(m_samples[k].y) : m_samples[k].y;
                if (dist > best) { best = dist; apexX = m_samples[k].x; }
            }

            std::vector<int> valid;
            for (int c = c0; c <= c1; c++) {
                if (!canSpike(c, iy, ceiling)) continue;
                if (!onGround && !hasSupport(c, iy, ceiling)) {
                    int sy = ceiling ? iy + 1 : iy - 1;
                    if (!canBlock(c, sy)) continue;
                }
                valid.push_back(c);
            }
            if (valid.empty()) continue;

            std::vector<int> chosen;
            if (m_st.style == Style::Dense || m_st.style == Style::Tight) {
                chosen = valid;
            } else {
                // Contiguous run of valid columns closest to the apex.
                int apexCol = cellOf(apexX);
                std::vector<std::vector<int>> runs;
                for (int c : valid) {
                    if (runs.empty() || runs.back().back() != c - 1) runs.push_back({});
                    runs.back().push_back(c);
                }
                auto distTo = [&](std::vector<int> const& r) {
                    if (apexCol < r.front()) return r.front() - apexCol;
                    if (apexCol > r.back()) return apexCol - r.back();
                    return 0;
                };
                auto run = *std::min_element(runs.begin(), runs.end(),
                    [&](auto const& a, auto const& b) { return distTo(a) < distTo(b); });
                std::sort(run.begin(), run.end(), [&](int a, int b) {
                    return std::abs(a - apexCol) < std::abs(b - apexCol);
                });
                int count = std::max(1, m_st.maxSpikes);
                for (int k = 0; k < static_cast<int>(run.size()) && k < count; k++) {
                    chosen.push_back(run[k]);
                }
            }
            for (int c : chosen) placeSpike(c, iy, ceiling, true);
        }
    }

    void corridors() {
        struct Col { float minB = 1e9f, maxT = -1e9f, margin = 0.f; };
        std::map<int, Col> cols;
        for (auto const& s : m_samples) {
            if (isPlatformMode(s.mode) || !modeEnabled(s.mode, m_st)) continue;
            float margin = (s.mode == Mode::Wave) ? m_st.waveMargin : m_st.shipMargin;
            if (m_st.style == Style::Dense) margin *= 0.5f;
            for (int c = cellOf(s.x - s.halfW + 1.f); c <= cellOf(s.x + s.halfW - 1.f); c++) {
                auto& col = cols[c];
                col.minB = std::min(col.minB, s.y - s.halfH);
                col.maxT = std::max(col.maxT, s.y + s.halfH);
                col.margin = std::max(col.margin, margin);
            }
        }
        if (cols.empty()) return;

        std::map<int, int> floorRow, ceilRow;
        for (auto const& [c, col] : cols) {
            floorRow[c] = cellOf(col.minB - col.margin) - 1;
            ceilRow[c] = static_cast<int>(std::ceil((col.maxT + col.margin) / kCell));
        }

        // Blocks along the floor/ceiling line, plus walls where the line steps.
        for (auto const& [c, fy] : floorRow) {
            placeBlock(c, fy);
            auto prev = floorRow.find(c - 1);
            if (prev != floorRow.end()) {
                int lo = std::min(fy, prev->second), hi = std::max(fy, prev->second);
                int wallCol = (fy > prev->second) ? c : c - 1;
                for (int y = lo + 1; y < hi; y++) placeBlock(wallCol, y);
            }
        }
        for (auto const& [c, cy] : ceilRow) {
            placeBlock(c, cy);
            auto prev = ceilRow.find(c - 1);
            if (prev != ceilRow.end()) {
                int lo = std::min(cy, prev->second), hi = std::max(cy, prev->second);
                int wallCol = (cy < prev->second) ? c : c - 1;
                for (int y = lo + 1; y < hi; y++) placeBlock(wallCol, y);
            }
        }

        if (m_st.style != Style::Dense) return;
        for (auto const& [c, fy] : floorRow) {
            if (isOurBlock(c, fy) || fy < kFirstRow) placeSpike(c, fy + 1, false, false);
        }
        for (auto const& [c, cy] : ceilRow) {
            if (isOurBlock(c, cy)) placeSpike(c, cy - 1, true, false);
        }
    }

    // Tight style: rows of spikes above and below every ship/wave/UFO part,
    // each one pushed right up against the closest the player ever gets.
    void tightCorridors() {
        float step = std::max(3.f, m_st.spikeSpacing);
        std::mt19937 rng(m_st.seed);
        std::uniform_real_distribution<float> u01(0.f, 1.f);
        auto randScale = [&] {
            float lo = std::min(m_st.sizeMin, m_st.sizeMax), hi = std::max(m_st.sizeMin, m_st.sizeMax);
            return lo + (hi - lo) * u01(rng);
        };
        size_t n = m_samples.size();
        size_t i = 0;
        while (i < n) {
            auto const& s = m_samples[i];
            if (!modeEnabled(s.mode, m_st)) { i++; continue; }
            size_t j = i;
            float x0 = s.x, x1 = s.x;
            while (j < n && modeEnabled(m_samples[j].mode, m_st)) {
                x0 = std::min(x0, m_samples[j].x);
                x1 = std::max(x1, m_samples[j].x);
                j++;
            }
            if (m_st.chain) { chainSegment(x0, x1, step, rng); i = j; continue; }
            for (float x = x0; x <= x1 + 0.01f; x += step) {
                for (bool ceiling : {true, false}) {
                    float scale = randScale();
                    bool saw = !m_st.saws.empty() && u01(rng) < m_st.sawChance;
                    if (saw) {
                        auto const& d = m_st.saws[rng() % m_st.saws.size()];
                        placeSaw(x, d.id, d.radius * scale, scale, ceiling);
                    } else {
                        placeScaledSpike(x, scale, ceiling);
                    }
                }
            }
            i = j;
        }
    }

    // Chain pattern for one stretch [x0, x1]: per side, first a row of identical saws
    // at an even spacing (the top row shifted half a step so they zig-zag), then
    // identical small spikes at Spike Spacing everywhere a saw doesn't cover.
    void chainSegment(float x0, float x1, float step, std::mt19937& rng) {
        (void)rng;
        // 1) Full rows of identical spikes - nothing takes their place.
        for (bool ceiling : {false, true}) {
            for (float x = x0; x <= x1 + 0.01f; x += step) placeScaledSpike(x, m_st.spikeScale, ceiling);
        }
        // 2) Saws (if on) at an even spacing just behind the spike rows, so they add to
        //    the layout without pushing any spike out.
        bool haveSaw = !m_st.saws.empty() && m_st.sawSpacing > 0.f && m_st.sawChance > 0.f;
        if (!haveSaw) return;
        float sawStep = std::max(6.f, m_st.sawSpacing);
        float behind = 30.f * m_st.spikeScale;   // a spike's full drawn height
        auto const& d = m_st.saws.front();
        float R = d.radius * m_st.sawScale;
        for (bool ceiling : {false, true}) {
            float start = x0 + (ceiling ? sawStep * 0.5f : 0.f);
            for (float x = start; x <= x1 + 0.01f; x += sawStep) placeSaw(x, d.id, R, m_st.sawScale, ceiling, behind);
        }
    }

    // Orb rows above and below the path, like the spike rows, but sitting half over
    // the path so the player passes inside them. A spot is only used if, at every
    // recorded step whose hitbox (plus the gap) touches the orb, and the two steps
    // before it, the button was not held - so the recorded run never sets it off.
    void innerOrbs() {
        std::mt19937 rng(m_st.seed ^ 0x9e3779b9u);
        std::uniform_real_distribution<float> u01(0.f, 1.f);
        float gap = std::max(m_st.tightGap, m_st.portalGap);
        float nextX = -1e9f;
        size_t n = m_samples.size();
        for (size_t i = 0; i < n; i++) {
            auto const& s = m_samples[i];
            if (!modeEnabled(s.mode, m_st) || s.x < nextX) continue;
            float slot = 0.f;
            for (bool ceiling : {false, true}) {
                if (u01(rng) >= m_st.orbChance) continue;
                auto const& o = m_st.orbs[rng() % m_st.orbs.size()];
                float hw = o.halfW * m_st.orbScale, hh = o.halfH * m_st.orbScale;
                slot = std::max(slot, 2.f * hw);
                // Center a bit off the path toward its side: the box still covers the
                // player's center line, the orb reads as part of the top/bottom row.
                float cy = s.y + (ceiling ? 1.f : -1.f) * hh * 0.6f;
                Rect box{s.x - hw, cy - hh, s.x + hw, cy + hh};
                if (!ceiling && box.b < kGroundTop) continue;
                if (!orbSpotSafe(box, gap) || insideSolid(box)) continue;
                Placement p{o.id, s.x, cy, 0.f, true};
                p.scale = m_st.orbScale;
                p.boxW = hw;
                p.boxH = hh;
                m_out.push_back(p);
            }
            float step = m_st.orbSpacing > 0.f ? m_st.orbSpacing : slot;
            nextX = s.x + std::max(step, 6.f);
        }
    }

    bool orbSpotSafe(Rect const& box, float gap) const {
        std::unordered_set<size_t> seen;
        for (int col = cellOf(box.l - gap - kPad); col <= cellOf(box.r + gap + kPad); col++) {
            auto it = m_segColumns.find(col);
            if (it == m_segColumns.end()) continue;
            for (size_t k : it->second) {
                if (!seen.insert(k).second) continue;
                auto pr = playerRect(m_samples[k]).expanded(gap);
                if (pr.r < box.l || pr.l > box.r || pr.t < box.b || pr.b > box.t) continue;
                for (size_t back = 0; back <= 2 && back <= k; back++) {
                    if (m_samples[k - back].holding) return false;
                }
                if (!modeEnabled(m_samples[k].mode, m_st)) return false;
            }
        }
        return true;
    }

    // Orb (square trigger box) pushed against the path at column x, kept the gap away
    // from every recorded step so the recorded run never touches it.
    bool placeOrb(float x, Settings::OrbDef const& o, float scale, bool ceiling) {
        float c = m_st.tightGap;
        float hw = o.halfW * scale, hh = o.halfH * scale;
        if (hw <= 0.f || hh <= 0.f) return false;
        float lo = 1e9f, hi = -1e9f;
        segmentExtremes(x - hw - c, x + hw + c, lo, hi);
        if (hi < lo) return false;
        float y = ceiling ? hi + c + hh + 0.01f : lo - c - hh - 0.01f;
        if (!ceiling && y - hh < kGroundTop) return false;
        Rect body{x - hw, y - hh, x + hw, y + hh};
        if (insideSolid(body)) return false;
        if (m_occupied.count(key(cellOf(x), cellOf(y)))) return false;
        Placement p{o.id, x, y, 0.f, true};
        p.scale = scale;
        p.boxW = hw;
        p.boxH = hh;
        m_out.push_back(p);
        return true;
    }

    // Spike (upright or hanging) pushed against the path at column x.
    bool placeScaledSpike(float x, float scale, bool ceiling) {
        float c = m_st.tightGap;
        float hw = m_st.spikeHalfW * scale, hh = m_st.spikeHalfH * scale;
        float ox = std::fabs(m_st.spikeOffX) * scale, oy = m_st.spikeOffY * scale;
        float lo = 1e9f, hi = -1e9f;
        segmentExtremes(x - ox - hw - c, x + ox + hw + c, lo, hi);
        if (hi < lo) return false;
        float y = ceiling ? hi + c + hh + oy : lo - c - hh - oy;
        if (!ceiling && y < kGroundTop + 15.f * scale) return false;   // would sit in the ground
        // Whole spike (30 x 30 at this scale) must not be inside one of the player's blocks.
        Rect body{x - 15.f * scale, y - 15.f * scale, x + 15.f * scale, y + 15.f * scale};
        if (insideSolid(body)) return false;
        if (m_occupied.count(key(cellOf(x), cellOf(y)))) return false;
        Placement p{m_st.spikeID, x, y, ceiling ? 180.f : 0.f, true};
        p.scale = scale;
        m_out.push_back(p);
        return true;
    }

    // Saw (round hitbox of radius R) pushed against the path at column x: its center
    // goes just far enough that the circle stays the gap away from every recorded step.
    bool placeSaw(float x, int id, float R, float scale, bool ceiling, float extra = 0.f) {
        if (R <= 0.f) return false;
        float c = m_st.tightGap + extra;
        bool any = false;
        float need = ceiling ? -1e9f : 1e9f;
        auto constrain = [&](Rect const& pr) {
            float dx = std::max({pr.l - x, 0.f, x - pr.r});
            if (dx >= R) return;
            float h = std::sqrt(R * R - dx * dx);
            any = true;
            if (ceiling) need = std::max(need, pr.t + h);
            else need = std::min(need, pr.b - h);
        };
        float reach = R + c + kPad;
        std::unordered_set<size_t> seen;
        for (int col = cellOf(x - reach); col <= cellOf(x + reach); col++) {
            auto it = m_segColumns.find(col);
            if (it == m_segColumns.end()) continue;
            for (size_t k : it->second) {
                if (!seen.insert(k).second) continue;
                bool near = k < m_continuous.size() && m_continuous[k];
                float g = c + (near ? portalExtra() : 0.f);
                constrain(playerRect(m_samples[k]).expanded(g));
                // Near portals also keep clear of the straight path to the next step.
                if (near && k + 1 < m_samples.size()) {
                    auto const& a = m_samples[k];
                    auto const& b = m_samples[k + 1];
                    float d = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y));
                    if (d > 60.f) continue;
                    int steps = std::max(1, static_cast<int>(std::ceil(d / 0.25f)));
                    for (int t = 1; t < steps; t++) {
                        Sample m = a;
                        float f = static_cast<float>(t) / steps;
                        m.x = a.x + (b.x - a.x) * f;
                        m.y = a.y + (b.y - a.y) * f;
                        m.halfW = std::max(a.halfW, b.halfW);
                        m.halfH = std::max(a.halfH, b.halfH);
                        constrain(playerRect(m).expanded(g));
                    }
                }
            }
        }
        if (!any) return false;
        float y = need + (ceiling ? 0.01f : -0.01f);     // touching counts as a hit in GD
        if (!ceiling && y < kGroundTop) return false;
        if (circleInSolid(x, y, R)) return false;
        if (m_occupied.count(key(cellOf(x), cellOf(y)))) return false;
        Placement p{id, x, y, 0.f, true};
        p.scale = scale;
        p.radius = R;
        m_out.push_back(p);
        return true;
    }

    // Exact highest top / lowest bottom of the player hitbox over every path segment
    // whose hitbox touches the x-window [l, r]. The path between two recorded physics
    // steps is treated as a straight line (GD only tests at the steps themselves, so
    // this is the same or slightly safer than the game).
    // Highest top / lowest bottom of the player hitbox over every recorded physics
    // step whose hitbox touches the x-window [l, r]. Only the recorded steps count:
    // GD tests collisions at exactly those positions and nowhere in between, so a
    // spike may reach into the gap between two steps on a slope.
    void segmentExtremes(float l, float r, float& lo, float& hi) const {
        std::unordered_set<size_t> seen;
        for (int col = cellOf(l); col <= cellOf(r); col++) {
            auto it = m_segColumns.find(col);
            if (it == m_segColumns.end()) continue;
            for (size_t i : it->second) {
                if (!seen.insert(i).second) continue;
                auto pr = playerRect(m_samples[i]);
                bool near = i < m_continuous.size() && m_continuous[i];
                if (near) pr = pr.expanded(portalExtra());   // bigger gap near portals
                if (!(pr.r < l || pr.l > r)) {           // touching counts, like GD
                    hi = std::max(hi, pr.t);
                    lo = std::min(lo, pr.b);
                }
                if (i < m_continuous.size() && m_continuous[i]) chordExtremes(i, l, r, lo, hi);
            }
        }
    }

    // Extremes over the straight path from step i to step i+1 (used near transitions).
    void chordExtremes(size_t i, float l, float r, float& lo, float& hi) const {
        if (i + 1 >= m_samples.size()) return;
        auto const& a = m_samples[i];
        auto const& b = m_samples[i + 1];
        if (std::fabs(b.x - a.x) > 60.f || std::fabs(b.y - a.y) > 60.f) return;  // teleport
        float hw = std::max(a.halfW, b.halfW) + portalExtra();
        float hh = std::max(a.halfH, b.halfH) + portalExtra();
        float xa = l - hw, xb = r + hw;
        float dx = b.x - a.x;
        float t0, t1;
        if (std::fabs(dx) < 1e-6f) {
            if (a.x < xa || a.x > xb) return;
            t0 = 0.f; t1 = 1.f;
        } else {
            float ta = (xa - a.x) / dx, tb = (xb - a.x) / dx;
            t0 = std::max(0.f, std::min(ta, tb));
            t1 = std::min(1.f, std::max(ta, tb));
            if (t0 > t1) return;
        }
        float y0 = a.y + (b.y - a.y) * t0, y1 = a.y + (b.y - a.y) * t1;
        hi = std::max(hi, std::max(y0, y1) + hh);
        lo = std::min(lo, std::min(y0, y1) - hh);
    }

    bool placeFreeSpike(float x, float y, bool ceiling) {
        if (m_occupied.count(key(cellOf(x), cellOf(y)))) return false;   // existing object here
        // Placement already keeps the hitbox exactly `tightGap` away from every segment;
        // re-check against the other side of the path (e.g. a spike row that a later
        // part of the path loops back into).
        float lo = 1e9f, hi = -1e9f;
        auto rc = freeSpikeRect(m_st, x, y, ceiling);
        float g = m_st.tightGap;
        segmentExtremes(rc.l - g, rc.r + g, lo, hi);
        if (hi >= lo && !(rc.b - g > hi || rc.t + g < lo)) {
            // The window's path spans across the spike; only safe if spike is fully outside.
            if (!(ceiling ? rc.b - g >= hi - 1e-4f : rc.t + g <= lo + 1e-4f)) return false;
        }
        m_out.push_back({m_st.spikeID, x, y, ceiling ? 180.f : 0.f, true});
        return true;
    }

    std::vector<Placement> run() {
        groundBlocks();
        if (m_st.style == Style::Tight) {
            tightCorridors();          // every gamemode, cube included
            if (!m_st.orbs.empty() && m_st.orbChance > 0.f) innerOrbs();
        } else {
            corridors();
            jumpSpikes();
        }
        return std::move(m_out);
    }

    bool insideSolid(Rect const& r) const {
        for (auto const& b : m_solids) {
            if (r.l < b.r - 0.5f && b.l < r.r - 0.5f && r.b < b.t - 0.5f && b.b < r.t - 0.5f) return true;
        }
        return false;
    }

    bool circleInSolid(float cx, float cy, float R) const {
        for (auto const& b : m_solids) {
            float dx = std::max({b.l - cx, 0.f, cx - b.r});
            float dy = std::max({b.b - cy, 0.f, cy - b.t});
            if (dx * dx + dy * dy < (R - 0.5f) * (R - 0.5f)) return true;
        }
        return false;
    }

private:
    static constexpr float kPad = 45.f;
    std::vector<Sample> const& m_samples;
    Settings const& m_st;
    std::vector<SolidRect> const& m_solids;
    std::vector<Sample> m_dense;
    std::unordered_map<int, std::vector<size_t>> m_columns;
    std::unordered_map<int, std::vector<size_t>> m_segColumns;
    std::vector<bool> m_continuous;               // segment i -> i+1 checked as a line
    // Samples on each side of a change that count as "near" it. Steps can have two
    // samples (before/after GD's push-out), so this covers roughly 8-16 steps.
    static constexpr size_t kTransitionSteps = 16;
    // Extra room on top of the normal tight gap near portals/pads/mode changes.
    float portalExtra() const { return std::max(0.f, m_st.portalGap - m_st.tightGap); }
    std::unordered_set<int64_t> m_occupied;
    std::unordered_set<int64_t> m_ourBlocks;
    std::vector<Placement> m_out;
};

} // namespace

std::vector<Placement> generate(
    std::vector<Sample> const& samples,
    Settings const& settings,
    std::vector<ExistingCell> const& existing,
    std::vector<SolidRect> const& solids
) {
    if (samples.size() < 2) return {};
    Builder b(samples, settings, existing, solids);
    return b.run();
}

bool hitsPath(Placement const& p, std::vector<Sample> const& samples, Settings const& st, float gap) {
    for (auto const& smp : samples) {
        float l = smp.x - smp.halfW - gap, r = smp.x + smp.halfW + gap;
        float b = smp.y - smp.halfH - gap, t = smp.y + smp.halfH + gap;
        if (p.radius > 0.f) {
            float dx = std::max({l - p.x, 0.f, p.x - r});
            float dy = std::max({b - p.y, 0.f, p.y - t});
            if (dx * dx + dy * dy <= p.radius * p.radius) return true;
        } else if (p.boxW > 0.f) {
            if (p.x - p.boxW <= r && l <= p.x + p.boxW && p.y - p.boxH <= t && b <= p.y + p.boxH) return true;
        } else if (p.isSpike) {
            bool flip = std::fabs(p.rotation - 180.f) < 1.f;
            float hw = st.spikeHalfW * p.scale, hh = st.spikeHalfH * p.scale;
            float cx = p.x + (flip ? -1.f : 1.f) * st.spikeOffX * p.scale;
            float cy = p.y + (flip ? -1.f : 1.f) * st.spikeOffY * p.scale;
            if (cx - hw <= r && l <= cx + hw && cy - hh <= t && b <= cy + hh) return true;
        } else {
            float h = 15.f * p.scale;
            if (p.x - h < r && l < p.x + h && p.y - h < t && b < p.y + h) return true;
        }
    }
    return false;
}

std::vector<Placement> generateDual(std::vector<Sample> const& s1, std::vector<Sample> const& s2,
                                    Settings const& st, std::vector<ExistingCell> const& existing,
                                    std::vector<SolidRect> const& solids) {
    if (s2.size() < 2) return generate(s1, st, existing, solids);
    float gap = st.style == Style::Tight ? std::max(st.tightGap, st.portalGap) : st.spikeClearance;
    auto a = generate(s1, st, existing, solids);
    // Player 2's run also keeps clear of the blocks placed for player 1.
    auto existing2 = existing;
    for (auto const& p : a) {
        if (!p.isSpike) existing2.push_back({static_cast<int>(std::floor(p.x / kCell)), static_cast<int>(std::floor(p.y / kCell))});
    }
    auto b = generate(s2, st, existing2, solids);
    std::vector<Placement> out;
    out.reserve(a.size() + b.size());
    for (auto const& p : a) if (!hitsPath(p, s2, st, gap)) out.push_back(p);
    for (auto const& p : b) if (!hitsPath(p, s1, st, gap)) out.push_back(p);
    return out;
}

} // namespace pb
