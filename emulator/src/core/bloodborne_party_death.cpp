// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Party death: going down, revives, the bloodstain, the party panel. Split from bloodborne_re.cpp;
// the shared declarations are in bloodborne_re_internal.h.

#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_write_watch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <fmt/ranges.h>
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/cpu_patches.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/system/userservice.h"
#include "core/memory.h"

namespace Core::Bloodborne {
namespace {

// Event script bindings ReviveDeadPartyGuest calls.
constexpr u64 IsWhiteGhostOffset = 0x01331D80;
constexpr u64 UpDateBloodMarkOffset = 0x013308B0;
constexpr u64 RevivePlayerOffset = 0x01332B20;
// 01.09's local-player revive: refills HP and clears the death's controller locks ([+0x58]
// +0x114 bit 0 and [+0x38]+0x48 bits 0-1), then makes the player a live one (type 0).
constexpr u64 ReviveLocalCharacterOffset = 0x013179B0;
// Resolves a respawn point id to a position and rotation in the loaded blocks.
constexpr u64 ResolveRespawnPointOffset = 0x0154AFC0;
constexpr u64 LoadedBlocksPointerOffset = 0x0553B148;
// ChrResetAnimation(_, entity): sends the character's behaviour back to idle.
constexpr u64 ChrResetAnimationOffset = 0x013370F0;

// Diagnostics for guest bloodstains in the host's world (Phase 2): logs the local bloodstain
// record, the spawned stain object and the world flags that gate them whenever they change.
// The local player's own bloodstain: [[SprjEventMan +0x80] +8], the object the game's show/hide
// (0x01868A50) takes when a load shows it (0x017C23EB-0x017C25DC) and on a join (0x0193A75A).
// (Not FrpgNetMan +0xC68's: passed that, the show crashed krolebord's game, Kaniel 2026-10-11.)
u64 OwnBloodstain() {
    const u64 events = ReadValue<u64>(image_base + EventManPointerOffset, 0);
    const u64 list = events >= 0x10000 ? ReadValue<u64>(events, 0x80) : 0;
    return list >= 0x10000 ? ReadValue<u64>(list, 8) : 0;
}

// A stain object the show can take: the stain (its vtable), readable, and its marker (+0x88) and
// effect (+0x98) either absent or readable (the show frees them first).
bool LooksLikeBloodstain(u64 stain) {
    if (!HasVtable(stain, BloodstainVtable, "ShowGuestBloodstain") ||
        !HasMemoryAccess(stain, 0xC8, MemoryProt::CpuRead)) {
        return false;
    }
    for (const u64 offset : {u64{0x88}, u64{0x98}}) {
        const u64 part = ReadValue<u64>(stain, offset);
        if (part != 0 && (part < 0x10000 || !HasMemoryAccess(part, 0x98, MemoryProt::CpuRead))) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace Coop {

// The stain's marker (+0x88) and effect (+0x98) are what 0x01868A50 makes when it shows the
// stain (and frees when it hides it): a stain recorded but never shown has neither (Kaniel
// 2026-10-11: no bloodstains in sight after the party wipe at the Tomb of Oedon).
void LogBloodstainStateChanges() {
    static std::string last;
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 record = data >= 0x10000 ? ReadValue<u64>(data, 0x30) : 0;
    const u64 stain = OwnBloodstain();
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayer();
    const auto state =
        fmt::format("has_mark={} record_valid={} record_30={} record_38={} record_block={:#x} "
                    "record_pos=({}, {}, {}) stain={:#x} "
                    "stain_hidden={} shown_marker={:#x} shown_effect={:#x} flag1592={} "
                    "flag1770={} chr_type={}",
                    data >= 0x10000 ? ReadValue<u8>(data, 0x28) : 0xFF,
                    record >= 0x10000 ? ReadValue<u8>(record, 0x44) : 0xFF,
                    record >= 0x10000 ? ReadValue<s32>(record, 0x30) : 0,
                    record >= 0x10000 ? ReadValue<s32>(record, 0x38) : 0,
                    record >= 0x10000 ? ReadValue<u32>(record, 0x3C) : 0,
                    record >= 0x10000 ? ReadValue<float>(record, 0x18) : 0.0f,
                    record >= 0x10000 ? ReadValue<float>(record, 0x1C) : 0.0f,
                    record >= 0x10000 ? ReadValue<float>(record, 0x20) : 0.0f, stain,
                    stain >= 0x10000 ? ReadValue<u8>(stain, 0xC0) : 0xFF,
                    stain >= 0x10000 ? ReadValue<u64>(stain, 0x88) : 0,
                    stain >= 0x10000 ? ReadValue<u64>(stain, 0x98) : 0,
                    global >= 0x10000 ? ReadValue<u8>(global, 0x1592) : 0xFF,
                    global >= 0x10000 ? ReadValue<u8>(global, 0x1770) : 0xFF,
                    player >= 0x10000 ? ReadValue<s32>(player, 0x78) : -1);
    if (state != last) {
        last = state;
        LOG_INFO(Debug, "Bloodborne bloodstain state map={:#x} {}", CurrentPlace().map, state);
    }
}

} // namespace Coop

namespace {

constexpr u64 ShowBloodstainOffset = 0x01868A50;

} // namespace

namespace Coop {

// See ShowsGuestBloodstain: a guest's own stain in the host's world.
void ShowGuestBloodstain() {
    static int tries = 0;
    static u64 tried_visit = 0;
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 record = data >= 0x10000 ? ReadValue<u64>(data, 0x30) : 0;
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayer();
    const Place place = CurrentPlace();
    const u32 map = place.state == PlaceState::Playing ? place.map : 0;
    if (data < 0x10000 || record < 0x10000 || player < 0x10000) {
        return;
    }
    if (place.visit != tried_visit) {
        tries = 0; // a load: a new visit
        tried_visit = place.visit;
    }
    const u64 stain = OwnBloodstain();
    const bool party_guest =
        party_controller && party_controller->SearchEffectId() == SmallResonantBellEffect;
    const bool loading = global < 0x10000 || TryRead<u8>(global, 0x08).value_or(1) != 0;
    if (!LooksLikeBloodstain(stain) ||
        !ShowsGuestBloodstain(party_guest, InPartySession(), loading,
                              ReadValue<u8>(data, 0x28) != 0, ReadValue<s32>(record, 0x38),
                              ReadValue<u32>(record, 0x3C), IsUsablePackedMap(map) ? map : 0,
                              ReadValue<u64>(stain, 0x88) != 0, tries)) {
        return;
    }
    ++tries;
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, bool, bool)>(image_base + ShowBloodstainOffset)(
        stain, true, false);
    LOG_INFO(Debug,
             "Bloodborne party: showed the guest's own bloodstain ({} echoes) in the host's "
             "world on map {:#x}: marker {:#x}",
             ReadValue<s32>(record, 0x38), map, ReadValue<u64>(stain, 0x88));
}

// See RestoresBloodstainMark: a party guest keeps its lost echoes through a join.
void KeepGuestBloodstainMark() {
    static bool mark_before = false;
    const u64 data = ReadValue<u64>(image_base + GameDataManPointerOffset, 0);
    const u64 record = data >= 0x10000 ? ReadValue<u64>(data, 0x30) : 0;
    if (data < 0x10000 || record < 0x10000) {
        return;
    }
    const bool mark_now = ReadValue<u8>(data, 0x28) != 0;
    const s32 echoes = ReadValue<s32>(record, 0x38);
    const u32 block = ReadValue<u32>(record, 0x3C);
    const bool party_guest =
        party_controller && party_controller->SearchEffectId() == SmallResonantBellEffect;
    if (RestoresBloodstainMark(party_guest, mark_before, mark_now, echoes, block)) {
        WriteValue(data, 0x28, u8{1});
        LOG_INFO(Debug, "Bloodborne party: kept the guest's lost echoes ({}) through the join",
                 echoes);
        return;
    }
    mark_before = mark_now;
}

} // namespace Coop

namespace {

// The event system's "load wait" byte: SetLoadWait (0x013358F0) sets it, the leave handlers
// (0x013840E0 and others) set it with 4047, and only its constructor (0x01339669) clears it.
// Every event update copies it to SprjActionButtonMan +0x52 (0x0133A114): no action button.
constexpr u64 LoadWaitOffset = 0x8D;
constexpr u64 ActionButtonManPointerOffset = 0x05540048;

} // namespace

namespace Coop {

DirectInbox<DirectVitals> direct_vitals_inbox;

} // namespace Coop

namespace {

// UpDateBloodMark (native 0x013D2910) takes the stain position from the player only while
// GlobalState +0x1592 is set, which it is in the player's own world. In the host's world it
// copies a stored position instead (+0x1500, where the guest was summoned from), so the stain
// went somewhere unseen. Set it for the call.
bool PS4_SYSV_ABI UpdateGuestBloodMark() {
    const u64 global = GetGlobalState();
    const bool can_flag =
        global >= 0x10000 && HasMemoryAccess(global, 0x1593, MemoryProt::CpuWrite);
    const u8 own_world = can_flag ? ReadValue<u8>(global, 0x1592) : 0;
    if (can_flag) {
        WriteValue(global, 0x1592, u8{1});
    }
    const bool result =
        reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + UpDateBloodMarkOffset)();
    if (can_flag) {
        WriteValue(global, 0x1592, own_world);
    }
    return result;
}

// Party death (see PartyDeathEnabled). The IsNoDead exit hook (KeepPartyMemberUp) keeps the
// local player at 1 HP where a lethal hit would kill it while it is armed (another member stands
// on its map) or down; the party tick (TickPartyDeath) puts it down, gets it up, lets it give up
// and wipes it; the vitals board client (RunVitalsBoardClient) tells the party.
#ifndef BLOODBORNE_PARTY_DEATH_DEFAULT
#define BLOODBORNE_PARTY_DEATH_DEFAULT 0 // the port's build defines it as 1
#endif
const bool party_death_enabled = PartyDeathEnabled(std::getenv("SHADPS4_BLOODBORNE_PARTY_DEATH"),
                                                   BLOODBORNE_PARTY_DEATH_DEFAULT != 0);
bool party_death_hook_installed{};
// What the game thread tells the hook: the local player's data module while party death is live
// (0 otherwise: the hook leaves everyone alone), and whether a lethal hit puts it down.
std::atomic<u64> party_death_data{};
std::atomic<bool> party_death_armed{};
std::atomic<bool> party_death_down{};
// Lethal hits the hook kept from killing the armed player: each change puts it down.
std::atomic<u32> party_death_catches{};
// The next death respawn resets the map (see DeathResetsMap).
std::atomic<bool> party_death_reset{};

} // namespace

namespace Coop {

// The lamp a death respawns the player at: resting at a lamp stores its respawn point id in
// GlobalState +0x1528 (SetReturnPointEntityId), and a death respawn resolves that id with
// 0x0154AFC0 against the loaded blocks ([0x0553B148], like Warp.RespawnTransform.Resolve). The
// same resolver finds the lamp while it is in a block the host's world has loaded; the guest is
// then moved there the way the Warp binding moves a character (vtable +0x5F8, then +0x1E1 |= 2).
// Otherwise the guest stays where it fell.
// A point on another map (see ChooseDeathRespawn: the first death's Hunter's Dream, a
// kidnapping's Gaol, a lamp elsewhere) is a trip there, as a guest's lamp trip is. So is a death
// that resets the map (party death's wipe), wherever the lamp is.
void MoveToRespawnPoint(bool arriving_from_trip) {
    const bool reset = !arriving_from_trip && party_death_reset.exchange(false);
    if (reset) {
        OnLifecycle(Lifecycle::Wipe); // kills and enemy HP, see their RegisterLifecycleReset
    }
    const u64 global = GetGlobalState();
    const s32 point = global >= 0x10000 ? ReadValue<s32>(global, 0x1528) : -1;
    const u64 blocks = ReadValue<u64>(image_base + LoadedBlocksPointerOffset, 0);
    const u64 player = GetLocalPlayer();
    if (blocks < 0x10000 || player < 0x10000) {
        return;
    }
    alignas(16) std::array<float, 4> position{};
    alignas(16) std::array<float, 4> rotation{};
    using Resolve = bool PS4_SYSV_ABI (*)(u64 blocks, float* position, float* rotation, s32 point);
    const bool in_loaded_block =
        point > 0 && reinterpret_cast<Resolve>(image_base + ResolveRespawnPointOffset)(
                         blocks, position.data(), rotation.data(), point);
    const DeathRespawn respawn =
        ChooseDeathRespawn(point, in_loaded_block, arriving_from_trip, reset);
    if (WipeTripRespawnsOnArrival(reset, respawn)) {
        wipe_trip_respawn_pending.store(true); // see RespawnWhatTheSaveLeftDead
    }
    switch (respawn) {
    case DeathRespawn::StayPut:
        LOG_INFO(Debug, "Bloodborne revive: respawn point {} not reachable; staying put", point);
        return;
    case DeathRespawn::Travel:
        guest_trip_point = point;
        // Right away: getting up where the player fell first looked like a second respawn.
        guest_lamp_trip_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
        LOG_INFO(Debug,
                 "Bloodborne revive: respawn point {} is on map {:#x}; travelling there "
                 "(reset={})",
                 point, PackedMapOfWarpPoint(point), reset);
        return;
    case DeathRespawn::MoveToPoint:
        break;
    }
    if (!PlaceLocalPlayer(position, rotation, "revive teleport")) {
        return;
    }
    LOG_INFO(Debug,
             "Bloodborne revive: moved to last lamp {} at ({}, {}, {}) rotation ({}, {}, {})",
             point, position[0], position[1], position[2], rotation[0], rotation[1], rotation[2]);
}

} // namespace Coop

namespace {

void WarpGuestToLastLamp() {
    MoveToRespawnPoint(false);
}

AliveStateSnapshot local_alive_state;

// The local player's state flags (+0x1E0, 8 bytes) and action state ([+0x3B0]+8: +0x1B4/+0x1B8).
u64 LocalActionModule(u64 player) {
    const u64 model = ReadValue<u64>(player, 0x3B0);
    return model >= 0x10000 ? ReadValue<u64>(model, 0x08) : 0;
}

std::optional<AliveState> ReadLocalAliveState(u64 player, bool& alive) {
    alive = false;
    const u64 module = LocalActionModule(player);
    const u64 model = ReadValue<u64>(player, 0x3B0);
    const u64 data = model >= 0x10000 ? ReadValue<u64>(model, 0x20) : 0;
    if (player < 0x10000 || module < 0x10000 || data < 0x10000) {
        return std::nullopt;
    }
    const AliveState state{
        .flags = ReadValue<u64>(player, 0x1E0),
        .action = ReadValue<s32>(module, 0x1B4),
        .action_sub = ReadValue<s32>(module, 0x1B8),
    };
    alive = ReadValue<s32>(data, 0xF8) > 0 && (ReadValue<u8>(player, 0x1E3) & 1) == 0;
    return state;
}

} // namespace

namespace Coop {

void ObserveLocalAliveState() {
    bool alive = false;
    if (const auto state = ReadLocalAliveState(GetLocalPlayer(), alive)) {
        local_alive_state.Observe(alive, *state);
    }
}

} // namespace Coop

namespace {

void RestoreLocalAliveState() {
    const u64 player = GetLocalPlayer();
    bool alive = false;
    const auto before = ReadLocalAliveState(player, alive);
    const auto& saved = local_alive_state.Saved();
    if (!before || !saved) {
        LOG_ERROR(Debug, "Bloodborne revive: no alive state to restore");
        return;
    }
    WriteValue(player, 0x1E0, saved->flags);
    const u64 module = LocalActionModule(player);
    WriteValue(module, 0x1B4, saved->action);
    WriteValue(module, 0x1B8, saved->action_sub);
    LOG_INFO(Debug,
             "Bloodborne revive restored alive state flags {:#x}->{:#x} action {}->{} "
             "sub {}->{}",
             before->flags, saved->flags, before->action, saved->action, before->action_sub,
             saved->action_sub);
}

GhostDeathCalls MakeReviveCalls() {
    return GhostDeathCalls{
        .is_white_ghost =
            reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + IsWhiteGhostOffset),
        .set_event_flag = reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
            image_base + PartyGhostSetEventFlagOffset),
        .clear_return_state =
            [] {
                // OnEvent_4000 (0x013813D0) sets it to 4 and skips deaths while it is above 0.
                const u64 events = ReadValue<u64>(image_base + EventManPointerOffset, 0);
                const u64 state = events >= 0x10000 ? ReadValue<u64>(events, 0x10) : 0;
                if (state >= 0x10000) {
                    WriteValue(state, 0x08, s32{0});
                }
            },
        .request_full_recover =
            reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + RequestFullRecoverOffset),
        .update_blood_mark = UpdateGuestBloodMark,
        .revive_player = reinterpret_cast<bool PS4_SYSV_ABI (*)()>(image_base + RevivePlayerOffset),
        .revive_local_character =
            reinterpret_cast<void PS4_SYSV_ABI (*)()>(image_base + ReviveLocalCharacterOffset),
        .restore_alive_state = RestoreLocalAliveState,
        .set_chr_type =
            reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32, s32)>(image_base + SetChrTypeOffset),
        .chr_reset_animation =
            reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + ChrResetAnimationOffset),
        .warp_to_last_lamp = WarpGuestToLastLamp,
    };
}

} // namespace

