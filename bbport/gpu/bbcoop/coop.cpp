// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the emulator's seamless co-op code (../emulator/src/core/bloodborne_*.cpp) in the
// native port, through the C interface in gpu/bbcoop.h, and the shadPS4 services it uses that
// this port has no emulator for (the eboot's place, param.sfo, shared trophies).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
#include "bbcoop.h"
#include "bbcoop/bbcoop_internal.h"
#include "bbcoop_revision.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/singleton.h"
#include "core/bloodborne_compass.h"
#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_map.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_portrait.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_team.h"
#include "core/cpu_patches.h"
#include "core/file_format/psf.h"
#include "core/libraries/network/server_link.h"
#include "core/libraries/np/np_trophy.h"

extern "C" int32_t runtime_trophy_unlock_from_party(int32_t trophy_id);

namespace MemoryPatcher {
EXPORT uintptr_t g_eboot_address = 0;
uint64_t g_eboot_image_size = 0;
std::string g_game_serial;
} // namespace MemoryPatcher

namespace Libraries::Np::NpTrophy {
namespace {
std::atomic<TrophyUnlockObserver> unlock_observer{};
} // namespace

void SetTrophyUnlockObserver(TrophyUnlockObserver observer) {
    unlock_observer.store(observer);
}

int UnlockTrophyFromParty(OrbisNpTrophyId trophy_id) {
    return runtime_trophy_unlock_from_party(trophy_id);
}

void NotifyTrophyUnlocked(OrbisNpTrophyId trophy_id) {
    if (const auto observer = unlock_observer.load(); observer != nullptr) {
        observer(trophy_id);
    }
}
} // namespace Libraries::Np::NpTrophy

extern "C" void bbcoop_note_segment(uint64_t address, uint64_t size, unsigned flags) {
    Core::NoteImageSegment(static_cast<uintptr_t>(address), size, flags);
}

namespace {
std::string game_folder; // app0, as bbcoop_install got it
} // namespace

extern "C" int bbcoop_game_file(const char* relative, char* out, size_t size) {
    if (game_folder.empty()) {
        return 0;
    }
    const auto path = (std::filesystem::path{game_folder} / relative).string();
    if (path.size() + 1 > size) {
        return 0;
    }
    std::memcpy(out, path.c_str(), path.size() + 1);
    return 1;
}

extern "C" void bbcoop_install(unsigned char* image, uint64_t eboot_size, void* code,
                               uint64_t code_size, const char* app0) {
    game_folder = app0 != nullptr ? app0 : "";
    auto* param_sfo = Common::Singleton<PSF>::Instance();
    const auto sfo = std::filesystem::path{app0 != nullptr ? app0 : "."} / "sce_sys" / "param.sfo";
    if (!param_sfo->Open(sfo)) {
        LOG_WARNING(Loader, "Co-op: no param.sfo at {}", sfo.string());
    }
    MemoryPatcher::g_game_serial = std::string{param_sfo->GetString("TITLE_ID").value_or("")};
    MemoryPatcher::g_eboot_address = reinterpret_cast<uintptr_t>(image);
    MemoryPatcher::g_eboot_image_size = eboot_size;
    // The build, as the emulator logs it: the rig checks that every game runs the same one.
    LOG_INFO(Loader, "Run: Revision {}", BB_SCM_REV);
    // What a log needs to compare two players' games, in one line: the build, the launcher's frame
    // rate preset and extra patches (patches.py's names; the patches themselves are compiled
    // into patches.bin), and the map event files the game reads (the kit's co-op ones over the
    // game's). The build and the event files also go to the room's peers (PeerHello).
    const u64 event_files = Core::Bloodborne::EventFilesHash(
        std::filesystem::path{game_folder.empty() ? "." : game_folder} / "dvdroot_ps4" / "event");
    Core::Bloodborne::SetBuildIdentity(BB_SCM_REV, event_files);
    const auto environment = [](const char* name, const char* absent) {
        const char* value = std::getenv(name);
        return std::string{value != nullptr && *value != '\0' ? value : absent};
    };
    char started[32] = "?";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", utc);
    }
    LOG_INFO(
        Loader, "Co-op: started {} UTC, build {}, FPS preset {}, extra patches {}, event files {}",
        started, BB_SCM_REV, environment("BB_FPS_PRESET", "?"), environment("BB_PATCHES", "none"),
        event_files != 0 ? fmt::format("{:016x}", event_files) : std::string{"unread"});
    if (code != nullptr) {
        Core::RegisterPatchModule(image, eboot_size, code, code_size);
    }
