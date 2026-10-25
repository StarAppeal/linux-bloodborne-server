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
#include <cctype>
#include <string>
#include <string_view>
#include <optional>
#include <map>
#include <span>
#include <memory>
#include <vector>

#include <SDL3/SDL.h>
#include "../bbcoop.h"
#include "../bbgpu.h"
#include "bbport_game_art.h"
#include "bbport_prompts.h"
#include "bbport_coop_art.h"
#include "bbport_key_guide.h"
#include "bbport_map_3d.h"
#include "bbport_map_charts.h"
#include "bbport_pad_rebind.h"
#include "bbport_party_icons.h"
#include "bbport_party_layout.h"
#include "bbport_save_editor.h"
#include "bbport_frame_gen_pace.h"
#include "bbport_guest_hooks.h"
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
// The co-op screens' own art (art/coop_art.bin, drawn for the port: shim/bbport_coop_art.h).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_coop_art\n"
    "bb_coop_art:\n"
    ".incbin \"" BB_COOP_ART_PATH "\"\n"
    ".global bb_coop_art_end\n"
    "bb_coop_art_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_coop_art\n"
    ".global bb_coop_art\n"
    "bb_coop_art:\n"
    ".incbin \"" BB_COOP_ART_PATH "\"\n"
    ".hidden bb_coop_art_end\n"
    ".global bb_coop_art_end\n"
    "bb_coop_art_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_coop_art[];
extern "C" const unsigned char bb_coop_art_end[];

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
// The team loading screen covers the game's own load: the settings menu stays shut meanwhile
// (Kaniel 2026-10-14: Insert during that load lost the GPU on one game, froze another).
std::atomic<bool> loading_cover{false};
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

// The ImGui backend's descriptor pool: one set per texture (the font atlas, the party art and
// icons, the team screens' menu art, and with button_icons=auto the key guide icons of every
// set: 98 textures), and the texture count last printed for the rig. Past it, menu art came out
// garbled (Kaniel 2026-10-19: the settings page's ornaments and slider tracks).
constexpr int OverlayDescriptorSets = 256;
int textures_logged = -1;

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
    if (value && loading_cover) {
        return;
    }
    if (menu_open.exchange(value) == value) {
        return;
    }
    if (value) {
        focus_request = true;
        SyncMousePos();
    }
    if (!value && dirty) {
        dirty = false;
        std::printf("Overlay: settings changed in the menu, saved\n");
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
    // The map screen's keys (moving the picture, the player, closing it, floors).
    case SDLK_W: return ImGuiKey_W;
    case SDLK_A: return ImGuiKey_A;
    case SDLK_S: return ImGuiKey_S;
    case SDLK_D: return ImGuiKey_D;
    case SDLK_C: return ImGuiKey_C;
    case SDLK_M: return ImGuiKey_M;
    case SDLK_F: return ImGuiKey_F;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_Z: return ImGuiKey_Z;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_EQUALS: return ImGuiKey_Equal;
    case SDLK_MINUS: return ImGuiKey_Minus;
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
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ImGuiKey_GamepadR3;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ImGuiKey_GamepadL3;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return ImGuiKey_GamepadBack;
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

// Kaniel 2026-10-17: the co-op screens' own art, drawn for the port (Nano Banana, flat gold on dark;
// shim/bbport_coop_art.h): the hunters' round medallions and sigils in place of the game's square
// slots and runes, the reliquary cards, the dividers and title scrollwork, the ready, leader and
// invitation marks, and the night city behind the team loading screen. Packed into one texture.
namespace CoopArt {
using BbCoopArt::Piece;
using BbCoopArt::Share;

ImTextureData* texture = nullptr;
std::vector<BbCoopArt::Rect> rects;
bool tried = false;

void Load() {
    if (tried) {
        return;
    }
    tried = true;
    auto atlas = BbCoopArt::Decode(bb_coop_art, size_t(bb_coop_art_end - bb_coop_art));
    if (!atlas) {
        std::printf("Overlay: the co-op art did not decode; the game's menu art is used\n");
        return;
    }
    // Kaniel 2026-10-18: in the souls counter's grey, not gold.
    BbCoopArt::Greyed(*atlas, BbCoopArt::SoulsGrey);
    texture = IM_NEW(ImTextureData)();
    texture->Create(ImTextureFormat_RGBA32, atlas->width, atlas->height);
    std::memcpy(texture->GetPixels(), atlas->rgba.data(), atlas->rgba.size());
    ImGui::RegisterUserTexture(texture);
    rects = atlas->rects;
}

bool Has() {
    return texture != nullptr;
}

ImVec2 SizeOf(Piece piece) {
    const auto& r = rects[size_t(piece)];
    return ImVec2(float(r.width), float(r.height));
}

ImU32 Tint(float alpha) {
    return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));
}

/// The `part` of `piece` stretched into [p0, p1].
void Part(ImDrawList* draw, Piece piece, ImVec2 p0, ImVec2 p1, Share part, float alpha) {
    const auto& r = rects[size_t(piece)];
    const float w = float(texture->Width), h = float(texture->Height);
    const auto u = [&](float share) { return (float(r.x) + share * float(r.width)) / w; };
    const auto v = [&](float share) { return (float(r.y) + share * float(r.height)) / h; };
    draw->AddImage(texture->GetTexRef(), p0, p1, ImVec2(u(part.x0), v(part.y0)), ImVec2(u(part.x1), v(part.y1)),
                   Tint(alpha));
}

void Draw(ImDrawList* draw, Piece piece, ImVec2 p0, ImVec2 p1, float alpha) {
    Part(draw, piece, p0, p1, Share{0.0f, 0.0f, 1.0f, 1.0f}, alpha);
}

/// `piece` `height` tall (its shape kept), centred on `centre`.
void Centred(ImDrawList* draw, Piece piece, ImVec2 centre, float height, float alpha) {
    const ImVec2 size = SizeOf(piece);
    const float width = height * size.x / size.y;
    Draw(draw, piece, ImVec2(centre.x - width * 0.5f, centre.y - height * 0.5f),
         ImVec2(centre.x + width * 0.5f, centre.y + height * 0.5f), alpha);
}

/// The reliquary card in [at, end], of any shape: its corners, arch and flourish at one scale (the
/// card's own shape fitted in), its edges and inside stretched between them.
void Card(ImDrawList* draw, ImVec2 at, ImVec2 end, float alpha) {
    namespace P = BbCoopArt::CardParts;
    const ImVec2 size = SizeOf(Piece::Card);
    const float w = end.x - at.x, h = end.y - at.y;
    const float k = std::min(w / size.x, h / size.y);
    const float cw = P::CornerX * size.x * k, ch = P::CornerY * size.y * k;
    Part(draw, Piece::Card, ImVec2(at.x + cw, at.y + ch), ImVec2(end.x - cw, end.y - ch), P::Inside, alpha);
    Part(draw, Piece::Card, ImVec2(at.x + cw, at.y), ImVec2(end.x - cw, at.y + ch), P::Top, alpha);
    Part(draw, Piece::Card, ImVec2(at.x + cw, end.y - ch), ImVec2(end.x - cw, end.y), P::Bottom, alpha);
    Part(draw, Piece::Card, ImVec2(at.x, at.y + ch), ImVec2(at.x + cw, end.y - ch), P::Left, alpha);
    Part(draw, Piece::Card, ImVec2(end.x - cw, at.y + ch), ImVec2(end.x, end.y - ch), P::Right, alpha);
    const Share corners[4] = {{0.0f, 0.0f, P::CornerX, P::CornerY},
                              {1.0f - P::CornerX, 0.0f, 1.0f, P::CornerY},
                              {0.0f, 1.0f - P::CornerY, P::CornerX, 1.0f},
                              {1.0f - P::CornerX, 1.0f - P::CornerY, 1.0f, 1.0f}};
    const ImVec2 places[4] = {at, ImVec2(end.x - cw, at.y), ImVec2(at.x, end.y - ch), ImVec2(end.x - cw, end.y - ch)};
    for (int i = 0; i < 4; ++i) {
        Part(draw, Piece::Card, places[i], ImVec2(places[i].x + cw, places[i].y + ch), corners[i], alpha);
    }
    const float mid = (at.x + end.x) * 0.5f;
    const auto stamp = [&](Share part, float y0) {
        const float pw = (part.x1 - part.x0) * size.x * k, ph = (part.y1 - part.y0) * size.y * k;
        Part(draw, Piece::Card, ImVec2(mid - pw * 0.5f, y0), ImVec2(mid + pw * 0.5f, y0 + ph), part, alpha);
    };
    stamp(P::Arch, at.y);
    stamp(P::Flourish, end.y - (P::Flourish.y1 - P::Flourish.y0) * size.y * k);
}

} // namespace CoopArt

// Kaniel 2026-10-14: the team screen and the team loading screen in the game's own style: its
// menu art (the wall, the title picture, the cards, the gold selection frame, the rows, the
// ornaments, the button icons) and its menu font, read at runtime from the player's game files
// (bbport_game_art.h) like the HP bars; nothing of the game's is shipped. Each piece falls back to
// the drawn look when the files can't be read. Kaniel 2026-10-16: the settings menu too.
// Kaniel 2026-10-17: the slots, runes, dividers and title scrollwork are the co-op art's
// (CoopArt) when it loaded.
namespace MenuArt {

enum Piece {
    Wall,          // the in-game menus' backdrop
    Title,         // the title screen's picture
    Logo,          // the loading screen's Bloodborne
    Row,           // a menu row (a button)
    RowSelected,   // the gold-framed selected row
    EntryRow,      // a list row with its square slot
    Slot,          // a square slot
    SlotSelected,  // the gold-framed square slot
    Card,          // a tall card
    CardSelected,  // the gold-framed tall card
    HeaderFrame,   // a framed header with corner ornaments
    Divider,       // the silver divider with its scroll
    HeaderOrnament, // the scrollwork under a title
    Flourish,      // a row's scroll
    Runes,         // eight 32 px runes in a row
    ButtonCross,   // the pad's Cross
    ButtonCircle,  // the pad's Circle
    ButtonMove,    // the d-pad
    // Kaniel 2026-10-19: every other button as the game's key guide shows it (BbGameArt's
    // KeyGuideTexture); the launcher's icons put the player's own Xbox or keyboard ones there.
    ButtonSquare,
    ButtonTriangle,
    ButtonL1,
    ButtonR1,
    ButtonL2,
    ButtonR2,
    ButtonL3,
    ButtonR3,
    ButtonOptions,
    ButtonTouchLeft,
    ButtonTouchRight,
    ButtonDpadUp,
    ButtonDpadDown,
    ButtonDpadLeftRight,
    ButtonDpadUpDown,
    ButtonLeftStick,
    ButtonRightStick,
    PieceCount
};

struct Source {
    const char* file;
    const char* texture;
    BbGameArt::PixelRect rect; // empty: all of it
    bool light_is_alpha;       // white art on black: its brightness is its coverage
};

// Measured on MENU_Common_00001-3 (common.tpf.dcx), MENU_Title_00001 and MENU_NowLoading_00001.
constexpr Source Sources[PieceCount] = {
    {"menu/common.tpf.dcx", "MENU_Common_00001", {0, 0, 1920, 1080}, false},
    {"menu/title.tpf.dcx", "MENU_Title_00001", {0, 0, 1780, 1024}, false},
    {"menu/nowloading.tpf.dcx", "MENU_NowLoading_00001", {440, 420, 1520, 660}, true},
    {"menu/common.tpf.dcx", "MENU_Common_00001", {988, 1532, 1562, 1580}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00001", {1156, 1228, 1958, 1326}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00001", {1144, 1438, 1716, 1530}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00001", {482, 1230, 610, 1358}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00001", {626, 1230, 760, 1364}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00002", {2, 708, 134, 966}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00002", {138, 708, 278, 974}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00002", {1156, 412, 1582, 502}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00003", {12, 14, 368, 46}, true},
    {"menu/common.tpf.dcx", "MENU_Common_00003", {42, 58, 438, 106}, true},
    {"menu/common.tpf.dcx", "MENU_Common_00001", {482, 1174, 754, 1208}, false},
    {"menu/common.tpf.dcx", "MENU_Common_00002", {1570, 4, 1826, 36}, false},
    {"menu/common.tpf.dcx", "KG_Cancel", {}, false},
    {"menu/common.tpf.dcx", "KG_OK", {}, false},
    {"menu/common.tpf.dcx", "KG_L_UDLR", {}, false},
    {"menu/common.tpf.dcx", "KG_R_L", {}, false},
    {"menu/common.tpf.dcx", "KG_R_U", {}, false},
    {"menu/common.tpf.dcx", "KG_L1", {}, false},
    {"menu/common.tpf.dcx", "KG_R1", {}, false},
    {"menu/common.tpf.dcx", "KG_L2", {}, false},
    {"menu/common.tpf.dcx", "KG_R2", {}, false},
    {"menu/common.tpf.dcx", "KG_L3", {}, false},
    {"menu/common.tpf.dcx", "KG_R3", {}, false},
    {"menu/common.tpf.dcx", "KG_Start", {}, false},
    {"menu/common.tpf.dcx", "KG_TP_L", {}, false},
    {"menu/common.tpf.dcx", "KG_TP_R", {}, false},
    {"menu/common.tpf.dcx", "KG_L_U", {}, false},
    {"menu/common.tpf.dcx", "KG_L_D", {}, false},
    {"menu/common.tpf.dcx", "KG_L_LR", {}, false},
    {"menu/common.tpf.dcx", "KG_L_UD", {}, false},
    {"menu/common.tpf.dcx", "KG_LS", {}, false},
    {"menu/common.tpf.dcx", "KG_RS", {}, false},
};

/// The key guide piece that shows `texture` (a KG_* name).
std::optional<Piece> KeyIcon(std::string_view texture) {
    for (int i = ButtonCross; i < PieceCount && !texture.empty(); ++i) {
        if (texture == Sources[i].texture) {
            return Piece(i);
        }
    }
    return std::nullopt;
}

ImTextureData* textures[PieceCount] = {};
/// button_icons=auto: the key guide pieces in the other sets (bbport_prompts.h), shown while the
/// game's own icons show that set.
ImTextureData* set_textures[BbPromptSets::SetCount][PieceCount] = {};
ImFont* font = nullptr; // the game's menu font
bool tried = false;

std::vector<std::uint8_t> ReadGameFile(const char* relative) {
    std::vector<std::uint8_t> bytes;
    char path[1024];
    if (!bbcoop_game_file((std::string("dvdroot_ps4/") + relative).c_str(), path, sizeof(path))) {
        return bytes;
    }
    if (FILE* f = std::fopen(path, "rb")) {
        std::uint8_t chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) {
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
        std::fclose(f);
    }
    return bytes;
}

/// Reads the pieces and the font once, before a frame (new textures and fonts go to the backend
/// with the next NewFrame).
void Load() {
    if (tried) {
        return;
    }
    tried = true;
    std::map<std::string, std::optional<BbGameArt::TpfFile>> files;
    int loaded = 0;
    for (int i = 0; i < PieceCount; ++i) {
        const Source& source = Sources[i];
        auto& file = files[source.file];
        if (!file) {
            file = BbGameArt::TpfFile::Read(ReadGameFile(source.file));
        }
        auto image = file ? file->Piece(source.texture, source.rect) : std::nullopt;
        if (!image) {
            continue;
        }
        if (source.light_is_alpha) {
            for (std::size_t at = 0; at < image->rgba.size(); at += 4) {
                std::uint8_t* px = image->rgba.data() + at;
                px[3] = std::max({px[0], px[1], px[2]});
            }
        }
        textures[i] = IM_NEW(ImTextureData)();
        textures[i]->Create(ImTextureFormat_RGBA32, image->width, image->height);
        std::memcpy(textures[i]->GetPixels(), image->rgba.data(), image->rgba.size());
        ImGui::RegisterUserTexture(textures[i]);
        ++loaded;
        for (int set = BbPromptSets::Xbox; set < BbPromptSets::SetCount; ++set) {
            const auto glyph = BbPrompts::Glyph(source.texture, BbPromptSets::Set(set));
            if (glyph.size() != 1024 || image->width != 32 || image->height != 32) {
                continue;
            }
            const auto blocks = BbGameArt::DeswizzleBlocks(glyph, 8, 8, 16);
            const auto rgba = BbGameArt::DecodeBc7Rect(blocks, 32, {0, 0, 32, 32});
            if (rgba.size() != image->rgba.size()) {
                continue;
            }
            auto*& texture = set_textures[set][i];
            texture = IM_NEW(ImTextureData)();
            texture->Create(ImTextureFormat_RGBA32, 32, 32);
            std::memcpy(texture->GetPixels(), rgba.data(), rgba.size());
            ImGui::RegisterUserTexture(texture);
        }
    }
    if (const auto ttf = BbGameArt::TtfFromGfxFont(ReadGameFile("menu/engus/font.gfx"))) {
        void* data = IM_ALLOC(ttf->size());
        std::memcpy(data, ttf->data(), ttf->size());
        ImFontConfig config;
        config.FontDataOwnedByAtlas = true;
        font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(ttf->size()), 21.0f, &config);
        // What the game's font lacks (the Russian menu's letters, ×) comes from the serif.
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontDataOwnedByAtlas = false;
        ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_serif_ttf),
                                                   int(bb_serif_ttf_end - bb_serif_ttf), 21.0f, &merge);
    }
    std::printf("Overlay: the menus use %d of %d pieces of the game's menu art%s\n", loaded,
                int(PieceCount), font ? " and its menu font" : " (the game's font did not load)");
    CoopArt::Load();
}

/// The co-op art's piece in place of the game's `piece`, if it has one.
std::optional<CoopArt::Piece> Own(Piece piece) {
    if (!CoopArt::Has()) {
        return std::nullopt;
    }
    switch (piece) {
    case Divider:
        return CoopArt::Piece::Divider;
    case HeaderOrnament:
        return CoopArt::Piece::TitleOrnament;
    case Slot:
        return CoopArt::Piece::Medallion;
    case SlotSelected:
        return CoopArt::Piece::MedallionReady;
    default:
        return std::nullopt;
    }
}

bool Has(Piece piece) {
    return Own(piece) || textures[piece] != nullptr;
}

ImVec2 SizeOf(Piece piece) {
    if (const auto own = Own(piece)) {
        // About the game's size, so layouts stay: its divider 356 px wide; the scrollwork 34 px tall
        // (the game's is 48 with clear space round its drawing).
        const ImVec2 size = CoopArt::SizeOf(*own);
        if (piece == Divider) {
            return ImVec2(356.0f, 356.0f * size.y / size.x);
        }
        if (piece == HeaderOrnament) {
            return ImVec2(34.0f * size.x / size.y, 34.0f);
        }
        return size;
    }
    return textures[piece] ? ImVec2(float(textures[piece]->Width), float(textures[piece]->Height))
                           : ImVec2(0, 0);
}

ImU32 Tint(float alpha, ImU32 tint = IM_COL32_WHITE) {
    const ImVec4 c = ImGui::ColorConvertU32ToFloat4(tint);
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

/// `piece` (its source pixels [s0, s1]) stretched into [p0, p1].
void Part(ImDrawList* draw, Piece piece, ImVec2 p0, ImVec2 p1, ImVec2 s0, ImVec2 s1, ImU32 color) {
    const ImVec2 size = SizeOf(piece);
    ImTextureData* texture = set_textures[BbPrompts::Shown()][piece];
    draw->AddImage((texture ? texture : textures[piece])->GetTexRef(), p0, p1,
                   ImVec2(s0.x / size.x, s0.y / size.y),
                   ImVec2(s1.x / size.x, s1.y / size.y), color);
}

/// All of `piece` stretched into [p0, p1]; false when it is not loaded.
bool Draw(ImDrawList* draw, Piece piece, ImVec2 p0, ImVec2 p1, float alpha = 1.0f,
          ImU32 tint = IM_COL32_WHITE) {
    if (const auto own = Own(piece)) {
        // The medallions are round where the game's slots were square: a little larger, and the
        // ready one's outer ring round the same medallion.
        float grow = 1.0f;
        if (piece == Slot || piece == SlotSelected) {
            grow = 1.2f * CoopArt::SizeOf(*own).x / CoopArt::SizeOf(CoopArt::Piece::Medallion).x;
        }
        const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        const ImVec2 half((p1.x - p0.x) * 0.5f * grow, (p1.y - p0.y) * 0.5f * grow);
        CoopArt::Draw(draw, *own, ImVec2(c.x - half.x, c.y - half.y), ImVec2(c.x + half.x, c.y + half.y), alpha);
        return true;
    }
    if (!Has(piece)) {
        return false;
    }
    Part(draw, piece, p0, p1, ImVec2(0, 0), SizeOf(piece), Tint(alpha, tint));
    return true;
}

/// `piece` as a nine-slice frame into [p0, p1]: its corners (`edge` source pixels, drawn `drawn`
/// pixels) kept, its edges and middle stretched.
bool Nine(ImDrawList* draw, Piece piece, ImVec2 p0, ImVec2 p1, float edge, float drawn,
          float alpha = 1.0f) {
    if (!Has(piece)) {
        return false;
    }
    const ImVec2 size = SizeOf(piece);
    drawn = std::min({drawn, (p1.x - p0.x) * 0.5f, (p1.y - p0.y) * 0.5f});
    const float sx[4] = {0, edge, size.x - edge, size.x}, sy[4] = {0, edge, size.y - edge, size.y};
    const float dx[4] = {p0.x, p0.x + drawn, p1.x - drawn, p1.x};
    const float dy[4] = {p0.y, p0.y + drawn, p1.y - drawn, p1.y};
    const ImU32 color = Tint(alpha);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            Part(draw, piece, ImVec2(dx[x], dy[y]), ImVec2(dx[x + 1], dy[y + 1]), ImVec2(sx[x], sy[y]),
                 ImVec2(sx[x + 1], sy[y + 1]), color);
        }
    }
    return true;
}

/// `piece` filling [p0, p1] without stretching it out of shape (cut to fit), as a backdrop.
bool Cover(ImDrawList* draw, Piece piece, ImVec2 p0, ImVec2 p1, float alpha = 1.0f) {
    if (!Has(piece)) {
        return false;
    }
    const ImVec2 size = SizeOf(piece);
    const float want = (p1.x - p0.x) / (p1.y - p0.y), have = size.x / size.y;
    ImVec2 s0(0, 0), s1 = size;
    if (have > want) {
        const float cut = (size.x - size.y * want) * 0.5f;
        s0.x = cut;
        s1.x = size.x - cut;
    } else {
        const float cut = (size.y - size.x / want) * 0.5f;
        s0.y = cut;
        s1.y = size.y - cut;
    }
    Part(draw, piece, p0, p1, s0, s1, Tint(alpha));
    return true;
}

/// Rune `index` (0..7) of the runes row, into [p0, p1].
bool Rune(ImDrawList* draw, int index, ImVec2 p0, ImVec2 p1, float alpha = 1.0f) {
    if (CoopArt::Has()) {
        // The place's hunter sigil (four), a little larger than the rune was in its medallion.
        const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        const float half = (p1.x - p0.x) * 0.6f;
        CoopArt::Draw(draw, CoopArt::Piece(int(CoopArt::Piece::Sigil0) + index % 4), ImVec2(c.x - half, c.y - half),
                      ImVec2(c.x + half, c.y + half), alpha);
        return true;
    }
    if (!Has(Runes)) {
        return false;
    }
    const float side = SizeOf(Runes).y;
    Part(draw, Runes, p0, p1, ImVec2(side * float(index % 8), 0), ImVec2(side * float(index % 8 + 1), side),
         Tint(alpha));
    return true;
}

// Japanese, Korean and Chinese (Kaniel 2026-10-17: every language the game has): the game's own
// font of that language first, as the game draws it (menu/<folder>/font.gfx; the English one
// holds the Japanese kana and kanji), then the system's for what the game's set lacks, made the
// first time the menus speak the language. The plain overlay fonts get the same glyphs merged in.
ImFont* language_fonts[BbSettings::MenuLanguageCount] = {};
bool language_tried[BbSettings::MenuLanguageCount] = {};

const char* GameFontFolder(BbSettings::MenuLanguage language) {
    switch (language) {
    case BbSettings::MenuLanguage::Ja:
        return "engus";
    case BbSettings::MenuLanguage::Ko:
        return "korkr";
    case BbSettings::MenuLanguage::ZhCn:
        return "zhocn";
    case BbSettings::MenuLanguage::ZhTw:
        return "zhotw";
    default:
        return nullptr;
    }
}

/// The system fonts with the language's glyphs (path, face in a collection), the first found.
std::optional<std::pair<std::string, int>> SystemFont(BbSettings::MenuLanguage language) {
    using L = BbSettings::MenuLanguage;
    std::vector<std::pair<std::string, int>> candidates;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    const std::string fonts = std::string(windir && *windir ? windir : "C:\\Windows") + "\\Fonts\\";
    const auto add = [&](std::initializer_list<const char*> names) {
        for (const char* name : names) {
            candidates.emplace_back(fonts + name, 0);
        }
    };
    switch (language) {
    case L::Ja: add({"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"}); break;
    case L::Ko: add({"malgun.ttf", "gulim.ttc"}); break;
    case L::ZhCn: add({"msyh.ttc", "simsun.ttc"}); break;
    case L::ZhTw: add({"msjh.ttc", "mingliu.ttc"}); break;
    default: break;
    }
#else
    // Noto Sans CJK's collection: Japanese, Korean, Simplified and Traditional Chinese faces.
    const int face = language == L::Ja ? 0 : language == L::Ko ? 1 : language == L::ZhCn ? 2 : 3;
    for (const char* path : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"}) {
        candidates.emplace_back(path, face);
    }
#endif
    for (const auto& candidate : candidates) {
        if (FILE* f = std::fopen(candidate.first.c_str(), "rb")) {
            std::fclose(f);
            return candidate;
        }
    }
    return std::nullopt;
}

/// Before NewFrame: the fonts of the language the menus speak now.
void LoadLanguage() {
    const auto language = BbSettings::Get().menu_language.load();
    const int l = int(language);
    const char* folder = GameFontFolder(language);
    if (!folder || language_tried[l]) {
        return;
    }
    language_tried[l] = true;
    std::vector<std::vector<std::uint8_t>> sources;
    if (auto ttf = BbGameArt::TtfFromGfxFont(ReadGameFile((std::string("menu/") + folder + "/font.gfx").c_str()))) {
        sources.push_back(std::move(*ttf));
    }
    if (std::string_view(folder) != "engus") {
        // Latin and Cyrillic as in the other languages.
        if (auto ttf = BbGameArt::TtfFromGfxFont(ReadGameFile("menu/engus/font.gfx"))) {
            sources.push_back(std::move(*ttf));
        }
    }
    sources.emplace_back(bb_serif_ttf, bb_serif_ttf_end);
    const auto system = SystemFont(language);
    if (system) {
        if (FILE* f = std::fopen(system->first.c_str(), "rb")) {
            std::vector<std::uint8_t> bytes;
            std::uint8_t chunk[65536];
            for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) {
                bytes.insert(bytes.end(), chunk, chunk + n);
            }
            std::fclose(f);
            sources.push_back(std::move(bytes));
        }
    }
    ImFont* made = nullptr;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const bool system_face = system && i + 1 == sources.size();
        void* data = IM_ALLOC(sources[i].size());
        std::memcpy(data, sources[i].data(), sources[i].size());
        ImFontConfig config;
        config.FontDataOwnedByAtlas = true;
        config.MergeMode = made != nullptr;
        config.DstFont = made;
        config.FontNo = system_face ? ImU32(system->second) : 0;
        ImFont* added = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(sources[i].size()), 21.0f, &config);
        made = made ? made : added;
    }
    language_fonts[l] = made;
    // The overlay's plain fonts: the language's glyphs after their own (the first source above
    // and the system's; the serif is already theirs).
    for (ImFont* plain : {sans_font, serif_font}) {
        for (std::size_t i = 0; plain && i < sources.size(); ++i) {
            const bool system_face = system && i + 1 == sources.size();
            if (i != 0 && !system_face) {
                continue;
            }
            void* data = IM_ALLOC(sources[i].size());
            std::memcpy(data, sources[i].data(), sources[i].size());
            ImFontConfig config;
            config.FontDataOwnedByAtlas = true;
            config.MergeMode = true;
            config.DstFont = plain;
            config.FontNo = system_face ? ImU32(system->second) : 0;
            ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(sources[i].size()), 18.0f, &config);
        }
    }
    std::printf("Overlay: %s text from the game's %s font%s%s\n", BbSettings::LanguageCode(language), folder,
                system ? " and " : "", system ? system->first.c_str() : "");
}

/// The menus' text font: the game's own when it loaded, in the menus' language.
ImFont* Font() {
    if (ImFont* own = language_fonts[int(BbSettings::Get().menu_language.load())]) {
        return own;
    }
    return font != nullptr ? font : serif_font;
}

/// A soft dark vignette over [p0, p1] (the game's menus darken toward their edges).
void Vignette(ImDrawList* draw, ImVec2 p0, ImVec2 p1, float strength) {
    const ImU32 dark = IM_COL32(0, 0, 0, int(255 * strength)), clear = IM_COL32(0, 0, 0, 0);
    const float band = (p1.y - p0.y) * 0.22f;
    draw->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + band), dark, dark, clear, clear);
    draw->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - band), p1, clear, clear, dark, dark);
    const float side = (p1.x - p0.x) * 0.16f;
    draw->AddRectFilledMultiColor(p0, ImVec2(p0.x + side, p1.y), dark, clear, clear, dark);
    draw->AddRectFilledMultiColor(ImVec2(p1.x - side, p0.y), p1, clear, dark, dark, clear);
}

} // namespace MenuArt

// The game's HP bar (the party list's art): the settings menu's sliders, the team loading
// screen's progress.
namespace Party {
bool GameHpBar(ImDrawList* draw, ImVec2 b0, float width, float height, float fill, float alpha);
void LoadGauge();
} // namespace Party

// The settings menu, laid out like a PC game's options screen in the game's own style: the game
// dimmed behind, a centred panel on the menus' wall between the game's dividers, tabs (L1/R1,
// Q/E, mouse), one setting per row on the game's menu rows ("name  < value >": left/right change
// it, Cross/Enter steps it, the mouse clicks either side), the focused row's description at the
// bottom and the game's key guide in the footer. Kaniel 2026-10-16: the same art, font and
// colours as the team screens and the party list (MenuArt), the drawn look without the art.
namespace Ui {

float S(float v) {
    return v * base_scale;
}
const char* T(const char* english, const char* russian) {
    return BbSettings::MenuText(english, russian);
}
// The accent (gold) and the bright text over it; the team screens draw in the souls counter's
// grey instead (Kaniel 2026-10-18, GreyAccent).
ImVec4 accent{0.80f, 0.68f, 0.46f, 1.0f};
ImVec4 accent_bright{0.96f, 0.88f, 0.70f, 1.0f};
ImU32 Gold(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, alpha));
}
ImU32 Bright(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(accent_bright.x, accent_bright.y, accent_bright.z, alpha));
}
/// While it lives, Gold() is the souls counter's grey and Bright() a near white.
struct GreyAccent {
    ImVec4 was = accent, was_bright = accent_bright;
    GreyAccent() {
        accent = ImVec4(BbCoopArt::SoulsGrey[0] / 255.0f, BbCoopArt::SoulsGrey[1] / 255.0f,
                        BbCoopArt::SoulsGrey[2] / 255.0f, 1.0f);
        accent_bright = ImVec4(0.93f, 0.94f, 0.94f, 1.0f);
    }
    ~GreyAccent() {
        accent = was;
        accent_bright = was_bright;
    }
};
ImU32 Plain(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.80f, 0.77f, 0.70f, alpha));
}
ImU32 Dim(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.48f, 0.46f, 0.42f, alpha));
}

/// Kaniel 2026-10-19: the buttons our screens name are the game's own key guide icons (the
/// launcher's Xbox or keyboard ones in their place for those players), never written out; the
/// keys only the overlay reads (its menus' Q / E, Escape) are key caps for a keyboard player.
/// Texts name a button as {L1} ("Hold {Circle} to give up"); translations keep the names.
namespace Keys {

/// The icon set the game's own icons show (run.py's BB_ICONS_SET): 'p'layStation, 'x'box,
/// 'k'eyboard or 's'witch; with "auto" the device used last (bbport_prompts.h).
char IconSet() {
    static const char set = [] {
        const char* value = std::getenv("BB_ICONS_SET");
        const std::string_view name = value ? value : "playstation";
        return name == "xbox"       ? 'x'
               : name == "keyboard" ? 'k'
               : name == "switch"   ? 's'
               : name == "auto"     ? 'a'
                                    : 'p';
    }();
    return set == 'a' ? BbPromptSets::Letter(BbPrompts::Shown()) : set;
}

/// A button as texts name it (BbGameArt::KeyGuideTextureNamed): the game's icon of it, and what
/// a key cap says when the game's art could not be read.
struct Name {
    MenuArt::Piece piece;
    std::string_view fallback;
};

std::optional<Name> Find(std::string_view name) {
    const auto piece = MenuArt::KeyIcon(BbGameArt::KeyGuideTextureNamed(name));
    if (!piece) {
        return std::nullopt;
    }
    return Name{*piece, name.size() <= 3 ? name : name.substr(0, 2)};
}

/// A key cap with `label`, its left edge and middle at `at`, `height` tall (only measured without
/// `draw`); returns its width.
float Cap(ImDrawList* draw, std::string_view label, ImVec2 at, float height, float alpha) {
    ImFont* font = sans_font;
    const float size = height * 0.55f, r = height * 0.5f;
    const float text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.data(), label.data() + label.size()).x;
    const float w = std::max(text + height * 0.5f, height);
    if (draw) {
        const ImVec2 a(at.x, at.y - r * 0.85f), b(at.x + w, at.y + r * 0.85f);
        draw->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.06f, 0.05f, 0.05f, 0.85f * alpha)), height * 0.16f);
        draw->AddRect(a, b, Plain(0.75f * alpha), height * 0.16f, 0, std::max(1.0f, height * 0.06f));
        draw->AddText(font, size, ImVec2(at.x + (w - text) * 0.5f, at.y - size * 0.55f), Plain(alpha),
                      label.data(), label.data() + label.size());
    }
    return w;
}

/// The game's icon of `name`, its left edge and middle at `at`, `height` tall (only measured
/// without `draw`); a key cap when the art could not be read. Returns its width.
float Icon(ImDrawList* draw, const Name& name, ImVec2 at, float height, float alpha) {
    if (!MenuArt::Has(name.piece)) {
        return Cap(draw, name.fallback, at, height, alpha);
    }
    if (draw) {
        MenuArt::Draw(draw, name.piece, ImVec2(at.x, at.y - height * 0.5f), ImVec2(at.x + height, at.y + height * 0.5f),
                      alpha);
    }
    return height;
}

/// A list of buttons as `"L1 R1"` (side by side) or `"L3+R3"` (pressed together: a '+' between),
/// as icons (`caps`: key caps with those words) from `at`; returns the width.
float Glyphs(ImDrawList* draw, std::string_view list, bool caps, ImVec2 at, float height, float alpha) {
    const float gap = height * 0.12f;
    float x = at.x;
    std::size_t start = 0;
    while (start < list.size()) {
        std::size_t end = list.find_first_of(" +", start);
        if (end == std::string_view::npos) {
            end = list.size();
        }
        const std::string_view word = list.substr(start, end - start);
        if (!word.empty()) {
            if (x > at.x) {
                x += gap;
            }
            const auto name = caps ? std::nullopt : Find(word);
            x += name ? Icon(draw, *name, ImVec2(x, at.y), height, alpha) : Cap(draw, word, ImVec2(x, at.y), height, alpha);
        }
        if (end < list.size() && list[end] == '+') {
            const float size = height * 0.6f, w = sans_font->CalcTextSizeA(size, FLT_MAX, 0.0f, "+").x;
            if (draw) {
                draw->AddText(sans_font, size, ImVec2(x + gap, at.y - size * 0.55f), Plain(0.8f * alpha), "+");
            }
            x += gap + w;
        }
        start = end + 1;
    }
    return x - at.x;
}

/// One entry of a key guide: the pad's buttons (Glyphs' list), a keyboard player's keys (key caps;
/// null: the pad's buttons, whose icons the launcher draws as that player's keys; empty: not for a
/// keyboard), and what they do.
struct Hint {
    const char* pad;
    const char* keys;
    const char* text;
};

/// A key guide as the game's (icons, then what they do) in the current font, centred on [x0, x1]
/// with its last line's bottom at `bottom`; a little smaller, else on two lines, when it does not
/// fit (BbKeyGuide). Returns its top.
float Guide(ImDrawList* draw, std::span<const Hint> hints, float x0, float x1, float bottom, float alpha = 1.0f) {
    const bool keyboard = IconSet() == 'k';
    const float icon = ImGui::GetFontSize() * 1.45f, icon_gap = S(6.0f), entry_gap = S(28.0f);
    std::vector<const Hint*> shown;
    std::vector<BbKeyGuide::Entry> entries;
    for (const Hint& hint : hints) {
        const char* list = keyboard && hint.keys ? hint.keys : hint.pad;
        if (!list || !*list) {
            continue;
        }
        shown.push_back(&hint);
        entries.push_back({Glyphs(nullptr, list, keyboard && hint.keys, ImVec2(0, 0), icon, 1.0f),
                           ImGui::CalcTextSize(hint.text).x});
    }
    const BbKeyGuide::Layout layout = BbKeyGuide::Arrange(entries, icon_gap, entry_gap, x1 - x0);
    const float scale = layout.scale, line_h = icon * scale + S(8.0f);
    const float top = bottom - float(layout.lines) * line_h + S(8.0f);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * scale;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const Hint& hint = *shown[i];
        const BbKeyGuide::Place& place = layout.places[i];
        const float mid = top + float(place.line) * line_h + icon * scale * 0.5f;
        const char* list = keyboard && hint.keys ? hint.keys : hint.pad;
        float x = x0 + place.x;
        x += Glyphs(draw, list, keyboard && hint.keys, ImVec2(x, mid), icon * scale, alpha) + icon_gap * scale;
        draw->AddText(font, size, ImVec2(x, mid - size * 0.5f), Plain(0.85f * alpha), hint.text);
    }
    return top;
}

} // namespace Keys

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

