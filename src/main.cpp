#ifndef CI_STUB_TEST
#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#else
#include "stub.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Click Indicator
//
// Draws one image while you're idle and another while you click / hold. It is
// drawn by the game itself, so it works in fullscreen and in OBS game capture.
// It follows the game's own button input, so it also works for macros / replay
// bots: whatever presses the player's button makes the image change.
//
// Everything is configured from Geode's normal mod settings page.
// ---------------------------------------------------------------------------

#ifdef CI_STUB_TEST
extern double g_fakeNow;
#endif

namespace {
    double nowSec() {
#ifdef CI_STUB_TEST
        return g_fakeNow;
#else
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
#endif
    }

    // ----- settings helpers --------------------------------------------------
    template <class T>
    T getS(char const* key) {
        return Mod::get()->getSettingValue<T>(key);
    }
    float getF(char const* key) { return static_cast<float>(getS<double>(key)); }
    float getPct(char const* key) { return static_cast<float>(getS<int64_t>(key)) / 100.f; }
    double getMs(char const* key) { return static_cast<double>(getS<int64_t>(key)) / 1000.0; }

    std::string modSprite(std::string const& name) {
        return Mod::get()->getID() + "/" + name;
    }

    std::string skinStem() {
        auto s = getS<std::string>("skin");
        if (s == "Spacebar") return "spacebar";
        if (s == "Gamepad") return "gamepad";
        if (s == "Dot") return "dot";
        if (s == "Arrow key") return "arrow";
        return "mouse";
    }

    struct Cfg {
        bool hideIdle = false;
        bool trim = true;
        bool flipH = false;
        bool flipV = false;
        bool backdrop = false;
        bool ripple = false;
        bool countMoves = false;
        bool separateP2 = false;
        std::string position = "Bottom Right";
        std::string p2Position = "Bottom Left";
        std::string track = "Both players";
        float size = 20.f;
        float marginX = 1.5f;
        float marginY = 1.5f;
        float rotation = 0.f;
        float pressedRotation = 0.f;
        float pressedScale = 1.f;
        float idleOpacity = 1.f;
        float pressedOpacity = 1.f;
        float backdropOpacity = 0.5f;
        float backdropPadding = 1.f;
        float pop = 0.f;
        float rippleSize = 150.f;
        float rippleOpacity = 0.6f;
        ccColor3B idleTint = {255, 255, 255};
        ccColor3B pressedTint = {255, 255, 255};
        ccColor3B backdropColor = {0, 0, 0};
        ccColor3B rippleColor = {255, 255, 255};
        double pressDelay = 0.0;
        double releaseDelay = 0.0;
        double minClick = 0.0;
        double fade = 0.0;
        double holdThreshold = 0.25;
        double idleTimeout = 0.0;
        double rippleTime = 0.35;
    };

    Cfg loadCfg() {
        Cfg c;
        c.hideIdle = getS<bool>("hide-idle");
        c.trim = getS<bool>("trim");
        c.flipH = getS<bool>("flip");
        c.flipV = getS<bool>("flip-vertical");
        c.backdrop = getS<bool>("backdrop");
        c.ripple = getS<bool>("ripple");
        c.countMoves = getS<bool>("platformer-buttons");
        c.separateP2 = getS<bool>("separate-p2");
        c.position = getS<std::string>("position");
        c.p2Position = getS<std::string>("p2-position");
        c.track = getS<std::string>("track");
        c.size = getF("size");
        c.marginX = getF("margin-x");
        c.marginY = getF("margin-y");
        c.rotation = getF("rotation");
        c.pressedRotation = getF("pressed-rotation");
        c.pressedScale = getF("pressed-scale");
        c.idleOpacity = getPct("idle-opacity");
        c.pressedOpacity = getPct("pressed-opacity");
        c.backdropOpacity = getPct("backdrop-opacity");
        c.backdropPadding = getF("backdrop-padding");
        c.pop = getF("pop") / 100.f;
        c.rippleSize = getF("ripple-size") / 100.f;
        c.rippleOpacity = getPct("ripple-opacity");
        c.idleTint = getS<ccColor3B>("idle-tint");
        c.pressedTint = getS<ccColor3B>("pressed-tint");
        c.backdropColor = getS<ccColor3B>("backdrop-color");
        c.rippleColor = getS<ccColor3B>("ripple-color");
        c.pressDelay = getMs("press-delay");
        c.releaseDelay = getMs("release-delay");
        c.minClick = getMs("min-click-time");
        c.fade = getMs("fade-time");
        c.holdThreshold = getMs("hold-threshold");
        c.idleTimeout = getS<double>("idle-timeout");
        c.rippleTime = getMs("ripple-time");
        return c;
    }