#ifdef _WIN32
    // The loader's own last-chance handler stays: it writes the co-op crash report itself
    // (bbcoop_crash_report) before it ends the process.
    const auto loader_filter = SetUnhandledExceptionFilter(nullptr);
    Core::Bloodborne::InstallCrashCatcher(reinterpret_cast<u64>(image), eboot_size);
    SetUnhandledExceptionFilter(loader_filter);
#else
    Core::Bloodborne::InstallCrashCatcher(reinterpret_cast<u64>(image), eboot_size);
#endif
    Core::Bloodborne::InstallSeamlessCoopPatches();
    Core::Bloodborne::InstallReverseEngineeringTrace();
}

extern "C" void bbcoop_crash_report(void* exception_pointers) {
    Core::Bloodborne::WriteCrashReport(exception_pointers);
}

extern "C" void bbcoop_window_closing(void) {
    Core::Bloodborne::ReportFreezeOnClose();
}

extern "C" int bbcoop_autoplay_pad(uint32_t* buttons, uint8_t sticks[4]) {
    std::uint32_t held = 0;
    std::array<std::uint8_t, 4> moved{128, 128, 128, 128};
    if (!Core::Bloodborne::AutoplayPad(held, moved)) {
        return 0;
    }
    *buttons = held;
    std::memcpy(sticks, moved.data(), moved.size());
    return 1;
}

extern "C" void bbcoop_party_death_pad(uint32_t* buttons, uint8_t sticks[4], uint8_t* l2,
                                       uint8_t* r2) {
    std::array<std::uint8_t, 4> moved{};
    std::memcpy(moved.data(), sticks, moved.size());
    Core::Bloodborne::FilterPartyDeathPad(*buttons, moved, *l2, *r2);
    std::memcpy(sticks, moved.data(), moved.size());
}

static_assert(BBCOOP_VITAL_OFF == static_cast<int>(Core::Bloodborne::Vital::Off) &&
              BBCOOP_VITAL_UP == static_cast<int>(Core::Bloodborne::Vital::Up) &&
              BBCOOP_VITAL_DOWN == static_cast<int>(Core::Bloodborne::Vital::Down) &&
              BBCOOP_VITAL_DEAD == static_cast<int>(Core::Bloodborne::Vital::Dead));

extern "C" int bbcoop_party_panel(bbcoop_panel* panel) {
    *panel = bbcoop_panel{.revive_target = -1};
    Core::Bloodborne::PartyPanel party;
    if (!Core::Bloodborne::ReadPartyPanel(party)) {
        return 0;
    }
    const auto count = std::min<std::size_t>(party.members.size(), BBCOOP_PANEL_MEMBERS);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& member = party.members[index];
        auto& out = panel->members[index];
        // Phase 11: the character's name when the server knows it, else "" (the overlay says
        // Hunter); never the online id.
        std::snprintf(out.name, sizeof(out.name), "%s", member.shown.c_str());
        out.vital = static_cast<int>(member.vital);
        out.here = member.here ? 1 : 0;
        out.hp = member.hp;
        out.hp_max = member.max;
        out.landed = member.landed;
        out.needed = member.needed;
        out.vials = member.vials;
        out.bullets = member.bullets;
    }
    panel->member_count = static_cast<int>(count);
    panel->revive_target = party.revive_target < static_cast<s32>(count) ? party.revive_target : -1;
    panel->vital = static_cast<int>(party.vital);
    panel->landed = party.landed;
    panel->needed = party.needed;
    panel->give_up = party.give_up;
    panel->native_list = Core::Bloodborne::NativePartyListShown() ? 1 : 0;
    panel->native_prompt = Core::Bloodborne::NativePartyPromptShown() ? 1 : 0;
    panel->native_supplies = Core::Bloodborne::NativePartySuppliesShown() ? 1 : 0;
    return 1;
}