namespace Coop {

// A party guest that died gets up again where the game would show the "returned to your world"
// menu (the warp home behind it is patched out, see GhostDeathKeepSessionPatches).
void ReviveGuestAfterGhostDeath() {
    const bool revived = ReviveDeadPartyGuest(MakeReviveCalls());
    const u64 player = GetLocalPlayer();
    LOG_INFO(Debug, "Bloodborne party guest died; revived in place={} type={} flags1e3={:#x}",
             revived, player >= 0x10000 ? ReadValue<s32>(player, 0x78) : -1,
             player >= 0x10000 ? ReadValue<u8>(player, 0x1E3) : 0xFF);
}

// Replaces native SoloPlayDeath_2 (bloodstain, then the reload at the lamp) for a host with a
// session: it revives like a guest and the world is not reloaded.
// OnLeave_Limit (event 4046), 5 s after a white phantom lost its host (the host left, travelled,
// kicked it or the room went away): the reset and warp into the phantom's own world. A party
// member skips it (SkipsLeaveLimit): a phantom while the session goes on (with nobody left, the
// host crashed, it goes home, as a phantom never rings again), and a live player always: the
// game ran it on a host whose guest died apart from it, and the warp took the host to the title.
bool PS4_SYSV_ABI StayWhenTheHostLeaves(u64, const GuestRegisterSnapshot* registers) {
    const u64 player = GetLocalPlayer();
    if (!party_controller || player < 0x10000 ||
        !HasMemoryAccess(player, 0x7C, MemoryProt::CpuRead)) {
        return false;
    }
    const s32 type = ReadValue<s32>(player, 0x78);
    if (!SkipsLeaveLimit(type, OtherSessionMembers().size())) {
        LOG_INFO(Debug, "Bloodborne party guest lost its host and the session; going home");
        return false;
    }
    if (type != WhiteGhostCharacterType && registers != nullptr) {
        LogGameCallChainAt("leave limit", registers->rsp);
    }
    // The leave set 4047 ("on the way out"); left set, it swallows the next leave and death.
    reinterpret_cast<bool PS4_SYSV_ABI (*)(u64, s32, bool)>(
        image_base + PartyGhostSetEventFlagOffset)(0, 4047, false);
    // It also set the event system's load wait (+0x8D, the SetLoadWait binding 0x013358F0; the
    // timer passes the event system in rsi) for the reload skipped here, and only a new event
    // system clears it: Kaniel 2026-10-11, after a friend's game went away neither the host nor
    // the other guest could pick up items or talk, only fight.
    const u64 events = registers != nullptr ? registers->rsi : 0;
    const u64 buttons = ReadValue<u64>(image_base + ActionButtonManPointerOffset, 0);
    s32 load_wait = -1;
    if (events >= 0x10000 && HasMemoryAccess(events, LoadWaitOffset + 1, MemoryProt::CpuWrite)) {
        load_wait = ReadValue<u8>(events, LoadWaitOffset);
        if (load_wait == 1) {
            WriteValue(events, LoadWaitOffset, u8{0});
        }
    }
    LOG_INFO(Debug,
             "Bloodborne party member (type {}) lost a member; staying where it is map={:#x} "
             "load wait {} (action buttons {}) cleared",
             type, CurrentPlace().map, load_wait,
             buttons >= 0x10000 ? ReadValue<u8>(buttons, 0x52) : -1);
    return true;
}

bool PS4_SYSV_ABI OverrideHostDeathRespawn(u64, const GuestRegisterSnapshot*) {
    const u64 multi_play = GetMatchingState();
    const s32 matching_state =
        multi_play >= 0x10000 && HasMemoryAccess(multi_play, 0x128, MemoryProt::CpuRead)
            ? ReadValue<s32>(multi_play, 0x124)
            : -1;
    if (!ShouldReviveHostInPlace(matching_state)) {
        return false;
    }
    ReviveDeadPartyHost(MakeReviveCalls());
    LOG_INFO(Debug,
             "Bloodborne live player died in a party session (state {}); revived without a "
             "world reload",
             matching_state);
    return true;
}

} // namespace Coop

