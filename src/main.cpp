// Predictive Input Buffer: frame-adjustable, input-buffered hazard assist for Cube and Wave.
//
// One idea drives both modes: your input is treated as *intent*, and the assist
// may adjust its timing within a window of N frames (60Hz-equivalent).
//   Cube: a click up to N frames before landing is remembered, and a landing
//         jump may be delayed up to N frames so it clears the hazard ahead.
//   Wave: the assist plans N frames ahead and overrides hold/release only when
//         your input would hit a hazard and a survivable alternative exists.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

// Cube physics in units and 60Hz frames. Tune here if results are off.
static constexpr float kJumpVel   = 11.180032f;
static constexpr float kGravity   = 0.958199f;
static constexpr float kBaseSpeed = 5.77f; // units/frame per unit of m_playerSpeed

template <class T>
static T cfg(char const* key) { return Mod::get()->getSettingValue<T>(key); }

static bool  anyEnabled()   { return cfg<bool>("enabled") && PlayLayer::get(); }
static bool  consoleOut()   { return cfg<bool>("console-output"); }
static int   cubeWindow()   { return static_cast<int>(cfg<int64_t>("cube-window")); }
static int   waveWindow()   { return static_cast<int>(cfg<int64_t>("wave-window")); }
static float cubeMargin()   { return static_cast<float>(cfg<double>("cube-margin")); }

class $modify(AssistPlayer, PlayerObject) {
    struct Fields {
        bool held = false;           // the player's real hold state

        // ---- cube ----
        bool pending = false;        // click waiting for landing
        float pendingLeft = 0.f;     // frames left in the buffer
        bool landingHandled = false; // processed this landing already
        bool delaying = false;       // holding a jump back
        float waited = 0.f;          // frames spent delaying
        bool tapRelease = false;     // release a synthetic tap next step
        std::vector<Ref<GameObject>> cubeHazards;

        // ---- wave ----
        bool wasWave = false;
        bool assistHeld = false;     // hold state last applied to the game
        std::vector<Ref<GameObject>> waveCache;
        float cacheStart = 0.f;
        float cacheEnd = -1.f;
    };

    bool isCubeLike() {
        return !(m_isShip || m_isBird || m_isDart || m_isSwing);
    }
    bool cubeActive() {
        return anyEnabled() && cfg<bool>("cube-assist") && cubeWindow() > 0 && isCubeLike();
    }
    bool waveActive() {
        return anyEnabled() && cfg<bool>("wave-assist") && waveWindow() > 0 && m_isDart;
    }

    // =====================================================================
    // CUBE
    // =====================================================================

    void scanCubeHazards() {
        auto f = m_fields.self();
        f->cubeHazards.clear();
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_objects) return;

        float s = m_playerSpeed * kBaseSpeed;
        float jumpLen = s * (2.f * kJumpVel / kGravity);
        float x0 = this->getPositionX();
        float x1 = x0 + jumpLen * 3.f;

