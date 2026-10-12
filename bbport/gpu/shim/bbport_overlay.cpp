// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <functional>
#include <string>
#include <string_view>
#include <optional>
#include <map>
#include <vector>

#include <SDL3/SDL.h>
#include "../bbcoop.h"
#include "bbport_game_art.h"
#include "bbport_party_icons.h"
#include "bbport_party_layout.h"
#include "bbport_save_editor.h"
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
// DejaVu Serif (same license): the settings menu, styled like the game's own menus.
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_serif_ttf\n"
    "bb_serif_ttf:\n"
    ".incbin \"" BB_SERIF_FONT_PATH "\"\n"
    ".global bb_serif_ttf_end\n"
    "bb_serif_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_serif_ttf\n"
    ".global bb_serif_ttf\n"
    "bb_serif_ttf:\n"
    ".incbin \"" BB_SERIF_FONT_PATH "\"\n"
    ".hidden bb_serif_ttf_end\n"
    ".global bb_serif_ttf_end\n"
    "bb_serif_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_serif_ttf[];
extern "C" const unsigned char bb_serif_ttf_end[];
// Party death's icons (art/party_icons.bin, drawn for the port: shim/bbport_party_icons.h).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_party_icons\n"
    "bb_party_icons:\n"
    ".incbin \"" BB_PARTY_ICONS_PATH "\"\n"
    ".global bb_party_icons_end\n"
    "bb_party_icons_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_party_icons\n"
    ".global bb_party_icons\n"
    "bb_party_icons:\n"
    ".incbin \"" BB_PARTY_ICONS_PATH "\"\n"
    ".hidden bb_party_icons_end\n"
    ".global bb_party_icons_end\n"
    "bb_party_icons_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_party_icons[];
extern "C" const unsigned char bb_party_icons_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)
// The controls (runtime_pad.c, declared in src/runtime.h): the Controls tab.
extern "C" int runtime_pad_input_count(void);
extern "C" const char* runtime_pad_input_name(int input);
extern "C" int runtime_pad_input_takes_pad(int input);
extern "C" void runtime_pad_binding_text(int input, int pad, char* out, size_t size);
extern "C" void runtime_pad_reload_controls(void);

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
// L3+R3: a tap toggles the settings menu (on release), a hold opens the quit box (Tick).
constexpr auto quit_hold = std::chrono::milliseconds(1500);
std::chrono::steady_clock::time_point combo_since{};
bool combo_held = false, combo_fired = false;
std::atomic<bool> quit_open{false};
// Controls tab: "press a key" for one input. The menu starts it; the window thread takes the next
// key, mouse button, wheel step or gamepad button (CaptureEvent) and the menu writes it to
// bbport.ini. Both sides hold imgui_mutex.
struct Capture {
    int input = -1; // waiting for a press for this input; -1 none
    bool pad_ok = false; // the input takes gamepad buttons (the movement and camera keys do not)
    std::chrono::steady_clock::time_point since{};
    bool done = false; // a press came: kind ("key" or "pad") and name; an empty name unbinds
    std::string kind, name;
    SDL_Scancode modifier = SDL_SCANCODE_UNKNOWN; // a modifier key went down alone: bound if it comes up alone
};
Capture capture;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;
ImFont* sans_font = nullptr;
ImFont* serif_font = nullptr;
bool focus_request = true; // the menu was opened: its first row takes the focus

// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

// The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at the
// next motion: mouse motion is not passed on while the menu is closed.
void SyncMousePos() {
    if (SDL_Window* window = SDL_GetMouseFocus()) {
        float x = 0.0f, y = 0.0f;
        SDL_GetMouseState(&x, &y);
        const float density = PixelDensity(SDL_GetWindowID(window));
        ImGui::GetIO().AddMousePosEvent(x * density, y * density);
    }
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    if (value) {
        focus_request = true;
        SyncMousePos();
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
        runtime_pad_reload_controls(); // the mouse settings of the Controls tab
    }
    if (!value) {
        capture = {};
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

// The widget runs before Store reads v: argument evaluation order is unspecified, and clang on
// Windows copied v before the widget changed it, so every click stored the old value
// (yumlevi/bloodborne_pc c777a15).
void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The settings menu, laid out like a PC game's options screen in the game's own style: the game
// dimmed behind, a centred panel framed by ornament lines, tabs (L1/R1, Q/E, mouse), one setting
// per row ("name  < value >": left/right change it, Cross/Enter steps it, the mouse clicks either
// side), the focused row's description at the bottom and the controls in the footer.
namespace Ui {

float S(float v) {
    return v * base_scale;
}
const char* T(const char* english, const char* russian) {
    return BbSettings::MenuText(english, russian);
}
ImU32 Gold(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.80f, 0.68f, 0.46f, alpha));
}
ImU32 Bright(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.96f, 0.88f, 0.70f, alpha));
}
ImU32 Plain(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.80f, 0.77f, 0.70f, alpha));
}
ImU32 Dim(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.48f, 0.46f, 0.42f, alpha));
}

/// The game's dialog border: a line fading out to both ends, a small diamond in the middle.
void Ornament(ImDrawList* draw, float x0, float x1, float y) {
    const float mid = (x0 + x1) * 0.5f, t = S(1.2f), d = S(5.0f);
    draw->AddRectFilledMultiColor(ImVec2(x0, y - t * 0.5f), ImVec2(mid, y + t * 0.5f), Gold(0.0f),
                                  Gold(0.85f), Gold(0.85f), Gold(0.0f));
    draw->AddRectFilledMultiColor(ImVec2(mid, y - t * 0.5f), ImVec2(x1, y + t * 0.5f), Gold(0.85f),
                                  Gold(0.0f), Gold(0.0f), Gold(0.85f));
    draw->AddQuadFilled(ImVec2(mid, y - d), ImVec2(mid + d, y), ImVec2(mid, y + d),
                        ImVec2(mid - d, y), Gold(0.95f));
    draw->AddQuadFilled(ImVec2(mid - d * 3.0f, y - d * 0.5f), ImVec2(mid - d * 2.5f, y),
                        ImVec2(mid - d * 3.0f, y + d * 0.5f), ImVec2(mid - d * 3.5f, y), Gold(0.7f));
    draw->AddQuadFilled(ImVec2(mid + d * 3.0f, y - d * 0.5f), ImVec2(mid + d * 3.5f, y),
                        ImVec2(mid + d * 3.0f, y + d * 0.5f), ImVec2(mid + d * 2.5f, y), Gold(0.7f));
}

// Focus is the menu's own, as in a game: a row index moved by up/down to the next enabled row
// (ImGui's navigation did not reach these rows), set by the mouse, scrolled into view.
struct State {
    int tab = 0;
    bool focus_first = true;
    int focus = 0;                   ///< the focused row of the current tab
    int move = 0;                    ///< -1 / +1: up / down pressed this frame
    bool activate = false;           ///< Cross / Enter pressed this frame
    bool scroll = false;             ///< bring the focused row into view
    int rows = 0;                    ///< rows drawn this frame
    std::vector<bool> enabled;       ///< per row drawn this frame
    const char* hint = nullptr;      ///< the focused row's description
    const char* next_hint = nullptr; ///< collected while drawing this frame
};
State state;

/// One setting row. Returns -1 / +1 when the value is to step back / forward (keys, Cross, a
/// click on either side of the value), 0 otherwise. A slider row also takes a drag: `drag`
/// receives the fraction under the mouse.
int Row(const char* id, const char* label, const char* value, const char* hint, bool enabled,
        float slider = -1.0f, float* drag = nullptr) {
    ImGui::PushID(id);
    const int index = state.rows++;
    state.enabled.push_back(enabled);
    const float width = ImGui::GetContentRegionAvail().x, height = S(44.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGui::EndDisabled();
    if (enabled && hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f ||
                               pressed)) {
        state.focus = index;
    }
    const bool focused = enabled && index == state.focus;
    if (focused && state.scroll) {
        ImGui::SetScrollHereY(0.5f);
        state.scroll = false;
    }
    if (focused && hint) {
        state.next_hint = hint;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float mid_y = at.y + height * 0.5f;
    if (focused || hovered) {
        // The game's highlight: a glow strongest under the text, fading to the sides.
        const float a = focused ? 0.22f : 0.10f;
        const float x0 = at.x, x1 = at.x + width, xm = at.x + width * 0.35f;
        draw->AddRectFilledMultiColor(ImVec2(x0, at.y), ImVec2(xm, at.y + height), Gold(0.0f),
                                      Gold(a), Gold(a), Gold(0.0f));
        draw->AddRectFilledMultiColor(ImVec2(xm, at.y), ImVec2(x1, at.y + height), Gold(a),
                                      Gold(0.0f), Gold(0.0f), Gold(a));
        if (focused) {
            draw->AddLine(ImVec2(x0 + width * 0.1f, at.y + height - S(1.0f)),
                          ImVec2(x1 - width * 0.1f, at.y + height - S(1.0f)), Gold(0.45f), S(1.0f));
        }
    }
    const ImU32 label_color = !enabled ? Dim() : focused ? Bright() : Plain();
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(at.x + S(28.0f), mid_y - label_size.y * 0.5f), label_color, label);

    // The value column: "<  value  >" centred, or a bar for sliders.
    const float col0 = at.x + width * 0.56f, col1 = at.x + width - S(28.0f);
    const float col_mid = (col0 + col1) * 0.5f;
    int step = 0;
    if (enabled && value) {
        const ImU32 arrow = focused ? Gold(1.0f) : Gold(0.55f);
        const float h = S(7.0f);
        draw->AddTriangleFilled(ImVec2(col0, mid_y), ImVec2(col0 + h, mid_y - h),
                                ImVec2(col0 + h, mid_y + h), arrow);
        draw->AddTriangleFilled(ImVec2(col1, mid_y), ImVec2(col1 - h, mid_y - h),
                                ImVec2(col1 - h, mid_y + h), arrow);
    }
    if (slider >= 0.0f) {
        const float b0 = col0 + S(24.0f), b1 = col1 - S(24.0f) - S(60.0f);
        const float bh = S(3.0f);
        draw->AddRectFilled(ImVec2(b0, mid_y - bh), ImVec2(b1, mid_y + bh), Dim(0.6f), bh);
        draw->AddRectFilled(ImVec2(b0, mid_y - bh),
                            ImVec2(b0 + (b1 - b0) * std::clamp(slider, 0.0f, 1.0f), mid_y + bh),
                            enabled ? Gold(0.9f) : Dim(), bh);
        const float knob = b0 + (b1 - b0) * std::clamp(slider, 0.0f, 1.0f);
        draw->AddCircleFilled(ImVec2(knob, mid_y), S(7.0f), enabled ? Bright() : Dim());
        if (value) {
            const ImVec2 size = ImGui::CalcTextSize(value);
            draw->AddText(ImVec2(col1 - S(24.0f) - size.x, mid_y - size.y * 0.5f),
                          enabled ? Plain() : Dim(), value);
        }
        if (enabled && drag && active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float x = ImGui::GetIO().MousePos.x;
            if (x >= b0 - S(12.0f) && x <= b1 + S(12.0f)) {
                *drag = std::clamp((x - b0) / (b1 - b0), 0.0f, 1.0f);
            }
        }
    } else if (value) {
        const ImVec2 size = ImGui::CalcTextSize(value);
        draw->AddText(ImVec2(col_mid - size.x * 0.5f, mid_y - size.y * 0.5f),
                      !enabled ? Dim() : focused ? Bright() : Plain(), value);
    }
    if (enabled && focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) {
            step = -1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                   ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                   ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) {
            step = 1;
        }
    }
    if (enabled && pressed && slider < 0.0f) {
        step = ImGui::GetIO().MousePos.x < col_mid && ImGui::GetIO().MousePos.x >= col0 - S(12.0f) ? -1 : 1;
    } else if (focused && state.activate) {
        step = 1;
        state.activate = false;
    }
    ImGui::PopID();
    return step;
}