namespace {

// The local player's data module ([[player +0x3B0] +0x20]: HP +0xF8, max HP +0xFC, flags +0x200).
u64 PlayerDataModule(u64 player) {
    const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
    return modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
}

// The local player's attack rating for the weapon in its right hand, as the equipment menu shows
// it: UpdateWeaponAtk (0x018FCCB0, the character) works out one per weapon slot (physical + arcane
// + fire + bolt with scaling, upgrades and gems) into the character's PlayerGameData (its vtable
// +0x1C8), right hand 1 at +0x5E4 and right hand 2 at +0x5EC; ChrAsm ([player +0x4B8]) +0x10 says
// which right-hand weapon is held. On the game thread.
constexpr u64 UpdateWeaponAtkOffset = 0x018FCCB0;
struct AttackRatings {
    std::array<s32, 4> slots{-1, -1, -1, -1}; // left 1, right 1, left 2, right 2
    s32 right_hand = -1;
    std::array<s32, 4> weapons{-1, -1, -1, -1}; // ChrAsm +0x64: the weapon ids in the same order

    s32 Held() const {
        return right_hand == 1 ? slots[3] : right_hand == 0 ? slots[1] : 0;
    }
};
AttackRatings ReadAttackRatings(u64 player) {
    AttackRatings ratings;
    const u64 vtable = IsCharacter(player, "ReadAttackRatings") ? ReadValue<u64>(player, 0) : 0;
    const u64 game_data_of = vtable >= 0x10000 ? ReadValue<u64>(vtable, 0x1C8) : 0;
    const u64 asm_data = player >= 0x10000 ? ReadValue<u64>(player, 0x4B8) : 0;
    if (game_data_of < 0x10000 || asm_data < 0x10000) {
        return ratings;
    }
    static constexpr std::array<u8, 6> UpdatePrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    if (std::ranges::equal(
            std::span<const u8>{reinterpret_cast<const u8*>(image_base + UpdateWeaponAtkOffset),
                                UpdatePrologue.size()},
            UpdatePrologue)) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64)>(image_base + UpdateWeaponAtkOffset)(player);
    }
    const u64 player_data = reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(game_data_of)(player);
    if (player_data < 0x10000 || !HasMemoryAccess(player_data, 0x5F0, MemoryProt::CpuRead)) {
        return ratings;
    }
    for (u64 slot = 0; slot < 4; ++slot) {
        ratings.slots[slot] = ReadValue<s32>(player_data, 0x5E0 + slot * 4);
        ratings.weapons[slot] = ReadValue<s32>(asm_data, 0x64 + slot * 4);
    }
    ratings.right_hand = ReadValue<s32>(asm_data, 0x10);
    return ratings;
}

