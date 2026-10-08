#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

class $modify(CIPlayLayer, PlayLayer) {
    struct Fields {
        CCSprite* idle = nullptr;
        CCSprite* pressed = nullptr;
        bool p1Down = false;
        bool p2Down = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        this->buildIndicator();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        f->p1Down = false;
        f->p2Down = false;
        this->refreshIndicator();
    }

    void buildIndicator() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        auto idle = CCSprite::create("image1.png"_spr);
        auto pressed = CCSprite::create("image2.png"_spr);
        if (!idle || !pressed) {
            log::error("Click Indicator: couldn't load image1.png / image2.png from the mod's resources.");
            return;
        }

        auto winSize = CCDirector::get()->getWinSize();
        float sizePct = static_cast<float>(mod->getSettingValue<double>("size"));
        float marginPct = static_cast<float>(mod->getSettingValue<double>("margin"));
        float opacityPct = static_cast<float>(mod->getSettingValue<int64_t>("opacity"));

        float targetHeight = winSize.height * sizePct / 100.f;
        float margin = winSize.height * marginPct / 100.f;

        for (auto spr : {idle, pressed}) {
            float scale = targetHeight / idle->getContentSize().height;
            spr->setScale(scale);
            spr->setAnchorPoint({1.f, 0.f});
            spr->setPosition({winSize.width - margin, margin});
            spr->setOpacity(static_cast<GLubyte>(255.f * opacityPct / 100.f));
            this->addChild(spr, 1000);
        }

        auto f = m_fields.self();
        f->idle = idle;
        f->pressed = pressed;
        this->refreshIndicator();
    }

    void refreshIndicator() {
        auto f = m_fields.self();
        if (!f->idle || !f->pressed) return;
        bool down = f->p1Down || f->p2Down;
        f->idle->setVisible(!down);
        f->pressed->setVisible(down);
    }

    void setButtonState(bool down, bool isPlayer1) {
        auto f = m_fields.self();
        if (isPlayer1) f->p1Down = down;
        else f->p2Down = down;
        this->refreshIndicator();
    }
};

class $modify(CIBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (button != 1) return;

        if (auto pl = PlayLayer::get()) {
            static_cast<CIPlayLayer*>(pl)->setButtonState(down, isPlayer1);
        }
    }
};
