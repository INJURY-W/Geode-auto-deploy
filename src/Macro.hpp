#pragma once
// Recorded clicks ("macro") for one level, saved per level so the bot also works
// after restarting the game and in real play.

#include <filesystem>
#include <string>
#include <vector>

namespace pb {

struct InputEvent {
    int step = 0;      // physics steps finished when the click happened
    bool down = true;  // press or release
    float x = 0.f;     // player x at that moment (for showing results)
};

struct Macro {
    std::vector<InputEvent> inputs;
    float x0 = 0.f;    // player x after the first physics step (used to line up replays)
    bool hasX0 = false;
    int endStep = 0;   // last physics step of the recorded run
    float frameDt = 0.f; // average frame time of the recorded run (0 = unknown)
    float tps = 0.f;     // physics steps per second of the recorded run (0 = unknown)

    bool empty() const { return inputs.empty(); }
    void clear() { *this = Macro{}; }

    bool save(std::filesystem::path const& file) const;
    bool load(std::filesystem::path const& file);
};

// "My Level" + 0 -> "My_Level_0"
std::string macroKey(std::string const& levelName, int levelID);

} // namespace pb
