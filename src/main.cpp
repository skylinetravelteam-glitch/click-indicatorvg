#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Click Indicator
//
// Draws one image while you're idle and another while you click / hold, in a
// corner of the screen. It is drawn by the game itself, so it works in
// fullscreen and in OBS game capture. It follows the game's own input, so it
// also works for macros / replay bots (XDBot, Eclipse, etc.): whatever presses
// the player's button makes the image change.
//
// Everything is configured from Geode's normal mod settings page.
// ---------------------------------------------------------------------------

namespace {
    // How much transparent padding each side of an image has, as a fraction (0..1)
    // of the image's width / height.
    struct Trim {
        float l = 0.f, r = 0.f, t = 0.f, b = 0.f;
    };

    struct Indicator {
        CCSprite* spr = nullptr;
        Trim trim;
    };

    std::string lowerExtension(std::filesystem::path const& p) {
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return ext;
    }

    // Finds the transparent border around a PNG so the *visible* part of the image
    // can sit exactly in the corner (this is what stops the image "floating").
    Trim computeTrim(std::string const& pathStr) {
        Trim out;
        auto img = new CCImage();
        if (!img->initWithImageFile(pathStr.c_str(), CCImage::kFmtPng)) {
            img->release();
            return out;
        }
        if (!img->hasAlpha() || img->getBitsPerComponent() != 8) {
            img->release();
            return out;
        }

        int w = img->getWidth();
        int h = img->getHeight();
        auto data = img->getData();
        if (!data || w <= 0 || h <= 0) {
            img->release();
            return out;
        }

        int minX = w, minY = h, maxX = -1, maxY = -1;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (data[(static_cast<size_t>(y) * w + x) * 4 + 3] > 8) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        img->release();

        if (maxX < 0 || maxY < 0) return out; // fully transparent, nothing to trim

        out.l = static_cast<float>(minX) / w;
        out.r = static_cast<float>(w - 1 - maxX) / w;
        out.t = static_cast<float>(minY) / h;
        out.b = static_cast<float>(h - 1 - maxY) / h;
        return out;
    }

    // Loads the image the player picked in settings, or the bundled default.
    Indicator loadIndicator(char const* settingKey, std::string const& fallback, bool trim) {
        Indicator out;

        auto path = Mod::get()->getSettingValue<std::filesystem::path>(settingKey);
        if (!path.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                auto pathStr = geode::utils::string::pathToString(path);
                // Make sure an edited file is re-read from disk.
                CCTextureCache::get()->removeTextureForKey(pathStr.c_str());
                out.spr = CCSprite::create(pathStr.c_str());
                if (out.spr && trim && lowerExtension(path) == ".png") {
                    out.trim = computeTrim(pathStr);
                }
            }
            if (!out.spr) {
                log::warn("Click Indicator: couldn't load '{}', using the default image instead.",
                          geode::utils::string::pathToString(path));
            }
        }

        if (!out.spr) {
            out.trim = Trim();
            out.spr = CCSprite::create(fallback.c_str());
        }
        return out;
    }

    // Sizes a sprite so its VISIBLE part is `targetHeight` tall and pins that visible
    // part to the chosen corner of the screen.
    void placeSprite(CCSprite* spr, Trim t, float targetHeight, float scaleMult,
                     bool right, bool top, float marginX, float marginY, bool flip,
                     CCSize const& win) {
        if (flip) std::swap(t.l, t.r);
        spr->setFlipX(flip);

        auto cs = spr->getContentSize();
        float visW = cs.width * (1.f - t.l - t.r);
        float visH = cs.height * (1.f - t.t - t.b);
        if (visW < 1.f || visH < 1.f) {
            t = Trim();
            visW = cs.width;
            visH = cs.height;
        }
        if (visH < 1.f) return;

        float s = targetHeight / visH * scaleMult;
        spr->setScale(s);
        spr->setAnchorPoint({0.f, 0.f});

        float visLeft = right ? (win.width - marginX - visW * s) : marginX;
        float visBottom = top ? (win.height - marginY - visH * s) : marginY;

        spr->setPosition({visLeft - t.l * cs.width * s, visBottom - t.b * cs.height * s});
    }
}

class $modify(CIPlayLayer, PlayLayer) {
    struct Fields {
        CCNode* root = nullptr;
        CCSprite* idle = nullptr;
        CCSprite* pressed = nullptr;
        uint8_t p1Mask = 0; // bit 0 = jump, bit 1 = left, bit 2 = right
        uint8_t p2Mask = 0;
        CCSize lastWin = {0.f, 0.f};
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        this->rebuildIndicator();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        // A button can look "held" across a death / restart; start clean.
        auto f = m_fields.self();
        f->p1Mask = 0;
        f->p2Mask = 0;
        this->refreshIndicator();
    }