/// A non-interactive line under the rows (state, warnings).
void Note(const char* text, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Indent(S(28.0f));
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - S(28.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::Unindent(S(28.0f));
    ImGui::PopStyleColor();
}

const char* OnOff(bool on) {
    return on ? T("On", "Вкл") : T("Off", "Выкл");
}

void Toggle(const char* id, const char* label, std::atomic<bool>& value, const char* hint,
            bool enabled = true) {
    if (Row(id, label, OnOff(value), hint, enabled) != 0) {
        Store(value, !value.load(), true);
    }
}

/// A choice among `count` values (wrapping); `allowed(i)` false skips a value.
template <typename Label, typename Allowed>
void Choice(const char* id, const char* label, int current, int count, Label&& name,
            Allowed&& allowed, const char* hint, bool enabled, const std::function<void(int)>& set) {
    const int step = Row(id, label, name(current), hint, enabled);
    if (step == 0) {
        return;
    }
    for (int n = 1; n <= count; ++n) {
        const int next = ((current + step * n) % count + count) % count;
        if (allowed(next)) {
            if (next != current) {
                set(next);
            }
            return;
        }
    }
}

void SliderRow(const char* id, const char* label, std::atomic<float>& value, float lo, float hi,
               float step_size, const char* hint, bool enabled = true) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value.load());
    float drag = -1.0f;
    const int step = Row(id, label, text, hint, enabled, (value - lo) / (hi - lo), &drag);
    if (step != 0) {
        const float v = std::clamp(std::round((value + step * step_size) / step_size) * step_size, lo, hi);
        Store(value, v, true);
    } else if (drag >= 0.0f) {
        const float v = std::round((lo + drag * (hi - lo)) / 0.01f) * 0.01f;
        Store(value, v, v != value);
    }
}

/// The icon style and mouse camera this run started with (the menu cannot change either before
/// its first frame): the icons are drawn into the game's textures and the camera hook installed
/// at start.
const std::optional<std::string>& StartupLine(const char* key) {
    static std::map<std::string, std::optional<std::string>> startup;
    const auto it = startup.find(key);
    return it != startup.end() ? it->second : startup.emplace(key, BbSettings::Line(key)).first->second;
}

bool ControlsNeedRestart() {
    const auto icons = BbSettings::Line("button_icons");
    const bool camera_on = BbSettings::Line("mouse_camera").value_or("1") != "0";
    return icons.value_or("auto") != StartupLine("button_icons").value_or("auto") ||
           (camera_on && StartupLine("mouse_camera").value_or("1") == "0");
}

bool RestartNeeded() {
    auto& s = BbSettings::Get();
    bool restart = s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart() || ControlsNeedRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    return restart;
}

/// HDR output: only while the display can show it (HDR switched on for it in Windows).
void HdrRows() {
    auto& s = BbSettings::Get();
    if (!s.hdr_supported) {
        Note(T("HDR: switch HDR on for this display in Windows to get the HDR options here.",
               "HDR: включите HDR для этого монитора в Windows, и здесь появятся настройки HDR."),
             Dim());
        return;
    }
    Toggle("hdr", T("HDR output", "Вывод HDR"), s.hdr,
           T("HDR10 output: the picture's white sits at the UI brightness, its highlights reach "
             "toward the peak brightness.",
             "Вывод HDR10: белый цвет картинки — на яркости интерфейса, яркие места тянутся к "
             "пиковой яркости."));
    const bool on = s.hdr;
    static char peak_names[BbSettings::HdrPeakCount][24], white_names[BbSettings::HdrPaperWhiteCount][24];
    for (int i = 0; i < BbSettings::HdrPeakCount; ++i) {
        std::snprintf(peak_names[i], sizeof(peak_names[i]), "%d %s", BbSettings::HdrPeakNits[i],
                      T("nits", "нит"));
    }
    for (int i = 0; i < BbSettings::HdrPaperWhiteCount; ++i) {
        std::snprintf(white_names[i], sizeof(white_names[i]), "%d %s", BbSettings::HdrPaperWhiteNits[i],
                      T("nits", "нит"));
    }
    Choice("hdr_peak", T("HDR peak brightness", "Пиковая яркость HDR"),
           BbSettings::HdrPeakIndex(s.hdr_peak_nits), BbSettings::HdrPeakCount,
           [](int i) { return peak_names[i]; }, [](int) { return true; },
           T("Your display's peak brightness (Windows HDR Calibration or the display's specs).",
             "Пиковая яркость вашего монитора (калибровка HDR в Windows или характеристики монитора)."),
           on, [&](int i) { Store(s.hdr_peak_nits, BbSettings::HdrPeakNits[i], true); });
    Choice("hdr_paper_white", T("HDR UI brightness", "Яркость интерфейса HDR"),
           BbSettings::HdrPaperWhiteIndex(s.hdr_paper_white), BbSettings::HdrPaperWhiteCount,
           [](int i) { return white_names[i]; }, [](int) { return true; },
           T("Brightness of white in the picture and the interface (paper white). 200 is a good "
             "start; lower in a dark room.",
             "Яркость белого в картинке и интерфейсе (paper white). 200 — хорошее начало; в тёмной "
             "комнате ниже."),
           on, [&](int i) { Store(s.hdr_paper_white, BbSettings::HdrPaperWhiteNits[i], true); });
    Toggle("hdr_scene", T("HDR scene highlights", "Света HDR из сцены"), s.hdr_scene,
           T("The game's HUD stays at UI brightness, and lamps and flames may get extra brightness "
             "from the game's 16-bit scene. Off: the whole SDR picture is stretched, HUD included.",
             "Интерфейс игры остаётся на яркости интерфейса, огни могут стать ярче по 16-битной "
             "сцене игры. Выкл: растягивается вся SDR картинка, вместе с интерфейсом."),
           on);
    SliderRow("hdr_highlights", T("HDR highlights", "Света HDR"), s.hdr_highlights, 0.0f, 1.0f, 0.1f,
              T("How bright the highlights (fire, lamps, sky) get. 0.5: white reaches the peak "
                "brightness; higher brightens more of the picture; 0 keeps the SDR picture.",
                "Насколько яркие света (огонь, фонари, небо). 0.5: белый доходит до пиковой "
                "яркости; больше — ярче становится большая часть картинки; 0 — как в SDR."),
              on);
}

void DisplayTab() {
    auto& s = BbSettings::Get();
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    Choice("output", T("Output resolution", "Разрешение вывода"), s.output_res, BbSettings::OutputCount,
           [](int i) { return outputs[i]; }, [](int) { return true; },
           BbSettings::FixedRenderSession()
               ? T("Size of the final frame and UI. The preset sets the scene size relative to "
                   "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
                   "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
                   "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры.")
               : T("The final frame and UI size changes at the next frame. The preset sets the "
                   "scene size relative to the output: 4K Performance = 1920x1080. Changing it "
                   "resets the upscaler's history (a brief pause).",
                   "Размер готового кадра и интерфейса меняется со следующего кадра. Пресет задаёт "
                   "размер сцены относительно вывода: 4K Performance = 1920x1080. Смена сбрасывает "
                   "историю апскейлера (короткая пауза)."),
           true, [&](int i) { Store(s.output_res, i, true); });
    const char* live_modes[] = {T("Auto (by GPU)", "Авто (по видеокарте)"), T("Off (faster)", "Выкл (быстрее)"),
                                T("On", "Вкл")};
    Choice("live", T("Live resolution changes", "Смена разрешения на лету"), s.live_resolution + 1, 3,
           [&](int i) { return live_modes[i]; }, [](int) { return true; },
           T("On: output resolution and preset change without restarting, but the game's "
             "post-processing stays at 1080p (slower on the Steam Deck and older GPUs). Off: "
             "everything renders at the preset's size, changes need a restart. Auto turns it on "
             "for strong discrete GPUs. Applies after restarting the game.",
             "Вкл: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
             "остаётся в 1080p (медленнее на Steam Deck и старых видеокартах). Выкл: всё рисуется "
             "в размере пресета, смена — через перезапуск. Авто включает её на мощных дискретных "
             "видеокартах. Применяется после перезапуска игры."),
           true, [&](int i) { Store(s.live_resolution, i - 1, true); });
    const char* window_modes[] = {T("Windowed", "В окне"), T("Borderless window", "Окно без рамки"),
                                  T("Exclusive fullscreen", "Эксклюзивный полный экран")};
    Choice("window_mode", T("Window mode", "Режим окна"),
           std::clamp(s.window_mode.load(), 0, BbSettings::WindowModeCount - 1), BbSettings::WindowModeCount,
           [&](int i) { return window_modes[i]; }, [](int) { return true; },
           T("Borderless fills the screen without taking it over: quick Alt+Tab, overlays and HDR "
             "keep working. Exclusive fullscreen switches the display over to the game.",
             "Без рамки — на весь экран без захвата дисплея: быстрый Alt+Tab, оверлеи и HDR "
             "работают. Эксклюзивный полный экран переключает дисплей на игру."),
           true, [&](int i) { Store(s.window_mode, i, true); });
    HdrRows();
    Toggle("fps", T("FPS counter", "Счётчик FPS"), s.show_fps,
           T("Frames per second and frame time in the top right corner.",
             "Кадры в секунду и время кадра в правом верхнем углу."));
    Choice("fps_limit", T("FPS limit", "Лимит FPS"), BbSettings::FpsLimitIndex(s.fps_limit),
           BbSettings::FpsLimitCount,
           [&](int i) {
               static char text[16];
               if (BbSettings::FpsLimits[i] == 0) {
                   return T("Off", "Выкл");
               }
               std::snprintf(text, sizeof(text), "%d", BbSettings::FpsLimits[i]);
               return static_cast<const char*>(text);
           },
           [](int) { return true; },
           T("Highest frame rate. A limit unlocks the game's frame rate from the next start "
             "(the 60 FPS mode is the game's own fixed 60); then it changes at once. Off keeps "
             "the launcher's mode.",
             "Предел частоты кадров. С лимитом частота кадров игры разблокируется со следующего "
             "запуска (режим 60 FPS — собственные 60 игры); дальше меняется сразу. Выкл "
             "оставляет режим лаунчера."),
           true, [&](int i) { Store(s.fps_limit, BbSettings::FpsLimits[i], true); });
    static const char* languages[] = {"English", "Русский"};
    Choice("language", "Language", s.menu_language == BbSettings::MenuLanguage::Russian ? 1 : 0, 2,
           [](int i) { return languages[i]; }, [](int) { return true; },
           T("Language of this menu.", "Язык этого меню."), true, [&](int i) {
               s.menu_language = i == 1 ? BbSettings::MenuLanguage::Russian
                                        : BbSettings::MenuLanguage::English;
               BbSettings::Save();
           });
}

void DlssModelRow() {
    auto& s = BbSettings::Get();
    Choice("dlss_model", T("DLSS model", "Модель DLSS"), BbSettings::DlssModelIndex(s.dlss_model),
           BbSettings::DlssModelCount,
           [&](int i) {
               return i == 0 ? T("Default", "По умолчанию") : BbSettings::DlssModelNames[i];
           },
           [](int) { return true; },
           T("NVIDIA's DLSS preset: K best quality, L and M faster, J like K with a little less "
             "ghosting. Default lets NVIDIA pick per quality mode.",
             "Пресет NVIDIA DLSS: K лучшее качество, L и M быстрее, J как K с чуть меньшими "
             "шлейфами. По умолчанию NVIDIA выбирает сама."),
           s.upscaler == BbSettings::UpscalerDlss,
           [&](int i) { Store(s.dlss_model, BbSettings::DlssModels[i], true); });
}

