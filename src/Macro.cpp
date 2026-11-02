#include "Macro.hpp"

#include <fstream>
#include <sstream>

namespace pb {

bool Macro::save(std::filesystem::path const& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << "PBMACRO 1\n";
    out << "x0 " << (hasX0 ? 1 : 0) << " " << x0 << "\n";
    out << "end " << endStep << "\n";
    for (auto const& e : inputs) out << e.step << " " << (e.down ? 1 : 0) << " " << e.x << "\n";
    return static_cast<bool>(out);
}

bool Macro::load(std::filesystem::path const& file) {
    std::ifstream in(file);
    if (!in) return false;
    std::string magic;
    int version = 0;
    in >> magic >> version;
    if (magic != "PBMACRO" || version != 1) return false;

    Macro m;
    std::string tag;
    int has = 0;
    in >> tag >> has >> m.x0;
    if (tag != "x0") return false;
    m.hasX0 = has != 0;
    in >> tag >> m.endStep;
    if (tag != "end") return false;

    InputEvent e;
    int down = 0;
    while (in >> e.step >> down >> e.x) {
        e.down = down != 0;
        m.inputs.push_back(e);
    }
    *this = std::move(m);
    return true;
}

std::string macroKey(std::string const& levelName, int levelID) {
    std::string out;
    for (char c : levelName) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        out += ok ? c : '_';
    }
    if (out.empty()) out = "unnamed";
    if (out.size() > 60) out.resize(60);
    return out + "_" + std::to_string(levelID);
}

} // namespace pb
