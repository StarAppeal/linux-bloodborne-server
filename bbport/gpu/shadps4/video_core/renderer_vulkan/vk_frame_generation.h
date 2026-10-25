// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame generation on top of the temporal upscaler (docs: README-Windows.txt).
//
// Generated frames are shown between every two game frames: FSR 3.1 (any GPU) makes one, DLSS
// Frame Generation (bbport_dlss.dll ABI 2 with nvngx_dlssg.dll; GeForce RTX 40: one, RTX 50: up
// to three, from Supermedo's bbport 026f8ff) as many as the multiplier asks for. The port keeps
// its own swapchain and presenter; this class drives the providers and keeps what they need:
//
//   * at the upscale (UpscalerFsr3/4/DLSS/XeSS/TAA, GPU command thread): the scene depth and the
//     camera motion vectors of the frame, which the game overwrites before the frame is
//     presented (CaptureScene), and a copy of the upscaled scene right before the first UI
//     draw (CaptureHudless);
//   * in the presenter (PrepareFrame, same thread): Plan() decides whether and how many frames
//     this frame gets, Record() puts the provider's passes after the frame's host passes;
//   * on the swap thread (Present): the smoothed interval of the game frames, which places the
//     generated frames evenly, how many the display has room for, and the statistics.
//
// SDR output only. With the switch off none of this runs, and its memory is freed.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "bbport_frame_gen_pace.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableFrameGenerationContext;

namespace Vulkan {

class Dlss;
class Instance;
class Scheduler;

class FrameGeneration {
public:
    /// Frames generated per game frame at most (DLSS 4x).
    static constexpr u32 MaxGenerated = BbFrameGenPace::MaxGenerated;

    FrameGeneration(const Instance& instance, Scheduler& scheduler);
    ~FrameGeneration();

    /// The switch is on (bbport.ini frame_generation, BB_FRAME_GEN) and a temporal upscaler is
    /// selected. Thread safe.
    [[nodiscard]] static bool Requested();
    /// Generated frames per game frame the settings ask for (FSR 3.1: one; DLSS: the multiplier
    /// less one, within what the GPU can), 0 when not requested. Thread safe.
    [[nodiscard]] static u32 WantedCount();

    // ---- Upscaler side: GPU command thread, in command order ---------------------------------

    struct Scene {
        vk::Image depth;
        vk::ImageLayout depth_layout; ///< layout `depth` is in when the copy runs
        vk::Format depth_format;
        vk::Image motion; ///< RG16F camera + object motion, pixels, previous minus current
        u32 width, height; ///< render size
        std::array<float, 2> jitter;
        float frame_ms;
        float camera_near, camera_fov;
        float camera_far; ///< 3000 when the projection's is infinite
        /// DLSS: row-vector D3D clip-space matrices (CameraMotion::ClipTransforms): the
        /// projection, current to previous clip, and back. Without them a still camera.
        bool has_matrices;
        std::array<float, 16> view_to_clip, clip_to_prev_clip, prev_clip_to_clip;
        bool reset; ///< the upscaler restarted its history with this frame
    };
    /// The upscaler ran for this frame: copies the depth, notes the rest for PrepareFrame.
    void CaptureScene(const Scene& scene);
    /// A scene was captured and its HUD-less copy is not taken yet (the first UI draw).
    [[nodiscard]] bool WantsHudless() const noexcept {
        return pending.valid && !pending.hudless;
    }
    /// The finished scene before any UI pixel: R8G8B8A8 (unorm or sRGB) of `width` x `height`.
    void CaptureHudless(vk::Image image, vk::ImageLayout layout, vk::Format format, u32 width,
                        u32 height);

    // ---- Presenter side: GPU command thread ---------------------------------------------------