extern "C" int bbcoop_compass_view(bbcoop_compass* compass) {
    *compass = {};
    Core::Bloodborne::CompassView view;
    if (!Core::Bloodborne::ReadCompass(view)) {
        return 0;
    }
    compass->heading = view.heading;
    for (size_t i = 0; i < view.markers.size() && i < BBCOOP_COMPASS_MARKERS; ++i) {
        const auto& marker = view.markers[i];
        auto& out = compass->markers[compass->count++];
        std::snprintf(out.name, sizeof(out.name), "%s", view.names[i].c_str());
        out.member = marker.member;
        out.bearing = marker.bearing;
        out.distance = marker.distance;
        out.x = marker.place.x;
        out.pinned = marker.place.pinned ? 1 : 0;
        std::snprintf(out.board, sizeof(out.board), "%s", view.members[i].c_str());
        out.vital = view.vitals[i];
        out.revive = view.revives[i];
    }
    if (view.stain) {
        compass->has_stain = 1;
        compass->stain.bearing = view.stain->bearing;
        compass->stain.distance = view.stain->distance;
        compass->stain.x = view.stain->place.x;
        compass->stain.pinned = view.stain->place.pinned ? 1 : 0;
    }
    return compass->count > 0 || compass->has_stain ? 1 : 0;
}

extern "C" int bbcoop_map_view(bbcoop_map* map, int open) {
    using namespace Core::Bloodborne;
    *map = {};
    MapScreenView view;
    if (!ReadMapScreen(view, open != 0)) {
        return 0;
    }
    map->in_world = view.in_world ? 1 : 0;
    map->map = view.map;
    map->area_name_id = AreaNameId(view.map);
    if (view.me) {
        map->has_me = 1;
        map->x = (*view.me)[0];
        map->y = (*view.me)[1];
        map->z = (*view.me)[2];
    }
    if (view.heading) {
        map->has_heading = 1;
        map->heading = *view.heading;
    }
    const MapTravelBlock block = MapTravelCheck({.lit = true,
                                                 .in_world = view.in_world,
                                                 .down = view.down,
                                                 .boss_fight = view.boss_fight,
                                                 .travelling = view.travelling});
    map->block = static_cast<int>(block); // the enums' orders agree (static_asserts below)
    PartyPanel panel;
    ReadPartyPanel(panel);
    for (const PanelMember& member : panel.members) {
        if (map->member_count >= BBCOOP_MAP_MEMBERS) {
            break;
        }
        bbcoop_map_member& out = map->members[map->member_count++];
        std::snprintf(out.name, sizeof(out.name), "%s", member.shown.c_str()); // "": Hunter
        std::snprintf(out.board, sizeof(out.board), "%s", member.member.c_str());
        out.map = member.map;
        out.vital = static_cast<int>(member.vital);
        if (member.at) {
            out.has_at = 1;
            out.x = (*member.at)[0];
            out.y = (*member.at)[1];
            out.z = (*member.at)[2];
        }
    }
    // Kaniel 2026-10-20: this player's own bloodstain, on its own map (only its own: Kaniel after
    // v1.0.2).
    if (const Bloodstain& stain = panel.own_stain; stain.map != 0) {
        map->has_stain = 1;
        map->stain = {.map = stain.map,
                      .x = stain.at[0],
                      .y = stain.at[1],
                      .z = stain.at[2],
                      .echoes = stain.echoes};
    }
    for (const u32 area : view.cleared) {
        if (map->cleared_count < 24) {
            map->cleared[map->cleared_count++] = area;
        }
    }
    for (const MapLamp& lamp : MapLamps()) {
        if (map->lamp_count >= BBCOOP_MAP_LAMPS) {
            break;
        }
        bbcoop_map_lamp& out = map->lamps[map->lamp_count++];
        out.warp_point = lamp.warp_point;
        out.map = lamp.packed_map;
        out.x = lamp.x;
        out.y = lamp.y;
        out.z = lamp.z;
        out.lit = std::ranges::find(view.lit, lamp.warp_point) != view.lit.end() ? 1 : 0;
        out.name_id = LampNameId(lamp.warp_point);
    }
    return 1;
}
static_assert(BBCOOP_MAP_GO == int(Core::Bloodborne::MapTravelBlock::None) &&
              BBCOOP_MAP_NOT_LIT == int(Core::Bloodborne::MapTravelBlock::NotLit) &&
              BBCOOP_MAP_NOT_IN_WORLD == int(Core::Bloodborne::MapTravelBlock::NotInWorld) &&
              BBCOOP_MAP_DOWN == int(Core::Bloodborne::MapTravelBlock::Down) &&
              BBCOOP_MAP_BOSS == int(Core::Bloodborne::MapTravelBlock::BossFight) &&
              BBCOOP_MAP_TRAVELLING == int(Core::Bloodborne::MapTravelBlock::Travelling));

extern "C" void bbcoop_map_travel(int32_t warp_point) {
    Core::Bloodborne::RequestMapTrip(warp_point);
}