// Data module +0x200 flags a downed player wears: bit 1 skips any HP loss, bit 3 a hit's damage.
constexpr u8 DownedNoDamageFlags = 0x2 | 0x8;
// WorldChrManDbg's static "Player Hide" and "Player Silence" (debug menu flags): the AI's view of
// the local player (0x01A55390, 0x01A55230) takes it as unseen and unheard, so enemies leave a
// downed player alone.
constexpr u64 PlayerHideFlagOffset = 0x0553E88E;
constexpr u64 PlayerSilenceFlagOffset = 0x0553E88F;

// The enemies' targeting systems (see ForgetsTargetNode): the enemy one's vtable and its base's.
constexpr u64 EnemyTargetingVtableOffset = 0x0531B9F0;
constexpr u64 BaseTargetingVtableOffset = 0x05366280;

// Every enemy in this world forgets the characters with these handles (see ForgetsTargetNode);
// returns how many target nodes it changed.
u32 ForgetDownedTargets(std::span<const u32> handles) {
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (handles.empty() || world < 0x10000) {
        return 0;
    }
    u32 forgotten = 0;
    ForEachWorldCharacter(world, [&](u64 character) {
        const u64 manager = ReadValue<u64>(character, 0x3B8);
        const u64 ai = manager >= 0x10000 && HasMemoryAccess(manager, 0xB8, MemoryProt::CpuRead)
                           ? ReadValue<u64>(manager, 0xB0)
                           : 0;
        const u64 targeting = ai >= 0x10000 && HasMemoryAccess(ai, 0x568, MemoryProt::CpuRead)
                                  ? ReadValue<u64>(ai, 0x560)
                                  : 0;
        if (targeting < 0x10000 || !HasMemoryAccess(targeting, 0x20, MemoryProt::CpuRead)) {
            return;
        }
        const u64 vtable = ReadValue<u64>(targeting, 0);
        if (vtable != image_base + EnemyTargetingVtableOffset &&
            vtable != image_base + BaseTargetingVtableOffset) {
            return;
        }
        u64 node = ReadValue<u64>(targeting, 0x18);
        for (int count = 0;
             count < 64 && node >= 0x10000 && HasMemoryAccess(node, 0x54, MemoryProt::CpuWrite);
             ++count, node = ReadValue<u64>(node, 0x08)) {
            if (ForgetsTargetNode(ReadValue<u8>(node, 0x02), ReadValue<s32>(node, 0x28),
                                  ReadValue<u32>(node, 0x50), handles) &&
                ReadValue<float>(node, 0x2C) != ForgottenTargetTimer) {
                WriteValue(node, 0x2C, ForgottenTargetTimer);
                ++forgotten;
            }
        }
    });
    return forgotten;
}

