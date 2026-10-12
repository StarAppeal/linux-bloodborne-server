// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The co-op runtime's internals that its files share (bloodborne_re.cpp and
// the bloodborne_*.cpp split from it): types, constants and templates, and the
// functions and variables one file defines for the others, in namespace Coop.
// Only those files include it.

#pragma once

#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_re.h"
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

#include <httplib.h>
#include "core/bloodborne_re_sites.h"

namespace Core::Bloodborne {
namespace Coop {
extern std::atomic<u64> event_sequence;
extern std::mutex capture_mutex;
extern std::ofstream capture_file;
extern uintptr_t image_base;
extern bool negative_area_patch_installed;
extern bool area_flag_patch_installed;
extern bool responder_bell_area_patch_installed;
extern bool responder_bell_common_patch_installed;
extern bool active_bell_negative_area_patch_installed;
extern bool active_bell_area_flag_patch_installed;
extern bool responder_search_negative_area_patch_installed;
extern bool sos_status_area_patch_installed;
extern bool summon_candidate_area_patch_installed;
extern bool summon_build_area_patch_installed;
extern bool summon_build_world_state_patch_installed;
extern bool summon_build_negative_event_patch_installed;
extern bool stage_transition_keep_matching_patch_installed;
extern bool world_teardown_hook_installed;
extern bool ghost_death_revive_hook_installed;
extern bool seamless_travel_hooks_installed;
extern bool stop_request_hook_installed;
extern bool host_death_respawn_hook_installed;
extern bool summon_build_host_placement_hook_installed;
extern bool cross_map_guest_handoff_hook_installed;
extern bool summon_reload_state_hook_installed;
extern bool deferred_summon_reload_hook_installed;
extern bool healing_fountain_host_availability_hook_installed;

struct SummonPlacementDescriptor {
    u32 packed_region{};
    float x{};
    float y{};
    float z{};
    float heading{};
    s32 area{};
};
static_assert(sizeof(SummonPlacementDescriptor) == 0x18);

struct HostPlacementRewriteRecord {
    std::string_view result{"not_attempted"};
    u64 prepared{};
    SummonPlacementDescriptor candidate{};
    SummonPlacementDescriptor host{};
};
extern thread_local HostPlacementRewriteRecord host_placement_rewrite;
extern std::atomic<u32> pending_summon_reload_map;
extern std::atomic<u32> pending_summon_reload_state_map;

struct GuestPlacementHandoffRecord {
    std::string_view result{"not_attempted"};
    u64 object{};
    u64 state{};
    u32 current_map{};
    u32 received_map{};
    u32 selected_map{};
    bool transported_host{};
    bool placement_refreshed{};
    bool select_result{};
    bool summon_reload_armed{};
    s32 transition_result{};
};
extern thread_local GuestPlacementHandoffRecord guest_placement_handoff;

struct SummonReloadStateRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 descriptor_map{};
    u64 reload_state{};
    s32 state_before{};
    s32 state_after{};
    bool summon_reload_started{};
    bool state_setter_used{};
    bool placement_refreshed{};
};
extern thread_local SummonReloadStateRecord summon_reload_state;

struct DeferredSummonReloadRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 current_map{};
    bool summon_reload_started{};
};
extern thread_local DeferredSummonReloadRecord deferred_summon_reload;

struct PreMatchGuestWarpRecord {
    std::string_view result{"not_pending"};
    u64 state{};
    u32 current_map{};
    u32 target_map{};
    u32 selected_map{};
    s32 multi_play_state{-1};
    bool placement_written{};
    bool select_result{};
    bool responder_resume_armed{};
    s32 transition_result{};
};
extern thread_local PreMatchGuestWarpRecord pre_match_guest_warp;

struct PendingResponderResume {
    u32 target_map{};
    s32 target_area{};
    u32 ready_observations{};
    u32 attempts{};
    bool dispatched{};
    std::chrono::steady_clock::time_point dispatched_at{};
};
extern thread_local PendingResponderResume pending_responder_resume;

