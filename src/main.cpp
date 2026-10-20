#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>

#ifdef GEODE_IS_DESKTOP
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#endif

using namespace geode::prelude;

namespace {
    struct Stage {
        int allTicks;   // window for normal gamemodes
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

    bool orbBufferEnabled() {
        return Mod::get()->getSettingValue<bool>("orb-buffer");
    }

    bool debugPopups() {
        return Mod::get()->getSettingValue<bool>("debug");
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

    // In-game popup: shows the current stage and its window size, and lets
    // you switch to the next stage without leaving the level.
    void openStagePopup() {
        int index = currentStage();
        auto const& stage = stages[index];

        std::string body;
        if (!stage.enabled) {
            body = "Stage 5 / 5: OFF\nRaw input, no buffer.";
        } else {
            body = fmt::format(
                "Stage {} / 5\nAll modes: {} ticks (~{:.0f} ms)\nWave: {} ticks (~{:.0f} ms)\nOrb pre-click: {}",
                index + 1,
                stage.allTicks, stage.allTicks * 1000.f / 240.f,
                stage.waveTicks, stage.waveTicks * 1000.f / 240.f,
                orbBufferEnabled() ? fmt::format("{} ticks", stage.allTicks) : std::string("off")
            );
        }

        createQuickPopup(
            "Input Buffer",
            body,
            "Close", "Next stage",
            [](FLAlertLayer*, bool next) {
                if (!next) return;
                int n = (currentStage() + 1) % 5;
                Mod::get()->setSettingValue<int64_t>("stage", n + 1);
                openStagePopup(); // reopen with the new stage
            }
        );
    }

    // A jump-button release that we are holding back for a few ticks.
    // Because the game keeps seeing the button as "down", a tap that lands
    // slightly before a landing / orb still counts.
    struct PendingRelease {
        bool active = false;
        int ticksLeft = 0;
    };

    PendingRelease pending[2];  // [0] = player 1, [1] = player 2
    bool flushing = false;      // true while we send a button event ourselves

    // Orb pre-click state: if you click up to N ticks BEFORE touching an orb,
    // the click is re-sent at the moment you touch it.
    int tickCount = 0;
    int pressTick[2] = {0, 0};
    bool armed[2] = {false, false};
    bool physDown[2] = {false, false};   // what your finger is really doing
    void const* prevRing[2] = {nullptr, nullptr};

    void clearPending() {
        pending[0] = PendingRelease{};
        pending[1] = PendingRelease{};
        armed[0] = armed[1] = false;
        prevRing[0] = prevRing[1] = nullptr;
    }

    int releaseDelay(PlayLayer* pl, bool isPlayer1) {
        auto const& stage = stages[currentStage()];
        if (!stage.enabled) return 0;

        PlayerObject* player = isPlayer1 ? pl->m_player1 : pl->m_player2;
        bool inWave = player && player->m_isDart;
        return inWave ? stage.waveTicks : stage.allTicks;
    }

    // Runs after each player update. Detects the first tick an orb is touched
    // and, if you clicked shortly before, re-sends the click so it fires.
    void orbBuffer(PlayLayer* pl, PlayerObject* player, int k) {
        void const* ring = static_cast<void const*>(player->m_touchedRing);
        bool newTouch = ring && ring != prevRing[k];
        prevRing[k] = ring;
        if (!newTouch) return;

        auto const& stage = stages[currentStage()];
        if (!stage.enabled || !orbBufferEnabled()) return;
        if (!armed[k] || tickCount - pressTick[k] > stage.allTicks) return;

        armed[k] = false;

        // Fresh click while touching the orb: release then press.
        flushing = true;
        auto gl = static_cast<GJBaseGameLayer*>(pl);
        gl->handleButton(false, 1, k == 0);
        gl->handleButton(true, 1, k == 0);
        flushing = false;

        // If your finger is already up, don't leave the game holding.
        if (!physDown[k]) {
            pending[k].active = true;
            pending[k].ticksLeft = 2;
        }

        if (debugPopups()) {
            Notification::create(
                "Buffer: orb click applied",
                NotificationIcon::Success,
                0.5f
            )->show();
        }
    }
}

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();

        // Only buffer the jump button, only inside a real level (not the
        // editor), and never while we are sending an event ourselves.
        if (flushing || button != 1 || !pl ||
            static_cast<GJBaseGameLayer*>(pl) != static_cast<GJBaseGameLayer*>(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int k = isPlayer1 ? 0 : 1;
        auto& slot = pending[k];
        physDown[k] = down;

        if (down) {
            if (slot.active) {
                // Re-press while a release is held back: send that release
                // first so the game still sees a clean up -> down.
                slot.active = false;
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
            }
            GJBaseGameLayer::handleButton(true, button, isPlayer1);

            // Arm the orb pre-click, but only if you aren't already on an orb.
            PlayerObject* player = isPlayer1 ? pl->m_player1 : pl->m_player2;
            armed[k] = player && !player->m_touchedRing;
            pressTick[k] = tickCount;
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

        if (debugPopups()) {
            Notification::create(
                fmt::format("Buffer: release held {} ticks", delay),
                NotificationIcon::Info,
                0.5f
            )->show();
        }
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        int k = -1;
        if (pl) {
            if (this == pl->m_player1) k = 0;
            else if (this == pl->m_player2) k = 1;
        }

        // Player 1's update runs once per physics tick, so use it as the clock.
        if (k == 0) {
            tickCount++;
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

        PlayerObject::update(dt);

        if (k >= 0) orbBuffer(pl, this, k);
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

// Adds a "Buffer" button (top-right) to the pause menu that opens the popup.
class $modify(BufPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto sprite = ButtonSprite::create("Buffer");
        auto button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(BufPauseLayer::onBufferButton)
        );

        auto menu = CCMenu::create();
        menu->addChild(button);

        auto winSize = CCDirector::get()->getWinSize();
        menu->setPosition({winSize.width - 60.f, winSize.height - 30.f});
        this->addChild(menu);
    }

    void onBufferButton(CCObject*) {
        openStagePopup();
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

    // Fires whenever the setting changes (settings menu, popup, or F6).
    listenForSettingChanges<int64_t>("stage", [](int64_t) {
        logStage();
    });
}
