#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <cmath>

#include "Generator.hpp"

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Recording state (one recording = the most recent editor playtest)
// ---------------------------------------------------------------------------
namespace {
    std::vector<pb::Sample> g_samples;
    bool g_recording = false;
    bool g_stoppedByDeath = false;
    bool g_holding = false;        // jump button currently held
    bool g_pressedThisFrame = false; // catches taps shorter than one frame

    // Playtest bot: replays the clicks of the last hand-played playtest, keyed to GD's
    // own physics step counter so every click lands on exactly the same step.
    struct InputEvent { int step; bool down; };
    std::vector<InputEvent> g_inputs;  // clicks from the last hand-played playtest
    bool g_botEnabled = false;         // BOT toggle
    bool g_botActive = false;          // replaying during the current playtest
    bool g_sessionActive = false;      // inside a playtest started from the editor
    int g_stepCount = 0;               // physics steps finished in this playtest (player 1)
    size_t g_botIndex = 0;
    bool g_injecting = false;          // the bot itself is pressing

    // A click is stored with the number of physics steps that had finished when it
    // happened. The bot presses again at that same point between two steps.
    void recordInput(PlayerObject* p, bool down) {
        if (!g_sessionActive || g_botActive) return;
        auto lel = LevelEditorLayer::get();
        if (!lel || lel->m_player1 != p || lel->m_playbackMode != PlaybackMode::Playing) return;
        g_inputs.push_back({g_stepCount, down});
    }

    void botFire(PlayerObject* p) {
        if (!g_botActive || !p || p->m_isDead) return;
        while (g_botIndex < g_inputs.size() && g_inputs[g_botIndex].step <= g_stepCount) {
            auto const& e = g_inputs[g_botIndex++];
            g_injecting = true;
            if (e.down) p->pushButton(PlayerButton::Jump);
            else p->releaseButton(PlayerButton::Jump);
            g_injecting = false;
        }
    }

    bool showMessages() { return Mod::get()->getSettingValue<bool>("show-messages"); }

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

    bool isEditorPlayer1(PlayerObject* p) {
        auto lel = LevelEditorLayer::get();
        return lel && lel->m_player1 == p;
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
        // Distances: blocks (1 = 1 grid cell = 30 units) or free-move pixels (1 px = 1 unit).
        constexpr float kBlock = 30.f;
        auto blocks = [&](char const* k) { return static_cast<float>(mod->getSettingValue<double>(k)) * kBlock; };
        auto pixels = [&](char const* k) { return static_cast<float>(mod->getSettingValue<double>(k)); };
        st.spikeClearance = blocks("clearance-blocks");
        st.shipMargin = blocks("ship-margin-blocks");
        st.waveMargin = blocks("wave-margin-blocks");
        st.tightGap = pixels("tight-gap");
        st.spikeSpacing = pixels("tight-spacing");
        st.blockID = static_cast<int>(mod->getSettingValue<int64_t>("block-id"));
        st.spikeID = static_cast<int>(mod->getSettingValue<int64_t>("spike-id"));
        return st;
    }

    void notify(std::string const& text, NotificationIcon icon) {
        // Warnings/errors always show; everything else only if "Show Messages" is on.
        bool important = icon == NotificationIcon::Warning || icon == NotificationIcon::Error;
        if (!important && !showMessages()) return;
        Notification::create(text, icon)->show();
    }
}

// ---------------------------------------------------------------------------
// Jump button tracking
// ---------------------------------------------------------------------------
class $modify(PBPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton button) {
        if (g_botActive && !g_injecting && isEditorPlayer1(this)) return false; // bot is playing
        bool ret = PlayerObject::pushButton(button);
        if (button == PlayerButton::Jump && isEditorPlayer1(this)) {
            g_holding = true;
            g_pressedThisFrame = true;
            recordInput(this, true);
        }
        return ret;
    }

    bool releaseButton(PlayerButton button) {
        if (g_botActive && !g_injecting && isEditorPlayer1(this)) return false; // bot is playing
        bool ret = PlayerObject::releaseButton(button);
        if (button == PlayerButton::Jump && isEditorPlayer1(this)) {
            g_holding = false;
            recordInput(this, false);
        }
        return ret;
    }
};