struct ResponderResumeRecord {
    std::string_view result{"not_pending"};
    u32 target_map{};
    u32 current_map{};
    s32 target_area{};
    s32 current_area{};
    u64 state{};
    u64 multi_play{};
    s32 multi_play_state{-1};
    u64 matching_controller{};
    u64 player{};
    u32 ready_observations{};
    u32 attempts{};
    bool effect_active{};
    bool native_result{};
};
extern thread_local ResponderResumeRecord responder_resume;

struct HealingFountainAvailabilityRecord {
    std::string_view result{"not_attempted"};
    u64 object{};
    u64 player{};
    bool blocked_before{};
    bool host_effect{};
    bool blocked_after{};
};
extern thread_local HealingFountainAvailabilityRecord healing_fountain_availability;
bool EnvFlagEnabled(const char* name);
std::string BytesToHex(std::span<const u8> bytes);
extern CoopFeatureReport coop_features;
std::string FeatureMismatch(u64 base, u64 image_size, std::string_view feature);
std::string SiteFailure(u64 base, u64 image_size, u64 offset);
bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestCodeHook hook);

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestFunctionOverride hook);

bool InstallCoopHook(void* address, std::span<const u8> bytes, u64 tag, GuestRegisterEditHook hook);
bool HasMemoryAccess(u64 address, size_t size, MemoryProt required);

// The value at address + offset, or nothing when that memory is not readable (for reads where 0
// means something, which ReadValue would give then).
template <typename T>
std::optional<T> TryRead(u64 address, size_t offset) {
    T value{};
    if (address > std::numeric_limits<u64>::max() - offset) {
        return std::nullopt;
    }
    const u64 source = address + offset;
    if (source > std::numeric_limits<u64>::max() - sizeof(value)) {
        return std::nullopt;
    }

    if (!HasMemoryAccess(source, sizeof(value), MemoryProt::CpuRead)) {
        return std::nullopt;
    }

    std::memcpy(&value, reinterpret_cast<const void*>(source), sizeof(value));
    return value;
}

template <typename T>
T ReadValue(u64 address, size_t offset) {
    return TryRead<T>(address, offset).value_or(T{});
}
bool HasVtable(u64 object, std::span<const u64> vtables, std::string_view site);
bool HasVtable(u64 object, u64 vtable, std::string_view site);
bool IsCharacter(u64 object, std::string_view site);
bool HasVirtual(u64 object, u64 slot, u64 function, std::string_view site);

template <typename T>
bool WriteValue(u64 address, size_t offset, const T& value) {
    if (address > std::numeric_limits<u64>::max() - offset) {
        return false;
    }
    const u64 destination = address + offset;
    if (!HasMemoryAccess(destination, sizeof(value), MemoryProt::CpuWrite)) {
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(destination), &value, sizeof(value));
    return true;
}
bool WriteTransportedSummonPlacement(u64 state, const SummonPlacementDescriptor& placement);
bool IsUsablePackedMap(u32 packed_region);

bool IsSameSummonPlacement(const SummonPlacementDescriptor& left,
                           const SummonPlacementDescriptor& right);

u64 GetSummonManagerRoot();
u64 GetGlobalState();
u64 GetMatchingState();
u64 GetLocalPlayer();
bool HasPlayerEffect(u64 player, s32 effect_id);
u32 GetCurrentPackedMap();
Place CurrentPlace();
u32 PlayingMap();

// The co-op trace's records (see CoopTraceVersion).
void TraceLife(const char* event, TraceSampler::Fields more = {});
void Trace(std::string kind, TraceSampler::Fields fields);

bool RegisterLifecycleReset(std::string name, std::initializer_list<Lifecycle> events,
                            std::function<void()> reset);
void OnLifecycle(Lifecycle event);
void RefreshSeamlessLocalPlacement();
void ApplyCrossMapSummonHostPlacement(const GuestRegisterSnapshot& registers);

bool IsPartyGuestRole();
void ApplyCrossMapSummonGuestPlacement(const GuestRegisterSnapshot& registers);
void ApplySummonReloadStateAtDescriptor(const GuestRegisterSnapshot& registers);
void ApplyDeferredSummonReload();
void ApplyPreMatchCrossMapGuestWarp();
bool WarpGuestToLampPoint(const LampPoint& point);
// A trip to a warp point (a guest's own, the rig's): at the lamp's own point when it is known
// (returned), else the lamp lookup's trip.
const LampPoint* StartTripToLamp(s32 warp_point);
bool StartPlayerSummonSearch(u64 player, s32 effect_id);
void ResumePreMatchCrossMapResponder();
extern std::optional<PartyController> party_controller;
void InitPartyController();