    struct Plan {
        bool generate = false; ///< Record() will be called for this frame
        bool present = false;  ///< the generated frames can be shown (not a history restart)
        bool hudless = false;  ///< a HUD-less image of the size of the display buffer exists
        u32 count = 0;         ///< frames to generate (1..MaxGenerated) when `generate`
        bool off = false;      ///< switched off: the presenter frees what it held for it
    };
    /// Called for every game frame at PrepareFrame (also with the switch off, cheaply):
    /// `width` x `height` is the size of the presented frame image, `format` its format,
    /// `display` the size of the image the host passes read (the display buffer).
    Plan PlanFrame(u32 width, u32 height, vk::Format format, bool hdr, vk::Extent2D display);
    /// Format of the generated images, valid after a Plan with generate set.
    [[nodiscard]] vk::Format OutputFormat() const noexcept {
        return output_format;
    }
    /// The presenter could not make the images for the generated frames (video memory full):
    /// generation stays off until the switch is turned off and on again.
    void OutOfMemory();
    /// The HUD-less copy as an image the host passes can sample, in `view_format`.
    struct Hudless {
        vk::ImageView view;
        vk::Extent2D size;
    };
    Hudless HudlessView(vk::Format view_format);
    [[nodiscard]] vk::Image HudlessImage() const noexcept {
        return vk::Image(hudless_raw);
    }

    struct Targets {
        vk::Image current;          ///< the presented frame, with the UI (General, shader readable)
        vk::ImageView current_view;
        vk::Image hudless;          ///< the same without the UI, or null
        vk::ImageView hudless_view;
        /// The generated frames, oldest first, to be written (any layout); `count` of them.
        std::array<vk::Image, MaxGenerated> outputs{};
        std::array<vk::ImageView, MaxGenerated> output_views{};
        u32 count = 0;
        vk::Format format;
        u32 width, height;
    };
    /// Records the provider's passes into `cmdbuf` (direct recording, after the frame's host
    /// passes). The outputs are in General afterwards. Returns the frames made, 0 when the
    /// provider refused (FSR 3.1: generation is off until the switch is cycled; DLSS: FSR 3.1
    /// takes over).
    u32 Record(vk::CommandBuffer cmdbuf, const Targets& targets);
    /// After the frame's submission: its tick, which the library's view recycling waits for.
    void EndFrame(u64 submission_tick);

    // ---- Swap thread --------------------------------------------------------------------------

    /// A game frame (not a redraw) is presented now. `generated`: that many generated frames go
    /// first. Returns the smoothed interval of the game frames in ms, which they are spread over.
    float OnPresent(u32 generated);
    /// Generated frames per game frame the display has room for now (BbFrameGenPace::Limiter),
    /// and whether 0 is the Limiter's pause; read by PlanFrame.
    void SetDisplayLimit(u32 limit, bool paused) noexcept {
        display_limit.store(limit, std::memory_order_relaxed);
        display_paused.store(paused, std::memory_order_relaxed);
    }
    /// Swap thread: a game frame went out after `generated` generated frames, `shown` of which
    /// the display had time to show (BbFrameGenPace::GeneratedShown). Feeds the menu, the FPS
    /// counter and the log with what really reaches the display. `starved`: generated frames
    /// whose spacing was cut because the game needed every presentation frame.
    void OnPresented(u32 generated, u32 shown, u32 starved);
    /// Swap thread: a game frame goes out with generation switched off.
    void OnSwitchedOff();
    /// Smoothed interval between game frames in ms, 0 until known.
    [[nodiscard]] float BaseIntervalMs() const noexcept {
        return base_interval_ms.load(std::memory_order_relaxed);
    }

private:
    struct Pending {
        bool valid = false;
        vk::Image motion;
        u32 width = 0, height = 0;
        vk::Format depth_format{};
        std::array<float, 2> jitter{};
        float frame_ms = 16.6f, camera_near = 0.05f, camera_fov = 1.0f, camera_far = 3000.0f;
        bool has_matrices = false;
        std::array<float, 16> view_to_clip{}, clip_to_prev_clip{}, prev_clip_to_clip{};
        bool reset = false;
        bool hudless = false;
        u32 hudless_width = 0, hudless_height = 0;
    };

