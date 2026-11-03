// Line Text: type text in the editor and it is drawn with thin line objects
// (one stretched, rotated object per straight stroke of a Hershey font).

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cmath>

#include "Layout.hpp"

using namespace geode::prelude;

namespace {
    void notify(std::string const& text, NotificationIcon icon) {
        Notification::create(text, icon)->show();
    }

    float settingF(char const* key) { return static_cast<float>(Mod::get()->getSettingValue<double>(key)); }

    int fontIndexFromSetting() {
        auto name = Mod::get()->getSettingValue<std::string>("font");
        for (int i = 0; i < lt::fontCount(); i++) {
            std::string n = lt::fontName(i);
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (n == name) return i;
        }
        return 0;
    }

    // Draw the text at the middle of the screen. Returns the number of objects made.
    int placeText(EditorUI* ui, std::string const& text, int font, float capHeight, float thickness) {
        auto lel = ui->m_editorLayer;
        if (!lel || text.empty()) return 0;

        lt::LayoutOptions opt;
        opt.font = font;
        opt.capHeight = capHeight;
        opt.letterSpacing = settingF("letter-spacing");
        opt.lineSpacing = settingF("line-spacing");
        opt.simplify = settingF("simplify");
        auto lines = lt::layoutText(text, opt);
        if (lines.empty()) return 0;

        int objectID = static_cast<int>(Mod::get()->getSettingValue<int64_t>("object-id"));
        float maxScale = std::max(0.5f, settingF("max-scale"));

        // Size of the line object at scale 1, so scales come out in real units.
        float baseW = 30.f, baseH = 30.f;
        if (auto probe = GameObject::createWithKey(objectID)) {
            auto sz = probe->getContentSize();
            if (sz.width > 0.5f && sz.width < 500.f) baseW = sz.width;
            if (sz.height > 0.5f && sz.height < 500.f) baseH = sz.height;
        } else {
            notify(fmt::format("Object ID {} doesn't exist.", objectID), NotificationIcon::Error);
            return 0;
        }

        auto win = CCDirector::get()->getWinSize();
        CCPoint center = lel->m_objectLayer->convertToNodeSpace(CCPoint(win.width * 0.5f, win.height * 0.5f));

        auto created = CCArray::create();
        for (auto const& ln : lines) {
            // A little longer than the stroke so neighbouring strokes join without gaps.
            float total = ln.length + thickness;
            int pieces = std::max(1, static_cast<int>(std::ceil(total / (baseW * maxScale))));
            float piece = total / pieces;
            float rad = ln.angle / 57.2957795f;
            float ux = std::cos(rad), uy = std::sin(rad);
            for (int k = 0; k < pieces; k++) {
                float off = (k + 0.5f) * piece - total * 0.5f;
                CCPoint pos = {center.x + ln.x + ux * off, center.y + ln.y + uy * off};
                auto obj = lel->createObject(objectID, pos, true);
                if (!obj) continue;
                obj->setRotation(-ln.angle);   // cocos rotation is clockwise
                obj->updateCustomScaleX(piece / baseW);
                obj->updateCustomScaleY(std::max(0.01f, thickness / baseH));
                created->addObject(obj);
            }
        }
        if (created->count() == 0) return 0;

        lel->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        if (Mod::get()->getSettingValue<bool>("select-placed")) {
            ui->deselectAll();
            ui->selectObjects(created, true);
        }
        ui->updateButtons();
        return static_cast<int>(created->count());
    }

    // -----------------------------------------------------------------------
    class LineTextPopup : public Popup {
        EditorUI* m_ui = nullptr;
        TextInput* m_text = nullptr;
        TextInput* m_sizeIn = nullptr;
        TextInput* m_thick = nullptr;
        CCLabelBMFont* m_fontLabel = nullptr;
        int m_font = 0;

        static float parse(TextInput* in, float fallback) {
            auto s = std::string(in->getString());
            char* end = nullptr;
            float v = std::strtof(s.c_str(), &end);
            return (end && end != s.c_str() && v > 0.f) ? v : fallback;
        }

        void updateFontLabel() {
            m_fontLabel->setString(lt::fontName(m_font));
        }

        void onFont(CCObject* sender) {
            int dir = sender->getTag();
            m_font = (m_font + dir + lt::fontCount()) % lt::fontCount();
            updateFontLabel();
        }

        void onSettings(CCObject*) { openSettingsPopup(Mod::get()); }