extern "C" int bbcoop_map_explored(uint32_t map, int16_t* xz, int max) {
    static_assert(BBCOOP_MAP_CELL == int(Core::Bloodborne::ExploredCellMetres));
    const auto cells = Core::Bloodborne::ExploredOn(map);
    int count = 0;
    for (const auto& cell : cells) {
        if (count >= max) {
            break;
        }
        xz[count * 2] = cell[0];
        xz[count * 2 + 1] = cell[1];
        ++count;
    }
    return count;
}

extern "C" int32_t bbcoop_map_area_name_id(uint32_t map) {
    return Core::Bloodborne::AreaNameId(map);
}

static_assert(sizeof(bbcoop_camera) == sizeof(Core::Bloodborne::GameCamera));

extern "C" void bbcoop_map_overhead(int on, float x, float y, float z, float distance, float heading,
                                    float pitch) {
    using namespace Core::Bloodborne;
    SetOverheadView(on != 0 ? std::optional<OverheadView>{OverheadView{.x = x,
                                                                      .y = y,
                                                                      .z = z,
                                                                      .distance = distance,
                                                                      .heading = heading,
                                                                      .pitch = pitch}}
                            : std::nullopt);
}

extern "C" int bbcoop_map_camera(bbcoop_camera* camera) {
    const auto drawn = Core::Bloodborne::DrawnGameCamera();
    if (!drawn) {
        return 0;
    }
    std::memcpy(camera, &*drawn, sizeof(*camera));
    return 1;
}

extern "C" int bbcoop_camera_project(const bbcoop_camera* camera, float x, float y, float z, float at[2]) {
    Core::Bloodborne::GameCamera game;
    std::memcpy(&game, camera, sizeof(game));
    const auto seen = Core::Bloodborne::ProjectWithCamera(game, x, y, z);
    if (!seen) {
        return 0;
    }
    at[0] = (*seen)[0];
    at[1] = (*seen)[1];
    return 1;
}

static_assert(BBCOOP_PORTRAIT_SIDE == Core::Bloodborne::PortraitSide);

extern "C" int bbcoop_member_portrait(const char* member, uint8_t* rgba, size_t size, uint32_t* id) {
    uint32_t seen = 0;
    const bool shown = member != nullptr &&
                       Core::Bloodborne::ReadMemberPortrait(member, std::span<uint8_t>{rgba, size}, seen);
    if (id != nullptr) {
        *id = seen;
    }
    return shown ? 1 : 0;
}

extern "C" float bbcoop_revive_share(uint32_t landed, uint32_t needed) {
    return Core::Bloodborne::ReviveShare(landed, needed);
}

extern "C" void bbcoop_portrait_model(const uint8_t* rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return;
    }
    Core::Bloodborne::NoteStatusModel(
        std::span<const uint8_t>{rgba, std::size_t(width) * std::size_t(height) * 4}, width, height);
}

extern "C" int bbcoop_portrait_offscreen_wanted(void) {
    return Core::Bloodborne::OffscreenPortraitWanted() ? 1 : 0;
}

extern "C" uint32_t bbcoop_portrait_version(int slot) {
    return Core::Bloodborne::PortraitSlotVersion(slot);
}

extern "C" int bbcoop_portrait_slot(int slot, uint8_t* rgba, size_t size, uint32_t* version) {
    uint32_t seen = 0;
    const bool shown = Core::Bloodborne::ReadPortraitSlot(slot, std::span<uint8_t>{rgba, size}, seen);
    if (version != nullptr) {
        *version = seen;
    }
    return shown ? 1 : 0;
}

namespace {

void CopyName(char* out, size_t size, const std::string& text) {
    const size_t n = std::min(size - 1, text.size());
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
}

int LobbyStateOf(bbcoop::LobbyPlayer::State state) {
    return state == bbcoop::LobbyPlayer::PLAYING ? BBCOOP_LOBBY_PLAYING
           : state == bbcoop::LobbyPlayer::TITLE ? BBCOOP_LOBBY_TITLE
                                                 : BBCOOP_LOBBY_OFFLINE;
}

} // namespace