/// The game's silver divider centred on [x0, x1] at `y` (the drawn ornament without the art).
void Divider(ImDrawList* draw, float x0, float x1, float y, float alpha = 0.9f) {
    const ImVec2 size = MenuArt::SizeOf(MenuArt::Divider);
    if (size.x <= 0.0f) {
        Ornament(draw, x0, x1, y);
        return;
    }
    const float w = std::min(x1 - x0, S(size.x * 1.1f)), h = w * size.y / size.x;
    const float mid = (x0 + x1) * 0.5f;
    MenuArt::Draw(draw, MenuArt::Divider, ImVec2(mid - w * 0.5f, y - h * 0.5f), ImVec2(mid + w * 0.5f, y + h * 0.5f),
                  alpha);
}

/// The game's scrollwork under a title, centred on `mid` from `y`; returns its height (0 without
/// the art).
float HeaderOrnament(ImDrawList* draw, float mid, float y, float scale, float alpha = 0.8f) {
    if (!MenuArt::Has(MenuArt::HeaderOrnament)) {
        return 0.0f;
    }
    const ImVec2 o = MenuArt::SizeOf(MenuArt::HeaderOrnament);
    const float ow = S(o.x * scale), oh = ow * o.y / o.x;
    MenuArt::Draw(draw, MenuArt::HeaderOrnament, ImVec2(mid - ow * 0.5f, y), ImVec2(mid + ow * 0.5f, y + oh), alpha);
    return oh;
}

/// `text` cut to `width` in the current font, "..." at its end when it was longer (a long name never
/// runs over the button or the card edge next to it).
std::string Fit(const char* text, float width) {
    if (ImGui::CalcTextSize(text).x <= width) {
        return text;
    }
    const float dots = ImGui::CalcTextSize("...").x;
    std::string cut(text);
    while (!cut.empty() && ImGui::CalcTextSize(cut.c_str()).x + dots > width) {
        // Drop one UTF-8 character (and any continuation bytes before it).
        do {
            cut.pop_back();
        } while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
    }
    return cut + "...";
}

/// A hunter's card: the game's stone card; the player's own framed in gold over it (the game's
/// selected card is tinted blue, out of place among the others).
void HunterCard(ImDrawList* draw, ImVec2 at, ImVec2 end, bool mine, float alpha = 1.0f) {
    if (CoopArt::Has()) {
        CoopArt::Card(draw, at, end, alpha);
        if (mine) {
            const float in = S(9.0f);
            draw->AddRect(ImVec2(at.x + in, at.y + in), ImVec2(end.x - in, end.y - in), Gold(0.55f), 0.0f, 0, S(1.2f));
        }
        return;
    }
    MenuArt::Nine(draw, MenuArt::Card, at, end, 14.0f, S(12.0f), alpha);
    if (mine) {
        const float in = S(4.0f);
        draw->AddRectFilled(ImVec2(at.x + in, at.y + in), ImVec2(end.x - in, end.y - in), Gold(0.06f));
        draw->AddRect(ImVec2(at.x + in, at.y + in), ImVec2(end.x - in, end.y - in), Gold(0.85f), 0.0f, 0, S(1.5f));
    }
}

/// A list's rule under its heading: a gold line fading out to the right (drawn: the game's scroll
/// art comes without a see-through background).
void ListRule(ImDrawList* draw, float x0, float x1, float y) {
    const float t = S(1.2f), d = S(4.0f);
    draw->AddRectFilledMultiColor(ImVec2(x0 + d * 2.5f, y - t * 0.5f), ImVec2(x1, y + t * 0.5f), Gold(0.7f), Gold(0.0f),
                                  Gold(0.0f), Gold(0.7f));
    draw->AddQuadFilled(ImVec2(x0 + d, y - d), ImVec2(x0 + d * 2.0f, y), ImVec2(x0 + d, y + d), ImVec2(x0, y),
                        Gold(0.9f));
}

