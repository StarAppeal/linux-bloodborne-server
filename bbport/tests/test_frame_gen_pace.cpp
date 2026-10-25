// Frame generation's pacing rules (gpu/shim/bbport_frame_gen_pace.h): how many frames are made
// per game frame for the provider and multiplier, how many the display has room for when fitted
// (or with V-Sync), the pause when presenting falls behind, and the FPS counter's text. No game,
// Vulkan device or window.
//   ninja -C out/gpu frame-gen-pace-test && out/gpu/frame-gen-pace-test.exe
#include <cassert>
#include <cstdio>
#include <cstring>
#include "bbport_frame_gen_pace.h"

using namespace BbFrameGenPace;

namespace {

/// `seconds` of game frames at `fps` fed to `limiter`: the limit after the last one.
unsigned Run(Limiter& limiter, double& clock, double seconds, double fps, unsigned hz, bool fit,
             double present_share = 0.5) {
    unsigned limit = 0;
    const double frame = 1.0 / fps;
    for (double t = 0.0; t < seconds; t += frame) {
        clock += frame;
        limit = limiter.OnGameFrame(clock, hz, fit, 1000.0 / fps, 1000.0 / fps * present_share);
    }
    return limit;
}

} // namespace

int main() {
    // Generated frames per game frame: FSR 3.1 makes one whatever the multiplier.
    assert(WantedCount(ProviderFsr3, 4, 3) == 1);
    assert(WantedCount(ProviderFsr3, 2, 0) == 1);
    // DLSS: the multiplier less one, within what the GPU can (RTX 40: one, RTX 50: three).
    assert(WantedCount(ProviderDlss, 2, 3) == 1);
    assert(WantedCount(ProviderDlss, 3, 3) == 2);
    assert(WantedCount(ProviderDlss, 4, 3) == 3);
    assert(WantedCount(ProviderDlss, 4, 1) == 1);
    assert(WantedCount(ProviderDlss, 9, 3) == 3);
    // Without DLSS Frame Generation on the GPU, FSR 3.1's one (the provider falls back).
    assert(WantedCount(ProviderDlss, 4, 0) == 1);
    assert(WantedCount(ProviderOff, 4, 3) == 0);

    // Not fitted: every frame wanted from the start, on any display.
    {
        Limiter limiter;
        double clock = 0.0;
        assert(Run(limiter, clock, 3.0, 60.0, 60, false) == MaxGenerated);
    }
    // Fitted: 60 FPS on 144 Hz has room for one generated frame (2x = 120), on 240 Hz for three.
    {
        Limiter limiter;
        double clock = 0.0;
        assert(Run(limiter, clock, 0.5, 60.0, 144, true) == 0); // starts without, decides per second
        assert(Run(limiter, clock, 4.0, 60.0, 144, true) == 1);
        assert(Run(limiter, clock, 6.0, 60.0, 144, true) == 1); // stays
    }
    {
        Limiter limiter;
        double clock = 0.0;
        assert(Run(limiter, clock, 10.0, 60.0, 240, true) == 3);
    }
    // Fitted: 100 FPS on 144 Hz has no room; when the game slows to 60 there is.
    {
        Limiter limiter;
        double clock = 0.0;
        assert(Run(limiter, clock, 5.0, 100.0, 144, true) == 0);
        assert(Run(limiter, clock, 5.0, 60.0, 144, true) == 1);
        // Back to 100: down at once (within the next second).
        assert(Run(limiter, clock, 2.5, 100.0, 144, true) == 0);
    }
    // Presenting that takes the whole frame interval (FIFO on a slower display) pauses it,
    // fitted or not, and it comes back after the pause.
    {
        Limiter limiter;
        double clock = 0.0;
        assert(Run(limiter, clock, 1.0, 60.0, 240, false) == MaxGenerated);
        assert(Run(limiter, clock, 1.0, 60.0, 240, false, 1.0) == 0);
        assert(limiter.Paused(clock));
        assert(Run(limiter, clock, 6.0, 60.0, 240, false) == MaxGenerated);
        assert(!limiter.Paused(clock));
    }
    // One long hitch (a load, a shader compile: 98 ms of a 16.7 ms interval) is no display that
    // cannot keep up: no pause (Supermedo b382856). Ten overloaded frames in a row still pause.
    {
        Limiter limiter;
        double clock = 0.0;
        assert(Run(limiter, clock, 1.0, 60.0, 240, false) == MaxGenerated);
        clock += 1.0 / 60.0;
        assert(limiter.OnGameFrame(clock, 240, false, 1000.0 / 60.0, 98.0) == MaxGenerated);
        assert(!limiter.Paused(clock));
        assert(Run(limiter, clock, 1.0, 60.0, 240, false) == MaxGenerated);
        assert(Run(limiter, clock, 0.5, 60.0, 240, false, 1.2) == 0);
        assert(limiter.Paused(clock));
    }
    // The refresh rate unknown (0): no room is assumed to be missing.
    {
        Limiter limiter;
        double clock = 0.0;
        assert(Run(limiter, clock, 3.0, 60.0, 0, true) == MaxGenerated);
    }

    // FPS counter: the total, and with frames generated the game's own rate and the multiplier.
    char text[96];
    FpsText(text, sizeof(text), 120.0f, 60.0f, true, 8.3f);
    assert(std::strcmp(text, "120 FPS (game 60, frame gen 2x)") == 0);
    FpsText(text, sizeof(text), 238.0f, 60.0f, true, 4.2f);
    assert(std::strcmp(text, "238 FPS (game 60, frame gen 4x)") == 0);
    FpsText(text, sizeof(text), 60.0f, 60.0f, false, 16.7f);
    assert(std::strcmp(text, "60 FPS  16.7 ms") == 0);
    FpsText(text, sizeof(text), 60.0f, 0.0f, true, 16.7f); // game rate not known yet
    assert(std::strcmp(text, "60 FPS  16.7 ms") == 0);

    // Shown or not (Kaniel 2026-10-25: "says it's up but does nothing"): Mailbox replaces an
    // image the next one follows within half a refresh; V-Sync (FIFO) shows every one.
    assert(GeneratedShown(8.3, 240, false));   // 60 FPS 2x on 240 Hz: 8.3 ms apart
    assert(!GeneratedShown(0.2, 240, false));  // the generated and the game frame at once
    assert(GeneratedShown(0.2, 240, true));
    assert(!GeneratedShown(3.0, 144, false));  // 6.9 ms refresh needs 3.5 ms
    assert(GeneratedShown(3.5, 144, false));
    assert(GeneratedShown(3.0, 0, false));     // refresh unknown: 2 ms
    assert(!GeneratedShown(1.0, 0, false));

    // The status of a measuring window: generated frames that reach the display, else why not.
    {
        // 60 FPS, one generated frame shown per game frame: active, 120 shown, 2x.
        Status st = Judge({1.0, 60, 60, 60}, IdleNone);
        assert(st.active && st.idle == IdleNone && st.multiplier == 2);
        assert(st.game_fps == 60.0f && st.shown_fps == 120.0f);
        // DLSS 4x.
        st = Judge({1.0, 60, 180, 180}, IdleNone);
        assert(st.active && st.multiplier == 4 && st.shown_fps == 240.0f);
        // Made every frame but none shown (presented together with the game frame): not
        // active, though the provider ran (the old counter said 120 FPS here).
        st = Judge({1.0, 60, 60, 0}, IdleNone);
        assert(!st.active && st.idle == IdleLate && st.multiplier == 1);
        assert(st.shown_fps == 60.0f);
        // Nothing made: the plan's reason.
        st = Judge({1.0, 60, 0, 0}, IdleNoScene);
        assert(!st.active && st.idle == IdleNoScene);
        st = Judge({1.0, 60, 0, 0}, IdleFailed);
        assert(!st.active && st.idle == IdleFailed);
        st = Judge({1.0, 60, 0, 0}, IdleNoRoom);
        assert(!st.active && st.idle == IdleNoRoom);
        // A few shown during a load's hitches: still active from a quarter of the game frames.
        st = Judge({1.0, 40, 12, 10}, IdleNoScene);
        assert(st.active && st.idle == IdleNone);
        st = Judge({1.0, 40, 12, 9}, IdleNoScene);
        assert(!st.active && st.idle == IdleNoScene);
        // Nothing presented yet.
        st = Judge({0.0, 0, 0, 0}, IdleNone);
        assert(!st.active && st.idle == IdleStarting);
        st = Judge({0.5, 0, 0, 0}, IdleFailed);
        assert(!st.active && st.idle == IdleFailed);
        // Log words for every reason.
        for (int idle = IdleNone; idle <= IdleLate; ++idle) {
            assert(IdleText(idle) && IdleText(idle)[0]);
        }
        assert(std::strcmp(IdleText(IdleNone), "generating") == 0);
    }

    // Presentation frames: with frame generation the present thread holds each game frame for
    // the spacing, so the pool must leave the game one to draw into meanwhile (the release
    // thread's test 2026-10-25: 3 frames, 25 of 55 generated frames/s shown, the rest sent
    // with no spacing because no frame was free).
    assert(PresentFramesNeeded(3, false) == 3);
    assert(PresentFramesNeeded(4, false) == 4);
    assert(PresentFramesNeeded(3, true) == 5);
    assert(PresentFramesNeeded(4, true) == 5);
    assert(PresentFramesNeeded(6, true) == 6);
    assert(PresentFramesNeeded(2, true) <= MaxPresentFrames);
    assert(PresentFramesNeeded(9, false) == 9);

    // Generated frames lost because the game needed every presentation frame (no spacing) are
    // counted apart, for the log.
    {
        const Status st = Judge({1.0, 60, 60, 25, 35}, IdleNone);
        assert(st.active && st.multiplier == 1);
        Window w{1.0, 60, 60, 25, 35};
        assert(w.starved == 35);
    }

    // A change of status is reported once it held for two windows.
    {
        StatusTracker tracker;
        const Status active = Judge({0.5, 30, 30, 30}, IdleNone);
        const Status no_scene = Judge({0.5, 30, 0, 0}, IdleNoScene);
        assert(!tracker.Update(active));
        assert(tracker.Update(active));  // held: reported
        assert(!tracker.Update(active)); // unchanged
        assert(!tracker.Update(no_scene)); // one window of a loading screen
        assert(!tracker.Update(active));
        assert(!tracker.Update(no_scene));
        assert(tracker.Update(no_scene));
        // A different multiplier is a change.
        const Status four = Judge({0.5, 30, 90, 90}, IdleNone);
        assert(!tracker.Update(four));
        assert(tracker.Update(four));
        tracker.Reset();
        assert(!tracker.Update(four));
        assert(tracker.Update(four));
    }

    // Releasing DLSS Frame Generation (switched off): under the present's lock, after the queue
    // has finished presenting (Kaniel's playtest 2026-10-22: the driver crashed in the swap
    // thread's present right after the release).
    {
        struct PresentLock {
            bool held = false;
            int locks = 0;
            void lock() {
                assert(!held);
                held = true;
                ++locks;
            }
            void unlock() {
                assert(held);
                held = false;
            }
        } present;
        char steps[8] = {};
        int n = 0;
        ReleaseBesidePresent(
            present,
            [&] {
                assert(present.held);
                steps[n++] = 'w';
            },
            [&] {
                assert(present.held);
                steps[n++] = 'r';
            });
        assert(std::strcmp(steps, "wr") == 0);
        assert(present.locks == 1 && !present.held);
    }

    std::printf("frame-gen-pace-test: all checks passed\n");
    return 0;
}
