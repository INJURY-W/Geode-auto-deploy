#include <Geode/Geode.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/MDPopup.hpp>

#include <cmath>

#include "Generator.hpp"
#include "Macro.hpp"

using namespace geode::prelude;

// ===========================================================================
// State
// ===========================================================================
namespace {
    // --- path recording (editor, hand-played or bot playtests) ---
    std::vector<pb::Sample> g_samples;
    bool g_recording = false;
    bool g_stoppedByDeath = false;
    bool g_holding = false;
    bool g_pressedThisFrame = false;
    float g_lastHitW = 0.f, g_lastHitH = 0.f;

    // --- macro (clicks) for the level in the editor ---
    pb::Macro g_macro;        // what BOT replays (last hand-played run, saved per level)
    pb::Macro g_recMacro;     // clicks of the run being hand-played right now
    std::string g_macroKey;   // file key of the level the editor has open
    bool g_botEnabled = false;

    // --- one play session (editor playtest or real attempt) ---
    enum class Where { None, Editor, Play };
    struct Session {
        Where where = Where::None;
        GJBaseGameLayer* layer = nullptr;
        bool botActive = false;            // bot presses, player's taps are ignored
        pb::Macro const* replay = nullptr; // clicks the bot plays
        bool recordInputs = false;         // hand-played editor run: store clicks
        int count = 0;                     // player-1 physics steps finished
        bool aligned = false;
        int offset = 0;                    // steps to skip so replay lines up with the recording
        size_t index = 0;
        bool dead = false;
    };
    Session g_s;
    bool g_injecting = false;

    // --- real-play bot / safe mode ---
    pb::Macro g_playMacro;
    bool g_playSafe = false;               // safe mode for the whole PlayLayer session
    int g_frozenJumps = 0, g_frozenAttempts = 0;

    // --- frame-perfect checker ---
    struct Variant { int index; int delta; };
    bool g_checking = false;
    std::vector<Variant> g_variants;
    int g_variantPos = -1;                 // -1 = baseline run (unchanged clicks)
    pb::Macro g_checkMacro;
    bool g_testDone = false;
    bool g_userStopped = false;            // player tapped the playtest STOP button
    int g_checkSpeed = 1;                  // simulated frames per real frame while checking
    double g_recDtSum = 0.0;               // frame times of the hand-played run
    int g_recFrames = 0;
    bool g_testSurvived = false;
    struct VariantResult { int index; int delta; bool survived; };
    std::vector<VariantResult> g_results;

    // -----------------------------------------------------------------------
    bool showMessages() { return Mod::get()->getSettingValue<bool>("show-messages"); }

    void notify(std::string const& text, NotificationIcon icon) {
        bool important = icon == NotificationIcon::Warning || icon == NotificationIcon::Error;
        if (!important && !showMessages()) return;
        Notification::create(text, icon)->show();
    }

    std::filesystem::path macroPath(std::string const& key) {
        return Mod::get()->getSaveDir() / "macros" / (key + ".txt");
    }

    // Load the macro for a level: exact (name + id) first, then the local copy (id 0).
    bool loadMacroFor(GJGameLevel* level, pb::Macro& out, std::string* keyOut = nullptr) {
        if (!level) return false;
        std::string name = level->m_levelName;
        int id = level->m_levelID.value();
        auto key = pb::macroKey(name, id);
        if (keyOut) *keyOut = key;
        if (out.load(macroPath(key)) && !out.empty()) return true;
        if (id != 0 && out.load(macroPath(pb::macroKey(name, 0))) && !out.empty()) return true;
        out.clear();
        return false;
    }

    bool sessionLayerAlive() {
        if (g_s.where == Where::Editor) return LevelEditorLayer::get() && static_cast<GJBaseGameLayer*>(LevelEditorLayer::get()) == g_s.layer;
        if (g_s.where == Where::Play) return PlayLayer::get() && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == g_s.layer;
        return false;
    }

    PlayerObject* sessionPlayer() {
        return sessionLayerAlive() ? g_s.layer->m_player1 : nullptr;
    }