/// `text` in the current font wrapped to `width` from `at`, its lines spaced apart (the game's
/// font draws taller than ImGui's line height: wrapped lines touched); returns its height.
float WrappedText(ImDrawList* draw, ImVec2 at, float width, ImU32 color, const char* text) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize(), step = std::round(size * 1.3f);
    const char* end = text + std::strlen(text);
    float y = at.y;
    if (std::strchr(text, '{')) {
        // Buttons named as {L3}: the game's icons in the line, the words wrapped round them.
        const float icon = size * 1.3f, right = at.x + width;
        float x = at.x;
        for (const char* run = text; run < end;) {
            const char* open = std::strchr(run, '{');
            const char* close = open ? std::strchr(open, '}') : nullptr;
            const auto name = close ? Keys::Find(std::string_view(open + 1, close - open - 1)) : std::nullopt;
            const char* stop = name ? open : end;
            if (!name && open && close) { // not a button: the braces stay words
                stop = close + 1;
            }
            while (run < stop) {
                const char* cut = font->CalcWordWrapPosition(size, run, stop, right - x);
                if (cut == run && *cut != '\n') {
                    if (x > at.x) { // nothing more fits on this line
                        x = at.x;
                        y += step;
                        continue;
                    }
                    cut = run + ImTextCountUtf8BytesFromChar(run, stop);
                }
                draw->AddText(font, size, ImVec2(x, y), color, run, cut);
                x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, run, cut).x;
                if (cut < stop) {
                    x = at.x;
                    y += step;
                    run = ImTextCalcWordWrapNextLineStart(cut, stop);
                } else {
                    run = cut;
                }
            }
            if (name) {
                const float w = Keys::Icon(nullptr, *name, ImVec2(0, 0), icon, 1.0f);
                if (x + w > right && x > at.x) {
                    x = at.x;
                    y += step;
                }
                x += Keys::Icon(draw, *name, ImVec2(x, y + size * 0.55f), icon,
                                float((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
                run = close + 1;
            }
        }
        return y + step - at.y;
    }
    for (const char* line = text; line < end;) {
        const char* cut = font->CalcWordWrapPosition(size, line, end, width);
        if (cut == line && *cut != '\n') {
            cut = line + ImTextCountUtf8BytesFromChar(line, end); // a word wider than the line
        }
        draw->AddText(font, size, ImVec2(at.x, y), color, line, cut);
        y += step;
        line = ImTextCalcWordWrapNextLineStart(cut, end);
    }
    return y - at.y;
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
    // The game's menu rows (as the team screen's buttons): the gold-framed one has the focus.
    const ImVec2 end(at.x + width, at.y + height);
    const bool row_art = MenuArt::Nine(draw, focused ? MenuArt::RowSelected : MenuArt::Row, at, end, 12.0f,
                                       S(9.0f), focused ? 1.0f : !enabled ? 0.25f : hovered ? 0.85f : 0.5f);
    if (!row_art && (focused || hovered)) {
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
    if (std::strchr(label, '{')) { // a button named in it: the game's icon in the line
        WrappedText(draw, ImVec2(at.x + S(28.0f), mid_y - label_size.y * 0.5f), width * 0.5f, label_color, label);
    } else {
        draw->AddText(ImVec2(at.x + S(28.0f), mid_y - label_size.y * 0.5f), label_color, label);
    }

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
        const float fill = std::clamp(slider, 0.0f, 1.0f), knob = b0 + (b1 - b0) * fill;
        // The game's own HP bar as the slider (as the team's loading screen shows progress),
        // a bone-white mark where the value is; drawn bars without the game's art.
        const float gh = S(15.0f);
        if (Party::GameHpBar(draw, ImVec2(b0, mid_y - gh * 0.5f), b1 - b0, gh, fill, enabled ? 1.0f : 0.4f)) {
            draw->AddRectFilled(ImVec2(knob - S(1.5f), mid_y - gh * 0.75f), ImVec2(knob + S(1.5f), mid_y + gh * 0.75f),
                                enabled ? Bright() : Dim());
        } else {
            draw->AddRectFilled(ImVec2(b0, mid_y - bh), ImVec2(b1, mid_y + bh), Dim(0.6f), bh);
            draw->AddRectFilled(ImVec2(b0, mid_y - bh), ImVec2(knob, mid_y + bh), enabled ? Gold(0.9f) : Dim(), bh);
            draw->AddCircleFilled(ImVec2(knob, mid_y), S(7.0f), enabled ? Bright() : Dim());
        }
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
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x - S(56.0f);
    ImGui::Dummy(ImVec2(width, WrappedText(ImGui::GetWindowDrawList(), ImVec2(at.x + S(28.0f), at.y), width,
                                           color, text)));
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

/// Frame generation's provider, multiplier and fitting to the display, and what runs now
/// (Supermedo's 026f8ff, 389124d). `on`: the switch is on and an upscaler selected.
void FrameGenRows(bool on) {
    auto& s = BbSettings::Get();
    const int dlss_max = std::clamp(s.dlssg_max_multiplier.load(), 1, 4);
    const bool dlss = s.frame_gen_provider == BbSettings::FrameGenDlss && dlss_max > 1;
    static const char* providers[] = {"FSR 3.1", "DLSS"};
    Choice("frame_gen_provider", T("Frame generation: provider", "Генерация кадров: технология"),
           dlss ? 1 : 0, 2, [](int i) { return providers[i]; },
           [&](int i) { return i == 0 || dlss_max > 1; },
           dlss_max > 1
               ? T("FSR 3.1: any GPU, 2x. DLSS: NVIDIA's frame generation, smoother in motion; up "
                   "to 4x on a GeForce RTX 50.",
                   "FSR 3.1: любая видеокарта, 2x. DLSS: генерация кадров NVIDIA, плавнее в "
                   "движении; до 4x на GeForce RTX 50.")
               : T("FSR 3.1: any GPU, 2x. DLSS Frame Generation needs a GeForce RTX 40 or newer "
                   "with bbport_dlss.dll and nvngx_dlssg.dll.",
                   "FSR 3.1: любая видеокарта, 2x. Для DLSS Frame Generation нужна GeForce RTX "
                   "40 или новее, bbport_dlss.dll и nvngx_dlssg.dll."),
           on, [&](int i) {
               Store(s.frame_gen_provider, i ? int(BbSettings::FrameGenDlss) : int(BbSettings::FrameGenFsr3),
                     true);
           });
    static const char* multipliers[] = {"2x", "3x", "4x"};
    const int multiplier = dlss ? std::clamp(s.frame_gen_multiplier.load(), 2, dlss_max) : 2;
    Choice("frame_gen_multiplier", T("Frame generation: multiplier", "Генерация кадров: множитель"),
           multiplier - 2, 3, [](int i) { return multipliers[i]; },
           [&](int i) { return i + 2 <= (dlss ? dlss_max : 2); },
           T("Frames shown per game frame. FSR 3.1 and DLSS on a GeForce RTX 40: 2x; DLSS on a "
             "GeForce RTX 50: up to 4x. Each one more uses some video memory.",
             "Кадров на экране на каждый кадр игры. FSR 3.1 и DLSS на GeForce RTX 40: 2x; DLSS на "
             "GeForce RTX 50: до 4x. Каждый следующий занимает видеопамять."),
           on && dlss && dlss_max > 2, [&](int i) { Store(s.frame_gen_multiplier, i + 2, true); });
    Toggle("frame_gen_fit", T("Fit to display refresh", "Под частоту экрана"), s.frame_gen_fit_display,
           T("On: no more frames than the display shows (an uncapped game gets fewer generated "
             "frames, and the GPU does less). Off: the multiplier as chosen; past the refresh rate "
             "the display shows the newest frames. With V-Sync always fitted.",
             "Вкл.: не больше кадров, чем показывает экран (при снятом ограничении FPS "
             "генерируется меньше, видеокарта меньше нагружена). Выкл.: множитель как выбран. С "
             "V-Sync всегда под экран."),
           on);
    if (!on) {
        return;
    }
    static char line[256];
    const int hz = s.frame_gen_display_hz.load();
    if (const char* problem = s.frame_gen_problem.load()) {
        Note(problem, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
    // What reaches the display (the presenter's count of shown generated frames), not what the
    // settings ask for: "on" with nothing generated says why (Kaniel 2026-10-25).
    if (s.fg_active) {
        const float game = s.fg_base_fps.load(), shown = s.fg_presented_fps.load();
        std::snprintf(line, sizeof(line),
                      T("Generating: game %d FPS, shown %d FPS (%dx)",
                        "Генерация идёт: игра %d FPS, на экране %d FPS (%dx)"),
                      int(game + 0.5f), int(shown + 0.5f),
                      game > 0.0f ? int(shown / game + 0.5f) : 1);
        Note(line, Dim());
        return;
    }
    const int idle = s.frame_gen_idle.load();
    if (idle == BbFrameGenPace::IdleNoRoom && hz > 0) {
        std::snprintf(line, sizeof(line),
                      T("Paused: no room on the %d Hz display at this frame rate",
                        "Приостановлено: нет места на экране %d Гц при этой частоте кадров"),
                      hz);
        Note(line, Dim());
        return;
    }
    const char* reason = T("starting", "запускается");
    switch (idle) {
    case BbFrameGenPace::IdleFailed:
        reason = T("the provider refused; switch it off and on to retry",
                   "технология отказала; выключите и включите, чтобы попробовать снова");
        break;
    case BbFrameGenPace::IdleNoMemory:
        reason = T("not enough video memory", "не хватает видеопамяти");
        break;
    case BbFrameGenPace::IdleNoScene:
        reason = T("no game scene (menu or loading screen)", "нет игровой сцены (меню или загрузка)");
        break;
    case BbFrameGenPace::IdleSceneLarger:
        reason = T("the scene is rendered larger than the window", "сцена отрисовывается больше окна");
        break;
    case BbFrameGenPace::IdlePresentBehind:
        reason = T("paused, presenting does not keep up", "пауза, вывод кадров не успевает");
        break;
    case BbFrameGenPace::IdleLate:
        reason = T("generated frames are replaced before the display shows them",
                   "сгенерированные кадры заменяются раньше, чем экран их покажет");
        break;
    default:
        break;
    }
    std::snprintf(line, sizeof(line), T("On, but not generating: %s", "Включено, но не генерирует: %s"),
                  reason);
    Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
}

/// A whole-number setting from lo to hi, one step a press; `game` is shown as the game's own.
void IntSliderRow(const char* id, const char* label, std::atomic<int>& value, int lo, int hi, int game,
                  const char* hint, bool enabled = true) {
    char text[48];
    if (value == game) {
        std::snprintf(text, sizeof(text), T("%d (game)", "%d (игра)"), value.load());
    } else {
        std::snprintf(text, sizeof(text), "%d", value.load());
    }
    float drag = -1.0f;
    const int step = Row(id, label, text, hint, enabled, float(value - lo) / float(hi - lo), &drag);
    if (step != 0) {
        Store(value, std::clamp(value + step, lo, hi), true);
    } else if (drag >= 0.0f) {
        const int v = std::clamp(lo + int(std::lround(drag * float(hi - lo))), lo, hi);
        Store(value, v, v != value);
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
                   s.fast_shader_math != s.startup_fast_shader_math ||
                   s.fov != s.startup_fov ||
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
    // Auto (the first choice) shows the display's value it takes, when known.
    const auto name = [](char* text, size_t size, int nits, float display) {
        if (nits != BbSettings::HdrAuto) {
            std::snprintf(text, size, "%d %s", nits, T("nits", "нит"));
        } else if (display > 0.0f) {
            std::snprintf(text, size, "%s (%.0f)", T("Auto", "Авто"), display);
        } else {
            std::snprintf(text, size, "%s", T("Auto", "Авто"));
        }
    };
    for (int i = 0; i < BbSettings::HdrPeakCount; ++i) {
        name(peak_names[i], sizeof(peak_names[i]), BbSettings::HdrPeakNits[i], s.hdr_display_peak);
    }
    for (int i = 0; i < BbSettings::HdrPaperWhiteCount; ++i) {
        name(white_names[i], sizeof(white_names[i]), BbSettings::HdrPaperWhiteNits[i], s.hdr_display_white);
    }
    Choice("hdr_peak", T("HDR peak brightness", "Пиковая яркость HDR"),
           BbSettings::HdrPeakIndex(s.hdr_peak_nits), BbSettings::HdrPeakCount,
           [](int i) { return peak_names[i]; }, [](int) { return true; },
           T("Your display's peak brightness. Auto: the peak the display reports (Windows HDR "
             "Calibration changes it).",
             "Пиковая яркость вашего монитора. Авто: пик, который сообщает монитор (его меняет "
             "калибровка HDR в Windows)."),
           on, [&](int i) { Store(s.hdr_peak_nits, BbSettings::HdrPeakNits[i], true); });
    Choice("hdr_paper_white", T("HDR UI brightness", "Яркость интерфейса HDR"),
           BbSettings::HdrPaperWhiteIndex(s.hdr_paper_white), BbSettings::HdrPaperWhiteCount,
           [](int i) { return white_names[i]; }, [](int) { return true; },
           T("Brightness of white in the picture and the interface (paper white). Auto: as bright "
             "as the desktop (Windows' SDR content brightness); lower in a dark room.",
             "Яркость белого в картинке и интерфейсе (paper white). Авто: как рабочий стол (яркость "
             "SDR-содержимого в Windows); в тёмной комнате ниже."),
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
    // Kaniel 2026-10-24: the display's own resolutions, any shape (21:9, 32:9, 16:10, 4:3), and
    // the one chosen even when the display has no such mode (another monitor, bbport.ini).
    static std::vector<BbSettings::OutputSize> outputs;
    static std::vector<std::string> output_names;
    outputs = BbSettings::OutputChoices();
    const BbSettings::OutputSize chosen = BbSettings::ChosenOutput();
    if (std::find(outputs.begin(), outputs.end(), chosen) == outputs.end()) {
        outputs.push_back(chosen);
    }
    output_names.clear();
    for (const auto& size : outputs) {
        const char* shape = BbOutputSizes::AspectName(size);
        output_names.push_back(std::to_string(size.width) + " x " + std::to_string(size.height) +
                               (shape[0] ? std::string("  (") + shape + ")" : std::string()));
    }
    Choice("output", T("Output resolution", "Разрешение вывода"), BbOutputSizes::Nearest(outputs, chosen),
           int(outputs.size()), [](int i) { return output_names[i].c_str(); }, [](int) { return true; },
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
           true, [&](int i) {
               BbSettings::ChooseOutput(outputs[i]);
               dirty = true;
           });
    const char* aspects[] = {T("Auto (the output's)", "Авто (как у вывода)"), T("16:9 (bars)", "16:9 (с полосами)")};
    Choice("aspect", T("Aspect ratio", "Соотношение сторон"),
           std::clamp(s.aspect.load(), 0, BbSettings::AspectModeCount - 1), BbSettings::AspectModeCount,
           [&](int i) { return aspects[i]; }, [](int) { return true; },
           BbSettings::FixedRenderSession()
               ? T("Auto: a wider screen (21:9, 32:9) shows more to the sides, a taller one (16:10, 4:3) "
                   "more above and below; the HUD goes to the screen's edges. 16:9: the game's picture "
                   "with black bars. Applies after restarting the game.",
                   "Авто: на широком экране (21:9, 32:9) видно больше по бокам, на высоком (16:10, 4:3) "
                   "— сверху и снизу; интерфейс у краёв экрана. 16:9: картинка игры с чёрными "
                   "полосами. Применяется после перезапуска игры.")
               : T("Auto: a wider screen (21:9, 32:9) shows more to the sides, a taller one (16:10, 4:3) "
                   "more above and below; the HUD goes to the screen's edges. 16:9: the game's picture "
                   "with black bars. Changes at the next frame.",
                   "Авто: на широком экране (21:9, 32:9) видно больше по бокам, на высоком (16:10, 4:3) "
                   "— сверху и снизу; интерфейс у краёв экрана. 16:9: картинка игры с чёрными "
                   "полосами. Меняется со следующего кадра."),
           true, [&](int i) { Store(s.aspect, i, true); });
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
    IntSliderRow("fov", T("Field of view", "Поле зрения"), s.fov, BbSettings::FovMin, BbSettings::FovMax,
                 BbSettings::FovDefault,
                 T("Vertical field of view in degrees; the game's camera has 43. Every camera is "
                   "widened by the same amount. Applies after restarting the game.",
                   "Вертикальное поле зрения в градусах; у камеры игры 43. Каждая камера "
                   "расширяется на ту же величину. Применяется после перезапуска игры."));
    Toggle("fast_shader_math", T("Fast shader math", "Быстрая математика шейдеров"), s.fast_shader_math,
           T("Pixel and compute shaders use faster, less strict float math: a little more FPS, the "
             "picture can differ slightly. Applies after restarting the game; shaders are compiled "
             "again once.",
             "Пиксельные и вычислительные шейдеры считают быстрее и менее строго: чуть больше FPS, "
             "картинка может немного отличаться. Применяется после перезапуска игры; шейдеры один раз "
             "компилируются заново."));
    Toggle("mute_unfocused", T("Mute in the background", "Без звука в фоне"), s.mute_unfocused,
           T("The game is silent while its window is not the active one.",
             "Игра молчит, пока её окно не активно."));
    // Kaniel 2026-10-17: the teammate compass.
    Toggle("compass", T("Teammate compass", "Компас союзников"), s.compass,
           T("A strip turned with the camera shows where each hunter of your party on this map is "
             "and how far, with their initial. One behind you sits at its end with an arrow.",
             "Полоса, повёрнутая вместе с камерой, показывает, где каждый охотник вашей группы на "
             "этой карте и как далеко, с его инициалом. Тот, кто позади, стоит на её краю со стрелкой."));
    const char* compass_places[] = {T("Top", "Сверху"), T("Bottom", "Снизу"),
                                    T("Around the middle", "Вокруг центра")};
    Choice("compass_position", T("Compass position", "Положение компаса"),
           std::clamp(s.compass_position.load(), 0, BbSettings::CompassPositionCount - 1),
           BbSettings::CompassPositionCount, [&](int i) { return compass_places[i]; }, [](int) { return true; },
           T("Top or bottom of the screen: a strip. Around the middle: the markers on a ring round "
             "the centre of the screen, ahead at the top.",
             "Сверху или снизу экрана: полоса. Вокруг центра: метки на кольце вокруг середины "
             "экрана, впереди сверху."),
           s.compass, [&](int i) { Store(s.compass_position, i, true); });
    // Kaniel 2026-10-20 ("bloodstains of ours will be displayed on compass and map").
    Toggle("bloodstains", T("Bloodstains on compass and map", "Пятна крови на компасе и карте"), s.bloodstains,
           T("Where your echoes lie after a death: on the compass on this map, on the map on "
             "every map. Only you see your bloodstain.",
             "Где лежат ваши отголоски после смерти: на компасе на этой карте, на карте на всех "
             "картах. Ваше пятно крови видите только вы."));
    // Kaniel 2026-10-24: team pings.
    Toggle("team_pings", T("Team pings", "Метки команды"), s.team_pings,
           T("Mark a spot for your team for 20 seconds: double tap the touchpad or press the ping key "
             "where you aim (on the enemy you are locked on to), {Square} on the map. Your team sees it "
             "in the world, on the compass and on the map.",
             "Отметьте место для команды на 20 секунд: двойное касание сенсорной панели или клавиша "
             "метки туда, куда вы смотрите (на врага, на котором фиксирован взгляд), {Square} на "
             "карте. Команда видит метку в мире, на компасе и на карте."));
    Choice("ping_key", T("Ping key", "Клавиша метки"),
           std::clamp(s.ping_key.load(), 0, BbSettings::PingKeyCount - 1), BbSettings::PingKeyCount,
           [&](int i) { return BbSettings::PingKeyNames[i]; }, [](int) { return true; },
           T("The keyboard key that pings where you aim.", "Клавиша, ставящая метку туда, куда вы смотрите."),
           s.team_pings, [&](int i) { Store(s.ping_key, i, true); });
    // Kaniel 2026-10-19 ("make it opt in", "an opt in option to view rendered in real time"):
    // the map's 3D view.
    const char* map_3d_names[] = {T("Off", "Выкл."), T("Prerendered", "Заранее нарисованная"),
                                  T("Live", "В реальном времени")};
    Choice("map_3d", T("3D map", "3D-карта"), std::clamp(s.map_3d.load(), 0, BbSettings::Map3dCount - 1),
           BbSettings::Map3dCount, [&](int i) { return map_3d_names[i]; }, [](int) { return true; },
           T("{L3} on the map shows the place from the game's own models, from an angle. Prerendered: "
             "each area is drawn once, the first time, in the background. Live: the models are "
             "drawn on the graphics card as you turn and tilt the view (right stick), using its "
             "memory. Leave it off on a low-end PC.",
             "{L3} на карте показывает местность из моделей самой игры под углом. Заранее "
             "нарисованная: каждая область рисуется один раз, при первом показе, в фоне. В "
             "реальном времени: модели рисуются видеокартой, пока вы поворачиваете и наклоняете "
             "вид (правый стик), используя её память. На слабом ПК оставьте выключенным."),
           true, [&](int i) { Store(s.map_3d, i, true); });
    Toggle("map_fog", T("Map fog of war", "Туман на карте"), s.map_fog,
           T("The map hides what your hunter has not walked near yet. Off shows the whole map.",
             "Карта скрывает то, рядом с чем ваш охотник ещё не проходил. Выкл. показывает всю карту."));
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
           BbSettings::FrameRateLocked()
               ? T("Co-op plays at the game's fixed 60 FPS: every player sends and takes the "
                   "same number of updates, or the others freeze and fly.",
                   "Кооператив идёт на собственных 60 FPS игры: все игроки шлют и принимают "
                   "одинаково обновлений, иначе остальные замирают и летают.")
               : T("Highest frame rate. A limit unlocks the game's frame rate from the next start "
                   "(the 60 FPS mode is the game's own fixed 60); then it changes at once. Off "
                   "keeps the launcher's mode.",
                   "Предел частоты кадров. С лимитом частота кадров игры разблокируется со "
                   "следующего запуска (режим 60 FPS — собственные 60 игры); дальше меняется "
                   "сразу. Выкл оставляет режим лаунчера."),
           !BbSettings::FrameRateLocked(), [&](int i) { Store(s.fps_limit, BbSettings::FpsLimits[i], true); });
    // Auto (the game's language), then every language the game has, in their own names.
    static std::string auto_name;
    auto_name = std::string("Auto (") + BbSettings::LanguageName(BbSettings::GameLanguage()) + ")";
    Choice("language", "Language", s.menu_language_auto ? 0 : int(s.menu_language.load()) + 1,
           BbSettings::MenuLanguageCount + 1,
           [](int i) {
               return i == 0 ? auto_name.c_str() : BbSettings::LanguageName(BbSettings::MenuLanguage(i - 1));
           },
           [](int) { return true; }, T("Language of this menu.", "Язык этого меню."), true, [&](int i) {
               BbSettings::SetMenuLanguage(i == 0 ? std::nullopt
                                                  : std::optional(BbSettings::MenuLanguage(i - 1)));
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
        const BbSettings::OutputSize output = BbSettings::CurrentOutput();
        std::snprintf(preset_names[i], sizeof(preset_names[i]), "%s  %dx%d", BbSettings::PresetName(i),
                      int(std::lround(output.width / scale / 2) * 2),
                      int(std::lround(output.height / scale / 2) * 2));
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
    Toggle("frame_generation", T("Frame generation", "Генерация кадров"), s.frame_generation,
           T("Frames made between the game's own: FSR 3.1 doubles the frame rate on any GPU, DLSS "
             "makes up to three on a GeForce RTX 50 (4x). Adds a little input lag and uses some "
             "video memory; best from 60 FPS. Off by default.",
             "Кадры между кадрами игры: FSR 3.1 удваивает частоту кадров на любой видеокарте, DLSS "
             "делает до трёх на GeForce RTX 50 (4x). Немного добавляет задержки ввода и занимает "
             "видеопамять; лучше от 60 FPS. По умолчанию выключено."),
           upscaler_on);
    FrameGenRows(upscaler_on && s.frame_generation);
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
    // Kaniel 2026-10-19 ("maybe there is a chance to add ray tracing"): screen-space lighting.
    const char* lighting_names[] = {T("Off", "Выкл"), T("Low", "Низкое"), T("High", "Высокое")};
    Choice("screen_lighting", T("Screen lighting", "Экранное освещение"),
           std::clamp(s.screen_lighting.load(), 0, BbSettings::ScreenLightingCount - 1),
           BbSettings::ScreenLightingCount, [&](int i) { return lighting_names[i]; },
           [](int) { return true; },
           T("Soft shadows in corners and light bounced off nearby surfaces, added to the game's "
             "own lighting (like ReShade's RTGI). Low works at half size; High costs more GPU time.",
             "Мягкие тени в углах и свет, отражённый от ближних поверхностей, поверх освещения "
             "игры (как RTGI в ReShade). Низкое считается в половинном размере; высокое "
             "нагружает видеочип сильнее."),
           true, [&](int i) { Store(s.screen_lighting, i, true); });
    const bool lighting_on = s.screen_lighting != BbSettings::ScreenLightingOff;
    SliderRow("screen_lighting_ao", T("Corner shadows", "Тени в углах"), s.screen_lighting_ao, 0.0f,
              1.0f, 0.1f,
              T("How much corners and contacts darken with screen lighting.",
                "Насколько темнеют углы и места касания с экранным освещением."),
              lighting_on);
    SliderRow("screen_lighting_gi", T("Bounced light", "Отражённый свет"), s.screen_lighting_gi,
              0.0f, 2.0f, 0.1f,
              T("How much colour nearby surfaces cast onto each other with screen lighting.",
                "Сколько цвета ближние поверхности отбрасывают друг на друга с экранным "
                "освещением."),
              lighting_on);
    // Kaniel 2026-10-21 ("continue with ray tracing"): the shadows also ray traced.
    Toggle("ray_traced_ao", T("Ray-traced shadows", "Трассировка теней"), s.ray_traced_ao,
           T("Screen lighting's shadows also traced against the area's own models: under "
             "ceilings, in alcoves and behind the camera. Needs a graphics card with ray tracing; "
             "turning it on applies after restarting the game.",
             "Тени экранного освещения также трассируются по моделям самой местности: под "
             "потолками, в нишах и за камерой. Нужна видеокарта с трассировкой лучей; "
             "включение применяется после перезапуска игры."),
           lighting_on);
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        const char* hint =
            std::string_view(effect.key) == "debug_camera"
                ? T("Hold {Cross} and press {L3}. Applies after restarting.",
                    "Удерживайте {Cross} и нажмите {L3}. После перезапуска.")
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
    static const char* const icon_values[] = {"auto", "playstation", "xbox", "keyboard", "switch"};
    constexpr int icon_count = int(std::size(icon_values));
    const std::string icons = BbSettings::Line("button_icons").value_or("auto");
    int icon = 0;
    for (int i = 0; i < icon_count; ++i) {
        if (icons == icon_values[i]) {
            icon = i;
        }
    }
    Choice("icons", T("Button icons", "Значки кнопок"), icon, icon_count,
           [](int i) {
               return i == 0   ? T("Automatic", "Автоматически")
                      : i == 1 ? "PlayStation"
                      : i == 2 ? "Xbox"
                      : i == 3 ? T("Keyboard and mouse", "Клавиатура и мышь")
                               : "Nintendo Switch";
           },
           [](int) { return true; },
           T("The buttons the game shows on screen. Automatic follows what you play with: the keys bound "
             "below while you use the keyboard and mouse, the controller's own buttons (PlayStation, Xbox or "
             "Switch) while you use one, switching as you change. A change here applies after a restart.",
             "Кнопки, которые игра показывает на экране. «Автоматически» — по тому, чем вы играете: "
             "назначенные ниже клавиши, пока вы за клавиатурой и мышью, кнопки контроллера (PlayStation, "
             "Xbox или Switch), пока в руках контроллер, и меняется на ходу. Изменение здесь применяется "
             "после перезапуска."),
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
    // Kaniel 2026-10-25: the free camera, opt-in.
    Toggle("free_camera", T("Free camera hotkey", "Клавиша свободной камеры"), BbSettings::Get().free_camera,
           T("F7, or {Square} while holding {L3}, takes the camera away from your hunter: the left stick or "
             "WASD flies it, the right stick or the mouse turns it, {R1} and {L1} go up and down, {R2} "
             "held flies fast and {L2} slow. Your hunter stands still meanwhile; the same key brings the "
             "camera back.",
             "F7 или {Square} с зажатым {L3} отделяет камеру от охотника: левый стик или WASD двигают её, "
             "правый стик или мышь поворачивают, {R1} и {L1} — вверх и вниз, с зажатым {R2} быстрее, с "
             "{L2} медленнее. Охотник тем временем стоит; та же клавиша возвращает камеру."));

    // A press that came for the input waiting for one, or the wait running out.
    if (capture.input >= 0 && capture.done) {
        const char* name = runtime_pad_input_name(capture.input);
        if (name && capture.kind == "pad") {
            // The input that had the button takes this input's old ones (bbport_pad_rebind.h).
            std::vector<BbPadRebind::Input> inputs;
            std::size_t target = 0;
            for (int i = 0; i < runtime_pad_input_count(); ++i) {
                if (!runtime_pad_input_takes_pad(i)) {
                    continue;
                }
                char pads[160];
                runtime_pad_binding_text(i, 1, pads, sizeof(pads));
                target = i == capture.input ? inputs.size() : target;
                inputs.push_back({runtime_pad_input_name(i), pads});
            }
            for (const auto& [input, buttons] : BbPadRebind::Assign(inputs, target, capture.name)) {
                BbSettings::SetLine("pad." + input, buttons);
            }
            SaveControls();
        } else if (name) {
            BbSettings::SetLine(capture.kind + "." + name, capture.name);
            SaveControls();
        }
        capture = {};
    } else if (capture.input >= 0 && std::chrono::steady_clock::now() - capture.since > std::chrono::seconds(10)) {
        capture = {};
    }

    ImGui::Spacing();
    Note(T("Keyboard and mouse  |  gamepad", "Клавиатура и мышь  |  геймпад"), Dim());
    const char* hint = T("{Cross} or Enter: assign. The next key, mouse button or wheel step replaces this input's "
                         "keyboard binding, a gamepad button its gamepad one; hold Shift, Ctrl or Alt for a "
                         "combination. Delete: no key. Esc or 10 s: cancel.",
                         "{Cross} или Enter: назначить. Следующая клавиша, кнопка мыши или шаг колеса заменяет "
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
        Note(T("Items: no item list found. It comes from the game's item names "
               "(dvdroot_ps4/msg/engus/item.msgbnd.dcx); check the game files.",
               "Предметы: список не найден. Он берётся из названий предметов игры "
               "(dvdroot_ps4/msg/engus/item.msgbnd.dcx); проверьте файлы игры."),
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
                             T("Left/right picks; {Cross} on the Give row hands it over.",
                               "Влево/вправо — выбор; {Cross} на строке «Выдать» — выдать."),
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
    MenuArt::Vignette(ImGui::GetBackgroundDrawList(), ImVec2(0, 0), display, 0.6f);
    static bool told = false;
    if (!told) {
        told = true;
        int pieces = 0;
        for (int i = 0; i < MenuArt::PieceCount; ++i) {
            pieces += MenuArt::Has(MenuArt::Piece(i)) ? 1 : 0;
        }
        std::printf("Overlay: the settings menu uses %d of %d pieces of the game's menu art%s\n", pieces,
                    int(MenuArt::PieceCount), MenuArt::font ? " and its menu font" : "");
    }

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
    const bool wall = MenuArt::Has(MenuArt::Wall);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, wall ? ImVec4(0, 0, 0, 0) : ImVec4(0.035f, 0.032f, 0.028f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(36.0f), S(24.0f)));
    ImGui::PushFont(MenuArt::Font(), 21.0f);
    ImGui::Begin("##bbport_settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    const float x0 = win.x + S(24.0f), x1 = win.x + size.x - S(24.0f);
    if (wall) { // the panel as the team screen's: the menus' wall, darker to its edges
        const ImVec2 w1(win.x + size.x, win.y + size.y);
        MenuArt::Cover(draw, MenuArt::Wall, win, w1, 0.97f);
        MenuArt::Vignette(draw, win, w1, 0.55f);
        draw->AddRect(win, w1, IM_COL32(150, 140, 120, 90), 0.0f, 0, S(1.0f));
    }
    Divider(draw, x0, x1, win.y + S(12.0f));
    Divider(draw, x0, x1, win.y + size.y - S(12.0f));

    // Title, the game's scrollwork under it.
    ImGui::PushFont(MenuArt::Font(), 38.0f);
    const char* title = T("Settings", "Настройки");
    const ImVec2 title_size = ImGui::CalcTextSize(title);
    draw->AddText(ImVec2(win.x + (size.x - title_size.x) * 0.5f, win.y + S(26.0f)), Bright(), title);
    ImGui::PopFont();
    const float ornament = HeaderOrnament(draw, win.x + size.x * 0.5f, win.y + S(26.0f) + title_size.y, 0.7f);
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

    // Tabs: L1 ... R1, the current one on the game's gold-framed row (underlined without the art).
    ImGui::SetCursorScreenPos(
        ImVec2(win.x + S(36.0f), win.y + S(26.0f) + title_size.y + std::max(ornament, S(12.0f)) + S(8.0f)));
    {
        const float row_y = ImGui::GetCursorScreenPos().y;
        float widths[tabs], names = 0.0f;
        const float pad_x = S(16.0f), tab_h = S(36.0f);
        // The buttons either side: the game's L1 / R1 icons, Q / E key caps for a keyboard player.
        const bool keyboard = Keys::IconSet() == 'k';
        const float tab_icon = S(28.0f);
        const ImVec2 l1(Keys::Glyphs(nullptr, keyboard ? "Q" : "L1", keyboard, ImVec2(0, 0), tab_icon, 1.0f),
                        tab_icon);
        ImGui::PushFont(MenuArt::Font(), 22.0f);
        for (int i = 0; i < tabs; ++i) {
            widths[i] = ImGui::CalcTextSize(T(tab_en[i], tab_ru[i])).x;
            names += widths[i];
        }
        // The gaps shrink to keep the row and its L1 / R1 inside the panel (the Russian names are
        // longer).
        const float room = size.x - S(72.0f) - 2.0f * l1.x;
        const float gap = std::clamp((room - names) / float(tabs + 1), S(20.0f), S(46.0f));
        const float total = names + gap * float(tabs - 1);
        float x = win.x + (size.x - total) * 0.5f;
        Keys::Glyphs(draw, keyboard ? "Q" : "L1", keyboard, ImVec2(x - gap - l1.x, row_y + tab_h * 0.5f), tab_icon, 0.9f);
        Keys::Glyphs(draw, keyboard ? "E" : "R1", keyboard, ImVec2(x + total + gap, row_y + tab_h * 0.5f), tab_icon, 0.9f);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int i = 0; i < tabs; ++i) {
            const char* name = T(tab_en[i], tab_ru[i]);
            const ImVec2 t0(x - pad_x, row_y), t1(x + widths[i] + pad_x, row_y + tab_h);
            ImGui::SetCursorScreenPos(t0);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(t1.x - t0.x, tab_h))) {
                state.tab = i;
                state.focus_first = true;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool current = i == state.tab;
            const bool art = (current || hovered) &&
                             MenuArt::Nine(draw, current ? MenuArt::RowSelected : MenuArt::Row, t0, t1, 12.0f,
                                           S(8.0f), current ? 1.0f : 0.7f);
            const float text_h = ImGui::CalcTextSize(name).y;
            draw->AddText(ImVec2(x, row_y + (tab_h - text_h) * 0.5f), current ? Bright() : hovered ? Plain() : Dim(),
                          name);
            if (current && !art) {
                draw->AddLine(ImVec2(x - S(6.0f), t1.y - S(1.0f)), ImVec2(x + widths[i] + S(6.0f), t1.y - S(1.0f)),
                              Gold(0.9f), S(1.5f));
            }
            x += widths[i] + gap;
        }
        ImGui::PopItemFlag();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(win.x + S(36.0f), row_y + tab_h + S(26.0f)));
    }
    Divider(draw, x0 + S(20.0f), x1 - S(20.0f), ImGui::GetCursorScreenPos().y - S(8.0f), 0.6f);
    ImGui::Dummy(ImVec2(0, S(8.0f)));

    // Rows (scrolling when they do not fit), then the description and the controls.
    const float footer = S(170.0f);
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
    // Kaniel 2026-10-16: the team menu from here too (as from the game's System menu): the team
    // screen over the game, with invite, remove, leave and join the hunt.
    if (bbcoop_lobby probe; bbcoop_lobby_view(&probe) &&
                            Row("team", T("Co-op Team", "Кооп-команда"), nullptr,
                                T("Your team: invite hunters, remove a member, leave, or join the hunt.",
                                  "Ваша команда: пригласить охотников, исключить, покинуть или вступить в охоту."),
                                true) != 0) {
        SetOpen(false);
        bbcoop_team_menu(2); // no page of the game under it
    }
    // Kaniel 2026-10-17: the map screen (also M, and the game's System menu).
    if (Row("map", T("Map", "Карта"), nullptr,
            T("Where you and your team are; travel to a lit lamp. M opens it in the game too.",
              "Где вы и ваша команда; путь к зажжённой лампе. В игре её открывает и клавиша M."),
            true) != 0) {
        SetOpen(false);
        bbcoop_map_menu(2);
    }
    if (Row("quit", T("Quit to desktop", "Выйти на рабочий стол"), nullptr,
            T("Asks first: progress since the last autosave is lost. Holding {L3} + {R3} asks too.",
              "Сначала спросит: прогресс после последнего автосохранения теряется. Удержание {L3} + {R3} "
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
    Divider(draw, x0 + S(20.0f), x1 - S(20.0f), desc_y + S(6.0f), 0.6f);
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    {
        ImVec2 at(win.x + S(64.0f), desc_y + S(20.0f));
        const float width = size.x - S(128.0f);
        if (RestartNeeded()) {
            at.y += WrappedText(draw, at, width, ImGui::GetColorU32(ImVec4(0.95f, 0.72f, 0.35f, 1.0f)),
                                T("Some changes apply after restarting the game.",
                                  "Часть изменений применится после перезапуска игры."));
        }
        if (state.hint) {
            WrappedText(draw, at, width, Plain(0.85f), state.hint);
        }
    }
    ImGui::PopFont();

    // Controls: as the game's own key guide (the team screen's), the buttons' icons, then what
    // they do; a keyboard player's keys as key caps.
    {
        const Keys::Hint keys[] = {{"Dpad", "Arrows", T("Choose, value", "Выбор, значение")},
                                   {"Cross", "Enter", T("Change", "Изменить")},
                                   {"Circle", "Esc", T("Close", "Закрыть")},
                                   {"L1 R1", "Q E", T("Tabs", "Вкладки")},
                                   {"L3+R3", "Insert", T("Menu", "Меню")},
                                   {"L3+R3", "", T("Hold: quit", "Удерживать: выход")}};
        ImGui::PushFont(MenuArt::Font(), 18.0f);
        Keys::Guide(draw, keys, win.x + S(36.0f), win.x + size.x - S(36.0f),
                    win.y + size.y - S(BbKeyGuide::BottomClearance));
        ImGui::PopFont();
    }

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
        // The frames shown, and the game's own rate (what resolution and upscaler change) with
        // the multiplier (Supermedo's aabd7f7): "120 FPS (game 60, frame gen 2x)".
        char text[96];
        BbFrameGenPace::FpsText(text, sizeof(text), s.fg_presented_fps.load(), s.fg_base_fps.load(),
                                true, frame_ms_avg);
        ImGui::Text("%s  %s", text, upscaler_name);
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
    // Opened this frame: the press that opened it (Cross on the menu's Quit row) is not an answer
    // (the rig: the box closed on Cancel the frame it opened).
    const bool opened = ImGui::GetFrameCount() != last_frame + 1;
    if (opened) {
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
    if (opened) {
        activate = false;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 165));
    const ImVec2 size(std::min(S(620.0f), display.x - S(40.0f)), S(230.0f));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    const bool wall = MenuArt::Has(MenuArt::Wall);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, wall ? ImVec4(0, 0, 0, 0) : ImVec4(0.035f, 0.032f, 0.028f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##quit", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    const float x0 = win.x + S(24.0f), x1 = win.x + size.x - S(24.0f);
    if (wall) { // as the settings menu's panel
        const ImVec2 w1(win.x + size.x, win.y + size.y);
        MenuArt::Cover(draw, MenuArt::Wall, win, w1, 0.97f);
        MenuArt::Vignette(draw, win, w1, 0.55f);
        draw->AddRect(win, w1, IM_COL32(150, 140, 120, 90), 0.0f, 0, S(1.0f));
    }
    Divider(draw, x0, x1, win.y + S(12.0f));
    Divider(draw, x0, x1, win.y + size.y - S(12.0f));

    ImGui::PushFont(MenuArt::Font(), 32.0f);
    const char* title = T("Quit to desktop?", "Выйти на рабочий стол?");
    const ImVec2 ts = ImGui::CalcTextSize(title);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, win.y + S(34.0f)), Bright(), title);
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 19.0f);
    const char* note = T("Progress since the last autosave is lost.",
                         "Прогресс после последнего автосохранения будет потерян.");
    const ImVec2 ns = ImGui::CalcTextSize(note);
    draw->AddText(ImVec2(win.x + (size.x - ns.x) * 0.5f, win.y + S(86.0f)), Plain(0.85f), note);
    ImGui::PopFont();

    // The choices on the game's menu rows, the chosen one gold-framed (underlined without the art).
    ImGui::PushFont(MenuArt::Font(), 24.0f);
    const char* labels[2] = {T("Cancel", "Отмена"), T("Quit", "Выйти")};
    const float gap = S(40.0f), bh = S(44.0f);
    const float bw = std::max(ImGui::CalcTextSize(labels[0]).x, ImGui::CalcTextSize(labels[1]).x) + S(70.0f);
    float x = win.x + (size.x - (2 * bw + gap)) * 0.5f;
    const float y = win.y + S(132.0f);
    for (int i = 0; i < 2; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##quit_choice", ImVec2(bw, bh))) {
            choice = i;
            activate = true;
        }
        if (ImGui::IsItemHovered()) {
            choice = i;
        }
        ImGui::PopID();
        const bool art = MenuArt::Nine(draw, i == choice ? MenuArt::RowSelected : MenuArt::Row, ImVec2(x, y),
                                       ImVec2(x + bw, y + bh), 12.0f, S(9.0f), i == choice ? 1.0f : 0.6f);
        const ImVec2 ls = ImGui::CalcTextSize(labels[i]);
        const float lx = x + (bw - ls.x) * 0.5f;
        draw->AddText(ImVec2(lx, y + (bh - ls.y) * 0.5f), i == choice ? Bright() : Dim(), labels[i]);
        if (i == choice && !art) {
            draw->AddLine(ImVec2(lx - S(6.0f), y + bh - S(4.0f)), ImVec2(lx + ls.x + S(6.0f), y + bh - S(4.0f)),
                          Gold(0.9f), S(1.5f));
        }
        x += bw + gap;
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
    // Over the team screen too (its join by code, invite by name), whatever was clicked last.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    // Kaniel 2026-10-19: the keys as the game's icons, a keyboard player's as key caps.
    const Ui::Keys::Hint keys[] = {{"Cross", "Enter", Ui::T("OK", "ОК")},
                                   {"Circle", "Esc", Ui::T("Cancel", "Отмена")},
                                   {"", "Backspace", Ui::T("Delete", "Удалить")}};
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, Ui::S(380.0f)), height = Ui::S(36.0f);
    ImGui::Dummy(ImVec2(width, height));
    Ui::Keys::Guide(ImGui::GetWindowDrawList(), keys, at.x, at.x + width, at.y + height);
    ImGui::End();
}

// The team screen (phase 5, docs/SERVER_COOP_PLAN.md "The team lobby"): up while the title holds a
// save's load for the team (bbcoop_team_screen). Four hunters' places, the players online to
// invite, the invitations, and the buttons: create or leave a team, begin the hunt, its world.
// Drawn like the menu (its colours and ornaments, the game's serif); focus moves with the
// d-pad / arrows over the buttons in the order they are drawn, Cross / Enter presses, the mouse
// works too.
namespace Team {
using namespace Ui;

bbcoop_lobby lobby{};
bool open = false; // present thread: as Visible() read it last

struct Focus {
    int focus = 0;
    int items = 0;
    int move = 0;
    bool activate = false;
    bool fresh = true; // the screen just came up: the focus goes to the Begin button
    // The focused button by its id, so the focus stays on it when the rows before it change
    // (Kaniel 2026-10-14: once an invitation was accepted its row went, and Cross landed on the
    // next button); this frame's button ids in order.
    std::string id;
    std::vector<std::string> ids;
    bool was_in_team = false;
};
Focus focus;

// A friend the lobby does not list (a hosted server lists 32 players): the hunter's name to invite,
// or the team's code to join, typed in the window's text entry (the game's name dialog's, drawn by
// TextPrompt; Enter or Cross sends it, Escape or Circle not).
enum class Ask { None, Name, Code };
Ask asking = Ask::None;

void BeginAsk(Ask what) {
    const char* prompt = what == Ask::Name ? T("Name of the hunter to invite", "Имя охотника для приглашения")
                                           : T("Code of the team to join", "Код команды для вступления");
    if (bbgpu_text_input_begin("", prompt)) {
        asking = what;
    }
}

void PollAsk() {
    if (asking == Ask::None) {
        return;
    }
    char text[128];
    const int state = bbgpu_text_input_poll(text, sizeof(text));
    if (state == 0) {
        return; // still typing
    }
    if (state == 1 && text[0] != 0) {
        bbcoop_lobby_request(asking == Ask::Name ? BBCOOP_TEAM_INVITE_NAME : BBCOOP_TEAM_JOIN_CODE, text);
    }
    asking = Ask::None;
}

/// One button: its label (drawn centred in `size` at `at`), true when pressed.
bool Button(ImDrawList* draw, const char* id, const char* label, ImVec2 at, ImVec2 size, bool enabled = true,
            bool primary = false) {
    const int index = focus.items++;
    focus.ids.emplace_back(id);
    if (!focus.id.empty() && focus.id == id) {
        focus.focus = index;
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##b", size) && enabled;
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x +
                           ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y >
                       0.0f) {
        focus.focus = index;
    }
    const bool focused = focus.focus == index;
    const ImVec2 end(at.x + size.x, at.y + size.y);
    // The game's row; the focused one framed in gold over it (the game's selected row is tinted
    // blue, out of place on this screen's stone).
    if (MenuArt::Nine(draw, MenuArt::Row, at, end, 12.0f, S(9.0f), enabled ? 1.0f : 0.5f)) {
        if (focused && enabled) {
            const float in = S(2.0f);
            draw->AddRectFilled(ImVec2(at.x + in, at.y + in), ImVec2(end.x - in, end.y - in), Gold(0.10f));
            draw->AddRect(ImVec2(at.x + in, at.y + in), ImVec2(end.x - in, end.y - in), Gold(0.9f), 0.0f, 0, S(1.5f));
        }
    } else {
        draw->AddRectFilled(at, end, focused ? Gold(primary ? 0.30f : 0.20f) : Gold(primary ? 0.12f : 0.05f),
                            S(2.0f));
        draw->AddRect(at, end, focused ? Gold(0.95f) : Gold(enabled ? 0.45f : 0.15f), S(2.0f), 0, S(1.2f));
    }
    const ImVec2 ts = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(at.x + (size.x - ts.x) * 0.5f, at.y + (size.y - ts.y) * 0.5f),
                  !enabled ? Dim(0.6f) : focused ? Bright() : Plain(), label);
    return clicked || (enabled && focused && focus.activate);
}

const char* StateText(const bbcoop_lobby_entry& e, bool member) {
    if (e.state == BBCOOP_LOBBY_PLAYING) {
        return T("In the hunt", "На охоте");
    }
    if (e.state == BBCOOP_LOBBY_OFFLINE) {
        return T("Away", "Не в сети");
    }
    if (member && e.ready) {
        return T("Ready", "Готов");
    }
    return T("Choosing", "Выбирает");
}

ImU32 StateColor(const bbcoop_lobby_entry& e, bool member) {
    if (e.state == BBCOOP_LOBBY_OFFLINE) {
        return Dim();
    }
    if (e.state == BBCOOP_LOBBY_PLAYING || (member && e.ready)) {
        return Gold();
    }
    return Plain(0.85f);
}

/// A hunter's place: the emblem, the name, the state; an empty one says so.
void Card(ImDrawList* draw, ImVec2 at, ImVec2 size, const bbcoop_lobby_entry* member, bool leader,
          int place = 0) {
    const ImVec2 end(at.x + size.x, at.y + size.y);
    if (MenuArt::Has(MenuArt::Card)) {
        const bool mine = member != nullptr && std::strcmp(member->id, lobby.me) == 0;
        HunterCard(draw, at, end, mine, member ? 1.0f : 0.55f);
        const float fit = size.x - S(28.0f);
        const float mid = (at.x + end.x) * 0.5f;
        // The emblem: the hunter's rune in a square slot, framed in gold once ready.
        const float slot = S(96.0f), sy = at.y + S(34.0f);
        const bool ready = member && (member->ready || member->state == BBCOOP_LOBBY_PLAYING);
        if (!MenuArt::Draw(draw, ready ? MenuArt::SlotSelected : MenuArt::Slot, ImVec2(mid - slot * 0.5f, sy),
                           ImVec2(mid + slot * 0.5f, sy + slot), member ? 1.0f : 0.5f)) {
            draw->AddRect(ImVec2(mid - slot * 0.5f, sy), ImVec2(mid + slot * 0.5f, sy + slot), Gold(0.4f));
        }
        if (member) {
            const float rune = slot * 0.58f;
            MenuArt::Rune(draw, place, ImVec2(mid - rune * 0.5f, sy + (slot - rune) * 0.5f),
                          ImVec2(mid + rune * 0.5f, sy + (slot + rune) * 0.5f),
                          member->state == BBCOOP_LOBBY_OFFLINE ? 0.35f : 1.0f);
        }
        const auto centred = [&](const char* text, float y, ImU32 color) {
            const std::string line = Fit(text, fit);
            const ImVec2 ts = ImGui::CalcTextSize(line.c_str());
            draw->AddText(ImVec2(mid - ts.x * 0.5f, y), color, line.c_str());
        };
        // The marks: the leader's hat at the top left, the ready quill at the top right.
        if (member && CoopArt::Has()) {
            const float mark = S(44.0f), in = S(34.0f);
            if (leader) {
                CoopArt::Centred(draw, CoopArt::Piece::MarkLeader, ImVec2(at.x + in, at.y + in), mark, 0.95f);
            }
            if (ready) {
                CoopArt::Centred(draw, CoopArt::Piece::MarkReady, ImVec2(end.x - in, at.y + in), mark, 0.95f);
            }
        }
        ImGui::PushFont(MenuArt::Font(), 26.0f);
        centred(member ? member->name : T("An empty place", "Свободное место"), at.y + S(146.0f),
                member ? Bright() : Dim(0.7f));
        ImGui::PopFont();
        ImGui::PushFont(MenuArt::Font(), 19.0f);
        if (member) {
            centred(StateText(*member, true), at.y + S(184.0f), StateColor(*member, true));
            if (leader) {
                centred(T("Leader, hosts the hunt", "Лидер, принимает у себя"), at.y + S(210.0f), Gold(0.75f));
            }
        } else {
            centred(T("Invite a hunter below", "Пригласите охотника ниже"), at.y + S(184.0f), Dim(0.6f));
        }
        ImGui::PopFont();
        return;
    }
    draw->AddRectFilledMultiColor(at, end, IM_COL32(28, 24, 20, 235), IM_COL32(28, 24, 20, 235),
                                  IM_COL32(10, 9, 8, 235), IM_COL32(10, 9, 8, 235));
    draw->AddRect(at, end, member ? Gold(0.55f) : Gold(0.18f), S(2.0f), 0, S(1.2f));
    Ornament(draw, at.x + S(14.0f), end.x - S(14.0f), at.y + S(12.0f));
    const float mid = (at.x + end.x) * 0.5f;
    // The emblem: a hunter's diamond, filled when the place is taken.
    const float r = S(34.0f), cy = at.y + S(96.0f);
    const ImU32 emblem = member ? StateColor(*member, true) : Dim(0.35f);
    draw->AddQuad(ImVec2(mid, cy - r), ImVec2(mid + r, cy), ImVec2(mid, cy + r), ImVec2(mid - r, cy), emblem,
                  S(2.0f));
    if (member) {
        const float k = r * 0.55f;
        draw->AddQuadFilled(ImVec2(mid, cy - k), ImVec2(mid + k, cy), ImVec2(mid, cy + k), ImVec2(mid - k, cy),
                            emblem);
    }
    ImGui::PushFont(MenuArt::Font(), 24.0f);
    const char* name = member ? member->name : T("An empty place", "Свободное место");
    ImVec2 ns = ImGui::CalcTextSize(name);
    draw->AddText(ImVec2(mid - ns.x * 0.5f, at.y + S(156.0f)), member ? Bright() : Dim(0.7f), name);
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    if (member) {
        const char* state = StateText(*member, true);
        ns = ImGui::CalcTextSize(state);
        draw->AddText(ImVec2(mid - ns.x * 0.5f, at.y + S(194.0f)), StateColor(*member, true), state);
        if (leader) {
            const char* lead = T("Leader, hosts the hunt", "Лидер, принимает у себя");
            ns = ImGui::CalcTextSize(lead);
            draw->AddText(ImVec2(mid - ns.x * 0.5f, at.y + S(222.0f)), Gold(0.75f), lead);
        }
    } else {
        const char* note = T("Invite a hunter below", "Пригласите охотника ниже");
        ns = ImGui::CalcTextSize(note);
        draw->AddText(ImVec2(mid - ns.x * 0.5f, at.y + S(194.0f)), Dim(0.6f), note);
    }
    ImGui::PopFont();
    Ornament(draw, at.x + S(14.0f), end.x - S(14.0f), end.y - S(12.0f));
}

/// The team menu in game: the server's last refusal or what the screen does, and its keys.
void MenuHint(ImDrawList* draw, ImVec2 win, ImVec2 size, float y, bool leader_hosts) {
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    const char* hint = leader_hosts
                           ? T("You host the hunt: remove the others before you leave, or quit to the title.",
                               "Вы принимаете охоту: исключите остальных перед уходом или выйдите в меню.")
                       : lobby.in_hunt ? T("Invite hunters online; they join your hunt where they stand.",
                                           "Приглашайте охотников в сети: они присоединятся там, где стоят.")
                       : lobby.in_team && lobby.playing
                           ? T("Join the hunt: this character goes into the team's world where it stands.",
                               "Вступите в охоту: персонаж войдёт в мир команды там, где стоит.")
                           : T("Accept an invitation to join a team's hunt, or create a team.",
                               "Примите приглашение, чтобы вступить в охоту команды, или создайте команду.");
    const char* line = lobby.notice[0] ? lobby.notice : hint;
    ImVec2 ts = ImGui::CalcTextSize(line);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, y),
                  lobby.notice[0] ? ImU32(IM_COL32(220, 150, 110, 255)) : Plain(0.8f), line);
    const Keys::Hint keys[] = {{"Dpad", "Arrows", T("Move", "Выбор")},
                               {"Cross", "Enter", T("Choose", "Нажать")},
                               {"Circle", "Esc", T("Close", "Закрыть")}};
    Keys::Guide(draw, keys, win.x + S(36.0f), win.x + size.x - S(36.0f),
                win.y + size.y - S(BbKeyGuide::BottomClearance));
    ImGui::PopFont();
}

/// Kaniel 2026-10-17: the world list, as the game's Load Game lists its saves: one row per world
/// of the server's (and a new one first), the d-pad moves, Cross picks, Triangle forgets (pressed
/// twice), Circle goes back. Only the team's leader (or a player in no team yet: the team it makes
/// takes the pick) opens it.
struct WorldPicker {
    bool open = false;
    int row = 0; // 0: a new world, 1..: lobby.worlds[row - 1]
    int top = 0; // the first row shown
    bool moved = false; // the player moved the focus (else it follows the chosen world)
    std::string armed; // the world Triangle asked about, and when
    double armed_at = 0.0;
};
WorldPicker picker;

bool CanPickWorld() {
    return lobby.signed_in && !lobby.menu && (lobby.in_team ? lobby.leader && !lobby.started : true);
}

/// Kaniel 2026-10-25: how the team's deaths play out, one for the whole team: its leader picks it
/// (at the title and in the game), the others see it. A caption over the mode, as the world's
/// button; in no team the default (down for a revive) shows, greyed.
void DeathModeButton(ImDrawList* draw, ImVec2 at, ImVec2 size) {
    const bool pick = lobby.signed_in && lobby.in_team && lobby.leader;
    const bool spectate = lobby.in_team && lobby.death_mode == BBCOOP_DEATH_SPECTATE;
    const ImVec2 end(at.x + size.x, at.y + size.y);
    bool lit = false;
    if (pick) {
        const int index = focus.items;
        if (Button(draw, "deaths", "", at, size)) {
            bbcoop_lobby_request(BBCOOP_TEAM_DEATH_MODE, spectate ? "0" : "1");
        }
        lit = focus.focus == index;
    } else if (!MenuArt::Nine(draw, MenuArt::Row, at, end, 12.0f, S(9.0f), 0.5f)) {
        draw->AddRect(at, end, Gold(0.15f), S(2.0f), 0, S(1.2f));
    }
    const float fit = size.x - S(24.0f);
    ImGui::PushFont(MenuArt::Font(), 16.0f);
    const std::string caption = Fit(T("Death mode", "Режим смерти"), fit);
    ImVec2 cs = ImGui::CalcTextSize(caption.c_str());
    draw->AddText(ImVec2(at.x + (size.x - cs.x) * 0.5f, at.y + S(6.0f)), Gold(pick ? 0.85f : 0.6f), caption.c_str());
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 21.0f);
    const std::string mode =
        Fit(spectate ? T("Spectate teammates", "Наблюдать за союзниками") : T("Go down (revive)", "Падать (поднимают)"),
            fit);
    cs = ImGui::CalcTextSize(mode.c_str());
    draw->AddText(ImVec2(at.x + (size.x - cs.x) * 0.5f, at.y + S(26.0f)), lit ? Bright() : Plain(pick ? 1.0f : 0.8f),
                  mode.c_str());
    ImGui::PopFont();
}

/// What the death mode means, under the buttons while its button has the focus.
const char* DeathModeHint() {
    return lobby.in_team && lobby.death_mode == BBCOOP_DEATH_SPECTATE
               ? T("Spectate: a hunter who falls watches a teammate until one uses a lamp or the boss dies, then "
                   "wakes at their own lamp. Everyone down is still a wipe.",
                   "Наблюдение: павший охотник смотрит за союзником, пока кто-то не воспользуется фонарём или "
                   "босс не падёт, и затем очнётся у своего фонаря. Если пали все — это всё ещё поражение.")
               : T("Go down: a hunter who falls stays down until a teammate strikes them back up, or gives up "
                   "with {Circle}.",
                   "Падение: павший охотник лежит, пока союзник не поднимет его ударами, или сдаётся {Circle}.");
}

void OpenWorldPicker() {
    picker.open = true;
    picker.row = lobby.world >= 0 && lobby.world < lobby.world_count ? lobby.world + 1 : 0;
    picker.top = 0;
    picker.moved = false;
    picker.armed.clear();
}

/// The list over the whole screen (the team screen's wall behind it).
void DrawWorldPicker(ImDrawList* draw, ImVec2 win, ImVec2 size) {
    const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
        for (const ImGuiKey key : keys) {
            if (ImGui::IsKeyPressed(key, true)) {
                return true;
            }
        }
        return false;
    };
    const int rows = lobby.world_count + 1;
    if (!picker.moved) {
        picker.row = lobby.world >= 0 && lobby.world < lobby.world_count ? lobby.world + 1 : 0;
    }
    picker.row = std::clamp(picker.row, 0, rows - 1);
    if (pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp})) {
        picker.row = (picker.row + rows - 1) % rows;
        picker.moved = true;
    } else if (pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown, ImGuiKey_Tab})) {
        picker.row = (picker.row + 1) % rows;
        picker.moved = true;
    }
    bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    bool choose = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                  ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    bool forget = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);

    ImGui::PushFont(MenuArt::Font(), 40.0f);
    const char* title = T("Choose a World", "Выбор мира");
    ImVec2 ts = ImGui::CalcTextSize(title);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, win.y + S(30.0f)), Bright(), title);
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 20.0f);
    const char* about = T("Each world keeps its own progress on the server; any team may hunt in it.",
                          "Каждый мир хранит свой прогресс на сервере; охотиться в нём может любая команда.");
    ts = ImGui::CalcTextSize(about);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, win.y + S(84.0f)), Gold(0.85f), about);
    ImGui::PopFont();

    // The rows, as many as fit; the list scrolls with the focused one.
    const float list_w = std::min(S(1100.0f), size.x - S(80.0f));
    const float list_x = win.x + (size.x - list_w) * 0.5f;
    const float list_y = win.y + S(130.0f), list_end = win.y + size.y - S(170.0f);
    const float row_h = S(104.0f), row_gap = S(8.0f);
    const int shown = std::max(1, static_cast<int>((list_end - list_y + row_gap) / (row_h + row_gap)));
    if (picker.row < picker.top) {
        picker.top = picker.row;
    } else if (picker.row >= picker.top + shown) {
        picker.top = picker.row - shown + 1;
    }
    picker.top = std::clamp(picker.top, 0, std::max(0, rows - shown));
    for (int r = picker.top; r < rows && r < picker.top + shown; ++r) {
        const bbcoop_lobby_world* w = r == 0 ? nullptr : &lobby.worlds[r - 1];
        const bool current = r == 0 ? lobby.world < 0 : lobby.world == r - 1;
        const bool busy = w && w->busy;
        const ImVec2 at(list_x, list_y + (r - picker.top) * (row_h + row_gap));
        const ImVec2 end(at.x + list_w, at.y + row_h);
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(r);
        const bool clicked = ImGui::InvisibleButton("##world", ImVec2(list_w, row_h));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) {
            picker.row = r;
            picker.moved = true;
        }
        if (clicked) {
            picker.row = r;
            picker.moved = true;
            choose = true;
        }
        const bool focused = picker.row == r;
        if (MenuArt::Nine(draw, MenuArt::Row, at, end, 12.0f, S(9.0f), busy ? 0.5f : 1.0f)) {
            if (focused) {
                const float in = S(2.0f);
                draw->AddRectFilled(ImVec2(at.x + in, at.y + in), ImVec2(end.x - in, end.y - in), Gold(0.10f));
                draw->AddRect(ImVec2(at.x + in, at.y + in), ImVec2(end.x - in, end.y - in), Gold(0.9f), 0.0f, 0,
                              S(1.5f));
            }
        } else {
            draw->AddRectFilled(at, end, focused ? Gold(0.18f) : Gold(0.05f), S(2.0f));
            draw->AddRect(at, end, focused ? Gold(0.95f) : Gold(busy ? 0.15f : 0.45f), S(2.0f), 0, S(1.2f));
        }
        const float tx = at.x + S(24.0f), tw = list_w - S(48.0f);
        ImGui::PushFont(MenuArt::Font(), 26.0f);
        const std::string name = w ? w->name : T("A new world", "Новый мир");
        draw->AddText(ImVec2(tx, at.y + S(10.0f)), busy ? Dim(0.6f) : focused ? Bright() : Plain(),
                      Fit(name.c_str(), tw - S(220.0f)).c_str());
        if (current) {
            const char* mark = T("Chosen", "Выбран");
            const ImVec2 ms = ImGui::CalcTextSize(mark);
            draw->AddText(ImVec2(end.x - S(24.0f) - ms.x, at.y + S(10.0f)), Gold(), mark);
        }
        ImGui::PopFont();
        ImGui::PushFont(MenuArt::Font(), 19.0f);
        std::string line, hunters;
        if (!w) {
            line = T("Starts from the leader's save; kept on the server from the first hunt.",
                     "Начинается с сохранения лидера; хранится на сервере с первой охоты.");
        } else {
            const auto add = [&](const std::string& part) {
                if (!part.empty()) {
                    line += (line.empty() ? "" : "   ·   ") + part;
                }
            };
            add(w->when[0] ? std::string(T("Last played ", "Последняя игра ")) + w->when : std::string{});
            add(w->created[0] ? std::string(T("Made ", "Создан ")) + w->created : std::string{});
            if (w->lamps > 0) {
                const std::string count = std::to_string(w->lamps);
                const std::string en = count + (w->lamps == 1 ? " lamp lit" : " lamps lit");
                const std::string ru = "фонарей: " + count;
                add(T(en.c_str(), ru.c_str()));
            }
            hunters = w->characters[0] ? std::string(T("Hunters: ", "Охотники: ")) + w->characters
                                       : std::string(T("No hunter yet", "Ещё никто не играл"));
        }
        draw->AddText(ImVec2(tx, at.y + S(46.0f)), busy ? Dim(0.6f) : Plain(0.85f), Fit(line.c_str(), tw).c_str());
        float hunters_w = tw;
        if (busy) {
            // Right on the hunters' line: why it cannot be picked now.
            const char* note = T("Another team is hunting here now", "Здесь сейчас охотится другая команда");
            const float nw = ImGui::CalcTextSize(note).x;
            draw->AddText(ImVec2(end.x - S(24.0f) - nw, at.y + S(72.0f)), IM_COL32(220, 150, 110, 255), note);
            hunters_w = tw - nw - S(24.0f);
        }
        if (!hunters.empty()) {
            draw->AddText(ImVec2(tx, at.y + S(72.0f)), busy ? Dim(0.7f) : Gold(0.75f),
                          Fit(hunters.c_str(), hunters_w).c_str());
        }
        ImGui::PopFont();
    }
    // More above or below: the game's arrows' place, as a line of dots.
    ImGui::PushFont(MenuArt::Font(), 19.0f);
    if (rows > shown) {
        const std::string where = std::to_string(picker.row + 1) + " / " + std::to_string(rows);
        ts = ImGui::CalcTextSize(where.c_str());
        draw->AddText(ImVec2(list_x + list_w - ts.x, list_end + S(4.0f)), Dim(0.8f), where.c_str());
    }
    ImGui::PopFont();

    // Back and Forget for the mouse too; Forget asks first.
    const bbcoop_lobby_world* focused_world = picker.row > 0 ? &lobby.worlds[picker.row - 1] : nullptr;
    const bool armed = focused_world && picker.armed == focused_world->id && ImGui::GetTime() - picker.armed_at < 4.0;
    ImGui::PushFont(MenuArt::Font(), 22.0f);
    const float bw = S(300.0f), bh = S(50.0f), gap = S(24.0f), by = win.y + size.y - S(176.0f);
    float bx = win.x + (size.x - (2 * bw + gap)) * 0.5f;
    const auto plain_button = [&](const char* id, const char* label, ImVec2 at, bool enabled) {
        ImGui::SetCursorScreenPos(at);
        const bool hit = ImGui::InvisibleButton(id, ImVec2(bw, bh)) && enabled;
        const bool hover = ImGui::IsItemHovered();
        const ImVec2 end(at.x + bw, at.y + bh);
        if (!MenuArt::Nine(draw, MenuArt::Row, at, end, 12.0f, S(9.0f), enabled ? 1.0f : 0.5f)) {
            draw->AddRect(at, end, Gold(enabled ? 0.45f : 0.15f), S(2.0f), 0, S(1.2f));
        }
        if (hover && enabled) {
            draw->AddRect(at, end, Gold(0.9f), 0.0f, 0, S(1.5f));
        }
        const ImVec2 ls = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(at.x + (bw - ls.x) * 0.5f, at.y + (bh - ls.y) * 0.5f), enabled ? Plain() : Dim(0.6f),
                      label);
        return hit;
    };
    if (plain_button("##forget", armed ? T("Forget it? Press again", "Забыть? Нажмите ещё раз")
                                       : T("Forget this world", "Забыть этот мир"),
                     ImVec2(bx, by), focused_world && !focused_world->busy)) {
        forget = true;
    }
    bx += bw + gap;
    if (plain_button("##back", T("Back", "Назад"), ImVec2(bx, by), true)) {
        back = true;
    }
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    const char* line = lobby.notice[0] ? lobby.notice
                       : armed ? T("Forgotten worlds go off the list; the server keeps their files aside.",
                                   "Забытые миры исчезают из списка; сервер сохраняет их файлы отдельно.")
                               : nullptr;
    if (line) {
        ts = ImGui::CalcTextSize(line);
        draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, by + bh + S(20.0f)),
                      lobby.notice[0] ? ImU32(IM_COL32(220, 150, 110, 255)) : Plain(0.75f), line);
    }
    const Keys::Hint keys[] = {{"UpDown", "Arrows", T("Move", "Выбор")},
                               {"Cross", "Enter", T("Choose", "Выбрать")},
                               {"Triangle", "Delete", T("Forget", "Забыть")},
                               {"Circle", "Esc", T("Back", "Назад")}};
    Keys::Guide(draw, keys, win.x + S(36.0f), win.x + size.x - S(36.0f),
                win.y + size.y - S(BbKeyGuide::BottomClearance));
    ImGui::PopFont();

    if (forget && focused_world && !focused_world->busy) {
        if (armed) {
            picker.armed.clear();
            bbcoop_lobby_request(BBCOOP_TEAM_FORGET_WORLD, focused_world->id);
        } else {
            picker.armed = focused_world->id;
            picker.armed_at = ImGui::GetTime();
        }
    } else if (choose && !(focused_world && focused_world->busy)) {
        bbcoop_lobby_request(BBCOOP_TEAM_WORLD, focused_world ? focused_world->id : "");
        picker.open = false;
    } else if (back) {
        picker.open = false;
    }
}

/// Amanises 2026-10-25: the friends page, over the team screen as the world list is: a search for
/// players by name (typed on the keyboard), the friends with their state (invite the online ones,
/// remove), the requests waiting for an answer and the ones sent, and what the search found.
/// Circle / Escape goes back.
struct FriendsPage {
    bool open = false;
    char query[40] = {};
    bool typing = false; // the search box had the keyboard last frame: keys are its, not the focus's
    std::string removing; // the friend Remove asked about (pressed twice), and when
    double removing_at = 0.0;
    // Amanises 2026-10-25: the team screen's box over Hunters online (only the names holding it).
    char online_filter[40] = {};
    bool online_typing = false;
};
FriendsPage friends;

void OpenFriends() {
    friends.open = true;
    friends.removing.clear();
    focus.fresh = true;
    // The press that opened it is used up: over the game the team screen's fresh focus is its Close
    // button, which the same press closed at once (the screen and the game's page under it).
    focus.activate = false;
    std::snprintf(friends.query, sizeof(friends.query), "%s", lobby.search_query);
}

/// One player's row: the name, then `status` right-aligned before `buttons` of width each.
void FriendRow(ImDrawList* draw, float x, float y, float w, float h, const bbcoop_lobby_entry& p,
               const char* status, ImU32 status_color, int buttons, float button_w) {
    MenuArt::Nine(draw, MenuArt::Row, ImVec2(x - S(6.0f), y - S(2.0f)), ImVec2(x + w + S(4.0f), y + h - S(4.0f)),
                  10.0f, S(6.0f), 0.7f);
    const float buttons_w = buttons * (button_w + S(6.0f));
    const float status_x = x + w - buttons_w - S(10.0f) - ImGui::CalcTextSize(status).x;
    draw->AddText(ImVec2(x + S(8.0f), y + S(8.0f)), p.state == BBCOOP_LOBBY_OFFLINE ? Plain(0.6f) : Plain(),
                  Fit(p.name, status_x - x - S(20.0f)).c_str());
    draw->AddText(ImVec2(status_x, y + S(8.0f)), status_color, status);
}