bool HostHasRoomForMore();
// When the game last posted a summon search (NoteSummonSearchPosted), if ever.
std::optional<std::chrono::steady_clock::time_point> LastSummonSearchPost();

bool EventFlagOn(s32 flag);
void LogBloodstainStateChanges();
bool InPartySession();
// In the session and in its world (see SessionArrival): a summoned guest only once it arrived.
// Party death counts and posts this member as in the party only then.
bool InSessionWorld();
void ShowGuestBloodstain();
void KeepGuestBloodstainMark();

void ObserveLocalAliveState();
void LogOtherPlayersChanges();
void LogLocalEffectChanges();
void LogActionBlockingState();

std::vector<s32> EffectsOf(u64 character);
std::vector<s32> LocalEffects();

// ForcePlayAnimation(_, entity, animation) event binding, and the lamp touch common event 7200
// plays before it warps (Lua_Warp's 7725 does nothing on the player).
constexpr u64 ForcePlayAnimationOffset = 0x0132E290;
extern bool lamp_rest_hooks_installed;
extern std::atomic<u32> lamp_uses;
extern std::atomic<bool> rest_board_active;
extern std::atomic<s32> rest_board_lamp;
extern std::mutex rest_board_position_mutex;
extern std::optional<LampPosition> rest_board_position;
extern std::mutex rest_board_mutex;
extern std::vector<LampRest> rest_board;
extern std::chrono::steady_clock::time_point rest_board_time;
extern std::atomic<u32> party_map_now;
extern std::atomic<bool> trophy_board_active;
extern std::mutex trophy_mutex;
extern std::vector<s32> trophies_earned_in_party;
void NoteGameTrophyUnlock(s32 trophy);
void ReadTrophyBoard(std::span<const s32> board);
void UnlockPartyTrophies(bool sharing);
extern std::mutex flag_board_mutex;
extern std::vector<std::pair<s32, bool>> flags_to_post;
extern std::mutex echo_board_mutex;
extern std::vector<KillEchoes> echoes_to_post;
extern KillEchoLedger echo_ledger;
extern std::mutex kill_board_mutex;
extern std::vector<std::pair<s32, bool>> kills_to_post;
extern std::unordered_set<u32> others_kills;
// The kill board's standing kills by member, as the board client last read them.
extern PartyKills party_kills;
// The next kill board post asks for nothing older than now (since FreshStart), so the board takes
// back this member's posted kills first: the kills changed path (see WorldLogPath).
extern bool kill_board_fresh_start;
extern std::mutex enemy_hp_mutex;
extern MemberMapState local_enemy_hp;
extern std::vector<MemberMapState> others_enemy_hp;
extern std::mutex map_state_mutex;
extern MemberMapState local_map_state;
extern std::optional<MemberMapState> host_map_state;
extern std::mutex shown_board_mutex;
extern MemberMapState local_shown;
extern std::optional<MemberMapState> host_shown;
extern std::vector<std::pair<s32, bool>> flags_to_set;
void PostWorldChanges(const FlagChanges& changes);
extern std::mutex map_board_mutex;
extern std::vector<MemberMap> map_board;
extern std::string map_board_member;
extern std::chrono::steady_clock::time_point map_board_time;
s32 SessionPlayers();
std::optional<LampPosition> CharacterPosition(u64 character);
std::optional<LampPosition> LocalPlayerPosition();

s32 CharacterHp(u64 character);

// The other players' characters in this world (WorldChrMan +0x30 count, +0x38 entries of 0x38
// bytes; the local player is one of them), with HP (+0x128 of their data) and position.
struct OtherPlayer {
    u64 character = 0;
    s32 type = 0;
    s32 hp = 0;
    u8 dead_flags = 0;
    std::optional<LampPosition> at;
};
std::vector<OtherPlayer> OtherPlayers();
extern s32 guest_trip_point;
extern bool guest_trip_in_flight;
// A party wipe's trip is under way: its arrival checks for what the save left dead (see
// WipeTripRespawnsOnArrival).
extern std::atomic<bool> wipe_trip_respawn_pending;
void MoveToRespawnPoint(bool arriving_from_trip);
extern std::optional<std::chrono::steady_clock::time_point> guest_lamp_trip_at;
constexpr u64 WarpNextStageBonfireOffset = 0x0132E050;
void InstallLampsAsCampfires(u64 base, u64 image_size);
extern std::atomic<u32> coop_trace_file;
bool CoopTraceOn();