    void endSession() { g_s = Session{}; }

    // Press/release every click due by `step` (steps counted like in the recording).
    void botFire(PlayerObject* p, int step) {
        if (!g_s.botActive || !g_s.replay || !p || p->m_isDead) return;
        auto const& in = g_s.replay->inputs;
        while (g_s.index < in.size() && in[g_s.index].step <= step) {
            auto const& e = in[g_s.index++];
            g_injecting = true;
            if (e.down) p->pushButton(PlayerButton::Jump);
            else p->releaseButton(PlayerButton::Jump);
            g_injecting = false;
        }
    }

    void startSession(Where where, GJBaseGameLayer* layer, bool bot, pb::Macro const* replay, bool recordInputs) {
        g_s = Session{};
        g_s.where = where;
        g_s.layer = layer;
        g_s.botActive = bot && replay && !replay->empty();
        g_s.replay = replay;
        g_s.recordInputs = recordInputs;
        g_holding = false;
        g_pressedThisFrame = false;
        // Clicks made before the first physics step.
        if (g_s.botActive) botFire(layer->m_player1, 0);
    }

    // Called after every physics step of the session's player 1.
    void onStep(PlayerObject* p) {
        if (g_s.dead) return;
        if (p->m_isDead) { g_s.dead = true; return; }
        g_s.count++;

        if (g_s.recordInputs) {
            if (g_s.count == 1) { g_recMacro.x0 = p->getPositionX(); g_recMacro.hasX0 = true; }
            g_recMacro.endStep = g_s.count;
        }

        if (g_s.botActive) {
            // Line the replay up with the recording: the recording's first step ended at
            // x0. If this run has extra steps before the player starts moving, skip them.
            if (!g_s.aligned) {
                if (!g_s.replay->hasX0) {
                    g_s.aligned = true;
                } else if (std::fabs(p->getPositionX() - g_s.replay->x0) < 0.05f) {
                    g_s.aligned = true;
                    g_s.offset = g_s.count - 1;
                } else if (g_s.count > 480) {
                    g_s.aligned = true;   // never matched: fall back to plain step count
                }
            }
            if (g_s.aligned) botFire(p, g_s.count - g_s.offset);
        }
    }

    void recordInput(PlayerObject* p, bool down) {
        if (!g_s.recordInputs || g_s.botActive || p != sessionPlayer()) return;
        g_recMacro.inputs.push_back({g_s.count, down, p->getPositionX()});
    }

    pb::Mode modeOf(PlayerObject* p) {
        if (p->m_isShip) return pb::Mode::Ship;
        if (p->m_isBird) return pb::Mode::Ufo;
        if (p->m_isBall) return pb::Mode::Ball;
        if (p->m_isDart) return pb::Mode::Wave;
        if (p->m_isRobot) return pb::Mode::Robot;
        if (p->m_isSpider) return pb::Mode::Spider;
        if (p->m_isSwing) return pb::Mode::Swing;
        return pb::Mode::Cube;
    }

    pb::Settings readSettings() {
        auto mod = Mod::get();
        pb::Settings st;
        auto style = mod->getSettingValue<std::string>("style");
        st.style = style == "dense" ? pb::Style::Dense
                 : style == "blocks-only" ? pb::Style::BlocksOnly
                 : style == "tight" ? pb::Style::Tight
                 : pb::Style::Basic;
        st.cube = mod->getSettingValue<bool>("mode-cube");
        st.ship = mod->getSettingValue<bool>("mode-ship");
        st.wave = mod->getSettingValue<bool>("mode-wave");
        st.ball = mod->getSettingValue<bool>("mode-ball");
        st.ufo = mod->getSettingValue<bool>("mode-ufo");
        st.maxSpikes = static_cast<int>(mod->getSettingValue<int64_t>("max-spikes"));
        constexpr float kBlock = 30.f;
        auto blocks = [&](char const* k) { return static_cast<float>(mod->getSettingValue<double>(k)) * kBlock; };
        auto pixels = [&](char const* k) { return static_cast<float>(mod->getSettingValue<double>(k)); };
        st.spikeClearance = blocks("clearance-blocks");
        st.shipMargin = blocks("ship-margin-blocks");
        st.waveMargin = blocks("wave-margin-blocks");
        st.tightGap = pixels("tight-gap");
        st.portalGap = pixels("portal-gap");
        st.spikeSpacing = pixels("tight-spacing");
        st.blockID = static_cast<int>(mod->getSettingValue<int64_t>("block-id"));
        st.spikeID = static_cast<int>(mod->getSettingValue<int64_t>("spike-id"));
        return st;
    }