void UpscalerTab() {
    auto& s = BbSettings::Get();
    const char* upscalers[] = {T("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1",
                               T("TAA (native)", "TAA (нативное)"), "DLSS (NVIDIA RTX)",
                               "XeSS (Intel, any GPU)"};
    const auto supported = [&](int i) {
        return i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
               : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
               : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
               : i == BbSettings::UpscalerXess   ? s.xess_supported.load()
                                                 : true;
    };
    const char* upscaler_hint =
        s.upscaler == BbSettings::UpscalerFsr411
            ? T("FSR 4.1.1: the model from AMD's 4.1.1 DLL, reproduced in Vulkan (output matches "
                "the DLL). Assets are built from your DLL in the launcher.",
                "FSR 4.1.1: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат совпадает "
                "с DLL). Ассеты собираются из вашей DLL в лаунчере.")
        : s.upscaler == BbSettings::UpscalerFsr4
            ? T("FSR 4 in INT8 mode (v07 model from AMD's FidelityFX SDK sources): higher quality "
                "than FSR 3.1, heavier.",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK): качество выше, "
                "чем у FSR 3.1, проход тяжелее.")
        : s.upscaler == BbSettings::UpscalerTaa
            ? T("TAA anti-aliases the scene at the output resolution, without an FSR model.",
                "TAA сглаживает сцену в разрешении вывода, без модели FSR.")
        : s.upscaler == BbSettings::UpscalerDlss
            ? T("NVIDIA DLSS Super Resolution (RTX GPUs; Native AA is DLAA). Needs the DLSS bridge "
                "and NVIDIA's library next to the game; otherwise FSR 3.1 is used.",
                "NVIDIA DLSS Super Resolution (видеокарты RTX; Native AA — это DLAA). Нужны мост DLSS "
                "и библиотека NVIDIA рядом с игрой; без них используется FSR 3.1.")
        : s.upscaler == BbSettings::UpscalerXess
            ? T("Intel XeSS Super Resolution (runs on any GPU). Needs Intel's library next to the "
                "game.",
                "Intel XeSS Super Resolution (работает на любой видеокарте). Нужна библиотека Intel "
                "рядом с игрой.")
            : T("Temporal upscaler: anti-aliasing and rendering below the output resolution. FSR 4 "
                "needs a GPU with the required features (unavailable values are skipped).",
                "Временной апскейлер: сглаживание и отрисовка ниже разрешения вывода. FSR 4 нужна "
                "видеокарта с нужными возможностями (недоступные значения пропускаются).");
    Choice("upscaler", T("Upscaler", "Апскейлер"), s.upscaler, BbSettings::UpscalerCount,
           [&](int i) { return upscalers[i]; }, supported, upscaler_hint, true,
           [&](int i) { Store(s.upscaler, i, true); });
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    static char preset_names[BbSettings::PresetCount][64];
    for (int i = 0; i < BbSettings::PresetCount; ++i) {
        const float scale = BbSettings::PresetScale(i);
        std::snprintf(preset_names[i], sizeof(preset_names[i]), "%s  %dx%d", BbSettings::PresetName(i),
                      int(std::lround(BbSettings::OutputWidths[s.output_res] / scale / 2) * 2),
                      int(std::lround(BbSettings::OutputHeights[s.output_res] / scale / 2) * 2));
    }
    Choice("preset", T("Quality preset", "Пресет качества"), taa ? BbSettings::NativeAA : s.preset.load(),
           BbSettings::PresetCount, [](int i) { return preset_names[i]; }, [](int) { return true; },
           T("Native AA: the upscaler only anti-aliases. The other presets render the scene below "
             "the output resolution (the size shown); the UI stays at the output resolution.",
             "Native AA: апскейлер только сглаживает. Остальные пресеты рисуют сцену ниже "
             "разрешения вывода (показан размер); интерфейс — в разрешении вывода."),
           upscaler_on && !taa, [&](int i) { Store(s.preset, i, true); });
    DlssModelRow();
    Toggle("sharpen", T("Sharpening", "Резкость"), s.sharpen,
           T("RCAS sharpening after the upscaler.", "Повышение резкости RCAS после апскейлера."),
           upscaler_on);
    SliderRow("sharpness", T("Sharpness", "Сила резкости"), s.sharpness, 0.0f, 2.0f, 0.05f,
              T("Up to 1: the upscaler's own RCAS. Above 1 another RCAS pass is added.",
                "До 1 — RCAS самого апскейлера. Выше 1 добавляется ещё один проход RCAS."),
              upscaler_on && s.sharpen);
    Toggle("jitter", T("Subpixel jitter", "Субпиксельный сдвиг"), s.jitter,
           T("Each frame shifts the scene by a fraction of a pixel so the upscaler reconstructs "
             "more detail from several frames.",
             "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает больше деталей из "
             "нескольких кадров."),
           upscaler_on);
    Toggle("motion", T("Character motion vectors", "Векторы движения персонажей"), s.object_motion,
           T("Accurate motion for animated objects: clothes and weapons break up less in motion. "
             "Applies after restarting the game.",
             "Точное движение анимированных объектов: одежда и оружие меньше рассыпаются в "
             "движении. Применяется после перезапуска игры."),
           upscaler_on);
    Toggle("frame_generation", T("Frame generation (FSR 3.1)", "Генерация кадров (FSR 3.1)"),
           s.frame_generation,
           T("Doubles the frame rate: one interpolated frame between every two game frames. Adds a "
             "little input lag; best from 60 FPS. Pauses while the frame rate is above half the "
             "display's refresh rate.",
             "Удваивает частоту кадров: между каждыми двумя кадрами игры показывается один "
             "интерполированный. Немного добавляет задержки ввода; лучше от 60 FPS. "
             "Приостанавливается, пока частота кадров выше половины частоты экрана."),
           upscaler_on);
    if (BbSettings::IsFsr4(s.upscaler)) {
        Toggle("fsr4_exposure", T("FSR 4: auto exposure", "FSR 4: автоэкспозиция"), s.fsr4_auto_exposure,
               T("The FSR 4 network normalises colour by exposure and uses it to decide when to "
                 "drop older frames.",
                 "Сеть FSR 4 нормирует цвет по экспозиции и по ней решает, когда отбросить прошлые "
                 "кадры."));
        Toggle("fsr4_jitter", T("FSR 4: invert jitter sign", "FSR 4: обратный знак сдвига"),
               s.fsr4_invert_jitter, T("For diagnosing ghosting.", "Для проверки шлейфов."));
    }
    char line[160];
    std::snprintf(line, sizeof(line), T("Scene renders at %d x %d", "Сцена рисуется в %d x %d"),
                  s.active_render_width.load(), s.active_render_height.load());
    ImGui::Spacing();
    Note(line, Dim());
    if (BbSettings::FixedRenderSession()) {
        std::snprintf(line, sizeof(line), T("Preset at startup: %s (changes after a restart)",
                                            "Пресет при запуске: %s (смена — после перезапуска)"),
                      BbSettings::PresetName(s.startup_preset));
        Note(line, Dim());
    }
    if (const char* problem = s.dlss_problem.load(); problem && s.upscaler == BbSettings::UpscalerDlss) {
        std::snprintf(line, sizeof(line), "DLSS: %s", problem);
        Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
    if (const char* problem = s.xess_problem.load(); problem && s.upscaler == BbSettings::UpscalerXess) {
        std::snprintf(line, sizeof(line), "XeSS: %s", problem);
        Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
    if (const char* problem = s.fsr4_problem.load()) {
        std::snprintf(line, sizeof(line), T("FSR 4 unavailable: %s", "FSR 4 недоступен: %s"), problem);
        Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
}

void EffectsTab() {
    auto& s = BbSettings::Get();
    const char* lods[] = {T("Highest (-2)", "Максимальная (-2)"), T("Game default", "Как в игре"),
                          T("Lower (1)", "Ниже (1)"), T("Lowest (2)", "Минимальная (2)")};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    Choice("lod", T("Model detail", "Детализация моделей"), lod_index, 4, [&](int i) { return lods[i]; },
           [](int) { return true; },
           T("Level of detail of models (game patch). Applies after restarting the game.",
             "Детализация моделей (патч игры). Применяется после перезапуска игры."),
           true, [&](int i) { Store(s.model_lod, lod_values[i], true); });
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        const char* hint =
            std::string_view(effect.key) == "debug_camera"
                ? T("Hold Cross and press L3 (keyboard: Space + Z). Applies after restarting.",
                    "Удерживайте Cross и нажмите L3 (клавиатура: Space + Z). После перезапуска.")
            : std::string_view(effect.key) == "debug_menu"
                ? T("Left side of the touchpad (Tab). Needs the adhoc folder from Nexus mod #253. "
                    "Applies after restarting.",
                    "Левая сторона тачпада (Tab). Нужна папка adhoc из мода Nexus #253. После "
                    "перезапуска.")
                : T("A game patch, applied when the game starts: after a restart. Motion blur and "
                    "shadows from dynamic lights cost the most GPU time.",
                    "Патч игры, применяется при запуске: после перезапуска. Размытие в движении и тени "
                    "от динамических источников нагружают видеочип сильнее всего.");
        Toggle(effect.key, T(effect.label, effect.label_ru), s.effects[e], hint);
    }
}

void AdvancedTab() {
    auto& s = BbSettings::Get();
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    Toggle("reactive", T("Reactive mask", "Маска реактивности"), s.reactive,
           BbSettings::IsFsr4(s.upscaler)
               ? T("Marks transparent effects (haze, light, water, particles): there the FSR 4 "
                   "output is blended with the current frame so effects do not drag older frames.",
                   "Помечает прозрачные эффекты (дымку, свет, воду, частицы): там результат FSR 4 "
                   "смешивается с текущим кадром, и эффекты не тянут за собой прошлые кадры.")
               : T("Marks transparent effects so the upscaler relies less on older frames: fewer "
                   "trails, more shimmer under them.",
                   "Помечает прозрачные эффекты, чтобы апскейлер меньше опирался на прошлые кадры: "
                   "меньше шлейфов, больше дрожания под ними."),
           upscaler_on && !taa);
    const bool mask = upscaler_on && !taa && s.reactive;
    SliderRow("reactive_scale", T("Mask scale", "Масштаб маски"), s.reactive_scale, 0.0f, 4.0f, 0.1f,
              nullptr, mask);
    SliderRow("reactive_threshold", T("Mask threshold", "Порог маски"), s.reactive_threshold, 0.0f,
              1.0f, 0.05f, nullptr, mask);
    SliderRow("reactive_max", T("Mask maximum", "Максимум маски"), s.reactive_max, 0.0f, 1.0f, 0.05f,
              nullptr, mask);
    const char* views[] = {T("Off", "Выкл"), T("Reactive mask", "Маска реактивности"),
                           T("Motion vectors", "Векторы движения")};
    Choice("debug_view", T("Debug view", "Отладочный вид"), s.debug_view, BbSettings::DebugViewCount,
           [&](int i) { return views[i]; }, [](int) { return true; },
           T("Motion vectors: red/green = horizontal/vertical motion (8 px = full), blue = the pixel "
             "has an accurate object vector. A moving object with neither is a trail.",
             "Векторы движения: красный/зелёный — движение по горизонтали/вертикали (8 пикселей = "
             "полная яркость), синий — у пикселя точный вектор объекта. Движущийся предмет без того "
             "и другого даёт шлейф."),
           upscaler_on, [&](int i) { s.debug_view = i; });
}

/// The save editor: the character's echoes, insight and attributes, and items given to it,
/// changed in the running game (bbport_save_editor.h); the game saves them as usual.
// Controls (Kaniel 2026-10-11): the bbport.ini lines runtime_pad.c reads (key.<input>,
// pad.<input>, mouse_*) and the icon style the start-up step draws (button_icons). Bindings are
// written at once and the runtime takes them at its next pad read; the mouse settings when the
// menu closes; the icon style at the next start.
const char* InputLabel(int input) {
    static const char* const en[] = {
        "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Options",
        "Touchpad, left half (gestures)", "Touchpad, right half (key items)", "D-pad up", "D-pad down",
        "D-pad left", "D-pad right", "Move forward", "Move back", "Move left", "Move right", "Camera up",
        "Camera down", "Camera left", "Camera right"};
    static const char* const ru[] = {
        "Крест", "Круг", "Квадрат", "Треугольник", "L1", "R1", "L2", "R2", "L3", "R3", "Options",
        "Тачпад, левая половина (жесты)", "Тачпад, правая половина (личные вещи)", "Крестовина вверх",
        "Крестовина вниз", "Крестовина влево", "Крестовина вправо", "Движение вперёд", "Движение назад",
        "Движение влево", "Движение вправо", "Камера вверх", "Камера вниз", "Камера влево", "Камера вправо"};
    static_assert(std::size(en) == std::size(ru));
    if (runtime_pad_input_count() != int(std::size(en))) { // another runtime: its own names
        const char* name = runtime_pad_input_name(input);
        return name ? name : "?";
    }
    return input >= 0 && input < int(std::size(en)) ? T(en[input], ru[input]) : "?";
}

/// SDL's gamepad button names (pad.<input> lines) as the buttons read: PlayStation, then Xbox.
std::string PadText(const std::string& names) {
    static const std::pair<std::string_view, const char*> labels[] = {
        {"a", "Cross (A)"}, {"b", "Circle (B)"}, {"x", "Square (X)"}, {"y", "Triangle (Y)"},
        {"leftshoulder", "L1"}, {"rightshoulder", "R1"}, {"lefttrigger", "L2"}, {"righttrigger", "R2"},
        {"leftstick", "L3"}, {"rightstick", "R3"}, {"start", "Options"}, {"back", "Share (Back)"},
        {"guide", "PS"}, {"touchpad", "Touchpad"}, {"dpup", "D-pad up"}, {"dpdown", "D-pad down"},
        {"dpleft", "D-pad left"}, {"dpright", "D-pad right"}};
    std::string out;
    size_t at = 0;
    while (at < names.size()) {
        size_t end = names.find(", ", at);
        if (end == std::string::npos) {
            end = names.size();
        }
        const std::string_view name{names.data() + at, end - at};
        const auto label = std::find_if(std::begin(labels), std::end(labels),
                                        [&](const auto& l) { return l.first == name; });
        out += (out.empty() ? "" : ", ") + (label != std::end(labels) ? std::string(label->second) : std::string(name));
        at = end + 2;
    }
    return out;
}

void SaveControls() {
    BbSettings::Save();
    runtime_pad_reload_controls();
}

float MouseSensitivity() {
    const float v = float(std::atof(BbSettings::Line("mouse_sensitivity").value_or("1.00").c_str()));
    return std::isfinite(v) ? std::clamp(v, 0.1f, 5.0f) : 1.0f;
}

void ControlsTab() {
    static const char* const icon_values[] = {"auto", "playstation", "xbox", "keyboard"};
    const std::string icons = BbSettings::Line("button_icons").value_or("auto");
    int icon = 0;
    for (int i = 0; i < 4; ++i) {
        if (icons == icon_values[i]) {
            icon = i;
        }
    }
    Choice("icons", T("Button icons", "Значки кнопок"), icon, 4,
           [](int i) {
               return i == 0   ? T("Automatic", "Автоматически")
                      : i == 1 ? "PlayStation"
                      : i == 2 ? "Xbox"
                               : T("Keyboard and mouse", "Клавиатура и мышь");
           },
           [](int) { return true; },
           T("The buttons the game shows on screen. Keyboard and mouse shows the keys bound below. Automatic "
             "follows the controller connected at start (Xbox icons for an Xbox or other controller, "
             "PlayStation ones for a PlayStation controller, your keys with none). Applies after a restart.",
             "Кнопки, которые игра показывает на экране. «Клавиатура и мышь» показывает назначенные ниже "
             "клавиши. «Автоматически» — по контроллеру при запуске (Xbox для Xbox и прочих, PlayStation для "
             "PlayStation, ваши клавиши без контроллера). Применяется после перезапуска."),
           true, [](int i) {
               BbSettings::SetLine("button_icons", std::string(icon_values[i]));
               SaveControls();
           });

    const bool camera = BbSettings::Line("mouse_camera").value_or("1") != "0";
    if (Row("mouse_camera", T("Mouse camera", "Камера мышью"), OnOff(camera),
            T("The mouse turns the camera while the game window has focus; its buttons and wheel can be bound "
              "below. Insert or Alt+Tab lets go of the mouse.",
              "Мышь поворачивает камеру, пока окно игры в фокусе; её кнопки и колесо можно назначить ниже. "
              "Insert или Alt+Tab освобождают мышь."),
            true) != 0) {
        BbSettings::SetLine("mouse_camera", std::string(camera ? "0" : "1"));
        dirty = true;
    }
    {
        const float sensitivity = MouseSensitivity();
        char text[16];
        std::snprintf(text, sizeof(text), "%.2f", sensitivity);
        float drag = -1.0f;
        const int step = Row("mouse_sensitivity", T("Mouse sensitivity", "Чувствительность мыши"), text,
                             T("How far the camera turns for a mouse movement (1.00: as in most PC games).",
                               "Насколько поворачивается камера при движении мыши (1.00 — как в большинстве игр на ПК)."),
                             camera, (sensitivity - 0.1f) / (5.0f - 0.1f), &drag);
        float next = sensitivity;
        if (step != 0) {
            next = std::clamp(std::round((sensitivity + step * 0.1f) * 10.0f) / 10.0f, 0.1f, 5.0f);
        } else if (drag >= 0.0f) {
            next = std::round((0.1f + drag * (5.0f - 0.1f)) * 100.0f) / 100.0f;
        }
        if (next != sensitivity) {
            std::snprintf(text, sizeof(text), "%.2f", next);
            BbSettings::SetLine("mouse_sensitivity", std::string(text));
            dirty = true;
        }
    }
    const bool invert = BbSettings::Line("mouse_invert_y").value_or("0") == "1";
    if (Row("mouse_invert_y", T("Invert mouse Y", "Инверсия мыши по вертикали"), OnOff(invert), nullptr, camera) != 0) {
        BbSettings::SetLine("mouse_invert_y", std::string(invert ? "0" : "1"));
        dirty = true;
    }

    // A press that came for the input waiting for one, or the wait running out.
    if (capture.input >= 0 && capture.done) {
        const char* name = runtime_pad_input_name(capture.input);
        if (name) {
            BbSettings::SetLine(capture.kind + "." + name, capture.name);
            SaveControls();
        }
        capture = {};
    } else if (capture.input >= 0 && std::chrono::steady_clock::now() - capture.since > std::chrono::seconds(10)) {
        capture = {};
    }

    ImGui::Spacing();
    Note(T("Keyboard and mouse  |  gamepad", "Клавиатура и мышь  |  геймпад"), Dim());
    const char* hint = T("Enter / Cross: assign. The next key, mouse button or wheel step replaces this input's "
                         "keyboard binding, a gamepad button its gamepad one; hold Shift, Ctrl or Alt for a "
                         "combination. Delete: no key. Esc or 10 s: cancel.",
                         "Enter / Крест: назначить. Следующая клавиша, кнопка мыши или шаг колеса заменяет "
                         "клавиатурное назначение, кнопка геймпада — геймпадное; Shift, Ctrl или Alt — для "
                         "сочетания. Delete — без клавиши. Esc или 10 с — отмена.");
    const char* waiting_pad = T("Press a key, mouse button or gamepad button...",
                                "Нажмите клавишу, кнопку мыши или геймпада...");
    const char* waiting_key = T("Press a key or mouse button...", "Нажмите клавишу или кнопку мыши...");
    for (int i = 0; i < runtime_pad_input_count(); ++i) {
        char keys[160], pads[160];
        runtime_pad_binding_text(i, 0, keys, sizeof(keys));
        std::string value = keys[0] ? keys : T("(none)", "(нет)");
        if (runtime_pad_input_takes_pad(i)) {
            runtime_pad_binding_text(i, 1, pads, sizeof(pads));
            value += "  |  " + (pads[0] ? PadText(pads) : std::string("-"));
        }
        if (value.size() > 46) {
            value = value.substr(0, 43) + "...";
        }
        const bool waiting = capture.input == i;
        char id[32];
        std::snprintf(id, sizeof(id), "bind_%d", i);
        if (Row(id, InputLabel(i), waiting ? (capture.pad_ok ? waiting_pad : waiting_key) : value.c_str(), hint,
                true) != 0 &&
            capture.input < 0) {
            capture = {};
            capture.input = i;
            capture.pad_ok = runtime_pad_input_takes_pad(i) != 0;
            capture.since = std::chrono::steady_clock::now();
        }
    }
    ImGui::Spacing();
    if (Row("controls_reset", T("Reset the controls", "Сбросить управление"), nullptr,
            T("Every key and gamepad button back to the defaults (the Dark Souls III keyboard layout).",
              "Все клавиши и кнопки геймпада по умолчанию (раскладка Dark Souls III)."),
            true) != 0) {
        for (int i = 0; i < runtime_pad_input_count(); ++i) {
            if (const char* name = runtime_pad_input_name(i)) {
                BbSettings::SetLine(std::string("key.") + name, std::nullopt);
                BbSettings::SetLine(std::string("pad.") + name, std::nullopt);
            }
        }
        SaveControls();
    }
}

void SaveTab() {
    static int category = 0, item_index = 0, quantity = 1;
    const auto character = BbSaveEditor::Read();
    if (!character) {
        Note(T("Load your character first: the page edits the character in the world.",
               "Сначала загрузите персонажа: страница меняет персонажа в игре."),
             Dim());
        return;
    }
    const BbSaveEditor::Character c = *character;
    char text[96];
    std::snprintf(text, sizeof(text), "%d", c.echoes);
    if (const int step = Row("echoes", T("Blood Echoes", "Отголоски крови"), text,
                             T("Left/right: 10,000 at a time.", "Влево/вправо: по 10 000."), true)) {
        BbSaveEditor::SetEchoes(BbSaveEditor::ClampEchoes(std::int64_t{c.echoes} + step * 10'000));
    }
    std::snprintf(text, sizeof(text), "%d", c.insight);
    if (const int step = Row("insight", T("Insight", "Озарение"), text, nullptr, true)) {
        BbSaveEditor::SetInsight(BbSaveEditor::ClampInsight(c.insight + step));
    }
    static const char* names_en[] = {"Vitality", "Endurance", "Strength", "Skill", "Bloodtinge",
                                     "Arcane"};
    static const char* names_ru[] = {"Живучесть", "Выносливость", "Сила", "Ловкость",
                                     "Кровь", "Тайна"};
    for (int i = 0; i < BbSaveEditor::AttributeCount; ++i) {
        std::snprintf(text, sizeof(text), "%d", c.attributes[i]);
        if (const int step = Row(names_en[i], T(names_en[i], names_ru[i]), text,
                                 T("The level follows (one per point); HP and stamina are "
                                   "recalculated as at the Doll.",
                                   "Уровень меняется вместе с ним (по одному за очко); здоровье и "
                                   "выносливость пересчитываются, как у Куклы."),
                                 true)) {
            std::int32_t attributes[BbSaveEditor::AttributeCount];
            std::copy(std::begin(c.attributes), std::end(c.attributes), attributes);
            attributes[i] = BbSaveEditor::ClampAttribute(attributes[i] + step);
            BbSaveEditor::SetAttributes(attributes);
        }
    }
    std::snprintf(text, sizeof(text), T("Level %d, HP %d / %d", "Уровень %d, здоровье %d / %d"),
                  c.level, c.hp, c.hp_max);
    Note(text, Dim());

    ImGui::Spacing();
    const auto& items = BbSaveEditor::Items();
    if (items.empty()) {
        Note(T("Items: no item list found. It comes from the shadPS4 kit's save editor "
               "(DATA/Bloodborne Save Editor/resources) or BB_SAVE_EDITOR_DATA.",
               "Предметы: список не найден. Он берётся из редактора сохранений набора shadPS4 "
               "(DATA/Bloodborne Save Editor/resources) или BB_SAVE_EDITOR_DATA."),
             Dim());
        return;
    }
    static constexpr std::uint32_t categories[] = {BbSaveEditor::GoodsCategory,
                                                   BbSaveEditor::WeaponCategory,
                                                   BbSaveEditor::ArmorCategory};
    const char* category_names[] = {T("Items", "Предметы"), T("Weapons", "Оружие"),
                                    T("Attire", "Одежда")};
    Choice("item_category", T("Category", "Категория"), category, 3,
           [&](int i) { return category_names[i]; }, [](int) { return true; }, nullptr, true,
           [&](int i) {
               category = i;
               item_index = 0;
           });
    std::vector<const BbSaveEditor::Item*> shown;
    for (const auto& item : items) {
        if (item.category == categories[category]) {
            shown.push_back(&item);
        }
    }
    if (shown.empty()) {
        return;
    }
    item_index = std::clamp(item_index, 0, int(shown.size()) - 1);
    if (const int step = Row("item", T("Item", "Предмет"), shown[item_index]->name.c_str(),
                             T("Left/right picks; Cross on the Give row hands it over.",
                               "Влево/вправо — выбор; Cross на строке «Выдать» — выдать."),
                             true)) {
        item_index = (item_index + step + int(shown.size())) % int(shown.size());
    }
    std::snprintf(text, sizeof(text), "%d", quantity);
    if (const int step = Row("quantity", T("Quantity", "Количество"), text, nullptr,
                             categories[category] == BbSaveEditor::GoodsCategory)) {
        quantity = std::clamp(quantity + step, 1, 99);
    }
    static int upgrade = 0;
    std::snprintf(text, sizeof(text), "+%d", upgrade);
    if (const int step = Row("upgrade", T("Upgrade", "Улучшение"), text,
                             T("Weapons: given at this level (+0 .. +10).",
                               "Оружие: выдаётся с этим уровнем (+0 .. +10)."),
                             categories[category] == BbSaveEditor::WeaponCategory)) {
        upgrade = std::clamp(upgrade + step, 0, 10);
    }
    if (Row("give", T("Give", "Выдать"), nullptr,
            T("Goods over the carry limit go to storage.",
              "Сверх лимита предметы уходят в хранилище."),
            true) != 0) {
        const auto* item = shown[item_index];
        const std::uint32_t id = item->category == BbSaveEditor::WeaponCategory
                                     ? BbSaveEditor::UpgradedWeaponId(item->id, upgrade)
                                     : item->id;
        BbSaveEditor::GiveItem(item->category, id,
                               item->category == BbSaveEditor::GoodsCategory ? quantity : 1);
    }
}

} // namespace Ui

void Menu() {
    using namespace Ui;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 165));

    static const char* tab_en[] = {"Display", "Upscaler", "Game effects", "Controls", "Advanced", "Save"};
    static const char* tab_ru[] = {"Изображение", "Апскейлер", "Эффекты игры", "Управление",
                                   "Дополнительно", "Сохранение"};
    constexpr int tabs = 6;
    if (focus_request) {
        focus_request = false;
        state.focus_first = true;
    }
    const bool prev_tab = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_Q, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_PageUp, false);
    const bool next_tab = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_E, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_PageDown, false);
    if (prev_tab || next_tab) {
        state.tab = (state.tab + (next_tab ? 1 : tabs - 1)) % tabs;
        state.focus_first = true;
    }
    if (state.focus_first) {
        state.focus_first = false;
        state.focus = 0;
        state.scroll = true;
    }
    const auto pressed_any = [](std::initializer_list<ImGuiKey> keys) {
        for (const ImGuiKey key : keys) {
            if (ImGui::IsKeyPressed(key, true)) {
                return true;
            }
        }
        return false;
    };
    state.move = pressed_any({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp})       ? -1
                 : pressed_any({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}) ? 1
                                                                                                          : 0;
    state.activate = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    state.rows = 0;
    state.enabled.clear();

    const ImVec2 size(std::min(S(1180.0f), display.x - S(40.0f)), std::min(S(800.0f), display.y - S(40.0f)));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.032f, 0.028f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(36.0f), S(24.0f)));
    ImGui::PushFont(serif_font, 21.0f);
    ImGui::Begin("##bbport_settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    const float x0 = win.x + S(24.0f), x1 = win.x + size.x - S(24.0f);
    Ornament(draw, x0, x1, win.y + S(10.0f));
    Ornament(draw, x0, x1, win.y + size.y - S(10.0f));

    // Title.
    ImGui::PushFont(serif_font, 34.0f);
    const char* title = T("Settings", "Настройки");
    const float title_w = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((size.x - title_w) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Bright()));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // Close (mouse), top right.
    {
        const char* close = "×";
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorPos(ImVec2(size.x - S(64.0f), S(22.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Gold()));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Gold(0.2f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(Gold(0.35f)));
        if (ImGui::Button(close, ImVec2(S(34.0f), S(34.0f)))) {
            SetOpen(false);
        }
        ImGui::PopStyleColor(4);
        ImGui::PopItemFlag();
    }

    // Tabs: L1 ... R1, the current one gold and underlined.
    ImGui::SetCursorPosY(S(76.0f));
    {
        const float row_y = ImGui::GetCursorScreenPos().y;
        float widths[tabs], total = 0.0f;
        const float gap = S(46.0f);
        for (int i = 0; i < tabs; ++i) {
            widths[i] = ImGui::CalcTextSize(T(tab_en[i], tab_ru[i])).x;
            total += widths[i] + (i ? gap : 0.0f);
        }
        float x = win.x + (size.x - total) * 0.5f;
        ImGui::PushFont(sans_font, 15.0f);
        const ImVec2 l1 = ImGui::CalcTextSize("L1 / Q");
        draw->AddText(ImVec2(x - gap - l1.x, row_y + S(4.0f)), Gold(0.6f), "L1 / Q");
        draw->AddText(ImVec2(x + total + gap, row_y + S(4.0f)), Gold(0.6f), "E / R1");
        ImGui::PopFont();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int i = 0; i < tabs; ++i) {
            const char* name = T(tab_en[i], tab_ru[i]);
            ImGui::SetCursorScreenPos(ImVec2(x, row_y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(widths[i], S(32.0f)))) {
                state.tab = i;
                state.focus_first = true;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool current = i == state.tab;
            draw->AddText(ImVec2(x, row_y), current ? Bright() : hovered ? Plain() : Dim(), name);
            if (current) {
                draw->AddLine(ImVec2(x - S(6.0f), row_y + S(31.0f)), ImVec2(x + widths[i] + S(6.0f), row_y + S(31.0f)),
                              Gold(0.9f), S(1.5f));
            }
            x += widths[i] + gap;
        }
        ImGui::PopItemFlag();
        ImGui::SetCursorScreenPos(ImVec2(win.x + S(36.0f), row_y + S(44.0f)));
    }
    draw->AddLine(ImVec2(x0 + S(20.0f), ImGui::GetCursorScreenPos().y),
                  ImVec2(x1 - S(20.0f), ImGui::GetCursorScreenPos().y), Gold(0.25f), S(1.0f));
    ImGui::Dummy(ImVec2(0, S(6.0f)));

    // Rows (scrolling when they do not fit), then the description and the controls.
    const float footer = S(150.0f);
    state.next_hint = nullptr;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, S(2.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(6.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(Gold(0.30f)));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(Gold(0.55f)));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImGui::ColorConvertU32ToFloat4(Gold(0.75f)));
    ImGui::BeginChild("##rows", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    switch (state.tab) {
    case 0: DisplayTab(); break;
    case 1: UpscalerTab(); break;
    case 2: EffectsTab(); break;
    case 3: ControlsTab(); break;
    case 4: AdvancedTab(); break;
    default: SaveTab(); break;
    }
    if (RestartNeeded()) {
        ImGui::Spacing();
        if (Row("restart", T("Apply and restart the game", "Применить и перезапустить игру"), nullptr,
                T("Some changes apply only when the game starts (game patches, resolution at "
                  "startup, motion vectors). The game restarts from the title screen; your "
                  "progress is in the save.",
                  "Часть изменений применяется только при запуске игры (патчи игры, разрешение при "
                  "запуске, векторы движения). Игра перезапустится с титульного экрана; прогресс — в "
                  "сохранении."),
                true) != 0) {
            BbSettings::Save();
            runtime_restart();
        }
    }
    ImGui::Spacing();
    if (Row("quit", T("Quit to desktop", "Выйти на рабочий стол"), nullptr,
            T("Asks first: progress since the last autosave is lost. Holding L3 + R3 asks too.",
              "Сначала спросит: прогресс после последнего автосохранения теряется. Удержание L3 + R3 "
              "тоже спрашивает."),
            true) != 0) {
        SetOpen(false);
        quit_open = true;
    }
    // Up / down: the next enabled row (also off a row that became disabled).
    if (state.rows != 0) {
        int focus = std::clamp(state.focus, 0, state.rows - 1);
        const int dir = state.move != 0 ? state.move : 1;
        if (state.move != 0 || !state.enabled[focus]) {
            for (int n = state.move != 0 ? 1 : 0; n < state.rows; ++n) {
                const int i = ((focus + dir * n) % state.rows + state.rows) % state.rows;
                if (state.enabled[i]) {
                    if (i != state.focus) {
                        state.scroll = true;
                    }
                    focus = i;
                    break;
                }
            }
        }
        state.focus = focus;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    state.hint = state.next_hint;

    // Description of the focused setting.
    const float desc_y = win.y + size.y - footer;
    draw->AddLine(ImVec2(x0 + S(20.0f), desc_y + S(6.0f)), ImVec2(x1 - S(20.0f), desc_y + S(6.0f)), Gold(0.25f),
                  S(1.0f));
    ImGui::SetCursorScreenPos(ImVec2(win.x + S(64.0f), desc_y + S(18.0f)));
    ImGui::PushFont(serif_font, 17.0f);
    ImGui::BeginGroup(); // the lines start where the first one does
    ImGui::PushTextWrapPos(size.x - S(64.0f));
    if (RestartNeeded()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
        ImGui::TextUnformatted(T("Some changes apply after restarting the game.",
                                 "Часть изменений применится после перезапуска игры."));
        ImGui::PopStyleColor();
    }
    if (state.hint) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Plain(0.85f)));
        ImGui::TextUnformatted(state.hint);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::PopFont();

    // Controls.
    ImGui::PushFont(sans_font, 15.0f);
    const char* controls = T("Enter / Cross: change     Left / Right: value     Q / E, L1 / R1: tabs     "
                             "Esc / Circle: close     Insert, L3 + R3: menu     hold L3 + R3: quit",
                             "Enter / Крест: изменить     Влево / Вправо: значение     Q / E, L1 / R1: "
                             "вкладки     Esc / Круг: закрыть     Insert, L3 + R3: меню     удерживать L3 + R3: выход");
    const ImVec2 cs = ImGui::CalcTextSize(controls);
    draw->AddText(ImVec2(win.x + (size.x - cs.x) * 0.5f, win.y + size.y - S(40.0f)), Gold(0.65f), controls);
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    const char* upscaler_name = s.upscaler == BbSettings::UpscalerFsr3     ? "FSR 3.1"
                                : s.upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
                                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                                : s.upscaler == BbSettings::UpscalerTaa    ? "TAA"
                                : s.upscaler == BbSettings::UpscalerDlss   ? "DLSS"
                                : s.upscaler == BbSettings::UpscalerXess   ? "XeSS"
                                                                           : "";
    if (s.fg_active) {
        // Game frames -> frames shown: the interval of the shown images is half the game's.
        ImGui::Text("%.0f -> %.0f FPS FG  %s", s.fg_base_fps.load(), s.fg_presented_fps.load(),
                    upscaler_name);
    } else {
        ImGui::Text(BbSettings::MenuText("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s"),
                    frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg,
                    upscaler_name);
    }
    ImGui::End();
}

