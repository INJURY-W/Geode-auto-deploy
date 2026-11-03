#pragma once
// Text -> straight line pieces, using the Hershey stroke fonts. Pure C++ (no GD).

#include <string>
#include <vector>

namespace lt {

struct Line {
    float x, y;      // center
    float length;    // end to end
    float angle;     // degrees, counter-clockwise from +x
};

struct LayoutOptions {
    int font = 0;
    float capHeight = 30.f;    // height of capital letters (units, 30 = 1 block)
    float letterSpacing = 2.f; // extra space between letters
    float lineSpacing = 12.f;  // extra space between lines
    float simplify = 0.5f;     // curves: drop points closer than this to a straight line (0 = keep all)
};

// Lines for the text, centered on (0, 0). "\n" (typed as backslash + n) starts a new line.
std::vector<Line> layoutText(std::string const& text, LayoutOptions const& opt);

int fontCount();
char const* fontName(int i);

} // namespace lt