    void finishCheckTest();
}

// ===========================================================================
// Clicks: track holding, record hand-played clicks, block taps while the bot plays
// ===========================================================================
class $modify(PBPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool mine = button == PlayerButton::Jump && this == sessionPlayer();
        if (mine && g_s.botActive && !g_injecting) return false;
        bool ret = PlayerObject::pushButton(button);
        if (mine) {
            g_holding = true;
            g_pressedThisFrame = true;
            recordInput(this, true);
        }
        return ret;
    }

    bool releaseButton(PlayerButton button) {
        bool mine = button == PlayerButton::Jump && this == sessionPlayer();
        if (mine && g_s.botActive && !g_injecting) return false;
        bool ret = PlayerObject::releaseButton(button);
        if (mine) {
            g_holding = false;
            recordInput(this, false);
        }
        return ret;
    }

    void incrementJumps() {
        if (g_playSafe && PlayLayer::get()) return;   // safe mode: freeze jump count
        PlayerObject::incrementJumps();
    }
};

// ===========================================================================
// Editor playtests
// ===========================================================================
class $modify(PBEditorLayer, LevelEditorLayer) {
    void onPlaytest() {
        bool fresh = m_playbackMode == PlaybackMode::Not;
        LevelEditorLayer::onPlaytest();
        if (!fresh) return;

        if (g_checking) {
            startSession(Where::Editor, this, true, &g_checkMacro, false);
            return;
        }

        g_samples.clear();
        g_samples.reserve(16384);
        g_recording = true;
        g_stoppedByDeath = false;

        bool bot = g_botEnabled && !g_macro.empty();
        if (bot) {
            notify(fmt::format("Bot: replaying {} clicks.", g_macro.inputs.size()), NotificationIcon::Info);
            startSession(Where::Editor, this, true, &g_macro, false);
        } else {
            g_recMacro.clear();
            g_recDtSum = 0.0;
            g_recFrames = 0;
            startSession(Where::Editor, this, false, nullptr, true);
        }
    }

    void onStopPlaytest() {
        bool wasHandRun = g_s.where == Where::Editor && g_s.recordInputs;
        // A check run stopped from outside. In the editor GD stops the playtest itself,
        // right inside the collision check, when the player dies - so any stop we did
        // not start is a death, unless the player tapped the STOP button.
        bool checkRunOpen = g_checking && !g_testDone;
        bool userStopped = g_userStopped;
        g_userStopped = false;
        LevelEditorLayer::onStopPlaytest();
        if (checkRunOpen && !userStopped) {
            g_testDone = true;
            g_testSurvived = false;
            Loader::get()->queueInMainThread([] { finishCheckTest(); });
        } else if (checkRunOpen) {
            g_checking = false;
            CCDirector::get()->getScheduler()->setTimeScale(1.f);
            Loader::get()->queueInMainThread([] { finishCheckTest(); });  // clears the progress label
            notify("Frame-perfect check cancelled.", NotificationIcon::Warning);
        }

        if (wasHandRun) {
            // Stopping the playtest releases the button with the player already reset
            // to the start; that isn't a real click.
            auto& in = g_recMacro.inputs;
            while (in.size() > 1 && !in.back().down && in.back().x < 1.f && in[in.size() - 2].x > 30.f) in.pop_back();
        }
        if (wasHandRun && !g_recMacro.empty()) {
            if (g_recFrames > 0) g_recMacro.frameDt = static_cast<float>(g_recDtSum / g_recFrames);
            g_macro = g_recMacro;
            if (!g_macroKey.empty()) g_macro.save(macroPath(g_macroKey));
        }
        if (g_s.where == Where::Editor) endSession();

        if (g_recording) {
            g_recording = false;
            if (g_samples.size() >= 2) {
                notify(fmt::format("Path recorded ({} steps, hitbox {:.1f}x{:.1f}).",
                                   g_samples.size(), g_lastHitW, g_lastHitH), NotificationIcon::Success);
            }
        }
    }
};