    void EnsureDepthCopy(vk::Format format, u32 width, u32 height);
    void EnsureHudless(vk::Format format, u32 width, u32 height);
    /// The generated images' format for frames in `format`: it if storage works, else RGBA8.
    bool ChooseOutputFormat(vk::Format format);
    bool CreateContext(u32 width, u32 height, vk::Format format);
    void DestroyContext();
    /// DLSS Frame Generation's feature released, with no present beside it.
    void ReleaseDlssFrameGen(Dlss& dlss);
    /// Switched off: the contexts and the captured inputs are freed.
    void ReleaseAll();
    /// The provider of this frame: DLSS when asked for and it can run, else FSR 3.1.
    int ChooseProvider();
    bool RecordFsr(vk::CommandBuffer cmdbuf, const Targets& targets);
    u32 RecordDlss(vk::CommandBuffer cmdbuf, const Targets& targets);
    vk::ImageView MotionView(vk::Image image);
    /// Logs a change of state once ("off (HDR output)").
    void Report(std::string_view state);
    void PrintStats(std::chrono::steady_clock::time_point now);
    /// Why this frame's plan makes no frames (BbFrameGenPace::Idle), for the swap thread.
    void SetIdle(int idle) noexcept {
        plan_idle.store(idle, std::memory_order_relaxed);
    }

    const Instance& instance;
    Scheduler& scheduler;

    // Capture (command thread).
    Pending pending;
    Pending current; ///< the capture PrepareFrame is using
    VideoCore::UniqueImage depth_copy;
    vk::ImageView depth_copy_view{}; ///< depth aspect (DLSS)
    vk::Format depth_copy_format{};
    u32 depth_copy_width = 0, depth_copy_height = 0;
    VideoCore::UniqueImage hudless_raw;
    vk::Format hudless_format{};
    u32 hudless_width = 0, hudless_height = 0;
    struct View {
        vk::Format format;
        vk::ImageView view;
    };
    std::vector<View> hudless_views;
    vk::Image motion_image{}; ///< the upscaler's, which motion_view shows
    vk::ImageView motion_view{};

    // Context (command thread).
    FfxVkPortableFrameGenerationContext* context = nullptr;
    u32 context_width = 0, context_height = 0;
    vk::Format context_format{};
    vk::Format output_format{};
    bool failed = false;          ///< FSR 3.1 refused, or no memory; until the switch is cycled
    int failed_idle = BbFrameGenPace::IdleFailed; ///< which of the two
    bool dlss_failed = false;     ///< DLSS Frame Generation refused; FSR 3.1 runs instead
    bool was_requested = false;
    bool history_broken = true;   ///< the next frame restarts the interpolation history
    bool paused = false;          ///< no room on the display (BbFrameGenPace::Limiter)
    int provider = 0;             ///< of the last frame (BbFrameGenPace::Provider)
    bool depth_inverted = false;  ///< BB_DEPTH_INVERTED=1 (DLSS)
    u64 frame_id = 0;
    std::deque<u64> ticks;        ///< submissions of the last frames the providers recorded
    std::string reported;
    bool reset_frame = false;     ///< this frame restarts the interpolation history

    // Pacing and statistics (swap thread writes; the command thread reads the atomics).
    std::atomic<float> base_interval_ms{0.0f};
    std::atomic<u32> display_limit{MaxGenerated};
    std::atomic<bool> display_paused{false};
    std::atomic<int> plan_idle{BbFrameGenPace::IdleStarting};
    std::chrono::steady_clock::time_point last_present{};
    std::chrono::steady_clock::time_point stats_start{}, overlay_start{}, summary_start{};
    u32 stats_base = 0, stats_images = 0, stats_generated = 0, stats_shown = 0;
    BbFrameGenPace::Window overlay{}, summary{};
    BbFrameGenPace::StatusTracker tracker;
    std::atomic<u32> skipped{0};
};

} // namespace Vulkan