void DrawFriends(ImDrawList* draw, ImVec2 win, ImVec2 size) {
    const bool back = !friends.typing && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
    ImGui::PushFont(MenuArt::Font(), 40.0f);
    const char* title = T("Friends", "Друзья");
    ImVec2 ts = ImGui::CalcTextSize(title);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, win.y + S(30.0f)), Bright(), title);
    ImGui::PopFont();

    // The search: the box, Search and Clear.
    ImGui::PushFont(MenuArt::Font(), 22.0f);
    const float box_w = std::min(S(560.0f), size.x * 0.4f), box_h = S(44.0f), sbw = S(150.0f), gap = S(16.0f);
    const float sx = win.x + (size.x - (box_w + 2 * (sbw + gap))) * 0.5f, sy = win.y + S(96.0f);
    ImGui::SetCursorScreenPos(ImVec2(sx, sy + (box_h - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::SetNextItemWidth(box_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.055f, 0.05f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Gold(0.6f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool entered = ImGui::InputTextWithHint("##friend_search", T("Hunter's name", "Имя охотника"),
                                                  friends.query, sizeof(friends.query),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    friends.typing = ImGui::IsItemActive();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    float bx = sx + box_w + gap;
    if (Button(draw, "friend_search", T("Search", "Найти"), ImVec2(bx, sy), ImVec2(sbw, box_h)) || entered) {
        bbcoop_lobby_request(BBCOOP_TEAM_SEARCH, friends.query);
    }
    bx += sbw + gap;
    if (Button(draw, "friend_clear", T("Clear", "Очистить"), ImVec2(bx, sy), ImVec2(sbw, box_h),
               friends.query[0] != 0 || lobby.search_query[0] != 0)) {
        friends.query[0] = 0;
        bbcoop_lobby_request(BBCOOP_TEAM_SEARCH, "");
    }
    ImGui::PopFont();

    // Three lists: friends, requests (received, then sent), what the search found.
    const float lists_y = sy + box_h + S(28.0f), lists_end = win.y + size.y - S(176.0f);
    const float col_gap = S(24.0f), col_w = (size.x - S(72.0f) - 2 * col_gap) / 3.0f;
    const float col_x[3] = {win.x + S(36.0f), win.x + S(36.0f) + col_w + col_gap,
                            win.x + S(36.0f) + 2 * (col_w + col_gap)};
    const float row_h = S(42.0f), small_w = S(104.0f);
    const int rows = std::max(1, static_cast<int>((lists_end - lists_y - S(50.0f)) / row_h));
    ImGui::PushFont(MenuArt::Font(), 22.0f);
    const std::string friends_title =
        std::string(T("Your friends", "Ваши друзья")) + "  (" + std::to_string(lobby.friend_count) + ")";
    draw->AddText(ImVec2(col_x[0], lists_y), Gold(), friends_title.c_str());
    draw->AddText(ImVec2(col_x[1], lists_y), Gold(), T("Friend requests", "Заявки в друзья"));
    draw->AddText(ImVec2(col_x[2], lists_y), Gold(), T("Found", "Найдено"));
    ImGui::PopFont();
    for (const float x : col_x) {
        ListRule(draw, x, x + col_w, lists_y + S(36.0f));
    }
    ImGui::PushFont(MenuArt::Font(), 19.0f);
    const bool can_invite = lobby.in_team && lobby.member_count < BBCOOP_LOBBY_TEAM;
    const auto more = [&](float x, float y, int left) {
        if (left > 0) {
            const std::string line = std::string(T("and more: ", "и ещё: ")) + std::to_string(left);
            draw->AddText(ImVec2(x + S(8.0f), y + S(8.0f)), Dim(), line.c_str());
        }
    };

    // Friends: online first (the server's order). Remove asks again before it removes.
    float y = lists_y + S(50.0f);
    if (lobby.friend_count == 0) {
        draw->AddText(ImVec2(col_x[0], y + S(8.0f)), Dim(),
                      T("No friends yet: search for a hunter", "Пока нет друзей: найдите охотника"));
    }
    const int friend_rows = std::min(lobby.friend_count, rows);
    for (int i = 0; i < friend_rows; ++i, y += row_h) {
        const bbcoop_lobby_entry& f = lobby.friends[i];
        const bool armed = friends.removing == f.id && ImGui::GetTime() - friends.removing_at < 4.0;
        const char* status = f.in_my_team ? T("In your team", "В вашей команде") : StateText(f, false);
        FriendRow(draw, col_x[0], y, col_w, row_h, f, status, f.in_my_team ? Gold() : StateColor(f, false), 2,
                  small_w);
        const float b1 = col_x[0] + col_w - 2 * small_w - S(6.0f), b2 = col_x[0] + col_w - small_w;
        const ImVec2 bs(small_w, row_h - S(6.0f));
        if (Button(draw, (std::string("finvite") + f.id).c_str(), T("Invite", "Пригласить"), ImVec2(b1, y), bs,
                   can_invite && !f.in_my_team && f.state != BBCOOP_LOBBY_OFFLINE)) {
            bbcoop_lobby_request(BBCOOP_TEAM_INVITE, f.id);
        }
        if (Button(draw, (std::string("fremove") + f.id).c_str(), armed ? T("Sure?", "Точно?") : T("Remove", "Удалить"),
                   ImVec2(b2, y), bs)) {
            if (armed) {
                friends.removing.clear();
                bbcoop_lobby_request(BBCOOP_TEAM_FRIEND_REMOVE, f.id);
            } else {
                friends.removing = f.id;
                friends.removing_at = ImGui::GetTime();
            }
        }
    }
    more(col_x[0], y, lobby.friend_count - friend_rows);

    // Requests: the ones to answer, then the ones sent (Cancel takes one back).
    y = lists_y + S(50.0f);
    int used = 0;
    if (lobby.request_count == 0 && lobby.sent_count == 0) {
        draw->AddText(ImVec2(col_x[1], y + S(8.0f)), Dim(), T("None", "Нет"));
    }
    for (int i = 0; i < lobby.request_count && used < rows; ++i, ++used, y += row_h) {
        const bbcoop_lobby_entry& r = lobby.requests[i];
        FriendRow(draw, col_x[1], y, col_w, row_h, r, T("asks you", "просит"), Gold(), 2, small_w);
        const float b1 = col_x[1] + col_w - 2 * small_w - S(6.0f), b2 = col_x[1] + col_w - small_w;
        const ImVec2 bs(small_w, row_h - S(6.0f));
        if (Button(draw, (std::string("faccept") + r.id).c_str(), T("Accept", "Принять"), ImVec2(b1, y), bs, true,
                   true)) {
            bbcoop_lobby_request(BBCOOP_TEAM_FRIEND_ADD, r.id);
        }
        if (Button(draw, (std::string("fdecline") + r.id).c_str(), T("Decline", "Отклонить"), ImVec2(b2, y), bs)) {
            bbcoop_lobby_request(BBCOOP_TEAM_FRIEND_DECLINE, r.id);
        }
    }
    for (int i = 0; i < lobby.sent_count && used < rows; ++i, ++used, y += row_h) {
        const bbcoop_lobby_entry& s = lobby.sent[i];
        FriendRow(draw, col_x[1], y, col_w, row_h, s, T("asked", "отправлено"), Plain(0.7f), 1, small_w);
        if (Button(draw, (std::string("fcancel") + s.id).c_str(), T("Cancel", "Отменить"),
                   ImVec2(col_x[1] + col_w - small_w, y), ImVec2(small_w, row_h - S(6.0f)))) {
            bbcoop_lobby_request(BBCOOP_TEAM_FRIEND_REMOVE, s.id);
        }
    }
    more(col_x[1], y, lobby.request_count + lobby.sent_count - used);

    // The search: each found hunter with what can be done (online or not).
    y = lists_y + S(50.0f);
    if (lobby.search_count == 0) {
        draw->AddText(ImVec2(col_x[2], y + S(8.0f)), Dim(),
                      lobby.search_query[0] ? T("Nobody by that name", "Никого с таким именем")
                                            : T("Type a name and press Search", "Введите имя и нажмите «Найти»"));
    }
    for (int i = 0; i < lobby.search_count && i < rows; ++i, y += row_h) {
        const bbcoop_lobby_entry& p = lobby.search[i];
        const bool self = p.relation == BBCOOP_FRIEND_SELF;
        FriendRow(draw, col_x[2], y, col_w, row_h, p, self ? T("You", "Вы") : StateText(p, false),
                  StateColor(p, false), 1, small_w);
        const char* label = p.relation == BBCOOP_FRIEND_YES        ? T("Friends", "Друзья")
                            : p.relation == BBCOOP_FRIEND_SENT     ? T("Asked", "Отправлено")
                            : p.relation == BBCOOP_FRIEND_RECEIVED ? T("Accept", "Принять")
                            : self                                 ? "-"
                                                                   : T("Add", "Добавить");
        const bool can = p.relation == BBCOOP_FRIEND_NONE || p.relation == BBCOOP_FRIEND_RECEIVED;
        if (Button(draw, (std::string("fadd") + p.id).c_str(), label, ImVec2(col_x[2] + col_w - small_w, y),
                   ImVec2(small_w, row_h - S(6.0f)), can && !self, p.relation == BBCOOP_FRIEND_RECEIVED)) {
            bbcoop_lobby_request(BBCOOP_TEAM_FRIEND_ADD, p.id);
        }
    }
    ImGui::PopFont();

    // Back, the server's last refusal or the hint, and the keys.
    ImGui::PushFont(MenuArt::Font(), 24.0f);
    const float bw = S(260.0f), bh = S(54.0f), by = win.y + size.y - S(180.0f);
    const bool pressed_back = Button(draw, "friends_back", T("Back", "Назад"),
                                     ImVec2(win.x + (size.x - bw) * 0.5f, by), ImVec2(bw, bh));
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    const char* line = lobby.notice[0]
                           ? lobby.notice
                           : T("Friends are kept on the server with your account. Type with the keyboard to search.",
                               "Друзья хранятся на сервере вместе с аккаунтом. Для поиска печатайте на клавиатуре.");
    ts = ImGui::CalcTextSize(line);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, by + bh + S(20.0f)),
                  lobby.notice[0] ? ImU32(IM_COL32(220, 150, 110, 255)) : Plain(0.8f), line);
    const Keys::Hint keys[] = {{"Dpad", "Arrows", T("Move", "Выбор")},
                               {"Cross", "Enter", T("Choose", "Нажать")},
                               {"Circle", "Esc", T("Back", "Назад")}};
    Keys::Guide(draw, keys, win.x + S(36.0f), win.x + size.x - S(36.0f),
                win.y + size.y - S(BbKeyGuide::BottomClearance));
    ImGui::PopFont();
    if (back || pressed_back) {
        friends.open = false;
        friends.typing = false;
        focus.fresh = true;
    }
}

void Screen() {
    const GreyAccent grey; // Kaniel 2026-10-18: the souls counter's grey, not gold
    const auto pressed_any = [](std::initializer_list<ImGuiKey> keys) {
        for (const ImGuiKey key : keys) {
            if (ImGui::IsKeyPressed(key, true)) {
                return true;
            }
        }
        return false;
    };
    focus.move = pressed_any({ImGuiKey_UpArrow, ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadUp,
                              ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickLeft})
                     ? -1
                 : pressed_any({ImGuiKey_DownArrow, ImGuiKey_RightArrow, ImGuiKey_Tab, ImGuiKey_GamepadDpadDown,
                                ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickDown,
                                ImGuiKey_GamepadLStickRight})
                     ? 1
                     : 0;
    focus.activate = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    focus.items = 0;
    focus.ids.clear();
    PollAsk();
    // Amanises 2026-10-25: while the friends page's search box has the keyboard, the keys type.
    if (asking != Ask::None || (friends.open ? friends.typing : friends.online_typing)) {
        focus.move = 0; // the pad's other buttons stay with the text entry
        focus.activate = false;
    }
    // Kaniel 2026-10-16: opened from the game's System menu (lobby.menu), Circle or Escape closes
    // it, as the game's own pages close (the friends page takes them first: back to the team).
    if (lobby.menu && !friends.open && !friends.online_typing && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        bbcoop_team_menu(0);
    }
    // Over the game the world stays in sight behind it, darkened as under the game's menus.
    const bool over_game = lobby.menu && lobby.playing;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* back = ImGui::GetBackgroundDrawList();
    if (over_game) {
        back->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 170));
        MenuArt::Vignette(back, ImVec2(0, 0), display, 0.85f);
    } else {
        back->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 255));
        if (MenuArt::Cover(back, MenuArt::Title, ImVec2(0, 0), display, 0.55f)) {
            MenuArt::Vignette(back, ImVec2(0, 0), display, 0.85f);
        }
    }
    // Kaniel 2026-10-17: the screen is the whole screen (the menus' wall edge to edge, as the game's
    // own pages), its content in the middle, as wide as reads well.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    const bool wall = MenuArt::Has(MenuArt::Wall);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, wall ? ImVec4(0, 0, 0, 0) : ImVec4(0.035f, 0.032f, 0.028f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##bbport_team", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (wall) {
        MenuArt::Cover(draw, MenuArt::Wall, ImVec2(0, 0), display, 1.0f);
        MenuArt::Vignette(draw, ImVec2(0, 0), display, 0.75f);
    }
    const ImVec2 size(std::min(S(1600.0f), display.x - S(80.0f)), display.y - S(40.0f));
    const ImVec2 win((display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f);
    const float x0 = win.x + S(24.0f), x1 = win.x + size.x - S(24.0f);
    Divider(draw, x0, x1, win.y + S(14.0f));
    Divider(draw, x0, x1, win.y + size.y - S(14.0f));
    if (bbcoop_team_world_list_asked() && CanPickWorld()) {
        OpenWorldPicker();
    }
    if (picker.open && !CanPickWorld()) {
        picker.open = false; // the team began, or its leader is another now
    }
    if (picker.open) {
        DrawWorldPicker(draw, win, size);
        focus.fresh = true; // back on the team screen: the focus goes to Begin again
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }
    if (friends.open) {
        DrawFriends(draw, win, size);
        if (focus.items > 0) {
            focus.focus = ((focus.focus + focus.move) % focus.items + focus.items) % focus.items;
            focus.id = focus.ids[static_cast<size_t>(focus.focus)];
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    // Title and the team's state.
    ImGui::PushFont(MenuArt::Font(), 40.0f);
    const char* title = T("The Hunt Together", "Совместная охота");
    ImVec2 ts = ImGui::CalcTextSize(title);
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, win.y + S(30.0f)), Bright(), title);
    ImGui::PopFont();
    if (MenuArt::Has(MenuArt::HeaderOrnament)) {
        const ImVec2 o = MenuArt::SizeOf(MenuArt::HeaderOrnament);
        const float ow = S(o.x * 0.9f), oh = ow * o.y / o.x, mid = win.x + size.x * 0.5f;
        MenuArt::Draw(draw, MenuArt::HeaderOrnament, ImVec2(mid - ow * 0.5f, win.y + S(70.0f)),
                      ImVec2(mid + ow * 0.5f, win.y + S(70.0f) + oh), 0.8f);
    }
    ImGui::PushFont(MenuArt::Font(), 20.0f);
    std::string status;
    if (!lobby.signed_in) {
        status = T("Reaching the co-op server...", "Подключение к кооп-серверу...");
    } else if (!lobby.in_team) {
        status = T("Not in a team: create one or accept an invitation",
                   "Не в команде: создайте её или примите приглашение");
    } else {
        status = std::string(lobby.members[0].name) + T("'s team", " — команда") + "   " +
                 std::to_string(lobby.member_count) + T(" of ", " из ") + std::to_string(lobby.team_size);
        if (lobby.leader && lobby.code[0]) {
            // For a friend the lobby does not list: it types the code (Join by code).
            status += std::string("   ") + T("Team code", "Код команды") + ": " + lobby.code;
        }
    }
    ts = ImGui::CalcTextSize(status.c_str());
    draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, win.y + S(MenuArt::Has(MenuArt::HeaderOrnament) ? 102.0f : 84.0f)),
                  Gold(0.85f), status.c_str());
    ImGui::PopFont();

    // The four places.
    const float card_w = S(290.0f), card_h = S(270.0f), gap = S(24.0f);
    const float cards_x = win.x + (size.x - (4 * card_w + 3 * gap)) * 0.5f,
                header_y = win.y + S(MenuArt::Has(MenuArt::HeaderOrnament) ? 140.0f : 128.0f);
    // Full screen: the cards and the two lists (five rows) as one block, in the middle of the room
    // between the title and the buttons.
    const float block_h = card_h + S(30.0f) + S(50.0f) + 5 * S(42.0f);
    const float room = (win.y + size.y - S(166.0f) - S(30.0f)) - header_y;
    const float cards_y = header_y + std::max(0.0f, (room - block_h) * 0.5f);
    for (int i = 0; i < BBCOOP_LOBBY_TEAM; ++i) {
        const bool taken = lobby.in_team && i < lobby.member_count;
        Card(draw, ImVec2(cards_x + i * (card_w + gap), cards_y), ImVec2(card_w, card_h),
             taken ? &lobby.members[i] : nullptr, taken && i == 0, i);
    }
    // Kaniel 2026-10-16: the leader removes a member from the team (the server's word: that
    // member's game leaves the hunt where it stands and plays on alone).
    if (lobby.leader) {
        ImGui::PushFont(MenuArt::Font(), 19.0f);
        const ImVec2 remove_size(S(170.0f), S(38.0f));
        for (int i = 1; i < lobby.member_count && i < BBCOOP_LOBBY_TEAM; ++i) {
            const bbcoop_lobby_entry& m = lobby.members[i];
            const std::string id = std::string("remove") + m.id;
            const ImVec2 at(cards_x + i * (card_w + gap) + (card_w - remove_size.x) * 0.5f,
                            cards_y + card_h - remove_size.y - S(12.0f));
            if (Button(draw, id.c_str(), T("Remove", "Исключить"), at, remove_size)) {
                bbcoop_lobby_request(BBCOOP_TEAM_REMOVE, m.id);
            }
        }
        ImGui::PopFont();
    }

    // The players online (invite) and the invitations.
    const float lists_y = cards_y + card_h + S(30.0f);
    const float list_w = (size.x - S(72.0f) - gap) * 0.5f;
    const float left_x = win.x + S(36.0f), right_x = left_x + list_w + gap;
    ImGui::PushFont(MenuArt::Font(), 22.0f);
    draw->AddText(ImVec2(left_x, lists_y), Gold(), T("Hunters online", "Охотники в сети"));
    // Amanises 2026-10-25: a player in no team sees the open teams there too.
    draw->AddText(ImVec2(right_x, lists_y), Gold(),
                  lobby.in_team ? T("Invitations", "Приглашения")
                                : T("Invitations and open teams", "Приглашения и открытые команды"));
    ImGui::PopFont();
    ListRule(draw, left_x, left_x + list_w, lists_y + S(36.0f));
    ListRule(draw, right_x, right_x + list_w, lists_y + S(36.0f));
    ImGui::PushFont(MenuArt::Font(), 19.0f);
    const float row_h = S(42.0f), button_w = S(130.0f);
    {
        // Beside the lists' titles: a friend not listed, by its name (into this team) or by the
        // team's code (into its team); the leader makes a new code (the old one joins no more).
        const ImVec2 ask_size(S(220.0f), S(36.0f));
        const float ask_y = lists_y - S(4.0f);
        if (Button(draw, "byname", T("Invite by name", "Пригласить по имени"),
                   ImVec2(left_x + list_w - ask_size.x, ask_y), ask_size,
                   lobby.in_team && lobby.member_count < BBCOOP_LOBBY_TEAM && asking == Ask::None)) {
            BeginAsk(Ask::Name);
        }
        if (lobby.in_team && lobby.leader) {
            if (Button(draw, "newcode", T("New code", "Новый код"), ImVec2(right_x + list_w - ask_size.x, ask_y),
                       ask_size)) {
                bbcoop_lobby_request(BBCOOP_TEAM_NEW_CODE, "");
            }
        } else if (Button(draw, "bycode", T("Join by code", "Вступить по коду"),
                          ImVec2(right_x + list_w - ask_size.x, ask_y), ask_size,
                          lobby.signed_in && !lobby.in_team && asking == Ask::None)) {
            BeginAsk(Ask::Code);
        }
        // Amanises 2026-10-25: the friends page, before "Invite by name" (requests waiting
        // counted on it).
        const std::string label = lobby.request_count > 0 ? std::string(T("Friends", "Друзья")) + " (" +
                                                                 std::to_string(lobby.request_count) + ")"
                                                           : std::string(T("Friends", "Друзья"));
        const ImVec2 fsize(S(170.0f), ask_size.y);
        if (Button(draw, "friends", label.c_str(),
                   ImVec2(left_x + list_w - ask_size.x - S(12.0f) - fsize.x, ask_y), fsize,
                   lobby.signed_in != 0 && asking == Ask::None, lobby.request_count > 0)) {
            OpenFriends();
        }
    }
    // As many rows as fit above the world row and the buttons (5 on the old 900 px panel).
    const int rows = 5;
    float y = lists_y + S(50.0f);
    const bool can_invite = lobby.in_team && lobby.member_count < BBCOOP_LOBBY_TEAM;
    // Amanises 2026-10-25: a box over the list keeps only the names holding its text (any case); a
    // name nobody here has goes to the friends page's search of the whole server with Enter (a
    // public server lists only some of its players here).
    const float filter_h = S(36.0f);
    ImGui::SetCursorScreenPos(ImVec2(left_x - S(2.0f), y + (filter_h - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::SetNextItemWidth(list_w + S(6.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.055f, 0.05f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Gold(0.45f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool filter_entered =
        ImGui::InputTextWithHint("##online_filter", T("Search hunters online", "Поиск охотников в сети"),
                                 friends.online_filter, sizeof(friends.online_filter),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    friends.online_typing = ImGui::IsItemActive();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    y += filter_h + S(8.0f);
    const auto lower = [](const char* text) {
        std::string out(text ? text : "");
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    const std::string wanted = lower(friends.online_filter);
    std::vector<int> shown;
    for (int i = 0; i < lobby.player_count; ++i) {
        if (wanted.empty() || lower(lobby.players[i].name).find(wanted) != std::string::npos) {
            shown.push_back(i);
        }
    }
    if (filter_entered && !wanted.empty() && shown.empty() && lobby.signed_in) {
        OpenFriends();
        std::snprintf(friends.query, sizeof(friends.query), "%s", friends.online_filter);
        bbcoop_lobby_request(BBCOOP_TEAM_SEARCH, friends.query);
    }
    if (lobby.player_count == 0) {
        draw->AddText(ImVec2(left_x, y + S(8.0f)), Dim(), T("Nobody else is online", "Больше никого нет в сети"));
    } else if (shown.empty()) {
        draw->AddText(ImVec2(left_x, y + S(8.0f)), Dim(),
                      Fit(T("Nobody here by that name: Enter searches the whole server",
                            "Здесь нет такого имени: Enter ищет по всему серверу"),
                          list_w - S(8.0f))
                          .c_str());
    }
    for (int n = 0; n < static_cast<int>(shown.size()) && n < rows - 1; ++n, y += row_h) {
        const bbcoop_lobby_entry& p = lobby.players[shown[static_cast<size_t>(n)]];
        MenuArt::Nine(draw, MenuArt::Row, ImVec2(left_x - S(6.0f), y - S(2.0f)),
                      ImVec2(left_x + list_w + S(4.0f), y + row_h - S(4.0f)), 10.0f, S(6.0f), 0.7f);
        // The state right-aligned before the buttons, the name in what is left. Amanises
        // 2026-10-25: a friend says so; another player can be asked from here (Add).
        const bool is_friend = p.relation == BBCOOP_FRIEND_YES;
        const char* state = is_friend ? T("Friend", "Друг") : StateText(p, false);
        const float add_w = S(110.0f);
        const float state_x = left_x + list_w - button_w - add_w - S(24.0f) - ImGui::CalcTextSize(state).x;
        draw->AddText(ImVec2(left_x + S(8.0f), y + S(8.0f)), Plain(),
                      Fit(p.name, state_x - left_x - S(24.0f)).c_str());
        draw->AddText(ImVec2(state_x, y + S(8.0f)), is_friend ? Gold() : StateColor(p, false), state);
        // A friend already: no button (the state says so).
        const char* add = p.relation == BBCOOP_FRIEND_SENT       ? T("Asked", "Отправлено")
                          : p.relation == BBCOOP_FRIEND_RECEIVED ? T("Accept", "Принять")
                                                                 : T("Add", "Добавить");
        const std::string add_id = std::string("befriend") + p.id;
        if (!is_friend &&
            Button(draw, add_id.c_str(), add, ImVec2(left_x + list_w - button_w - add_w - S(6.0f), y),
                   ImVec2(add_w, row_h - S(6.0f)),
                   p.relation == BBCOOP_FRIEND_NONE || p.relation == BBCOOP_FRIEND_RECEIVED)) {
            bbcoop_lobby_request(BBCOOP_TEAM_FRIEND_ADD, p.id);
        }
        const std::string id = std::string("invite") + p.id;
        if (Button(draw, id.c_str(), T("Invite", "Пригласить"), ImVec2(left_x + list_w - button_w, y),
                   ImVec2(button_w, row_h - S(6.0f)), can_invite)) {
            bbcoop_lobby_request(BBCOOP_TEAM_INVITE, p.id);
        }
    }
    y = lists_y + S(50.0f);
    if (lobby.invite_count == 0 && lobby.ask_count == 0 && (lobby.in_team || lobby.open_count == 0)) {
        draw->AddText(ImVec2(right_x, y + S(8.0f)), Dim(), T("None", "Нет"));
    }
    for (int i = 0; i < lobby.invite_count && i < rows; ++i, y += row_h) {
        const bbcoop_lobby_entry& inv = lobby.invites[i];
        const std::string text = std::string(inv.name) + T(" calls you to hunt", " зовёт на охоту");
        MenuArt::Nine(draw, MenuArt::Row, ImVec2(right_x - S(6.0f), y - S(2.0f)),
                      ImVec2(right_x + list_w + S(4.0f), y + row_h - S(4.0f)), 10.0f, S(6.0f), 0.7f);
        float text_x = right_x + S(8.0f);
        if (CoopArt::Has()) {
            CoopArt::Centred(draw, CoopArt::Piece::MarkInvite, ImVec2(text_x + S(14.0f), y + row_h * 0.5f - S(3.0f)),
                             S(26.0f), 0.95f);
            text_x += S(36.0f);
        }
        draw->AddText(ImVec2(text_x, y + S(8.0f)), Plain(),
                      Fit(text.c_str(), list_w - 2 * button_w - S(32.0f) - (text_x - right_x - S(8.0f))).c_str());
        const std::string accept = std::string("accept") + inv.id, decline = std::string("decline") + inv.id;
        if (Button(draw, accept.c_str(), T("Join", "Вступить"),
                   ImVec2(right_x + list_w - 2 * button_w - S(8.0f), y), ImVec2(button_w, row_h - S(6.0f)),
                   !lobby.in_team, true)) {
            if (lobby.playing) {
                bbcoop_team_join_hunt(inv.id); // in game: into the team and its hunt at once
            } else {
                bbcoop_lobby_request(BBCOOP_TEAM_ACCEPT, inv.id);
            }
        }
        if (Button(draw, decline.c_str(), T("Decline", "Отклонить"), ImVec2(right_x + list_w - button_w, y),
                   ImVec2(button_w, row_h - S(6.0f)))) {
            bbcoop_lobby_request(BBCOOP_TEAM_DECLINE, inv.id);
        }
    }
    // Amanises 2026-10-25, open teams: under the invitations, the leader's asks to join (take
    // in or say no) and, for a player in no team, the open teams to ask.
    int right_rows = std::min(lobby.invite_count, rows);
    for (int i = 0; i < lobby.ask_count && right_rows < rows; ++i, ++right_rows, y += row_h) {
        const bbcoop_lobby_entry& ask = lobby.asks[i];
        const std::string text = std::string(ask.name) + T(" asks to join", " просится в команду");
        MenuArt::Nine(draw, MenuArt::Row, ImVec2(right_x - S(6.0f), y - S(2.0f)),
                      ImVec2(right_x + list_w + S(4.0f), y + row_h - S(4.0f)), 10.0f, S(6.0f), 0.7f);
        draw->AddText(ImVec2(right_x + S(8.0f), y + S(8.0f)), Plain(),
                      Fit(text.c_str(), list_w - 2 * button_w - S(32.0f)).c_str());
        const std::string take = std::string("askok") + ask.id, refuse = std::string("askno") + ask.id;
        if (Button(draw, take.c_str(), T("Accept", "Принять"), ImVec2(right_x + list_w - 2 * button_w - S(8.0f), y),
                   ImVec2(button_w, row_h - S(6.0f)), lobby.member_count < lobby.team_size, true)) {
            bbcoop_lobby_request(BBCOOP_TEAM_ASK_ACCEPT, ask.id);
        }
        if (Button(draw, refuse.c_str(), T("Decline", "Отклонить"), ImVec2(right_x + list_w - button_w, y),
                   ImVec2(button_w, row_h - S(6.0f)))) {
            bbcoop_lobby_request(BBCOOP_TEAM_ASK_DECLINE, ask.id);
        }
    }
    if (!lobby.in_team) {
        const float ask_w = S(170.0f);
        for (int i = 0; i < lobby.open_count && right_rows < rows; ++i, ++right_rows, y += row_h) {
            const bbcoop_lobby_open& team = lobby.open_teams[i];
            const std::string count = std::to_string(team.members) + "/" + std::to_string(lobby.team_size);
            const char* state = team.started ? T("In the hunt", "На охоте") : "";
            MenuArt::Nine(draw, MenuArt::Row, ImVec2(right_x - S(6.0f), y - S(2.0f)),
                          ImVec2(right_x + list_w + S(4.0f), y + row_h - S(4.0f)), 10.0f, S(6.0f), 0.7f);
            const float count_x = right_x + list_w - ask_w - S(18.0f) - ImGui::CalcTextSize(count.c_str()).x;
            const float state_x = count_x - (state[0] ? ImGui::CalcTextSize(state).x + S(14.0f) : 0.0f);
            const std::string name = std::string(team.name) + T("'s team", " — команда");
            draw->AddText(ImVec2(right_x + S(8.0f), y + S(8.0f)), Plain(),
                          Fit(name.c_str(), state_x - right_x - S(20.0f)).c_str());
            if (state[0]) {
                draw->AddText(ImVec2(state_x, y + S(8.0f)), Dim(), state);
            }
            draw->AddText(ImVec2(count_x, y + S(8.0f)), Gold(), count.c_str());
            const std::string id = std::string("askjoin") + team.id;
            // An open team is joined at once; one that asks waits on its leader.
            const char* label = team.open    ? T("Join", "Вступить")
                                : team.asked ? T("Asked", "Отправлено")
                                             : T("Ask to join", "Попроситься");
            if (Button(draw, id.c_str(), label, ImVec2(right_x + list_w - ask_w, y), ImVec2(ask_w, row_h - S(6.0f)),
                       true, !team.asked)) {
                bbcoop_lobby_request(team.asked && !team.open ? BBCOOP_TEAM_ASK_CANCEL : BBCOOP_TEAM_ASK_JOIN,
                                     team.id);
            }
        }
    }
    ImGui::PopFont();

    // The buttons: the team, Begin the hunt, the world.
    ImGui::PushFont(MenuArt::Font(), 24.0f);
    const float bw = S(260.0f), bh = S(54.0f);
    // Kaniel 2026-10-19: high enough for the line under them and the key guide over the ornament.
    const float by = win.y + size.y - S(180.0f);
    // Kaniel 2026-10-25: over the game a fourth, the team's duel; the death mode the last.
    const int buttons = lobby.menu ? 5 : 4;
    float bx = win.x + (size.x - (buttons * bw + (buttons - 1) * gap)) * 0.5f;
    // The leader hosts the hunt: it leaves only once it is alone in it (its game leaving would
    // end the others' session).
    const bool leader_hosts = lobby.leader && lobby.in_hunt && lobby.member_count > 1;
    // Invasions (docs/INVASIONS.md): the leader allows invaders into the team's world, over the
    // buttons (the server keeps them out unless its operator turned invasions on).
    if (lobby.in_team && lobby.leader) {
        // Amanises 2026-10-25, open teams: how the team is joined, each press the next way (by
        // invite only, listed with its leader taking who asks, listed for anyone to join).
        const char* join = lobby.join == BBCOOP_JOIN_OPEN ? T("Join: open to all", "Вход: открыт всем")
                           : lobby.join == BBCOOP_JOIN_ASK ? T("Join: ask to join", "Вход: по запросу")
                                                           : T("Join: invite only", "Вход: по приглашению");
        if (Button(draw, "openteam", join, ImVec2(bx, by - bh - S(14.0f)), ImVec2(bw, bh), lobby.signed_in != 0)) {
            const char* next = lobby.join == BBCOOP_JOIN_INVITE_ONLY ? "1" : lobby.join == BBCOOP_JOIN_ASK ? "2" : "0";
            bbcoop_lobby_request(BBCOOP_TEAM_OPEN, next);
        }
        const char* invaders = lobby.allow_invaders ? T("Invaders: allowed", "Захватчики: разрешены")
                                                    : T("Invaders: off", "Захватчики: запрещены");
        if (Button(draw, "invaders", invaders, ImVec2(win.x + (size.x - bw) * 0.5f, by - bh - S(14.0f)),
                   ImVec2(bw, bh), lobby.signed_in != 0)) {
            bbcoop_team_allow_invaders(lobby.allow_invaders ? 0 : 1);
        }
    }
    if (!lobby.in_team) {
        if (Button(draw, "create", T("Create a team", "Создать команду"), ImVec2(bx, by), ImVec2(bw, bh),
                   lobby.signed_in != 0)) {
            bbcoop_lobby_request(BBCOOP_TEAM_CREATE, "");
        }
    } else if (Button(draw, "leave", T("Leave the team", "Покинуть команду"), ImVec2(bx, by), ImVec2(bw, bh),
                      !leader_hosts)) {
        bbcoop_lobby_request(BBCOOP_TEAM_LEAVE, "");
    }
    bx += bw + gap;
    if (lobby.menu) {
        // Kaniel 2026-10-16, the team menu in game: join the hunt from here (this save, where it
        // stands), and close back to the game's menu.
        if (focus.fresh) {
            focus.fresh = false;
            focus.focus = focus.items;
            focus.id = lobby.playing && lobby.in_team && !lobby.in_hunt ? "hunt" : "close";
        }
        // Invasions: a player in no team invades another team's world from here instead (a red
        // phantom), and leaves the search or the invasion the same way.
        bbcoop_invasion invasion{};
        const bool invading = bbcoop_invasion_view(&invasion) != 0 && invasion.role == BBCOOP_INVASION_INVADER &&
                              invasion.phase != BBCOOP_INVASION_ENDED;
        if (!lobby.in_team && lobby.playing) {
            const char* invade =
                invading ? T("Leave the invasion", "Покинуть вторжение") : T("Invade a hunt", "Вторгнуться в охоту");
            if (Button(draw, "invade", invade, ImVec2(bx, by), ImVec2(bw, bh), lobby.signed_in != 0)) {
                bbcoop_invade(invading ? 0 : 1);
            }
        } else {
            const char* hunt = lobby.in_hunt ? T("In the hunt", "На охоте") : T("Join the hunt", "Вступить в охоту");
            if (Button(draw, "hunt", hunt, ImVec2(bx, by), ImVec2(bw, bh),
                       lobby.playing && lobby.in_team && !lobby.in_hunt, true)) {
                bbcoop_team_join_hunt("");
            }
        }
        bx += bw + gap;
        // Kaniel 2026-10-25: a duel with the team members here (they answer in the world), or the
        // end of the one this player is in. Either goes back to the game.
        bbcoop_duel duel{};
        bbcoop_duel_view(&duel);
        if (duel.may_cancel) {
            if (Button(draw, "duel", T("End the duel", "Завершить дуэль"), ImVec2(bx, by), ImVec2(bw, bh))) {
                bbcoop_duel_cancel();
                bbcoop_team_menu(0);
            }
        } else if (Button(draw, "duel", T("Duel", "Дуэль"), ImVec2(bx, by), ImVec2(bw, bh),
                          duel.may_propose != 0)) {
            bbcoop_duel_propose();
            bbcoop_team_menu(0);
        }
        bx += bw + gap;
        if (Button(draw, "close", T("Close", "Закрыть"), ImVec2(bx, by), ImVec2(bw, bh))) {
            bbcoop_team_menu(0);
        }
        bx += bw + gap;
        ImGui::PopFont();
        DeathModeButton(draw, ImVec2(bx, by), ImVec2(bw, bh));
        if (focus.id == "deaths") {
            ImGui::PushFont(MenuArt::Font(), 18.0f);
            WrappedText(draw, ImVec2(win.x + S(80.0f), by + bh + S(16.0f)), size.x - S(160.0f), Plain(0.8f),
                        DeathModeHint());
            ImGui::PopFont();
        } else {
            MenuHint(draw, win, size, by + bh + S(20.0f), leader_hosts);
        }
        if (focus.items > 0) {
            focus.focus = ((focus.focus + focus.move) % focus.items + focus.items) % focus.items;
            focus.id = focus.ids[static_cast<size_t>(focus.focus)];
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }
    // Kaniel 2026-10-20: no Ready to press (a member's screen says it is ready by itself) and no
    // Play alone (alone is a team of one). The leader begins as soon as the members are in: one
    // still choosing goes in once its screen is up. In no team (the server out of reach too)
    // Begin plays this save alone.
    bool everyone_in = lobby.started != 0;
    if (!everyone_in) {
        everyone_in = true;
        for (int i = 0; i < lobby.member_count; ++i) {
            const bbcoop_lobby_entry& m = lobby.members[i];
            if (std::strcmp(m.id, lobby.me) != 0 && (!m.ready || m.state == BBCOOP_LOBBY_OFFLINE)) {
                everyone_in = false;
            }
        }
    }
    // Into a team (an invitation accepted): the focus goes to Begin.
    if (lobby.in_team && !focus.was_in_team) {
        focus.fresh = true;
    }
    focus.was_in_team = lobby.in_team != 0;
    if (focus.fresh && lobby.in_team) {
        focus.fresh = false;
        focus.focus = focus.items; // the next button's index
        focus.id = "go";
    }
    if (Button(draw, "go", T("Begin the hunt", "Начать охоту"), ImVec2(bx, by), ImVec2(bw, bh),
               !lobby.in_team || lobby.leader, true)) {
        bbcoop_team_choose(lobby.in_team ? BBCOOP_TEAM_GO_START : BBCOOP_TEAM_GO_ALONE);
    }
    bx += bw + gap;
    ImGui::PopFont();
    // Kaniel 2026-10-17: the world the hunt plays, one of the server's (any team plays any of them,
    // each kept apart). Its leader (or a player in no team yet: the team it makes takes it) opens
    // the list to choose it; the others see which it is. Kaniel 2026-10-20: in the third button's
    // place, its name under what the button does (the date is in the list).
    if (lobby.in_team || lobby.signed_in) {
        const bbcoop_lobby_world* chosen =
            lobby.world >= 0 && lobby.world < lobby.world_count ? &lobby.worlds[lobby.world] : nullptr;
        const char* world = chosen && chosen->name[0] ? chosen->name : T("A new world", "Новый мир");
        const bool pick = CanPickWorld();
        const ImVec2 at(bx, by), end(bx + bw, by + bh);
        bool lit = false;
        if (pick) {
            const int index = focus.items;
            if (Button(draw, "world", "", at, ImVec2(bw, bh))) {
                OpenWorldPicker();
            }
            lit = focus.focus == index;
        } else if (!MenuArt::Nine(draw, MenuArt::Row, at, end, 12.0f, S(9.0f), 0.5f)) {
            draw->AddRect(at, end, Gold(0.15f), S(2.0f), 0, S(1.2f));
        }
        const float fit = bw - S(24.0f);
        ImGui::PushFont(MenuArt::Font(), 16.0f);
        const std::string caption =
            Fit(pick ? T("Choose a world", "Выбрать мир") : T("The world", "Мир"), fit);
        ImVec2 cs = ImGui::CalcTextSize(caption.c_str());
        draw->AddText(ImVec2(bx + (bw - cs.x) * 0.5f, by + S(6.0f)), Gold(pick ? 0.85f : 0.6f), caption.c_str());
        ImGui::PopFont();
        ImGui::PushFont(MenuArt::Font(), 21.0f);
        const std::string name = Fit(world, fit);
        cs = ImGui::CalcTextSize(name.c_str());
        draw->AddText(ImVec2(bx + (bw - cs.x) * 0.5f, by + S(26.0f)), lit ? Bright() : Plain(pick ? 1.0f : 0.8f),
                      name.c_str());
        ImGui::PopFont();
        bx += bw + gap;
        DeathModeButton(draw, ImVec2(bx, by), ImVec2(bw, bh));
    }

    // The server's last refusal, the hint and the controls.
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    const char* hint = lobby.leader && everyone_in
                           ? T("Everyone is in: begin the hunt together.", "Все на месте: начните охоту вместе.")
                       : lobby.leader ? T("Begin when you like: hunters still choosing join the hunt at once.",
                                          "Начинайте, когда захотите: кто ещё выбирает, сразу вступит в охоту.")
                       : lobby.in_team ? T("The leader begins the hunt; you join it at once.",
                                           "Лидер начинает охоту; вы сразу вступите в неё.")
                                       : T("Your character plays its own world alone.",
                                           "Ваш персонаж играет в своём мире один.");
    const char* line = lobby.notice[0] ? lobby.notice : hint;
    if (!lobby.notice[0] && focus.id == "deaths") {
        // Kaniel 2026-10-25: what the death mode means while its button has the focus.
        WrappedText(draw, ImVec2(win.x + S(80.0f), by + bh + S(16.0f)), size.x - S(160.0f), Plain(0.8f),
                    DeathModeHint());
    } else {
        ts = ImGui::CalcTextSize(line);
        draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, by + bh + S(20.0f)),
                      lobby.notice[0] ? ImU32(IM_COL32(220, 150, 110, 255)) : Plain(0.8f), line);
    }
    ImGui::PopFont();
    {
        // Kaniel 2026-10-25: the host rules (the leader's launcher's Co-op settings), shown to all.
        std::string rules;
        const auto add = [&](bool on, const char* name) {
            if (on) {
                rules += rules.empty() ? "" : "  \xC2\xB7  ";
                rules += name;
            }
        };
        add(lobby.rule_rot, T("Rot", "Гниль"));
        add(lobby.rule_travel, lobby.rule_vote ? T("Travel together (vote)", "Путь вместе (голосование)")
                                               : T("Travel together", "Путь вместе"));
        add(lobby.rule_scale, T("Enemies scale with the team", "Враги сильнее с командой"));
        const std::string text = std::string(T("Host rules: ", "Правила хоста: ")) +
                                 (rules.empty() ? std::string(T("none", "нет")) : rules);
        ImGui::PushFont(MenuArt::Font(), 16.0f);
        ts = ImGui::CalcTextSize(text.c_str());
        draw->AddText(ImVec2(win.x + (size.x - ts.x) * 0.5f, by + bh + S(46.0f)), Plain(0.7f), text.c_str());
        ImGui::PopFont();
    }
    {
        // As the game's own key guide: the buttons' icons, then what they do.
        ImGui::PushFont(MenuArt::Font(), 18.0f);
        const Keys::Hint keys[] = {{"Dpad", "Arrows", T("Move", "Выбор")},
                                   {"Cross", "Enter", T("Choose", "Нажать")},
                                   {"L3+R3", "Insert", T("Settings", "Настройки")}};
        Keys::Guide(draw, keys, win.x + S(36.0f), win.x + size.x - S(36.0f),
                    win.y + size.y - S(BbKeyGuide::BottomClearance));
        ImGui::PopFont();
    }

    if (focus.items > 0) {
        focus.focus = ((focus.focus + focus.move) % focus.items + focus.items) % focus.items;
        focus.id = focus.ids[static_cast<size_t>(focus.focus)];
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace Team

// Kaniel 2026-10-14: the team loading screen, as in League of Legends but in Bloodborne's colours:
// each hunter's name, its stage and a bar, over the whole screen until every member is in the
// team's session (bbcoop_team_load_view); the hunt opens for everyone at once.

namespace Joining {
bool open = false; // present thread: as Visible() read it last
bbcoop_team_load load{};

/// The drawn look (no game art): plain cards and bars.
void PlainScreen() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(6, 5, 4, 255));
    const ImU32 gold = Ui::Gold();
    const ImU32 dim = IM_COL32(140, 128, 110, 255);
    ImFont* font = MenuArt::Font();
    const float unit = display.y / 40.0f;
    const auto centred = [&](const char* text, float size, float y, ImU32 colour) {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        draw->AddText(font, size, ImVec2((display.x - extent.x) * 0.5f, y), colour, text);
    };
    centred(Team::T("The Hunt Begins", "Охота начинается"), unit * 1.6f, display.y * 0.16f, gold);
    // The cards across the middle 16:9 part of a wider screen (21:9, 32:9), not its whole width.
    const float wide = std::min(display.x, display.y * 16.0f / 9.0f), side = (display.x - wide) * 0.5f;
    const float left = side + wide * 0.18f, right = side + wide * 0.82f;
    draw->AddLine(ImVec2(left, display.y * 0.24f), ImVec2(right, display.y * 0.24f), dim, 1.0f);
    const float card_w = (right - left) / 4.0f;
    const int count = std::clamp(load.count, 1, 4);
    const float start = (display.x - card_w * count) * 0.5f;
    for (int i = 0; i < load.count && i < 4; ++i) {
        const bbcoop_team_load_row& row = load.rows[i];
        const float x0 = start + card_w * i + unit * 0.5f, x1 = start + card_w * (i + 1) - unit * 0.5f;
        const float y0 = display.y * 0.30f, y1 = display.y * 0.72f;
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(20, 17, 14, 255), 2.0f);
        draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), row.self ? gold : dim, 2.0f, 0, row.self ? 2.0f : 1.0f);
        const ImVec2 name_size = font->CalcTextSizeA(unit * 1.1f, FLT_MAX, 0.0f, row.name);
        draw->AddText(font, unit * 1.1f, ImVec2((x0 + x1 - name_size.x) * 0.5f, y0 + (y1 - y0) * 0.45f), gold,
                      row.name);
        const float bx0 = x0 + unit, bx1 = x1 - unit, by = y0 + (y1 - y0) * 0.84f;
        draw->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + unit * 0.45f), IM_COL32(40, 34, 28, 255));
        draw->AddRectFilled(ImVec2(bx0, by),
                            ImVec2(bx0 + (bx1 - bx0) * std::clamp(row.percent, 0, 100) / 100.0f, by + unit * 0.45f),
                            row.stage == BBCOOP_LOAD_READY ? IM_COL32(150, 40, 30, 255) : gold);
    }
}

const char* StageText(int stage) {
    return stage == BBCOOP_LOAD_READY     ? Team::T("Ready", "Готов")
           : stage == BBCOOP_LOAD_JOINING ? Team::T("Joining the hunt", "Присоединяется")
           : stage == BBCOOP_LOAD_LOADING ? Team::T("Loading", "Загрузка")
                                          : Team::T("Waiting", "Ожидание");
}

/// Kaniel 2026-10-14: as the game's own loading screen (black, the Bloodborne word, the menus'
/// cards and font), each hunter's card with its rune, name, stage and the game's own HP bar as
/// its progress; it opens for all of them at once. Kaniel 2026-10-15: it covers the whole screen,
/// the input and the sound (bbcoop_sound_held) until every guest's character has spawned; no
/// more top bar once this game's own world is in.
void Screen() {
    const Ui::GreyAccent grey; // Kaniel 2026-10-18: the souls counter's grey, not gold
    if (!MenuArt::Has(MenuArt::Card)) {
        PlainScreen();
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 255));
    // The menus' wall under it all, as the team screen has it (Kaniel 2026-10-17: the same
    // background as the party view; the co-op art's night city looked out of place); the Bloodborne
    // word above the cards.
    if (MenuArt::Cover(draw, MenuArt::Wall, ImVec2(0, 0), display, 1.0f)) {
        MenuArt::Vignette(draw, ImVec2(0, 0), display, 0.75f);
    }
    const float u = display.y / 1080.0f; // 1080p pixels
    ImFont* font = MenuArt::Font();
    const auto text = [&](const char* s, float size, ImVec2 at, ImU32 colour, float align) {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
        at.x = std::floor(at.x - extent.x * align);
        draw->AddText(font, size, ImVec2(at.x + 2.0f * u, at.y + 2.0f * u), IM_COL32(0, 0, 0, 200), s);
        draw->AddText(font, size, at, colour, s);
    };
    const float mid = display.x * 0.5f;
    if (MenuArt::Has(MenuArt::Logo)) {
        const ImVec2 logo = MenuArt::SizeOf(MenuArt::Logo);
        const float lw = 640.0f * u, lh = lw * logo.y / logo.x;
        MenuArt::Draw(draw, MenuArt::Logo, ImVec2(mid - lw * 0.5f, 120.0f * u), ImVec2(mid + lw * 0.5f, 120.0f * u + lh),
                      0.85f);
    }
    text(Team::T("The Hunt Begins", "Охота начинается"), 40.0f * u, ImVec2(mid, 290.0f * u),
         IM_COL32(222, 214, 196, 255), 0.5f);
    if (MenuArt::Has(MenuArt::HeaderOrnament)) {
        const ImVec2 o = MenuArt::SizeOf(MenuArt::HeaderOrnament);
        const float ow = 420.0f * u, oh = ow * o.y / o.x;
        MenuArt::Draw(draw, MenuArt::HeaderOrnament, ImVec2(mid - ow * 0.5f, 342.0f * u),
                      ImVec2(mid + ow * 0.5f, 342.0f * u + oh), 0.8f);
    }
    // The cards.
    const int count = std::clamp(load.count, 1, 4);
    const float card_w = 300.0f * u, card_h = 420.0f * u, gap = 36.0f * u;
    // Kaniel 2026-10-18: more room between the title's scrollwork and the cards (420 before).
    const float x_start = mid - (card_w * count + gap * (count - 1)) * 0.5f, y0 = 480.0f * u;
    for (int i = 0; i < load.count && i < 4; ++i) {
        const bbcoop_team_load_row& row = load.rows[i];
        const float x0 = x_start + i * (card_w + gap), x1 = x0 + card_w, cx = (x0 + x1) * 0.5f;
        const bool ready = row.stage == BBCOOP_LOAD_READY;
        Ui::HunterCard(draw, ImVec2(x0, y0), ImVec2(x1, y0 + card_h), row.self != 0);
        const float slot = 110.0f * u, sy = y0 + 40.0f * u;
        MenuArt::Draw(draw, ready ? MenuArt::SlotSelected : MenuArt::Slot, ImVec2(cx - slot * 0.5f, sy),
                      ImVec2(cx + slot * 0.5f, sy + slot));
        const float rune = slot * 0.58f;
        MenuArt::Rune(draw, i, ImVec2(cx - rune * 0.5f, sy + (slot - rune) * 0.5f),
                      ImVec2(cx + rune * 0.5f, sy + (slot + rune) * 0.5f), row.stage == BBCOOP_LOAD_WAITING ? 0.4f : 1.0f);
        if (ready && CoopArt::Has()) {
            CoopArt::Centred(draw, CoopArt::Piece::MarkReady, ImVec2(x1 - 34.0f * u, y0 + 34.0f * u), 38.0f * u,
                             0.95f);
        }
        text(row.name, 30.0f * u, ImVec2(cx, y0 + 180.0f * u), IM_COL32(222, 214, 196, 255), 0.5f);
        text(StageText(row.stage), 22.0f * u, ImVec2(cx, y0 + 228.0f * u),
             ready ? Ui::Gold() : IM_COL32(150, 142, 128, 255), 0.5f);
        // Progress on the game's own HP bar (red fill in its framed track).
        const float bar_w = card_w - 52.0f * u, bar_h = 17.0f * u, bx = cx - bar_w * 0.5f, by = y0 + 300.0f * u;
        const float fill = std::clamp(row.percent, 0, 100) / 100.0f;
        if (!Party::GameHpBar(draw, ImVec2(bx, by), bar_w, bar_h, fill, 1.0f)) {
            draw->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bar_w, by + bar_h), IM_COL32(40, 34, 28, 255));
            draw->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bar_w * fill, by + bar_h), IM_COL32(150, 40, 30, 255));
        }
        char percent[8];
        std::snprintf(percent, sizeof(percent), "%d%%", std::clamp(row.percent, 0, 100));
        text(percent, 22.0f * u, ImVec2(cx, by + 30.0f * u), IM_COL32(170, 160, 140, 255), 0.5f);
    }
    // The divider and the line under it.
    const ImVec2 d = MenuArt::SizeOf(MenuArt::Divider);
    if (d.x > 0.0f) {
        const float dw = 520.0f * u, dh = dw * d.y / d.x, dy = y0 + card_h + 50.0f * u;
        MenuArt::Draw(draw, MenuArt::Divider, ImVec2(mid - dw * 0.5f, dy), ImVec2(mid + dw * 0.5f, dy + dh), 0.8f);
    }
    text(Team::T("The hunt opens for everyone once all hunters are ready",
                 "Охота начнётся, когда все охотники будут готовы"),
         22.0f * u, ImVec2(mid, y0 + card_h + 100.0f * u), IM_COL32(150, 142, 128, 255), 0.5f);
}
} // namespace Joining