// ===========================================================================
// Physics steps: record the path, count steps, drive the bot and the checker
// ===========================================================================
class $modify(PBGameLayer, GJBaseGameLayer) {
    // CHK speed: instead of speeding up time (which packs many physics steps into
    // one frame and changes what GD checks per frame), run several ordinary
    // 120 fps frames per real frame. Each one behaves exactly like normal play.
    void update(float dt) {
        auto lel = LevelEditorLayer::get();
        bool checkRun = g_checking && lel && static_cast<GJBaseGameLayer*>(lel) == this &&
                        lel->m_playbackMode == PlaybackMode::Playing;
        if (!checkRun) {
            if (g_s.where == Where::Editor && g_s.recordInputs && g_s.layer == this && dt > 0.f) {
                g_recDtSum += dt;
                g_recFrames++;
            }
            GJBaseGameLayer::update(dt);
            return;
        }
        // Same frame rate as the run was recorded at (falls back to the live one).
        float kFrame = g_macro.frameDt > 0.f ? g_macro.frameDt : dt;
        for (int i = 0; i < g_checkSpeed; i++) {
            if (!g_checking || g_testDone || lel->m_playbackMode != PlaybackMode::Playing) break;
            GJBaseGameLayer::update(kFrame);
        }
    }

    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        bool recorded = recordStep(player);
        int ret = GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
        // Touching the ground/ceiling, GD pushes the player out inside this call, so
        // also keep the corrected position: spikes have to clear both.
        if (recorded && player && !player->m_isDead && !g_samples.empty()) {
            auto pos = player->getPosition();
            auto const& last = g_samples.back();
            if (std::fabs(pos.x - last.x) > 1e-4f || std::fabs(pos.y - last.y) > 1e-4f) {
                pb::Sample s = last;
                s.x = pos.x;
                s.y = pos.y;
                s.onGround = player->m_isOnGround;
                g_samples.push_back(s);
            }
        }

        if (g_s.where != Where::None && g_s.layer == this && player == this->m_player1 && sessionLayerAlive()) {
            onStep(player);

            if (g_checking && g_s.where == Where::Editor && !g_testDone) {
                bool died = player->m_isDead;
                bool reachedEnd = !died && g_s.count >= g_macro.endStep;
                if (died || reachedEnd) {
                    g_testDone = true;
                    g_testSurvived = reachedEnd;
                    Loader::get()->queueInMainThread([] { finishCheckTest(); });
                }
            }
        }
        return ret;
    }

    bool recordStep(PlayerObject* p) {
        if (!g_recording || g_stoppedByDeath || !p || g_checking) return false;
        auto lel = LevelEditorLayer::get();
        if (!lel || static_cast<GJBaseGameLayer*>(lel) != this) return false;
        if (lel->m_playbackMode != PlaybackMode::Playing) return false;
        if (p != lel->m_player1) return false;
        if (p->m_isDead) { g_stoppedByDeath = true; return false; }

        auto pos = p->getPosition();
        pb::Sample s;
        s.x = pos.x;
        s.y = pos.y;
        s.size = p->m_vehicleSize > 0.f ? p->m_vehicleSize : 1.f;
        auto rect = p->getObjectRect();
        if (rect.size.width > 0.5f && rect.size.width <= 35.f &&
            rect.size.height > 0.5f && rect.size.height <= 35.f) {
            s.halfW = rect.size.width * 0.5f;
            s.halfH = rect.size.height * 0.5f;
        } else {
            s.halfW = s.halfH = 15.f * s.size;
        }
        g_lastHitW = s.halfW * 2.f;
        g_lastHitH = s.halfH * 2.f;
        s.mode = modeOf(p);
        s.upsideDown = p->m_isUpsideDown;
        s.onGround = p->m_isOnGround;
        s.holding = g_holding || g_pressedThisFrame;
        g_pressedThisFrame = false;
        g_samples.push_back(s);
        return true;
    }
};