// The handles of the downed players in this world: the local player while `down`, and each other
// member the board has down on this map (the other player character nearest to where it posted).
std::vector<u32> DownedPlayerHandles(u64 player, bool down,
                                     std::span<const LampPosition> others_down) {
    std::vector<u32> handles;
    if (down && player >= 0x10000) {
        handles.push_back(ReadValue<u32>(player, 0x08));
    }
    if (others_down.empty()) {
        return handles;
    }
    const std::vector<OtherPlayer> players = OtherPlayers();
    for (const LampPosition& at : others_down) {
        const OtherPlayer* nearest = nullptr;
        float nearest_distance = 4.0f;
        for (const OtherPlayer& other : players) {
            const float distance = other.at
                                       ? std::hypot((*other.at)[0] - at[0], (*other.at)[1] - at[1],
                                                    (*other.at)[2] - at[2])
                                       : nearest_distance + 1.0f;
            if (distance <= nearest_distance) {
                nearest_distance = distance;
                nearest = &other;
            }
        }
        if (nearest != nullptr) {
            handles.push_back(ReadValue<u32>(nearest->character, 0x08));
        }
    }
    return handles;
}

// The fall damage step (0x01A35C33) takes a fall into the void as a fixed lethal height while
// [[character +0x58] +0x38] +0x48 bit 1 is set. Such a fall stays a real death: nobody could
// reach the player down there.
bool FallsIntoTheVoid(u64 data) {
    const u64 character = ReadValue<u64>(data, 0x08);
    const u64 control = character >= 0x10000 ? ReadValue<u64>(character, 0x58) : 0;
    const u64 state = control >= 0x10000 ? ReadValue<u64>(control, 0x38) : 0;
    return state >= 0x10000 && (ReadValue<u8>(state, 0x48) & 2) != 0;
}

// Diagnostics: IsNoDead's other callers that ask about the local player, once each.
void NoteOtherNoDeadCheck(u64 return_offset) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::scoped_lock lock{mutex};
    if (seen.size() < 64 && seen.insert(return_offset).second) {
        LOG_INFO(Debug, "Bloodborne party death: IsNoDead asked from {:#x} (left alone)",
                 return_offset);
    }
}

// IsNoDead's exit (see IsNoDeadExit): for the local player a lethal check answers "no dead"
// while it is armed or down, and each catch that puts it down is counted for the party tick.
void PS4_SYSV_ABI KeepPartyMemberUp(u64, GuestRegisterSnapshot* registers) {
    const u64 data = party_death_data.load(std::memory_order_relaxed);
    if (data == 0 || registers->rbx != data) {
        return;
    }
    u64 return_address = 0;
    std::memcpy(&return_address, reinterpret_cast<const void*>(registers->rbp + 8),
                sizeof(return_address));
    const u64 return_offset = return_address - image_base;
    NoDeadCheck check = ClassifyNoDeadCheck(return_offset);
    if (check == NoDeadCheck::Other) {
        NoteOtherNoDeadCheck(return_offset);
        return;
    }
    check = NoDeadCheckAt(check, ReadValue<s32>(data, 0xF8));
    if (check == NoDeadCheck::Other) {
        return; // a hit that left HP
    }
    if (check == NoDeadCheck::Lethal && FallsIntoTheVoid(data)) {
        check = NoDeadCheck::Instant;
    }
    const bool answer = (registers->r14 & 0xFF) != 0;
    const bool down = party_death_down.load(std::memory_order_relaxed);
    if (answer ||
        !KeepsPlayerAlive(check, party_death_armed.load(std::memory_order_relaxed), down, answer)) {
        return;
    }
    registers->r14 = (registers->r14 & ~u64{0xFF}) | 1;
    if (!down) {
        party_death_catches.fetch_add(1, std::memory_order_relaxed);
    }
}

// SHADPS4_BLOODBORNE_DOWNED_ANIMATION: an animation a downed player plays (none by default; for
// trying poses that lie down), undone by ChrResetAnimation when it gets up.
const s32 downed_animation = [] {
    const char* setting = std::getenv("SHADPS4_BLOODBORNE_DOWNED_ANIMATION");
    s32 animation = 0;
    if (setting != nullptr) {
        std::from_chars(setting, setting + std::strlen(setting), animation);
    }
    return animation;
}();