// Party death (bbcoop.h): the other members down the left side, under the game's own bars, after
// Elden Ring Nightreign's party list; whom an attack would revive; this player's own down in the
// middle of the screen. Drawn shapes in the menu's colours; the HP bars are the game's own, from
// the player's game files (bbport_game_art.h), with drawn ones when those can't be read. No art
// from either game is shipped.
namespace Party {

bbcoop_panel panel{}; // present thread: as Visible() read it last
bool shown = false;
BbPartyLayout::Rect game{}; // present thread: the game's frame on screen, as Render() got it
/// How far the game's HUD groups moved toward the frame's edges (another aspect ratio than 16:9):
/// the list and the supplies stay beside the player's bars.
BbPartyLayout::Shift shift{};

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

// The pad inputs party death names (runtime_pad.c's order).
constexpr int InputCircle = 1, InputSquare = 2, InputL1 = 4, InputR1 = 5, InputR2 = 7, InputL3 = 8;

using Ui::Keys::IconSet;

/// The game's key guide icon of runtime_pad.c's `input` (the launcher's Xbox or keyboard one for
/// those players), when it was read from the game's files.
std::optional<MenuArt::Piece> InputIcon(int input) {
    const auto piece = MenuArt::KeyIcon(BbGameArt::KeyGuideTexture(input));
    return piece && MenuArt::Has(*piece) ? piece : std::nullopt;
}

/// The first of a binding text's entries ("Space, Return" -> "Space").
std::string FirstBinding(int input, int pad) {
    char text[96];
    runtime_pad_binding_text(input, pad, text, sizeof(text));
    std::string first = text;
    return first.substr(0, first.find(", "));
}

/// The button glyph for `input` with its left edge and middle at `at`, `height` tall: the icon set's
/// own (PlayStation shapes, Xbox letters, or the bound key on a key cap). Returns its width.
/// A shoulder button's or trigger's label in the player's icon set; null for any other button.
const char* ShoulderLabel(const std::string& pad) {
    if (pad.find("shoulder") == std::string::npos && pad.find("trigger") == std::string::npos) {
        return nullptr;
    }
    const bool right = pad.rfind("right", 0) == 0, trigger = pad.find("trigger") != std::string::npos;
    if (IconSet() == 's') {
        return right ? (trigger ? "ZR" : "R") : (trigger ? "ZL" : "L");
    }
    return IconSet() == 'x' ? (right ? (trigger ? "RT" : "RB") : (trigger ? "LT" : "LB"))
                            : (right ? (trigger ? "R2" : "R1") : (trigger ? "L2" : "L1"));
}

/// How wide ButtonGlyph draws `input` at `height`: a key or a shoulder button's label may be wider
/// than the round face buttons.
float ButtonGlyphWidth(int input, float height) {
    if (InputIcon(input)) {
        return height;
    }
    if (IconSet() == 'k') {
        const std::string key = FirstBinding(input, 0);
        const char* label = key.empty() ? "?" : key.c_str();
        return std::max(sans_font->CalcTextSizeA(height * 0.62f, FLT_MAX, 0.0f, label).x + height * 0.5f,
                        height);
    }
    const std::string pad = FirstBinding(input, 1);
    if (const char* label = ShoulderLabel(pad)) {
        return sans_font->CalcTextSizeA(height * 0.6f, FLT_MAX, 0.0f, label).x + height * 0.6f;
    }
    return height;
}

float ButtonGlyph(ImDrawList* draw, int input, ImVec2 at, float height, float alpha) {
    // Kaniel 2026-10-19: the game's own icon (the drawn one below only when it could not be read).
    if (const auto piece = InputIcon(input)) {
        MenuArt::Draw(draw, *piece, ImVec2(at.x, at.y - height * 0.5f), ImVec2(at.x + height, at.y + height * 0.5f),
                      alpha);
        return height;
    }
    const char set = IconSet();
    const float r = height * 0.5f;
    const ImU32 ink = Bone(alpha), ground = Rgba(0.05f, 0.04f, 0.04f, 0.85f * alpha);
    if (set == 'k') {
        const std::string key = FirstBinding(input, 0);
        const char* label = key.empty() ? "?" : key.c_str();
        const float size = height * 0.62f;
        const float w = std::max(sans_font->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x + height * 0.5f,
                                 height);
        draw->AddRectFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + w, at.y + r), ground, height * 0.18f);
        draw->AddRect(ImVec2(at.x, at.y - r), ImVec2(at.x + w, at.y + r), Bone(0.7f * alpha),
                      height * 0.18f, 0, std::max(1.0f, height * 0.06f));
        draw->AddText(sans_font, size, ImVec2(at.x + height * 0.25f, at.y - size * 0.55f),
                      ink, label);
        return w;
    }
    const std::string pad = FirstBinding(input, 1);
    const ImVec2 centre(at.x + r, at.y);
    if (const char* label = ShoulderLabel(pad)) {
        const float size = height * 0.6f;
        const float w = sans_font->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x + height * 0.6f;
        draw->AddRectFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + w, at.y + r), ground, r);
        draw->AddRect(ImVec2(at.x, at.y - r), ImVec2(at.x + w, at.y + r), Bone(0.7f * alpha), r, 0,
                      std::max(1.0f, height * 0.06f));
        draw->AddText(sans_font, size, ImVec2(at.x + height * 0.3f, at.y - size * 0.55f), ink, label);
        return w;
    }
    draw->AddCircleFilled(centre, r, ground);
    draw->AddCircle(centre, r, Bone(0.7f * alpha), 0, std::max(1.0f, height * 0.06f));
    const char face = pad == "a" ? 'a' : pad == "b" ? 'b' : pad == "x" ? 'x' : pad == "y" ? 'y' : 0;
    if (set == 'x' || face == 0) {
        const char label[2] = {face ? char(face - 'a' + 'A') : '?', 0};
        const float size = height * 0.7f;
        const float w = sans_font->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x;
        draw->AddText(sans_font, size, ImVec2(centre.x - w * 0.5f, centre.y - size * 0.55f),
                      face == 'b' ? Blood(alpha) : ink, label);
        return height;
    }
    // PlayStation: Cross (a), Circle (b), Square (x), Triangle (y), in the game's white.
    const float s = r * 0.5f, line = std::max(1.5f, height * 0.09f);
    if (face == 'a') {
        draw->AddLine(ImVec2(centre.x - s, centre.y - s), ImVec2(centre.x + s, centre.y + s), ink, line);
        draw->AddLine(ImVec2(centre.x + s, centre.y - s), ImVec2(centre.x - s, centre.y + s), ink, line);
    } else if (face == 'b') {
        draw->AddCircle(centre, s, ink, 0, line);
    } else if (face == 'x') {
        draw->AddRect(ImVec2(centre.x - s * 0.9f, centre.y - s * 0.9f),
                      ImVec2(centre.x + s * 0.9f, centre.y + s * 0.9f), ink, 0.0f, 0, line);
    } else {
        draw->AddTriangle(ImVec2(centre.x, centre.y - s), ImVec2(centre.x + s, centre.y + s * 0.75f),
                          ImVec2(centre.x - s, centre.y + s * 0.75f), ink, line);
    }
    return height;
}

/// A line of words with a button glyph between `before` and `after`, centred on `mid`.
void TextWithGlyph(ImDrawList* draw, ImFont* font, float size, float mid, float y, ImU32 color,
                   const char* before, int input, const char* after) {
    const float gap = size * 0.35f, glyph_h = size * 1.25f;
    const float w0 = font->CalcTextSizeA(size, FLT_MAX, 0.0f, before).x;
    const float w1 = font->CalcTextSizeA(size, FLT_MAX, 0.0f, after).x;
    // The glyph's width depends on what it shows: measured by drawing it off screen first.
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch.AddDrawCmd();
    const float glyph_w = ButtonGlyph(&scratch, input, ImVec2(-10000.0f, -10000.0f), glyph_h, 0.0f);
    float x = mid - (w0 + gap + glyph_w + gap + w1) * 0.5f;
    Text(draw, font, size, ImVec2(x, y), color, before);
    x += w0 + gap;
    ButtonGlyph(draw, input, ImVec2(x, y + size * 0.55f), glyph_h, Alpha(color));
    x += glyph_w + gap;
    Text(draw, font, size, ImVec2(x, y), color, after);
}

void SupplyCounts(ImDrawList* draw, float right, float cy, float unit, uint32_t vials,
                  uint32_t bullets, float alpha);

void MemberList(ImDrawList* draw, float pulse) {
    using Ui::S;
    using Ui::T;
    const float x = BbPartyLayout::ListLeft(game, base_scale, shift), height = S(54.0f), gap = S(10.0f),
                bar = S(240.0f);
    float y = BbPartyLayout::ListTop(game, shift);
    char line[64];
    for (int i = 0; i < panel.member_count; ++i) {
        const bbcoop_panel_member& member = panel.members[i];
        const bool down = member.vital == BBCOOP_VITAL_DOWN;
        const bool dead = member.vital == BBCOOP_VITAL_DEAD || member.vital == BBCOOP_VITAL_SPECTATE;
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
        SupplyCounts(draw, text_x + bar, y + S(15.0f), base_scale, member.vials, member.bullets, a);
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
    std::snprintf(line, sizeof(line), T("strike to revive %s  %d%%", "ударьте, чтобы поднять %s  %d%%"),
                  member.name[0] != '\0' ? member.name : T("Hunter", "Охотник"),
                  RevivePercent(member.landed, member.needed));
    const float size = S(21.0f), mid = game.x + game.w * 0.5f,
                y = BbPartyLayout::RevivePromptY(game);
    const float w = serif_font->CalcTextSizeA(size, FLT_MAX, 0.0f, line).x + S(220.0f);
    const ImU32 band = Rgba(0.0f, 0.0f, 0.0f, 0.62f), clear = Rgba(0.0f, 0.0f, 0.0f, 0.0f);
    draw->AddRectFilledMultiColor(ImVec2(mid - w * 0.5f, y - S(10.0f)), ImVec2(mid, y + S(38.0f)),
                                  clear, band, band, clear);
    draw->AddRectFilledMultiColor(ImVec2(mid, y - S(10.0f)), ImVec2(mid + w * 0.5f, y + S(38.0f)),
                                  band, clear, clear, band);
    Ui::Ornament(draw, mid - w * 0.5f, mid + w * 0.5f, y - S(10.0f));
    Ui::Ornament(draw, mid - w * 0.5f, mid + w * 0.5f, y + S(38.0f));
    // R1 / R2 as the player's own buttons, then the words; the strike icon before them.
    const float glyph_h = size * 1.15f, gap = size * 0.3f;
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch.AddDrawCmd();
    const float g1 = ButtonGlyph(&scratch, InputR1, ImVec2(-10000.0f, -10000.0f), glyph_h, 0.0f);
    const float g2 = ButtonGlyph(&scratch, InputR2, ImVec2(-10000.0f, -10000.0f), glyph_h, 0.0f);
    const float slash = serif_font->CalcTextSizeA(size, FLT_MAX, 0.0f, "/").x;
    const float words = serif_font->CalcTextSizeA(size, FLT_MAX, 0.0f, line).x;
    const float total = g1 + gap + slash + gap + g2 + gap * 2.0f + words;
    float x = mid - total * 0.5f;
    const float glyph_y = y + S(2.0f) + size * 0.55f;
    Icon(draw, BbPartyIcons::Icon::Strike, ImVec2(x - S(30.0f), y + S(14.0f)), S(40.0f), 1.0f);
    x += ButtonGlyph(draw, InputR1, ImVec2(x, glyph_y), glyph_h, 1.0f) + gap;
    Text(draw, serif_font, size, ImVec2(x, y + S(2.0f)), Bone(), "/");
    x += slash + gap;
    x += ButtonGlyph(draw, InputR2, ImVec2(x, glyph_y), glyph_h, 1.0f) + gap * 2.0f;
    Text(draw, serif_font, size, ImVec2(x, y + S(2.0f)), Bone(), line);
}

/// The give-up hold's bar under the downed player's ring: an empty track with a faint frame (so it
/// shows before the button is held), filling while it's held.
void GiveUpBar(ImDrawList* draw, float mid, float ring_y) {
    using namespace BbPartyLayout;
    using Ui::S;
    const float y = ring_y + S(DownBarBelow), w = S(180.0f), h = S(DownBarHeight);
    const ImVec2 b0(mid - w * 0.5f, y), b1(mid + w * 0.5f, y + h);
    draw->AddRectFilled(b0, b1, Rgba(0.0f, 0.0f, 0.0f, 0.6f));
    if (panel.give_up > 0.0f) {
        draw->AddRectFilled(b0, ImVec2(b0.x + w * std::clamp(panel.give_up, 0.0f, 1.0f), b1.y),
                            Blood());
    }
    draw->AddRect(ImVec2(b0.x - S(1.0f), b0.y - S(1.0f)), ImVec2(b1.x + S(1.0f), b1.y + S(1.0f)),
                  Bone(0.45f), 0.0f, 0, S(1.0f));
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
    if (panel.native_prompt) { // the game's own prompt says it (CoopPrompt): only the hold's bar here
        GiveUpBar(draw, mid, centre.y);
        return;
    }
    Text(draw, serif_font, S(18.0f), ImVec2(mid, centre.y + S(DownHintBelow)), Ui::Plain(0.9f),
         T("A party member can raise you by striking you",
           "Союзник может поднять вас, нанося вам удары"),
         0.5f);
    // Hold <the dodge button, as the player's icon set shows it> to give up.
    const char* hold = T("Hold", "Удерживайте");
    const char* to_give_up = T("to give up", ", чтобы сдаться");
    const float give_up_w = sans_font->CalcTextSizeA(S(15.0f), FLT_MAX, 0.0f, hold).x +
                            sans_font->CalcTextSizeA(S(15.0f), FLT_MAX, 0.0f, to_give_up).x +
                            S(15.0f) * 2.0f;
    Icon(draw, BbPartyIcons::Icon::GiveUp,
         ImVec2(mid - give_up_w * 0.5f - S(30.0f), centre.y + S(DownGiveUpBelow) + S(9.0f)),
         S(28.0f), 0.95f);
    TextWithGlyph(draw, sans_font, S(15.0f), mid, centre.y + S(DownGiveUpBelow), Bone(0.85f), hold,
                  InputCircle, to_give_up);
    GiveUpBar(draw, mid, centre.y);
}

/// Words and button glyphs in a row centred on `mid` (each part a glyph when `input` >= 0).
struct GlyphPart {
    const char* words = nullptr;
    int input = -1;
};
void GlyphLine(ImDrawList* draw, ImFont* font, float size, float mid, float y, ImU32 color,
               std::initializer_list<GlyphPart> parts) {
    const float gap = size * 0.35f, glyph_h = size * 1.25f;
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch.AddDrawCmd();
    float total = 0.0f;
    for (const GlyphPart& part : parts) {
        total += (part.input >= 0
                      ? ButtonGlyph(&scratch, part.input, ImVec2(-10000.0f, -10000.0f), glyph_h, 0.0f)
                      : font->CalcTextSizeA(size, FLT_MAX, 0.0f, part.words).x) +
                 gap;
    }
    float x = mid - (total - gap) * 0.5f;
    for (const GlyphPart& part : parts) {
        if (part.input >= 0) {
            x += ButtonGlyph(draw, part.input, ImVec2(x, y + size * 0.55f), glyph_h, Alpha(color)) + gap;
        } else {
            Text(draw, font, size, ImVec2(x, y), color, part.words);
            x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, part.words).x + gap;
        }
    }
}

/// Kaniel 2026-10-25: this player spectates its teammates (the team's death mode): high in the
/// middle, whom it watches and what its buttons do, in the game's letters; the hold's bar under it.
void OwnSpectate(ImDrawList* draw) {
    using Ui::S;
    using Ui::T;
    const float mid = game.x + game.w * 0.5f, y = game.y + game.h * 0.07f;
    const ImU32 band = Rgba(0.0f, 0.0f, 0.0f, 0.55f), clear = Rgba(0.0f, 0.0f, 0.0f, 0.0f);
    const float w = S(560.0f);
    draw->AddRectFilledMultiColor(ImVec2(mid - w * 0.5f, y - S(8.0f)), ImVec2(mid, y + S(70.0f)), clear, band,
                                  band, clear);
    draw->AddRectFilledMultiColor(ImVec2(mid, y - S(8.0f)), ImVec2(mid + w * 0.5f, y + S(70.0f)), band, clear,
                                  clear, band);
    char line[96];
    if (panel.watching) {
        std::snprintf(line, sizeof(line), T("Spectating %s", "Наблюдение: %s"),
                      panel.watching_name[0] != '\0' ? panel.watching_name : T("Hunter", "Охотник"));
    } else {
        std::snprintf(line, sizeof(line), "%s", T("No teammate stands here", "Здесь не стоит ни один союзник"));
    }
    Text(draw, serif_font, S(24.0f), ImVec2(mid, y), Bone(), line, 0.5f);
    GlyphLine(draw, sans_font, S(15.0f), mid, y + S(36.0f), Bone(0.85f),
              {{nullptr, InputL1}, {"/"}, {nullptr, InputR1}, {T("Next teammate", "Следующий союзник")},
               {"   "}, {nullptr, InputCircle},
               {T("hold to wake at your lamp", "удерживайте, чтобы очнуться у своего фонаря")}});
    // The hold's bar, as the downed player's give-up.
    const float bw = S(180.0f), bh = S(4.0f), by = y + S(62.0f);
    const ImVec2 b0(mid - bw * 0.5f, by), b1(mid + bw * 0.5f, by + bh);
    draw->AddRectFilled(b0, b1, Rgba(0.0f, 0.0f, 0.0f, 0.6f));
    if (panel.give_up > 0.0f) {
        draw->AddRectFilled(b0, ImVec2(b0.x + bw * std::clamp(panel.give_up, 0.0f, 1.0f), b1.y), Bone(0.9f));
    }
}

/// The game's HUD shows the prompt (CoopPrompt): only its button, the player's own, where the game's
/// prompt has its button.
void PromptGlyph(ImDrawList* draw) {
    using namespace BbPartyLayout;
    // The game's prompt stays on its 16:9 screen (the middle of a wider or taller frame).
    const Rect stage = Stage(game);
    const float sx = stage.w / 1920.0f, sy = stage.h / 1080.0f;
    const float size = PromptGlyphStageSize * sy;
    const int input = panel.vital == BBCOOP_VITAL_DOWN ? InputCircle : InputR1;
    const float right = stage.x + PromptGlyphStageRight * sx;
    ButtonGlyph(draw, input, ImVec2(right - ButtonGlyphWidth(input, size), stage.y + PromptGlyphStageY * sy),
                size, 1.0f);
}

/// Kaniel 2026-10-18: a member's Blood Vials and Quicksilver Bullets, right aligned at `right`
/// and centred on `cy` (`unit` screen pixels to a 1080p pixel): a red vial and its count, a silver
/// bullet and its count.
void SupplyCounts(ImDrawList* draw, float right, float cy, float unit, uint32_t vials,
                  uint32_t bullets, float alpha) {
    ImFont* font = MenuArt::Font();
    const float size = 20.0f * unit;
    const ImU32 ink = IM_COL32(0xC1, 0xC4, 0xC5, int(235 * alpha)), shade = IM_COL32(0, 0, 0, int(170 * alpha));
    const auto count = [&](uint32_t value) {
        char text[12];
        std::snprintf(text, sizeof(text), "%u", value);
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const ImVec2 at(std::floor(right - extent.x), std::floor(cy - extent.y * 0.5f));
        draw->AddText(font, size, ImVec2(at.x + 1.5f * unit, at.y + 1.5f * unit), shade, text);
        draw->AddText(font, size, at, ink, text);
        right -= extent.x + 4.0f * unit;
    };
    count(bullets);
    {
        const float w = 6.0f * unit, h = 13.0f * unit, x0 = right - w, y0 = cy - h * 0.5f;
        draw->AddRectFilled(ImVec2(x0, y0 + w * 0.5f), ImVec2(x0 + w, y0 + h),
                            IM_COL32(150, 154, 160, int(240 * alpha)), 1.0f * unit);
        draw->AddCircleFilled(ImVec2(x0 + w * 0.5f, y0 + w * 0.5f), w * 0.5f,
                              IM_COL32(205, 208, 212, int(250 * alpha)));
        right = x0 - 10.0f * unit;
    }
    count(vials);
    {
        const float r = 5.0f * unit, x = right - r, y = cy + 2.0f * unit;
        draw->AddRectFilled(ImVec2(x - 1.6f * unit, y - r - 6.0f * unit), ImVec2(x + 1.6f * unit, y - r + 1.0f * unit),
                            IM_COL32(170, 170, 176, int(240 * alpha)));
        draw->AddCircleFilled(ImVec2(x, y), r, IM_COL32(150, 24, 22, int(245 * alpha)));
        draw->AddCircle(ImVec2(x, y), r, IM_COL32(205, 208, 212, int(200 * alpha)), 0, 1.0f * unit);
    }
}

/// The game's party list (the party HUD's CoopParty tags, hud_patch.py LIST_X/LIST_Y/LIST_STEP): each
/// member's supplies on its name's line, at its HP bar's end (the tag at (204, 330 + 95 i), its bar
/// from x 109 to about 300 on the 1080p stage).
void Supplies(ImDrawList* draw) {
    using BbPartyLayout::HudX;
    using BbPartyLayout::HudY;
    const float unit = BbPartyLayout::Stage(game).h / 1080.0f;
    for (int i = 0; i < panel.member_count && i < 3; ++i) {
        const float bar_y = 330.0f + 95.0f * float(i) - 0.5f;
        SupplyCounts(draw, HudX(game, 300.0f, shift), HudY(game, bar_y - 20.0f, shift), unit,
                     panel.members[i].vials, panel.members[i].bullets, 1.0f);
    }
}

/// Kaniel 2026-10-25 (the Rot host rule): this player's rot, a small line under the party's
/// list (under the player's own bars when it plays alone): each kind's sum, the game's way.
bbcoop_rot rot{}; // present thread: as Visible() read it last
bool rot_shown = false;

bool ReadRot() {
    rot_shown = bbcoop_rot_view(&rot) != 0;
    return rot_shown;
}

void RotLine(ImDrawList* draw) {
    using Ui::S;
    using Ui::T;
    if (!rot_shown) {
        return;
    }
    int count[6] = {};
    for (int i = 0; i < rot.count; ++i) {
        if (rot.kinds[i] >= 1 && rot.kinds[i] <= 5) {
            ++count[rot.kinds[i]];
        }
    }
    std::string line;
    char part[64];
    std::snprintf(part, sizeof part, T("Rot %d/%d", "Гниль %d/%d"), rot.count, BBCOOP_ROT_MAX);
    line = part;
    const char* names[6] = {"",
                            T("Max HP", "Макс. ОЗ"),
                            T("Max stamina", "Макс. выносл."),
                            T("Stamina use", "Расход выносл."),
                            T("Rally", "Возврат ОЗ"),
                            T("Discovery", "Находки")};
    for (int kind = 1; kind <= 5; ++kind) {
        if (count[kind] == 0) {
            continue;
        }
        if (kind == BBCOOP_ROT_DISCOVERY) {
            std::snprintf(part, sizeof part, "   %s -%d", names[kind], 10 * count[kind]);
        } else {
            const int step = kind == BBCOOP_ROT_RALLY ? 15 : 10;
            std::snprintf(part, sizeof part, "   %s %s%d%%", names[kind],
                          kind == BBCOOP_ROT_STAMINA_USE ? "+" : "-", step * count[kind]);
        }
        line += part;
    }
    float y = BbPartyLayout::ListTop(game, shift);
    if (shown && panel.member_count > 0) {
        y = panel.native_list
                ? HudY(game, 330.0f + 95.0f * float(panel.member_count) - 30.0f, shift)
                : y + float(panel.member_count) * S(64.0f);
    }
    const float x = BbPartyLayout::ListLeft(game, base_scale, shift) + S(8.0f);
    Text(draw, sans_font, S(15.0f), ImVec2(x, y), Rgba(0.86f, 0.52f, 0.46f, 0.95f), line.c_str());
}