extern "C" int bbcoop_lobby_view(bbcoop_lobby* lobby) {
    namespace Coop = Core::Bloodborne::Coop;
    *lobby = {};
    if (!Coop::TeamModeOn()) {
        return 0;
    }
    const auto state = Coop::TeamLobbyState();
    if (!state) {
        // Refused (another build): no lobby comes, so the screen says why instead of "reaching".
        CopyName(lobby->notice, sizeof(lobby->notice),
                 Core::Bloodborne::LinkRefusalNotice(Libraries::Net::ServerLinkRefusal()));
        return 1;
    }
    const std::string me = Coop::TeamLobbyMember();
    CopyName(lobby->me, sizeof(lobby->me), me);
    lobby->signed_in = 1;
    lobby->menu = Coop::TeamMenuOpen() ? 1 : 0;
    bool in_hunt = false;
    lobby->playing = Coop::TeamPlaying(&in_hunt) ? 1 : 0;
    lobby->in_hunt = in_hunt ? 1 : 0;
    CopyName(lobby->notice, sizeof(lobby->notice), state->notice());
    if (state->has_team()) {
        const bbcoop::Team& team = state->team();
        lobby->in_team = 1;
        lobby->leader = team.leader() == me;
        lobby->started = team.started();
        // The leader first.
        std::vector<const bbcoop::TeamMember*> members;
        for (const bbcoop::TeamMember& m : team.members()) {
            if (m.member() == team.leader()) {
                members.insert(members.begin(), &m);
            } else {
                members.push_back(&m);
            }
        }
        for (const bbcoop::TeamMember* m : members) {
            if (lobby->member_count == BBCOOP_LOBBY_TEAM) {
                break;
            }
            bbcoop_lobby_entry& out = lobby->members[lobby->member_count++];
            CopyName(out.name, sizeof(out.name), m->name());
            CopyName(out.id, sizeof(out.id), m->member());
            out.state = LobbyStateOf(m->state());
            out.ready = m->ready();
        }
    }
    // Kaniel 2026-10-17: the server's worlds (any team's to pick, the leader's or, before a team, the
    // player's own pick), as the game's Load Game lists its saves.
    const std::string pick = state->has_team() ? state->team().world() : state->world();
    const std::string own_team = state->has_team() ? state->team().id() : std::string{};
    lobby->world = -1;
    const auto local_time = [](int64_t ms, char* out, size_t size, const char* format) {
        if (ms <= 0) {
            return;
        }
        const std::time_t at = static_cast<std::time_t>(ms / 1000);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &at);
#else
        localtime_r(&at, &local);
#endif
        std::strftime(out, size, format, &local);
    };
    for (const bbcoop::TeamWorldInfo& w : state->worlds()) {
        if (lobby->world_count == BBCOOP_LOBBY_WORLDS) {
            break;
        }
        if (w.id() == pick) {
            lobby->world = lobby->world_count;
        }
        bbcoop_lobby_world& out = lobby->worlds[lobby->world_count++];
        CopyName(out.id, sizeof(out.id), w.id());
        CopyName(out.name, sizeof(out.name), Core::Bloodborne::TeamWorldName(w));
        out.lamps = static_cast<int>(w.lamps());
        local_time(w.played_ms(), out.when, sizeof(out.when), "%d %b %H:%M");
        local_time(w.created_ms(), out.created, sizeof(out.created), "%d %b %Y");
        std::string hunters;
        for (const std::string& name : w.characters()) {
            hunters += (hunters.empty() ? "" : ", ") + name;
        }
        CopyName(out.characters, sizeof(out.characters), hunters);
        out.busy = !w.played_by().empty() && w.played_by() != own_team;
    }
    for (const bbcoop::LobbyPlayer& player : state->players()) {
        const bool mine = state->has_team() && player.team() == state->team().id();
        if (mine || lobby->player_count == BBCOOP_LOBBY_PLAYERS) {
            continue;
        }
        bbcoop_lobby_entry& out = lobby->players[lobby->player_count++];
        CopyName(out.name, sizeof(out.name), player.name());
        CopyName(out.id, sizeof(out.id), player.member());
        out.state = LobbyStateOf(player.state());
    }
    for (const bbcoop::TeamInvite& invite : state->invites()) {
        if (lobby->invite_count == BBCOOP_LOBBY_INVITES) {
            break;
        }
        bbcoop_lobby_entry& out = lobby->invites[lobby->invite_count++];
        CopyName(out.name, sizeof(out.name), invite.from_name());
        CopyName(out.id, sizeof(out.id), invite.team());
    }
    return 1;
}

static_assert(BBCOOP_TEAM_GO_START == int(Core::Bloodborne::TeamGate::Choice::Start) &&
              BBCOOP_TEAM_GO_ALONE == int(Core::Bloodborne::TeamGate::Choice::Alone));

extern "C" int bbcoop_team_screen(void) {
    return Core::Bloodborne::Coop::TeamScreenUp() ? 1 : 0;
}