// ===========================================================================
// Real play: showcase bot + safe mode (nothing is saved while it is on)
// ===========================================================================
class $modify(PBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_playMacro.clear();
        g_playSafe = false;
        if (Mod::get()->getSettingValue<bool>("play-bot")) {
            if (loadMacroFor(level, g_playMacro)) {
                g_playSafe = true;
            } else {
                Loader::get()->queueInMainThread([] {
                    Notification::create("Showcase bot: no saved clicks for this level.", NotificationIcon::Warning)->show();
                });
            }
        }
        auto gsm = GameStatsManager::sharedState();
        g_frozenJumps = gsm->getStat("1");
        g_frozenAttempts = gsm->getStat("2");
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        if (g_playSafe && m_level) m_level->m_attempts = m_level->m_attempts.value() - 1;
        PlayLayer::resetLevel();

        bool canBot = g_playSafe && !m_isPracticeMode && !m_startPosObject && !m_isPlatformer;
        if (canBot) startSession(Where::Play, this, true, &g_playMacro, false);
        else if (g_s.where == Where::Play) endSession();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (!g_playSafe) { PlayLayer::destroyPlayer(player, object); return; }
        bool testMode = m_isTestMode;
        int percent = m_level ? m_level->m_normalPercent.value() : 0;
        m_isTestMode = true;
        PlayLayer::destroyPlayer(player, object);
        m_isTestMode = testMode;
        if (m_level) m_level->m_normalPercent = percent;
        auto gsm = GameStatsManager::sharedState();
        gsm->setStat("1", g_frozenJumps);
        gsm->setStat("2", g_frozenAttempts);
    }

    void levelComplete() {
        if (!g_playSafe) { PlayLayer::levelComplete(); return; }
        // Play the end screen (showcases need it) but save nothing and never verify.
        bool testMode = m_isTestMode;
        int percent = m_level ? m_level->m_normalPercent.value() : 0;
        int verified = m_level ? m_level->m_isVerified.value() : 0;
        bool verifiedRaw = m_level ? m_level->m_isVerifiedRaw : false;
        m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = testMode;
        if (m_level) {
            m_level->m_normalPercent = percent;
            m_level->m_isVerified = verified;
            m_level->m_isVerifiedRaw = verifiedRaw;
        }
    }

    void onQuit() {
        if (g_s.where == Where::Play) endSession();
        g_playSafe = false;
        PlayLayer::onQuit();
    }
};

class $modify(PBGameLevel, GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        if (g_playSafe && PlayLayer::get()) return;   // safe mode: no progress
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};