    // ----- image loading -----------------------------------------------------

    // How much transparent padding each side of an image has, as a fraction (0..1)
    // of the image's width / height.
    struct Trim {
        float l = 0.f, r = 0.f, t = 0.f, b = 0.f;
    };

    struct Part {
        CCSprite* spr = nullptr;
        Trim trim;
        CCPoint center = {0.f, 0.f}; // screen position of the visible part's centre
        CCSize visSize = {0.f, 0.f}; // visible size in points (at base scale)
        float baseScale = 1.f;
    };

    std::string lowerExtension(std::filesystem::path const& p) {
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return ext;
    }

    // Finds the transparent border around a PNG so the *visible* part of the image
    // can sit exactly where you want it (this is what stops the image "floating").
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

        if (maxX < 0 || maxY < 0) return out;

        out.l = static_cast<float>(minX) / w;
        out.r = static_cast<float>(w - 1 - maxX) / w;
        out.t = static_cast<float>(minY) / h;
        out.b = static_cast<float>(h - 1 - maxY) / h;
        return out;
    }

    // Tries each setting key in order for a user-picked image; falls back to the
    // built-in skin image. Pass an empty fallback for "optional" images.
    Part loadPart(std::initializer_list<char const*> keys, std::string const& fallback, bool trim) {
        Part out;
        for (auto key : keys) {
            auto path = getS<std::filesystem::path>(key);
            if (path.empty()) continue;

            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                log::warn("Click Indicator: the image for '{}' doesn't exist anymore.", key);
                continue;
            }

            auto pathStr = geode::utils::string::pathToString(path);
            // Make sure an edited file is re-read from disk.
            CCTextureCache::get()->removeTextureForKey(pathStr.c_str());
            auto spr = CCSprite::create(pathStr.c_str());
            if (!spr) {
                log::warn("Click Indicator: couldn't load the image for '{}'.", key);
                continue;
            }
            out.spr = spr;
            if (trim && lowerExtension(path) == ".png") out.trim = computeTrim(pathStr);
            return out;
        }
        if (!fallback.empty()) out.spr = CCSprite::create(fallback.c_str());
        return out;
    }

    struct Ripple {
        CCSprite* spr = nullptr;
        double born = 0.0;
        float base = 1.f;
    };

    struct PendingEvent {
        double at;
        bool down;
    };

    // One indicator on screen (Player 1's, or Player 2's when they're separate).
    class IndicatorNode : public CCNode {
    public:
        static IndicatorNode* create(bool isP2, bool platformer) {
            auto ret = new IndicatorNode();
            if (ret->setup(isP2, platformer)) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }

        // A button of the player changed. button: 1 = jump, 2 = left, 3 = right.
        void onButton(int button, bool down, bool isPlayer1) {
            if (button < 1 || button > 3) return;
            // Left/right only ever count in platformer levels, and only if enabled.
            if (button != 1 && !m_countMoves) return;

            auto bit = static_cast<uint8_t>(1u << (button - 1));
            uint8_t& mask = isPlayer1 ? m_mask1 : m_mask2;
            if (down) mask = static_cast<uint8_t>(mask | bit);
            else mask = static_cast<uint8_t>(mask & ~bit);

            bool raw = this->computeRaw();
            if (raw == m_rawDown) return;
            m_rawDown = raw;

            double now = nowSec();
            m_lastActive = now;
            double at = now + (raw ? m_cfg.pressDelay : m_cfg.releaseDelay);
            at = std::max(at, m_lastEventAt);
            m_lastEventAt = at;
            m_events.push_back({at, raw});
        }

        // Forget any held buttons (death, restart, pause).
        void resetInput() {
            m_mask1 = 0;
            m_mask2 = 0;
            m_rawDown = false;
            m_events.clear();
            m_dispDown = false;
            this->snapAlphas(nowSec());
        }

        void update(float dt) override {
            auto win = CCDirector::get()->getWinSize();
            if (win.width != m_win.width || win.height != m_win.height) {
                m_win = win;
                this->relayoutAll();
            }
            if (!m_idle.spr || !m_pressed.spr) return;

            double now = nowSec();
            this->processEvents(now);
            this->animate(dt, now);
        }

    private:
        bool m_isP2 = false;
        bool m_platformer = false;
        bool m_countMoves = false;
        Cfg m_cfg;
        CCSize m_win = {0.f, 0.f};

        Part m_idle, m_pressed, m_hold;
        CCLayerColor* m_backdrop = nullptr;
        std::vector<Ripple> m_rips;

        uint8_t m_mask1 = 0;
        uint8_t m_mask2 = 0;
        bool m_rawDown = false;
        bool m_dispDown = false;
        double m_dispSince = 0.0;
        double m_lastEventAt = 0.0;
        double m_lastActive = 0.0;
        std::deque<PendingEvent> m_events;

        float m_idleA = 0.f, m_pressedA = 0.f, m_holdA = 0.f;
        float m_pop = 0.f;

        bool setup(bool isP2, bool platformer) {
            if (!CCNode::init()) return false;
            m_isP2 = isP2;
            m_platformer = platformer;
            m_lastActive = nowSec();
            this->reload();
            this->scheduleUpdate();
            return true;
        }

        bool computeRaw() const {
            auto isDown = [this](uint8_t m) {
                return m_countMoves ? (m != 0) : ((m & 1) != 0);
            };
            bool d1 = isDown(m_mask1);
            bool d2 = isDown(m_mask2);
            if (m_isP2) return d2;
            if (m_cfg.separateP2) return d1;
            if (m_cfg.track == "Player 1 only") return d1;
            if (m_cfg.track == "Player 2 only") return d2;
            return d1 || d2;
        }

        void reload() {
            this->removeAllChildrenWithCleanup(true);
            m_idle = Part();
            m_pressed = Part();
            m_hold = Part();
            m_backdrop = nullptr;
            m_rips.clear();

            m_cfg = loadCfg();
            m_countMoves = m_platformer && m_cfg.countMoves;
            m_win = CCDirector::get()->getWinSize();

            auto stem = skinStem();
            auto idleFallback = modSprite("skin-" + stem + "-idle.png");
            auto pressedFallback = modSprite("skin-" + stem + "-pressed.png");

            if (m_isP2) {
                m_idle = loadPart({"p2-idle-image", "idle-image"}, idleFallback, m_cfg.trim);
                m_pressed = loadPart({"p2-pressed-image", "pressed-image"}, pressedFallback, m_cfg.trim);
            } else {
                m_idle = loadPart({"idle-image"}, idleFallback, m_cfg.trim);
                m_pressed = loadPart({"pressed-image"}, pressedFallback, m_cfg.trim);
            }
            m_hold = loadPart({"hold-image"}, "", m_cfg.trim);

            if (!m_idle.spr || !m_pressed.spr) {
                log::error("Click Indicator: couldn't load the indicator images.");
                m_idle = Part();
                m_pressed = Part();
                m_hold = Part();
                return;
            }

            m_idle.spr->setColor(m_cfg.idleTint);
            m_pressed.spr->setColor(m_cfg.pressedTint);
            this->addChild(m_idle.spr, 2);
            this->addChild(m_pressed.spr, 3);
            if (m_hold.spr) {
                m_hold.spr->setColor(m_cfg.pressedTint);
                this->addChild(m_hold.spr, 4);
            }

            this->relayoutAll();
            this->snapAlphas(nowSec());
        }

        // Sizes a part so its VISIBLE area has the wanted height, and puts that
        // visible area where the "Position" setting says.
        void layoutPart(Part& p, float scaleMult) {
            if (!p.spr) return;
            auto t = p.trim;
            if (m_cfg.flipH) std::swap(t.l, t.r);
            if (m_cfg.flipV) std::swap(t.t, t.b);
            p.spr->setFlipX(m_cfg.flipH);
            p.spr->setFlipY(m_cfg.flipV);

            auto cs = p.spr->getContentSize();
            float visW = cs.width * (1.f - t.l - t.r);
            float visH = cs.height * (1.f - t.t - t.b);
            if (visW < 1.f || visH < 1.f) {
                t = Trim();
                visW = cs.width;
                visH = cs.height;
            }
            if (visW < 1.f || visH < 1.f) return;

            float targetH = m_win.height * m_cfg.size / 100.f;
            float s = targetH / visH * scaleMult;
            p.baseScale = s;
            p.visSize = CCSize(visW * s, visH * s);

            // Pivot = centre of the visible part, so rotation / pop look right.
            p.spr->setAnchorPoint(CCPoint(t.l + (1.f - t.l - t.r) / 2.f,
                                          t.b + (1.f - t.t - t.b) / 2.f));

            auto const& pos = (m_isP2 && m_cfg.separateP2) ? m_cfg.p2Position : m_cfg.position;
            bool right = pos.find("Right") != std::string::npos;
            bool left = pos.find("Left") != std::string::npos;
            bool top = pos.find("Top") != std::string::npos;
            bool bottom = pos.find("Bottom") != std::string::npos;

            float mx = m_win.height * m_cfg.marginX / 100.f;
            float my = m_win.height * m_cfg.marginY / 100.f;
            float halfW = p.visSize.width / 2.f;
            float halfH = p.visSize.height / 2.f;

            float cx = m_win.width / 2.f + mx; // centred axis: margin nudges right
            if (right) cx = m_win.width - mx - halfW;
            else if (left) cx = mx + halfW;

            float cy = m_win.height / 2.f + my; // centred axis: margin nudges up
            if (top) cy = m_win.height - my - halfH;
            else if (bottom) cy = my + halfH;

            p.center = CCPoint(cx, cy);
            p.spr->setPosition(p.center);
            p.spr->setScale(s);
        }

        void relayoutAll() {
            if (!m_idle.spr || !m_pressed.spr) return;

            this->layoutPart(m_idle, 1.f);
            this->layoutPart(m_pressed, m_cfg.pressedScale);
            this->layoutPart(m_hold, m_cfg.pressedScale);

            m_idle.spr->setRotation(m_cfg.rotation);
            m_pressed.spr->setRotation(m_cfg.rotation + m_cfg.pressedRotation);
            if (m_hold.spr) m_hold.spr->setRotation(m_cfg.rotation + m_cfg.pressedRotation);

            if (m_backdrop) {
                m_backdrop->removeFromParentAndCleanup(true);
                m_backdrop = nullptr;
            }
            if (m_cfg.backdrop) {
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (Part* p : {&m_idle, &m_pressed, &m_hold}) {
                    if (!p->spr) continue;
                    minX = std::min(minX, p->center.x - p->visSize.width / 2.f);
                    maxX = std::max(maxX, p->center.x + p->visSize.width / 2.f);
                    minY = std::min(minY, p->center.y - p->visSize.height / 2.f);
                    maxY = std::max(maxY, p->center.y + p->visSize.height / 2.f);
                }
                float pad = m_win.height * m_cfg.backdropPadding / 100.f;
                auto col = m_cfg.backdropColor;
                auto alpha = static_cast<GLubyte>(std::clamp(m_cfg.backdropOpacity, 0.f, 1.f) * 255.f);
                m_backdrop = CCLayerColor::create(ccc4(col.r, col.g, col.b, alpha),
                                                  (maxX - minX) + pad * 2.f,
                                                  (maxY - minY) + pad * 2.f);
                if (m_backdrop) {
                    m_backdrop->setPosition(CCPoint(minX - pad, minY - pad));
                    this->addChild(m_backdrop, 0);
                }
            }
        }

        void processEvents(double now) {
            while (!m_events.empty() && m_events.front().at <= now) {
                auto ev = m_events.front();
                // Keep the clicking image up for at least "minimum click time".
                if (!ev.down && m_dispDown && now < m_dispSince + m_cfg.minClick) break;
                m_events.pop_front();
                if (ev.down == m_dispDown) continue;
                this->applyDisplay(ev.down, now);
            }
        }

        void applyDisplay(bool down, double now) {
            m_dispDown = down;
            if (down) {
                m_dispSince = now;
                m_pop = 1.f;
                this->spawnRipple(now);
            }
        }

        void spawnRipple(double now) {
            if (!m_cfg.ripple) return;
            auto spr = CCSprite::create(modSprite("ripple.png").c_str());
            if (!spr) return;
            auto cs = spr->getContentSize();
            if (cs.width < 1.f) return;

            float diameter = m_pressed.visSize.height * m_cfg.rippleSize;
            float base = diameter / cs.width;
            spr->setPosition(m_pressed.center);
            spr->setColor(m_cfg.rippleColor);
            spr->setScale(base * 0.4f);
            spr->setOpacity(0);
            this->addChild(spr, 1);

            Ripple r;
            r.spr = spr;
            r.born = now;
            r.base = base;
            m_rips.push_back(r);
        }

        static void applyAlpha(Part& p, float a) {
            if (!p.spr) return;
            a = std::clamp(a, 0.f, 1.f);
            p.spr->setOpacity(static_cast<GLubyte>(a * 255.f));
            p.spr->setVisible(a > 0.002f);
        }

        void targets(double now, float& idleT, float& pressedT, float& holdT) const {
            bool holdActive = m_hold.spr && m_dispDown && (now - m_dispSince) >= m_cfg.holdThreshold;
            bool timedOut = m_cfg.idleTimeout > 0.0 && !m_dispDown &&
                            (now - m_lastActive) > m_cfg.idleTimeout;

            idleT = (!m_dispDown && !m_cfg.hideIdle && !timedOut) ? m_cfg.idleOpacity : 0.f;
            pressedT = (m_dispDown && !holdActive) ? m_cfg.pressedOpacity : 0.f;
            holdT = holdActive ? m_cfg.pressedOpacity : 0.f;
        }

        void snapAlphas(double now) {
            float i, p, h;
            this->targets(now, i, p, h);
            m_idleA = i;
            m_pressedA = p;
            m_holdA = h;
            applyAlpha(m_idle, m_idleA);
            applyAlpha(m_pressed, m_pressedA);
            applyAlpha(m_hold, m_holdA);
        }

        void animate(float dt, double now) {
            float idleT, pressedT, holdT;
            this->targets(now, idleT, pressedT, holdT);

            auto step = [&](float& cur, float target) {
                if (m_cfg.fade <= 0.0) {
                    cur = target;
                    return;
                }
                auto maxStep = static_cast<float>(dt / m_cfg.fade);
                if (cur < target) cur = std::min(target, cur + maxStep);
                else cur = std::max(target, cur - maxStep);
            };
            step(m_idleA, idleT);
            step(m_pressedA, pressedT);
            step(m_holdA, holdT);
            applyAlpha(m_idle, m_idleA);
            applyAlpha(m_pressed, m_pressedA);
            applyAlpha(m_hold, m_holdA);

            // "Pop": the clicking image briefly swells, then settles.
            m_pop = std::max(0.f, m_pop - dt * 8.f);
            float k = 1.f + m_cfg.pop * m_pop;
            m_pressed.spr->setScale(m_pressed.baseScale * k);
            if (m_hold.spr) m_hold.spr->setScale(m_hold.baseScale * k);

            // Ripples expand and fade out.
            for (size_t i = 0; i < m_rips.size();) {
                auto& r = m_rips[i];
                double t = m_cfg.rippleTime > 0.0 ? (now - r.born) / m_cfg.rippleTime : 1.0;
                if (t >= 1.0) {
                    r.spr->removeFromParentAndCleanup(true);
                    m_rips.erase(m_rips.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                auto tf = static_cast<float>(t);
                float ease = 1.f - (1.f - tf) * (1.f - tf);
                r.spr->setScale(r.base * (0.4f + 0.6f * ease));
                float fadeOut = (1.f - tf) * (1.f - tf);
                r.spr->setOpacity(static_cast<GLubyte>(
                    std::clamp(m_cfg.rippleOpacity * fadeOut, 0.f, 1.f) * 255.f));
                ++i;
            }
        }
    };
}

#ifndef CI_STUB_TEST

class $modify(CIPlayLayer, PlayLayer) {
    struct Fields {
        IndicatorNode* ind1 = nullptr;
        IndicatorNode* ind2 = nullptr;
        bool platformer = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_fields.self()->platformer = level && level->isPlatformer();
        this->rebuildIndicator();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        this->resetIndicatorInput();
    }

    // If you pause while holding, the release never arrives: clear it.
    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);
        this->resetIndicatorInput();
    }

    void resetIndicatorInput() {
        auto f = m_fields.self();
        if (f->ind1) f->ind1->resetInput();
        if (f->ind2) f->ind2->resetInput();
    }

    void rebuildIndicator() {
        auto f = m_fields.self();
        if (f->ind1) {
            f->ind1->removeFromParentAndCleanup(true);
            f->ind1 = nullptr;
        }
        if (f->ind2) {
            f->ind2->removeFromParentAndCleanup(true);
            f->ind2 = nullptr;
        }

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        auto showIn = mod->getSettingValue<std::string>("show-in");
        if (showIn == "Classic levels only" && f->platformer) return;
        if (showIn == "Platformer levels only" && !f->platformer) return;

        f->ind1 = IndicatorNode::create(false, f->platformer);
        if (f->ind1) this->addChild(f->ind1, 1000);

        if (mod->getSettingValue<bool>("separate-p2")) {
            f->ind2 = IndicatorNode::create(true, f->platformer);
            if (f->ind2) this->addChild(f->ind2, 1000);
        }
    }

    void setButtonState(int button, bool down, bool isPlayer1) {
        auto f = m_fields.self();
        if (f->ind1) f->ind1->onButton(button, down, isPlayer1);
        if (f->ind2) f->ind2->onButton(button, down, isPlayer1);
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
    listenForSettingChanges<bool>("enabled", [](bool) { rebuildLive(); });
    listenForSettingChanges<std::string>("show-in", [](std::string) { rebuildLive(); });
    listenForSettingChanges<bool>("hide-idle", [](bool) { rebuildLive(); });
    listenForSettingChanges<std::string>("skin", [](std::string) { rebuildLive(); });
    listenForSettingChanges<std::filesystem::path>("idle-image", [](std::filesystem::path) { rebuildLive(); });
    listenForSettingChanges<std::filesystem::path>("pressed-image", [](std::filesystem::path) { rebuildLive(); });
    listenForSettingChanges<std::filesystem::path>("hold-image", [](std::filesystem::path) { rebuildLive(); });
    listenForSettingChanges<bool>("trim", [](bool) { rebuildLive(); });
    listenForSettingChanges<std::string>("position", [](std::string) { rebuildLive(); });
    listenForSettingChanges<double>("size", [](double) { rebuildLive(); });
    listenForSettingChanges<double>("margin-x", [](double) { rebuildLive(); });
    listenForSettingChanges<double>("margin-y", [](double) { rebuildLive(); });
    listenForSettingChanges<bool>("flip", [](bool) { rebuildLive(); });
    listenForSettingChanges<bool>("flip-vertical", [](bool) { rebuildLive(); });
    listenForSettingChanges<double>("rotation", [](double) { rebuildLive(); });
    listenForSettingChanges<double>("pressed-rotation", [](double) { rebuildLive(); });
    listenForSettingChanges<double>("pressed-scale", [](double) { rebuildLive(); });
    listenForSettingChanges<int64_t>("idle-opacity", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<int64_t>("pressed-opacity", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<ccColor3B>("idle-tint", [](ccColor3B) { rebuildLive(); });
    listenForSettingChanges<ccColor3B>("pressed-tint", [](ccColor3B) { rebuildLive(); });
    listenForSettingChanges<bool>("backdrop", [](bool) { rebuildLive(); });
    listenForSettingChanges<ccColor3B>("backdrop-color", [](ccColor3B) { rebuildLive(); });
    listenForSettingChanges<int64_t>("backdrop-opacity", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<double>("backdrop-padding", [](double) { rebuildLive(); });
    listenForSettingChanges<int64_t>("min-click-time", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<int64_t>("press-delay", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<int64_t>("release-delay", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<int64_t>("fade-time", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<int64_t>("hold-threshold", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<double>("idle-timeout", [](double) { rebuildLive(); });
    listenForSettingChanges<double>("pop", [](double) { rebuildLive(); });
    listenForSettingChanges<bool>("ripple", [](bool) { rebuildLive(); });
    listenForSettingChanges<ccColor3B>("ripple-color", [](ccColor3B) { rebuildLive(); });
    listenForSettingChanges<double>("ripple-size", [](double) { rebuildLive(); });
    listenForSettingChanges<int64_t>("ripple-time", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<int64_t>("ripple-opacity", [](int64_t) { rebuildLive(); });
    listenForSettingChanges<std::string>("track", [](std::string) { rebuildLive(); });
    listenForSettingChanges<bool>("platformer-buttons", [](bool) { rebuildLive(); });
    listenForSettingChanges<bool>("separate-p2", [](bool) { rebuildLive(); });
    listenForSettingChanges<std::string>("p2-position", [](std::string) { rebuildLive(); });
    listenForSettingChanges<std::filesystem::path>("p2-idle-image", [](std::filesystem::path) { rebuildLive(); });
    listenForSettingChanges<std::filesystem::path>("p2-pressed-image", [](std::filesystem::path) { rebuildLive(); });
}

#endif