extern "C" int bbcoop_team_load_view(bbcoop_team_load* view) {
    const Core::Bloodborne::TeamLoad::View load = Core::Bloodborne::Coop::TeamLoadView();
    if (!load.up || view == nullptr) {
        return 0;
    }
    *view = {};
    for (const auto& row : load.rows) {
        if (view->count >= 4) {
            break;
        }
        bbcoop_team_load_row& out = view->rows[view->count++];
        std::snprintf(out.name, sizeof(out.name), "%s", row.name.c_str());
        out.stage = static_cast<int>(row.stage);
        out.percent = static_cast<int>(row.percent);
        out.self = row.self ? 1 : 0;
    }
    return 1;
}

extern "C" int bbcoop_sound_held(void) {
    return Core::Bloodborne::Coop::TeamLoadCovers() ? 1 : 0;
}

extern "C" int bbcoop_cutscene_skip(void) {
    return Core::Bloodborne::Coop::CutsceneSkipPressed() ? 1 : 0;
}

extern "C" int bbcoop_cutscene_vote_pad(int options) {
    using Pad = Core::Bloodborne::CutsceneVote::Pad;
    switch (Core::Bloodborne::Coop::CutsceneVotePad(options != 0)) {
    case Pad::Hide:
        return 1;
    case Pad::Press:
        return 2;
    case Pad::Leave:
        break;
    }
    return 0;
}

extern "C" int bbcoop_cutscene_vote(int* want, int* watching) {
    std::uint32_t voted = 0;
    std::uint32_t all = 0;
    const bool shown = Core::Bloodborne::Coop::CutsceneVoteView(voted, all);
    *want = static_cast<int>(voted);
    *watching = static_cast<int>(all);
    return shown ? 1 : 0;
}

extern "C" void bbcoop_team_menu(int open) {
    Core::Bloodborne::Coop::TeamMenu(open != 0, open == 1);
}

extern "C" int bbcoop_team_world_list_asked(void) {
    return Core::Bloodborne::Coop::TeamWorldListAsked() ? 1 : 0;
}

namespace {
std::atomic<int> map_menu{}; // how the map screen was opened (bbcoop_map_menu), 0 shut
std::mutex map_back_mutex;
Core::Bloodborne::TeamMenuBack map_back;
} // namespace

extern "C" void bbcoop_map_menu(int open) {
    const int was = map_menu.exchange(open);
    if (open == 0 && was != 0) {
        std::scoped_lock lock{map_back_mutex};
        map_back.Closed(Core::Bloodborne::Coop::SteadyMilliseconds(), was == 1);
    }
}

extern "C" void bbcoop_map_from_pad(void) {
    Core::Bloodborne::MapScreenView view;
    if (map_menu.load() == 0 && Core::Bloodborne::ReadMapScreen(view, false) && view.in_world) {
        bbcoop_map_menu(2);
    }
}

extern "C" int bbcoop_map_screen(void) {
    return map_menu.load() != 0 ? 1 : 0;
}

extern "C" int bbcoop_team_menu_back(void) {
    bool map_page = false;
    {
        std::scoped_lock lock{map_back_mutex};
        map_page = map_back.Pressed(Core::Bloodborne::Coop::SteadyMilliseconds());
    }
    return Core::Bloodborne::Coop::TeamMenuBackPressed() || map_page ? 1 : 0;
}

extern "C" void bbcoop_team_join_hunt(const char* team) {
    Core::Bloodborne::Coop::TeamJoinHunt(team ? team : "");
}

extern "C" void bbcoop_team_choose(int what) {
    Core::Bloodborne::Coop::TeamChoose(static_cast<Core::Bloodborne::TeamGate::Choice>(what));
}

static_assert(BBCOOP_TEAM_WORLD == bbcoop::TeamRequest::WORLD &&
              BBCOOP_TEAM_FORGET_WORLD == bbcoop::TeamRequest::FORGET_WORLD);

extern "C" void bbcoop_lobby_request(int action, const char* id) {
    Core::Bloodborne::Coop::TeamLobbyRequest(static_cast<bbcoop::TeamRequest::Action>(action),
                                             id ? id : "");
}

extern "C" void bbcoop_file_opened(const char* guest_path) {
    Core::Bloodborne::NoteSaveFileOpened(guest_path);
}

extern "C" void bbcoop_trophy_unlocked(int32_t trophy_id) {
    Libraries::Np::NpTrophy::NotifyTrophyUnlocked(trophy_id);
}