void TraceFrame(const char* kind, std::span<const u8> frame, u32 addr, u16 port,
                std::optional<bool> from_peer = std::nullopt);

void TraceBoard(const char* kind, const std::string& endpoint, std::optional<int> status,
                std::string_view body);
std::chrono::sys_seconds LocalTimeNow();
void StartCoopTrace();
extern std::condition_variable_any enemy_hp_wake;
extern bool enemy_hp_changed;
void StartRestBoardClient();

// Characters hidden before a party rest (see KeepDisabledAfterRespawn). WorldChrMan's blocks are
// +0x20 (count) / +0x28 (0x160 bytes each); a block's characters +0xB8 / +0xC0 (0x38 bytes each,
// the character first), the same walk as the respawn (0x01915330). A character's explicit
// disable is [+0x18]+0x20 bit 0, set by 0x018C6390 (the SetDisable binding's call).
constexpr u64 SetCharacterDisabledOffset = 0x018C6390;

template <typename Visit>
void ForEachWorldCharacter(u64 world, Visit&& visit) {
    const s32 blocks = ReadValue<s32>(world, 0x20);
    const u64 block_array = ReadValue<u64>(world, 0x28);
    for (s32 block = 0; block_array >= 0x10000 && block < std::min(blocks, 64); ++block) {
        const u64 entry = block_array + static_cast<u64>(block) * 0x160;
        const s32 count = ReadValue<s32>(entry, 0xB8);
        const u64 characters = ReadValue<u64>(entry, 0xC0);
        for (s32 index = 0; characters >= 0x10000 && index < std::min(count, 1024); ++index) {
            const u64 character = ReadValue<u64>(characters + static_cast<u64>(index) * 0x38, 0);
            if (character >= 0x10000 && HasMemoryAccess(character, 0x3B8, MemoryProt::CpuRead)) {
                visit(character);
            }
        }
    }
}
bool IsCharacterDisabled(u64 character);
void TickLampRest();

bool EnemiesAreHarder();
void KeepBossHpInStep();

// Diagnostics for a guest alone on a map (the host elsewhere): its enemies lagged and took no
// damage, the host runs them. For the 4 living characters nearest to the local player within
// 40 m: handle, type +0x78, network update authority +0x1EC, state flags +0x1E0, and the
// control block [+0x18] words +0x00..+0x3C, logged every 2 s on change to compare who runs an
// enemy on the host, on a guest beside it and on a guest alone.
void WatchEnemyHp(u64 hp_address, u32 handle);
void LogNearbyEnemyControl();

void TickPartyTrip();
void LogJoinTaskChanges();
void TickPlayersOnThisMap();

void TickAutoplay();
void TickPartyDeath();

void ReopenClosedPartySession();
void WatchSummonJoin();
bool PartyTripActive();
s64 SteadyMilliseconds();
void TickPartyController();

void PS4_SYSV_ABI TraceEntry(u64 tag, const GuestRegisterSnapshot* registers);
bool PS4_SYSV_ABI OverrideHostDeathRespawn(u64 tag, const GuestRegisterSnapshot* registers);
bool PS4_SYSV_ABI StayWhenTheHostLeaves(u64 tag, const GuestRegisterSnapshot* registers);

void LogGameCallChainAt(const char* what, u64 rsp);

// Seamless travel (see PartyTrip): the stage transition every trip goes through (lamps, the
// Dream's headstones, the event warps), and LeaveSession, which 01.09 calls from several places
// on the way (one is the stage change at 0x0193C689).
constexpr u64 StageTransitionEntryOffset = 0x013CDE30;
void SaveRemoteJoinTasks();
extern std::vector<s32> effects_before_trip;

void ParkEnemyWatch();
extern std::atomic<u64> party_trips_started;