        void onPlace(CCObject*) {
            std::string text = m_text->getString();
            if (text.empty()) { notify("Type some text first.", NotificationIcon::Warning); return; }
            float size = parse(m_sizeIn, settingF("cap-height"));
            float thick = parse(m_thick, settingF("thickness"));
            Mod::get()->setSavedValue<std::string>("last-text", text);
            Mod::get()->setSavedValue<int64_t>("last-font", m_font);
            Mod::get()->setSavedValue<double>("last-size", size);
            Mod::get()->setSavedValue<double>("last-thick", thick);
            auto ui = m_ui;
            this->onClose(nullptr);
            int n = placeText(ui, text, m_font, size, thick);
            if (n > 0) notify(fmt::format("Placed {} line objects.", n), NotificationIcon::Success);
        }

        bool init(EditorUI* ui) {
            if (!Popup::init(320.f, 230.f)) return false;
            m_ui = ui;
            this->setTitle("Line Text");
            auto mod = Mod::get();
            float w = m_size.width, h = m_size.height;

            m_text = TextInput::create(280.f, "Text (use \\n for a new line)");
            m_text->setCommonFilter(CommonFilter::Any);
            m_text->setMaxCharCount(200);
            m_text->setString(mod->getSavedValue<std::string>("last-text", "REVOLVE"));
            m_text->setPosition({w * 0.5f, h - 62.f});
            m_mainLayer->addChild(m_text);

            m_font = static_cast<int>(mod->getSavedValue<int64_t>("last-font", fontIndexFromSetting()));
            if (m_font < 0 || m_font >= lt::fontCount()) m_font = 0;
            m_fontLabel = CCLabelBMFont::create("", "bigFont.fnt");
            m_fontLabel->setScale(0.55f);
            m_fontLabel->setPosition({w * 0.5f, h - 102.f});
            m_mainLayer->addChild(m_fontLabel);
            updateFontLabel();

            auto mkArrow = [&](bool right) {
                auto spr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
                spr->setScale(0.5f);
                if (right) spr->setFlipX(true);
                auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(LineTextPopup::onFont));
                btn->setTag(right ? 1 : -1);
                btn->setPosition({w * 0.5f + (right ? 95.f : -95.f), h - 102.f});
                m_buttonMenu->addChild(btn);
            };
            mkArrow(false);
            mkArrow(true);

            auto mkNumber = [&](char const* caption, float x, double value) {
                auto label = CCLabelBMFont::create(caption, "goldFont.fnt");
                label->setScale(0.5f);
                label->setPosition({x, h - 135.f});
                m_mainLayer->addChild(label);
                auto in = TextInput::create(90.f, "0");
                in->setCommonFilter(CommonFilter::Float);
                in->setString(fmt::format("{:g}", value));
                in->setPosition({x, h - 160.f});
                m_mainLayer->addChild(in);
                return in;
            };
            m_sizeIn = mkNumber("Size (px)", w * 0.3f, mod->getSavedValue<double>("last-size", mod->getSettingValue<double>("cap-height")));
            m_thick = mkNumber("Line (px)", w * 0.7f, mod->getSavedValue<double>("last-thick", mod->getSettingValue<double>("thickness")));

            auto place = CCMenuItemSpriteExtra::create(ButtonSprite::create("Place"), this, menu_selector(LineTextPopup::onPlace));
            place->setPosition({w * 0.5f, 28.f});
            m_buttonMenu->addChild(place);

            auto gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
            gear->setScale(0.5f);
            auto gearBtn = CCMenuItemSpriteExtra::create(gear, this, menu_selector(LineTextPopup::onSettings));
            gearBtn->setPosition({w - 24.f, 24.f});
            m_buttonMenu->addChild(gearBtn);
            return true;
        }

    public:
        static LineTextPopup* create(EditorUI* ui) {
            auto ret = new LineTextPopup();
            if (ret->init(ui)) { ret->autorelease(); return ret; }
            delete ret;
            return nullptr;
        }
    };
}

class $modify(LTEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        auto label = CCLabelBMFont::create("TXT", "bigFont.fnt");
        label->setScale(0.45f);
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Blue, CircleBaseSize::Small);
        spr->setScale(0.65f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(LTEditorUI::onLineText));
        btn->setID("line-text-button"_spr);

        auto menu = CCMenu::create();
        menu->setID("line-text-menu"_spr);
        menu->addChild(btn);
        btn->setPosition({0.f, 0.f});

        // Right of the zoom buttons on the left; falls back to the left edge.
        auto win = CCDirector::get()->getWinSize();
        CCPoint pos = {28.f, win.height * 0.5f};
        if (auto zoom = this->getChildByID("zoom-menu")) {
            auto bb = zoom->boundingBox();
            pos = CCPoint(bb.getMaxX() + 18.f, bb.getMidY());
        }
        menu->setPosition(pos);
        this->addChild(menu, 10);
        return true;
    }

    void onLineText(CCObject*) {
        if (m_editorLayer->m_playbackMode != PlaybackMode::Not) {
            notify("Stop the playtest first.", NotificationIcon::Warning);
            return;
        }
        if (auto p = LineTextPopup::create(this)) p->show();
    }
};