// ---------------------------------------------------------------------------
// Playtest recording
// ---------------------------------------------------------------------------
class $modify(PBEditorLayer, LevelEditorLayer) {
    void onPlaytest() {
        bool fresh = m_playbackMode == PlaybackMode::Not;
        LevelEditorLayer::onPlaytest();
        if (fresh) {
            g_samples.clear();
            g_samples.reserve(16384);
            g_recording = true;
            g_stoppedByDeath = false;
            g_holding = false;
            g_pressedThisFrame = false;

            g_sessionActive = true;
            g_stepCount = 0;
            g_botIndex = 0;
            g_botActive = g_botEnabled && !g_inputs.empty();
            if (g_botActive) {
                notify(fmt::format("Bot: replaying {} clicks.", g_inputs.size()), NotificationIcon::Info);
                botFire(m_player1);   // clicks made before the first physics step
            } else {
                g_inputs.clear();   // hand-played run: record fresh clicks
                g_inputs.reserve(1024);
            }
        }
    }

    void onStopPlaytest() {
        LevelEditorLayer::onStopPlaytest();
        g_sessionActive = false;
        g_botActive = false;
        if (g_recording) {
            g_recording = false;
            if (g_samples.size() >= 2) {
                notify(
                    fmt::format("Path recorded ({} physics steps). Tap AUTO to build.", g_samples.size()),
                    NotificationIcon::Success
                );
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Record every physics step (GD runs several per frame). Recording here, right
// before GD checks collisions, captures exactly the positions and hitboxes GD
// tests against hazards - including the sharp corner of every wave click.
// ---------------------------------------------------------------------------
class $modify(PBGameLayer, GJBaseGameLayer) {
    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        recordStep(player);
        int ret = GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
        auto lel = LevelEditorLayer::get();
        if (g_sessionActive && lel && static_cast<GJBaseGameLayer*>(lel) == this &&
            lel->m_playbackMode == PlaybackMode::Playing && player == lel->m_player1) {
            g_stepCount++;
            botFire(player);   // presses due before the next physics step
        }
        return ret;
    }

    void recordStep(PlayerObject* p) {
        if (!g_recording || g_stoppedByDeath || !p) return;
        auto lel = LevelEditorLayer::get();
        if (!lel || static_cast<GJBaseGameLayer*>(lel) != this) return;
        if (lel->m_playbackMode != PlaybackMode::Playing) return;
        if (p != lel->m_player1) return;
        if (p->m_isDead) {
            // Anything after a death would describe a path the player can't survive.
            g_stoppedByDeath = true;
            return;
        }

        auto pos = p->getPosition();
        pb::Sample s;
        s.x = pos.x;
        s.y = pos.y;
        s.size = p->m_vehicleSize > 0.f ? p->m_vehicleSize : 1.f;
        // Real hitbox from the game (the wave's is much smaller than the cube's).
        auto rect = p->getObjectRect();
        if (rect.size.width > 0.5f && rect.size.height > 0.5f) {
            s.x = rect.getMidX();
            s.y = rect.getMidY();
            s.halfW = rect.size.width * 0.5f;
            s.halfH = rect.size.height * 0.5f;
        } else {
            s.halfW = s.halfH = 15.f * s.size;
        }
        s.mode = modeOf(p);
        s.upsideDown = p->m_isUpsideDown;
        s.onGround = p->m_isOnGround;
        s.holding = g_holding || g_pressedThisFrame;
        g_pressedThisFrame = false;
        g_samples.push_back(s);
    }
};

// ---------------------------------------------------------------------------
// Editor buttons: AUTO (generate), SET (settings), BOT (playtest replay)
// ---------------------------------------------------------------------------
class $modify(PBEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        auto menu = CCMenu::create();
        menu->setID("path-builder-menu"_spr);

        auto makeBtn = [&](char const* text, CircleBaseColor color, SEL_MenuHandler cb) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(0.45f);
            auto spr = CircleButtonSprite::create(label, color, CircleBaseSize::Small);
            spr->setScale(0.8f);
            return CCMenuItemSpriteExtra::create(spr, this, cb);
        };

        auto autoBtn = makeBtn("AUTO", CircleBaseColor::Green, menu_selector(PBEditorUI::onPathBuild));
        autoBtn->setID("auto-button"_spr);
        auto setBtn = makeBtn("SET", CircleBaseColor::Gray, menu_selector(PBEditorUI::onPathSettings));
        setBtn->setID("settings-button"_spr);

        auto makeSpr = [](char const* text, CircleBaseColor color) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(0.45f);
            auto spr = CircleButtonSprite::create(label, color, CircleBaseSize::Small);
            spr->setScale(0.8f);
            return spr;
        };
        auto botBtn = CCMenuItemToggler::create(
            makeSpr("BOT", CircleBaseColor::Gray), makeSpr("BOT", CircleBaseColor::Cyan),
            this, menu_selector(PBEditorUI::onPathBot)
        );
        botBtn->toggle(g_botEnabled);
        botBtn->setID("bot-button"_spr);

        // Place the buttons just below the playtest buttons (left side of the editor).
        auto winSize = CCDirector::get()->getWinSize();
        CCPoint base = {12.f, winSize.height * 0.55f};
        if (auto playtestMenu = this->getChildByID("playtest-menu")) {
            base = CCPoint(playtestMenu->getPositionX(), playtestMenu->boundingBox().getMinY() - 4.f);
        }
        menu->setPosition(base);
        // Stacked vertically: AUTO, SET, BOT
        autoBtn->setPosition({18.f, -18.f});
        setBtn->setPosition({18.f, -56.f});
        botBtn->setPosition({18.f, -94.f});
        menu->addChild(autoBtn);
        menu->addChild(setBtn);
        menu->addChild(botBtn);
        this->addChild(menu, 10);
        return true;
    }