// What the pad reader saw (FilterPartyDeathPad): the buttons held before the downed filter, the
// attack presses so far and whether the last one was heavy (R2).
std::atomic<u32> party_pad_buttons{};
std::atomic<u32> party_attack_presses{};
std::atomic<bool> party_attack_heavy{};

} // namespace

namespace Coop {

VitalsBoard vitals_board;
std::optional<std::chrono::steady_clock::time_point> vitals_heard_at;

} // namespace Coop

namespace {

// What the port's overlay draws (ReadPartyPanel), as of the last party tick.
std::mutex panel_mutex;
PartyPanel panel_now;

void SetPartyPanel(PartyPanel panel) {
    std::scoped_lock lock{panel_mutex};
    panel_now = std::move(panel);
}

// Party death's "in the session": only in the session's world. A guest still on its way (its
// summon load) posts "off" and dies as a solo player there (SessionArrival).
bool InPartyHere() {
    return InPartySession() && InSessionWorld();
}

// The other session members' board names (SessionBoardNames), read again every second, nothing
// out of a session or while one of them is not found: the lines of members out of this session
// are dropped from the board (OnlySessionMembers).
std::optional<std::vector<std::string>> SessionNames(std::span<const MemberVitals> board,
                                                     std::string_view self) {
    static s64 read_at = std::numeric_limits<s64>::min();
    static std::optional<std::vector<std::string>> names;
    const s64 now = SteadyMilliseconds();
    if (read_at != std::numeric_limits<s64>::min() && now - read_at < 1'000) {
        return names;
    }
    read_at = now;
    names.reset();
    if (!InPartySession()) {
        return names;
    }
    std::vector<std::string> online_ids;
    for (const MemberVitals& member : board) {
        if (std::string id = OnlineIdOfBoardMember(member.member);
            member.member != self && !id.empty()) {
            online_ids.push_back(std::move(id));
        }
    }
    std::vector<std::optional<std::string>> found;
    for (const u64 member : OtherSessionMembers()) {
        found.push_back(SessionMemberBoardName(member, online_ids));
    }
    names = SessionBoardNames(found);
    return names;
}

} // namespace

namespace Coop {

VitalsView ReadVitalsBoard() {
    VitalsView view;
    {
        std::scoped_lock lock{vitals_mutex};
        view = VitalsView{vitals_board, vitals_member};
        if (vitals_heard_at) {
            view.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - *vitals_heard_at)
                              .count();
        }
    }
    // The peers' own lines straight over the P2P link, fresher than the board's copies.
    std::vector<DirectVitals> direct;
    {
        std::scoped_lock lock{direct_boards_mutex};
        direct = direct_vitals_inbox.Fresh(SteadyMilliseconds());
    }
    if (!direct.empty()) {
        view.board = MergeDirectVitals(view.board, direct);
        view.age_ms = std::min<s64>(view.age_ms, 0);
    }
    view.board.members = OnlySessionMembers(view.board.members, view.member,
                                            SessionNames(view.board.members, view.member));
    return view;
}
PartyDeathRun party_death_run;

} // namespace Coop

namespace {

void SetPlayerFlag(u64 offset, u8 value) {
    WriteValue(image_base + offset, 0, value);
}

// The yellow YOU ARE DOWN banner (the party HUD's, in place of the game's PvP Draw banner): queued
// by the game's own SetTextEffect, as YOU DIED is. Without the party HUD it would show the Draw
// banner's own words, so it is left out where the HUD is not the port's (the emulator) or is off
// (BB_PARTY_HUD=0).
void ShowDownedBanner() {
#ifdef BLOODBORNE_PARTY_HUD
    static const bool party_hud = [] {
        const char* setting = std::getenv("BB_PARTY_HUD"); // run.py's switch for the patched HUD
        return setting == nullptr || std::string_view{setting} != "0";
    }();
    if (!party_hud) {
        return;
    }
    const auto* start = reinterpret_cast<const u8*>(image_base + SetTextEffectOffset);
    if (!std::ranges::equal(std::span<const u8>{start, SetTextEffectStart.size()},
                            SetTextEffectStart)) {
        LOG_ERROR(Debug, "Bloodborne party death: SetTextEffect is not where it should be; no "
                         "downed banner");
        return;
    }
    reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + SetTextEffectOffset)(
        0, DownedBannerTextEffect);
#endif
}

void GoDown(PartyDeathRun& run, u64 data, s64 now_ms) {
    const u32 down = run.downed.GoDown(now_ms, ReadValue<s32>(data, 0xFC));
    run.give_up.Restart((party_pad_buttons.load() & PadCircle) != 0);
    party_death_down.store(true);
    const u8 flags = ReadValue<u8>(data, 0x200);
    run.flags_added = DownedNoDamageFlags & static_cast<u8>(~flags);
    WriteValue(data, 0x200, static_cast<u8>(flags | DownedNoDamageFlags));
    WriteValue(data, 0xF8, s32{1});
    run.hide_before = ReadValue<u8>(image_base + PlayerHideFlagOffset, 0);
    run.silence_before = ReadValue<u8>(image_base + PlayerSilenceFlagOffset, 0);
    SetPlayerFlag(PlayerHideFlagOffset, 1);
    SetPlayerFlag(PlayerSilenceFlagOffset, 1);
    ShowDownedBanner();
    bool posed = false;
    if (downed_animation > 0) {
        using ForcePlayAnimation = bool PS4_SYSV_ABI (*)(u64 unused, s32 entity, s32 animation);
        posed = reinterpret_cast<ForcePlayAnimation>(image_base + ForcePlayAnimationOffset)(
            0, LocalPlayerEntity, downed_animation);
    }
    LOG_INFO(Debug,
             "Bloodborne party death: down {} on map {:#x}, {} damage to get up (max HP {}, "
             "pose {} played={})",
             down, CurrentPlace().map, run.downed.DamageNeeded(), ReadValue<s32>(data, 0xFC),
             downed_animation, posed);
}

