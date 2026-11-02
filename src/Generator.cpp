#include "Generator.hpp"

#include <algorithm>
#include <cmath>
#include <map>
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

// GD's spike (ID 8) hazard hitbox: a 6 x 12 box around the object's center.
constexpr float kSpikeHalfW = 3.f;
constexpr float kSpikeHalfH = 6.f;

Rect freeSpikeRect(float x, float y) {
    return {x - kSpikeHalfW, y - kSpikeHalfH, x + kSpikeHalfW, y + kSpikeHalfH};
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
            std::vector<ExistingCell> const& existing)
        : m_samples(samples), m_st(st), m_dense(densify(samples)) {
        for (auto const& c : existing) m_occupied.insert(key(c.ix, c.iy));
        // Index (densified) samples by the grid columns their padded hitbox touches.
        for (size_t i = 0; i < m_dense.size(); i++) {
            auto r = playerRect(m_dense[i]).expanded(kPad);
            for (int c = cellOf(r.l); c <= cellOf(r.r); c++) m_columns[c].push_back(i);
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
        float c = m_st.spikeClearance;
        float step = std::max(3.f, m_st.spikeSpacing);
        size_t n = m_samples.size();
        size_t i = 0;
        while (i < n) {
            auto const& s = m_samples[i];
            if (isPlatformMode(s.mode) || !modeEnabled(s.mode, m_st)) { i++; continue; }
            size_t j = i;
            float x0 = s.x, x1 = s.x;
            while (j < n && !isPlatformMode(m_samples[j].mode) && modeEnabled(m_samples[j].mode, m_st)) {
                x0 = std::min(x0, m_samples[j].x);
                x1 = std::max(x1, m_samples[j].x);
                j++;
            }
            for (float x = x0; x <= x1 + 0.01f; x += step) {
                float lo = 1e9f, hi = -1e9f;
                float l = x - kSpikeHalfW - c, r = x + kSpikeHalfW + c;
                for (int col = cellOf(l); col <= cellOf(r); col++) {
                    auto it = m_columns.find(col);
                    if (it == m_columns.end()) continue;
                    for (size_t k : it->second) {
                        auto pr = playerRect(m_dense[k]);
                        if (pr.r <= l || pr.l >= r) continue;
                        lo = std::min(lo, pr.b);
                        hi = std::max(hi, pr.t);
                    }
                }
                if (hi < lo) continue;
                placeFreeSpike(x, hi + c + kSpikeHalfH, true);
                float fy = lo - c - kSpikeHalfH;
                if (fy >= kGroundTop + 15.f) placeFreeSpike(x, fy, false);
            }
            i = j;
        }
    }

    bool placeFreeSpike(float x, float y, bool ceiling) {
        if (m_occupied.count(key(cellOf(x), cellOf(y)))) return false;   // existing object here
        if (hitsPath(freeSpikeRect(x, y), std::max(0.f, m_st.spikeClearance - 0.25f))) return false;
        m_out.push_back({m_st.spikeID, x, y, ceiling ? 180.f : 0.f, true});
        return true;
    }

    std::vector<Placement> run() {
        groundBlocks();
        if (m_st.style == Style::Tight) tightCorridors();
        else corridors();
        jumpSpikes();
        return std::move(m_out);
    }

private:
    static constexpr float kPad = 45.f;
    std::vector<Sample> const& m_samples;
    Settings const& m_st;
    std::vector<Sample> m_dense;
    std::unordered_map<int, std::vector<size_t>> m_columns;
    std::unordered_set<int64_t> m_occupied;
    std::unordered_set<int64_t> m_ourBlocks;
    std::vector<Placement> m_out;
};

} // namespace

std::vector<Placement> generate(
    std::vector<Sample> const& samples,
    Settings const& settings,
    std::vector<ExistingCell> const& existing
) {
    if (samples.size() < 2) return {};
    Builder b(samples, settings, existing);
    return b.run();
}

} // namespace pb
