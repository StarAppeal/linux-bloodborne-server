// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

// Line / SetLine: the file's lines as Load read them, and the changes the next Save writes.
std::mutex lines_mutex;
std::map<std::string, std::string> file_lines;
std::map<std::string, std::optional<std::string>> line_changes;

std::string Trimmed(std::string text) {
    text.erase(text.find_last_not_of(" \t") + 1);
    text.erase(0, text.find_first_not_of(" \t"));
    return text;
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "overlay_language") {
        // Not menu_language (en / ru): every save wrote that one, chosen or not.
        if (value == "auto") {
            v.menu_language_auto = true;
        } else if (const auto language = LanguageFromCode(value)) {
            v.menu_language_auto = false;
            v.menu_language = *language;
        } else {
            std::printf("Settings: unknown overlay_language '%s'\n", value.c_str());
        }
    } else if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "frame_generation") {
        v.frame_generation = i != 0;
    } else if (key == "frame_gen_provider") {
        v.frame_gen_provider = value == "dlss" ? FrameGenDlss : FrameGenFsr3;
    } else if (key == "frame_gen_multiplier") {
        v.frame_gen_multiplier = std::clamp(i, 2, 4);
    } else if (key == "frame_gen_fit_display") {
        v.frame_gen_fit_display = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "mute_unfocused") {
        v.mute_unfocused = i != 0;
    } else if (key == "fast_shader_math") {
        v.fast_shader_math = i != 0;
    } else if (key == "fov") {
        v.fov = std::clamp(i, FovMin, FovMax);
    } else if (key == "compass") {
        v.compass = i != 0;
    } else if (key == "bloodstains") {
        v.bloodstains = i != 0;
    } else if (key == "map_3d") {
        v.map_3d = std::clamp(i, 0, Map3dCount - 1);
    } else if (key == "screen_lighting") {
        for (int q = 0; q < ScreenLightingCount; ++q) {
            if (value == ScreenLightingNames[q]) {
                v.screen_lighting = q;
            }
        }
    } else if (key == "screen_lighting_ao") {
        v.screen_lighting_ao = Clamp(f, 0.0f, 1.0f);
    } else if (key == "screen_lighting_gi") {
        v.screen_lighting_gi = Clamp(f, 0.0f, 4.0f);
    } else if (key == "ray_traced_ao") {
        v.ray_traced_ao = i != 0;
    } else if (key == "map_fog") {
        v.map_fog = i != 0;
    } else if (key == "compass_position") {
        for (int p = 0; p < CompassPositionCount; ++p) {
            if (value == CompassPositionNames[p]) {
                v.compass_position = p;
            }
        }
    } else if (key == "fps_limit") {
        v.fps_limit = std::clamp(i, 0, 1000);
    } else if (key == "dlss_model") {
        v.dlss_model = 0;
        for (int m = 0; m < DlssModelCount; ++m) {
            if (value == DlssModelNames[m]) {
                v.dlss_model = DlssModels[m];
            }
        }
    } else if (key == "window_mode") {
        for (int m = 0; m < WindowModeCount; ++m) {
            if (value == WindowModeNames[m]) {
                v.window_mode = m;
            }
        }
    } else if (key == "hdr") {
        v.hdr = i != 0;
    } else if (key == "hdr_peak_nits") {
        v.hdr_peak_nits = value == "auto" || i == HdrAuto ? HdrAuto : std::clamp(i, 100, 10000);
    } else if (key == "hdr_paper_white") {
        v.hdr_paper_white = value == "auto" || i == HdrAuto ? HdrAuto : std::clamp(i, 50, 1000);
    } else if (key == "hdr_highlights") {
        v.hdr_highlights = Clamp(f, 0.0f, 1.0f);
    } else if (key == "hdr_scene") {
        v.hdr_scene = i != 0;
    } else if (key == "menu_pos") {
        float x = -1.0f, y = -1.0f;
        if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 1.0f && y >= 0.0f &&
            y <= 1.0f) {
            v.menu_x = x;
            v.menu_y = y;
        }
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

namespace {

struct LanguageInfo {
    const char* code;
    const char* name;
    std::vector<int> ps4_ids;
};

const std::vector<LanguageInfo>& Languages() {
    static const std::vector<LanguageInfo> languages = {
        {"en", "English", {1, 18}},
        {"ru", "Русский", {8}},
#define BB_MENU_LANG(name, code, own_name, ...) {code, own_name, {__VA_ARGS__}},
#include "bbport_menu_texts.inc"
#undef BB_MENU_LANG
    };
    return languages;
}

constexpr int TableLanguages = MenuLanguageCount - 2;
struct TableText {
    const char* english;
    const char* texts[TableLanguages];
};
constexpr TableText table_texts[] = {
#define BB_MENU_TEXT(english, ...) {english, __VA_ARGS__},
#include "bbport_menu_texts.inc"
#undef BB_MENU_TEXT
};

} // namespace