// Hold L3 + R3, or the menu's Quit row: asks before quitting, so a stray press does not drop
// progress since the last autosave. The quit goes through the window thread's close path, which
// runs the shutdown hooks before exiting. Cancel has the focus when the box opens.
void QuitBox() {
    using namespace Ui;
    static int choice = 0; // 0 Cancel, 1 Quit
    static int last_frame = -2;
    if (ImGui::GetFrameCount() != last_frame + 1) {
        choice = 0;
    }
    last_frame = ImGui::GetFrameCount();
    const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
        for (const ImGuiKey key : keys) {
            if (ImGui::IsKeyPressed(key, false)) {
                return true;
            }
        }
        return false;
    };
    if (pressed({ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                 ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_Tab})) {
        choice = 1 - choice;
    }
    bool activate = pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown});
    if (pressed({ImGuiKey_GamepadFaceRight})) {
        choice = 0;
        activate = true;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 165));
    const ImVec2 size(std::min(S(620.0f), display.x - S(40.0f)), S(230.0f));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.032f, 0.028f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##quit", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    const float x0 = win.x + S(24.0f), x1 = win.x + size.x - S(24.0f);
    Ornament(draw, x0, x1, win.y + S(10.0f));
    Ornament(draw, x0, x1, win.y + size.y - S(10.0f));

    ImGui::PushFont(serif_font, 30.0f);
    const char* title = T("Quit to desktop?", "Выйти на рабочий стол?");
    const ImVec2 ts = ImGui::CalcTextSize(title);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, win.y + S(36.0f)), Bright(), title);
    ImGui::PopFont();
    ImGui::PushFont(serif_font, 17.0f);
    const char* note = T("Progress since the last autosave is lost.",
                         "Прогресс после последнего автосохранения будет потерян.");
    const ImVec2 ns = ImGui::CalcTextSize(note);
    draw->AddText(ImVec2(win.x + (size.x - ns.x) * 0.5f, win.y + S(88.0f)), Plain(0.85f), note);
    ImGui::PopFont();

    ImGui::PushFont(serif_font, 24.0f);
    const char* labels[2] = {T("Cancel", "Отмена"), T("Quit", "Выйти")};
    const float gap = S(90.0f);
    const float w0 = ImGui::CalcTextSize(labels[0]).x, w1 = ImGui::CalcTextSize(labels[1]).x;
    float x = win.x + (size.x - (w0 + gap + w1)) * 0.5f;
    const float y = win.y + S(140.0f);
    for (int i = 0; i < 2; ++i) {
        const float w = i == 0 ? w0 : w1;
        ImGui::SetCursorScreenPos(ImVec2(x - S(10.0f), y - S(4.0f)));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##quit_choice", ImVec2(w + S(20.0f), S(40.0f)))) {
            choice = i;
            activate = true;
        }
        if (ImGui::IsItemHovered()) {
            choice = i;
        }
        ImGui::PopID();
        draw->AddText(ImVec2(x, y), i == choice ? Bright() : Dim(), labels[i]);
        if (i == choice) {
            draw->AddLine(ImVec2(x - S(6.0f), y + S(33.0f)), ImVec2(x + w + S(6.0f), y + S(33.0f)), Gold(0.9f),
                          S(1.5f));
        }
        x += w + gap;
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (activate) {
        if (choice == 1) {
            BbSettings::Save();
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_open = false;
    }
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel");
    ImGui::TextUnformatted("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// Party death (bbcoop.h): the other members down the left side, under the game's own bars, after
// Elden Ring Nightreign's party list; whom an attack would revive; this player's own down in the
// middle of the screen. Drawn shapes in the menu's colours; the HP bars are the game's own, from
// the player's game files (bbport_game_art.h), with drawn ones when those can't be read. No art
// from either game is shipped.
namespace Party {

bbcoop_panel panel{}; // present thread: as Visible() read it last
bool shown = false;
BbPartyLayout::Rect game{}; // present thread: the game's frame on screen, as Render() got it

bool Read() {
    shown = bbcoop_party_panel(&panel) != 0;
    return shown;
}

// The game's gauge art (BbGameArt::LoadGaugeArt) as an ImGui texture, read once the panel first
// shows; null when the game's files don't have it.
ImTextureData* gauge = nullptr;
bool gauge_tried = false;

// The party icons as an ImGui texture (a row of BbPartyIcons::Size squares), made once; null if the
// embedded atlas does not decode (the plain shapes are drawn then).
ImTextureData* icons = nullptr;

void LoadIcons() {
    const auto atlas = BbPartyIcons::Decode(bb_party_icons, size_t(bb_party_icons_end - bb_party_icons));
    if (!atlas) {
        std::printf("Overlay: the party icons did not decode; drawing plain shapes\n");
        return;
    }
    icons = IM_NEW(ImTextureData)();
    icons->Create(ImTextureFormat_RGBA32, atlas->width, atlas->height);
    std::memcpy(icons->GetPixels(), atlas->rgba.data(), atlas->rgba.size());
    ImGui::RegisterUserTexture(icons);
}

void LoadGauge() {
    if (gauge_tried) {
        return;
    }
    gauge_tried = true;
    LoadIcons();
    char path[1024];
    if (!bbcoop_game_file("dvdroot_ps4/menu/common.tpf.dcx", path, sizeof(path))) {
        return;
    }
    std::vector<std::uint8_t> file;
    if (FILE* f = std::fopen(path, "rb")) {
        std::uint8_t chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) {
            file.insert(file.end(), chunk, chunk + n);
        }
        std::fclose(f);
    }
    const auto art = BbGameArt::LoadGaugeArt(file);
    if (!art) {
        std::printf("Overlay: the game's HP bar art was not found in %s; drawing its own\n", path);
        return;
    }
    gauge = IM_NEW(ImTextureData)();
    gauge->Create(ImTextureFormat_RGBA32, art->width, art->height);
    std::memcpy(gauge->GetPixels(), art->rgba.data(), art->rgba.size());
    ImGui::RegisterUserTexture(gauge);
    std::printf("Overlay: party HP bars use the game's own art (%dx%d)\n", art->width,
                art->height);
}

/// A part of the gauge art drawn into [p0, p1].
void GaugePart(ImDrawList* draw, ImVec2 p0, ImVec2 p1, float sx0, float sy0, float sx1, float sy1,
               float alpha) {
    const float w = float(gauge->Width), h = float(gauge->Height);
    draw->AddImage(gauge->GetTexRef(), p0, p1, ImVec2(sx0 / w, sy0 / h), ImVec2(sx1 / w, sy1 / h),
                   ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
}

/// The game's HP bar at `b0`, `width` wide, `fill` of it red: its framed track (end caps kept,
/// the middle stretched) and the red fill cut where the HP ends, as the game's own bar shrinks.
/// False when the game's art is not loaded.
bool GameHpBar(ImDrawList* draw, ImVec2 b0, float width, float height, float fill, float alpha) {
    if (gauge == nullptr) {
        return false;
    }
    using BbGameArt::GaugeFill;
    using BbGameArt::GaugeFrame;
    using BbGameArt::GaugeFrameCap;
    const float s = height / float(GaugeFrame.y1 - GaugeFrame.y0), cap = GaugeFrameCap * s;
    const float x1 = b0.x + width, y1 = b0.y + height;
    const float fy0 = float(GaugeFrame.y0), fy1 = float(GaugeFrame.y1);
    GaugePart(draw, b0, ImVec2(b0.x + cap, y1), float(GaugeFrame.x0), fy0,
              float(GaugeFrame.x0 + GaugeFrameCap), fy1, alpha);
    GaugePart(draw, ImVec2(b0.x + cap, b0.y), ImVec2(x1 - cap, y1),
              float(GaugeFrame.x0 + GaugeFrameCap), fy0, float(GaugeFrame.x1 - GaugeFrameCap),
              fy1, alpha);
    GaugePart(draw, ImVec2(x1 - cap, b0.y), ImVec2(x1, y1), float(GaugeFrame.x1 - GaugeFrameCap),
              fy0, float(GaugeFrame.x1), fy1, alpha);
    // The fill sits inside the track as in the game's HUD: 6 px in from its left end, 4 from its
    // right, centred top to bottom.
    const float fill_h = float(GaugeFill.y1 - GaugeFill.y0) * s;
    const float left = b0.x + 6.0f * s, right = x1 - 4.0f * s;
    const float top = b0.y + (height - fill_h) * 0.5f;
    if (fill > 0.0f) {
        const float cut = std::clamp(fill, 0.0f, 1.0f);
        GaugePart(draw, ImVec2(left, top), ImVec2(left + (right - left) * cut, top + fill_h),
                  float(GaugeFill.x0), float(GaugeFill.y0),
                  float(GaugeFill.x0) + float(GaugeFill.x1 - GaugeFill.x0) * cut,
                  float(GaugeFill.y1), alpha);
    }
    return true;
}

ImU32 Rgba(float r, float g, float b, float a) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

float Alpha(ImU32 color) {
    return float((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
}

/// Text with a soft shadow (readable over bright scenes); `align` 0.5 centres it on `at`.
void Text(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text,
          float align = 0.0f) {
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    at.x = std::floor(at.x - extent.x * align);
    const float shadow = std::max(1.0f, size * 0.07f);
    draw->AddText(font, size, ImVec2(at.x + shadow, at.y + shadow),
                  Rgba(0, 0, 0, 0.8f * Alpha(color)), text);
    draw->AddText(font, size, at, color, text);
}

// The party palette, as the game's HUD: bone white, ash, iron and blood.
ImU32 Bone(float alpha = 1.0f) {
    return Rgba(0.86f, 0.84f, 0.78f, alpha);
}
ImU32 Blood(float alpha = 1.0f) {
    return Rgba(0.77f, 0.16f, 0.13f, alpha);
}
ImU32 Iron(float alpha = 1.0f) {
    return Rgba(0.23f, 0.21f, 0.20f, alpha);
}

/// One of the party icons, `size` square, centred on `centre`. False without the icons.
bool Icon(ImDrawList* draw, BbPartyIcons::Icon icon, ImVec2 centre, float size, float alpha) {
    if (icons == nullptr) {
        return false;
    }
    const float h = size * 0.5f;
    draw->AddImage(icons->GetTexRef(), ImVec2(centre.x - h, centre.y - h),
                   ImVec2(centre.x + h, centre.y + h), ImVec2(BbPartyIcons::U0(icon), 0.0f),
                   ImVec2(BbPartyIcons::U1(icon), 1.0f),
                   ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
    return true;
}

/// The red share of a ring (an icon's, in its 128 units), filling clockwise from the top.
void RingShare(ImDrawList* draw, ImVec2 centre, float size, BbPartyIcons::Ring ring, float share,
               float alpha) {
    if (share <= 0.0f) {
        return;
    }
    const float unit = size / float(BbPartyIcons::Size);
    const float top = -IM_PI * 0.5f;
    draw->PathArcTo(centre, ring.radius * unit, top, top + 2.0f * IM_PI * std::min(share, 1.0f),
                    64);
    draw->PathStroke(Blood(alpha), 0, ring.width * unit);
}

/// The revive ring: the iron ring of the icons, its red share and the percentage in its middle.
/// Plain circles without the icons.
void ReviveRing(ImDrawList* draw, ImVec2 centre, float radius, u32 landed, u32 needed,
                float alpha) {
    const float share = bbcoop_revive_share(landed, needed), size = radius * 2.0f;
    if (!Icon(draw, BbPartyIcons::Icon::ReviveRing, centre, size, alpha)) {
        draw->AddCircleFilled(centre, radius, Rgba(0.03f, 0.03f, 0.03f, 0.85f * alpha));
        draw->AddCircle(centre, radius * 0.75f, Iron(alpha), 0, radius * 0.19f);
    }
    RingShare(draw, centre, size, BbPartyIcons::ReviveRing, share, alpha);
    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", int(std::lround(share * 100.0f)));
    const float font = radius * 0.42f;
    Text(draw, serif_font, font, ImVec2(centre.x, centre.y - font * 0.55f), Bone(alpha), text, 0.5f);
}

/// Revive progress as text: the share of the damage dealt.
int RevivePercent(u32 landed, u32 needed) {
    return int(std::lround(bbcoop_revive_share(landed, needed) * 100.0f));
}

void MemberList(ImDrawList* draw, float pulse) {
    using Ui::S;
    using Ui::T;
    const float x = BbPartyLayout::ListLeft(game, base_scale), height = S(54.0f), gap = S(10.0f),
                bar = S(240.0f);
    float y = BbPartyLayout::ListTop(game);
    char line[64];
    for (int i = 0; i < panel.member_count; ++i) {
        const bbcoop_panel_member& member = panel.members[i];
        const bool down = member.vital == BBCOOP_VITAL_DOWN;
        const bool dead = member.vital == BBCOOP_VITAL_DEAD;
        const float a = member.here ? 1.0f : 0.55f;
        const ImU32 clear = Rgba(0.0f, 0.0f, 0.0f, 0.0f);
        // A shade fading out to the right behind the row: red while down. No edge lines.
        const float mid_y = y + height * 0.5f;
        const ImU32 frame = down ? Rgba(0.46f, 0.04f, 0.06f, (0.55f + 0.20f * pulse) * a)
                                 : Rgba(0.03f, 0.03f, 0.03f, 0.62f * a);
        draw->AddRectFilledMultiColor(ImVec2(x + S(8.0f), y), ImVec2(x + S(330.0f), y + height),
                                      frame, clear, clear, frame);
        // The icon: the downed drop with its ring filling red with the revive, the headstone
        // while dead, the lantern on another map, the hunter while up.
        const ImVec2 icon(x + S(34.0f), mid_y);
        const float icon_size = S(48.0f);
        using BbPartyIcons::Icon;
        if (down && Party::Icon(draw, Icon::Downed, icon, icon_size, a)) {
            RingShare(draw, icon, icon_size, BbPartyIcons::DownedRing,
                      bbcoop_revive_share(member.landed, member.needed),
                      a * (0.80f + 0.20f * pulse));
        } else if (dead && Party::Icon(draw, Icon::Fallen, icon, icon_size, a)) {
        } else if (!down && !dead &&
                   Party::Icon(draw, member.here ? Icon::Hunter : Icon::Elsewhere, icon,
                               icon_size, a)) {
        } else if (down) {
            ReviveRing(draw, icon, S(21.0f), member.landed, member.needed,
                       a * (0.80f + 0.20f * pulse));
        } else if (dead) {
            const float r = S(7.0f);
            draw->AddLine(ImVec2(icon.x - r, icon.y - r), ImVec2(icon.x + r, icon.y + r),
                          Ui::Dim(a), S(2.0f));
            draw->AddLine(ImVec2(icon.x + r, icon.y - r), ImVec2(icon.x - r, icon.y + r),
                          Ui::Dim(a), S(2.0f));
        } else {
            const float r = S(7.0f);
            draw->AddQuadFilled(ImVec2(icon.x, icon.y - r), ImVec2(icon.x + r, icon.y),
                                ImVec2(icon.x, icon.y + r), ImVec2(icon.x - r, icon.y), Bone(a));
        }
        const float text_x = x + S(64.0f);
        const char* name = member.name[0] != '\0' ? member.name : T("Hunter", "Охотник");
        const ImU32 name_color = down   ? Rgba(1.0f, 0.86f, 0.82f, a)
                                 : dead ? Ui::Dim(a)
                                        : Rgba(0.95f, 0.94f, 0.92f, a);
        Text(draw, serif_font, S(21.0f), ImVec2(text_x, y + S(4.0f)), name_color, name);
        if (!member.here) {
            const float name_w = serif_font->CalcTextSizeA(S(21.0f), FLT_MAX, 0.0f, name).x;
            Text(draw, sans_font, S(13.0f), ImVec2(text_x + name_w + S(10.0f), y + S(10.0f)),
                 Ui::Dim(0.95f), T("elsewhere", "в другом месте"));
        }
        const float status_y = y + S(33.0f);
        if (down) {
            std::snprintf(line, sizeof(line), "%s  %d%%", T("Down", "Повержен"),
                          RevivePercent(member.landed, member.needed));
            Text(draw, sans_font, S(14.0f), ImVec2(text_x, status_y - S(3.0f)),
                 Rgba(1.0f, 0.52f, 0.46f, a), line);
        } else if (dead) {
            Text(draw, sans_font, S(14.0f), ImVec2(text_x, status_y - S(3.0f)), Ui::Dim(a),
                 T("Dead", "Погиб"));
        } else if (const float fill =
                       member.hp_max > 0
                           ? std::clamp(float(member.hp) / float(member.hp_max), 0.0f, 1.0f)
                           : 0.0f;
                   !GameHpBar(draw, ImVec2(text_x, status_y - S(3.0f)), bar, S(13.0f), fill, a)) {
            // The game's art could not be read: HP red, the rest grey, framed in a thin line.
            const ImVec2 b0(text_x, status_y), b1(text_x + bar, status_y + S(7.0f));
            const float split = b0.x + bar * fill;
            draw->AddRectFilled(ImVec2(b0.x - S(1.0f), b0.y - S(1.0f)),
                                ImVec2(b1.x + S(1.0f), b1.y + S(1.0f)),
                                Rgba(0.0f, 0.0f, 0.0f, 0.75f * a));
            draw->AddRectFilledMultiColor(
                ImVec2(split, b0.y), b1, Rgba(0.36f, 0.36f, 0.38f, 0.8f * a),
                Rgba(0.30f, 0.30f, 0.32f, 0.8f * a), Rgba(0.22f, 0.22f, 0.24f, 0.8f * a),
                Rgba(0.26f, 0.26f, 0.28f, 0.8f * a));
            draw->AddRectFilledMultiColor(
                b0, ImVec2(split, b1.y), Rgba(0.50f, 0.07f, 0.06f, a), Rgba(0.80f, 0.20f, 0.14f, a),
                Rgba(0.60f, 0.10f, 0.08f, a), Rgba(0.38f, 0.04f, 0.04f, a));
            if (fill > 0.0f && fill < 1.0f) {
                draw->AddLine(ImVec2(split, b0.y), ImVec2(split, b1.y),
                              Rgba(1.0f, 0.86f, 0.80f, 0.9f * a), S(1.5f));
            }
            draw->AddRect(ImVec2(b0.x - S(1.0f), b0.y - S(1.0f)),
                          ImVec2(b1.x + S(1.0f), b1.y + S(1.0f)),
                          Rgba(0.80f, 0.80f, 0.84f, 0.45f * a), 0.0f, 0, S(1.0f));
        }
        y += height + gap;
    }
}

/// An attack now would revive someone: the prompt low in the middle, as the game's own prompts.
void RevivePrompt(ImDrawList* draw) {
    using Ui::S;
    using Ui::T;
    const bbcoop_panel_member& member = panel.members[panel.revive_target];
    char line[96];
    std::snprintf(
        line, sizeof(line),
        T("R1 / R2: strike to revive %s  %d%%", "R1 / R2: ударьте, чтобы поднять %s  %d%%"),
        member.name[0] != '\0' ? member.name : T("Hunter", "Охотник"),
        RevivePercent(member.landed, member.needed));
    const float size = S(21.0f), mid = game.x + game.w * 0.5f,
                y = BbPartyLayout::RevivePromptY(game);
    const float w = serif_font->CalcTextSizeA(size, FLT_MAX, 0.0f, line).x + S(120.0f);
    const ImU32 band = Rgba(0.0f, 0.0f, 0.0f, 0.62f), clear = Rgba(0.0f, 0.0f, 0.0f, 0.0f);
    draw->AddRectFilledMultiColor(ImVec2(mid - w * 0.5f, y - S(10.0f)), ImVec2(mid, y + S(38.0f)),
                                  clear, band, band, clear);
    draw->AddRectFilledMultiColor(ImVec2(mid, y - S(10.0f)), ImVec2(mid + w * 0.5f, y + S(38.0f)),
                                  band, clear, clear, band);
    Ui::Ornament(draw, mid - w * 0.5f, mid + w * 0.5f, y - S(10.0f));
    Ui::Ornament(draw, mid - w * 0.5f, mid + w * 0.5f, y + S(38.0f));
    const float text_w = w - S(120.0f);
    Icon(draw, BbPartyIcons::Icon::Strike, ImVec2(mid - text_w * 0.5f - S(26.0f), y + S(14.0f)),
         S(40.0f), 1.0f);
    Text(draw, serif_font, size, ImVec2(mid, y + S(2.0f)), Bone(), line, 0.5f);
}

/// This player is down: a red edge around the screen, its revive ring and how to give up.
void OwnDown(ImDrawList* draw, float pulse) {
    using namespace BbPartyLayout;
    using Ui::S;
    using Ui::T;
    // The red edge frames the game's picture (inside any black bars).
    const ImVec2 g0(game.x, game.y), g1(game.x + game.w, game.y + game.h);
    const float edge = game.h * 0.24f;
    const ImU32 red = Rgba(0.30f, 0.0f, 0.0f, 0.50f + 0.12f * pulse);
    const ImU32 clear = Rgba(0.30f, 0.0f, 0.0f, 0.0f);
    draw->AddRectFilledMultiColor(g0, ImVec2(g1.x, g0.y + edge), red, red, clear, clear);
    draw->AddRectFilledMultiColor(ImVec2(g0.x, g1.y - edge), g1, clear, clear, red, red);
    draw->AddRectFilledMultiColor(g0, ImVec2(g0.x + edge, g1.y), red, clear, clear, red);
    draw->AddRectFilledMultiColor(ImVec2(g1.x - edge, g0.y), g1, clear, red, red, clear);

    const float mid = game.x + game.w * 0.5f;
    const ImVec2 centre(mid, DownRingY(game, base_scale));
    ReviveRing(draw, centre, S(DownRingRadius), panel.landed, panel.needed,
               0.85f + 0.15f * pulse);
    // The party HUD's YOU ARE DOWN banner says it in the game's own letters (BB_PARTY_HUD=0: here).
    static const bool hud_banner = [] {
        const char* setting = std::getenv("BB_PARTY_HUD");
        return setting == nullptr || std::string_view{setting} != "0";
    }();
    if (!hud_banner) {
        Text(draw, serif_font, S(30.0f), ImVec2(mid, centre.y + S(DownTitleBelow)), Bone(),
             T("You are down", "Вы повержены"), 0.5f);
    }
    Text(draw, serif_font, S(18.0f), ImVec2(mid, centre.y + S(DownHintBelow)), Ui::Plain(0.9f),
         T("A party member can raise you by striking you",
           "Союзник может поднять вас, нанося вам удары"),
         0.5f);
    const char* give_up = T("Hold Circle to give up", "Удерживайте Круг, чтобы сдаться");
    const float give_up_w = sans_font->CalcTextSizeA(S(15.0f), FLT_MAX, 0.0f, give_up).x;
    Icon(draw, BbPartyIcons::Icon::GiveUp,
         ImVec2(mid - give_up_w * 0.5f - S(18.0f), centre.y + S(DownGiveUpBelow) + S(9.0f)),
         S(28.0f), 0.95f);
    Text(draw, sans_font, S(15.0f), ImVec2(mid, centre.y + S(DownGiveUpBelow)), Bone(0.85f),
         give_up, 0.5f);
    // The give-up bar: an empty track with a faint frame (so it shows before Circle is held),
    // filling while it's held.
    const float y = centre.y + S(DownBarBelow), w = S(180.0f), h = S(DownBarHeight);
    const ImVec2 b0(mid - w * 0.5f, y), b1(mid + w * 0.5f, y + h);
    draw->AddRectFilled(b0, b1, Rgba(0.0f, 0.0f, 0.0f, 0.6f));
    if (panel.give_up > 0.0f) {
        draw->AddRectFilled(b0, ImVec2(b0.x + w * std::clamp(panel.give_up, 0.0f, 1.0f), b1.y),
                            Blood());
    }
    draw->AddRect(ImVec2(b0.x - S(1.0f), b0.y - S(1.0f)), ImVec2(b1.x + S(1.0f), b1.y + S(1.0f)),
                  Bone(0.45f), 0.0f, 0, S(1.0f));
}

void Draw() {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 4.0f);
    MemberList(draw, pulse);
    if (panel.vital == BBCOOP_VITAL_DOWN) {
        OwnDown(draw, pulse);
    } else if (panel.revive_target >= 0 && panel.revive_target < panel.member_count) {
        RevivePrompt(draw);
    }
}

} // namespace Party

/// BB_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1,
/// consumed when the file appears (it is removed), one key press per frame.
void ScriptedKeys() {
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point last_check{};
    static std::vector<std::string> queue;
    static bool release = false;
    static ImGuiKey held = ImGuiKey_None;
    if (!path) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (release) {
        io.AddKeyEvent(held, false);
        release = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (queue.empty() && now - last_check > std::chrono::milliseconds(100)) {
        last_check = now;
        if (FILE* f = std::fopen(path, "r")) {
            char token[32];
            while (std::fscanf(f, "%31s", token) == 1) {
                queue.emplace_back(token);
            }
            std::fclose(f);
            std::remove(path);
        }
    }
    if (queue.empty()) {
        return;
    }
    const std::string token = queue.front();
    queue.erase(queue.begin());
    if (token == "toggle") {
        SetOpen(!menu_open);
        return;
    }
    held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (held != ImGuiKey_None) {
        io.AddKeyEvent(held, true);
        release = true;
    }
}

} // namespace

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    // The menu moves its own focus (Ui::State); ImGui only gets the keys.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    sans_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                               int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
    ImFontConfig serif_config;
    serif_config.FontDataOwnedByAtlas = false;
    serif_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_serif_ttf),
                                                int(bb_serif_ttf_end - bb_serif_ttf), 21.0f, &serif_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// The Controls tab waits for a press (capture.input): the next key, mouse button, wheel step or
// gamepad button is its binding, in the names runtime_pad.c reads. True: the event was taken.
bool CaptureEvent(const SDL_Event& event) {
    if (capture.input < 0 || capture.done || !menu_open) {
        return false;
    }
    const auto modifiers = [](SDL_Keymod mod) {
        std::string text;
        if (mod & SDL_KMOD_SHIFT) text += "Shift+";
        if (mod & SDL_KMOD_CTRL) text += "Ctrl+";
        if (mod & SDL_KMOD_ALT) text += "Alt+";
        return text;
    };
    const auto bind = [](const char* kind, std::string name) {
        capture.kind = kind;
        capture.name = std::move(name);
        capture.done = true;
        return true;
    };
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (event.key.repeat) {
            return true;
        }
        const SDL_Scancode code = event.key.scancode;
        if (event.key.key == SDLK_INSERT) {
            capture = {};
            return false; // the menu's own key closes it
        }
        if (code == SDL_SCANCODE_ESCAPE) {
            capture = {};
            return true;
        }
        if (code == SDL_SCANCODE_DELETE) {
            return bind("key", "");
        }
        const bool is_modifier = code == SDL_SCANCODE_LSHIFT || code == SDL_SCANCODE_RSHIFT ||
                                 code == SDL_SCANCODE_LCTRL || code == SDL_SCANCODE_RCTRL ||
                                 code == SDL_SCANCODE_LALT || code == SDL_SCANCODE_RALT;
        if (is_modifier) { // alone, it is bound when it comes up; with another key, a combination
            capture.modifier = code;
            return true;
        }
        return bind("key", modifiers(event.key.mod) + SDL_GetScancodeName(code));
    }
    case SDL_EVENT_KEY_UP:
        if (capture.modifier != SDL_SCANCODE_UNKNOWN && event.key.scancode == capture.modifier) {
            return bind("key", SDL_GetScancodeName(capture.modifier));
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        static const char* const names[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};
        if (event.button.button < 1 || event.button.button > 5) {
            return true;
        }
        return bind("key", modifiers(SDL_GetModState()) + names[event.button.button]);
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return true;
    case SDL_EVENT_MOUSE_WHEEL: {
        const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        if (y == 0.0f) {
            return true;
        }
        return bind("key", modifiers(SDL_GetModState()) + (y > 0.0f ? "Wheel Up" : "Wheel Down"));
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const char* name = SDL_GetGamepadStringForButton(SDL_GamepadButton(event.gbutton.button));
        if (!capture.pad_ok || !name) {
            return true; // the movement and camera keys are keyboard only
        }
        return bind("pad", name);
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if ((event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            event.gaxis.value > 16000) {
            if (!capture.pad_ok) {
                return true;
            }
            return bind("pad", event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "lefttrigger" : "righttrigger");
        }
        return false;
    default:
        return false;
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (CaptureEvent(event)) {
        return true;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open || quit_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        // bbport: numpad 0 with Num Lock off is Insert on keyboards without the separate key, and
        // SDL reports it as the keypad key.
        const bool insert = event.key.key == SDLK_INSERT ||
                            (event.key.scancode == SDL_SCANCODE_KP_0 && !(event.key.mod & SDL_KMOD_NUM));
        if (down && !event.key.repeat && (insert || (is_open && event.key.key == SDLK_ESCAPE))) {
            quit_open = false;
            SetOpen(insert && !is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            combo_held = true;
            combo_fired = false;
            combo_since = std::chrono::steady_clock::now();
            return true;
        }
        if (!down && combo_held) {
            combo_held = false;
            if (!combo_fired) { // a tap: toggles the settings menu, or closes the quit box
                quit_open = false;
                SetOpen(!is_open);
            }
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (down && button == SDL_GAMEPAD_BUTTON_EAST) { // Circle: back to the game
            quit_open = false;
            SetOpen(false);
            return true;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    const bool party = initialized && Party::Read(); // read every frame, whatever else is drawn
    return initialized && (menu_open || quit_open || prompt_active || BbSettings::Get().show_fps || party);
}

bool MenuOpen() {
    return menu_open || quit_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || quit_open || prompt_active;
}

void Tick() {
    std::scoped_lock lock{imgui_mutex};
    if (initialized && combo_held && !combo_fired &&
        std::chrono::steady_clock::now() - combo_since >= quit_hold) {
        combo_fired = true;
        SetOpen(false);
        quit_open = true;
        SyncMousePos();
    }
}

void TestTick() {
    // BB_TEST_L3R3=<seconds>:<held seconds>[,...]: presses L3+R3 through the SDL event queue, so
    // a harness run exercises the tap (settings menu) and the hold (quit box) without a pad.
    static std::vector<std::pair<double, bool>> presses = [] {
        std::vector<std::pair<double, bool>> parsed;
        const char* env = std::getenv("BB_TEST_L3R3");
        for (const char* at = env; at && *at;) {
            char* end = nullptr;
            const double start = std::strtod(at, &end);
            if (end == at || *end != ':') {
                break;
            }
            const double held = std::strtod(end + 1, &end);
            parsed.emplace_back(start, true);
            parsed.emplace_back(start + held, false);
            at = *end == ',' ? end + 1 : end;
        }
        return parsed;
    }();
    static const auto test_start = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - test_start).count();
    while (initialized && !presses.empty() && elapsed >= presses.front().first) {
        const bool down = presses.front().second;
        presses.erase(presses.begin());
        for (const u8 button : {SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            event.gbutton.button = button;
            event.gbutton.down = down;
            SDL_PushEvent(&event);
        }
        std::printf("Overlay: test L3+R3 %s at %.1f s\n", down ? "down" : "up", elapsed);
    }

    static std::vector<double> times = [] {
        std::vector<double> parsed;
        const char* env = std::getenv("BB_TEST_OVERLAY_AT");
        for (const char* at = env; at && *at;) {
            char* end = nullptr;
            const double seconds = std::strtod(at, &end);
            if (end == at) {
                break;
            }
            parsed.push_back(seconds);
            at = *end == ',' ? end + 1 : end;
        }
        return parsed;
    }();
    static const auto start = std::chrono::steady_clock::now();
    if (times.empty() || !initialized) {
        return;
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    while (!times.empty() && now >= times.front()) {
        times.erase(times.begin());
        SetOpen(!menu_open);
        std::printf("Overlay: test toggle at %.1f s, menu %s\n", now, menu_open ? "open" : "closed");
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent, vk::Rect2D game) {

    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (std::getenv("BB_MENU_KEYS_FILE") && initialized) {
        std::scoped_lock lock{imgui_mutex};
        ScriptedKeys();
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = BbPartyLayout::Scale(float(extent.height));
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    if (Party::shown) {
        Party::LoadGauge(); // before NewFrame, which hands new textures to the backend
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (Party::shown && !menu_open) {
        Party::game = {float(game.offset.x), float(game.offset.y), float(game.extent.width),
                       float(game.extent.height)};
        Party::Draw();
    }
    if (menu_open) {
        Menu();
    }
    if (quit_open) {
        QuitBox();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