    void rebuildIndicator() {
        auto f = m_fields.self();

        if (f->root) {
            f->root->removeFromParentAndCleanup(true);
            f->root = nullptr;
            f->idle = nullptr;
            f->pressed = nullptr;
        }

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        bool trim = mod->getSettingValue<bool>("trim");
        auto idle = loadIndicator("idle-image", "default-idle.png"_spr, trim);
        auto pressed = loadIndicator("pressed-image", "default-pressed.png"_spr, trim);
        if (!idle.spr || !pressed.spr) {
            log::error("Click Indicator: couldn't load the indicator images.");
            return;
        }

        auto win = CCDirector::get()->getWinSize();
        auto corner = mod->getSettingValue<std::string>("corner");
        bool right = corner.find("Right") != std::string::npos;
        bool top = corner.find("Top") != std::string::npos;

        float sizePct = static_cast<float>(mod->getSettingValue<double>("size"));
        float marginX = win.height * static_cast<float>(mod->getSettingValue<double>("margin-x")) / 100.f;
        float marginY = win.height * static_cast<float>(mod->getSettingValue<double>("margin-y")) / 100.f;
        float pressedMult = static_cast<float>(mod->getSettingValue<double>("pressed-scale"));
        bool flip = mod->getSettingValue<bool>("flip");
        auto opacity = static_cast<GLubyte>(
            255.f * static_cast<float>(mod->getSettingValue<int64_t>("opacity")) / 100.f);

        float targetHeight = win.height * sizePct / 100.f;

        placeSprite(idle.spr, idle.trim, targetHeight, 1.f, right, top, marginX, marginY, flip, win);
        placeSprite(pressed.spr, pressed.trim, targetHeight, pressedMult, right, top, marginX, marginY, flip, win);
        idle.spr->setOpacity(opacity);
        pressed.spr->setOpacity(opacity);

        auto root = CCNode::create();
        root->addChild(idle.spr);
        root->addChild(pressed.spr);
        this->addChild(root, 1000);

        f->root = root;
        f->idle = idle.spr;
        f->pressed = pressed.spr;
        f->lastWin = win;

        this->refreshIndicator();
    }

    void refreshIndicator() {
        auto f = m_fields.self();
        if (!f->idle || !f->pressed) return;

        auto mod = Mod::get();
        bool anyButton = mod->getSettingValue<bool>("platformer-buttons");
        auto track = mod->getSettingValue<std::string>("track");

        auto isDown = [anyButton](uint8_t mask) {
            return anyButton ? (mask != 0) : ((mask & 1) != 0);
        };
        bool d1 = isDown(f->p1Mask);
        bool d2 = isDown(f->p2Mask);

        bool down;
        if (track == "Player 1 only") down = d1;
        else if (track == "Player 2 only") down = d2;
        else down = d1 || d2;

        bool hideIdle = mod->getSettingValue<bool>("hide-idle");
        f->idle->setVisible(!down && !hideIdle);
        f->pressed->setVisible(down);
    }

    void setButtonState(int button, bool down, bool isPlayer1) {
        if (button < 1 || button > 3) return;
        auto f = m_fields.self();

        // If the window was resized / switched to fullscreen, re-pin to the corner.
        auto win = CCDirector::get()->getWinSize();
        if (f->root && (win.width != f->lastWin.width || win.height != f->lastWin.height)) {
            this->rebuildIndicator();
        }

        uint8_t bit = static_cast<uint8_t>(1u << (button - 1));
        uint8_t& mask = isPlayer1 ? f->p1Mask : f->p2Mask;
        if (down) mask = static_cast<uint8_t>(mask | bit);
        else mask = static_cast<uint8_t>(mask & ~bit);

        this->refreshIndicator();
    }
};

namespace {
    void notifyFromPlayer(PlayerObject* player, int button, bool down) {
        auto pl = PlayLayer::get();
        if (!pl) return;

        bool isP1 = player == pl->m_player1;
        bool isP2 = player == pl->m_player2;
        if (!isP1 && !isP2) return;

        static_cast<CIPlayLayer*>(pl)->setButtonState(button, down, isP1);
    }

    void rebuildLive() {
        if (auto pl = PlayLayer::get()) {
            static_cast<CIPlayLayer*>(pl)->rebuildIndicator();
        }
    }
}

// Catches every press/release of the player's buttons, no matter where it came
// from: your keyboard / mouse / touch, or a macro / replay bot.
class $modify(CIPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool result = PlayerObject::pushButton(button);
        notifyFromPlayer(this, static_cast<int>(button), true);
        return result;
    }

    bool releaseButton(PlayerButton button) {
        bool result = PlayerObject::releaseButton(button);
        notifyFromPlayer(this, static_cast<int>(button), false);
        return result;
    }
};

// Second path: the game's own input handler (bots usually go through this too).
class $modify(CIBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;

        static_cast<CIPlayLayer*>(pl)->setButtonState(button, down, isPlayer1);
    }
};

// Changing a setting from the pause menu updates the indicator instantly.
$on_mod(Loaded) {
    listenForSettingChanges("enabled", [](bool) { rebuildLive(); });
    listenForSettingChanges("idle-image", [](std::filesystem::path) { rebuildLive(); });
    listenForSettingChanges("pressed-image", [](std::filesystem::path) { rebuildLive(); });
    listenForSettingChanges("trim", [](bool) { rebuildLive(); });
    listenForSettingChanges("corner", [](std::string) { rebuildLive(); });
    listenForSettingChanges("size", [](double) { rebuildLive(); });
    listenForSettingChanges("margin-x", [](double) { rebuildLive(); });
    listenForSettingChanges("margin-y", [](double) { rebuildLive(); });
    listenForSettingChanges("opacity", [](int64_t) { rebuildLive(); });
    listenForSettingChanges("pressed-scale", [](double) { rebuildLive(); });
    listenForSettingChanges("flip", [](bool) { rebuildLive(); });
    listenForSettingChanges("hide-idle", [](bool) { rebuildLive(); });
    listenForSettingChanges("track", [](std::string) { rebuildLive(); });
    listenForSettingChanges("platformer-buttons", [](bool) { rebuildLive(); });
}