// CSMultiPlayMan (0x05540230) keeps the join tasks in a vector at +0x10..+0x18 (see
// ShouldRecreateAfterTrip). The task's SetStep virtual (vtable +0x28) requests the next step.
constexpr u64 MultiPlayManPointerOffset = 0x05540230;

template <typename Visit>
void ForEachJoinTask(Visit&& visit) {
    const u64 manager = ReadValue<u64>(image_base + MultiPlayManPointerOffset, 0);
    if (manager < 0x10000 || !HasMemoryAccess(manager, 0x20, MemoryProt::CpuRead)) {
        return;
    }
    const u64 begin = ReadValue<u64>(manager, 0x10);
    const u64 end = ReadValue<u64>(manager, 0x18);
    if (begin < 0x10000 || end <= begin || end - begin > 8 * 16 ||
        !HasMemoryAccess(begin, end - begin, MemoryProt::CpuRead)) {
        return;
    }
    for (u64 entry = begin; entry < end; entry += 8) {
        const u64 task = ReadValue<u64>(entry, 0);
        if (task >= 0x10000 && HasMemoryAccess(task, 0xF0, MemoryProt::CpuRead)) {
            visit(task);
        }
    }
}

// What the game does when a member joins (0x0178C20C): allocate 0x100 bytes from the allocator
// at 0x05540438 (vtable +0x58), construct the task (0x01E4F9F0: task, member id, kind) and add it
// to CSMultiPlayMan +0x08 (0x01E55A70). Member ids are 0x20 bytes: SprjSessionManager
// (0x05540290) lists the session's at +0xF8..+0x100 and holds the local one at +0xD0; the game
// compares them with 0x00CA1C70.
constexpr u64 SessionManagerPointerOffset = 0x05540290;
constexpr u64 MemberIdEqualOffset = 0x00CA1C70;
extern std::vector<SavedJoinTask> saved_join_tasks;
extern std::map<u64, MemberDataWait> joins_waiting_for_data;
// `build_missing`: build a task for a member that has none (after a trip, whose load emptied the
// list). Not after a join: a member without a task there is one whose join the game is still
// making, and the game's own join (0x0178C1E3) makes no task for a member that already has one,
// so one built there first (as kind 0, rig smoke 2026-10-12) kept that guest invisible.
void RecreateRemotePlayers(std::span<const u64> only, bool build_missing);
std::optional<u32> HostMapFromItsPost();
std::vector<u64> OtherSessionMembers();

std::optional<std::string> SessionMemberBoardName(u64 member,
                                                  const std::vector<std::string>& online_ids);
extern bool restore_search_pending;
void RestoreSearchAfterTrip(bool in_session);
void PlayGuestAsHost(bool in_session);
void TurnOffMultiplayerWalls(bool in_session, u32 map);
u64 FindLoadedParam(std::string_view type);
void KeepGuestFullHp();
void MakeEnemiesHarder();
void PolishMultiplayerUi();
void InstallNoLevelSyncInParty(u64 base, u64 image_size);
void InstallSolidNpcsInParty(u64 base, u64 image_size);

// Diagnostics for shared loot (RE trace only): the map item module (MapItemMan, 0x017D-0x017E)
// asks "am I a summoned guest" (matching state 6) or "am I the host" (3) at these spots. Counting
// which run while a guest walks up to and picks up an item shows where the guest is held back.
struct ItemModuleCheck {
    u64 offset;
    std::array<u8, 7> bytes;
};
constexpr std::array<u8, 7> GuestStateCheck{0x83, 0xB8, 0x24, 0x01, 0x00, 0x00, 0x06};
constexpr std::array<u8, 7> GuestStateCheckRcx{0x83, 0xB9, 0x24, 0x01, 0x00, 0x00, 0x06};
constexpr std::array<u8, 7> HostStateCheck{0x83, 0xB8, 0x24, 0x01, 0x00, 0x00, 0x03};