const char* Translate(MenuLanguage language, const char* english, const char* russian) {
    if (language == MenuLanguage::English) {
        return english;
    }
    if (language == MenuLanguage::Russian) {
        return russian;
    }
    static const std::unordered_map<std::string_view, const TableText*> by_english = [] {
        std::unordered_map<std::string_view, const TableText*> map;
        for (const TableText& text : table_texts) {
            map.emplace(text.english, &text);
        }
        return map;
    }();
    const auto found = by_english.find(english);
    const int column = int(language) - 2;
    if (found == by_english.end() || column < 0 || column >= TableLanguages) {
        return english;
    }
    const char* text = found->second->texts[column];
    return text ? text : english;
}

const char* MenuText(const char* english, const char* russian) {
    return Translate(Get().menu_language, english, russian);
}

MenuLanguage LanguageFromPs4(int id) {
    const auto& languages = Languages();
    for (std::size_t l = 0; l < languages.size(); ++l) {
        if (std::ranges::find(languages[l].ps4_ids, id) != languages[l].ps4_ids.end()) {
            return MenuLanguage(l);
        }
    }
    return MenuLanguage::English;
}

MenuLanguage GameLanguage() {
    // runtime_services.c answers the game's language query from the same variable.
    const char* id = std::getenv("BB_LANGUAGE");
    return LanguageFromPs4(id && *id ? std::atoi(id) : 1);
}

const char* LanguageCode(MenuLanguage language) {
    return Languages()[std::size_t(language)].code;
}

const char* LanguageName(MenuLanguage language) {
    return Languages()[std::size_t(language)].name;
}

std::optional<MenuLanguage> LanguageFromCode(std::string_view code) {
    const auto& languages = Languages();
    for (std::size_t l = 0; l < languages.size(); ++l) {
        if (code == languages[l].code) {
            return MenuLanguage(l);
        }
    }
    return std::nullopt;
}