// ===========================================================================
// Editor buttons: AUTO, SET, BOT, CHK
// ===========================================================================
class $modify(PBEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        // Clicks saved for this level from an earlier session.
        g_macro.clear();
        g_macroKey.clear();
        loadMacroFor(editorLayer->m_level, g_macro, &g_macroKey);
        g_checking = false;

        auto menu = CCMenu::create();
        menu->setID("path-builder-menu"_spr);

        auto makeSpr = [](char const* text, CircleBaseColor color) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(0.45f);
            auto spr = CircleButtonSprite::create(label, color, CircleBaseSize::Small);
            spr->setScale(0.7f);
            return spr;
        };
        auto autoBtn = CCMenuItemSpriteExtra::create(makeSpr("AUTO", CircleBaseColor::Green), this, menu_selector(PBEditorUI::onPathBuild));
        autoBtn->setID("auto-button"_spr);
        auto setBtn = CCMenuItemSpriteExtra::create(makeSpr("SET", CircleBaseColor::Gray), this, menu_selector(PBEditorUI::onPathSettings));
        setBtn->setID("settings-button"_spr);
        auto botBtn = CCMenuItemToggler::create(
            makeSpr("BOT", CircleBaseColor::Gray), makeSpr("BOT", CircleBaseColor::Cyan),
            this, menu_selector(PBEditorUI::onPathBot));
        botBtn->toggle(g_botEnabled);
        botBtn->setID("bot-button"_spr);
        auto chkBtn = CCMenuItemSpriteExtra::create(makeSpr("CHK", CircleBaseColor::Pink), this, menu_selector(PBEditorUI::onPathCheck));
        chkBtn->setID("check-button"_spr);

        auto winSize = CCDirector::get()->getWinSize();
        CCPoint base = {12.f, winSize.height * 0.8f};
        if (auto playtestMenu = this->getChildByID("playtest-menu")) {
            base.x = playtestMenu->getPositionX();
            base.y = playtestMenu->boundingBox().getMinY() + 60.f;
        }
        if (auto playbackMenu = this->getChildByID("playback-menu")) {
            base.y = playbackMenu->boundingBox().getMaxY();
        }
        // Put the column right of the playtest STOP button (it only shows while
        // playtesting, next to the play button), so nothing overlaps then.
        auto rightEdge = [this](CCNode* n) -> float {
            if (!n || !n->getParent()) return -1.f;
            auto bb = n->boundingBox();
            auto world = n->getParent()->convertToWorldSpace(CCPoint(bb.getMaxX(), bb.getMidY()));
            return this->convertToNodeSpace(world).x;
        };
        auto leftEdge = [this](CCNode* n) -> float {
            if (!n || !n->getParent()) return -1.f;
            auto bb = n->boundingBox();
            auto world = n->getParent()->convertToWorldSpace(CCPoint(bb.getMinX(), bb.getMidY()));
            return this->convertToNodeSpace(world).x;
        };
        float playR = rightEdge(m_playtestBtn);
        float stopR = rightEdge(m_playtestStopBtn);
        // If the hidden stop button hasn't been laid out yet, assume it sits right of
        // the play button with the same width.
        if (playR > 0.f && stopR < playR + 5.f) stopR = playR + 4.f + (playR - leftEdge(m_playtestBtn));
        float right = std::max({stopR, playR, rightEdge(m_playbackBtn)});
        if (right > 0.f) base.x = right + 6.f;
        else base.x += 70.f;
        menu->setPosition(base);
        // Stacked vertically: AUTO, SET, BOT, CHK
        autoBtn->setPosition({18.f, -15.f});
        setBtn->setPosition({18.f, -45.f});
        botBtn->setPosition({18.f, -75.f});
        chkBtn->setPosition({18.f, -105.f});
        menu->addChild(autoBtn);
        menu->addChild(setBtn);
        menu->addChild(botBtn);
        menu->addChild(chkBtn);
        this->addChild(menu, 10);
        return true;
    }

    void onStopPlaytest(CCObject* sender) {
        if (sender && g_checking) g_userStopped = true;
        EditorUI::onStopPlaytest(sender);
    }

    void onPathBot(CCObject* sender) {
        bool on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        g_botEnabled = on;
        if (!on) {
            notify("Bot OFF - your own taps are recorded.", NotificationIcon::Info);
        } else if (g_macro.empty()) {
            notify("Bot ON - play once yourself first so it has clicks to copy.", NotificationIcon::Warning);
        } else {
            notify(fmt::format("Bot ON - next playtest replays {} clicks.", g_macro.inputs.size()), NotificationIcon::Success);
        }
    }

    void onPathSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get(), false);
    }

    // -----------------------------------------------------------------------
    // Frame-perfect check: replay the saved clicks with one click moved a single
    // physics step earlier or later. If the player still survives, that click is
    // not frame perfect. A baseline run first proves the unchanged clicks pass.
    // -----------------------------------------------------------------------
    void onPathCheck(CCObject*) {
        if (g_checking) {
            stopCheck();
            notify("Frame-perfect check cancelled.", NotificationIcon::Warning);
            return;
        }
        if (m_editorLayer->m_playbackMode != PlaybackMode::Not) {
            notify("Stop the playtest first.", NotificationIcon::Warning);
            return;
        }
        if (g_macro.empty() || g_macro.endStep <= 0) {
            notify("No saved clicks - play the level once yourself first.", NotificationIcon::Warning);
            return;
        }

        g_variants.clear();
        g_results.clear();
        auto const& in = g_macro.inputs;
        for (int i = 0; i < static_cast<int>(in.size()); i++) {
            for (int d : {-1, 1}) {
                int s = in[i].step + d;
                if (s < 0) continue;
                if (i > 0 && s < in[i - 1].step) continue;                       // keep click order
                if (i + 1 < static_cast<int>(in.size()) && s > in[i + 1].step) continue;
                g_variants.push_back({i, d});
            }
        }

        g_checking = true;
        g_variantPos = -1;
        g_checkSpeed = std::max<int>(1, static_cast<int>(Mod::get()->getSettingValue<int64_t>("check-speed")));
        queueNextCheck();
    }

    // Next run is started on the next frame, outside GD's own playtest handling.
    static void queueNextCheck() {
        Loader::get()->queueInMainThread([] {
            Loader::get()->queueInMainThread([] {
                auto lel = LevelEditorLayer::get();
                if (lel && lel->m_editorUI) static_cast<PBEditorUI*>(lel->m_editorUI)->runNextCheck();
            });
        });
    }

    // Progress shown on screen while checking: "CHK 3 / 25".
    void setCheckProgress(std::string const& text) {
        auto label = static_cast<CCLabelBMFont*>(this->getChildByID("check-progress"_spr));
        if (!label) {
            label = CCLabelBMFont::create("", "goldFont.fnt");
            label->setID("check-progress"_spr);
            label->setScale(0.6f);
            auto winSize = CCDirector::get()->getWinSize();
            label->setPosition({winSize.width / 2.f, winSize.height - 60.f});
            this->addChild(label, 100);
        }
        label->setString(text.c_str());
    }

    void clearCheckProgress() {
        if (auto label = this->getChildByID("check-progress"_spr)) label->removeFromParent();
    }

    void runNextCheck() {
        if (!g_checking) { clearCheckProgress(); return; }
        int total = static_cast<int>(g_variants.size()) + 1;
        setCheckProgress(g_variantPos < 0 ? fmt::format("CHK baseline / {} runs", total)
                                          : fmt::format("CHK {} / {}", g_variantPos + 2, total));
        g_checkMacro = g_macro;
        if (g_variantPos >= 0) {
            auto const& v = g_variants[g_variantPos];
            g_checkMacro.inputs[v.index].step += v.delta;
        }
        g_testDone = false;
        g_testSurvived = false;
        this->onPlaytest(nullptr);
    }

    void afterCheckTest() {
        if (m_editorLayer->m_playbackMode != PlaybackMode::Not) this->onStopPlaytest(nullptr);
        if (!g_checking) { clearCheckProgress(); return; }

        if (g_variantPos < 0) {
            if (!g_testSurvived) {
                stopCheck();
                MDPopup::create("Frame Perfect Check",
                    "## Does not pass\n\nThe bot dies even with the exact recorded clicks, so this layout "
                    "is not passable on the recorded path (gap too small?).", "OK")->show();
                return;
            }
        } else {
            auto const& v = g_variants[g_variantPos];
            g_results.push_back({v.index, v.delta, g_testSurvived});
        }

        g_variantPos++;
        if (g_variantPos >= static_cast<int>(g_variants.size())) {
            stopCheck();
            showCheckResults();
            return;
        }
        queueNextCheck();
    }

    void stopCheck() {
        g_checking = false;
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        clearCheckProgress();
        if (m_editorLayer->m_playbackMode != PlaybackMode::Not) this->onStopPlaytest(nullptr);
    }

    void showCheckResults() {
        auto const& in = g_macro.inputs;
        std::vector<bool> earlyOk(in.size(), false), lateOk(in.size(), false);
        for (auto const& r : g_results) {
            if (!r.survived) continue;
            if (r.delta < 0) earlyOk[r.index] = true; else lateOk[r.index] = true;
        }
        int perfect = 0;
        std::string list;
        for (size_t i = 0; i < in.size(); i++) {
            if (!earlyOk[i] && !lateOk[i]) { perfect++; continue; }
            list += fmt::format("- **#{}** {} @ {:.1f} blocks: {}\n", i + 1, in[i].down ? "press" : "release",
                                in[i].x / 30.f,
                                earlyOk[i] && lateOk[i] ? "early & late OK" : earlyOk[i] ? "early OK" : "late OK");
        }
        std::string text = fmt::format(
            "# {} / {} frame perfect\n\nMoving a frame-perfect click 1 physics step earlier or later kills you.\n\n"
            "Checked at **{} fps** (the frame rate the clicks were recorded at).\n\n",
            perfect, in.size(), g_macro.frameDt > 0.f ? static_cast<int>(std::lround(1.f / g_macro.frameDt)) : 0);
        if (!list.empty()) text += "## Not frame perfect\n\n" + list;
        MDPopup::create("Frame Perfect Check", text, "OK")->show();
    }

    // -----------------------------------------------------------------------
    void onPathBuild(CCObject*) {
        if (m_editorLayer->m_playbackMode != PlaybackMode::Not) {
            notify("Stop the playtest first.", NotificationIcon::Warning);
            return;
        }
        if (g_samples.size() < 2) {
            notify("No path yet - start a playtest and play through first.", NotificationIcon::Warning);
            return;
        }

        std::vector<pb::ExistingCell> existing;
        if (auto objs = m_editorLayer->m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(objs)) {
                auto type = obj->m_objectType;
                if (type != GameObjectType::Solid && type != GameObjectType::Hazard &&
                    type != GameObjectType::Slope && type != GameObjectType::Breakable) continue;
                auto pos = obj->getPosition();
                existing.push_back({
                    static_cast<int>(std::floor(pos.x / pb::kCell)),
                    static_cast<int>(std::floor(pos.y / pb::kCell))
                });
            }
        }

        auto settings = readSettings();
        if (auto probe = GameObject::createWithKey(settings.spikeID)) {
            probe->setPosition({0.f, 0.f});
            auto r = probe->getObjectRect();
            if (r.size.width > 0.5f && r.size.width < 25.f && r.size.height > 0.5f && r.size.height < 25.f) {
                settings.spikeHalfW = r.size.width * 0.5f;
                settings.spikeHalfH = r.size.height * 0.5f;
                float ox = r.getMidX(), oy = r.getMidY();
                settings.spikeOffX = std::fabs(ox) <= 6.f ? ox : 0.f;
                settings.spikeOffY = std::fabs(oy) <= 6.f ? oy : 0.f;
            }
        }

        auto placements = pb::generate(g_samples, settings, existing);
        if (placements.empty()) {
            notify("Nothing to place (check the mode toggles in SET).", NotificationIcon::Info);
            return;
        }

        auto created = CCArray::create();
        int blocks = 0, spikes = 0;
        for (auto const& p : placements) {
            auto obj = m_editorLayer->createObject(p.objectID, {p.x, p.y}, true);
            if (!obj) continue;
            if (p.rotation != 0.f) obj->setRotation(p.rotation);
            created->addObject(obj);
            (p.isSpike ? spikes : blocks)++;
        }
        if (created->count() == 0) return;

        m_editorLayer->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        if (Mod::get()->getSettingValue<bool>("select-generated")) {
            this->deselectAll();
            this->selectObjects(created, true);
        }
        this->updateButtons();
        notify(fmt::format("Placed {} blocks and {} spikes.", blocks, spikes), NotificationIcon::Success);
    }
};

namespace {
    void finishCheckTest() {
        auto lel = LevelEditorLayer::get();
        if (!lel || !lel->m_editorUI) { g_checking = false; return; }
        static_cast<PBEditorUI*>(lel->m_editorUI)->afterCheckTest();
    }
}