/// Kaniel 2026-10-25: a team duel's countdown, and the line a defeated duelist keeps until it ends
/// (the game's own banners say the defeat and the win).
void DuelLine(ImDrawList* draw) {
    using Ui::S;
    using Ui::T;
    bbcoop_duel duel{};
    bbcoop_duel_view(&duel);
    const float mid = game.x + game.w * 0.5f, y = game.y + game.h * 0.22f;
    char line[160];
    if (duel.part == BBCOOP_DUEL_COUNTDOWN) {
        std::snprintf(line, sizeof(line), T("The duel begins in %d", "Дуэль начнётся через %d"),
                      int(std::ceil(std::max(duel.seconds_left, 0.0f))));
        Text(draw, serif_font, S(34.0f), ImVec2(mid, y), Bone(), line, 0.5f);
    } else if (duel.part == BBCOOP_DUEL_OUT) {
        Text(draw, serif_font, S(22.0f), ImVec2(mid, y), Bone(0.85f),
             T("Defeated: the duel goes on without you", "Вы побеждены: дуэль идёт без вас"), 0.5f);
    }
}

void Draw() {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 4.0f);
    RotLine(draw);
    DuelLine(draw);
    if (!panel.native_list) { // else the game's HUD shows it (the party HUD's CoopParty tags)
        MemberList(draw, pulse);
    } else if (!panel.native_supplies) { // else the HUD's own vial and bullet show them
        Supplies(draw);
    }
    if (panel.vital == BBCOOP_VITAL_SPECTATE) {
        OwnSpectate(draw);
    } else if (panel.vital == BBCOOP_VITAL_DOWN) {
        OwnDown(draw, pulse);
    } else if (panel.revive_target >= 0 && panel.revive_target < panel.member_count &&
               !panel.native_prompt) {
        RevivePrompt(draw);
    }
    if (panel.native_prompt) {
        PromptGlyph(draw);
    }
}

} // namespace Party

/// Kaniel 2026-10-25: the free camera flies: low in the middle, how to fly it and how to come back.
namespace FreeCamera {
bool flying = false; // present thread: as Visible() read it last

void Draw(float x, float y, float w, float h) {
    using Ui::S;
    using Ui::T;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float mid = x + w * 0.5f, top = y + h * 0.88f;
    Party::Text(draw, serif_font, S(20.0f), ImVec2(mid, top), Party::Bone(0.9f), T("Free camera", "Свободная камера"),
                0.5f);
    Party::GlyphLine(draw, sans_font, S(14.0f), mid, top + S(30.0f), Party::Bone(0.8f),
                     {{nullptr, Party::InputL3}, {"+"}, {nullptr, Party::InputSquare},
                      {T("or F7: back to your hunter", "или F7: назад к охотнику")}});
}
} // namespace FreeCamera

/// Kaniel 2026-10-24: team pings (defined after the compass, which draws them too).
namespace Pings {
extern bbcoop_pings view;
bool OnCompass();
void OnStrip(ImDrawList* draw, float mid, float half, float y);
void OnRing(ImDrawList* draw, ImVec2 mid, float r);
} // namespace Pings

/// Kaniel 2026-10-17: the teammate compass. A thin strip turned with the camera (ticks every 15
/// degrees of the world, a notch for straight ahead) with each party member on this map as a
/// marker in its colour, its initial and its distance under it; one behind sits at the strip's end
/// with an arrow. In the ring position the markers go round the middle of the screen instead, at
/// their bearing (ahead is up).
namespace Compass {

bbcoop_compass view{}; // present thread: as Visible() read it last
bool shown = false;
BbPartyLayout::Rect game{};

bool Read() {
    shown = BbSettings::Get().compass && bbcoop_compass_view(&view) != 0;
    if (!BbSettings::Get().bloodstains) {
        view.has_stain = 0; // Kaniel 2026-10-20: the Insert menu turned the bloodstains off
    }
    shown = shown && (view.count > 0 || view.has_stain);
    // Kaniel 2026-10-24: the team's pings on this map are on it too, alone if need be.
    if (BbSettings::Get().compass && !shown && Pings::OnCompass()) {
        view = {};
        view.heading = Pings::view.heading;
        shown = true;
    }
    return shown;
}

/// Kaniel 2026-10-17 (after a play test): the compass in the light gray of the game's own counters
/// (the souls at the top right), not gold, and a bit transparent: everything of it is drawn at
/// Opacity. A downed member's marker stays red.
constexpr float Opacity = 0.78f;
ImU32 Gray(float alpha) {
    return Party::Rgba(0.85f, 0.85f, 0.82f, alpha * Opacity);
}
ImU32 Faded(float r, float g, float b, float alpha) {
    return Party::Rgba(r, g, b, alpha * Opacity);
}

/// The first letter of a name (UTF-8: a whole character), "?" for none.
std::string Initial(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return "?";
    }
    if (name[0] >= 'a' && name[0] <= 'z') {
        return std::string(1, char(name[0] - 'a' + 'A')); // a Latin one in capitals
    }
    size_t length = 1;
    const auto lead = static_cast<unsigned char>(name[0]);
    if (lead >= 0xF0) {
        length = 4;
    } else if (lead >= 0xE0) {
        length = 3;
    } else if (lead >= 0xC0) {
        length = 2;
    }
    size_t end = 0;
    while (end < length && name[end] != '\0') {
        ++end;
    }
    return std::string(name, end);
}

/// Kaniel 2026-10-17: the members' portraits on the markers, as the overlay's own textures: one
/// per marker place, made once and filled again only when the member or its picture changes.
struct PortraitTexture {
    ImTextureData* texture = nullptr;
    std::string member;
    uint32_t id = 0;
    bool has = false;
};
PortraitTexture portraits[BBCOOP_COMPASS_MARKERS];

/// The markers' portrait textures brought up to date: before NewFrame, which hands new textures and
/// their uploads to the backend.
/// A slot's texture brought up to date with the member's (board name's) portrait.
void RefreshPortrait(PortraitTexture& slot, const char* board) {
    uint32_t id = 0;
    bbcoop_member_portrait(board, nullptr, 0, &id); // the id alone: no pixels asked for
    if (slot.member == board && slot.id == id) {
        return;
    }
    slot.member = board;
    slot.id = id;
    slot.has = false;
    static std::vector<uint8_t> rgba(size_t{BBCOOP_PORTRAIT_SIDE} * BBCOOP_PORTRAIT_SIDE * 4);
    if (id != 0 && bbcoop_member_portrait(board, rgba.data(), rgba.size(), &id)) {
        if (slot.texture == nullptr) {
            slot.texture = IM_NEW(ImTextureData)();
            slot.texture->Create(ImTextureFormat_RGBA32, BBCOOP_PORTRAIT_SIDE, BBCOOP_PORTRAIT_SIDE);
            ImGui::RegisterUserTexture(slot.texture);
        }
        std::memcpy(slot.texture->GetPixels(), rgba.data(), rgba.size());
        if (slot.texture->Status == ImTextureStatus_OK || slot.texture->Status == ImTextureStatus_WantUpdates) {
            ImTextureDataQueueUpload(slot.texture, 0, 0, BBCOOP_PORTRAIT_SIDE, BBCOOP_PORTRAIT_SIDE);
        }
        slot.has = true;
    }
}

void RefreshPortraits() {
    for (int place = 0; place < view.count; ++place) {
        RefreshPortrait(portraits[place], view.markers[place].board);
    }
}

/// The marker's portrait texture, or null when the member has none (its initial is drawn then).
ImTextureData* Portrait(int place, const bbcoop_compass_marker& marker) {
    const PortraitTexture& slot = portraits[place];
    return slot.has && slot.member == marker.board ? slot.texture : nullptr;
}

/// A member's marker centred on `at`: its portrait (or its initial on a dark disc) ringed in its
/// colour, its distance under it; `arrow` -1 or 1 puts a chevron on that side (a member past the
/// strip's end). Down: larger, the party list's downed badge with its red revive share pulsing
/// round it and the revive's percentage with the distance. Dead: the party list's headstone.
void Marker(ImDrawList* draw, ImVec2 at, int place, const bbcoop_compass_marker& marker, int arrow) {
    using Ui::S;
    const bool down = marker.vital == BBCOOP_VITAL_DOWN, dead = marker.vital == BBCOOP_VITAL_DEAD;
    const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 4.0f);
    const float r = down ? S(17.0f) : S(13.0f);
    const ImU32 color = down ? Party::Blood((0.75f + 0.25f * pulse) * Opacity) : Gray(dead ? 0.5f : 1.0f);
    draw->AddCircleFilled(ImVec2(at.x + S(1.0f), at.y + S(1.5f)), r + S(1.5f), Faded(0, 0, 0, 0.55f), 32);
    using BbPartyIcons::Icon;
    if (down && Party::Icon(draw, Icon::Downed, at, r * 2.6f, Opacity)) {
        // The badge's own ring fills red with the revive, as on the party list.
        Party::RingShare(draw, at, r * 2.6f, BbPartyIcons::DownedRing, marker.revive,
                         (0.8f + 0.2f * pulse) * Opacity);
    } else if (dead && Party::Icon(draw, Icon::Fallen, at, r * 2.6f, 0.6f * Opacity)) {
    } else {
        draw->AddCircleFilled(at, r, Faded(0.06f, 0.05f, 0.05f, 0.88f), 32);
        if (ImTextureData* face = dead ? nullptr : Portrait(place, marker)) {
            const float f = r - S(1.0f);
            draw->AddImage(face->GetTexRef(), ImVec2(at.x - f, at.y - f), ImVec2(at.x + f, at.y + f), ImVec2(0, 0),
                          ImVec2(1, 1), Faded(1, 1, 1, 1));
        } else {
            const std::string initial = Initial(marker.name[0] != '\0' ? marker.name : Ui::T("Hunter", "Охотник"));
            const float size = S(17.0f);
            const ImVec2 extent = serif_font->CalcTextSizeA(size, FLT_MAX, 0.0f, initial.c_str());
            Party::Text(draw, serif_font, size, ImVec2(at.x, at.y - extent.y * 0.5f),
                        Gray(dead ? 0.5f : 1.0f), initial.c_str(), 0.5f);
        }
        draw->AddCircle(at, r, color, 32, S(2.0f));
        if (down) {
            // No badge art: the revive's share as a red arc round the ring.
            const float top = -IM_PI * 0.5f;
            draw->PathArcTo(at, r + S(3.0f), top, top + 2.0f * IM_PI * std::clamp(marker.revive, 0.0f, 1.0f), 48);
            draw->PathStroke(Party::Blood(0.9f * Opacity), 0, S(3.0f));
        }
    }
    if (arrow != 0) {
        const float x = at.x + arrow * (r + S(5.0f)), h = S(6.0f);
        draw->AddTriangleFilled(ImVec2(x, at.y - h), ImVec2(x + arrow * h, at.y), ImVec2(x, at.y + h), color);
    }
    char line[40];
    if (down) {
        std::snprintf(line, sizeof(line), marker.distance < 10.0f ? "%d%%  %.1f m" : "%d%%  %.0f m",
                      int(std::lround(std::clamp(marker.revive, 0.0f, 1.0f) * 100.0f)), marker.distance);
    } else {
        std::snprintf(line, sizeof(line), marker.distance < 10.0f ? "%.1f m" : "%.0f m", marker.distance);
    }
    Party::Text(draw, sans_font, S(13.0f), ImVec2(at.x, at.y + r + S(3.0f)),
                down ? Faded(1.0f, 0.52f, 0.46f, 1.0f) : Gray(dead ? 0.6f : 0.95f), line, 0.5f);
}

/// Kaniel 2026-10-20: a bloodstain as a drop of blood, in the compass's gray on a soft shadow (the
/// game has no icon of its own for one): its point up, round below, centred on `at`, `r` high.
void Drop(ImDrawList* draw, ImVec2 at, float r, ImU32 color, ImU32 shadow) {
    const ImVec2 belly(at.x, at.y + r * 0.28f);
    const float round = r * 0.62f;
    draw->AddCircleFilled(ImVec2(at.x + Ui::S(1.0f), at.y + Ui::S(1.5f)), r + Ui::S(2.0f), shadow, 24);
    draw->AddCircleFilled(belly, round, color, 24);
    draw->AddTriangleFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + round * 0.93f, belly.y - round * 0.37f),
                            ImVec2(at.x - round * 0.93f, belly.y - round * 0.37f), color);
}

/// This player's bloodstain on the compass: the drop and its distance under it; `arrow` as a member
/// marker's.
void StainMarker(ImDrawList* draw, ImVec2 at, const bbcoop_compass_stain& stain, int arrow) {
    using Ui::S;
    const float r = S(11.0f);
    Drop(draw, at, r, Gray(0.95f), Faded(0, 0, 0, 0.45f));
    if (arrow != 0) {
        const float x = at.x + arrow * (r + S(4.0f)), h = S(5.0f);
        draw->AddTriangleFilled(ImVec2(x, at.y - h), ImVec2(x + arrow * h, at.y), ImVec2(x, at.y + h), Gray(0.9f));
    }
    char line[48];
    std::snprintf(line, sizeof(line), stain.distance < 10.0f ? "%.1f m" : "%.0f m", stain.distance);
    Party::Text(draw, sans_font, S(12.0f), ImVec2(at.x, at.y + r + S(3.0f)), Gray(0.85f), line, 0.5f);
}

void Strip(ImDrawList* draw, float y) {
    using Ui::S;
    constexpr float Pi = 3.14159265f;
    const float mid = game.x + game.w * 0.5f, half = S(280.0f);
    // The shade behind it, fading out to both ends; no frame.
    const ImU32 shade = Faded(0.02f, 0.02f, 0.02f, 0.50f), clear = Party::Rgba(0, 0, 0, 0);
    draw->AddRectFilledMultiColor(ImVec2(mid - half, y - S(10.0f)), ImVec2(mid, y + S(10.0f)), clear, shade,
                                  shade, clear);
    draw->AddRectFilledMultiColor(ImVec2(mid, y - S(10.0f)), ImVec2(mid + half, y + S(10.0f)), shade, clear,
                                  clear, shade);
    // The line, fading out to both ends, as the game's dialog borders.
    const int pieces = 24;
    for (int i = 0; i < pieces; ++i) {
        const float t0 = float(i) / pieces, t1 = float(i + 1) / pieces;
        const float a = 0.8f * std::sin(Pi * (t0 + t1) * 0.5f);
        draw->AddLine(ImVec2(mid - half + 2 * half * t0, y), ImVec2(mid - half + 2 * half * t1, y), Gray(a),
                      S(1.0f));
    }
    // Ticks at every 15 degrees of the world's headings, turning with the camera; every 45 longer.
    const float step = Pi / 12.0f;
    for (int k = -24; k <= 24; ++k) {
        const float world = std::round(view.heading / step) * step + k * step;
        float bearing = std::remainder(world - view.heading, 2 * Pi);
        if (std::abs(bearing) > Pi / 2) {
            continue;
        }
        const float x = mid + half * bearing / (Pi / 2);
        const bool major = std::lround(world / step) % 3 == 0;
        const float a = 0.75f * std::cos(bearing);
        const float h = major ? S(6.0f) : S(3.5f);
        draw->AddLine(ImVec2(x, y - h), ImVec2(x, y + h), Gray(a), S(major ? 1.5f : 1.0f));
    }
    // Straight ahead: a small notch above the line.
    draw->AddTriangleFilled(ImVec2(mid - S(5.0f), y - S(14.0f)), ImVec2(mid + S(5.0f), y - S(14.0f)),
                            ImVec2(mid, y - S(7.0f)), Gray(0.9f));
    // The bloodstain first: the members' markers go over it.
    if (view.has_stain) {
        const bbcoop_compass_stain& stain = view.stain;
        StainMarker(draw, ImVec2(mid + half * stain.x, y), stain, stain.pinned ? (stain.x < 0.0f ? -1 : 1) : 0);
    }
    Pings::OnStrip(draw, mid, half, y);
    for (int i = 0; i < view.count; ++i) {
        const bbcoop_compass_marker& marker = view.markers[i];
        const int arrow = marker.pinned ? (marker.x < 0.0f ? -1 : 1) : 0;
        Marker(draw, ImVec2(mid + half * marker.x, y), i, marker, arrow);
    }
}

void Ring(ImDrawList* draw) {
    using Ui::S;
    const ImVec2 mid(game.x + game.w * 0.5f, game.y + game.h * 0.5f);
    const float r = S(150.0f);
    draw->AddCircle(mid, r, Gray(0.18f), 96, S(1.0f));
    if (view.has_stain) {
        const bbcoop_compass_stain& stain = view.stain;
        StainMarker(draw, ImVec2(mid.x + r * std::sin(stain.bearing), mid.y - r * std::cos(stain.bearing)), stain, 0);
    }
    Pings::OnRing(draw, mid, r);
    for (int i = 0; i < view.count; ++i) {
        const bbcoop_compass_marker& marker = view.markers[i];
        const ImVec2 at(mid.x + r * std::sin(marker.bearing), mid.y - r * std::cos(marker.bearing));
        Marker(draw, at, i, marker, 0);
    }
}

void Draw() {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    using Ui::S;
    switch (BbSettings::Get().compass_position.load()) {
    case BbSettings::CompassBottom:
        Strip(draw, game.y + game.h - S(150.0f));
        break;
    case BbSettings::CompassReticle:
        Ring(draw);
        break;
    default:
        Strip(draw, game.y + S(52.0f));
        break;
    }
}

} // namespace Compass

/// Kaniel 2026-10-24: team pings. Each member's ping (this player's own too) as a small mark in the
/// compass's light gray, a ring round a dot (the game has no icon of its own for one), with the
/// sender's character name and how far it is: over its spot in the world while it is in the
/// picture, at the picture's edge with a chevron its way while it is not; on the compass (this
/// map's); on the map (every map's, the area named when it is not this player's). It fades out over
/// its last moment.
namespace Pings {

bbcoop_pings view{}; // present thread: as Visible() read it last
bool shown = false;
BbPartyLayout::Rect game{};

bool Read() {
    const bool on = BbSettings::Get().team_pings;
    bbcoop_set_team_pings(on ? 1 : 0);
    shown = on && bbcoop_pings_view(&view) != 0;
    if (!shown) {
        view = {};
    }
    return shown;
}

bool OnCompass() {
    if (!shown || !view.has_heading) {
        return false;
    }
    for (int i = 0; i < view.count; ++i) {
        if (view.pings[i].here) {
            return true;
        }
    }
    return false;
}

const char* NameOf(const bbcoop_ping& ping) {
    return ping.name[0] != '\0' ? ping.name : Ui::T("Hunter", "Охотник"); // never an online id
}

float Alpha(const bbcoop_ping& ping) {
    return std::clamp(ping.seconds_left / 1.5f, 0.0f, 1.0f);
}

/// Kaniel 2026-10-24 (after the first pictures: "Bolder + dark outline"): everything of a ping
/// has a soft dark edge, so it reads on a bright scene too.
ImU32 Edge(float alpha) {
    return Compass::Faded(0.02f, 0.02f, 0.02f, 0.9f * alpha);
}

/// The mark centred on `at`, `r` its ring's radius: a 3 px ring (at 1080p) round a dot.
void Mark(ImDrawList* draw, ImVec2 at, float r, float alpha) {
    using Ui::S;
    draw->AddCircle(at, r, Edge(alpha), 32, S(6.0f));
    draw->AddCircleFilled(at, r * 0.32f + S(1.5f), Edge(alpha), 16);
    draw->AddCircle(at, r, Compass::Gray(alpha), 32, S(3.0f));
    draw->AddCircleFilled(at, r * 0.32f, Compass::Gray(alpha), 16);
}

/// A chevron beside a mark of radius `r`, pointing `angle` (radians on the screen, 0 right).
void Chevron(ImDrawList* draw, ImVec2 at, float r, float angle, float alpha) {
    using Ui::S;
    const ImVec2 dir(std::cos(angle), std::sin(angle)), side(-dir.y, dir.x);
    const float h = S(7.0f);
    const ImVec2 base(at.x + dir.x * (r + S(5.0f)), at.y + dir.y * (r + S(5.0f)));
    const ImVec2 a(base.x + side.x * h, base.y + side.y * h), b(base.x + dir.x * h, base.y + dir.y * h),
        c(base.x - side.x * h, base.y - side.y * h);
    draw->AddTriangle(a, b, c, Edge(alpha), S(3.0f));
    draw->AddTriangleFilled(a, b, c, Compass::Gray(0.95f * alpha));
}

/// A ping's words in the gray with a dark edge all round; `align` as Party::Text's.
void Words(ImDrawList* draw, float size, ImVec2 at, float alpha, const char* text, float align) {
    const ImVec2 extent = sans_font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    at.x = std::floor(at.x - extent.x * align);
    const float d = std::max(1.0f, size * 0.1f);
    for (const ImVec2 off : {ImVec2(-d, 0), ImVec2(d, 0), ImVec2(0, -d), ImVec2(0, d), ImVec2(-d, -d), ImVec2(d, -d),
                             ImVec2(-d, d), ImVec2(d, d)}) {
        draw->AddText(sans_font, size, ImVec2(at.x + off.x, at.y + off.y), Edge(alpha), text);
    }
    draw->AddText(sans_font, size, at, Compass::Gray(0.97f * alpha), text);
}

/// Its words: the sender's name and how far it is (`distance`), else the area it is in (may be "").
std::string Label(const bbcoop_ping& ping, bool distance, const std::string& area) {
    char line[128];
    bbcoop_ping_label(NameOf(ping), distance ? 1 : 0, ping.distance, area.c_str(), line, sizeof(line));
    return line;
}

/// In the world: this map's pings over their spots, or at the picture's edge.
void Draw() {
    using Ui::S;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < view.count; ++i) {
        const bbcoop_ping& ping = view.pings[i];
        if (!ping.here || !ping.on_screen) {
            continue;
        }
        const float alpha = Alpha(ping), r = S(10.0f);
        ImVec2 at(game.x + ping.screen_x * game.w, game.y + ping.screen_y * game.h);
        // Pinned at the top, it stays under the compass and the counters there (the second
        // pictures: it sat on the compass's strip).
        if (ping.pinned) {
            at.y = std::max(at.y, game.y + S(115.0f));
        }
        Mark(draw, at, r, alpha);
        if (ping.pinned) {
            Chevron(draw, at, r, ping.arrow, alpha);
        }
        // Its words over it; under it while its arrow points up (the arrow takes the place over it).
        const std::string line = Label(ping, true, "");
        const bool under = ping.pinned && std::sin(ping.arrow) < -0.3f;
        Words(draw, S(17.0f), ImVec2(at.x, under ? at.y + r + S(5.0f) : at.y - r - S(26.0f)), alpha, line.c_str(), 0.5f);
    }
}

/// On the compass: the mark, its words over the strip (the members' go under it).
void OnCompassAt(ImDrawList* draw, ImVec2 at, const bbcoop_ping& ping, int arrow) {
    using Ui::S;
    const float alpha = Alpha(ping), r = S(8.0f);
    Mark(draw, at, r, alpha);
    if (arrow != 0) {
        Chevron(draw, at, r, arrow < 0 ? 3.14159265f : 0.0f, alpha);
    }
    const std::string line = Label(ping, true, "");
    Words(draw, S(15.0f), ImVec2(at.x, at.y - r - S(23.0f)), alpha, line.c_str(), 0.5f);
}

void OnStrip(ImDrawList* draw, float mid, float half, float y) {
    for (int i = 0; shown && i < view.count; ++i) {
        const bbcoop_ping& ping = view.pings[i];
        if (ping.here) {
            OnCompassAt(draw, ImVec2(mid + half * ping.strip_x, y), ping,
                        ping.strip_pinned ? (ping.strip_x < 0.0f ? -1 : 1) : 0);
        }
    }
}

void OnRing(ImDrawList* draw, ImVec2 mid, float r) {
    for (int i = 0; shown && i < view.count; ++i) {
        const bbcoop_ping& ping = view.pings[i];
        if (ping.here) {
            OnCompassAt(draw, ImVec2(mid.x + r * std::sin(ping.bearing), mid.y - r * std::cos(ping.bearing)), ping, 0);
        }
    }
}

} // namespace Pings

/// Kaniel 2026-10-17: a cutscene every party member on the map sees is skipped once all of them
/// voted (Options, CutsceneVote): how many want to, low in the picture while someone does.
namespace SkipVote {

int want = 0, watching = 0; // present thread: as Visible() read it last
bool shown = false;
BbPartyLayout::Rect game{};

bool Read() {
    shown = bbcoop_cutscene_vote(&want, &watching) != 0;
    return shown;
}

void Draw() {
    using Ui::S;
    using Ui::T;
    char line[96];
    std::snprintf(line, sizeof line, T("%d of %d want to skip", "%d из %d хотят пропустить"), want,
                  watching);
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const BbPartyLayout::Rect stage = BbPartyLayout::Stage(game); // the game's 16:9 screen
    Party::Text(draw, sans_font, S(18.0f), ImVec2(stage.x + stage.w * 0.5f, stage.y + stage.h * 0.86f),
                Ui::Plain(0.9f), line, 0.5f);
    // Kaniel 2026-10-19: which button votes, as the game's icon.
    const Ui::Keys::Hint keys[] = {{"Options", nullptr, T("Skip", "Пропустить")}};
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    Ui::Keys::Guide(draw, keys, stage.x, stage.x + stage.w, stage.y + stage.h * 0.86f + S(62.0f));
    ImGui::PopFont();
}

} // namespace SkipVote

/// Invasions (docs/INVASIONS.md): "An invader approaches" with its countdown to the invaded team,
/// the search and the trip to the invader, and how it ended, high in the picture in the party's
/// blood red.
namespace Invasion {

bbcoop_invasion view{}; // present thread: as Visible() read it last
bool shown = false;
BbPartyLayout::Rect game{};

bool Read() {
    shown = bbcoop_invasion_view(&view) != 0;
    return shown;
}

/// How it ended, as the side that reads it sees it.
const char* EndLine() {
    using Ui::T;
    switch (view.reason) {
    case BBCOOP_INVASION_INVADER_DIED:
        return view.role == BBCOOP_INVASION_TARGET ? T("Invader defeated", "Захватчик повержен")
                                                   : T("You were defeated", "Вы повержены");
    case BBCOOP_INVASION_TARGET_KILLED:
    case BBCOOP_INVASION_PARTY_WIPED:
        return view.role == BBCOOP_INVASION_TARGET ? T("The invader prevailed", "Захватчик победил")
                                                   : T("Your prey has fallen", "Ваша добыча пала");
    case BBCOOP_INVASION_NOT_FOUND:
        return T("No world to invade was found", "Мир для вторжения не найден");
    case BBCOOP_INVASION_REFUSED:
        return T("The server refused the invasion", "Сервер отказал во вторжении");
    default:
        return T("The invasion is over", "Вторжение окончено");
    }
}

void Draw() {
    using Ui::S;
    using Ui::T;
    char line[160];
    const bool target = view.role == BBCOOP_INVASION_TARGET;
    switch (view.phase) {
    case BBCOOP_INVASION_SEARCHING:
        std::snprintf(line, sizeof line, "%s", T("Looking for a world to invade", "Поиск мира для вторжения"));
        break;
    case BBCOOP_INVASION_APPROACHING:
        std::snprintf(line, sizeof line,
                      target ? T("An invader approaches: %d", "Приближается захватчик: %d")
                             : T("Invading in %d", "Вторжение через %d"),
                      view.seconds);
        break;
    case BBCOOP_INVASION_ACTIVE:
        std::snprintf(line, sizeof line,
                      target ? T("Invaded by %s", "Вторжение: %s") : T("Invading the world of %s", "Вторжение в мир: %s"),
                      view.name);
        break;
    case BBCOOP_INVASION_RETURNING:
        std::snprintf(line, sizeof line, "%s", T("Returning home", "Возвращение домой"));
        break;
    default:
        std::snprintf(line, sizeof line, "%s", EndLine());
        break;
    }
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    Party::Text(draw, sans_font, S(24.0f), ImVec2(game.x + game.w * 0.5f, game.y + game.h * 0.16f),
                Party::Blood(0.95f), line, 0.5f);
}

} // namespace Invasion

