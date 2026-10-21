// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kaniel 2026-10-17: the port's map screen (bloodborne_map.h) on the game thread: what it shows
// (where this player stands, the lamps lit) read while it is open, and a trip to a lamp from it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/bloodborne_lamp_points.h"
#include "core/bloodborne_map.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/cpu_patches.h"

namespace Core::Bloodborne {

using namespace Coop;

namespace {

using Clock = std::chrono::steady_clock;

// Boss fights: on while one is (the party controller's boss sequence asks it too).
constexpr s32 BossFightFlag = 4000;

std::atomic<bool> map_screen_open{};
std::atomic<s32> map_trip_request{};
std::mutex map_view_mutex;
MapScreenView map_view_now;
bool map_view_read = false;

// A trip from the map in flight (not a party guest's: TickGuestLampTrip makes those).
struct MapTrip {
    bool active = false;
    s32 point = 0;
    const LampPoint* planned = nullptr;
    MapTripProgress progress;
    Clock::time_point started{};
    std::optional<Clock::time_point> fix_at;
};
MapTrip map_trip;

// Kaniel 2026-10-18 (fog of war): the cells this character has walked, kept in the user folder by
// its save slot (bloodborne_party_explored_slotN.txt), written a while after they change.
std::mutex explored_mutex;
std::set<ExploredCell> explored;
int explored_slot = -2; // the slot the set was loaded for
bool explored_dirty = false;
Clock::time_point explored_saved_at{};

std::filesystem::path ExploredFile(int slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / SlotFileName("explored", slot);
}

void SaveExploredNow() {
    if (explored_slot < 0) {
        return;
    }
    const auto bytes = SaveExplored(std::vector<ExploredCell>(explored.begin(), explored.end()));
    std::ofstream(ExploredFile(explored_slot), std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    explored_dirty = false;
    explored_saved_at = Clock::now();
}

void NoteExplored(u32 map, const LampPosition& at) {
    const int slot = character_slot.load();
    std::scoped_lock lock{explored_mutex};
    if (slot != explored_slot) {
        if (explored_dirty) {
            SaveExploredNow();
        }
        explored.clear();
        explored_slot = slot;
        if (slot >= 0) {
            std::ifstream file(ExploredFile(slot), std::ios::binary);
            const std::vector<u8> bytes{std::istreambuf_iterator<char>(file), {}};
            if (const auto cells = LoadExplored(bytes)) {
                explored.insert(cells->begin(), cells->end());
            }
        }
    }
    if (slot < 0 || map == 0) {
        return;
    }
    explored_dirty = explored.insert(ExploredCellAt(map, at[0], at[2])).second || explored_dirty;
    if (explored_dirty && Clock::now() - explored_saved_at > std::chrono::seconds{20}) {
        SaveExploredNow();
    }
}

bool Loading() {
    const u64 global = GetGlobalState();
    return global < 0x10000 || TryRead<u8>(global, 0x08).value_or(1) != 0;
}

// Kaniel 2026-10-21 (the overhead map): the camera the game made this frame (GameCamera), read in
// the frame step once its camera manager has updated (the hook at OverheadCameraHookOffset, r13 the
// step, its camera manager at +0x128), and the view the map wants drawn instead; the camera the
// frame was then drawn with. A view not asked again for OverheadViewHold is dropped (the overlay
// went, or stopped asking): the game's own camera is never left moved.
constexpr u64 StepCameraManagerField = 0x128;
static_assert(sizeof(GameCamera) == 0x50);
std::mutex camera_mutex;
std::optional<GameCamera> camera_drawn;
std::optional<OverheadView> overhead_wanted;
Clock::time_point overhead_asked_at{};
constexpr auto OverheadViewHold = std::chrono::milliseconds{750};

void OverheadCameraStepNow(const GuestRegisterSnapshot* registers) {
    const u64 manager = ReadValue<u64>(registers->r13, StepCameraManagerField);
    if (manager < 0x10000 ||
        !HasMemoryAccess(manager + GameCameraAt, sizeof(GameCamera), MemoryProt::CpuReadWrite)) {
        return;
    }
    GameCamera game;
    std::memcpy(&game, reinterpret_cast<const void*>(manager + GameCameraAt), sizeof(game));
    if (!LooksLikeGameCamera(game)) {
        return;
    }
    std::optional<OverheadView> wanted;
    {
        std::scoped_lock lock{camera_mutex};
        if (overhead_wanted && Clock::now() - overhead_asked_at > OverheadViewHold) {
            overhead_wanted.reset();
        }
        wanted = overhead_wanted;
    }
    // Taken and given back: logged (the rig's map-overhead checks both).
    static bool was_wanted = false;
    if (wanted.has_value() != was_wanted) {
        was_wanted = wanted.has_value();
        if (wanted) {
            LOG_INFO(
                Debug,
                "Bloodborne map screen: the game camera over ({:.1f}, {:.1f}, {:.1f}), {:.0f} m "
                "away, pitch {:.2f}",
                wanted->x, wanted->y, wanted->z, wanted->distance, wanted->pitch);
        } else {
            LOG_INFO(Debug, "Bloodborne map screen: the game camera is the game's again");
        }
    }
    const GameCamera drawn = wanted ? OverheadCamera(game, *wanted) : game;
    if (wanted) {
        std::memcpy(reinterpret_cast<void*>(manager + GameCameraAt), &drawn, sizeof(drawn));
    }
    std::scoped_lock lock{camera_mutex};
    camera_drawn = drawn;
}

bool LocalPlayerDown() {
    const u64 player = GetLocalPlayer();
    const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
    const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
    if (data >= 0x10000 && ReadValue<s32>(data, 0xF8) <= 0) {
        return true;
    }
    PartyPanel panel;
    ReadPartyPanel(panel);
    return panel.vital == Vital::Down || panel.vital == Vital::Dead;
}

bool Travelling() {
    return map_trip.active || PartyTripActive() || guest_lamp_trip_at.has_value() ||
           guest_trip_in_flight;
}

// The trip's arrival: the warp alone can leave the player over the void, so a second after the
// map is in it goes to the lamp, as the rig's travel does (FixAutoplayTravelArrival); the lamp
// travelled to is the respawn point from then on.
void TickMapTripArrival(bool loading) {
    if (!map_trip.active) {
        return;
    }
    const auto now = Clock::now();
    map_trip.progress.load_seen = map_trip.progress.load_seen || loading;
    map_trip.progress.seconds = std::chrono::duration<double>(now - map_trip.started).count();
    if (map_trip.progress.seconds > 180.0) {
        LOG_WARNING(Debug, "Bloodborne map screen: the trip to {} never arrived", map_trip.point);
        map_trip = {};
        return;
    }
    if (!MapTripArrived(map_trip.progress, GetCurrentPackedMap(), loading || PartyTripActive()) ||
        !GameHudUp()) {
        return;
    }
    if (!map_trip.fix_at) {
        map_trip.fix_at = now + std::chrono::seconds{1};
        return;
    }
    if (now < *map_trip.fix_at) {
        return;
    }
    if (const u64 global = GetGlobalState(); global >= 0x10000) {
        WriteValue(global, 0x1528, map_trip.point);
    }
    const auto at = LocalPlayerPosition();
    const bool move = ArrivalNeedsMove(map_trip.planned, at);
    LOG_INFO(Debug, "Bloodborne map screen: arrived at {} ({}, {}, {}), moved to the lamp={}",
             map_trip.point, at ? (*at)[0] : 0.0f, at ? (*at)[1] : 0.0f, at ? (*at)[2] : 0.0f,
             move);
    if (move) {
        MoveToRespawnPoint(true);
    }
    map_trip = {};
}

void StartMapTrip(s32 point) {
    if (IsPartyGuestRole() && InPartySession()) {
        // A guest's own lamp trip, as from a lamp (the session's guest trip and its arrival).
        guest_trip_point = point;
        guest_lamp_trip_at = Clock::now();
        LOG_INFO(Debug, "Bloodborne map screen: party guest travels to {}", point);
        return;
    }
    map_trip = {};
    map_trip.active = true;
    map_trip.point = point;
    map_trip.progress.target_map = PackedMapOfWarpPoint(point);
    map_trip.started = Clock::now();
    map_trip.planned = StartTripToLamp(point);
    LOG_INFO(Debug, "Bloodborne map screen: travels to {} ({:#x}), at the lamp={}", point,
             map_trip.progress.target_map, map_trip.planned != nullptr);
}

MapScreenView ReadMapScreenNow(bool loading) {
    MapScreenView view;
    view.in_world = !loading && GameHudUp() && GetLocalPlayer() >= 0x10000;
    view.travelling = Travelling();
    if (view.in_world) {
        view.map = PlayingMap();
        view.me = LocalPlayerPosition();
        view.heading = CameraHeadingNow();
        // Game flags only here, in the world with the HUD up (never every frame of a load).
        for (const MapLamp& lamp : MapLamps()) {
            const auto flag = LampActiveFlag(lamp.warp_point);
            if (!flag || EventFlagOn(*flag)) {
                view.lit.push_back(lamp.warp_point);
            }
        }
        view.down = LocalPlayerDown();
        view.boss_fight = EventFlagOn(BossFightFlag);
        if (view.me) {
            NoteExplored(view.map, *view.me);
        }
        // The areas whose main bosses are all down: the map shows the whole of them.
        for (const MapLamp& lamp : MapLamps()) {
            const u32 area = lamp.packed_map;
            const auto flags = AreaBossFlags(area);
            if (!flags.empty() && std::ranges::find(view.cleared, area) == view.cleared.end() &&
                std::ranges::all_of(flags, [](s32 flag) { return EventFlagOn(flag); })) {
                view.cleared.push_back(area);
            }
        }
    }
    return view;
}

} // namespace

namespace Coop {

void TickMapScreen() {
    const bool open = map_screen_open.load(std::memory_order_relaxed);
    const s32 request = map_trip_request.exchange(0);
    static Clock::time_point read_at{};
    const auto now = Clock::now();
    if (!open && request == 0) {
        TickMapTripArrival(map_trip.active && Loading());
        // Shut: only which map this player is on, every few seconds (the overlay draws its picture
        // ahead), and no game flags.
        if (now - read_at >= std::chrono::seconds{1}) {
            read_at = now;
            const bool in_world = !Loading() && GameHudUp() && GetLocalPlayer() >= 0x10000;
            const u32 map = in_world ? PlayingMap() : 0;
            if (in_world) {
                if (const auto at = LocalPlayerPosition()) {
                    NoteExplored(map, *at);
                }
            }
            std::scoped_lock lock{map_view_mutex};
            map_view_now.in_world = in_world;
            map_view_now.map = map;
            map_view_read = true;
        }
        return;
    }
    const bool loading = Loading();
    TickMapTripArrival(loading);
    if (request == 0 && now - read_at < std::chrono::milliseconds{400}) {
        return;
    }
    read_at = now;
    MapScreenView view = ReadMapScreenNow(loading);
    if (request != 0) {
        const bool lit = std::ranges::find(view.lit, request) != view.lit.end();
        const MapTravelBlock block = MapTravelCheck({.lit = lit,
                                                     .in_world = view.in_world,
                                                     .down = view.down,
                                                     .boss_fight = view.boss_fight,
                                                     .travelling = view.travelling});
        if (block == MapTravelBlock::None && FindMapLamp(request) != nullptr) {
            StartMapTrip(request);
            view.travelling = true;
        } else {
            LOG_INFO(Debug, "Bloodborne map screen: no trip to {} ({})", request,
                     static_cast<int>(block));
        }
    }
    std::scoped_lock lock{map_view_mutex};
    map_view_now = std::move(view);
    map_view_read = true;
}

void PS4_SYSV_ABI OverheadCameraStep(u64, const GuestRegisterSnapshot* registers) {
    OverheadCameraStepNow(registers);
}

} // namespace Coop

void SetOverheadView(const std::optional<OverheadView>& view) {
    std::scoped_lock lock{camera_mutex};
    overhead_wanted = view;
    overhead_asked_at = Clock::now();
}

std::optional<GameCamera> DrawnGameCamera() {
    std::scoped_lock lock{camera_mutex};
    return camera_drawn;
}

bool ReadMapScreen(MapScreenView& view, bool open) {
    const bool was_open = map_screen_open.exchange(open, std::memory_order_relaxed);
    std::scoped_lock lock{map_view_mutex};
    if (open && !was_open) {
        // Just opened: where the player stood when it was last open is no news.
        map_view_now.me.reset();
        map_view_now.heading.reset();
    }
    view = map_view_now;
    return map_view_read;
}

std::vector<std::array<s16, 2>> ExploredOn(u32 packed_map) {
    std::scoped_lock lock{explored_mutex};
    std::vector<std::array<s16, 2>> cells;
    for (const ExploredCell& cell : explored) {
        if (cell.packed_map == (packed_map & 0xFFFF0000u)) {
            cells.push_back({cell.x, cell.z});
        }
    }
    return cells;
}

void RequestMapTrip(s32 warp_point) {
    map_trip_request.store(warp_point);
}

} // namespace Core::Bloodborne
