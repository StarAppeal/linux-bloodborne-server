// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <atomic>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerXess = 6, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
/// Frame generation's provider (bbport.ini frame_gen_provider): FSR 3.1 on any GPU, DLSS Frame
/// Generation on a GeForce RTX 40 (2x) or RTX 50 (up to 4x). BbFrameGenPace::Provider values.
enum FrameGenProvider : int { FrameGenFsr3 = 1, FrameGenDlss = 2 };
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// The menus' languages: every one the game has (bbport_menu_texts.inc, written by
/// scripts/menu_texts.py). English and Russian live in the sources, the rest in the table.
enum class MenuLanguage {
    English,
    Russian,
#define BB_MENU_LANG(name, ...) name,
#include "bbport_menu_texts.inc"
#undef BB_MENU_LANG
};
inline constexpr int MenuLanguageCount = int(MenuLanguage::Tr) + 1;

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera ({Cross} + {L3})", "Свободная камера ({Cross} + {L3})", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
    {"cheat_no_death", "Cheat: no death (HP stays at 1 or more)", "Чит: бессмертие (не ниже 1 HP)", false},
    {"cheat_stealth", "Cheat: enemies do not see you", "Чит: враги не замечают", false},
    {"cheat_silent", "Cheat: enemies do not hear you", "Чит: враги не слышат", false},
    {"cheat_rally_no_decay", "Cheat: Rally does not fade", "Чит: Rally не угасает", false},
    {"cheat_enemy_control", "Cheat: control an enemy ({R3} / {L3})", "Чит: управление врагом ({R3} / {L3})", false},
    {"tweak_no_rally", "No Rally (HP regain)", "Без Rally (возврата HP)", false},
    {"tweak_camera_distance", "Camera further away", "Камера дальше", false},
    {"tweak_no_camera_rotation", "No automatic camera rotation", "Без автоповорота камеры", false},
    {"tweak_easy_run", "Run with less stick tilt", "Бег с меньшим наклоном стика", false},
    {"tweak_ragdoll", "Dark Souls-style body physics", "Физика тел как в Dark Souls", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size
/// The frame caps the game menu offers (FPS limit), 0 = none.
inline constexpr int FpsLimits[] = {0, 30, 60, 90, 120, 144, 165, 240};
inline constexpr int FpsLimitCount = int(sizeof(FpsLimits) / sizeof(FpsLimits[0]));
/// The DLSS models the game menu offers: NVIDIA's NVSDK_NGX_DLSS_Hint_Render_Preset values (0 its
/// default per quality mode; K transformer, best quality; L and M lighter, NVIDIA's own picks for
/// Ultra Performance and Performance; J like K, a little less ghosting, more flicker).
inline constexpr int DlssModels[] = {0, 11, 12, 13, 10};
inline constexpr const char* DlssModelNames[] = {"default", "K", "L", "M", "J"};
inline constexpr int DlssModelCount = int(sizeof(DlssModels) / sizeof(DlssModels[0]));
/// How the game's window sits on the screen (bbport.ini window_mode; else the launcher's
/// BB_FULLSCREEN: 1 borderless, 0 windowed). Live.
enum WindowMode : int { Windowed = 0, Borderless = 1, ExclusiveFullscreen = 2, WindowModeCount };
inline constexpr const char* WindowModeNames[] = {"windowed", "borderless", "fullscreen"};
enum Map3d { Map3dOff, Map3dPrerendered, Map3dLive, Map3dCount };
/// Kaniel 2026-10-17, the teammate compass (bbport.ini compass_position): a strip at the top or
/// the bottom of the screen, or a ring around its middle.
enum CompassPosition : int { CompassTop = 0, CompassBottom = 1, CompassReticle = 2, CompassPositionCount };
inline constexpr const char* CompassPositionNames[] = {"top", "bottom", "reticle"};
/// Kaniel 2026-10-19: screen-space lighting (vk_screen_lighting.h; bbport.ini screen_lighting):
/// ambient occlusion and one bounce of indirect light, traced at half (low) or full (high) size.
enum ScreenLighting : int {
    ScreenLightingOff = 0,
    ScreenLightingLow = 1,
    ScreenLightingHigh = 2,
    ScreenLightingCount
};
inline constexpr const char* ScreenLightingNames[] = {"off", "low", "high"};
/// HDR output (HDR10, ST.2084 PQ): the display's peak brightness and the brightness of SDR
/// white (the game's UI, paper white) the menus offer, in nits. HdrAuto (first): the display's
/// own values (HdrPeak, HdrPaperWhite); 480 is Windows' brightest SDR content brightness.
inline constexpr int HdrAuto = 0;
inline constexpr int HdrPeakNits[] = {HdrAuto, 400, 600, 800, 1000, 1500, 2000, 4000};
inline constexpr int HdrPeakCount = int(sizeof(HdrPeakNits) / sizeof(HdrPeakNits[0]));
inline constexpr int HdrPaperWhiteNits[] = {HdrAuto, 80, 100, 150, 200, 250, 300, 400, 480};
inline constexpr int HdrPaperWhiteCount = int(sizeof(HdrPaperWhiteNits) / sizeof(HdrPaperWhiteNits[0]));

/// Field of view (fov.py): the game's camera has 43 degrees.
inline constexpr int FovDefault = 43, FovMin = 30, FovMax = 90;

struct Values {
    /// The menus' language: the game's (menu_language_auto) or the one chosen in the Insert menu.
    std::atomic<MenuLanguage> menu_language{MenuLanguage::English};
    std::atomic<bool> menu_language_auto{true};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// Silence the game while its window does not have focus (bbport.ini mute_unfocused,
    /// bbgpu_audio_muted). From Supermedo's bbport (b75e811).
    std::atomic<bool> mute_unfocused{false};
    /// shadPS4's fast_shader_math (#5455; bbport.ini fast_shader_math, off by default): pixel and
    /// compute shaders may fuse float operations and drop the Inf/NaN/signed-zero rules. The
    /// shaders are compiled with startup_fast_shader_math: a change applies after a restart.
    std::atomic<bool> fast_shader_math{false};
    /// Vertical field of view in degrees, FovDefault the game's (bbport.ini fov; run.py makes
    /// the camera table with fov.py at start).
    std::atomic<int> fov{FovDefault};
    /// The teammate compass (bbport.ini compass, on by default) and where it is (CompassPosition).
    std::atomic<bool> compass{true};
    std::atomic<int> compass_position{CompassTop};
    /// Kaniel 2026-10-20: the party's bloodstains on the compass and the map (bbport.ini
    /// bloodstains, on by default).
    std::atomic<bool> bloodstains{true};
    /// Kaniel 2026-10-19 ("make it opt in, so low vram cards will not prerender it", "an opt in
    /// option to view rendered in real time 3d map"): the map screen's 3D view (bbport.ini map_3d,
    /// Map3d*, off by default): prerendered pictures, or the models drawn live on the GPU.
    std::atomic<int> map_3d{Map3dOff};
    /// Screen-space lighting (ScreenLighting, off by default) and how strong its ambient
    /// occlusion (0..1, screen_lighting_ao) and bounced light (0..2, screen_lighting_gi) are. Live.
    std::atomic<int> screen_lighting{ScreenLightingOff};
    /// Kaniel 2026-10-21 ("not just darker"): Bloodborne's light is all lamps (no ambient term), so
    /// the shadows only touch contacts (0.3) and the bounced lamp light is strong (2, up to 4).
    std::atomic<float> screen_lighting_ao{0.3f};
    std::atomic<float> screen_lighting_gi{2.0f};
    /// Kaniel 2026-10-21 ("continue with ray tracing"): the screen lighting's shadows also traced
    /// against the map's own models (vk_ray_occlusion.h; bbport.ini ray_traced_ao, off by default).
    /// Vulkan ray query is switched on at start, so turning it on needs a restart.
    std::atomic<bool> ray_traced_ao{false};
    /// Kaniel 2026-10-19 ("a setting to remove fog of war"): the map screen hides what this
    /// character has not walked near (bbport.ini map_fog, on by default); off shows the whole map.
    std::atomic<bool> map_fog{true};
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (gpu/dlss_bridge, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};
    /// XeSS (libxess.dll, any GPU with DP4a) is ready, or why not (null before the device exists).
    std::atomic<bool> xess_supported{false};
    std::atomic<const char*> xess_problem{nullptr};
    /// FSR 3.1 frame generation (vk_frame_generation.h): one interpolated frame between every
    /// two game frames, on top of the selected upscaler. Off by default (bbport.ini
    /// frame_generation, BB_FRAME_GEN).
    std::atomic<bool> frame_generation{false};
    /// Its provider (FrameGenProvider; bbport.ini frame_gen_provider fsr3|dlss,
    /// BB_FRAME_GEN_PROVIDER) and the frames shown per game frame with DLSS (2..4;
    /// frame_gen_multiplier, BB_FRAME_GEN_MULTIPLIER; FSR 3.1 makes one). Live.
    std::atomic<int> frame_gen_provider{FrameGenFsr3};
    std::atomic<int> frame_gen_multiplier{2};
    /// At most the frames the display shows between two of the game's (an uncapped game then
    /// gets fewer generated frames); off: the multiplier as chosen (frame_gen_fit_display,
    /// BB_FRAME_GEN_FIT_DISPLAY). With V-Sync (FIFO) the frames are always fitted.
    std::atomic<bool> frame_gen_fit_display{true};
    /// Set by the renderer: the multiplier DLSS Frame Generation reaches on this GPU (1 without
    /// it), why a provider stopped (or null), and what runs now: generated frames per game
    /// frame (0 paused) and the display's refresh rate.
    std::atomic<int> dlssg_max_multiplier{1};
    std::atomic<const char*> frame_gen_problem{nullptr};
    std::atomic<int> frame_gen_now{0};
    std::atomic<int> frame_gen_display_hz{0};
    /// Frame cap of the unlocked frame rate, 0 none (bbport.ini fps_limit, the game menu; else
    /// the launcher's BB_FPS_LIMIT). Live.
    std::atomic<int> fps_limit{0};
    /// DLSS render preset (DlssModels; bbport.ini dlss_model), 0 NVIDIA's default. Live.
    std::atomic<int> dlss_model{0};
    /// What the presenter measured (overlay): interpolated frames are being shown, game frames
    /// and presented images a second.
    std::atomic<bool> fg_active{false};
    /// Window mode (WindowMode). Applied by the window thread whenever it changes.
    std::atomic<int> window_mode{Windowed};
    /// HDR output (bbport.ini hdr): used when the display supports it (hdr_supported). Live.
    std::atomic<bool> hdr{false};
    /// Nits, or HdrAuto (bbport.ini "auto"): the display's (hdr_display_*).
    std::atomic<int> hdr_peak_nits{HdrAuto};
    std::atomic<int> hdr_paper_white{HdrAuto};
    /// Set by the window: the desktop's SDR white (Windows' SDR content brightness) and the
    /// display's peak, in nits; 0 while unknown.
    std::atomic<float> hdr_display_white{0.0f}, hdr_display_peak{0.0f};
    /// Highlight expansion 0..1 (host_shaders/hdr_highlight.glsl): 0.5 takes the game's lights (and
    /// white) to the peak brightness, higher values lower the knee (0: the SDR picture only).
    std::atomic<float> hdr_highlights{0.5f};
    /// HDR gain map per frame (vk_scene_hdr.h): the expansion leaves the game's HUD alone and
    /// takes extra brightness from the 16-bit scene where it has more; off: the whole SDR frame
    /// is expanded.
    std::atomic<bool> hdr_scene{true};
    /// Set by the renderer: the swapchain offers HDR10 and the display has HDR switched on.
    std::atomic<bool> hdr_supported{false};
    std::atomic<float> fg_base_fps{0.0f}, fg_presented_fps{0.0f};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    bool startup_fast_shader_math = false;
    int startup_fov = FovDefault;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();
/// Localized overlay text in the menus' language: the Russian given, the table's for the
/// others, the English when the table lacks the text.
const char* MenuText(const char* english, const char* russian);
const char* Translate(MenuLanguage language, const char* english, const char* russian);
/// The game's language: BB_LANGUAGE, the PS4 system language the game reads (English without it).
MenuLanguage GameLanguage();
MenuLanguage LanguageFromPs4(int id);
/// bbport.ini's code ("en", "ru", "zh_TW", ...) and the language's own name.
const char* LanguageCode(MenuLanguage language);
const char* LanguageName(MenuLanguage language);
std::optional<MenuLanguage> LanguageFromCode(std::string_view code);
/// A language chosen in the menu, or none: the game's again.
void SetMenuLanguage(std::optional<MenuLanguage> language);

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// After device creation: XeSS availability; an XeSS setting falls back to FSR 3.1 without it.
void ConfigureXessSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// bbport.ini lines the menu does not keep in Values: the controls runtime_pad.c and the icon step
/// read (key.<input>, pad.<input>, mouse_*, button_icons). Line: the value in the file as Load read
/// it, or as changed since; nullopt: no line. SetLine: the next Save replaces the line in place or
/// appends it, or with nullopt removes it (the default applies again). Any thread.
std::optional<std::string> Line(const std::string& key);
void SetLine(const std::string& key, std::optional<std::string> value);

/// The cap the present thread applies: the menu's with the unlocked frame rate (vblank above
/// 120 Hz), none with the game's own fixed 30/60 timestep.
int EffectiveFrameLimit(int fps_limit, bool uncapped_vblank);
/// The FPS limit row's choice for a cap: the largest choice not above it.
int FpsLimitIndex(int limit);
/// Whether the frame rate is locked to the fixed 60 FPS mode: online (co-op) play, BB_ONLINE
/// `online` "1" (run.py launches it at 60; a host above 60 made the guests see everyone freeze
/// and fly). The menus' FPS limit rows are then disabled and no limit applies.
bool FrameRateLocked(const char* online);
bool FrameRateLocked();
/// The DLSS model row's choice for a preset value, 0 (NVIDIA's default) if not offered.
int DlssModelIndex(int preset);
/// The HDR rows' choices for a brightness: the nearest offered value.
int HdrPeakIndex(int nits);
int HdrPaperWhiteIndex(int nits);
/// The HDR output's paper white and peak in nits: the picked brightness, else the display's
/// (auto white at most half the peak), else 200 and 1000. The peak is never below the
/// white.
float HdrPaperWhite();
float HdrPeak();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
