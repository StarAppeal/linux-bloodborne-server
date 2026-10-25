// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame generation's pacing rules (vk_frame_generation.h, vk_presenter.cpp), apart so
// they can be tested without a GPU. From Supermedo's bbport (026f8ff, 389124d, aabd7f7):
//  - the frames made per game frame for the provider (FSR 3.1: one; DLSS: the multiplier less
//    one, up to three on a GeForce RTX 50);
//  - "fit to display refresh": at most the frames the display shows between two game frames,
//    decided once a second; always so with V-Sync (FIFO), where each frame more waits for the
//    display (upstream caps the game instead, which would slow co-op's fixed 60 FPS);
//  - a pause when presenting takes the whole frame interval (a GPU or display too slow);
//  - the FPS counter: the total and, with frames generated, the game's own rate;
//  - DLSS Frame Generation's release apart from the swap thread's present.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>

namespace BbFrameGenPace {

/// BbSettings::frame_gen_provider values (0 when frame generation is off).
enum Provider { ProviderOff = 0, ProviderFsr3 = 1, ProviderDlss = 2 };

/// Frames generated per game frame at most (DLSS 4x).
constexpr unsigned MaxGenerated = 3;

/// Generated frames per game frame the settings ask for. `dlss_max`: what DLSS Frame
/// Generation can make on this GPU (0 none: FSR 3.1 takes over, one).
inline unsigned WantedCount(int provider, int multiplier, int dlss_max) {
    if (provider == ProviderOff) {
        return 0;
    }
    if (provider == ProviderDlss && dlss_max > 0) {
        const int most = std::min(dlss_max, int(MaxGenerated));
        return unsigned(std::clamp(multiplier - 1, 1, most));
    }
    return 1;
}

/// How many generated frames go out per game frame now (present thread, once per game frame).
class Limiter {
public:
    /// A game frame was presented at `now_s` (seconds). `hz`: the display's refresh rate (0
    /// unknown); `fit`: no more frames than it shows (the setting, or V-Sync); `interval_ms`: the
    /// game's smoothed frame interval; `present_ms`: the time presenting its images took.
    unsigned OnGameFrame(double now_s, unsigned hz, bool fit, double interval_ms, double present_ms) {
        // The images the display shows per game frame: 60 FPS on 144 Hz has room for one
        // generated frame, on 240 Hz for three. Decided once a second from that second's game
        // frame rate (single slow frames while an area loads do not count): down at once when
        // 10% short, up after two seconds with 5% to spare.
        if (window_start < 0.0) {
            window_start = now_s;
            frames = 0;
        }
        ++frames;
        const double window_s = now_s - window_start;
        if (window_s >= 1.0) {
            const double ratio = hz * window_s / frames;
            window_start = now_s;
            frames = 0;
            if (window_s < 2.0) { // not after a pause of the game
                if (limit > 0 && ratio < (limit + 1) * 0.9) {
                    while (limit > 0 && ratio < (limit + 1) * 0.9) {
                        --limit;
                    }
                    up_seconds = 0;
                } else if (limit < MaxGenerated && ratio >= (limit + 2) * 0.95) {
                    if (++up_seconds >= 2) {
                        ++limit;
                        up_seconds = 0;
                    }
                } else {
                    up_seconds = 0;
                }
            }
        }
        if (!fit || hz == 0) {
            // The multiplier as chosen: past the refresh rate the display shows the newest image.
            limit = MaxGenerated;
        }
        // The share of the frame interval spent presenting: paced, about count / (count + 1).
        // Near one presenting holds the game back: off for a while, longer each time.
        // Each frame counts at most 1.2: one long hitch (a load, a shader compile) is no display
        // that cannot keep up; about ten overloaded frames in a row are (Supermedo b382856).
        if (interval_ms > 0.0) {
            busy += (std::min(present_ms / interval_ms, 1.2) - busy) * 0.1;
        }
        if (shown_before && busy > 0.97) {
            hold_seconds = now_s - hold_until > 60.0 ? 5 : std::min(hold_seconds * 2, 120);
            hold_until = now_s + hold_seconds;
            busy = 0.0;
            std::printf("Frame generation: paused for %d s, presenting does not keep up (%.1f of "
                        "%.1f ms per game frame)\n",
                        hold_seconds, present_ms, interval_ms);
        }
        const unsigned now_limit = Paused(now_s) ? 0 : limit;
        shown_before = now_limit > 0;
        return now_limit;
    }

    [[nodiscard]] bool Paused(double now_s) const noexcept {
        return now_s < hold_until;
    }