void SetMenuLanguage(std::optional<MenuLanguage> language) {
    auto& v = Get();
    v.menu_language_auto = !language;
    v.menu_language = language ? *language : GameLanguage();
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    // The menus speak the game's language until one is chosen in them (overlay_language).
    v.menu_language_auto = true;
    bool file_fps_limit = false;
    bool file_window_mode = false;
    v.fps_limit = 0;
    v.window_mode = Windowed;
    v.dlss_model = 0;
    {
        std::scoped_lock lock{lines_mutex};
        file_lines.clear();
        line_changes.clear();
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            {
                std::scoped_lock lock{lines_mutex};
                file_lines[Trimmed(text.substr(0, eq))] = Trimmed(text.substr(eq + 1));
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
            file_fps_limit = file_fps_limit || text.substr(0, eq) == "fps_limit";
            file_window_mode = file_window_mode || text.substr(0, eq) == "window_mode";
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_FRAME_GEN", "frame_generation"},       {"BB_FRAME_GEN_PROVIDER", "frame_gen_provider"},
        {"BB_FRAME_GEN_MULTIPLIER", "frame_gen_multiplier"},
        {"BB_FRAME_GEN_FIT_DISPLAY", "frame_gen_fit_display"},
        {"BB_MAP_3D", "map_3d"},                    {"BB_MAP_FOG", "map_fog"}, // the rig's map scenarios
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    // The launcher's frame cap until the game menu has set one (it is kept in this file).
    if (const char* value = std::getenv("BB_FPS_LIMIT"); value && !file_fps_limit) {
        Set(v, "fps_limit", value);
    }
    // The launcher's fullscreen switch until the game menu has picked a window mode.
    if (const char* value = std::getenv("BB_FULLSCREEN"); value && !file_window_mode) {
        v.window_mode = value[0] == '1' ? Borderless : Windowed;
    }
    if (const char* value = std::getenv("BB_WINDOW_MODE"); value && *value) {
        Set(v, "window_mode", value);
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_fast_shader_math = v.fast_shader_math;
    v.startup_fov = v.fov;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    if (v.menu_language_auto) {
        v.menu_language = GameLanguage();
    }
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "the DLSS bridge or NVIDIA's DLSS library is missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureXessSupport(bool available, const char* problem) {
    auto& v = Get();
    v.xess_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.xess_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerXess && !available) {
        std::printf("Upscaler: XeSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "libxess.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

int EffectiveFrameLimit(int fps_limit, bool uncapped_vblank) {
    return uncapped_vblank ? std::max(fps_limit, 0) : 0;
}

bool FrameRateLocked(const char* online) {
    return online != nullptr && std::string_view{online} == "1";
}

bool FrameRateLocked() {
    return FrameRateLocked(std::getenv("BB_ONLINE"));
}

int FpsLimitIndex(int limit) {
    int index = 0;
    for (int i = 0; i < FpsLimitCount; ++i) {
        if (FpsLimits[i] <= limit) {
            index = i;
        }
    }
    return index;
}

int DlssModelIndex(int preset) {
    for (int i = 0; i < DlssModelCount; ++i) {
        if (DlssModels[i] == preset) {
            return i;
        }
    }
    return 0;
}

namespace {
template <int N>
int Nearest(const int (&choices)[N], int value, int first = 0) {
    int best = first;
    for (int i = first + 1; i < N; ++i) {
        if (std::abs(choices[i] - value) < std::abs(choices[best] - value)) {
            best = i;
        }
    }
    return best;
}

// The picked peak, else the display's, else 1000.
float PeakBeforeWhite() {
    const auto& v = Get();
    const int picked = v.hdr_peak_nits.load(std::memory_order_relaxed);
    const float display = v.hdr_display_peak.load(std::memory_order_relaxed);
    return picked != HdrAuto ? float(picked) : display > 0.0f ? display : 1000.0f;
}

// Auto is the first choice; a brightness is the nearest of the others.
template <int N>
int NearestHdr(const int (&choices)[N], int nits) {
    return nits == HdrAuto ? 0 : Nearest(choices, nits, 1);
}
} // namespace

int HdrPeakIndex(int nits) {
    return NearestHdr(HdrPeakNits, nits);
}

int HdrPaperWhiteIndex(int nits) {
    return NearestHdr(HdrPaperWhiteNits, nits);
}

float HdrPaperWhite() {
    const auto& v = Get();
    const int picked = v.hdr_paper_white.load(std::memory_order_relaxed);
    const float display = v.hdr_display_white.load(std::memory_order_relaxed);
    if (picked != HdrAuto) {
        return float(picked);
    }
    // At most half the peak: lights get a stop above white. Bloodborne's 16-bit scene holds
    // nothing brighter than its SDR picture, so that room is all HDR adds (Kaniel 2026-10-19: a
    // quarter, 250 nits on his display, was too dark; the desktop's 480 fits under his 1000).
    return display > 0.0f ? std::min(display, PeakBeforeWhite() / 2.0f) : 200.0f;
}

float HdrPeak() {
    return std::max(PeakBeforeWhite(), HdrPaperWhite());
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    // The keys the menu writes, in this order; the rest of the file stays as it is: the
    // launcher's controls (key.* / pad.*) and its other keys, comments.
    std::vector<std::pair<std::string, std::string>> keys;
    const auto put = [&](const char* key, std::string value) { keys.emplace_back(key, std::move(value)); };
    const auto fixed = [](float value, int digits) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return std::string(text);
    };
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    put("overlay_language", v.menu_language_auto ? "auto" : LanguageCode(v.menu_language));
    put("upscaler", UpscalerName(v.upscaler));
    put("preset", std::to_string(v.preset.load()));
    put("sharpen", flag(v.sharpen));
    put("sharpness", fixed(v.sharpness, 2));
    put("jitter", flag(v.jitter));
    put("reactive", flag(v.reactive));
    put("object_motion", flag(v.object_motion));
    put("frame_generation", flag(v.frame_generation));
    put("frame_gen_provider", v.frame_gen_provider == FrameGenDlss ? "dlss" : "fsr3");
    put("frame_gen_multiplier", std::to_string(std::clamp(v.frame_gen_multiplier.load(), 2, 4)));
    put("frame_gen_fit_display", flag(v.frame_gen_fit_display));
    put("reactive_scale", fixed(v.reactive_scale, 2));
    put("reactive_threshold", fixed(v.reactive_threshold, 2));
    put("reactive_max", fixed(v.reactive_max, 2));
    put("debug_view", std::to_string(v.debug_view.load()));
    put("show_fps", flag(v.show_fps));
    put("mute_unfocused", flag(v.mute_unfocused));
    put("fast_shader_math", flag(v.fast_shader_math));
    // Read by run.py at start (fov.py).
    put("fov", std::to_string(std::clamp(v.fov.load(), FovMin, FovMax)));
    put("compass", flag(v.compass));
    put("bloodstains", flag(v.bloodstains));
    put("compass_position",
        CompassPositionNames[std::clamp(v.compass_position.load(), 0, CompassPositionCount - 1)]);
    put("map_3d", std::to_string(std::clamp(v.map_3d.load(), 0, Map3dCount - 1)));
    put("screen_lighting",
        ScreenLightingNames[std::clamp(v.screen_lighting.load(), 0, ScreenLightingCount - 1)]);
    put("screen_lighting_ao", fixed(v.screen_lighting_ao, 2));
    put("screen_lighting_gi", fixed(v.screen_lighting_gi, 2));
    put("ray_traced_ao", flag(v.ray_traced_ao));
    put("map_fog", flag(v.map_fog));
    put("fps_limit", std::to_string(v.fps_limit.load()));
    put("dlss_model", DlssModelNames[DlssModelIndex(v.dlss_model)]);
    put("window_mode", WindowModeNames[std::clamp(v.window_mode.load(), 0, WindowModeCount - 1)]);
    put("hdr", flag(v.hdr));
    const auto nits = [](int value) { return value == HdrAuto ? std::string("auto") : std::to_string(value); };
    put("hdr_peak_nits", nits(v.hdr_peak_nits.load()));
    put("hdr_paper_white", nits(v.hdr_paper_white.load()));
    put("hdr_highlights", fixed(v.hdr_highlights, 2));
    put("hdr_scene", flag(v.hdr_scene));
    put("fsr4_auto_exposure", flag(v.fsr4_auto_exposure));
    put("fsr4_invert_jitter", flag(v.fsr4_invert_jitter));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        put(Effects[e].key, flag(v.effects[e]));
    }
    put("model_lod", std::to_string(v.model_lod.load()));
    put("output_res", std::to_string(OutputWidths[v.output_res]) + "x" +
                          std::to_string(OutputHeights[v.output_res]));
    // Read by run.sh at start.
    put("live_resolution", v.live_resolution < 0 ? "auto" : flag(v.live_resolution != 0));
    if (v.menu_x >= 0.0f && v.menu_y >= 0.0f) {
        put("menu_pos", fixed(v.menu_x, 4) + "," + fixed(v.menu_y, 4));
    }

    std::map<std::string, std::optional<std::string>> changes;
    {
        std::scoped_lock lock{lines_mutex};
        changes = line_changes;
    }
    std::string out;
    bool had_lines = false;
    std::vector<bool> written(keys.size());
    std::vector<std::string> changed_written;
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            had_lines = true;
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
                std::string key = text.substr(0, eq);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                const auto it = std::find_if(keys.begin(), keys.end(),
                                             [&](const auto& k) { return k.first == key; });
                if (it != keys.end()) {
                    out += it->first + "=" + it->second + "\n";
                    written[size_t(it - keys.begin())] = true;
                    continue;
                }
                if (const auto change = changes.find(key); change != changes.end()) {
                    if (change->second) { // the first line of a key gets the value, repeats go
                        if (std::find(changed_written.begin(), changed_written.end(), key) == changed_written.end()) {
                            out += key + "=" + *change->second + "\n";
                            changed_written.push_back(key);
                        }
                    }
                    continue;
                }
            }
            out += text + "\n";
        }
        std::fclose(file);
    }
    if (!had_lines) {
        out = "# bbport settings (in-game menu: Insert / L3+R3)\n";
    }
    for (size_t k = 0; k < keys.size(); ++k) {
        if (!written[k]) {
            out += keys[k].first + "=" + keys[k].second + "\n";
        }
    }
    for (const auto& [key, value] : changes) {
        if (value && std::find(changed_written.begin(), changed_written.end(), key) == changed_written.end()) {
            out += key + "=" + *value + "\n";
        }
    }
    // Replaced whole: a crash while writing leaves the old file.
    const std::string temporary = std::string(Path()) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    bool ok = file && std::fwrite(out.data(), 1, out.size(), file) == out.size();
    ok = file && std::fclose(file) == 0 && ok;
#ifdef _WIN32
    // bbport (Windows): rename() does not replace an existing file there.
    const bool replaced =
        ok && MoveFileExA(temporary.c_str(), Path(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const bool replaced = ok && std::rename(temporary.c_str(), Path()) == 0;
#endif
    if (!replaced) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::scoped_lock lock{lines_mutex};
    for (const auto& [key, value] : changes) {
        if (value) {
            file_lines[key] = *value;
        } else {
            file_lines.erase(key);
        }
        if (const auto now = line_changes.find(key); now != line_changes.end() && now->second == value) {
            line_changes.erase(now); // unless it changed again meanwhile
        }
    }
}

std::optional<std::string> Line(const std::string& key) {
    std::scoped_lock lock{lines_mutex};
    if (const auto change = line_changes.find(key); change != line_changes.end()) {
        return change->second;
    }
    if (const auto line = file_lines.find(key); line != file_lines.end()) {
        return line->second;
    }
    return std::nullopt;
}

void SetLine(const std::string& key, std::optional<std::string> value) {
    std::scoped_lock lock{lines_mutex};
    line_changes[key] = std::move(value);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss", "xess"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
