#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
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
        st.spikeClearance = static_cast<float>(mod->getSettingValue<double>("spike-clearance"));
        st.spikeSpacing = static_cast<float>(mod->getSettingValue<double>("spike-spacing"));
        st.shipMargin = static_cast<float>(mod->getSettingValue<double>("ship-margin"));
        st.waveMargin = static_cast<float>(mod->getSettingValue<double>("wave-margin"));
        st.blockID = static_cast<int>(mod->getSettingValue<int64_t>("block-id"));
        st.spikeID = static_cast<int>(mod->getSettingValue<int64_t>("spike-id"));
        return st;
    }

    void notify(std::string const& text, NotificationIcon icon) {
        Notification::create(text, icon)->show();
    }
}

// ---------------------------------------------------------------------------
// Jump button tracking
// ---------------------------------------------------------------------------
class $modify(PBPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool ret = PlayerObject::pushButton(button);
        if (button == PlayerButton::Jump && isEditorPlayer1(this)) {
            g_holding = true;
            g_pressedThisFrame = true;
        }
        return ret;
    }

    bool releaseButton(PlayerButton button) {
        bool ret = PlayerObject::releaseButton(button);
        if (button == PlayerButton::Jump && isEditorPlayer1(this)) {
            g_holding = false;
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
            g_samples.reserve(4096);
            g_recording = true;
            g_stoppedByDeath = false;
            g_holding = false;
            g_pressedThisFrame = false;
        }
    }

    void onStopPlaytest() {
        LevelEditorLayer::onStopPlaytest();
        if (g_recording) {
            g_recording = false;
            if (g_samples.size() >= 2) {
                notify(
                    fmt::format("Path recorded ({} frames). Tap AUTO to build.", g_samples.size()),
                    NotificationIcon::Success
                );
            }
        }
    }

    void postUpdate(float dt) {
        LevelEditorLayer::postUpdate(dt);
        if (!g_recording || g_stoppedByDeath) return;
        if (m_playbackMode != PlaybackMode::Playing) return;

        auto p = m_player1;
        if (!p) return;
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
// Editor buttons: AUTO (generate) and SET (settings)
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

        // Place the buttons just below the playtest buttons (left side of the editor).
        auto winSize = CCDirector::get()->getWinSize();
        CCPoint base = {12.f, winSize.height * 0.55f};
        if (auto playtestMenu = this->getChildByID("playtest-menu")) {
            base = CCPoint(playtestMenu->getPositionX(), playtestMenu->boundingBox().getMinY() - 4.f);
        }
        menu->setPosition(base);
        autoBtn->setPosition({18.f, -18.f});
        setBtn->setPosition({56.f, -18.f});
        menu->addChild(autoBtn);
        menu->addChild(setBtn);
        this->addChild(menu, 10);
        return true;
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

        auto placements = pb::generate(g_samples, readSettings(), existing);
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
