#include "Layout.hpp"
#include "HersheyData.hpp"

#include <algorithm>
#include <cmath>

namespace lt {

namespace {
    struct P { float x, y; };

    float distToSeg(P p, P a, P b) {
        float dx = b.x - a.x, dy = b.y - a.y;
        float l2 = dx * dx + dy * dy;
        float t = l2 > 0.f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
        float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
        return std::sqrt(ex * ex + ey * ey);
    }

    // Douglas-Peucker: keep the points that matter for the shape.
    void simplify(std::vector<P> const& pts, size_t a, size_t b, float tol, std::vector<bool>& keep) {
        if (b <= a + 1) return;
        float best = -1.f;
        size_t idx = a;
        for (size_t i = a + 1; i < b; i++) {
            float d = distToSeg(pts[i], pts[a], pts[b]);
            if (d > best) { best = d; idx = i; }
        }
        if (best > tol) {
            keep[idx] = true;
            simplify(pts, a, idx, tol, keep);
            simplify(pts, idx, b, tol, keep);
        }
    }

    std::vector<std::string> splitLines(std::string const& text) {
        std::vector<std::string> out(1);
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '\n') { out.emplace_back(); continue; }
            if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { out.emplace_back(); i++; continue; }
            out.back() += text[i];
        }
        return out;
    }
}

int fontCount() { return kFontCount; }
char const* fontName(int i) { return kFonts[std::clamp(i, 0, kFontCount - 1)].name; }

std::vector<Line> layoutText(std::string const& text, LayoutOptions const& opt) {
    auto const& font = kFonts[std::clamp(opt.font, 0, kFontCount - 1)];
    float s = opt.capHeight / 21.f;   // Hershey capitals are 21 units tall
    float lineStep = 32.f * s + opt.lineSpacing;

    std::vector<Line> out;
    auto lines = splitLines(text);
    float y0 = 0.f;
    for (auto const& row : lines) {
        float pen = 0.f;
        for (unsigned char ch : row) {
            if (ch < 32 || ch > 126) ch = '?';
            auto const& g = font.glyphs[ch - 32];
            for (int k = 0; k < g.count; k++) {
                auto const& st = kStrokes[g.first + k];
                std::vector<P> pts;
                for (int j = 0; j < st.count; j++) {
                    auto const& q = kPoints[st.first + j];
                    pts.push_back({pen + q[0] * s, y0 + q[1] * s});
                }
                std::vector<bool> keep(pts.size(), opt.simplify <= 0.f);
                keep.front() = keep.back() = true;
                if (opt.simplify > 0.f) simplify(pts, 0, pts.size() - 1, opt.simplify, keep);
                P prev = pts.front();
                for (size_t j = 1; j < pts.size(); j++) {
                    if (!keep[j]) continue;
                    P cur = pts[j];
                    float dx = cur.x - prev.x, dy = cur.y - prev.y;
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.01f) {
                        out.push_back({(prev.x + cur.x) * 0.5f, (prev.y + cur.y) * 0.5f, len,
                                       std::atan2(dy, dx) * 57.2957795f});
                    }
                    prev = cur;
                }
            }
            pen += g.width * s + opt.letterSpacing;
        }
        y0 -= lineStep;
    }
    if (out.empty()) return out;

    // Center the whole block on (0, 0).
    float l = 1e9f, r = -1e9f, b = 1e9f, t = -1e9f;
    for (auto const& ln : out) {
        float hx = std::fabs(std::cos(ln.angle / 57.2957795f)) * ln.length * 0.5f;
        float hy = std::fabs(std::sin(ln.angle / 57.2957795f)) * ln.length * 0.5f;
        l = std::min(l, ln.x - hx); r = std::max(r, ln.x + hx);
        b = std::min(b, ln.y - hy); t = std::max(t, ln.y + hy);
    }
    float cx = (l + r) * 0.5f, cy = (b + t) * 0.5f;
    for (auto& ln : out) { ln.x -= cx; ln.y -= cy; }
    return out;
}

} // namespace lt