// Each tick while down: no damage, the revive's progress as HP (DownedHp), unseen and unheard
// (the game may undo any of them).
void KeepDown(PartyDeathRun& run, u64 data) {
    const u8 flags = ReadValue<u8>(data, 0x200);
    if ((flags & DownedNoDamageFlags) != DownedNoDamageFlags) {
        run.flags_added |= DownedNoDamageFlags & static_cast<u8>(~flags);
        WriteValue(data, 0x200, static_cast<u8>(flags | DownedNoDamageFlags));
    }
    const s32 hp =
        DownedHp(ReadValue<s32>(data, 0xFC), run.downed.DamageLanded(), run.downed.DamageNeeded());
    if (ReadValue<s32>(data, 0xF8) != hp) {
        WriteValue(data, 0xF8, hp);
    }
    SetPlayerFlag(PlayerHideFlagOffset, 1);
    SetPlayerFlag(PlayerSilenceFlagOffset, 1);
}

void EndDown(PartyDeathRun& run, u64 data) {
    party_death_down.store(false);
    WriteValue(data, 0x200, static_cast<u8>(ReadValue<u8>(data, 0x200) & ~run.flags_added));
    run.flags_added = 0;
    SetPlayerFlag(PlayerHideFlagOffset, run.hide_before);
    SetPlayerFlag(PlayerSilenceFlagOffset, run.silence_before);
}

void GetUp(PartyDeathRun& run, u64 data, s64 now_ms) {
    EndDown(run, data);
    const s32 hp = RevivedHp(ReadValue<s32>(data, 0xFC));
    WriteValue(data, 0xF8, hp);
    run.downed.GetUp(now_ms);
    if (downed_animation > 0) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, s32)>(image_base + ChrResetAnimationOffset)(
            0, LocalPlayerEntity);
    }
    LOG_INFO(Debug, "Bloodborne party death: revived from down {} with {} HP", run.downed.DownId(),
             hp);
}

// A real death from the down (a wipe, giving up, shadNet gone): the game's own kill, then the
// party death respawn (see MoveToRespawnPoint), which reloads the map if `reset`.
void DieFromDown(PartyDeathRun& run, u64 player, u64 data, bool reset, std::string_view why) {
    EndDown(run, data);
    run.downed.Died();
    party_death_reset.store(reset);
    static constexpr std::array<u8, 6> KillPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    const bool killed =
        IsCharacter(player, "DieFromDown kill") &&
        std::ranges::equal(
            std::span<const u8>{reinterpret_cast<const u8*>(image_base + KillCharacterOffset),
                                KillPrologue.size()},
            KillPrologue);
    if (killed) {
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64 character)>(image_base +
                                                               KillCharacterOffset)(player);
    } else {
        WriteValue(data, 0xF8, s32{0});
    }
    LOG_INFO(Debug, "Bloodborne party death: {} ends down {}; dies (reset={}, game kill={})", why,
             run.downed.DownId(), reset, killed);
}

} // namespace