    void onPathBot(CCObject* sender) {
        // The toggler flips its state after this callback, so the new state is the opposite.
        bool on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        g_botEnabled = on;
        if (!on) {
            notify("Bot OFF - your own taps are recorded.", NotificationIcon::Info);
        } else if (g_inputs.empty()) {
            notify("Bot ON - play once yourself first so it has clicks to copy.", NotificationIcon::Warning);
        } else {
            notify(fmt::format("Bot ON - next playtest replays {} clicks.", g_inputs.size()), NotificationIcon::Success);
        }
    }

    void onPathSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get(), false);
    }

    void onPathBuild(CCObject*) {
        if (m_editorLayer->m_playbackMode != PlaybackMode::Not) {
            notify("Stop the playtest first.", NotificationIcon::Warning);
            return;
        }
        if (g_samples.size() < 2) {
            notify("No path yet - start a playtest and play through first.", NotificationIcon::Warning);
            return;
        }

        // Grid cells already filled with solids/hazards, so we don't stack on them.
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

        // Read the spike's real hazard hitbox from the game instead of guessing it.
        if (auto probe = GameObject::createWithKey(settings.spikeID)) {
            probe->setPosition({0.f, 0.f});
            auto r = probe->getObjectRect();
            // A fresh object can report its full 30x30 sprite instead of the small hazard
            // hitbox; only trust the probe if it is clearly the shrunk hazard box.
            if (r.size.width > 0.5f && r.size.width < 25.f && r.size.height > 0.5f && r.size.height < 25.f) {
                settings.spikeHalfW = r.size.width * 0.5f;
                settings.spikeHalfH = r.size.height * 0.5f;
                settings.spikeOffX = r.getMidX();
                settings.spikeOffY = r.getMidY();
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

        // One undo step for the whole batch.
        m_editorLayer->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);

        if (Mod::get()->getSettingValue<bool>("select-generated")) {
            this->deselectAll();
            this->selectObjects(created, true);
        }
        this->updateButtons();
        notify(fmt::format("Placed {} blocks and {} spikes.", blocks, spikes), NotificationIcon::Success);
    }
};