constexpr auto ItemModuleChecks = std::to_array<ItemModuleCheck>({
    {0x017D3759, GuestStateCheck},
    {0x017D4F54, GuestStateCheck},
    {0x017D5642, GuestStateCheck},
    {0x017D6818, GuestStateCheck},
    {0x017D6AD2, GuestStateCheck},
    {0x017D83CB, GuestStateCheck},
    {0x017D853B, GuestStateCheck},
    {0x017D88BB, GuestStateCheck},
    {0x017D9AA8, GuestStateCheckRcx},
    {0x017D9E1C, GuestStateCheck},
    {0x017D9EE0, GuestStateCheckRcx},
    {0x017DD17C, GuestStateCheck},
    {0x017DDCD9, GuestStateCheck},
    {0x017DE5EC, GuestStateCheck},
    {0x017DE738, GuestStateCheck},
    {0x017DE79D, HostStateCheck},
    {0x017DE8CB, GuestStateCheck},
    {0x017DE925, HostStateCheck},
});
extern std::array<std::atomic<u64>, ItemModuleChecks.size()> item_module_check_hits;
void PS4_SYSV_ABI CountItemModuleCheck(u64 tag, const GuestRegisterSnapshot*);

void SetGuestPickupsBeforeMapItems();

// Diagnostics: who adds echoes (AddSouls 0x018FC600(player, amount, _, apply rates)).
constexpr u64 AddSoulsOffset = 0x018FC600;

bool PostsKillEchoesInSession();
void InstallSharedLoot(u64 base, u64 image_size);
void InstallItemModuleDiagnostics(u64 base, u64 image_size);
void LogItemModuleChecks();
constexpr u64 MapItemManPointerOffset = 0x0553D6E0;
extern std::array<std::atomic<u64>, 6> item_button_counts;
void InstallItemButtonDiagnostics(u64 base, u64 image_size);
std::optional<std::array<float, 3>> MapItemSearchPlayerPosition();

struct MapItemSpot {
    u32 handle;
    std::array<float, 3> at;
};
std::vector<std::pair<float, MapItemSpot>> UsableMapItemsNear(const std::array<float, 3>& from);
void LogItemButtonAndMapItems();
extern std::atomic<int> character_slot;

size_t SetGuestWorldChanges();

std::optional<s32> CountSetFlags();

// From the map item creation hook: before a load creates the guest's map items.
size_t ApplyHostMapState();

// A copy of the whole event flag store (see ChangedFlags): the manager's block map at +0x38 is a
// tree (left +0x00, right +0x10, nil marker +0x19, block id +0x20); a block's bits are at
// [node+0x30] (kind 2 at +0x28) or in the pool +0x28 at index [node+0x30] (kind 1), +0x20 bytes
// each.
struct FlagBlock {
    u32 id = 0;
    std::vector<u8> bytes;
    u64 address = 0;
};

struct FlagStore {
    u32 flags_per_block = 0;
    std::vector<FlagBlock> blocks;
    // Every node and block was read (none unmapped, the walk not cut short).
    bool whole = false;
};
FlagStore ReadFlagStore();
void LogWatchedWrites();
void InstallLampsStayLitAtRegistration(u64 base, u64 image_size);

// A party guest's boss item (see DefersItemLotAward): held at the award's entry and given
// from the tick once the boss-clear sequence is over.
constexpr u64 AwardItemLotOffset = 0x017DDC50;
extern thread_local bool giving_deferred_item_lot;
void GiveDeferredBossItems();
void InstallKeepLocalFlags(u64 base, u64 image_size);
void InstallDeferredBossItems(u64 base, u64 image_size);
extern std::atomic<s64> world_changed_ms;
void LogFlagChangesInSession(bool sharing);
void KeepGuestProgress(bool in_session);
void SetPartyWorldChanges(bool sharing);
void GrantPartyEchoes();
// Whether this guest hid or switched off the character to match the host (game thread).
bool ChangedLikeTheHost(u32 handle);
void ClaimEnemiesWhenAlone(bool in_session, u32 map);
extern std::mutex own_hits_mutex;
extern OwnDamage own_damage;
void InstallOwnHitCount(u64 base, u64 image_size);
extern DirectDamageInbox direct_damage_inbox;
extern std::mutex direct_boards_mutex;
extern DirectInbox<DirectVitals> direct_vitals_inbox;
extern DirectInbox<DirectKills> direct_kills_inbox;
extern std::mutex peer_hello_mutex;
extern std::optional<PeerHello> own_hello;
bool TakeDirectDamage();
// A co-op frame to every room peer, or to one (both traced).
void SendToRoomPeers(const std::vector<u8>& frame);
void SendToPeer(const std::vector<u8>& frame, u32 addr, u16 port);
void SendDirectKills(std::span<const u32> kills, bool changed);
// The world log's runtime (bloodborne_world_log.cpp, docs/WORLD_SYNC.md Stage C).
// SHADPS4_BLOODBORNE_WORLD_LOG (this game's hello says so: WorldLogFeature).
bool WorldLogSwitchOn();
// A peer's hello, by sender ((addr << 16) | port): which peers have the log.
void NoteWorldLogHello(u64 sender, const PeerHello& hello);
// A world log frame from a room peer (game thread).
void TakeWorldLogFrame(u8 type, std::span<const u8> payload, u32 addr, u16 port);
// The kill step's view of the party's kills: `members` the other members' (the log's or the kill
// board's), `all` with this member's own and held ones and, on the board path, the peers' direct
// frames. Decides the path, runs the log (or posts `changes` to the board) on the way.
struct PartyKillsNow {
    std::unordered_set<u32> all;
    std::unordered_set<u32> members;
};
PartyKillsNow GatherPartyKills(const VisitKills& visit_kills, u32 map,
                               std::span<const std::pair<s32, bool>> changes);