    /// Frame generation off: the next start decides afresh.
    void Reset() noexcept {
        limit = 0;
        frames = up_seconds = 0;
        window_start = -1.0;
        busy = 0.0;
        shown_before = false;
    }

private:
    unsigned limit = 0;
    unsigned frames = 0, up_seconds = 0;
    double window_start = -1.0;
    double busy = 0.0;
    bool shown_before = false;
    int hold_seconds = 5;
    double hold_until = -1e9;
};

/// The FPS counter's text: "120 FPS (game 60, frame gen 2x)" while frames are generated (the
/// total follows the game's rate times the multiplier, the game's rate is what resolution and
/// upscaler change), else "60 FPS  16.7 ms".
inline void FpsText(char* text, std::size_t size, float shown_fps, float game_fps, bool generating,
                    float frame_ms) {
    if (generating && game_fps > 0.0f && shown_fps > game_fps * 1.25f) {
        std::snprintf(text, size, "%.0f FPS (game %.0f, frame gen %dx)", shown_fps, game_fps,
                      int(shown_fps / game_fps + 0.5f));
    } else {
        std::snprintf(text, size, "%.0f FPS  %.1f ms", shown_fps, frame_ms);
    }
}

// ---- What is really shown (Kaniel 2026-10-25: "sometimes it just says it's up but does nothing")

/// Why frame generation, switched on, shows no generated frames now.
enum Idle {
    IdleNone = 0,      ///< generating
    IdleStarting,      ///< nothing measured yet
    IdleFailed,        ///< the provider refused; until the switch is turned off and on
    IdleNoMemory,      ///< no video memory for the generated frames; likewise
    IdleNoScene,       ///< menus, loading screens, movies: nothing to interpolate
    IdleSceneLarger,   ///< the scene is rendered larger than the window
    IdleNoRoom,        ///< fitted to the display: no room at this frame rate
    IdlePresentBehind, ///< Limiter's pause: presenting did not keep up
    IdleLate,          ///< made, but replaced before the display showed them
};

/// The log's words for `idle` (the menu has its own, translated).
inline const char* IdleText(int idle) {
    switch (idle) {
    case IdleStarting:
        return "starting";
    case IdleFailed:
        return "the provider refused (switch it off and on to retry)";
    case IdleNoMemory:
        return "not enough video memory for the generated frames";
    case IdleNoScene:
        return "no game scene (menu, loading screen or movie)";
    case IdleSceneLarger:
        return "the scene is rendered larger than the window";
    case IdleNoRoom:
        return "no room on the display at this frame rate";
    case IdlePresentBehind:
        return "paused, presenting does not keep up";
    case IdleLate:
        return "generated frames are replaced before the display shows them";
    default:
        return "generating";
    }
}

/// A generated image counts as shown when the next image goes out at least half a refresh
/// later: Mailbox and Immediate replace (or tear over) an image the display has not scanned out
/// yet, V-Sync (FIFO) queues every one. `gap_ms`: from its present to the next.
inline bool GeneratedShown(double gap_ms, unsigned hz, bool fifo) {
    if (fifo) {
        return true;
    }
    const double needed = hz > 0 ? 500.0 / hz : 2.0;
    return gap_ms >= needed;
}

/// One measuring window of the swap thread: game frames presented, generated frames presented
/// and how many of those were shown (GeneratedShown); `starved`: generated frames sent without
/// their spacing because the game needed every presentation frame (the log's count).
struct Window {
    double seconds = 0.0;
    unsigned game = 0, generated = 0, shown = 0, starved = 0;
};

/// Presentation frames (the game frames' images) the presenter keeps at most.
constexpr unsigned MaxPresentFrames = 8;

/// Presentation frames for `images` swapchain images. With frame generation the present thread
/// holds each game frame from its GPU end through the generated frames' spacing (half a frame at
/// 2x), while the game draws the next one, another waits in the swap queue and the last one's
/// present copy still runs: five, so the game always finds a free one and the spacing is never
/// cut (Mailbox then replaced the generated frame: the release thread's test 2026-10-25, three
/// frames, 25 of 55 generated frames a second shown).
inline unsigned PresentFramesNeeded(unsigned images, bool generating) {
    return std::min(generating ? std::max(images, 5u) : images, std::max(images, MaxPresentFrames));
}

/// What the menu, the FPS counter and the log say about a window.
struct Status {
    bool active = false; ///< generated frames reach the display
    int idle = IdleStarting;
    float game_fps = 0.0f, shown_fps = 0.0f; ///< shown: game frames plus shown generated ones
    int multiplier = 1;                      ///< images shown per game frame, rounded
};

/// `plan_idle`: why the presenter's last plan made no frames (IdleNone when it made them).
/// Active when at least a quarter of the game frames had a generated frame shown (a load's
/// hitches and history restarts skip a few); otherwise why not.
inline Status Judge(const Window& window, int plan_idle) {
    Status status;
    if (window.seconds <= 0.0 || window.game == 0) {
        status.idle = plan_idle != IdleNone ? plan_idle : IdleStarting;
        return status;
    }
    status.game_fps = float(window.game / window.seconds);
    status.shown_fps = float((window.game + window.shown) / window.seconds);
    status.multiplier = int(double(window.game + window.shown) / window.game + 0.5);
    status.active = window.shown * 4 >= window.game;
    if (status.active) {
        status.idle = IdleNone;
    } else if (plan_idle != IdleNone) {
        status.idle = plan_idle;
    } else {
        status.idle = window.generated > 0 ? IdleLate : IdleStarting;
    }
    return status;
}

/// Reports a change of Status once it held for two windows (one window of a loading screen is
/// no change worth a log line).
class StatusTracker {
public:
    /// True when `status` is a change to report now.
    bool Update(const Status& status) {
        const int key = Key(status);
        if (key == reported) {
            candidate = -1;
            return false;
        }
        if (key != candidate) {
            candidate = key;
            held = 0;
        }
        if (++held < 2) {
            return false;
        }
        reported = key;
        candidate = -1;
        return true;
    }
    /// Switched off: the next switch-on reports afresh.
    void Reset() noexcept {
        reported = candidate = -1;
        held = 0;
    }

private:
    static int Key(const Status& status) {
        return status.active ? 1000 + status.multiplier : status.idle;
    }
    int reported = -1, candidate = -1, held = 0;
};

/// DLSS Frame Generation released (switched off, or another provider): under the present's lock
/// (`present_lock`, which every present and queue submit takes), once the queues are idle
/// (`wait_idle`), so no present runs beside the release. Kaniel's playtest 2026-10-22 (v1.1.5):
/// NVIDIA's driver crashed in the swap thread's present right after frame generation went off.
template <class Lock, class WaitIdle, class Release>
void ReleaseBesidePresent(Lock& present_lock, WaitIdle&& wait_idle, Release&& release) {
    std::scoped_lock lock{present_lock};
    wait_idle();
    release();
}

} // namespace BbFrameGenPace