/// Kaniel 2026-10-17: the map screen (an Elden Ring style map). The area's picture
/// (BbMapCharts, drawn from the game's map files) on the menus' wall, framed in the game's card
/// art: this player as an arrow turned with the camera, the party members on this map at their
/// spots (their portraits ringed in the compass's light gray, not gold), the lamps lit as small
/// lamps; beside it the area's lit lamps and where each member is. L1 / R1 go through the areas
/// with a lit lamp, Cross on a lamp asks to travel there (BBCOOP_MAP_* says when it can't).
/// Opened by M, the Insert menu's row or the System menu's "Map"; Circle, Escape or M closes it.
namespace Map {
using namespace Ui;

bbcoop_map view{};
bool open = false;
bool was_open = false;
int shut_reads = 0; // frames while shut: the view is read every 120th

struct State {
    uint32_t shown = 0;      // the area on screen
    float cx = 0, cz = 0;    // the world spot in the middle of the picture
    float zoom = 0;          // screen pixels a metre (0: fit the area on first sight)
    int focus = 0;           // the focused lamp row
    int32_t asking = 0;      // the lamp a travel question is open for
    bool ask_yes = true;
    std::string notice;      // why a trip did not start
    double notice_until = 0;
    bool fresh = true;       // just opened: the player's area, centred on the player
    bool placed = false;     // the first reading since it opened came (or a moment passed)
    double opened = 0;
    // Kaniel 2026-10-18 (as Elden Ring's): the cursor over the picture, moved by the left stick (or
    // the mouse); the lit lamp under it, if any (its warp point).
    ImVec2 cursor{-1.0f, -1.0f};
    int32_t hover = 0;
    // Kaniel 2026-10-18 ("sometimes roofs, sometimes floors"): 0 by itself (floors while a roof
    // is over the player), 1 roofs, 2 floors (R3 or F switches).
    int floors = 0;
    // Kaniel 2026-10-19 ("3d map ... as an optional feature"): the picture as a landscape seen
    // from an angle (L3 or V): turned `yaw` radians from north, tilted `pitch` from the ground
    // (pi/2 straight down), round the height `cy` of the middle.
    bool three_d = false;
    float yaw = 0.0f, pitch = 0.87f, cy = NAN;
    // Kaniel 2026-10-19 ("can we prerender it?", "we need just one side"): the side the 3D view
    // is seen from (BbMapArt::ViewOf): from the south, looking north.
    int side = 1;
};
State state;
bool three_d_kept = false; // the 3D view stays on the next time the map opens
bool sides_ready = false;  // the 3D view's pictures of its side are there (else the flat map shows)
bool live_ready = false;   // the live 3D view has the shown area on the GPU
/// The map is shown angled: the 3D view is on and its pictures (or meshes) are there.
bool Angled() {
    return state.three_d && (sides_ready || live_ready);
}
bool Live() {
    return BbSettings::Get().map_3d == BbSettings::Map3dLive;
}

/// An area's picture as a texture.
struct ChartTexture {
    uint32_t map = 0;
    std::shared_ptr<const BbMapArt::Chart> chart;
    ImTextureData* texture = nullptr;
};
/// Kaniel 2026-10-17 ("one big map"), 2026-10-19 ("every map should be just one picture"): the
/// pictures of every area the screen goes through, drawn so far, on one atlas (AtlasOffset).
std::map<uint32_t, ChartTexture> charts;
/// Kaniel 2026-10-19 ("can we prerender it?"): the 3D view's pictures of those areas from the
/// side it is seen from (and the side it turns from), by `map | side`.
std::map<uint32_t, ChartTexture> sides;
std::vector<ImTextureData*> dying;

/// The shown area's picture once it is there; the areas drawn with it (WorldAreas, below).
const ChartTexture* Shown();
std::vector<uint32_t> WorldAreas();

/// The areas of one world share the game's coordinates (their MSBs place them where they stand in
/// it), so the map draws them together: the waking world (Hemwick, Old Yharnam, the Cathedral Ward
/// blocks, Cainhurst, the Forbidden Woods, Yahar'gul, Byrgenwerth), the Nightmare of Mensis, the
/// Nightmare Frontier with the Hunter's Nightmare; the Dream and anything else alone.
int WorldOf(uint32_t map) {
    switch (map >> 24) {
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1b:
    case 0x1c:
    case 0x20:
        return 1;
    case 0x1a:
        return 2;
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
        return 3;
    default:
        return 100 + int(map >> 16);
    }
}

/// Kaniel 2026-10-19 ("every map should be just one picture"): where each world lies on the one
/// picture, in metres added to its own coordinates. The worlds share no coordinates with each
/// other (each is a dream of its own), so they are laid side by side with 150 m between them:
/// the waking world (x -575 to 495, z -492 to 782) in the middle, the Nightmare Frontier and the
/// Hunter's Nightmare (x -770 to 221, z -997 to 47) east of it, the Nightmare of Mensis (x -215
/// to 223, z -122 to 176) north of those, the Hunter's Dream and the Abandoned Old Workshop west.
struct Place {
    float dx = 0.0f, dz = 0.0f;
};
Place AtlasOffset(uint32_t map) {
    switch (WorldOf(map)) {
    case 1:
        return {};
    case 3:
        return {1414.0f, 620.0f};
    case 2:
        return {859.0f, 939.0f};
    default:
        break;
    }
    switch (map >> 16) {
    case 0x1500: // the Hunter's Dream
        return {-793.0f, 145.0f};
    case 0x1501: // the Abandoned Old Workshop
        return {-891.0f, -165.0f};
    default: // anything else (a chalice's): south-west of the waking world
        return {-900.0f, -800.0f};
    }
}

Compass::PortraitTexture portraits[BBCOOP_MAP_MEMBERS];

ImU32 Gray(float alpha = 1.0f) {
    return Party::Rgba(0.85f, 0.85f, 0.82f, alpha);
}
ImU32 LampLight(float alpha = 1.0f) {
    return Party::Rgba(1.0f, 0.80f, 0.48f, alpha);
}

/// Kaniel 2026-10-18 (fog of war, as Elden Ring's): only what this character has walked near (40 m
/// round its walked cells), round its lit lamps and where it stands shows. Kaniel 2026-10-18 ("only
/// dark fog, not even hidden area of map"): the rest is see-through, so only the screen's dark
/// backing shows there, not even the area's outline. An area whose main bosses are down shows
/// whole. The picture's pixels into `out`.
bool rebuild = false; // the map opened: the pictures are made again with the fog as it is now
/// The height the pictures show the floors at (BbMapArt::FloorView: the player's feet), NaN while
/// they show the roofs.
float drawn_floors = NAN;
void Fogged(const BbMapArt::Chart& chart, uint32_t map, std::uint8_t* out) {
    std::memcpy(out, chart.rgba.data(), chart.rgba.size());
    if (!std::isnan(drawn_floors)) {
        BbMapArt::FloorView(chart, drawn_floors, out);
    }
    if (!BbSettings::Get().map_fog) {
        return; // Kaniel 2026-10-19: the fog turned off in the settings, the whole map shows
    }
    for (int i = 0; i < view.cleared_count; ++i) {
        if (view.cleared[i] == (map & 0xFFFF0000u)) {
            return;
        }
    }
    // The reveal on a coarse grid (2 m), softened, then sampled for each pixel.
    constexpr float Grid = 2.0f, Reveal = 40.0f;
    const float w_m = float(chart.width) / chart.ppm, h_m = float(chart.height) / chart.ppm;
    const int gw = int(w_m / Grid) + 2, gh = int(h_m / Grid) + 2;
    std::vector<float> seen(std::size_t(gw) * std::size_t(gh), 0.0f);
    const auto disc = [&](float x, float z, float radius) {
        const float gx = (x - chart.x0) / Grid, gz = (chart.z1 - z) / Grid, r = radius / Grid;
        for (int y = std::max(0, int(gz - r - 1)); y <= std::min(gh - 1, int(gz + r + 1)); ++y) {
            for (int xx = std::max(0, int(gx - r - 1)); xx <= std::min(gw - 1, int(gx + r + 1)); ++xx) {
                const float d = std::sqrt((float(xx) - gx) * (float(xx) - gx) + (float(y) - gz) * (float(y) - gz));
                const float v = std::clamp((r - d) / (r * 0.35f), 0.0f, 1.0f); // soft rim
                float& cell = seen[std::size_t(y) * std::size_t(gw) + std::size_t(xx)];
                cell = std::max(cell, v);
            }
        }
    };
    static std::vector<int16_t> cells(2 * 65536);
    const int count = bbcoop_map_explored(map, cells.data(), 65536);
    for (int i = 0; i < count; ++i) {
        disc((float(cells[size_t(i) * 2]) + 0.5f) * BBCOOP_MAP_CELL, (float(cells[size_t(i) * 2 + 1]) + 0.5f) * BBCOOP_MAP_CELL,
             Reveal);
    }
    for (int i = 0; i < view.lamp_count; ++i) {
        if (view.lamps[i].lit && view.lamps[i].map == (map & 0xFFFF0000u)) {
            disc(view.lamps[i].x, view.lamps[i].z, 60.0f);
        }
    }
    if (view.has_me && view.map == map) {
        disc(view.x, view.z, Reveal);
    }
    for (int y = 0; y < chart.height; ++y) {
        const float gz = (float(y) / chart.ppm) / Grid;
        for (int x = 0; x < chart.width; ++x) {
            const float gx = (float(x) / chart.ppm) / Grid;
            const int ix = std::min(gw - 2, int(gx)), iz = std::min(gh - 2, int(gz));
            const float fx = gx - float(ix), fz = gz - float(iz);
            const auto at = [&](int a, int b) { return seen[std::size_t(b) * std::size_t(gw) + std::size_t(a)]; };
            const float v = (at(ix, iz) * (1 - fx) + at(ix + 1, iz) * fx) * (1 - fz) +
                            (at(ix, iz + 1) * (1 - fx) + at(ix + 1, iz + 1) * fx) * fz;
            std::uint8_t* px = out + (std::size_t(y) * std::size_t(chart.width) + std::size_t(x)) * 4;
            px[3] = std::uint8_t(float(px[3]) * v);
        }
    }
}

/// Textures for the frame ahead: the shown area's picture once it is drawn, the members'
/// portraits. Before NewFrame, which hands new textures to the backend. (While the screen is shut,
/// Visible asks for the picture of the player's own area, so it is ready when it opens.)
void Refresh() {
    for (auto it = dying.begin(); it != dying.end();) {
        if ((*it)->Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(*it);
            IM_DELETE(*it);
            it = dying.erase(it);
        } else {
            ++it;
        }
    }
    if (!open) {
        return;
    }
    // Kaniel 2026-10-18 ("sometimes roofs, sometimes floors"): floors where the player stands
    // under a roof (indoors, under a bridge), roofs in the open; R3 or F picks the other.
    float floors = NAN;
    if (view.has_me) {
        bool want = state.floors == 2;
        if (state.floors == 0) {
            const auto mine = BbMapCharts::Get(view.map);
            want = mine && BbMapArt::UnderRoof(*mine, view.x, view.y, view.z);
        }
        floors = want ? view.y : NAN;
    }
    const bool same_floors = std::isnan(floors) ? std::isnan(drawn_floors)
                                                : !std::isnan(drawn_floors) && std::abs(floors - drawn_floors) < 1.0f;
    static bool drawn_fog = true; // the fog setting the pictures were made with
    if (drawn_fog != BbSettings::Get().map_fog) {
        drawn_fog = BbSettings::Get().map_fog;
        rebuild = true;
    }
    if ((rebuild || !same_floors) && view.lamp_count > 0) { // once the first reading since opening is in
        if (!same_floors) {
            drawn_floors = floors;
            if (std::isnan(floors)) {
                std::printf("Map: shows the roofs\n");
            } else {
                std::printf("Map: shows the floors at %.1f m\n", floors);
            }
        }
        rebuild = false;
        for (auto& [map, c] : charts) {
            if (c.texture != nullptr) {
                c.texture->WantDestroyNextFrame = true;
                dying.push_back(c.texture);
            }
        }
        charts.clear();
    }
    for (int i = 0; i < view.member_count; ++i) {
        Compass::RefreshPortrait(portraits[i], view.members[i].board);
    }
    // Every area the screen goes through, as they are drawn (the shown area's asked for first).
    const auto world = WorldAreas();
    for (auto it = charts.begin(); it != charts.end();) {
        if (std::find(world.begin(), world.end(), it->first) == world.end()) {
            if (it->second.texture != nullptr) {
                it->second.texture->WantDestroyNextFrame = true;
                dying.push_back(it->second.texture);
            }
            it = charts.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<uint32_t> order{state.shown};
    for (const uint32_t map : world) {
        if (map != state.shown) {
            order.push_back(map);
        }
    }
    for (const uint32_t map : order) {
        if (map == 0 || charts.contains(map)) {
            continue;
        }
        auto chart = BbMapCharts::Get(map);
        if (!chart) {
            continue;
        }
        ChartTexture& slot = charts[map];
        slot = {map, chart, IM_NEW(ImTextureData)()};
        slot.texture->Create(ImTextureFormat_RGBA32, chart->width, chart->height);
        Fogged(*chart, map, static_cast<std::uint8_t*>(slot.texture->GetPixels()));
        ImGui::RegisterUserTexture(slot.texture);
    }
    // The 3D view's pictures: of the areas drawn above, from its side and the one it turns from;
    // the others go.
    for (auto it = sides.begin(); it != sides.end();) {
        const int side = int(it->first & 7u);
        const bool wanted = state.three_d && charts.contains(it->first & ~7u) && side == state.side;
        if (!wanted) {
            if (it->second.texture != nullptr) {
                it->second.texture->WantDestroyNextFrame = true;
                dying.push_back(it->second.texture);
            }
            it = sides.erase(it);
        } else {
            ++it;
        }
    }
    if (!BbSettings::Get().map_3d) {
        state.three_d = false;
    }
    const bool live = BbSettings::Get().map_3d == BbSettings::Map3dLive;
    // Kaniel 2026-10-19 ("an opt in option to view rendered in real time"): the live view's meshes
    // of the areas on screen, made ahead and put on the GPU as they are ready; gone when it is off.
    if (live) {
        for (const uint32_t map : order) {
            if (map != 0 && charts.contains(map) && !BbMap3d::Has(map)) {
                if (const auto mesh = BbMapCharts::GetMesh(map)) {
                    const Place p = AtlasOffset(map);
                    BbMap3d::Upload(map, mesh, charts[map].chart, p.dx, p.dz);
                }
            }
        }
    } else {
        BbMap3d::Clear();
    }
    // Kaniel 2026-10-19 ("now it shows this state with boxes"): with the 3D map on, the areas on
    // screen are drawn for it ahead, while the flat map is up.
    if (BbSettings::Get().map_3d == BbSettings::Map3dPrerendered) {
        for (const uint32_t map : order) {
            if (map != 0 && charts.contains(map)) {
                BbMapCharts::Get(map | uint32_t(state.side));
            }
        }
    }
    if (state.three_d && !live) {
        for (const uint32_t map : order) {
            {
                const uint32_t key = map | uint32_t(state.side);
                if (map == 0 || !charts.contains(map) || sides.contains(key)) {
                    continue;
                }
                auto chart = BbMapCharts::Get(key);
                if (!chart) {
                    continue;
                }
                ChartTexture& slot = sides[key];
                slot = {map, chart, IM_NEW(ImTextureData)()};
                slot.texture->Create(ImTextureFormat_RGBA32, chart->width, chart->height);
                std::memcpy(slot.texture->GetPixels(), chart->rgba.data(), chart->rgba.size());
                ImGui::RegisterUserTexture(slot.texture);
            }
        }
    }
    sides_ready = false;
    for (const auto& [key, side_chart] : sides) {
        sides_ready = sides_ready || (int(key & 7u) == state.side && side_chart.texture != nullptr);
    }
    // The live view is ready once the area shown is on the GPU.
    live_ready = live && BbMap3d::Has(state.shown);
}

const ChartTexture* Shown() {
    const auto found = charts.find(state.shown);
    return found != charts.end() && found->second.texture != nullptr ? &found->second : nullptr;
}

std::string AreaName(uint32_t map) {
    std::string name = BbMapCharts::AreaName(bbcoop_map_area_name_id(map));
    if (name.empty()) {
        char fallback[24];
        std::snprintf(fallback, sizeof(fallback), "m%02u_%02u", (map >> 24) & 0xFF, (map >> 16) & 0xFF);
        name = fallback;
    }
    return name;
}

/// The areas the screen goes through: each with a lit lamp, and the player's own. Kaniel 2026-10-19
/// ("I lifted fog of war. why don't I see the whole map? only yaharnam"): with the fog off, every
/// area with a lamp, lit or not.
std::vector<uint32_t> Areas() {
    std::vector<uint32_t> areas;
    const auto add = [&](uint32_t map) {
        if (map != 0 && std::find(areas.begin(), areas.end(), map) == areas.end()) {
            areas.push_back(map);
        }
    };
    add(view.map);
    const bool fog = BbSettings::Get().map_fog;
    for (int i = 0; i < view.lamp_count; ++i) {
        if (view.lamps[i].lit || !fog) {
            add(view.lamps[i].map);
        }
    }
    std::sort(areas.begin(), areas.end());
    return areas;
}

/// The areas drawn with the shown one: all of Areas(), each world where AtlasOffset puts it.
std::vector<uint32_t> WorldAreas() {
    std::vector<uint32_t> world = Areas();
    if (std::find(world.begin(), world.end(), state.shown) == world.end() && state.shown != 0) {
        world.push_back(state.shown);
    }
    // Kaniel 2026-10-24: the areas the team's pings are on.
    for (int i = 0; i < Pings::view.count; ++i) {
        const uint32_t map = Pings::view.pings[i].map;
        if (map != 0 && std::find(world.begin(), world.end(), map) == world.end()) {
            world.push_back(map);
        }
    }
    return world;
}

/// The shown area's lit lamps, in their order.
std::vector<const bbcoop_map_lamp*> LitLamps() {
    std::vector<const bbcoop_map_lamp*> lamps;
    for (int i = 0; i < view.lamp_count; ++i) {
        if (view.lamps[i].lit && view.lamps[i].map == state.shown) {
            lamps.push_back(&view.lamps[i]);
        }
    }
    return lamps;
}

const char* BlockText(int block) {
    switch (block) {
    case BBCOOP_MAP_NOT_LIT:
        return T("That lamp is not lit", "Эта лампа не зажжена");
    case BBCOOP_MAP_NOT_IN_WORLD:
        return T("Not now", "Не сейчас");
    case BBCOOP_MAP_DOWN:
        return T("Not while you are down", "Нельзя, пока вы повержены");
    case BBCOOP_MAP_BOSS:
        return T("Not during a boss fight", "Нельзя во время битвы с боссом");
    case BBCOOP_MAP_TRAVELLING:
        return T("Already travelling", "Уже в пути");
    default:
        return "";
    }
}

/// The view's transform: atlas (x, z) to the screen inside `box`; in the 3D view the spot turned
/// by the yaw and seen from the pitch, its height `y` (metres; NaN: the middle's) lifting it.
struct Frame {
    ImVec2 p0, p1;
    ImVec2 Mid() const { return ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f); }
    ImVec2 Screen(float x, float z, float y = NAN) const {
        const ImVec2 m = Mid();
        if (!Angled()) {
            return ImVec2(m.x + (x - state.cx) * state.zoom, m.y - (z - state.cz) * state.zoom);
        }
        const float dx = x - state.cx, dz = z - state.cz;
        const float c = std::cos(state.yaw), s = std::sin(state.yaw);
        const float u = dx * c - dz * s, v = dx * s + dz * c; // right, ahead
        const float up = std::isnan(y) ? 0.0f : y - (std::isnan(state.cy) ? 0.0f : state.cy);
        return ImVec2(m.x + u * state.zoom,
                      m.y - (v * std::sin(state.pitch) + up * std::cos(state.pitch)) * state.zoom);
    }
    /// An area's own spot, where its world lies on the atlas.
    ImVec2 At(uint32_t map, float x, float z, float y = NAN) const {
        const Place p = AtlasOffset(map);
        return Screen(x + p.dx, z + p.dz, y);
    }
    /// A move of the picture by (sx, sy) screen pixels, in atlas metres.
    std::array<float, 2> Metres(float sx, float sy) const {
        if (!Angled()) {
            return {sx / state.zoom, -sy / state.zoom};
        }
        const float u = sx / state.zoom, v = -sy / state.zoom / std::max(0.2f, std::sin(state.pitch));
        const float c = std::cos(state.yaw), s = std::sin(state.yaw);
        return {u * c + v * s, -u * s + v * c};
    }
};

/// The zoom that fits the whole picture in the frame.
float FitZoom(const Frame& frame, const BbMapArt::Chart& chart) {
    const float w = float(chart.width) / chart.ppm, h = float(chart.height) / chart.ppm;
    return std::min((frame.p1.x - frame.p0.x) / w, (frame.p1.y - frame.p0.y) / h) * 0.95f;
}

void CentreOnChart(uint32_t map, const BbMapArt::Chart& chart) {
    const Place p = AtlasOffset(map);
    state.cx = chart.x0 + float(chart.width) / chart.ppm * 0.5f + p.dx;
    state.cz = chart.z1 - float(chart.height) / chart.ppm * 0.5f + p.dz;
}

/// The atlas's extent of the pictures drawn so far: x0, x1, z0, z1.
std::array<float, 4> AtlasExtent() {
    float x0 = FLT_MAX, x1 = -FLT_MAX, z0 = FLT_MAX, z1 = -FLT_MAX;
    for (const auto& [map, c] : charts) {
        const BbMapArt::Chart& chart = *c.chart;
        const Place p = AtlasOffset(map);
        x0 = std::min(x0, chart.x0 + p.dx), x1 = std::max(x1, chart.x0 + float(chart.width) / chart.ppm + p.dx);
        z0 = std::min(z0, chart.z1 - float(chart.height) / chart.ppm + p.dz), z1 = std::max(z1, chart.z1 + p.dz);
    }
    return {x0, x1, z0, z1};
}

/// The picture's top (BbMapArt::Chart::Storeys) at an atlas spot; NaN where nothing is drawn.
float TopAt(float x, float z) {
    for (const auto& [map, c] : charts) {
        const BbMapArt::Chart& chart = *c.chart;
        const BbMapArt::Chart::Storeys& s = chart.storeys;
        const Place p = AtlasOffset(map);
        const auto [px, py] = chart.PixelOf(x - p.dx, z - p.dz);
        const int cx = int(std::floor(px)) / BbMapArt::Chart::FloorCell, cy = int(std::floor(py)) / BbMapArt::Chart::FloorCell;
        if (px >= 0 && py >= 0 && cx < s.width && cy < s.height) {
            const int16_t top = s.roof[size_t(cy) * size_t(s.width) + size_t(cx)];
            if (top != BbMapArt::Chart::NoHeight) {
                return float(top) / 10.0f;
            }
        }
    }
    return NAN;
}

/// Kaniel 2026-10-24: the boxes (x0, y0, x1, y1) of the names drawn on the picture this frame, for
/// the pings' words to keep off.
std::vector<float> name_boxes;
void NoteName(ImFont* font, float size, ImVec2 at, const char* text, float align) {
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float x0 = at.x - extent.x * align;
    name_boxes.insert(name_boxes.end(), {x0, at.y, x0 + extent.x, at.y + extent.y});
}

/// A small lamp: its post and its glowing head; `focused` rings it.
void LampMark(ImDrawList* draw, ImVec2 at, bool focused, float pulse) {
    const float r = S(7.0f);
    draw->AddCircleFilled(at, r * 2.4f, LampLight(0.10f + 0.06f * pulse), 24);
    draw->AddRectFilled(ImVec2(at.x - S(1.2f), at.y), ImVec2(at.x + S(1.2f), at.y + r * 1.6f), Party::Iron(0.95f));
    draw->AddCircleFilled(at, r, Party::Rgba(0.10f, 0.07f, 0.05f, 0.9f), 20);
    draw->AddCircleFilled(at, r * 0.68f, LampLight(0.95f), 20);
    if (focused) {
        draw->AddCircle(at, r * 2.0f + S(2.0f) * pulse, Gold(0.95f), 32, S(2.0f));
    }
}

/// This player: an arrow turned to the camera's heading (0 north, turning right toward east).
void PlayerMark(ImDrawList* draw, ImVec2 at) {
    const float h = view.has_heading ? view.heading : 0.0f;
    const ImVec2 dir(std::sin(h), -std::cos(h)), side(-dir.y, dir.x);
    const float len = S(15.0f), wid = S(9.0f);
    const ImVec2 tip(at.x + dir.x * len, at.y + dir.y * len);
    const ImVec2 back(at.x - dir.x * len * 0.45f, at.y - dir.y * len * 0.45f);
    const ImVec2 l(back.x - dir.x * S(4.0f) + side.x * wid, back.y - dir.y * S(4.0f) + side.y * wid);
    const ImVec2 r(back.x - dir.x * S(4.0f) - side.x * wid, back.y - dir.y * S(4.0f) - side.y * wid);
    draw->AddCircleFilled(at, S(15.0f), Party::Rgba(0, 0, 0, 0.35f), 24);
    draw->AddQuadFilled(tip, l, back, r, Party::Rgba(0.97f, 0.96f, 0.92f, 1.0f));
    draw->AddQuad(tip, l, back, r, Party::Rgba(0.05f, 0.04f, 0.03f, 0.9f), S(1.5f));
}

/// A party member: its portrait (or initial) in a dark disc, ringed in light gray; red while down.
void MemberMark(ImDrawList* draw, ImVec2 at, int index, bool label = true) {
    const bbcoop_map_member& m = view.members[index];
    const bool down = m.vital == BBCOOP_VITAL_DOWN, dead = m.vital == BBCOOP_VITAL_DEAD;
    const float r = S(13.0f);
    draw->AddCircleFilled(ImVec2(at.x + S(1.0f), at.y + S(1.5f)), r + S(1.5f), Party::Rgba(0, 0, 0, 0.55f), 32);
    draw->AddCircleFilled(at, r, Party::Rgba(0.06f, 0.05f, 0.05f, 0.92f), 32);
    const char* name = m.name[0] != '\0' ? m.name : Ui::T("Hunter", "Охотник"); // never an online id
    const Compass::PortraitTexture& face = portraits[index];
    if (face.has && face.member == m.board && !dead) {
        const float f = r - S(1.0f);
        draw->AddImage(face.texture->GetTexRef(), ImVec2(at.x - f, at.y - f), ImVec2(at.x + f, at.y + f));
    } else {
        const std::string initial = Compass::Initial(name);
        const float size = S(17.0f);
        const ImVec2 extent = serif_font->CalcTextSizeA(size, FLT_MAX, 0.0f, initial.c_str());
        Party::Text(draw, serif_font, size, ImVec2(at.x, at.y - extent.y * 0.5f), Gray(dead ? 0.5f : 1.0f),
                    initial.c_str(), 0.5f);
    }
    draw->AddCircle(at, r, down ? Party::Blood(0.95f) : Gray(dead ? 0.5f : 1.0f), 32, S(2.0f));
    if (label) {
        Party::Text(draw, sans_font, S(14.0f), ImVec2(at.x, at.y + r + S(3.0f)), Gray(0.95f), name, 0.5f);
        NoteName(sans_font, S(14.0f), ImVec2(at.x, at.y + r + S(3.0f)), name, 0.5f);
    }
}

/// Kaniel 2026-10-20: this player's bloodstain: the compass's drop and its echoes under it.
void StainMark(ImDrawList* draw, const bbcoop_map_stain& stain, ImVec2 at) {
    const float r = S(12.0f);
    Compass::Drop(draw, at, r, Gray(0.95f), Party::Rgba(0, 0, 0, 0.55f));
    char echoes[16];
    std::snprintf(echoes, sizeof(echoes), "%d", int(stain.echoes));
    Party::Text(draw, sans_font, S(13.0f), ImVec2(at.x, at.y + r + S(3.0f)), Gray(0.9f), echoes, 0.5f);
}

/// The picture with its marks, inside the game's card art.
void DrawPicture(ImDrawList* draw, const Frame& frame, const std::vector<const bbcoop_map_lamp*>& lamps,
                 float pulse) {
    MenuArt::Nine(draw, MenuArt::Card, ImVec2(frame.p0.x - S(14.0f), frame.p0.y - S(14.0f)),
                  ImVec2(frame.p1.x + S(14.0f), frame.p1.y + S(14.0f)), 14.0f, S(12.0f), 0.95f);
    draw->AddRectFilled(frame.p0, frame.p1, Party::Rgba(0.045f, 0.040f, 0.035f, 0.92f));
    draw->PushClipRect(frame.p0, frame.p1, true);
    name_boxes.clear();
    if (const ChartTexture* shown = Shown()) {
        // The world's other areas under the shown one, a little darker; the shown one over them.
        const auto picture = [&](const ChartTexture& c, float tone) {
            const BbMapArt::Chart& chart = *c.chart;
            const ImVec2 a = frame.At(c.map, chart.x0, chart.z1);
            const ImVec2 b = frame.At(c.map, chart.x0 + float(chart.width) / chart.ppm,
                                      chart.z1 - float(chart.height) / chart.ppm);
            draw->AddImage(c.texture->GetTexRef(), a, b, ImVec2(0, 0), ImVec2(1, 1), Party::Rgba(tone, tone, tone, 1.0f));
        };
        // Kaniel 2026-10-19 ("can we prerender it?"): each area's picture from the side the 3D
        // view is seen from, placed by the view's own frame (BbMapArt::ToView) and its world's
        // place on the atlas.
        const auto side_picture = [&](int side, float alpha) {
            const BbMapArt::ObliqueView seen = BbMapArt::ViewOf(side);
            const float c = std::cos(seen.yaw), s = std::sin(seen.yaw), sp = std::sin(seen.pitch),
                        cp = std::cos(seen.pitch);
            const float cy = std::isnan(state.cy) ? 0.0f : state.cy;
            const float uc = state.cx * c - state.cz * s, wc = (state.cx * s + state.cz * c) * sp + cy * cp;
            std::vector<std::pair<float, const ChartTexture*>> drawn;
            for (const auto& [key, side_chart] : sides) {
                if (int(key & 7u) == side && side_chart.texture != nullptr) {
                    const BbMapArt::Chart& chart = *side_chart.chart;
                    // Far areas first: the top of a picture lies furthest away.
                    drawn.push_back({chart.z1, &side_chart});
                }
            }
            std::sort(drawn.begin(), drawn.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            const ImVec2 m = frame.Mid();
            for (const auto& [far, side_chart] : drawn) {
                const BbMapArt::Chart& chart = *side_chart->chart;
                const Place p = AtlasOffset(side_chart->map);
                const float du = p.dx * c - p.dz * s, dw = (p.dx * s + p.dz * c) * sp;
                const ImVec2 a(m.x + (chart.x0 + du - uc) * state.zoom, m.y - (chart.z1 + dw - wc) * state.zoom);
                const ImVec2 b(a.x + float(chart.width) / chart.ppm * state.zoom,
                               a.y + float(chart.height) / chart.ppm * state.zoom);
                const float tone = side_chart->map == state.shown ? 1.0f : 0.62f;
                draw->AddImage(side_chart->texture->GetTexRef(), a, b, ImVec2(0, 0), ImVec2(1, 1),
                               Party::Rgba(tone, tone, tone, alpha));
            }
            return !drawn.empty();
        };
        bool sides_shown = state.three_d && !Live() && side_picture(state.side, 1.0f);
        if (state.three_d && Live() && live_ready) {
            // Kaniel 2026-10-19 (live 3D): the models drawn this frame on the GPU, with this camera.
            ImVec2 uv1(1, 1);
            const ImTextureRef live_picture = BbMap3d::Request(
                BbMap3d::Camera{frame.p0.x, frame.p0.y, frame.p1.x, frame.p1.y, state.cx,
                                std::isnan(state.cy) ? 0.0f : state.cy, state.cz, state.yaw, state.pitch, state.zoom},
                uv1);
            if (live_picture.GetTexID() != ImTextureID_Invalid) {
                draw->AddImage(live_picture, frame.p0, frame.p1, ImVec2(0, 0), uv1);
                sides_shown = true;
            }
        }
        // Kaniel 2026-10-19 ("now it shows this state with boxes"): while the sides are still
        // being drawn, the flat map with a note.
        const bool preparing = state.three_d && !sides_shown;
        if (!sides_shown) {
            for (const auto& [map, c] : charts) {
                if (map != state.shown && c.texture != nullptr) {
                    picture(c, 0.62f);
                }
            }
            picture(*shown, 1.0f);
        }
        // Zoomed out: each area's name over its middle.
        if (state.zoom < S(1.2f) && charts.size() > 1) {
            for (const auto& [map, c] : charts) {
                const BbMapArt::Chart& chart = *c.chart;
                const ImVec2 at = frame.At(map, chart.x0 + float(chart.width) / chart.ppm * 0.5f,
                                           chart.z1 - float(chart.height) / chart.ppm * 0.5f);
                Party::Text(draw, MenuArt::Font(), S(map == state.shown ? 24.0f : 20.0f), at,
                            map == state.shown ? Bright() : Plain(0.8f), AreaName(map).c_str(), 0.5f);
                NoteName(MenuArt::Font(), S(map == state.shown ? 24.0f : 20.0f), at, AreaName(map).c_str(), 0.5f);
            }
        }
        // Kaniel 2026-10-18: the places the game names (Iosefka's Clinic, the Healing Church
        // Workshop) where their floors lie, once close enough to read.
        if (state.zoom >= S(1.0f)) {
            for (const auto& [map, c] : charts) {
                for (const BbMapArt::Chart::Label& label : c.chart->labels) {
                    const std::string name = BbMapCharts::AreaName(label.name_id);
                    if (!name.empty()) {
                        // A little under its middle: a lamp's name sits over the lamp there.
                        const ImVec2 at = frame.At(map, label.x, label.z, TopAt(label.x + AtlasOffset(map).dx,
                                                                                label.z + AtlasOffset(map).dz));
                        Party::Text(draw, MenuArt::Font(), S(17.0f), ImVec2(at.x, at.y + S(22.0f)), Plain(0.85f),
                                    name.c_str(), 0.5f);
                        NoteName(MenuArt::Font(), S(17.0f), ImVec2(at.x, at.y + S(22.0f)), name.c_str(), 0.5f);
                    }
                }
            }
        }
        // The lamps lit in the other areas, then the shown area's (focusable).
        for (int i = 0; i < view.lamp_count; ++i) {
            const bbcoop_map_lamp& lamp = view.lamps[i];
            if (lamp.lit && lamp.map != state.shown && charts.contains(lamp.map)) {
                LampMark(draw, frame.At(lamp.map, lamp.x, lamp.z, lamp.y), false, 0.0f);
            }
        }
        for (size_t i = 0; i < lamps.size(); ++i) {
            LampMark(draw, frame.At(lamps[i]->map, lamps[i]->x, lamps[i]->z, lamps[i]->y), int(i) == state.focus, pulse);
        }
        // Kaniel 2026-10-20: this player's bloodstain on its map, under the members.
        if (BbSettings::Get().bloodstains && view.has_stain && charts.contains(view.stain.map)) {
            StainMark(draw, view.stain, frame.At(view.stain.map, view.stain.x, view.stain.z, view.stain.y));
        }
        for (int i = 0; i < view.member_count; ++i) {
            const bbcoop_map_member& m = view.members[i];
            if (m.has_at && m.map != 0) {
                MemberMark(draw, frame.At(m.map, m.x, m.z, m.y), i);
            }
        }
        if (view.has_me && view.map != 0) {
            PlayerMark(draw, frame.At(view.map, view.x, view.z, view.y));
        }
        // The lamp under the cursor (else the list's): ringed, its name over it.
        const bbcoop_map_lamp* named = nullptr;
        for (int i = 0; i < view.lamp_count; ++i) {
            if (view.lamps[i].warp_point == state.hover) {
                named = &view.lamps[i];
            }
        }
        if (named == nullptr && state.focus >= 0 && state.focus < int(lamps.size())) {
            named = lamps[size_t(state.focus)];
        }
        if (named != nullptr) {
            const ImVec2 at = frame.At(named->map, named->x, named->z, named->y);
            LampMark(draw, at, true, pulse);
            const std::string name = BbMapCharts::LampName(named->name_id);
            Party::Text(draw, MenuArt::Font(), S(20.0f), ImVec2(at.x, at.y - S(40.0f)), Bright(), name.c_str(), 0.5f);
            NoteName(MenuArt::Font(), S(20.0f), ImVec2(at.x, at.y - S(40.0f)), name.c_str(), 0.5f);
        }
        // Kaniel 2026-10-24: the team's pings, on every map, drawn last: their words go off the
        // place names (one on another area than this player's names it).
        for (int i = 0; i < Pings::view.count; ++i) {
            const bbcoop_ping& ping = Pings::view.pings[i];
            if (!charts.contains(ping.map)) {
                continue;
            }
            const float alpha = Pings::Alpha(ping), r = S(11.0f), size = S(17.0f);
            const ImVec2 at = frame.At(ping.map, ping.x, ping.z, ping.y);
            Pings::Mark(draw, at, r, alpha);
            const std::string line = Pings::Label(ping, false, ping.map != view.map ? AreaName(ping.map) : std::string{});
            const ImVec2 extent = sans_font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.c_str());
            float box[4];
            bbcoop_ping_label_place(at.x, at.y, r, extent.x, extent.y, name_boxes.data(), int(name_boxes.size() / 4),
                                    S(4.0f), box);
            Pings::Words(draw, size, ImVec2(box[0], box[1]), alpha, line.c_str(), 0.0f);
            name_boxes.insert(name_boxes.end(), box, box + 4);
        }
        // The cursor: a ring with four ticks, in the compass's light gray.
        if (state.cursor.x >= 0.0f) {
            const ImVec2 c = state.cursor;
            const float r = S(16.0f), t = S(10.0f); // Kaniel 2026-10-19: "a bit bigger"
            const ImU32 ink = Party::Rgba(0, 0, 0, 0.6f), gray = Gray(0.95f);
            for (const auto& [color, width] : {std::pair{ink, S(3.5f)}, {gray, S(1.6f)}}) {
                draw->AddCircle(c, r, color, 32, width);
                draw->AddLine(ImVec2(c.x - r - t, c.y), ImVec2(c.x - r + S(2.0f), c.y), color, width);
                draw->AddLine(ImVec2(c.x + r - S(2.0f), c.y), ImVec2(c.x + r + t, c.y), color, width);
                draw->AddLine(ImVec2(c.x, c.y - r - t), ImVec2(c.x, c.y - r + S(2.0f)), color, width);
                draw->AddLine(ImVec2(c.x, c.y + r - S(2.0f)), ImVec2(c.x, c.y + r + t), color, width);
            }
        }
        if (preparing) {
            Party::Text(draw, MenuArt::Font(), S(22.0f), ImVec2(frame.Mid().x, frame.p0.y + S(16.0f)), Plain(0.9f),
                        T("Preparing the 3D view...", "Готовится 3D-вид..."), 0.5f);
        }
    } else {
        ImGui::PushFont(MenuArt::Font(), 24.0f);
        const bool failed = BbMapCharts::Failed(state.shown);
        const std::string text = failed ? T("No map of this place", "Для этого места нет карты")
                                        : std::string(T("Charting the area", "Составляется карта")) +
                                              std::string(int(ImGui::GetTime() * 2.0) % 4, '.');
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const ImVec2 m = frame.Mid();
        draw->AddText(ImVec2(m.x - ts.x * 0.5f, m.y - ts.y * 0.5f), failed ? Dim() : Plain(0.85f), text.c_str());
        ImGui::PopFont();
    }
    draw->PopClipRect();
    MenuArt::Vignette(draw, frame.p0, frame.p1, 0.35f);
}

/// The right-hand column: the area's lit lamps (the focused one framed), then the party.
void DrawList(ImDrawList* draw, ImVec2 at, float width, float bottom, const std::vector<const bbcoop_map_lamp*>& lamps) {
    ImGui::PushFont(MenuArt::Font(), 22.0f);
    draw->AddText(at, Gold(), T("Lamps", "Лампы"));
    ImGui::PopFont();
    ListRule(draw, at.x, at.x + width, at.y + S(34.0f));
    float y = at.y + S(46.0f);
    const float row_h = S(40.0f);
    ImGui::PushFont(MenuArt::Font(), 19.0f);
    if (lamps.empty()) {
        draw->AddText(ImVec2(at.x + S(8.0f), y + S(8.0f)), Dim(), T("No lamp lit here", "Здесь нет зажжённых ламп"));
        y += row_h;
    }
    for (size_t i = 0; i < lamps.size() && y + row_h < bottom - S(150.0f); ++i, y += row_h) {
        const ImVec2 p0(at.x - S(6.0f), y - S(2.0f)), p1(at.x + width + S(4.0f), y + row_h - S(4.0f));
        MenuArt::Nine(draw, MenuArt::Row, p0, p1, 10.0f, S(6.0f), 0.8f);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(int(i));
        if (ImGui::InvisibleButton("##lamp", ImVec2(p1.x - p0.x, p1.y - p0.y))) {
            state.focus = int(i);
            state.asking = lamps[i]->warp_point;
            state.ask_yes = true;
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x +
                                              ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y >
                                          0.0f) {
            state.focus = int(i);
        }
        ImGui::PopID();
        const bool focused = int(i) == state.focus;
        if (focused) {
            const float in = S(2.0f);
            draw->AddRectFilled(ImVec2(p0.x + in, p0.y + in), ImVec2(p1.x - in, p1.y - in), Gold(0.10f));
            draw->AddRect(ImVec2(p0.x + in, p0.y + in), ImVec2(p1.x - in, p1.y - in), Gold(0.9f), 0.0f, 0, S(1.5f));
        }
        LampMark(draw, ImVec2(at.x + S(14.0f), y + row_h * 0.5f - S(4.0f)), false, 0.0f);
        const std::string name = BbMapCharts::LampName(lamps[i]->name_id);
        draw->AddText(ImVec2(at.x + S(34.0f), y + S(8.0f)), focused ? Bright() : Plain(),
                      Fit(name.c_str(), width - S(40.0f)).c_str());
    }
    ImGui::PopFont();
    // The party: who is where.
    y += S(18.0f);
    if (view.member_count > 0) {
        ImGui::PushFont(MenuArt::Font(), 22.0f);
        draw->AddText(ImVec2(at.x, y), Gold(), T("Hunters", "Охотники"));
        ImGui::PopFont();
        ListRule(draw, at.x, at.x + width, y + S(34.0f));
        y += S(46.0f);
        ImGui::PushFont(MenuArt::Font(), 18.0f);
        for (int i = 0; i < view.member_count && y + row_h < bottom; ++i, y += row_h) {
            const bbcoop_map_member& m = view.members[i];
            MemberMark(draw, ImVec2(at.x + S(14.0f), y + S(10.0f)), i, false);
            const std::string where = m.map == 0 ? T("Away", "Не в мире") : AreaName(m.map);
            const char* name = m.name[0] != '\0' ? m.name : Ui::T("Hunter", "Охотник");
            draw->AddText(ImVec2(at.x + S(38.0f), y + S(1.0f)), Plain(), Fit(name, width * 0.45f).c_str());
            const std::string shown = Fit(where.c_str(), width * 0.5f);
            const float wx = at.x + width - ImGui::CalcTextSize(shown.c_str()).x;
            draw->AddText(ImVec2(wx, y + S(1.0f)), m.map == view.map ? Gray(0.95f) : Dim(), shown.c_str());
        }
        ImGui::PopFont();
    }
}

/// The question before a trip: the lamp's name, Yes and No in the game's rows.
void DrawQuestion(ImDrawList* draw, ImVec2 display, bool yes_pressed, bool no_pressed) {
    const bbcoop_map_lamp* lamp = nullptr;
    for (int i = 0; i < view.lamp_count; ++i) {
        if (view.lamps[i].warp_point == state.asking) {
            lamp = &view.lamps[i];
        }
    }
    if (lamp == nullptr) {
        state.asking = 0;
        return;
    }
    const ImVec2 size(S(640.0f), S(230.0f));
    const ImVec2 p0((display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f), p1(p0.x + size.x, p0.y + size.y);
    draw->AddRectFilled(ImVec2(0, 0), display, Party::Rgba(0, 0, 0, 0.45f));
    HunterCard(draw, p0, p1, false);
    ImGui::PushFont(MenuArt::Font(), 24.0f);
    const std::string name = BbMapCharts::LampName(lamp->name_id);
    ImVec2 ts = ImGui::CalcTextSize(name.c_str());
    draw->AddText(ImVec2((display.x - ts.x) * 0.5f, p0.y + S(44.0f)), Bright(), name.c_str());
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 20.0f);
    const char* line = T("Travel to this lamp?", "Отправиться к этой лампе?");
    ts = ImGui::CalcTextSize(line);
    draw->AddText(ImVec2((display.x - ts.x) * 0.5f, p0.y + S(80.0f)), Plain(), line);
    ImGui::PopFont();
    Divider(draw, p0.x + S(40.0f), p1.x - S(40.0f), p0.y + S(122.0f), 0.7f);
    ImGui::PushFont(MenuArt::Font(), 22.0f);
    const float bw = S(200.0f), bh = S(48.0f), gap = S(30.0f);
    const float bx = (display.x - 2 * bw - gap) * 0.5f, by = p1.y - bh - S(36.0f);
    bool choose = yes_pressed, cancel = no_pressed;
    for (int b = 0; b < 2; ++b) {
        const bool yes = b == 0;
        const ImVec2 a(bx + float(b) * (bw + gap), by), e(a.x + bw, a.y + bh);
        MenuArt::Nine(draw, MenuArt::Row, a, e, 12.0f, S(9.0f), 1.0f);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(b);
        if (ImGui::InvisibleButton("##ask", ImVec2(bw, bh))) {
            (yes ? choose : cancel) = true;
            state.ask_yes = yes;
        }
        if (ImGui::IsItemHovered()) {
            state.ask_yes = yes;
        }
        ImGui::PopID();
        const bool focused = state.ask_yes == yes;
        if (focused) {
            draw->AddRect(ImVec2(a.x + S(2.0f), a.y + S(2.0f)), ImVec2(e.x - S(2.0f), e.y - S(2.0f)), Gold(0.9f), 0.0f,
                          0, S(1.5f));
        }
        const char* label = yes ? T("Travel", "В путь") : T("Stay", "Остаться");
        ts = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(a.x + (bw - ts.x) * 0.5f, a.y + (bh - ts.y) * 0.5f), focused ? Bright() : Plain(), label);
    }
    ImGui::PopFont();
    if (choose && state.ask_yes) {
        if (view.block == BBCOOP_MAP_GO) {
            bbcoop_map_travel(state.asking);
            bbcoop_map_menu(0);
        } else {
            state.notice = BlockText(view.block);
            state.notice_until = ImGui::GetTime() + 3.0;
        }
        state.asking = 0;
    } else if (cancel || (choose && !state.ask_yes)) {
        state.asking = 0;
    }
}

/// The game's key guide at the bottom: its buttons' icons (a keyboard player's keys as key caps),
/// then what they do, centred on [x0, x1] above `bottom`.
void DrawKeys(ImDrawList* draw, float x0, float x1, float bottom) {
    ImGui::PushFont(MenuArt::Font(), 18.0f);
    // Kaniel 2026-10-19 ("make it opt in"): L3 only while the 3D map is on in the settings.
    std::vector<Keys::Hint> keys = {
        {"L1 R1", "Q E", T("Area", "Область")},
        {"LS", "WASD", T("Move", "Сдвиг")},
        {state.three_d && Live() ? "L2 R2" : "RS", "- =", T("Zoom", "Масштаб")},
        {"Options", "C", T("You", "Вы")},
        {"Square", "RMB", T("Ping", "Метка")},
        {"Triangle", "Tab", T("Whole map", "Вся карта")},
        {"R3", "F", std::isnan(drawn_floors) ? T("Floors", "Этажи") : T("Roofs", "Крыши")},
    };
    if (state.three_d && Live()) {
        keys.insert(keys.begin() + 3, {"RS", "Z X", T("Turn", "Поворот")});
    }
    if (state.three_d || BbSettings::Get().map_3d) {
        keys.push_back({"L3", "V", state.three_d ? T("Flat", "Плоско") : T("3D", "3D")});
    }
    keys.push_back({"Cross", "Enter", T("Travel", "В путь")});
    keys.push_back({"Circle", "Esc", T("Close", "Закрыть")});
    Keys::Guide(draw, keys, x0, x1, bottom);
    ImGui::PopFont();
}

/// Kaniel 2026-10-24: a team ping on the spot under `pick` (the cross, or the mouse): on the area
/// drawn there, at its picture's height there (else this player's).
void PingAt(const Frame& frame, ImVec2 pick) {
    const ImVec2 m = frame.Mid();
    const auto [dx, dz] = frame.Metres(pick.x - m.x, pick.y - m.y);
    const float ax = state.cx + dx, az = state.cz + dz;
    std::vector<bbcoop_ping_area> areas;
    for (const auto& [map, c] : charts) {
        const BbMapArt::Chart& chart = *c.chart;
        const Place p = AtlasOffset(map);
        areas.push_back({map, chart.x0 + p.dx, chart.x0 + float(chart.width) / chart.ppm + p.dx,
                         chart.z1 - float(chart.height) / chart.ppm + p.dz, chart.z1 + p.dz, p.dx, p.dz});
    }
    float y = TopAt(ax, az);
    if (std::isnan(y)) {
        y = view.has_me ? view.y : 0.0f;
    }
    if (!bbcoop_ping_map(areas.data(), int(areas.size()), state.shown, ax, az, y)) {
        std::printf("Map: no area under the ping at (%.1f, %.1f)\n", ax, az);
    }
}

bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat = false) {
    for (const ImGuiKey key : keys) {
        if (ImGui::IsKeyPressed(key, repeat)) {
            return true;
        }
    }
    return false;
}