void SendDirectDamage(const MemberMapState& mine, bool changed);
void SendPeerHello();
void KeepEnemyDeathsInStep(bool sharing);
void SnapshotHostMapState(bool sharing);
void TeleportLocalPlayer(const LampPosition& at, float heading = 0.0f);
// Has every camera jump to where it should be on its next update (see PlacementCalls).
void SnapCamera();
// Moves the local player the way the game places it at a lamp (see PlaceCharacter); `what` names
// the caller in the character check's log.
bool PlaceLocalPlayer(const std::array<float, 4>& position, const std::array<float, 4>& rotation,
                      const char* what);
std::optional<float> LocalPlayerHeading();

void FinishWaitingJoins(bool in_session);
void ResendOwnDataOnChange(bool in_session);
void LogMemberDataChanges(bool in_session);
u64 OwnGameData();
void InstallSeamlessTravel(u64 base, u64 image_size);
void EndSessionOnWorldTeardown();
void ReviveGuestAfterGhostDeath();
extern std::mutex vitals_mutex;
extern VitalsBoard vitals_board;
extern std::string vitals_member;
extern std::optional<std::chrono::steady_clock::time_point> vitals_heard_at;

struct VitalsView {
    VitalsBoard board;
    std::string member;
    s64 age_ms = std::numeric_limits<s64>::max();

    bool Fresh() const {
        return age_ms <= 3'000;
    }
};
VitalsView ReadVitalsBoard();
void PostVitals(const MemberVitals& mine, std::vector<ReviveHitTotal> hits);
void StartVitalsBoardClient();

// The local player's side of party death, on the game thread.
struct PartyDeathRun {
    // Down ids start somewhere new each run, so the party's hits on an old down never count.
    DownedState downed{static_cast<u32>(std::random_device{}() % 0x40000000U) + 1};
    HoldToConfirm give_up{GiveUpHoldMs};
    ReviveHits revive_hits;
    u32 seen_catches = 0;
    u32 seen_attacks = 0;
    u32 seen_lamp_uses = 0;
    u32 handled_wipe = 0;
    u8 flags_added = 0;
    u8 hide_before = 0;
    u8 silence_before = 0;
    u32 targets_forgotten = 0; // since the last log line
    DownedTeams downed_teams;  // the downed members' bodies here, struck back up
    // After a revive the player takes no damage for ReviveGraceMs: the party's last swings, still
    // landing on its body as the invaders' team on their screens, would put it straight down again.
    std::optional<s64> grace_until;
    u8 grace_flags = 0;
    u32 real_hits_logged = 0;
};
extern PartyDeathRun party_death_run;
void InstallPartyDeath(u64 base, u64 image_size);
void ApplyHealingFountainHostAvailability(const GuestRegisterSnapshot& registers);
u64 GetSummonManager();
void WriteHex(std::ostream& out, u64 value);
bool VerifySites();

} // namespace Coop

using namespace Coop;

} // namespace Core::Bloodborne