        for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (obj->m_objectType != GameObjectType::Hazard) continue;
            float ox = obj->getPositionX();
            if (ox < x0 || ox > x1) continue;
            f->cubeHazards.push_back(obj);
        }
    }

    // How many frames must we wait before a jump clears the nearest hazard?
    // Returns 0 if jumping now is fine (or nothing can be done).
    // Returns > 0 when waiting that long centres the jump in the clear window.
    float framesToWait() {
        auto pr = this->getObjectRect();
        float s = m_playerSpeed * kBaseSpeed;
        bool flip = m_isUpsideDown;

        float bottom = flip ? -pr.getMaxY() : pr.getMinY();
        float top    = flip ? -pr.getMinY() : pr.getMaxY();

        CCRect tr;
        bool found = false;
        float bestNear = 1e9f;

        for (auto& o : m_fields->cubeHazards) {
            auto hr = o->getObjectRect();
            float hb = flip ? -hr.getMaxY() : hr.getMinY();
            float ht = flip ? -hr.getMinY() : hr.getMaxY();
            if (ht <= bottom || hb >= top) continue;      // not in our lane
            float dNear = hr.getMinX() - pr.getMaxX();
            float dFar  = hr.getMaxX() - pr.getMinX();
            if (dFar < 0.f || dNear < 0.f) continue;      // passed / overlapping
            if (dNear < bestNear) { bestNear = dNear; tr = hr; found = true; }
        }
        if (!found) return 0.f;

        float ht = flip ? -tr.getMinY() : tr.getMaxY();
        float dNear = tr.getMinX() - pr.getMaxX();
        float dFar  = tr.getMaxX() - pr.getMinX();

        float rise = ht - bottom + cubeMargin();
        float disc = kJumpVel * kJumpVel - 2.f * kGravity * rise;
        if (disc <= 0.f) return 0.f;                       // cube can't clear it

        float root = std::sqrt(disc);
        float t1 = (kJumpVel - root) / kGravity;           // frame we rise above `rise`
        float t2 = (kJumpVel + root) / kGravity;           // frame we drop below it
        float width = (dFar - dNear) / s;                  // frames spent over hazard
        if (width > t2 - t1) return 0.f;                   // hazard too wide

        float tHit = dNear / s;
        if (tHit < t1) return 0.f;                         // already too late

        float slack = ((t2 - t1) - width) * 0.5f;
        float need = dFar / s - (t2 - slack);              // frames until centred
        return need > 0.f ? need : 0.f;
    }

    void cubeFire() {
        auto f = m_fields.self();
        PlayerObject::pushButton(PlayerButton::Jump);
        if (!f->held) f->tapRelease = true;
    }

    void cubeStep(float frames) {
        auto f = m_fields.self();

        if (f->tapRelease) {
            f->tapRelease = false;
            if (!f->held) PlayerObject::releaseButton(PlayerButton::Jump);
        }

        if (!m_isOnGround) {
            f->landingHandled = false;
            f->delaying = false;
            if (f->pending) {
                f->pendingLeft -= frames;
                if (f->pendingLeft < 0.f) f->pending = false;
            }
            return;
        }

        bool intent = f->held || f->pending;
        float window = static_cast<float>(cubeWindow());

        if (f->delaying) {
            f->waited += frames;
            if (!intent) {
                f->delaying = false;
            } else if (f->waited >= window || framesToWait() <= 0.f) {
                f->delaying = false;
                cubeFire();
                if (consoleOut()) log::info("cube jump released after {:.1f} frames", f->waited);
            }
            return;
        }

        if (f->landingHandled) return;
        f->landingHandled = true;
        if (!intent) return;

        scanCubeHazards();
        float need = framesToWait();
        bool wasPending = f->pending;
        f->pending = false;

        if (need > 0.f && need <= window) {
            f->delaying = true;
            f->waited = 0.f;
            // Cancel the game's own hold-to-jump; we fire it ourselves.
            if (f->held) PlayerObject::releaseButton(PlayerButton::Jump);
            if (consoleOut()) log::info("delaying cube jump by ~{:.1f} frames", need);
        } else if (wasPending && !f->held) {
            cubeFire(); // plain buffered click
            if (consoleOut()) log::info("buffered click fired");
        }
        // if held and no delay is needed, the game jumps on its own
    }

    // =====================================================================
    // WAVE
    // =====================================================================

    void refreshWaveCache(float x0, float reach) {
        auto f = m_fields.self();
        if (x0 >= f->cacheStart && x0 + reach <= f->cacheEnd) return;

        f->waveCache.clear();
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_objects) return;

        f->cacheStart = x0 - 150.f;
        f->cacheEnd = x0 + reach * 4.f;
        bool solids = cfg<bool>("wave-solids");

        for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            auto t = obj->m_objectType;
            if (t != GameObjectType::Hazard && !(solids && t == GameObjectType::Solid)) continue;
            float ox = obj->getPositionX();
            if (ox < f->cacheStart - 200.f || ox > f->cacheEnd + 200.f) continue;
            f->waveCache.push_back(obj);
        }
    }

    // The wave's path depends only on how many of the planned frames were spent
    // holding, so a DP over (frame, holds-so-far) answers: which of {hold,
    // release} still has a survivable continuation within the window?
    void waveStep() {
        auto f = m_fields.self();
        bool desired = f->held;

        auto pr = this->getObjectRect();
        float s = m_playerSpeed * kBaseSpeed;                 // horizontal units/frame
        float slope = (m_vehicleSize < 1.f) ? 2.f : 1.f;      // mini wave is steeper
        float v = s * slope;                                  // vertical units/frame
        float dir = (cfg<bool>("wave-flip-gravity") && m_isUpsideDown) ? -1.f : 1.f;

        int H = waveWindow();
        float m = static_cast<float>(cfg<double>("wave-margin"));
        float floorY = static_cast<float>(cfg<double>("wave-floor-y"));
        float ceilY  = static_cast<float>(cfg<double>("wave-ceiling-y"));

        float reach = (H + 3) * s + pr.size.width;
        refreshWaveCache(this->getPositionX(), reach);

        std::vector<CCRect> rects;
        for (auto& o : f->waveCache) {
            auto hr = o->getObjectRect();
            if (hr.getMaxX() < pr.getMinX() - 2.f) continue;
            if (hr.getMinX() > pr.getMaxX() + reach) continue;
            hr.origin.x -= m; hr.origin.y -= m;
            hr.size.width += 2 * m; hr.size.height += 2 * m;
            rects.push_back(hr);
        }

        bool eff = desired;
        if (!rects.empty()) {
            float cx = pr.getMidX(), cy = pr.getMidY();
            float w = pr.size.width, h = pr.size.height;

            // Segment from frame k with u holds so far, choosing du (1 = hold).
            auto segSafe = [&](int k, int u, int du) {
                for (float t : {0.34f, 0.67f, 1.f}) {
                    float x = cx + (k + t) * s;
                    float y = cy + dir * ((2 * u - k) + t * (2 * du - 1)) * v;
                    // Approximate sliding on floor/ceiling by clamping for the check.
                    if (floorY > 0.f) y = std::max(y, floorY + h * 0.5f);
                    if (ceilY  > 0.f) y = std::min(y, ceilY  - h * 0.5f);
                    CCRect wr(x - w * 0.5f, y - h * 0.5f, w, h);
                    for (auto& r : rects) if (wr.intersectsRect(r)) return false;
                }
                return true;
            };

            int stride = H + 2;
            std::vector<char> can((H + 1) * stride, 1);
            for (int k = H - 1; k >= 0; --k) {
                for (int u = 0; u <= k; ++u) {
                    bool up = can[(k + 1) * stride + (u + 1)] && segSafe(k, u, 1);
                    bool dn = can[(k + 1) * stride + u]       && segSafe(k, u, 0);
                    can[k * stride + u] = (up || dn);
                }
            }
            bool holdOK    = can[1 * stride + 1] && segSafe(0, 0, 1);
            bool releaseOK = can[1 * stride + 0] && segSafe(0, 0, 0);

            if (eff && !holdOK && releaseOK) eff = false;
            else if (!eff && !releaseOK && holdOK) eff = true;
            // Neither survivable: follow the player.
        }

        if (eff != f->assistHeld) {
            if (eff) PlayerObject::pushButton(PlayerButton::Jump);
            else     PlayerObject::releaseButton(PlayerButton::Jump);
            f->assistHeld = eff;
            if (consoleOut() && eff != desired)
                log::info("wave override -> {}", eff ? "hold" : "release");
        }
    }

    // =====================================================================
    // Hooks
    // =====================================================================

    bool pushButton(PlayerButton button) {
        auto f = m_fields.self();
        if (button == PlayerButton::Jump) {
            f->held = true;
            if (waveActive()) return true; // the assist applies wave input itself
            if (cubeActive() && !m_isOnGround) {
                f->pending = true;
                f->pendingLeft = static_cast<float>(cubeWindow());
            }
        }
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        auto f = m_fields.self();
        if (button == PlayerButton::Jump) {
            f->held = false;
            if (waveActive()) return true;
        }
        return PlayerObject::releaseButton(button);
    }

    void update(float dt) {
        auto f = m_fields.self();
        float frames = dt * 60.f;

        // Keep the game's own hold state in sync when entering/leaving the wave.
        bool waveNow = waveActive();
        if (waveNow != f->wasWave) {
            if (waveNow) f->assistHeld = f->held;
            if (f->held) PlayerObject::pushButton(PlayerButton::Jump);
            else         PlayerObject::releaseButton(PlayerButton::Jump);
            f->wasWave = waveNow;
        }

        if (waveNow) waveStep();
        else if (cubeActive()) cubeStep(frames);

        PlayerObject::update(dt);
    }

    void resetObject() {
        auto f = m_fields.self();
        f->held = f->pending = f->landingHandled = f->delaying = f->tapRelease = false;
        f->wasWave = f->assistHeld = false;
        f->pendingLeft = f->waited = 0.f;
        f->cubeHazards.clear();
        f->waveCache.clear();
        f->cacheStart = 0.f;
        f->cacheEnd = -1.f;
        PlayerObject::resetObject();
    }
};

