// PC check: lay out a string and dump the lines for a quick look.
#include "../src/Layout.hpp"
#include <cstdio>
int main(int argc, char** argv) {
    lt::LayoutOptions o;
    o.font = argc > 2 ? std::atoi(argv[2]) : 0;
    auto lines = lt::layoutText(argc > 1 ? argv[1] : "REVOLVE", o);
    for (auto const& l : lines) std::printf("%f %f %f %f\n", l.x, l.y, l.length, l.angle);
    std::fprintf(stderr, "%zu lines\n", lines.size());
}