/// Moving and zooming the picture: the right stick, the arrow-free keys, the mouse.
/// Kaniel 2026-10-19 ("more like elden ring"): the cross stays in the middle; the left stick (or
/// WASD, or a left drag) moves the map under it, the right stick zooms (up: in), as L2 / R2, the
/// wheel and = / - do. In the live 3D view the right stick turns (Z / X) and tilts it (a right drag
/// does both), and only L2 / R2 zoom. Near a lit lamp the map slows and, once the stick lets go, settles
/// with the lamp under the cross.
void MoveView(const Frame& frame) {
    ImGuiIO& io = ImGui::GetIO();
    const float dt = std::min(io.DeltaTime, 0.1f);
    const auto analog = [](ImGuiKey key) { return ImGui::GetKeyData(key)->AnalogValue; };
    float lx = analog(ImGuiKey_GamepadLStickRight) - analog(ImGuiKey_GamepadLStickLeft);
    float ly = analog(ImGuiKey_GamepadLStickDown) - analog(ImGuiKey_GamepadLStickUp);
    const float rx = analog(ImGuiKey_GamepadRStickRight) - analog(ImGuiKey_GamepadRStickLeft);
    const float ry = analog(ImGuiKey_GamepadRStickDown) - analog(ImGuiKey_GamepadRStickUp);
    const bool inside = io.MousePos.x >= frame.p0.x && io.MousePos.x < frame.p1.x && io.MousePos.y >= frame.p0.y &&
                        io.MousePos.y < frame.p1.y;
    const bool live = state.three_d && Live();
    if (state.three_d && !live) {
        // Kaniel 2026-10-19 ("we need just one side"): the prerendered 3D view is seen from the south.
        const BbMapArt::ObliqueView seen = BbMapArt::ViewOf(state.side);
        state.yaw = seen.yaw;
        state.pitch = seen.pitch;
    }
    if (live) {
        // Kaniel 2026-10-19 ("in 3d right stick should rotate, r2/l2 zoom"): sideways turns, up and
        // down tilts.
        const float turn = (ImGui::IsKeyDown(ImGuiKey_X) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_Z) ? 1.0f : 0.0f);
        state.yaw += (rx + turn) * 1.8f * dt;
        state.pitch = std::clamp(state.pitch - ry * 0.9f * dt, 0.35f, 1.5707f);
        if (inside && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f)) {
            state.yaw += io.MouseDelta.x * 0.006f;
            state.pitch = std::clamp(state.pitch + io.MouseDelta.y * 0.004f, 0.35f, 1.5707f);
        }
    }
    lx += (ImGui::IsKeyDown(ImGuiKey_D) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_A) ? 1.0f : 0.0f);
    ly += (ImGui::IsKeyDown(ImGuiKey_S) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_W) ? 1.0f : 0.0f);
    // The lit lamp nearest the middle, on screen.
    const ImVec2 mid = frame.Mid();
    const bbcoop_map_lamp* near_lamp = nullptr;
    ImVec2 near_at{};
    float near_d = S(56.0f) * S(56.0f);
    for (int i = 0; i < view.lamp_count; ++i) {
        const bbcoop_map_lamp& lamp = view.lamps[i];
        if (!lamp.lit || !charts.contains(lamp.map)) {
            continue;
        }
        const ImVec2 at = frame.At(lamp.map, lamp.x, lamp.z, lamp.y);
        const float d = (at.x - mid.x) * (at.x - mid.x) + (at.y - mid.y) * (at.y - mid.y);
        if (d < near_d) {
            near_d = d, near_lamp = &lamp, near_at = at;
        }
    }
    const bool steering = std::abs(lx) > 0.15f || std::abs(ly) > 0.15f;
    const float speed = S(700.0f) * (near_lamp != nullptr ? 0.55f : 1.0f); // screen pixels a second
    if (steering) {
        const auto [mx, mz] = frame.Metres(lx * speed * dt, ly * speed * dt);
        state.cx += mx;
        state.cz += mz;
    } else if (near_lamp != nullptr && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Let go near a lamp: the map settles with it under the cross.
        const float ease = std::min(1.0f, dt * 10.0f);
        const auto [mx, mz] = frame.Metres((near_at.x - mid.x) * ease, (near_at.y - mid.y) * ease);
        state.cx += mx;
        state.cz += mz;
    }
    if (inside && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const auto [mx, mz] = frame.Metres(io.MouseDelta.x, io.MouseDelta.y);
        state.cx -= mx;
        state.cz -= mz;
    }
    float zoom = live ? 1.0f : std::pow(2.0f, -ry * dt * 1.6f);
    if (ImGui::IsKeyDown(ImGuiKey_GamepadR2)) {
        zoom *= std::pow(2.0f, dt * 1.5f);
    }
    if (ImGui::IsKeyDown(ImGuiKey_GamepadL2)) {
        zoom /= std::pow(2.0f, dt * 1.5f);
    }
    if (ImGui::IsKeyDown(ImGuiKey_Equal)) {
        zoom *= std::pow(2.0f, dt * 1.5f);
    }
    if (ImGui::IsKeyDown(ImGuiKey_Minus)) {
        zoom /= std::pow(2.0f, dt * 1.5f);
    }
    if (inside && io.MouseWheel != 0.0f) {
        zoom *= std::pow(1.25f, io.MouseWheel);
    }
    state.zoom = std::clamp(state.zoom * zoom, S(0.05f), S(12.0f));
    // The middle stays over the pictures.
    if (!charts.empty()) {
        const auto [x0, x1, z0, z1] = AtlasExtent();
        state.cx = std::clamp(state.cx, x0, x1);
        state.cz = std::clamp(state.cz, z0, z1);
    }
    // The 3D view turns round the ground at the middle (eased, so the picture never jumps).
    if (const float top = TopAt(state.cx, state.cz); !std::isnan(top)) {
        state.cy = std::isnan(state.cy) ? top : state.cy + (top - state.cy) * std::min(1.0f, dt * 4.0f);
    }
}

/// The cross: always in the middle of the picture (a mouse pointing into the picture picks what
/// is under it instead); the lit lamp under it becomes `state.hover`.
void MoveCursor(const Frame& frame) {
    ImGuiIO& io = ImGui::GetIO();
    state.cursor = frame.Mid();
    const bool inside = io.MousePos.x >= frame.p0.x && io.MousePos.x < frame.p1.x && io.MousePos.y >= frame.p0.y &&
                        io.MousePos.y < frame.p1.y;
    static double mouse_moved_at = -10.0;
    if (inside && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        mouse_moved_at = ImGui::GetTime();
    }
    const ImVec2 pick = inside && ImGui::GetTime() - mouse_moved_at < 2.0 ? io.MousePos : state.cursor;
    state.hover = 0;
    float best = S(30.0f) * S(30.0f);
    for (int i = 0; i < view.lamp_count; ++i) {
        const bbcoop_map_lamp& lamp = view.lamps[i];
        if (!lamp.lit || !charts.contains(lamp.map)) {
            continue;
        }
        const ImVec2 at = frame.At(lamp.map, lamp.x, lamp.z, lamp.y);
        const float d = (at.x - pick.x) * (at.x - pick.x) + (at.y - pick.y) * (at.y - pick.y);
        if (d < best) {
            best = d;
            state.hover = lamp.warp_point;
        }
    }
}

void Screen() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 4.0f);
    const auto areas = Areas();
    if (state.fresh) {
        state = {};
        state.fresh = false;
        state.opened = ImGui::GetTime();
        // Kaniel 2026-10-19 ("use it as default"): with the 3D map on, the map first opens in 3D.
        static bool first_opening = true;
        if (first_opening) {
            first_opening = false;
            three_d_kept = BbSettings::Get().map_3d != BbSettings::Map3dOff;
        }
        state.three_d = three_d_kept;
    }
    // The player's own area once the game thread has read where it stands (a moment after opening).
    if (!state.placed && (view.has_me || ImGui::GetTime() - state.opened > 1.5)) {
        state.placed = true;
        state.shown = view.map != 0 ? view.map : (areas.empty() ? 0 : areas.front());
        state.zoom = 0;
    }
    // Keys first (the question takes them while it is up).
    // The list's up and down: the d-pad and the arrows (the left stick moves the cursor).
    const bool up = Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp}, true);
    const bool down = Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown}, true);
    const bool left = Pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft}, true);
    const bool right = Pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight}, true);
    const bool cross = Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown});
    const bool asked_before = state.asking != 0; // a question opened this frame takes no keys yet
    const bool circle = Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight});
    if (state.asking == 0) {
        // Kaniel 2026-10-18: the touchpad (Back, View) that opened it closes it too.
        if (circle || Pressed({ImGuiKey_M, ImGuiKey_GamepadBack})) {
            bbcoop_map_menu(0);
            return;
        }
        if (!areas.empty() && Pressed({ImGuiKey_GamepadL1, ImGuiKey_GamepadR1, ImGuiKey_Q, ImGuiKey_E, ImGuiKey_PageUp,
                                       ImGuiKey_PageDown})) {
            const bool next = Pressed({ImGuiKey_GamepadR1, ImGuiKey_E, ImGuiKey_PageDown});
            auto it = std::find(areas.begin(), areas.end(), state.shown);
            const int at = it == areas.end() ? 0 : int(it - areas.begin());
            const int n = int(areas.size());
            state.shown = areas[size_t(((at + (next ? 1 : -1)) % n + n) % n)];
            state.zoom = 0;
            state.focus = 0;
        }
    }
    const auto lamps = LitLamps();
    if (!lamps.empty()) {
        state.focus = std::clamp(state.focus, 0, int(lamps.size()) - 1);
    }

    ImDrawList* back = ImGui::GetBackgroundDrawList();
    back->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 175));
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    const bool wall = MenuArt::Has(MenuArt::Wall);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, wall ? ImVec4(0, 0, 0, 0) : ImVec4(0.035f, 0.032f, 0.028f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##bbport_map", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (wall) {
        MenuArt::Cover(draw, MenuArt::Wall, ImVec2(0, 0), display, 1.0f);
        MenuArt::Vignette(draw, ImVec2(0, 0), display, 0.75f);
    }
    const ImVec2 size(std::min(S(1760.0f), display.x - S(60.0f)), display.y - S(40.0f));
    const ImVec2 win((display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f);
    Divider(draw, win.x + S(24.0f), win.x + size.x - S(24.0f), win.y + S(14.0f));
    Divider(draw, win.x + S(24.0f), win.x + size.x - S(24.0f), win.y + size.y - S(14.0f));

    // The area's name, with L1 and R1 either side when there are others.
    ImGui::PushFont(MenuArt::Font(), 40.0f);
    const std::string title = state.shown != 0 ? AreaName(state.shown) : std::string(T("Map", "Карта"));
    ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    const float mid = win.x + size.x * 0.5f;
    draw->AddText(ImVec2(mid - ts.x * 0.5f, win.y + S(30.0f)), Bright(), title.c_str());
    ImGui::PopFont();
    // Kaniel 2026-10-18: an area whose main bosses are down carries the party list's headstone.
    float title_end = mid + ts.x * 0.5f + S(24.0f);
    for (int i = 0; i < view.cleared_count; ++i) {
        if (view.cleared[i] == (state.shown & 0xFFFF0000u)) {
            Party::Icon(draw, BbPartyIcons::Icon::Fallen, ImVec2(title_end + S(4.0f), win.y + S(54.0f)), S(34.0f), 0.8f);
            title_end += S(30.0f);
        }
    }
    if (areas.size() > 1) { // the buttons to the others: L1 and R1 (Q and E keys) either side
        const bool keyboard = Keys::IconSet() == 'k';
        const char* back = keyboard ? "Q" : "L1";
        const float icon = S(30.0f), y = win.y + S(54.0f);
        const float w = Keys::Glyphs(nullptr, back, keyboard, ImVec2(0, 0), icon, 1.0f);
        Keys::Glyphs(draw, back, keyboard, ImVec2(mid - ts.x * 0.5f - S(24.0f) - w, y), icon, 0.9f);
        Keys::Glyphs(draw, keyboard ? "E" : "R1", keyboard, ImVec2(title_end, y), icon, 0.9f);
    }
    const float ornament = HeaderOrnament(draw, mid, win.y + S(78.0f), 0.9f);

    const float list_w = S(430.0f);
    // Kaniel 2026-10-19: room under the picture for the key guide, clear of the bottom ornament.
    const float top = win.y + S(84.0f) + ornament + S(20.0f), bottom = win.y + size.y - S(100.0f);
    const Frame frame{ImVec2(win.x + S(40.0f), top), ImVec2(win.x + size.x - list_w - S(70.0f), bottom)};
    if (const ChartTexture* shown = Shown(); state.zoom <= 0.0f && shown != nullptr) {
        state.zoom = FitZoom(frame, *shown->chart);
        CentreOnChart(state.shown, *shown->chart);
        if (view.has_me && view.map == state.shown) {
            state.zoom = std::max(state.zoom, S(3.0f)); // the streets round the player
            const Place p = AtlasOffset(view.map);
            state.cx = view.x + p.dx;
            state.cz = view.z + p.dz;
        }
    }
    if (state.asking == 0) {
        if (state.zoom > 0.0f) {
            MoveView(frame);
            // Kaniel 2026-10-19 ("just one picture"): once the middle leaves the named area for
            // another's ground, that one is named and listed.
            const auto under = [&](uint32_t map, const BbMapArt::Chart& chart) {
                const Place p = AtlasOffset(map);
                const auto [px, py] = chart.PixelOf(state.cx - p.dx, state.cz - p.dz);
                return px >= 0 && py >= 0 && px < float(chart.width) && py < float(chart.height) &&
                       chart.rgba[(size_t(py) * size_t(chart.width) + size_t(px)) * 4 + 3] != 0;
            };
            const ChartTexture* named = Shown();
            if (named == nullptr || !under(state.shown, *named->chart)) {
                for (const auto& [map, c] : charts) {
                    if (map != state.shown && under(map, *c.chart)) {
                        state.shown = map;
                        state.focus = 0;
                        break;
                    }
                }
            }
        }
        // Kaniel 2026-10-24: Square (or a right click on the picture, not a drag) pings the spot
        // under the cursor; the player's own spot moved to Options (C stays).
        if (BbSettings::Get().team_pings) {
            const bool right_click = ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                                     io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < S(6.0f) * S(6.0f) &&
                                     io.MousePos.x >= frame.p0.x && io.MousePos.x < frame.p1.x &&
                                     io.MousePos.y >= frame.p0.y && io.MousePos.y < frame.p1.y;
            if (Pressed({ImGuiKey_GamepadFaceLeft}) || right_click) {
                PingAt(frame, right_click ? io.MousePos : state.cursor);
            }
        }
        if (Pressed({ImGuiKey_GamepadStart, ImGuiKey_C}) && view.has_me && view.map != 0) {
            const Place p = AtlasOffset(view.map);
            state.cx = view.x + p.dx;
            state.cz = view.z + p.dz;
        }
        if (Pressed({ImGuiKey_GamepadR3, ImGuiKey_F}) && view.has_me) {
            state.floors = std::isnan(drawn_floors) ? 2 : 1;
        }
        // Kaniel 2026-10-19 ("3d map ... as an optional feature"): L3 (or V) turns the 3D view on
        // and off; it stays as left for the next time.
        if (Pressed({ImGuiKey_GamepadL3, ImGuiKey_V})) {
            if (BbSettings::Get().map_3d || state.three_d) {
                state.three_d = !state.three_d;
                three_d_kept = state.three_d;
                std::printf("Map: %s view\n", state.three_d ? "3D" : "flat");
            } else {
                state.notice = T("Turn on the 3D map in the settings (Insert, Display)",
                                 "Включите 3D-карту в настройках (Insert, Изображение)");
                state.notice_until = ImGui::GetTime() + 3.0;
            }
        }
        // Kaniel 2026-10-17 ("one big map"), 2026-10-19 ("just one picture"): Triangle shows every
        // world drawn so far.
        if (Pressed({ImGuiKey_GamepadFaceUp, ImGuiKey_Tab}) && !charts.empty()) {
            const auto [x0, x1, z0, z1] = AtlasExtent();
            state.zoom = std::min((frame.p1.x - frame.p0.x) / std::max(1.0f, x1 - x0),
                                  (frame.p1.y - frame.p0.y) / std::max(1.0f, z1 - z0)) * 0.95f;
            state.zoom = std::max(state.zoom, S(0.05f));
            state.cx = (x0 + x1) * 0.5f;
            state.cz = (z0 + z1) * 0.5f;
        }
        if (!lamps.empty() && (up || down || left || right)) {
            state.focus = std::clamp(state.focus + ((down || right) ? 1 : -1), 0, int(lamps.size()) - 1);
            const Place p = AtlasOffset(lamps[size_t(state.focus)]->map);
            state.cx = lamps[size_t(state.focus)]->x + p.dx; // the picture follows the focused lamp
            state.cz = lamps[size_t(state.focus)]->z + p.dz;
            state.cursor = frame.Mid(); // and the cursor sits on it
        }
        MoveCursor(frame);
        // Cross (or a click on the picture): the lamp under the cursor, else the list's focused one.
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && state.hover != 0 &&
                             io.MousePos.x >= frame.p0.x && io.MousePos.x < frame.p1.x &&
                             io.MousePos.y >= frame.p0.y && io.MousePos.y < frame.p1.y;
        if ((cross || clicked) && state.hover != 0) {
            state.asking = state.hover;
            state.ask_yes = true;
        } else if (cross && !lamps.empty()) {
            state.asking = lamps[size_t(state.focus)]->warp_point;
            state.ask_yes = true;
        }
    }
    DrawPicture(draw, frame, lamps, pulse);
    DrawList(draw, ImVec2(frame.p1.x + S(46.0f), top), list_w, bottom, lamps);
    if (!state.notice.empty() && ImGui::GetTime() < state.notice_until) {
        ImGui::PushFont(MenuArt::Font(), 20.0f);
        ts = ImGui::CalcTextSize(state.notice.c_str());
        draw->AddText(ImVec2(mid - ts.x * 0.5f, bottom + S(8.0f)), IM_COL32(220, 150, 110, 255), state.notice.c_str());
        ImGui::PopFont();
    } else {
        DrawKeys(draw, win.x + S(40.0f), win.x + size.x - S(40.0f), win.y + size.y - S(BbKeyGuide::BottomClearance));
    }
    if (state.asking != 0) {
        if (asked_before && (left || right || up || down)) {
            state.ask_yes = !state.ask_yes;
        }
        DrawQuestion(ImGui::GetForegroundDrawList(), display, asked_before && cross, asked_before && circle);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace Map

/// Kaniel 2026-10-25 (the travel together host rule): "Travel with the team?" over the game when
/// this player's lamp or headstone trip waits for it, a member's trip for the team's vote (Yes or
/// No, with the time left), and a short line while this player's vote waits or after a No.
namespace TeamTravel {
using namespace Ui;

bbcoop_team_travel view{}; // present thread: as Visible() read it last
bool shown = false;
bool yes = true;
int question_seen = 0;

bool Read() {
    const int was = view.question;
    shown = bbcoop_team_travel_view(&view) != 0;
    if (!shown) {
        view = {};
    }
    if (view.question != was) {
        yes = true; // a new question starts on Yes
        question_seen = 0;
    }
    return shown;
}

bool Asking() {
    return shown && view.question != 0;
}

std::string LampName() {
    return view.lamp_name_id >= 0 ? BbMapCharts::LampName(view.lamp_name_id)
                                  : std::string{T("Hunter's Dream", "Сон охотника")};
}

void Draw(ImVec2 display) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    char line[160];
    if (!Asking()) {
        // The vote waiting, or why a trip did not go: one line low in the middle.
        if (view.waiting) {
            std::snprintf(line, sizeof line, T("The team votes on the trip: %.0f s", "Команда голосует: %.0f с"),
                          double(view.seconds_left));
        } else if (view.notice == 1) {
            std::snprintf(line, sizeof line, T("%s said no: nobody travels", "%s против: никто не отправляется"),
                          view.from);
        } else if (view.notice == 2) {
            std::snprintf(line, sizeof line, T("%s's trip is off", "Путь %s отменён"), view.from);
        } else {
            return;
        }
        ImGui::PushFont(MenuArt::Font(), 20.0f);
        const ImVec2 ts = ImGui::CalcTextSize(line);
        draw->AddText(ImVec2((display.x - ts.x) * 0.5f, display.y * 0.80f), Plain(0.9f), line);
        ImGui::PopFont();
        return;
    }
    // Keys: left / right (or up / down) pick, Cross chooses, Circle is No. A question that opened
    // this frame takes no keys yet.
    const bool ready = ++question_seen > 1;
    const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
        for (const ImGuiKey key : keys) {
            if (ImGui::IsKeyPressed(key, false)) {
                return true;
            }
        }
        return false;
    };
    if (ready && pressed({ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                          ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                          ImGuiKey_GamepadDpadDown})) {
        yes = !yes;
    }
    bool choose = ready && pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown});
    bool cancel = ready && pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight});
    const ImVec2 size(S(680.0f), S(250.0f));
    const ImVec2 p0((display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f), p1(p0.x + size.x, p0.y + size.y);
    draw->AddRectFilled(ImVec2(0, 0), display, Party::Rgba(0, 0, 0, 0.45f));
    HunterCard(draw, p0, p1, false);
    ImGui::PushFont(MenuArt::Font(), 24.0f);
    const std::string name = LampName();
    ImVec2 ts = ImGui::CalcTextSize(name.c_str());
    draw->AddText(ImVec2((display.x - ts.x) * 0.5f, p0.y + S(40.0f)), Bright(), name.c_str());
    ImGui::PopFont();
    ImGui::PushFont(MenuArt::Font(), 20.0f);
    if (view.question == 1) {
        std::snprintf(line, sizeof line, "%s", T("Travel with the team?", "Отправиться всей командой?"));
    } else {
        std::snprintf(line, sizeof line,
                      T("%s calls the team here. Go? (%.0f s)", "%s зовёт команду сюда. Идти? (%.0f с)"),
                      view.from, double(view.seconds_left));
    }
    ts = ImGui::CalcTextSize(line);
    draw->AddText(ImVec2((display.x - ts.x) * 0.5f, p0.y + S(78.0f)), Plain(), line);
    ImGui::PopFont();
    Divider(draw, p0.x + S(40.0f), p1.x - S(40.0f), p0.y + S(122.0f), 0.7f);
    ImGui::PushFont(MenuArt::Font(), 22.0f);
    const float bw = S(220.0f), bh = S(48.0f), gap = S(30.0f);
    const float bx = (display.x - 2 * bw - gap) * 0.5f, by = p1.y - bh - S(40.0f);
    for (int b = 0; b < 2; ++b) {
        const bool is_yes = b == 0;
        const ImVec2 a(bx + float(b) * (bw + gap), by), end(a.x + bw, a.y + bh);
        MenuArt::Nine(draw, MenuArt::Row, a, end, 12.0f, S(9.0f), 1.0f);
        if (ImGui::IsMouseHoveringRect(a, end)) {
            yes = is_yes;
            choose = choose || (ready && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
        }
        const bool focused = yes == is_yes;
        if (focused) {
            draw->AddRect(ImVec2(a.x + S(2.0f), a.y + S(2.0f)), ImVec2(end.x - S(2.0f), end.y - S(2.0f)),
                          Bright(0.8f), 0.0f, 0, S(1.5f));
        }
        const char* label = view.question == 1 ? (is_yes ? T("With the team", "Всей командой") : T("Alone", "Одному"))
                                               : (is_yes ? T("Yes", "Да") : T("No", "Нет"));
        ts = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(a.x + (bw - ts.x) * 0.5f, a.y + (bh - ts.y) * 0.5f), focused ? Bright() : Plain(), label);
    }
    ImGui::PopFont();
    if (choose || cancel) {
        bbcoop_team_travel_answer(choose && yes ? 1 : 0);
        view.question = 0; // until the next read
    }
}

} // namespace TeamTravel

/// BB_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1
/// (map: M, the map screen; circle, square, triangle), consumed when the file appears (it is removed), one
/// key press per frame.
bool scripted_busy = false; // keys of the file still to press
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
    scripted_busy = !queue.empty();
    if (queue.empty()) {
        return;
    }
    const std::string token = queue.front();
    queue.erase(queue.begin());
    if (token == "toggle") {
        SetOpen(!menu_open);
        return;
    }
    if (token == "map") {
        bbcoop_map_menu(Map::open ? 0 : 2);
        return;
    }
    held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : token == "circle" ? ImGuiKey_GamepadFaceRight
         : token == "square" ? ImGuiKey_GamepadFaceLeft : token == "start" ? ImGuiKey_GamepadStart
         : token == "triangle" ? ImGuiKey_GamepadFaceUp : token == "r3" ? ImGuiKey_GamepadR3
         : token == "l3" ? ImGuiKey_GamepadL3 : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (held != ImGuiKey_None) {
        io.AddKeyEvent(held, true);
        release = true;
    }
}

/// Scripted keys are waiting (their file is there, or a key is still to be pressed or let go):
/// the overlay runs its frames for them even with nothing to draw, or a game that shows nothing
/// (no party, no FPS counter) never opened the menu for the rig.
bool ScriptedKeysWaiting() {
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point last_check{};
    static bool file_there = false;
    if (!path) {
        return false;
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last_check > std::chrono::milliseconds(100)) {
        last_check = now;
        FILE* f = std::fopen(path, "r");
        file_there = f != nullptr;
        if (f) {
            std::fclose(f);
        }
    }
    return file_there || scripted_busy;
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
    // One set per texture: the font atlas, the party art and icons, the team screens' menu art (18).
    info.DescriptorPoolSize = OverlayDescriptorSets;
    // A failed descriptor set allocation leaves the texture with a null set, which the overlay's
    // draw still binds: garbled art, and on NVIDIA a device lost after the presenter's post
    // process (Lisara 2026-10-19, 102 textures in a pool of 64). Said in the log, not silent.
    info.CheckVkResultFn = [](VkResult result) {
        if (result >= 0) {
            return;
        }
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 8) {
            std::printf("Overlay: Vulkan call failed (%d)%s\n", int(result),
                        result == VK_ERROR_OUT_OF_POOL_MEMORY
                            ? ": the texture pool is full, a texture has no descriptor set"
                            : "");
        }
    };
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
    BbMap3d::Init(instance); // the map's live 3D view (used only when it is turned on)
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
    const bool menu_or_quit = menu_open || quit_open;
    // The team screen takes the input too (it has no Circle / Escape: its buttons decide).
    const bool is_open = menu_or_quit || Team::open || Map::open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        // bbport: numpad 0 with Num Lock off is Insert on keyboards without the separate key, and
        // SDL reports it as the keypad key.
        const bool insert = event.key.key == SDLK_INSERT ||
                            (event.key.scancode == SDL_SCANCODE_KP_0 && !(event.key.mod & SDL_KMOD_NUM));
        if (down && !event.key.repeat && (insert || (menu_or_quit && event.key.key == SDLK_ESCAPE))) {
            quit_open = false;
            SetOpen(insert && !menu_or_quit);
            return true;
        }
        // Kaniel 2026-10-24: the ping key (Insert > Display) pings where the camera aims.
        if (down && !event.key.repeat && !is_open && !prompt_active && BbSettings::Get().team_pings &&
            event.key.key == SDL_GetKeyFromName(
                                 BbSettings::PingKeyNames[std::clamp(BbSettings::Get().ping_key.load(), 0,
                                                                     BbSettings::PingKeyCount - 1)])) {
            bbcoop_ping_aim();
            return false; // the game may see it too: no default control is on it
        }
        // Kaniel 2026-10-25: F7 toggles the free camera (its setting allows it, see bbcoop.h).
        if (down && !event.key.repeat && event.key.key == SDLK_F7 && !is_open && !prompt_active &&
            BbSettings::Get().free_camera) {
            bbcoop_free_camera_toggle();
            return true;
        }
        // Kaniel 2026-10-17: M opens the map in the world (the screen closes itself on M).
        if (down && !event.key.repeat && event.key.key == SDLK_M && !is_open && !prompt_active &&
            Map::view.in_world) {
            bbcoop_map_menu(2);
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
                SetOpen(!menu_or_quit);
            }
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (down && button == SDL_GAMEPAD_BUTTON_EAST && menu_or_quit) { // Circle: back to the game
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
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        // Kaniel 2026-10-18 ("no way to scroll the map on the gamepad"): the sticks and triggers as
        // ImGui's analog keys (they never reached it before), a little dead zone in the middle.
        if (!is_open) {
            return false;
        }
        const float v = std::clamp(float(event.gaxis.value) / 32767.0f, -1.0f, 1.0f);
        const auto put = [&](ImGuiKey negative, ImGuiKey positive) {
            const float dead = 0.2f, out = std::abs(v) < dead ? 0.0f : (std::abs(v) - dead) / (1.0f - dead);
            io.AddKeyAnalogEvent(negative, v < 0.0f && out > 0.0f, v < 0.0f ? out : 0.0f);
            io.AddKeyAnalogEvent(positive, v > 0.0f && out > 0.0f, v > 0.0f ? out : 0.0f);
        };
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            put(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            put(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown);
            break;
        case SDL_GAMEPAD_AXIS_RIGHTX:
            put(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight);
            break;
        case SDL_GAMEPAD_AXIS_RIGHTY:
            put(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, v > 0.3f, std::max(0.0f, v));
            break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, v > 0.3f, std::max(0.0f, v));
            break;
        default:
            break;
        }
        return true;
    }
    default:
        return false;
    }
}

bool Visible() {
    // Kaniel 2026-10-25: the free camera's setting, every frame (switched off: back at once).
    bbcoop_set_free_camera(BbSettings::Get().free_camera ? 1 : 0);
    FreeCamera::flying = initialized && bbcoop_free_camera_on() != 0;
    const bool party = initialized && Party::Read(); // read every frame, whatever else is drawn
    const bool pings = initialized && Pings::Read(); // before the compass, which shows them too
    const bool compass = initialized && Compass::Read();
    const bool skip_vote = initialized && SkipVote::Read();
    const bool invasion = initialized && Invasion::Read();
    const bool rot = initialized && Party::ReadRot();
    const bool team_travel = initialized && TeamTravel::Read();
    const bool was_team = Team::open;
    Team::open = initialized && bbcoop_team_screen() != 0 && bbcoop_lobby_view(&Team::lobby) != 0;
    if (!Team::open) {
        Team::friends.open = false; // the team screen opens on the team, not where it was left
        Team::friends.online_typing = false;
    }
    Joining::open = initialized && bbcoop_team_load_view(&Joining::load) != 0;
    loading_cover = Joining::open;
    Map::was_open = Map::open;
    Map::open = initialized && bbcoop_map_screen() != 0;
    if (initialized && (Map::open || Map::was_open || ++Map::shut_reads % 120 == 0)) {
        bbcoop_map_view(&Map::view, Map::open ? 1 : 0); // shut: which map (its picture drawn ahead)
        if (!Map::open && Map::view.in_world && Map::view.map != 0) {
            BbMapCharts::Get(Map::view.map);
        }
    }
    if (Map::open && !Map::was_open) {
        SyncMousePos();
        Map::state.fresh = true;
        Map::rebuild = true;
    }
    if (Team::open && !was_team) {
        SyncMousePos();
        Team::focus.fresh = true;
    }
    return initialized && (menu_open || quit_open || prompt_active || BbSettings::Get().show_fps || party || compass ||
                           pings || skip_vote || invasion || rot || team_travel || Team::open ||
                           Joining::open || Map::open || FreeCamera::flying || ScriptedKeysWaiting());
}

bool MenuOpen() {
    return menu_open || quit_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || quit_open || prompt_active || Team::open || Joining::open || Map::open ||
           TeamTravel::Asking();
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
    // UI scale follows the display height (1080p = 1); on a frame of another shape than 16:9 (21:9,
    // 4:3) the height of the game's 16:9 screen in it, as the game's own HUD is scaled.
    const BbPartyLayout::Rect frame{float(game.offset.x), float(game.offset.y), float(game.extent.width),
                                    float(game.extent.height)};
    const BbPartyLayout::Rect stage = BbPartyLayout::Stage(frame);
    const bool other_shape = game.extent.width > 0 && game.extent.height > 0 &&
                             (stage.w != frame.w || stage.h != frame.h);
    const float scale = BbPartyLayout::Scale(other_shape ? stage.h : float(extent.height));
    // The game's HUD groups at the frame's edges (the aspect hooks): the party HUD follows them,
    // here and in the game's own HUD (the native list, bloodborne_party PartyListPosition).
    const BbGuestHooks::HudShift hud_shift = BbGuestHooks::HudEdgeShift();
    Party::shift = {hud_shift.dx, hud_shift.dy};
    bbcoop_set_hud_shift(hud_shift.dx, hud_shift.dy);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    if (Party::shown || Joining::open || menu_open) {
        Party::LoadGauge(); // before NewFrame, which hands new textures to the backend
    }
    // The party HUD's prompts and the skip vote show the game's button icons from it too.
    if (Team::open || Joining::open || Map::open || menu_open || quit_open || Party::shown || SkipVote::shown) {
        MenuArt::Load(); // the same: the game's menu art and font
    }
    MenuArt::LoadLanguage(); // the same: Japanese, Korean or Chinese glyphs once they are needed
    Map::Refresh(); // the same: the area's picture, the members' portraits
    if (Compass::shown) {
        Compass::RefreshPortraits(); // the same: the markers' portraits
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    // Each texture takes a set from the backend's pool: past it, icons and runes came out garbled
    // (0.2.4). The rig's smoke checks this line against the pool.
    if (const int textures = ImGui::GetPlatformIO().Textures.Size; textures != textures_logged) {
        textures_logged = textures;
        std::printf("Overlay: %d textures, pool of %d sets\n", textures, OverlayDescriptorSets);
    }
    if (Party::shown && !menu_open) {
        Party::game = {float(game.offset.x), float(game.offset.y), float(game.extent.width),
                       float(game.extent.height)};
        Party::Draw();
    } else if (Party::rot_shown && !menu_open && !Team::open && !Joining::open && !Map::open) {
        Party::game = {float(game.offset.x), float(game.offset.y), float(game.extent.width),
                       float(game.extent.height)};
        Party::RotLine(ImGui::GetBackgroundDrawList()); // alone: under the player's own bars
    }
    if (Compass::shown && !menu_open && !quit_open && !Team::open && !Joining::open && !Map::open) {
        Compass::game = {float(game.offset.x), float(game.offset.y), float(game.extent.width),
                         float(game.extent.height)};
        Compass::Draw();
    }
    if (Pings::shown && !menu_open && !quit_open && !Team::open && !Joining::open && !Map::open) {
        Pings::game = {float(game.offset.x), float(game.offset.y), float(game.extent.width),
                       float(game.extent.height)};
        Pings::Draw();
    }
    if (FreeCamera::flying && !menu_open && !quit_open && !Team::open && !Joining::open && !Map::open) {
        FreeCamera::Draw(float(game.offset.x), float(game.offset.y), float(game.extent.width),
                         float(game.extent.height));
    }
    if (SkipVote::shown && !menu_open && !quit_open && !Team::open && !Joining::open) {
        SkipVote::game = {float(game.offset.x), float(game.offset.y), float(game.extent.width),
                          float(game.extent.height)};
        SkipVote::Draw();
    }
    if (Invasion::shown && !menu_open && !quit_open && !Joining::open && !Map::open) {
        Invasion::game = {float(game.offset.x), float(game.offset.y), float(game.extent.width),
                          float(game.extent.height)};
        Invasion::Draw();
    }
    if (Team::open && !menu_open) {
        Team::Screen();
    }
    if (Joining::open && !menu_open) {
        Joining::Screen();
    }
    if (Map::open && !menu_open && !Team::open && !Joining::open) {
        Map::Screen();
    }
    if (TeamTravel::shown && !menu_open && !quit_open && !Team::open && !Joining::open && !Map::open) {
        TeamTravel::Draw(ImGui::GetIO().DisplaySize);
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
    BbMap3d::Draw(cmdbuf); // the map's live 3D picture, which the frame's draw lists show

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
