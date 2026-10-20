#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>

#ifdef GEODE_IS_DESKTOP
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#endif

using namespace geode::prelude;

namespace {
    struct Stage {
        int allTicks;   // release delay in normal gamemodes
        int waveTicks;  // release delay while in wave
        bool enabled;
    };

    // 1 tick = 1/240 s (4 ticks = 1 frame at 60 fps)
    constexpr Stage stages[] = {
        {2, 2, true},
        {4, 4, true},
        {8, 8, true},
        {16, 8, true},
        {0, 0, false}
    };

    // Stage index 0..4, backed by the "stage" setting (1..5) so it works on
    // Android, where there is no F6 key.
    int currentStage() {
        auto value = Mod::get()->getSettingValue<int64_t>("stage");
        return static_cast<int>(std::clamp<int64_t>(value, 1, 5)) - 1;
    }

    void logStage() {
        int index = currentStage();
        auto const& stage = stages[index];

        if (!stage.enabled) {
            log::info("[InputBuffer] Stage 5: OFF (100% Raw Input)");
            return;
        }

        log::info(
            "[InputBuffer] Stage {}: {} Ticks All | {} Ticks Wave",
            index + 1,
            stage.allTicks,
            stage.waveTicks
        );
    }

    // A jump-button release that we are holding back for a few ticks.
    // Because the game keeps seeing the button as "down", a tap that lands
    // slightly before a landing / orb still counts.
    struct PendingRelease {
        bool active = false;
        int ticksLeft = 0;
    };

    PendingRelease pending[2]; // [0] = player 1, [1] = player 2
    bool flushing = false;     // true while we send a release ourselves

    void clearPending() {
        pending[0] = PendingRelease{};
        pending[1] = PendingRelease{};
    }

    int releaseDelay(PlayLayer* pl, bool isPlayer1) {
        auto const& stage = stages[currentStage()];
        if (!stage.enabled) return 0;

        PlayerObject* player = isPlayer1 ? pl->m_player1 : pl->m_player2;
        bool inWave = player && player->m_isDart;
        return inWave ? stage.waveTicks : stage.allTicks;
    }
}

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();

        // Only buffer the jump button, only inside a real level (not the
        // editor), and never while we are flushing a release ourselves.
        if (flushing || button != 1 || !pl ||
            static_cast<GJBaseGameLayer*>(pl) != static_cast<GJBaseGameLayer*>(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& slot = pending[isPlayer1 ? 0 : 1];

        if (down) {
            if (slot.active) {
                // Re-press while a release is held back: send that release
                // first so the game still sees a clean up -> down.
                slot.active = false;
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
            }
            GJBaseGameLayer::handleButton(true, button, isPlayer1);
            return;
        }

        int delay = releaseDelay(pl, isPlayer1);
        if (delay <= 0) {
            slot.active = false;
            GJBaseGameLayer::handleButton(false, button, isPlayer1);
            return;
        }

        // Hold the release back instead of passing it on now.
        slot.active = true;
        slot.ticksLeft = delay;
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    void update(float dt) {
        // Player 1's update runs once per physics tick, so use it as the clock.
        if (auto pl = PlayLayer::get()) {
            if (this == pl->m_player1) {
                for (int i = 0; i < 2; i++) {
                    auto& slot = pending[i];
                    if (!slot.active) continue;
                    if (--slot.ticksLeft > 0) continue;

                    slot.active = false;
                    flushing = true;
                    static_cast<GJBaseGameLayer*>(pl)->handleButton(false, 1, i == 0);
                    flushing = false;
                }
            }
        }

        PlayerObject::update(dt);
    }
};

class $modify(BufPlayLayer, PlayLayer) {
    void resetLevel() {
        bool was[2] = {pending[0].active, pending[1].active};
        clearPending();

        PlayLayer::resetLevel();

        // The button was physically released, so make sure the game knows.
        flushing = true;
        for (int i = 0; i < 2; i++) {
            if (was[i]) GJBaseGameLayer::handleButton(false, 1, i == 0);
        }
        flushing = false;
    }

    void onQuit() {
        clearPending();
        PlayLayer::onQuit();
    }
};

// F6 only exists on desktop, so this hook is compiled out on Android.
#ifdef GEODE_IS_DESKTOP
class $modify(PredictiveInputBufferKeyboard, CCKeyboardDispatcher) {
    void dispatchKeyboardMSG(
        enumKeyCodes key,
        bool isKeyDown,
        bool isKeyRepeat,
        double timestamp
    ) {
        if (key == enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat) {
            int next = (currentStage() + 1) % 5;
            Mod::get()->setSettingValue<int64_t>("stage", next + 1);
        }

        CCKeyboardDispatcher::dispatchKeyboardMSG(
            key, isKeyDown, isKeyRepeat, timestamp
        );
    }
};
#endif

$on_mod(Loaded) {
    log::info("[InputBuffer] Loaded.");
    logStage();

    // Fires whenever the setting changes (settings menu or F6 on desktop).
    listenForSettingChanges<int64_t>("stage", [](int64_t) {
        logStage();
    });
}