namespace Coop {

void TickPartyDeath() {
    if (!party_death_hook_installed) {
        // Without party death the vitals board still says who stands on which map: a host back
        // from a trip asks a member that was still joining for its data only once it stands here
        // (FinishWaitingJoins; rig 2026-10-11, party death off: the guest stayed unseen).
        const u64 data = PlayerDataModule(GetLocalPlayer());
        if (data >= 0x10000 && HasMemoryAccess(data, 0x100, MemoryProt::CpuRead)) {
            const bool in_session = InPartyHere();
            PostVitals(MemberVitals{.map = in_session ? PlayingMap() : 0,
                                    .vital = in_session ? Vital::Up : Vital::Off,
                                    .hp = ReadValue<s32>(data, 0xF8),
                                    .max = ReadValue<s32>(data, 0xFC),
                                    .at = LocalPlayerPosition().value_or(LampPosition{})},
                       {});
        }
        return;
    }
    auto& run = party_death_run;
    const u64 player = GetLocalPlayer();
    const u64 data = PlayerDataModule(player);
    if (data < 0x10000 || !HasMemoryAccess(data, 0x204, MemoryProt::CpuWrite)) {
        party_death_data.store(0);
        SetPartyPanel({});
        return; // loading
    }
    const s64 now = SteadyMilliseconds();
    const bool in_session = InPartyHere();
    const u32 map = PlayingMap();
    const VitalsView view = ReadVitalsBoard();
    const std::span<const MemberVitals> board =
        view.Fresh() ? std::span<const MemberVitals>{view.board.members}
                     : std::span<const MemberVitals>{};
    const bool armed =
        in_session && !view.member.empty() && SomeoneElseStands(board, view.member, map);
    party_death_armed.store(armed);
    party_death_data.store(in_session || run.downed.Current() == Vital::Down ? data : 0);
    bool alive = false;
    ReadLocalAliveState(player, alive);
    if (const u32 uses = lamp_uses.load(); uses != run.seen_lamp_uses) {
        run.seen_lamp_uses = uses;
        run.downed.ClearPenalty();
    }

    switch (run.downed.Current()) {
    case Vital::Off:
    case Vital::Up: {
        const u32 catches = party_death_catches.load();
        const bool caught = catches != run.seen_catches;
        run.seen_catches = catches;
        if (caught && alive) {
            GoDown(run, data, now);
            break;
        }
        if (!alive) {
            // A real death: one of the game's kills, or a lethal hit with nobody else standing.
            run.downed.Died();
            party_death_reset.store(in_session && DeathResetsMap(false, armed));
            LOG_INFO(Debug, "Bloodborne party death: died for real on map {:#x} (reset={})", map,
                     party_death_reset.load());
            break;
        }
        const u32 attacks = party_attack_presses.load();
        const bool attacked = attacks != run.seen_attacks;
        run.seen_attacks = attacks;
        const auto at = LocalPlayerPosition();
        const auto target = attacked && in_session && at
                                ? ReviveTarget(board, view.member, map, *at)
                                : std::nullopt;
        const AttackRatings ratings = target ? ReadAttackRatings(player) : AttackRatings{};
        const s32 rating = ratings.Held();
        const bool heavy = party_attack_heavy.load();
        const u32 damage = ReviveHitDamage(rating, heavy);
        if (target && run.revive_hits.Press(target->member, target->down, damage, now)) {
            LOG_INFO(Debug,
                     "Bloodborne party death: revive hit on {} (down {}) for {} damage (attack "
                     "rating {}, heavy={}; ratings {} weapons {} right hand {})",
                     target->member, target->down, damage, rating, heavy,
                     fmt::join(ratings.slots, " "), fmt::join(ratings.weapons, " "),
                     ratings.right_hand);
        }
        break;
    }
    case Vital::Down: {
        if (!alive) {
            // Killed anyway (a fall into the void, an event's kill).
            EndDown(run, data);
            run.downed.Died();
            party_death_reset.store(in_session && DeathResetsMap(false, armed));
            LOG_INFO(Debug, "Bloodborne party death: killed while down {} (reset={})",
                     run.downed.DownId(), party_death_reset.load());
            break;
        }
        const auto wipe = view.Fresh() ? WipeFor(view.board, view.member) : std::nullopt;
        const bool wiped = wipe && *wipe != run.handled_wipe;
        const bool gave_up = run.give_up.Tick((party_pad_buttons.load() & PadCircle) != 0, now);
        u32 damage = 0;
        for (const MemberVitals& entry : board) {
            if (entry.member == view.member && entry.down == run.downed.DownId()) {
                damage = entry.revive_damage;
            }
        }
        const bool revived = run.downed.Damage(damage, now);
        run.downed.Tick(now);
        KeepDown(run, data);
        switch (EndOfDown(wiped, gave_up, revived, view.age_ms)) {
        case DownEnd::None:
            break;
        case DownEnd::Revive:
            GetUp(run, data, now);
            break;
        case DownEnd::Wipe:
            run.handled_wipe = *wipe;
            DieFromDown(run, player, data, true, fmt::format("wipe {}", *wipe));
            break;
        case DownEnd::GiveUp:
            DieFromDown(run, player, data, in_session && DeathResetsMap(false, armed), "giving up");
            break;
        case DownEnd::BoardLost:
            DieFromDown(run, player, data, in_session, "the vitals board going quiet");
            break;
        }
        break;
    }
    case Vital::Dead:
        if (alive) {
            run.downed.Alive(now);
            party_death_reset.store(false);
            LOG_INFO(Debug, "Bloodborne party death: up again after a real death");
        }
        break;
    }

    // Enemies forget the downed players here (this one and the others), every tick while any is
    // down: an enemy that sees a downed guest again (Hide is the local player's only) would take it
    // as a target again.
    const std::vector<LampPosition> others_down =
        in_session ? OthersDownOnMap(board, view.member, map) : std::vector<LampPosition>{};
    const std::vector<u32> downed_handles =
        DownedPlayerHandles(player, run.downed.Current() == Vital::Down, others_down);
    if (const u32 forgotten = ForgetDownedTargets(downed_handles); forgotten > 0) {
        if (run.targets_forgotten == 0) {
            LOG_INFO(Debug,
                     "Bloodborne party death: enemies forgot {} target(s) on downed players "
                     "(self down={}, others down={})",
                     forgotten, run.downed.Current() == Vital::Down, others_down.size());
        }
        run.targets_forgotten += forgotten;
    } else if (downed_handles.empty()) {
        run.targets_forgotten = 0;
    }

    const auto at = LocalPlayerPosition();
    PostVitals(
        MemberVitals{
            .map = in_session ? map : 0,
            .vital = in_session ? run.downed.Current() : Vital::Off,
            .hp = ReadValue<s32>(data, 0xF8),
            .max = ReadValue<s32>(data, 0xFC),
            .down = run.downed.DownId(),
            .landed = run.downed.DamageLanded(),
            .needed = run.downed.DamageNeeded(),
            .at = at.value_or(LampPosition{}),
        },
        run.revive_hits.Totals(board));

    const Vital vital = run.downed.Current();
    PartyPanel panel = in_session ? BuildPartyPanel(board, view.member, map,
                                                    vital == Vital::Up ? at : std::nullopt)
                                  : PartyPanel{};
    panel.vital = in_session ? vital : Vital::Off;
    panel.landed = run.downed.DamageLanded();
    panel.needed = run.downed.DamageNeeded();
    panel.give_up = vital == Vital::Down ? run.give_up.Progress(now) : 0.0f;
    SetPartyPanel(std::move(panel));
}

// Party death needs a party (SHADPS4_BLOODBORNE_PARTY) and the setting.
void InstallPartyDeath(u64 base, u64 image_size) {
    if (!party_death_enabled || !party_controller) {
        return;
    }
    if (IsNoDeadExitOffset + IsNoDeadExit.size() > image_size ||
        !InstallCoopHook(reinterpret_cast<void*>(base + IsNoDeadExitOffset), IsNoDeadExit, 0,
                         KeepPartyMemberUp)) {
        LOG_ERROR(Debug, "Bloodborne party death hook unavailable; deaths stay as they are");
        coop_features.Skipped("party death", SiteFailure(base, image_size, IsNoDeadExitOffset));
        return;
    }
    party_death_hook_installed = true;
    coop_features.On("party death");
    LOG_INFO(Debug, "Bloodborne party death: a lethal hit puts a party member down while another "
                    "stands on its map (damage revives it, holding Circle gives up)");
}

} // namespace Coop

void FilterPartyDeathPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks,
                         std::uint8_t& l2, std::uint8_t& r2) {
    if (!party_death_hook_installed) {
        return;
    }
    const u32 before = party_pad_buttons.exchange(buttons, std::memory_order_relaxed);
    if (AttackPressed(before, buttons)) {
        party_attack_heavy.store(HeavyAttackPressed(before, buttons), std::memory_order_relaxed);
        party_attack_presses.fetch_add(1, std::memory_order_relaxed);
    }
    if (!party_death_down.load(std::memory_order_relaxed)) {
        return;
    }
    const PadInput kept = DownedPad({.buttons = buttons, .sticks = sticks, .l2 = l2, .r2 = r2});
    buttons = kept.buttons;
    sticks = kept.sticks;
    l2 = kept.l2;
    r2 = kept.r2;
}

bool ReadPartyPanel(PartyPanel& panel) {
    std::scoped_lock lock{panel_mutex};
    panel = panel_now;
    return !panel.members.empty() || panel.vital == Vital::Down;
}

} // namespace Core::Bloodborne
