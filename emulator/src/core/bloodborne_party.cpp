// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "core/bloodborne_flags.h"
#include "core/bloodborne_lamp_points.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne {

namespace {

// Must match shadNet's NormalizePartyKey so both sides agree on what a party is.
constexpr std::size_t MaxPartyKeySize = 128;

bool IsWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

} // namespace

std::optional<std::string> ParsePartySetting(std::string_view setting) {
    while (!setting.empty() && IsWhitespace(setting.front())) {
        setting.remove_prefix(1);
    }
    while (!setting.empty() && IsWhitespace(setting.back())) {
        setting.remove_suffix(1);
    }
    if (setting.empty() || setting.size() > MaxPartyKeySize) {
        return std::nullopt;
    }
    for (const char c : setting) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return std::nullopt;
        }
    }
    const std::size_t colon = setting.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon == setting.size() - 1) {
        return std::nullopt;
    }
    return std::string{setting};
}

bool StartSummonSearch(u64 player, s32 effect_id, AddSpEffectFn add_sp_effect) {
    if (player == 0 || add_sp_effect == nullptr) {
        return false;
    }
    add_sp_effect(player, effect_id, player, 0, 1, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
    return true;
}

bool ShouldEndSessionOnWorldTeardown(s32 matching_state, s32 reload_state, bool reload_pending) {
    const bool in_session =
        matching_state == MatchingStateSummonedGuest || matching_state == MatchingStateHosting;
    return in_session && reload_state != SummonReloadInProgress && !reload_pending;
}

std::vector<u8> HexBytes(std::string_view hex) {
    std::vector<u8> bytes;
    for (size_t at = 0; at + 1 < hex.size(); at += 3) {
        u8 value = 0;
        if (hex.substr(at, 2) != "??") {
            std::from_chars(hex.data() + at, hex.data() + at + 2, value, 16);
        }
        bytes.push_back(value);
    }
    return bytes;
}

bool MatchesHexPattern(std::span<const u8> bytes, std::string_view pattern) {
    const std::vector<u8> expected = HexBytes(pattern);
    if (bytes.size() != expected.size()) {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (pattern.substr(index * 3, 2) != "??" && bytes[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

std::vector<std::string_view> AcceptedVariants(const CoopSite& site) {
    std::vector<std::string_view> variants;
    for (std::string_view rest = site.accepted; !rest.empty();) {
        const size_t bar = rest.find('|');
        variants.push_back(rest.substr(0, bar));
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
    }
    return variants;
}

bool MatchesCoopSite(std::span<const u8> bytes, const CoopSite& site) {
    return MatchesHexPattern(bytes, site.original) ||
           std::ranges::any_of(AcceptedVariants(site), [bytes](std::string_view variant) {
               return MatchesHexPattern(bytes, variant);
           });
}

const CoopFeature* FindCoopFeature(std::string_view name) {
    const auto found = std::ranges::find_if(
        CoopFeatures, [name](const CoopFeature& feature) { return feature.name == name; });
    return found == CoopFeatures.end() ? nullptr : &*found;
}

std::vector<CodePatch> CoopPatchesOf(std::string_view feature) {
    std::vector<CodePatch> patches;
    for (const CoopSite& site : CoopSites) {
        if (site.feature == feature && site.kind == CoopSiteKind::Patch) {
            patches.push_back({site.offset, HexBytes(site.original), HexBytes(site.patched)});
        }
    }
    return patches;
}

const CoopSite* FirstMismatchedCoopSite(std::span<const u8> image, std::string_view feature) {
    for (const CoopSite& site : CoopSites) {
        if (site.feature != feature) {
            continue;
        }
        const size_t size = HexBytes(site.original).size();
        if (site.offset > image.size() || size > image.size() - site.offset ||
            !MatchesCoopSite(image.subspan(site.offset, size), site)) {
            return &site;
        }
    }
    return nullptr;
}

bool IsRegisteredCoopHook(u64 offset, std::span<const u8> bytes) {
    return std::ranges::any_of(CoopSites, [offset, bytes](const CoopSite& site) {
        return site.kind == CoopSiteKind::Hook && site.offset == offset &&
               std::ranges::equal(HexBytes(site.original), bytes);
    });
}

std::string CoopSitesJson() {
    const auto quoted = [](std::string_view text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        return out + '"';
    };
    std::string json = "{\n  \"generated_from\": \"emulator/src/core/bloodborne_patch_sites.h\",\n"
                       "  \"features\": [\n";
    for (size_t index = 0; index < CoopFeatures.size(); ++index) {
        const CoopFeature& feature = CoopFeatures[index];
        json += fmt::format("    {{\"name\": {}, \"required\": {}}}{}\n", quoted(feature.name),
                            feature.required ? "true" : "false",
                            index + 1 < CoopFeatures.size() ? "," : "");
    }
    json += "  ],\n  \"sites\": [\n";
    for (size_t index = 0; index < CoopSites.size(); ++index) {
        const CoopSite& site = CoopSites[index];
        const char* kind = site.kind == CoopSiteKind::Patch  ? "patch"
                           : site.kind == CoopSiteKind::Hook ? "hook"
                                                             : "call";
        std::string accepted;
        for (const std::string_view variant : AcceptedVariants(site)) {
            accepted += (accepted.empty() ? "" : ", ") + quoted(variant);
        }
        json += fmt::format("    {{\"name\": {}, \"feature\": {}, \"kind\": \"{}\", "
                            "\"offset\": \"0x{:08X}\", \"original\": {}, \"patched\": {}, "
                            "\"accepted\": [{}]}}{}\n",
                            quoted(site.name), quoted(site.feature), kind, site.offset,
                            quoted(site.original), quoted(site.patched), accepted,
                            index + 1 < CoopSites.size() ? "," : "");
    }
    return json + "  ]\n}\n";
}

void CoopFeatureReport::Set(std::string_view feature, std::optional<std::string> skipped) {
    const auto found = std::ranges::find_if(
        results, [feature](const Result& result) { return result.feature == feature; });
    if (found != results.end()) {
        found->skipped = std::move(skipped);
    } else {
        results.push_back({std::string{feature}, std::move(skipped)});
    }
}

void CoopFeatureReport::On(std::string_view feature) {
    Set(feature, std::nullopt);
}

void CoopFeatureReport::Skipped(std::string_view feature, std::string reason) {
    Set(feature, std::move(reason));
}

std::string CoopFeatureReport::Summary() const {
    std::string skipped;
    size_t on = 0;
    for (const Result& result : results) {
        if (!result.skipped) {
            ++on;
            continue;
        }
        skipped +=
            fmt::format("{}{} [{}]", skipped.empty() ? "" : ", ", result.feature, *result.skipped);
    }
    std::string summary = fmt::format("Co-op features: {}/{} on", on, results.size());
    if (!skipped.empty()) {
        summary += fmt::format(" (skipped: {})", skipped);
    }
    return summary;
}

std::vector<std::string> CoopFeatureReport::SkippedRequired() const {
    std::vector<std::string> required;
    for (const Result& result : results) {
        const CoopFeature* feature = FindCoopFeature(result.feature);
        if (result.skipped && feature != nullptr && feature->required) {
            required.push_back(result.feature);
        }
    }
    return required;
}

const std::vector<CodePatch>& GhostDeathKeepSessionPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("ghost death");
    return patches;
}

const std::vector<CodePatch>& GuestBloodstainPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("guest bloodstain");
    return patches;
}

const std::vector<CodePatch>& NoEchoTheftPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("no echo theft");
    return patches;
}

const std::vector<CodePatch>& HostDeathKeepGuestsPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("host death");
    return patches;
}

const std::vector<CodePatch>& BossClearKeepSessionPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("boss clear");
    return patches;
}

bool StartsTripOnWorldReload(u64 caller_offset, bool trip_active, bool in_party_session) {
    return IsWorldReloadStopRequest(caller_offset) && !trip_active && in_party_session;
}

bool IsWorldReloadStopRequest(u64 caller_offset) {
    constexpr u64 WorldLoadStopRequestReturn = 0x0193C65C;
    return caller_offset == WorldLoadStopRequestReturn;
}

const std::vector<CodePatch>& JoinAnywherePatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("join anywhere");
    return patches;
}

namespace {
u64 DecodeRel32(u8 opcode, u64 offset, std::span<const u8> bytes) {
    if (bytes.size() != 5 || bytes[0] != opcode) {
        return 0;
    }
    s32 displacement;
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    return offset + bytes.size() + static_cast<s64>(displacement);
}
} // namespace

u64 DecodeNearCallTarget(u64 offset, std::span<const u8> bytes) {
    return DecodeRel32(0xE8, offset, bytes);
}

u64 DecodeNearJumpTarget(u64 offset, std::span<const u8> bytes) {
    return DecodeRel32(0xE9, offset, bytes);
}

namespace {

bool HasReviveCalls(const GhostDeathCalls& calls) {
    return calls.set_event_flag && calls.clear_return_state && calls.request_full_recover &&
           calls.update_blood_mark && calls.revive_player && calls.revive_local_character &&
           calls.restore_alive_state && calls.chr_reset_animation && calls.warp_to_last_lamp;
}

// What both deaths share. `character_type` is set back after the local revive makes the player
// a live one (only a guest needs that).
void ReviveInPlace(const GhostDeathCalls& calls, std::optional<s32> character_type) {
    calls.set_event_flag(0, DeathEventFlag, false);
    calls.clear_return_state();
    calls.request_full_recover();
    calls.update_blood_mark();
    calls.revive_player();
    calls.revive_local_character();
    calls.restore_alive_state();
    if (character_type) {
        calls.set_chr_type(0, LocalPlayerEntity, *character_type);
    }
    calls.chr_reset_animation(0, LocalPlayerEntity);
    calls.warp_to_last_lamp();
}

} // namespace

const LampPoint* FindLampPoint(s32 warp_point) {
    const auto found = std::ranges::lower_bound(LampPoints, warp_point, {}, &LampPoint::warp_point);
    return found != LampPoints.end() && found->warp_point == warp_point ? &*found : nullptr;
}

bool ArrivalNeedsMove(const LampPoint* planned, const std::optional<LampPosition>& at) {
    if (planned == nullptr || !at) {
        return true;
    }
    const float dx = (*at)[0] - planned->x;
    const float dy = (*at)[1] - planned->y;
    const float dz = (*at)[2] - planned->z;
    return dx * dx + dy * dy + dz * dz > ArrivedAtLampMeters * ArrivedAtLampMeters;
}

bool PlaceCharacter(const PlacementCalls& calls, u64 character, const float* position,
                    const float* rotation) {
    if (!calls.teleport || !calls.snap_position || !calls.set_rotation || !calls.reset_follow ||
        !calls.snap_camera) {
        return false;
    }
    calls.teleport(character, position, rotation, 1);
    calls.snap_position(character, position);
    calls.set_rotation(character, rotation);
    calls.reset_follow(character, true);
    calls.snap_camera();
    return true;
}

bool ReviveDeadPartyGuest(const GhostDeathCalls& calls) {
    if (!HasReviveCalls(calls) || !calls.is_white_ghost || !calls.set_chr_type ||
        !calls.is_white_ghost()) {
        return false;
    }
    ReviveInPlace(calls, WhiteGhostCharacterType);
    return true;
}

bool ShouldReviveHostInPlace(s32 matching_state) {
    return matching_state == MatchingStateHosting || matching_state == MatchingStateSummonedGuest;
}

void ReviveDeadPartyHost(const GhostDeathCalls& calls) {
    if (HasReviveCalls(calls)) {
        ReviveInPlace(calls, std::nullopt);
    }
}

std::array<u8, 7> EncodeSignTimerStore(u8 modrm, float seconds) {
    const auto value = std::bit_cast<u32>(seconds);
    return {0xC7,
            modrm,
            0x28,
            static_cast<u8>(value),
            static_cast<u8>(value >> 8),
            static_cast<u8>(value >> 16),
            static_cast<u8>(value >> 24)};
}

bool IsSignTimerStore(std::span<const u8> bytes, const SignTimerSite& site) {
    const auto store = EncodeSignTimerStore(site.modrm, site.original_seconds);
    constexpr size_t TimeStart = 3;
    return bytes.size() >= store.size() &&
           std::ranges::equal(bytes.first(TimeStart), std::span{store}.first(TimeStart));
}

bool IsLampTravelFlag(s32 flag) {
    // 7MMM01LL: map code MMM (241 = m24_01), lamp LL (see FlagTable).
    return RuleFor(flag).Has(FlagLampTrip);
}

bool LampRestsEnabled(const char* setting) {
    return setting != nullptr && std::string_view{setting} == "1";
}

bool ShouldRecreateAfterTrip(s32 kind, s32 step) {
    return (kind == RemoteHostTask || kind == RemoteGuestTask) && step == InSessionStep;
}

std::string EncodeMapUpdate(std::string_view member, u32 map) {
    std::array<char, 8> hex{};
    const auto [end, error] = std::to_chars(hex.data(), hex.data() + hex.size(), map, 16);
    return std::string{member} + ' ' + std::string{hex.data(), end};
}

std::optional<std::vector<MemberMap>> ParseMapBoard(std::string_view body) {
    std::vector<MemberMap> board;
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        if (space == std::string_view::npos || space == 0) {
            return std::nullopt;
        }
        const std::string_view map_text = line.substr(space + 1);
        u32 map = 0;
        const auto [parsed_end, error] =
            std::from_chars(map_text.data(), map_text.data() + map_text.size(), map, 16);
        if (error != std::errc{} || parsed_end != map_text.data() + map_text.size()) {
            return std::nullopt;
        }
        board.push_back(MemberMap{std::string{line.substr(0, space)}, map});
    }
    return board;
}

bool EveryoneElseOnMap(std::span<const MemberMap> board, std::string_view self, u32 map) {
    bool anyone_else = false;
    for (const MemberMap& entry : board) {
        if (entry.member == self) {
            continue;
        }
        if (entry.map != map) {
            return false;
        }
        anyone_else = true;
    }
    return anyone_else;
}

bool NoOneElseOnMap(std::span<const MemberMap> board, std::string_view self, u32 map) {
    if (map == 0) {
        return false;
    }
    return std::ranges::none_of(
        board, [&](const MemberMap& entry) { return entry.member != self && entry.map == map; });
}

namespace {
bool IsOnlineIdChar(u8 c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
           c == '_';
}
} // namespace

std::string BoardMemberOfOnlineId(std::string_view online_id) {
    std::string member;
    for (const char c : online_id) {
        member += fmt::format("{:02x}", static_cast<u8>(c));
    }
    return member;
}

std::string OnlineIdOfBoardMember(std::string_view member) {
    const auto nibble = [](char c) -> s32 {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    if (member.empty() || member.size() % 2 != 0) {
        return {};
    }
    std::string online_id;
    for (size_t index = 0; index < member.size(); index += 2) {
        const s32 high = nibble(member[index]);
        const s32 low = nibble(member[index + 1]);
        if (high < 0 || low < 0 || !IsOnlineIdChar(static_cast<u8>(high * 16 + low))) {
            return {};
        }
        online_id += static_cast<char>(high * 16 + low);
    }
    return online_id;
}

std::optional<std::string> FindOnlineIdIn(std::span<const u8> bytes,
                                          std::span<const std::string> candidates) {
    std::optional<std::string> found;
    for (const std::string& candidate : candidates) {
        if (candidate.empty() || candidate.size() >= bytes.size()) {
            continue;
        }
        for (size_t at = 0; at + candidate.size() < bytes.size(); ++at) {
            if (!std::equal(candidate.begin(), candidate.end(), bytes.begin() + at) ||
                bytes[at + candidate.size()] != 0 || (at > 0 && IsOnlineIdChar(bytes[at - 1]))) {
                continue;
            }
            if (found && *found != candidate) {
                return std::nullopt; // two members' names: not this member's object
            }
            found = candidate;
            break;
        }
    }
    return found;
}

RecreatePlan PlanRecreate(std::span<const std::optional<std::string>> members,
                          std::span<const MemberMap> board, std::string_view self, u32 map,
                          const std::set<std::string>& handled) {
    RecreatePlan plan;
    if (std::ranges::any_of(members, [](const auto& member) { return !member.has_value(); })) {
        if (EveryoneElseOnMap(board, self, map)) {
            for (size_t index = 0; index < members.size(); ++index) {
                plan.now.push_back(index);
            }
            plan.done = true;
        }
        return plan;
    }
    plan.done = true;
    std::vector<size_t> here;
    bool someone_new = false;
    for (size_t index = 0; index < members.size(); ++index) {
        const std::string& name = *members[index];
        const auto entry = std::ranges::find_if(
            board, [&](const MemberMap& member) { return member.member == name; });
        const bool on_map = entry != board.end() && entry->map == map;
        if (on_map) {
            here.push_back(index);
        }
        if (handled.contains(name)) {
            continue;
        }
        if (on_map) {
            someone_new = true;
        } else {
            plan.done = false;
        }
    }
    // A later arrival re-creates everyone here again: making its copy lost the one made before
    // (Kaniel 2026-10-11: Lisara no longer saw krolebord once the host arrived after her).
    if (someone_new) {
        plan.now = std::move(here);
    }
    return plan;
}

bool HostIsHere(std::optional<u32> host_map, std::span<const MemberMap> board,
                std::string_view self, u32 map) {
    return host_map && *host_map == map && map != 0 &&
           std::ranges::any_of(board, [&](const MemberMap& entry) {
               return entry.member != self && entry.map == map;
           });
}

bool HostAway(std::optional<u32> host_map, std::span<const MemberMap> board, std::string_view self,
              u32 map) {
    return host_map ? !HostIsHere(host_map, board, self, map) : NoOneElseOnMap(board, self, map);
}

bool SomeoneElsewhere(std::span<const MemberMap> board, std::string_view self, u32 map) {
    return std::ranges::any_of(
        board, [&](const MemberMap& entry) { return entry.member != self && entry.map != map; });
}

bool PostsKillEchoes(std::span<const MemberMap> board, std::string_view self, u32 map) {
    return SomeoneElsewhere(board, self, map) &&
           std::ranges::none_of(board, [&](const MemberMap& entry) {
               return entry.member != self && entry.map == map && entry.member < self;
           });
}

bool RoomForMore(bool hosting, size_t in_session, size_t party_online) {
    constexpr size_t MaxPlayers = 3; // the host and two cooperators
    return hosting && in_session < MaxPlayers && party_online > in_session;
}

std::optional<size_t> PickSavedJoinTask(std::span<const SavedJoinTask> saved, s32 kind,
                                        u64 identity) {
    std::optional<size_t> only;
    size_t of_kind = 0;
    for (size_t index = 0; index < saved.size(); ++index) {
        if (saved[index].kind != kind) {
            continue;
        }
        if (saved[index].identity == identity) {
            return index;
        }
        ++of_kind;
        only = index;
    }
    return of_kind == 1 ? only : std::nullopt;
}

bool RebuildsFromSave(const SavedJoinTask& saved) {
    return saved.step > CreateCharacterStep;
}

bool HasAllMemberData(u8 parts) {
    return (parts & MemberDataParts) == MemberDataParts;
}

MemberDataWait::Action MemberDataWait::Tick(s64 now_ms, bool member_here, u8 parts) {
    if (!member_here) {
        here_since.reset();
        requested_at.reset();
        return Action::None;
    }
    if (!here_since) {
        here_since = now_ms;
    }
    if (now_ms - *here_since < SettleMs) {
        return Action::None;
    }
    if (requested_at && HasAllMemberData(parts)) {
        return Action::Create;
    }
    if (!requested_at || now_ms - *requested_at >= RepeatMs) {
        requested_at = now_ms;
        ++requests;
        return Action::Request;
    }
    return Action::None;
}

bool OwnDataWatch::Observe(std::optional<u64> fingerprint, s64 now_ms) {
    if (!fingerprint) {
        sent.reset();
        pending.reset();
        return false;
    }
    if (!sent) {
        sent = fingerprint;
        return false;
    }
    if (*fingerprint == *sent) {
        pending.reset();
        return false;
    }
    if (pending != fingerprint) {
        pending = fingerprint;
        pending_since = now_ms;
    }
    if (now_ms - pending_since < SettleMs) {
        return false;
    }
    sent = fingerprint;
    pending.reset();
    return true;
}

std::vector<SavedJoinTask> MergeSavedJoinTasks(std::span<const SavedJoinTask> before,
                                               std::span<const SavedJoinTask> found) {
    std::vector<SavedJoinTask> merged{found.begin(), found.end()};
    for (const SavedJoinTask& task : before) {
        if (std::ranges::none_of(
                found, [&](const SavedJoinTask& now) { return now.identity == task.identity; })) {
            merged.push_back(task);
        }
    }
    return merged;
}

std::string EncodeTrophyUpdate(std::string_view member, std::span<const s32> trophies) {
    std::string body{member};
    for (const s32 trophy : trophies) {
        body += ' ' + std::to_string(trophy);
    }
    return body;
}

std::optional<std::vector<s32>> ParseTrophyBoard(std::string_view body) {
    std::vector<s32> trophies;
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        s32 trophy = 0;
        const auto [parsed_end, error] =
            std::from_chars(line.data(), line.data() + line.size(), trophy);
        if (error != std::errc{} || parsed_end != line.data() + line.size() || trophy < 0) {
            return std::nullopt;
        }
        trophies.push_back(trophy);
    }
    return trophies;
}

std::vector<s32> TrophiesToUnlock(std::span<const s32> board, std::span<const s32> handled) {
    std::vector<s32> missing;
    for (const s32 trophy : board) {
        if (std::ranges::find(handled, trophy) == handled.end()) {
            missing.push_back(trophy);
        }
    }
    return missing;
}

std::optional<int> CharacterSlotOfSavePath(std::string_view path) {
    constexpr std::string_view prefix = "/savedata0/userdata000";
    if (path.size() != prefix.size() + 1 || !path.starts_with(prefix)) {
        return std::nullopt;
    }
    const char digit = path.back();
    if (digit < '0' || digit > '9') {
        return std::nullopt;
    }
    return digit - '0';
}

LampUnblock LampBytesToClear(bool blocked, bool missing, bool host_effect, bool in_party_session) {
    return {blocked && (host_effect || in_party_session), missing && in_party_session};
}

void SummonJoinWatch::Delivered(Clock::time_point now) {
    if (!since_ || (reported_ && now - last_ >= NewSummonGap)) {
        since_ = now;
        reported_ = false;
    }
    last_ = now;
}

bool SummonJoinWatch::Stalled(bool joining, Clock::time_point now) {
    if (joining) {
        since_.reset();
        reported_ = false;
        return false;
    }
    if (!since_ || reported_ || now - *since_ < Patience) {
        return false;
    }
    reported_ = true;
    return true;
}

bool IsOtherCharacter(s32 progress_now, s32 progress_kept) {
    // Event flags mostly add up as a character plays; far fewer set means another character. A
    // load clears some area flags (dozens to a few hundred: krolebord's restarts read 5800 set
    // against 5859 kept), so "far" is a quarter of what was kept.
    const s32 slack = std::max<s32>(20, progress_kept / 4);
    // No world has 0 flags set: that is a store read mid-load, not a character.
    return progress_kept > 0 && progress_now > 0 && progress_now + slack < progress_kept;
}

std::optional<s32> SettledFlagCount(std::optional<s32> first, std::optional<s32> second) {
    if (!first || !second || *first <= 0 || *second <= 0) {
        return std::nullopt;
    }
    // Two reads a moment apart differ by a few flags at most; more is a read torn by the load.
    const s32 slack = std::max<s32>(20, std::max(*first, *second) / 100);
    if (std::abs(*first - *second) > slack) {
        return std::nullopt;
    }
    return std::max(*first, *second);
}

bool ForgetsSlotLists(std::optional<s32> progress_now, s32 progress_kept, bool new_in_slot) {
    if (!progress_now || *progress_now <= 0) {
        return false;
    }
    return new_in_slot || IsOtherCharacter(*progress_now, progress_kept);
}

std::string SlotBackupName(std::string_view file, std::string_view stamp) {
    return fmt::format("{}.bak-{}", file, stamp);
}

std::vector<std::string> SlotBackupsToRemove(std::string_view file,
                                             std::span<const std::string> names, size_t keep) {
    const std::string prefix = SlotBackupName(file, "");
    std::vector<std::string> backups;
    for (const std::string& name : names) {
        if (name.size() > prefix.size() && name.starts_with(prefix)) {
            backups.push_back(name);
        }
    }
    std::ranges::sort(backups);
    if (backups.size() <= keep) {
        return {};
    }
    backups.resize(backups.size() - keep);
    return backups;
}

bool IsNewCharacterInSlot(std::span<const s32> confirmed, size_t confirmed_set_now) {
    return !confirmed.empty() && confirmed_set_now == 0;
}

bool IsMapPickupFlag(s32 flag) {
    return RuleFor(flag).Has(FlagMapPickup);
}

std::string FormatFlagList(std::span<const s32> flags) {
    std::string text;
    for (const s32 flag : flags) {
        text += std::to_string(flag) + '\n';
    }
    return text;
}

std::vector<s32> ParseFlagList(std::string_view text) {
    std::vector<s32> flags;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        s32 flag = 0;
        const auto [parsed_end, error] =
            std::from_chars(line.data(), line.data() + line.size(), flag);
        if (!line.empty() && error == std::errc{} && parsed_end == line.data() + line.size()) {
            flags.push_back(flag);
        }
    }
    return flags;
}

FlagChanges ChangedFlags(u32 block, u32 flags_per_block, std::span<const u8> before,
                         std::span<const u8> after) {
    FlagChanges changes;
    const size_t bytes = std::min(before.size(), after.size());
    for (size_t byte = 0; byte < bytes; ++byte) {
        const u8 differs = before[byte] ^ after[byte];
        if (differs == 0) {
            continue;
        }
        for (u32 bit = 0; bit < 8; ++bit) {
            const u8 mask = static_cast<u8>(0x80 >> bit);
            if ((differs & mask) == 0) {
                continue;
            }
            const s32 flag = static_cast<s32>(block * flags_per_block + byte * 8 + bit);
            ((after[byte] & mask) != 0 ? changes.set : changes.cleared).push_back(flag);
        }
    }
    return changes;
}

bool CarriesToGuestWorld(s32 flag) {
    return RuleFor(flag).Has(FlagCarriesHome);
}

bool GameThreadStalled(s64 now_ms, s64 last_tick_ms) {
    return last_tick_ms > 0 && now_ms - last_tick_ms >= GameStallMs;
}

std::span<const s32> ShopLineFlags() {
    static constexpr auto flags = FlagsWith<FlagShopLine>();
    return flags;
}

bool RechecksShopLines(bool party_configured, int slot, bool done_for_slot, u32 map) {
    constexpr u32 HuntersDream = 0x15000000;
    return party_configured && slot >= 0 && !done_for_slot && map != 0 && map != HuntersDream;
}

std::vector<s32> GuestProgressFlags(bool guest, bool in_session, u32 map) {
    constexpr u32 HuntersDream = 0x15000000;
    if (!guest || !in_session || map == 0 || map == HuntersDream) {
        return {};
    }
    const auto flags = FlagsWith<FlagGuestSetsInWorld>();
    return {flags.begin(), flags.end()};
}

std::optional<std::pair<s32, s32>> CoopHpScale(s32 unscaled_max, s32 max, s32 hp,
                                               std::span<const s32> effects) {
    // SpEffectParam maxHpRate of the co-op boss effects 7500, 7501, 7502.
    static constexpr std::array<float, 3> Rates{1.5f, 2.0f, 2.5f};
    if (unscaled_max <= 0 || hp <= 0 || max != unscaled_max) {
        return std::nullopt;
    }
    for (const s32 effect : effects) {
        if (effect >= 7500 && effect < 7500 + static_cast<s32>(Rates.size())) {
            const float rate = Rates[effect - 7500];
            return std::pair{static_cast<s32>(std::lround(static_cast<float>(max) * rate)),
                             static_cast<s32>(std::lround(static_cast<float>(hp) * rate))};
        }
    }
    return std::nullopt;
}

bool SetsGuestWorldChangesOnLoad(bool in_session, bool on_trip) {
    return !in_session || on_trip;
}

std::string FormatCrashLine(u32 code, u64 address, u64 image_base, u64 image_size,
                            std::string_view thread) {
    const bool in_game = address >= image_base && address - image_base < image_size;
    return fmt::format("crash: exception {:#x} at {:#x} ({}) thread {}", code, address,
                       in_game ? fmt::format("eboot+{:#x}", address - image_base) : "emulator",
                       thread.empty() ? "?" : thread);
}

std::string FormatGameAddresses(std::span<const u64> words, u64 image_base, u64 image_size,
                                size_t max) {
    std::string text;
    size_t found = 0;
    for (const u64 word : words) {
        if (found == max) {
            break;
        }
        if (word >= image_base && word - image_base < image_size) {
            text += fmt::format(" eboot+{:#x}", word - image_base);
            ++found;
        }
    }
    return text;
}

bool IsBossFogEntryFlag(s32 flag) {
    return RuleFor(flag).Has(FlagBossFogEntry);
}

bool KeepsLocalFlag(bool party_configured, s32 flag) {
    // The messengers' gifts and pick, each boss's "found" flag (see FlagTable).
    return party_configured && RuleFor(flag).owner == FlagOwner::PerPlayer;
}

std::span<const EncounterInsight> EncounterInsights() {
    // {defeat event, found flag, insight}: each map's xx1800/xx1700/xx1850 defeat event and the
    // entrance event beside it (xx1802, xx1702, xx1852, or xx1801/xx1851 in the Chalice and DLC
    // maps) that starts 9350 behind the found flag.
    static constexpr std::array<EncounterInsight, 20> bosses{{
        {12101800, 9343, 1}, {12101850, 9307, 5}, {12201800, 9338, 1}, {12301800, 9339, 1},
        {12301700, 9340, 1}, {12401800, 9301, 1}, {12411700, 9300, 1}, {12411800, 9336, 1},
        {12421800, 9304, 3}, {12501800, 9341, 1}, {12601800, 9306, 3}, {12601850, 9342, 2},
        {12801800, 9305, 1}, {13201800, 9303, 2}, {13301800, 9302, 3}, {13401800, 9344, 1},
        {13401850, 9302, 1}, {13501800, 9346, 1}, {13501850, 9348, 2}, {13601800, 9347, 5},
    }};
    return bosses;
}

std::optional<s32> TracedFlagSetting(const char* setting) {
    if (setting == nullptr) {
        return std::nullopt;
    }
    const std::string_view text{setting};
    s32 flag = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), flag);
    if (error != std::errc{} || end != text.data() + text.size() || flag <= 0) {
        return std::nullopt;
    }
    return flag;
}

FlagBit FlagBitLocation(s32 flag, u32 flags_per_block) {
    const u32 within = static_cast<u32>(flag) % flags_per_block;
    return {static_cast<u32>(flag) / flags_per_block, within / 8,
            static_cast<u8>(0x80 >> (within % 8))};
}

std::vector<s32> LampFlagsKeptAtRegistration(bool party_configured, s32 base, u16 on_before) {
    std::vector<s32> kept;
    if (!party_configured || base < 0) {
        return kept;
    }
    for (s32 index = 0; index < 12; ++index) {
        if ((on_before >> index) & 1) {
            kept.push_back(base + index);
        }
    }
    return kept;
}

DeathRespawn ChooseDeathRespawn(s32 point, bool in_loaded_block, bool arriving_from_trip,
                                bool reset) {
    if (point <= 0) {
        return DeathRespawn::StayPut;
    }
    if (reset && !arriving_from_trip) {
        return DeathRespawn::Travel;
    }
    if (in_loaded_block) {
        return DeathRespawn::MoveToPoint;
    }
    return arriving_from_trip ? DeathRespawn::StayPut : DeathRespawn::Travel;
}

bool WipeTripRespawnsOnArrival(bool reset, DeathRespawn respawn) {
    return reset && respawn == DeathRespawn::Travel;
}

std::vector<std::pair<s32, bool>> FlagsToShare(const FlagChanges& changes) {
    std::vector<std::pair<s32, bool>> shared;
    for (const s32 flag : changes.set) {
        if (SharedWithParty(flag)) {
            shared.emplace_back(flag, true);
        }
    }
    return shared;
}

bool SharedWithParty(s32 flag) {
    // A lamp's active flags, a boss fog's "entered" flag (see FlagTable).
    return RuleFor(flag).owner == FlagOwner::Shared;
}

std::optional<s32> LampOfActiveFlag(s32 flag) {
    constexpr s32 ActiveFlagBase = 10007810;
    const s32 offset = flag - ActiveFlagBase;
    if (offset < 0) {
        return std::nullopt;
    }
    const s32 map = offset / 10000;
    const s32 within = offset % 10000;
    if (map < 210 || map >= 400 || within % 20 != 0 || within / 20 >= 20) {
        return std::nullopt;
    }
    return map * 10000 + 1950 + within / 20;
}

bool IsBulkFlagChange(size_t changed_flags) {
    return changed_flags > 64;
}

void RecordWorldChanges(std::map<s32, bool>& world, const FlagChanges& changes) {
    for (const s32 flag : changes.set) {
        if (CarriesToGuestWorld(flag)) {
            world[flag] = true;
        }
    }
    for (const s32 flag : changes.cleared) {
        if (CarriesToGuestWorld(flag)) {
            world[flag] = false;
        }
    }
}

std::string FormatWorldChanges(const std::map<s32, bool>& world) {
    std::string text;
    for (const auto& [flag, on] : world) {
        text += std::to_string(flag) + (on ? " 1\n" : " 0\n");
    }
    return text;
}

std::map<s32, bool> ParseWorldChanges(std::string_view text) {
    std::map<s32, bool> world;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto space = line.find(' ');
        if (space == std::string_view::npos || space + 2 != line.size()) {
            continue;
        }
        s32 flag = 0;
        const auto [parsed_end, error] = std::from_chars(line.data(), line.data() + space, flag);
        const char state = line[space + 1];
        if (error == std::errc{} && parsed_end == line.data() + space &&
            (state == '0' || state == '1') && CarriesToGuestWorld(flag)) {
            world[flag] = state == '1';
        }
    }
    return world;
}

std::string EncodeFlagUpdate(std::string_view member, s64 since,
                             std::span<const std::pair<s32, bool>> changes) {
    std::string body = std::string{member} + ' ' + std::to_string(since);
    for (const auto& [flag, on] : changes) {
        body += ' ' + std::to_string(flag) + (on ? ":1" : ":0");
    }
    return body;
}

std::optional<FlagBoardAnswer> ParseFlagBoard(std::string_view body) {
    FlagBoardAnswer answer;
    const auto first_end = body.find('\n');
    const std::string_view first = body.substr(0, first_end);
    const auto [latest_end, latest_error] =
        std::from_chars(first.data(), first.data() + first.size(), answer.latest);
    if (latest_error != std::errc{} || latest_end != first.data() + first.size()) {
        return std::nullopt;
    }
    body = first_end == std::string_view::npos ? std::string_view{} : body.substr(first_end + 1);
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        s32 flag = 0;
        const auto [flag_end, flag_error] =
            std::from_chars(line.data(), line.data() + std::min(space, line.size()), flag);
        if (space == std::string_view::npos || flag_error != std::errc{} ||
            flag_end != line.data() + space || line.size() != space + 2 ||
            (line[space + 1] != '0' && line[space + 1] != '1')) {
            return std::nullopt;
        }
        answer.changes.emplace_back(flag, line[space + 1] == '1');
    }
    return answer;
}

std::string EncodeResumePoint(const ResumePoint& point) {
    return fmt::format("{} {} {} {} {}", point.map, point.position[0], point.position[1],
                       point.position[2], point.progress);
}

std::optional<ResumePoint> ParseResumePoint(std::string_view text) {
    ResumePoint point;
    const char* cursor = text.data();
    const char* end = text.data() + text.size();
    const auto [map_end, map_error] = std::from_chars(cursor, end, point.map);
    if (map_error != std::errc{}) {
        return std::nullopt;
    }
    cursor = map_end;
    for (float& coordinate : point.position) {
        if (cursor == end || *cursor != ' ') {
            return std::nullopt;
        }
        ++cursor;
        const auto [next, error] = std::from_chars(cursor, end, coordinate);
        if (error != std::errc{}) {
            return std::nullopt;
        }
        cursor = next;
    }
    if (cursor == end || *cursor != ' ') {
        return std::nullopt;
    }
    ++cursor;
    const auto [progress_end, progress_error] = std::from_chars(cursor, end, point.progress);
    if (progress_error != std::errc{}) {
        return std::nullopt;
    }
    cursor = progress_end;
    while (cursor != end && (*cursor == '\n' || *cursor == '\r' || *cursor == ' ')) {
        ++cursor;
    }
    if (cursor != end) {
        return std::nullopt;
    }
    return point;
}

s32 MapWarpPoint(u32 packed_map) {
    constexpr u32 HuntersDream = 0x15000000;
    constexpr s32 HuntersDreamHeadstones = 2102962;
    if (packed_map == HuntersDream) {
        return HuntersDreamHeadstones;
    }
    const s32 area = static_cast<s32>((packed_map >> 24) & 0xFF);
    const s32 block = static_cast<s32>((packed_map >> 16) & 0xFF);
    return (area * 10 + block) * 10000 + 2950;
}

ResumeStep ChooseResumeStep(const ResumePoint& point, u32 map, const LampPosition& at, s32 progress,
                            u32 loaded_map) {
    if (point.map == 0 || (loaded_map != 0 && loaded_map != map)) {
        return {};
    }
    if (IsOtherCharacter(progress, point.progress)) {
        return {ResumeStep::Kind::Forget, 0};
    }
    if (point.map != map) {
        return {ResumeStep::Kind::Travel, MapWarpPoint(point.map)};
    }
    const float dx = point.position[0] - at[0];
    const float dy = point.position[1] - at[1];
    const float dz = point.position[2] - at[2];
    if (dx * dx + dy * dy + dz * dz <= 5.0f * 5.0f) {
        return {};
    }
    return {ResumeStep::Kind::Move, 0};
}

s32 MapFlagBase(u32 packed_map) {
    const s32 area = static_cast<s32>((packed_map >> 24) & 0xFF);
    const s32 block = static_cast<s32>((packed_map >> 16) & 0xFF);
    return (1000 + area * 10 + block) * MapFlagCount;
}

bool TakesHostMapFlag(s32 flag) {
    return RuleFor(flag).Has(FlagTakesHostMap);
}

std::string EncodeMapStateUpdate(std::string_view member, u32 map, std::span<const u8> bits) {
    std::string body = std::string{member} + ' ' + std::to_string(map);
    if (!bits.empty()) {
        body += ' ';
        for (const u8 byte : bits) {
            body += fmt::format("{:02x}", byte);
        }
    }
    return body;
}

std::optional<std::vector<MemberMapState>> ParseMapStateBoard(std::string_view body) {
    std::vector<MemberMapState> states;
    const auto nibble = [](char c) -> s32 {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        if (space == std::string_view::npos) {
            return std::nullopt;
        }
        MemberMapState state;
        const auto [map_end, error] = std::from_chars(line.data(), line.data() + space, state.map);
        const std::string_view hex = line.substr(space + 1);
        if (error != std::errc{} || map_end != line.data() + space || hex.size() % 2 != 0) {
            return std::nullopt;
        }
        for (size_t index = 0; index < hex.size(); index += 2) {
            const s32 high = nibble(hex[index]);
            const s32 low = nibble(hex[index + 1]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            state.bits.push_back(static_cast<u8>(high * 16 + low));
        }
        states.push_back(std::move(state));
    }
    return states;
}

u64 VisitTracker::Update(u32 new_map, u64 new_player, bool in_session, u64 new_trips) {
    // A new player character is a load: outside a session, or onto another map (a summon there).
    // A map change with the same character is a walk across a border (Central Yharnam into the
    // Cathedral Ward): it loads nothing and the enemies stay as they are, so the visit goes on.
    const bool reloaded = new_player != player && (!in_session || new_map != seen_map);
    if (new_trips != trips || reloaded || generation == 0) {
        ++generation;
        map = new_map;
    }
    seen_map = new_map;
    player = new_player;
    trips = new_trips;
    return (u64{map} << 32) | generation;
}

std::optional<PlaceChange> PlaceTracker::Update(const PlaceReading& reading) {
    const bool known_map = reading.map != 0 && reading.map != 0xFFFFFFFFu;
    trip_seen = trip_seen || reading.trip;
    if (reading.player != 0 && reading.dead) {
        death_seen = true;
    }
    if (reading.loading || reading.player == 0) {
        pending.reset();
        last_at.reset();
        jumped_ms.reset();
        if (place.state == PlaceState::Loading) {
            return std::nullopt;
        }
        return Move(PlaceState::Loading, place.map, false, LoadCause(), reading.now_ms);
    }
    // Another character object: the world was rebuilt between two ticks (a reload the ticks did
    // not see). VisitTracker has always ended a visit there too.
    if (player != 0 && reading.player != player) {
        arrival_due = true;
    }
    player = reading.player;
    if (reading.at && last_at &&
        std::hypot((*reading.at)[0] - (*last_at)[0], (*reading.at)[1] - (*last_at)[1],
                   (*reading.at)[2] - (*last_at)[2]) > TeleportMeters) {
        jumped_ms = reading.now_ms;
    }
    if (reading.at) {
        last_at = reading.at;
    }
    if (!known_map) {
        return std::nullopt; // a failed read keeps the place, and a pending crossing
    }
    if (place.state == PlaceState::Unknown || place.state == PlaceState::Loading || arrival_due) {
        const PlaceCause cause = LoadCause();
        arrival_due = false;
        trip_seen = false;
        death_seen = false;
        pending.reset();
        jumped_ms.reset();
        return Move(PlaceState::Playing, reading.map, true, cause, reading.now_ms);
    }
    if (place.state == PlaceState::Cutscene) {
        if (reading.cutscene) {
            return std::nullopt; // what the map reads while a cutscene plays is not followed
        }
        return Move(PlaceState::Playing, place.map, false, PlaceCause::CutsceneEnd, reading.now_ms);
    }
    // Playing. Up again without a load (a party death revive): the next load is no death reload.
    if (!reading.dead) {
        death_seen = false;
    }
    if (reading.cutscene) {
        pending.reset();
        return Move(PlaceState::Cutscene, place.map, false, PlaceCause::Cutscene, reading.now_ms);
    }
    if (reading.map == place.map) {
        pending.reset(); // back before the crossing committed
        return std::nullopt;
    }
    if (jumped_ms && reading.now_ms - *jumped_ms <= TeleportWindowMs) {
        pending.reset();
        jumped_ms.reset();
        return Move(PlaceState::Playing, reading.map, true, PlaceCause::Teleport, reading.now_ms);
    }
    if (!pending || pending->first != reading.map) {
        pending = std::pair{reading.map, reading.now_ms};
        return std::nullopt;
    }
    if (reading.now_ms - pending->second < BorderSteadyMs) {
        return std::nullopt;
    }
    pending.reset();
    return Move(PlaceState::Playing, reading.map, true, PlaceCause::Border, reading.now_ms);
}

std::optional<PlaceChange> PlaceTracker::Move(PlaceState state, u32 map, bool arrival,
                                              PlaceCause cause, s64 now_ms) {
    PlaceChange change{.from = place, .cause = cause};
    place.state = state;
    place.map = map;
    place.visit += arrival ? 1 : 0;
    place.since_ms = now_ms;
    change.to = place;
    return change;
}

// A death and its reload come before a trip: a party wipe's respawn travels to the lamp.
PlaceCause PlaceTracker::LoadCause() const {
    return death_seen ? PlaceCause::DeathReload : trip_seen ? PlaceCause::Lamp : PlaceCause::Load;
}

std::string_view PlaceStateName(PlaceState state) {
    switch (state) {
    case PlaceState::Unknown:
        return "Unknown";
    case PlaceState::Loading:
        return "Loading";
    case PlaceState::Cutscene:
        return "Cutscene";
    case PlaceState::Playing:
        return "Playing";
    }
    return "?";
}

std::string_view PlaceCauseName(PlaceCause cause) {
    switch (cause) {
    case PlaceCause::Load:
        return "Load";
    case PlaceCause::Border:
        return "Border";
    case PlaceCause::Lamp:
        return "Lamp";
    case PlaceCause::Teleport:
        return "Teleport";
    case PlaceCause::DeathReload:
        return "DeathReload";
    case PlaceCause::Cutscene:
        return "Cutscene";
    case PlaceCause::CutsceneEnd:
        return "CutsceneEnd";
    }
    return "?";
}

std::string FormatPlaceChange(const PlaceChange& change) {
    return fmt::format("Co-op place: {} -> {} map {:#x} visit {} ({})",
                       PlaceStateName(change.from.state), PlaceStateName(change.to.state),
                       change.to.map, change.to.visit, PlaceCauseName(change.cause));
}

std::string_view LifecycleName(Lifecycle event) {
    switch (event) {
    case Lifecycle::Wipe:
        return "wipe";
    case Lifecycle::Trip:
        return "trip";
    case Lifecycle::Leave:
        return "leave";
    case Lifecycle::Join:
        return "join";
    case Lifecycle::NewCharacter:
        return "new character";
    case Lifecycle::MemberLeft:
        return "member left";
    }
    return "?";
}

size_t MemberDepartures::Tick(bool in_session, bool steady, size_t others, s64 now_ms) {
    if (!in_session) {
        known = 0;
        fewer_since.reset();
        return 0;
    }
    if (others >= known) {
        known = others;
        fewer_since.reset();
        return 0;
    }
    if (!steady) {
        fewer_since.reset();
        return 0;
    }
    if (!fewer_since) {
        fewer_since = now_ms;
        return 0;
    }
    if (now_ms - *fewer_since < SettleMs) {
        return 0;
    }
    const size_t left = known - others;
    known = others;
    fewer_since.reset();
    return left;
}

void LifecycleResets::Register(std::string name, std::initializer_list<Lifecycle> events,
                               std::function<void()> reset) {
    u32 bits = 0;
    for (const Lifecycle event : events) {
        bits |= 1u << static_cast<u32>(event);
    }
    entries.push_back(Entry{std::move(name), bits, std::move(reset)});
}

std::vector<std::string> LifecycleResets::Dispatch(Lifecycle event) {
    // A trip keeps the session (seamless travel), and so does another member leaving it;
    // everything else here starts a new one.
    if (event != Lifecycle::Trip && event != Lifecycle::MemberLeft) {
        ++epoch;
    }
    std::vector<std::string> ran;
    for (const Entry& entry : entries) {
        if ((entry.events & (1u << static_cast<u32>(event))) != 0) {
            entry.reset();
            ran.push_back(entry.name);
        }
    }
    return ran;
}

namespace {
template <typename T>
void AppendLittleEndian(std::vector<u8>& bytes, T value) {
    for (size_t index = 0; index < sizeof(T); ++index) {
        bytes.push_back(static_cast<u8>(static_cast<u64>(value) >> (8 * index)));
    }
}

template <typename T>
T ReadLittleEndian(std::span<const u8> bytes, size_t offset) {
    u64 value = 0;
    for (size_t index = 0; index < sizeof(T); ++index) {
        value |= u64{bytes[offset + index]} << (8 * index);
    }
    return static_cast<T>(value);
}
} // namespace

void SharedDamage::ForgetDamageBefore(std::span<const EnemyHpReport> others) {
    for (const EnemyHpReport& report : others) {
        before_reset.insert(report.visit);
    }
}

SharedDamage::Step SharedDamage::Update(u64 new_visit, bool now_active,
                                        std::span<const EnemyCopy> alive,
                                        std::span<const EnemyHpReport> others, s64 now_ms) {
    if (new_visit != visit || now_active != active) {
        tracks.clear();
        written.clear();
        written_by.clear();
        held_since.clear();
        visit = new_visit;
        active = now_active;
    }
    Step step;
    if (!active) {
        return step;
    }
    for (const EnemyCopy& copy : alive) {
        if (copy.hp <= 0 || copy.max <= 0) {
            continue;
        }
        auto [track, first] = tracks.try_emplace(copy.handle, Track{copy.hp, 0});
        if (first) {
            track->second.hits_before = copy.own_hits.value_or(0);
        }
        Track& seen = track->second;
        // This member's own damage: what its own hits took off (counted by the game's hit
        // function), or else everything the board did not write.
        const s32 hits = copy.own_hits ? static_cast<s32>(*copy.own_hits - seen.hits_before) : 0;
        // The rest of the drop: other players' hits the game sent (after a trip a guest's hits
        // still reach the host's copy), which pays for their posts before anything is written.
        s32 sent = seen.start - copy.hp - seen.written - hits;
        if (sent < 0) { // it came back up (a reload, a heal): count from here
            seen.start = copy.hp + seen.written + hits;
            seen.paid = 0;
            sent = 0;
        }
        const s32 own = copy.own_hits ? hits : sent;
        if (!copy.own_hits) {
            sent = 0; // not told apart: all of it is this member's
        }
        s32 unpaid = std::max(sent - seen.paid, 0);
        if (copy.own_hits) {
            // Counted hit by hit: a world showing its characters at 1 HP is no damage.
            if (own > 0) {
                step.post.push_back({copy.handle, own, copy.max});
            }
        } else {
            // Two reads must agree: a world showing its characters at 1 HP for one read (hidden
            // or loading) posted that as damage.
            const s32 confirmed = std::min(own, seen.last_own);
            seen.last_own = own;
            if (confirmed > 0) {
                step.post.push_back({copy.handle, confirmed, copy.max});
            }
        }
        // Each other member's damage on its copy, as a share of this copy's max HP, on top of
        // what this copy already took from that member (from the board or through the game).
        s32 hp = copy.hp;
        for (const EnemyHpReport& report : others) {
            if ((report.visit & HostHpReport) != 0) {
                continue; // HP from one world, not damage
            }
            if (before_reset.contains(report.visit)) {
                continue; // damage on the world before a map reset
            }
            for (const EnemyHp& damage : report.enemies) {
                if (damage.handle != copy.handle || damage.max <= 0 || damage.hp <= 0) {
                    continue;
                }
                const s32 scaled =
                    static_cast<s32>((s64{damage.hp} * copy.max + damage.max / 2) / damage.max);
                const std::pair<u64, u32> key{report.visit, copy.handle};
                s32& taken = written[key];
                s32 delta = scaled - taken;
                if (delta <= 0) {
                    continue;
                }
                const s32 paid = std::min(delta, unpaid);
                unpaid -= paid;
                seen.paid += paid;
                taken += paid;
                delta -= paid;
                if (delta <= 0) {
                    held_since.erase(key);
                    continue;
                }
                // A member whose hits the game sends here too: wait a moment for its copy.
                if (now_ms != 0 && game_sends.contains(report.visit)) {
                    const auto [since, fresh] = held_since.try_emplace(key, now_ms);
                    if (now_ms - since->second < SharedDamageHoldMs) {
                        continue;
                    }
                }
                held_since.erase(key);
                if (hp <= 1) {
                    continue;
                }
                const s32 lowered = std::max(hp - delta, 1);
                taken += hp - lowered;
                seen.written += hp - lowered;
                written_by[key] += hp - lowered;
                hp = lowered;
            }
        }
        // The game's copy of a hit already written from a report (it came straight over the P2P
        // link first): the write is given back, and that member's damage is held from now on.
        if (const s32 back = std::min(unpaid, seen.written); back > 0) {
            s32 left = back;
            for (auto& [key, amount] : written_by) {
                if (key.second != copy.handle || amount <= 0 || left <= 0) {
                    continue;
                }
                const s32 part = std::min(amount, left);
                amount -= part;
                left -= part;
                game_sends.insert(key.first);
            }
            seen.written -= back;
            seen.paid += back;
            hp = std::min(hp + back, copy.max);
        }
        if (hp != copy.hp) {
            step.writes.push_back({copy.handle, hp});
        }
    }
    return step;
}

void OwnDamage::Hit(u32 handle, bool mine, s32 taken) {
    last_hit_mine[handle] = mine;
    if (mine && taken > 0) {
        totals[handle] += taken;
    }
}

void OwnDamage::Effect(u32 handle, s32 taken) {
    const auto last = last_hit_mine.find(handle);
    if (taken > 0 && last != last_hit_mine.end() && last->second) {
        totals[handle] += taken;
    }
}

s64 OwnDamage::Total(u32 handle) const {
    const auto total = totals.find(handle);
    return total != totals.end() ? total->second : 0;
}

std::chrono::milliseconds HpBoardWait(bool own_damage_changed) {
    return own_damage_changed ? std::chrono::milliseconds{0} : HpBoardRound;
}

std::vector<u8> EncodeDirectDamage(u32 map, std::span<const u8> report) {
    std::vector<u8> frame{'S', 'H', 'A', 'D', DirectDamageType};
    frame.reserve(frame.size() + 4 + report.size());
    AppendLittleEndian(frame, map);
    frame.insert(frame.end(), report.begin(), report.end());
    return frame;
}

std::optional<MemberMapState> DecodeDirectDamage(std::span<const u8> frame) {
    constexpr size_t Header = 5 + 4;
    if (frame.size() < Header || !std::ranges::equal(frame.first(4), std::string_view{"SHAD"}) ||
        frame[4] != DirectDamageType) {
        return std::nullopt;
    }
    MemberMapState state{ReadLittleEndian<u32>(frame, 5), {}};
    state.bits.assign(frame.begin() + Header, frame.end());
    if (!DecodeEnemyHp(state.bits)) {
        return std::nullopt;
    }
    return state;
}

void DirectDamageInbox::Take(u64 sender, MemberMapState state, s64 now_ms) {
    by_sender[sender] = Received{std::move(state), now_ms};
}

std::vector<MemberMapState> DirectDamageInbox::Fresh(s64 now_ms) {
    std::erase_if(by_sender, [now_ms](const auto& entry) {
        return now_ms - entry.second.at > DirectDamageLifeMs;
    });
    std::vector<const Received*> fresh;
    for (const auto& [sender, received] : by_sender) {
        fresh.push_back(&received);
    }
    std::ranges::stable_sort(fresh, std::greater{}, &Received::at);
    std::vector<MemberMapState> states;
    for (const Received* received : fresh) {
        states.push_back(received->state);
    }
    return states;
}

std::vector<EnemyHpReport> MergeEnemyHpReports(std::span<const EnemyHpReport> reports) {
    std::vector<EnemyHpReport> merged;
    for (const EnemyHpReport& report : reports) {
        const auto same = std::ranges::find(merged, report.visit, &EnemyHpReport::visit);
        if (same == merged.end()) {
            merged.push_back(report);
            continue;
        }
        const bool hp = (report.visit & HostHpReport) != 0;
        for (const EnemyHp& enemy : report.enemies) {
            const auto mine = std::ranges::find(same->enemies, enemy.handle, &EnemyHp::handle);
            if (mine == same->enemies.end()) {
                same->enemies.push_back(enemy);
            } else if (hp ? enemy.hp < mine->hp : enemy.hp > mine->hp) {
                *mine = enemy;
            }
        }
    }
    return merged;
}

std::vector<EnemyWrite> LowerToHostHp(std::span<const EnemyCopy> mine,
                                      std::span<const EnemyHpReport> reports) {
    std::vector<EnemyWrite> writes;
    for (const EnemyCopy& copy : mine) {
        std::optional<s32> lowest;
        for (const EnemyHpReport& report : reports) {
            if ((report.visit & HostHpReport) == 0) {
                continue;
            }
            for (const EnemyHp& enemy : report.enemies) {
                if (enemy.handle == copy.handle) {
                    if (const auto target = SharedEnemyHp(lowest.value_or(copy.hp), copy.max,
                                                          enemy.hp, enemy.max)) {
                        lowest = target;
                    }
                }
            }
        }
        if (lowest) {
            writes.push_back({copy.handle, *lowest});
        }
    }
    return writes;
}

std::vector<u8> EncodeEnemyHp(u64 visit, std::span<const EnemyHp> enemies) {
    std::vector<u8> bytes;
    bytes.reserve(8 + enemies.size() * 12);
    AppendLittleEndian(bytes, visit);
    for (const EnemyHp& enemy : enemies) {
        AppendLittleEndian(bytes, enemy.handle);
        AppendLittleEndian(bytes, static_cast<u32>(enemy.hp));
        AppendLittleEndian(bytes, static_cast<u32>(enemy.max));
    }
    return bytes;
}

std::optional<EnemyHpReport> DecodeEnemyHp(std::span<const u8> bytes) {
    if (bytes.size() < 8 || (bytes.size() - 8) % 12 != 0) {
        return std::nullopt;
    }
    EnemyHpReport report{ReadLittleEndian<u64>(bytes, 0), {}};
    for (size_t offset = 8; offset < bytes.size(); offset += 12) {
        report.enemies.push_back({ReadLittleEndian<u32>(bytes, offset),
                                  static_cast<s32>(ReadLittleEndian<u32>(bytes, offset + 4)),
                                  static_cast<s32>(ReadLittleEndian<u32>(bytes, offset + 8))});
    }
    return report;
}

std::optional<s32> LowestSharedHp(u32 handle, s32 hp, s32 max, u32 map,
                                  std::span<const MemberMapState> others) {
    std::optional<s32> lowest;
    for (const MemberMapState& state : others) {
        // Any block of the same area (packed map 0xAABBCCDD, area AA): a member who joined in
        // place stands on another block of it (Cathedral Ward next to Central Yharnam), and the
        // blocks give their enemies distinct handles; another area may reuse a handle.
        const auto report =
            (state.map >> 24) == (map >> 24) ? DecodeEnemyHp(state.bits) : std::nullopt;
        if (!report) {
            continue;
        }
        for (const EnemyHp& enemy : report->enemies) {
            if (enemy.handle != handle) {
                continue;
            }
            if (const auto target = SharedEnemyHp(lowest.value_or(hp), max, enemy.hp, enemy.max)) {
                lowest = target;
            }
        }
    }
    return lowest;
}

std::optional<s32> SharedEnemyHp(s32 hp, s32 max, s32 other_hp, s32 other_max) {
    if (hp <= 0 || max <= 0 || other_max <= 0 || other_hp < 0) {
        return std::nullopt;
    }
    // Rounded up: a sliver left on either side is still alive here.
    const s64 scaled = (s64{other_hp} * max + other_max - 1) / other_max;
    const s32 target = static_cast<s32>(std::max<s64>(scaled, 1));
    return target < hp ? std::optional<s32>{target} : std::nullopt;
}

std::vector<u8> EncodeShownCharacters(std::span<const CharacterShown> characters) {
    std::vector<u8> bytes;
    bytes.reserve(characters.size() * 5);
    for (const CharacterShown& character : characters) {
        AppendLittleEndian(bytes, character.handle);
        bytes.push_back(static_cast<u8>((character.hidden ? 1 : 0) | (character.disabled ? 2 : 0) |
                                        (character.distant ? 4 : 0) | (character.dead ? 8 : 0)));
    }
    return bytes;
}

bool IsDeadInItsWorld(u8 death_flags) {
    return (death_flags & 0x05) != 0;
}

std::unordered_set<u32> DeadInTheHostsWorld(std::span<const CharacterShown> host) {
    std::unordered_set<u32> dead;
    for (const CharacterShown& character : host) {
        if (character.dead) {
            dead.insert(character.handle);
        }
    }
    return dead;
}

bool DiesInsteadOfHiding(const std::optional<LampPosition>& me,
                         const std::optional<LampPosition>& enemy) {
    if (!me || !enemy) {
        return false;
    }
    float squared = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = (*enemy)[axis] - (*me)[axis];
        squared += delta * delta;
    }
    return squared < DiesNearMetres * DiesNearMetres;
}

bool FarFromHost(const std::optional<LampPosition>& host,
                 const std::optional<LampPosition>& character) {
    if (!host || !character) {
        return false;
    }
    float squared = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = (*character)[axis] - (*host)[axis];
        squared += delta * delta;
    }
    return squared > FarFromHostMetres * FarFromHostMetres;
}

std::optional<std::vector<CharacterShown>> DecodeShownCharacters(std::span<const u8> bytes) {
    if (bytes.size() % 5 != 0) {
        return std::nullopt;
    }
    std::vector<CharacterShown> characters;
    for (size_t offset = 0; offset < bytes.size(); offset += 5) {
        const u8 state = bytes[offset + 4];
        characters.push_back({ReadLittleEndian<u32>(bytes, offset), (state & 1) != 0,
                              (state & 2) != 0, (state & 4) != 0, (state & 8) != 0});
    }
    return characters;
}

std::vector<CharacterShown> ShownLikeTheHost(std::span<const CharacterShown> host,
                                             std::span<const CharacterShown> mine,
                                             const std::unordered_set<u32>& kill_hidden,
                                             const std::unordered_set<u32>& hidden_here) {
    std::unordered_map<u32, CharacterShown> hosts;
    for (const CharacterShown& character : host) {
        hosts[character.handle] = character;
    }
    std::vector<CharacterShown> changes;
    for (const CharacterShown& character : mine) {
        const auto it = hosts.find(character.handle);
        if (it == hosts.end() || kill_hidden.contains(character.handle)) {
            continue;
        }
        // Far from the host: only undo what this member did.
        const bool undo = character.hidden && hidden_here.contains(character.handle);
        const bool hidden = it->second.distant ? character.hidden && !undo : it->second.hidden;
        if (hidden != character.hidden) {
            changes.push_back({character.handle, hidden});
        }
    }
    return changes;
}

std::vector<CharacterShown> HostNearStates::Fill(u32 host_map,
                                                 std::span<const CharacterShown> host) {
    if (host_map != map) {
        map = host_map;
        near_states.clear();
    }
    std::vector<CharacterShown> filled;
    filled.reserve(host.size());
    for (const CharacterShown& character : host) {
        if (!character.distant) {
            near_states[character.handle] = character;
            filled.push_back(character);
        } else if (const auto it = near_states.find(character.handle); it != near_states.end()) {
            filled.push_back(it->second);
        } else {
            filled.push_back(character);
        }
    }
    return filled;
}

std::vector<CharacterSwitch> UndoneOnMapChange(const std::unordered_set<u32>& switched_off,
                                               std::span<const CharacterSwitch> mine) {
    std::vector<CharacterSwitch> changes;
    for (const CharacterSwitch& character : mine) {
        if (character.disabled && switched_off.contains(character.handle)) {
            changes.push_back({character.handle, false});
        }
    }
    return changes;
}

std::vector<CharacterSwitch> SwitchedLikeTheHost(std::span<const CharacterShown> host,
                                                 std::span<const CharacterSwitch> mine,
                                                 const std::unordered_set<u32>& switched_off,
                                                 const std::unordered_set<u32>& kill_hidden) {
    std::unordered_map<u32, bool> hosts; // off in the host's world: removed or switched off
    for (const CharacterShown& character : host) {
        hosts[character.handle] = !character.distant && (character.hidden || character.disabled);
    }
    std::vector<CharacterSwitch> changes;
    for (const CharacterSwitch& character : mine) {
        const auto it = hosts.find(character.handle);
        if (it == hosts.end() || kill_hidden.contains(character.handle)) {
            continue;
        }
        if (it->second && !character.disabled) {
            changes.push_back({character.handle, true});
        } else if (!it->second && character.disabled && switched_off.contains(character.handle)) {
            changes.push_back({character.handle, false});
        }
    }
    return changes;
}

bool WatchesKills(bool sharing, bool trip_active, bool loading) {
    return sharing && !trip_active && !loading;
}

bool CharacterSettles(s64 now_ms, s64 world_changed_ms, std::optional<s64> owner_changed_ms) {
    const s64 settle =
        std::chrono::duration_cast<std::chrono::milliseconds>(EnemyOwnerChangeSettles).count();
    return now_ms - world_changed_ms < settle ||
           (owner_changed_ms && now_ms - *owner_changed_ms < settle);
}

bool IsRealDeath(u8 death_flags) {
    constexpr u8 PutDownByTheWorld = 0x20;
    return (death_flags & 0x05) != 0 && (death_flags & PutDownByTheWorld) == 0;
}

std::vector<std::pair<s32, bool>> VisitKills::Observe(u64 visit,
                                                      std::span<const SeenCharacter> characters,
                                                      std::span<const u32> others_maps) {
    std::vector<std::pair<s32, bool>> changes;
    const auto map_of = [](u64 v) { return static_cast<u32>(v >> 32); };
    const auto someone_on = [&](u32 map) {
        return map != 0 && std::ranges::find(others_maps, map) != others_maps.end();
    };
    if (visit != current) {
        const u32 old_map = map_of(current);
        if (someone_on(old_map)) {
            auto& standing = held[old_map];
            for (const u32 handle : killed) {
                if (std::ranges::find(standing, handle) == standing.end()) {
                    standing.push_back(handle);
                }
            }
        } else {
            for (const u32 handle : killed) {
                changes.emplace_back(static_cast<s32>(handle), false);
            }
        }
        killed.clear();
        seen.clear();
        current = visit;
    }
    for (auto it = held.begin(); it != held.end();) {
        if (it->first == map_of(current) || someone_on(it->first)) {
            ++it;
            continue;
        }
        for (const u32 handle : it->second) {
            changes.emplace_back(static_cast<s32>(handle), false);
        }
        it = held.erase(it);
    }
    for (const auto& [handle, now, instance, settling, untouched_boss, others_kill] : characters) {
        const auto [it, added] = seen.try_emplace(handle, Seen{now, instance, untouched_boss});
        // The same object seen alive, now dead: a world that loaded brings new objects, some
        // dead already (in a save that killed them for good), and one that settles after a
        // takeover puts such ones down where they stand. Nor a boss-sized one last seen
        // untouched (see SeenCharacter::untouched_boss), nor another member's kill.
        if (!added && !settling && !others_kill && it->second.instance == instance &&
            it->second.hp > 0 && !it->second.untouched_boss && now <= 0 &&
            std::ranges::find(killed, handle) == killed.end()) {
            killed.push_back(handle);
            changes.emplace_back(static_cast<s32>(handle), true);
        }
        it->second = {now, instance, untouched_boss};
    }
    return changes;
}

std::vector<u32> VisitKills::Held(u32 map) const {
    const auto it = held.find(map);
    return it != held.end() ? it->second : std::vector<u32>{};
}

WorldLogKills VisitKills::Standing() const {
    WorldLogKills kills;
    for (const auto& [map, handles] : held) {
        kills[map].insert(handles.begin(), handles.end());
    }
    if (!killed.empty()) {
        kills[static_cast<u32>(current >> 32)].insert(killed.begin(), killed.end());
    }
    return kills;
}

std::vector<std::pair<s32, bool>> VisitKills::Forget() {
    std::vector<std::pair<s32, bool>> changes;
    for (const u32 handle : killed) {
        changes.emplace_back(static_cast<s32>(handle), false);
    }
    for (const auto& [map, handles] : held) {
        for (const u32 handle : handles) {
            if (std::ranges::find(killed, handle) == killed.end()) {
                changes.emplace_back(static_cast<s32>(handle), false);
            }
        }
    }
    killed.clear();
    held.clear();
    seen.clear();
    return changes;
}

bool AppliesMemberKill(bool member_killed, s32 hp) {
    return member_killed && hp > 0;
}

bool KilledByAnother(bool hooks, std::optional<bool> last_hit_mine, bool hosting) {
    if (!hooks) {
        return false;
    }
    return last_hit_mine ? !*last_hit_mine : !hosting;
}

bool GetsMemberKillDrop(bool in_session, bool member_kill, s32 lot_flag, bool flag_on) {
    return in_session && member_kill && lot_flag > 0 && !flag_on;
}

bool SharesWithParty(bool party_configured, bool player_loaded) {
    return party_configured && player_loaded;
}

bool SharesKillEchoes(u64 caller_offset, bool posts) {
    constexpr u64 EnemyKillReward = 0x018FC99B;
    constexpr u64 BossKillReward = 0x01385263;
    return posts && (caller_offset == EnemyKillReward || caller_offset == BossKillReward);
}

void KillEchoLedger::Paid(KillEchoes kill, s64 now_ms) {
    paid.emplace_back(kill, now_ms);
}

void KillEchoLedger::Received(KillEchoes kill, s64 now_ms) {
    received.emplace_back(kill, now_ms);
}

std::vector<KillEchoes> KillEchoLedger::Due(s64 now_ms) {
    std::vector<KillEchoes> due;
    std::vector<std::pair<KillEchoes, s64>> waiting;
    for (const auto& [kill, read_ms] : received) {
        const auto reward = std::ranges::find_if(paid, [&](const auto& entry) {
            return entry.first.amount == kill.amount &&
                   (kill.map == 0 || entry.first.map == kill.map) &&
                   entry.second >= read_ms - PaidWindowMs &&
                   entry.second <= read_ms + ReceivedGraceMs;
        });
        if (reward != paid.end()) {
            paid.erase(reward); // the game paid this member: it was on the kill's map
        } else if (now_ms - read_ms < ReceivedGraceMs) {
            waiting.emplace_back(kill, read_ms);
        } else {
            due.push_back(kill);
        }
    }
    received = std::move(waiting);
    std::erase_if(paid, [&](const auto& entry) {
        return entry.second < now_ms - PaidWindowMs - ReceivedGraceMs;
    });
    return due;
}

std::string EncodeEchoUpdate(std::string_view member, s64 since,
                             std::span<const KillEchoes> echoes) {
    std::string body = std::string{member} + ' ' + std::to_string(since);
    for (const KillEchoes& kill : echoes) {
        body += ' ' + std::to_string(kill.amount);
        if (kill.map != 0) {
            body += ':' + std::to_string(kill.map);
        }
    }
    return body;
}

std::optional<KillBoardAnswer> ParseKillBoard(std::string_view body) {
    KillBoardAnswer answer;
    const auto first_end = body.find('\n');
    const std::string_view first = body.substr(0, first_end);
    const auto [latest_end, latest_error] =
        std::from_chars(first.data(), first.data() + first.size(), answer.latest);
    if (latest_error != std::errc{} || latest_end != first.data() + first.size()) {
        return std::nullopt;
    }
    body = first_end == std::string_view::npos ? std::string_view{} : body.substr(first_end + 1);
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        const auto space = line.find(' ');
        s32 handle = 0;
        const auto [handle_end, handle_error] =
            std::from_chars(line.data(), line.data() + std::min(space, line.size()), handle);
        if (space == std::string_view::npos || handle_error != std::errc{} ||
            handle_end != line.data() + space || line.size() < space + 2 ||
            (line[space + 1] != '0' && line[space + 1] != '1') ||
            (line.size() > space + 2 && line[space + 2] != ' ')) {
            return std::nullopt;
        }
        const std::string_view member =
            line.size() > space + 3 ? line.substr(space + 3) : std::string_view{};
        answer.changes.push_back(KillChange{handle, line[space + 1] == '1', std::string{member}});
    }
    return answer;
}

void PartyKills::Apply(std::span<const KillChange> changes) {
    for (const KillChange& change : changes) {
        const u32 handle = static_cast<u32>(change.handle);
        if (change.killed) {
            by_member[change.member].insert(handle);
        } else if (const auto it = by_member.find(change.member); it != by_member.end()) {
            it->second.erase(handle);
        }
    }
}

bool PartyKills::Contains(u32 handle) const {
    return std::ranges::any_of(by_member,
                               [&](const auto& entry) { return entry.second.contains(handle); });
}

std::unordered_set<u32> PartyKills::All() const {
    std::unordered_set<u32> all;
    for (const auto& [member, handles] : by_member) {
        all.insert(handles.begin(), handles.end());
    }
    return all;
}

std::unordered_set<u32> PartyKills::AllExcept(std::string_view member) const {
    std::unordered_set<u32> all;
    for (const auto& [name, handles] : by_member) {
        if (name != member) {
            all.insert(handles.begin(), handles.end());
        }
    }
    return all;
}

std::optional<EchoBoardAnswer> ParseEchoBoard(std::string_view body) {
    EchoBoardAnswer answer;
    bool first = true;
    while (!body.empty()) {
        const auto end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        if (line.empty() && !first) {
            continue;
        }
        if (first) {
            const auto [latest_end, error] =
                std::from_chars(line.data(), line.data() + line.size(), answer.latest);
            if (error != std::errc{} || latest_end != line.data() + line.size()) {
                return std::nullopt;
            }
            first = false;
            continue;
        }
        const auto space = line.find(' ');
        const std::string_view amount_text = line.substr(0, space);
        s32 amount = 0;
        const auto [amount_end, error] =
            std::from_chars(amount_text.data(), amount_text.data() + amount_text.size(), amount);
        if (error != std::errc{} || amount_end != amount_text.data() + amount_text.size() ||
            amount <= 0) {
            return std::nullopt;
        }
        u32 map = 0;
        if (space != std::string_view::npos) {
            const std::string_view map_text = line.substr(space + 1);
            const auto [map_end, map_error] =
                std::from_chars(map_text.data(), map_text.data() + map_text.size(), map);
            if (map_error != std::errc{} || map_end != map_text.data() + map_text.size()) {
                return std::nullopt;
            }
        }
        answer.echoes.push_back(KillEchoes{amount, map});
    }
    if (first) {
        return std::nullopt;
    }
    return answer;
}

std::vector<s32> EffectsToRestoreAfterTrip(std::span<const s32> before,
                                           std::span<const s32> after) {
    std::vector<s32> missing;
    for (const s32 effect : before) {
        // The bells' effects; 9001 (in a session as host) the game puts back itself.
        constexpr std::array<s32, 4> BellEffects{9000, 9003, 9005, 9006};
        if (std::ranges::find(BellEffects, effect) != BellEffects.end() &&
            std::ranges::find(after, effect) == after.end()) {
            missing.push_back(effect);
        }
    }
    return missing;
}

bool ShouldRestoreSearchAfterTrip(bool in_party_session, bool search_effect_active) {
    return in_party_session && !search_effect_active;
}

s32 LocalTaskKind(bool hosting) {
    return hosting ? 3 : 2;
}

bool ReopensClosedSession(bool party, s32 matching_state, u8 closing) {
    return party && matching_state == MatchingStateHosting && closing != 0;
}

s32 RemoteTaskKind(bool hosting) {
    return hosting ? RemoteGuestTask : RemoteHostTask;
}

bool RecreateIsForAJoin(bool pending, bool for_join) {
    return !pending || for_join;
}

u8 RebuiltTaskFlags(u8 constructed, const SavedJoinTask* saved, bool waiting) {
    if (!saved) {
        return constructed & JoinTaskFlagBits;
    }
    return (waiting ? saved->flags : static_cast<u8>(saved->flags | 1)) & JoinTaskFlagBits;
}

s32 RebuiltTaskKind(bool hosting, std::span<const SavedJoinTask> saved,
                    std::span<const SavedJoinTask> live, u64 identity) {
    if (hosting) {
        return RemoteGuestTask;
    }
    for (const SavedJoinTask& task : live) {
        if ((task.kind == RemoteHostTask || task.kind == RemoteGuestTask) &&
            task.identity == identity) {
            return task.kind;
        }
    }
    const auto is_host = [](const SavedJoinTask& task) { return task.kind == RemoteHostTask; };
    if (std::ranges::any_of(live, is_host)) {
        return RemoteGuestTask; // a session has one host
    }
    bool host_elsewhere = false;
    for (const SavedJoinTask& task : saved) {
        if (task.kind != RemoteHostTask && task.kind != RemoteGuestTask) {
            continue; // this player's own task
        }
        if (task.identity == identity) {
            return task.kind;
        }
        host_elsewhere |= task.kind == RemoteHostTask;
    }
    return host_elsewhere ? RemoteGuestTask : RemoteHostTask;
}

bool TalksAsHost(bool in_party_session, s32 character_type) {
    return in_party_session && character_type == WhiteGhostCharacterType;
}

bool SettingIsOn(const char* setting) {
    return setting != nullptr && std::string_view{setting} == "1";
}

bool ShouldPlayGuestAsHost(bool switched_on, bool in_party_session, s32 character_type) {
    return switched_on && in_party_session && character_type == WhiteGhostCharacterType;
}

bool ShowsGuestBloodstain(bool party_guest, bool in_session, bool loading, bool has_stain,
                          s32 echoes, u32 record_block, u32 map, bool shown, int tries) {
    const bool same_map = map != 0 && (record_block >> 16) == (map >> 16);
    return party_guest && in_session && !loading && has_stain && echoes > 0 && same_map && !shown &&
           tries < GuestBloodstainTries;
}

bool RestoresBloodstainMark(bool party_guest, bool mark_before, bool mark_now, s32 echoes,
                            u32 block) {
    return party_guest && mark_before && !mark_now && echoes > 0 && block != 0xFFFFFFFF;
}

bool LevelSyncsGuest(bool in_party_session, bool above_host_band) {
    return above_host_band && !in_party_session;
}

bool NpcTurnsIntoWanderGhost(bool in_party_session, bool param_says_wander_ghost) {
    return param_says_wander_ghost && !in_party_session;
}

bool IsGameVtable(u64 vtable, u64 image_base, std::span<const u64> vtables) {
    return image_base != 0 && vtable >= image_base &&
           std::ranges::find(vtables, vtable - image_base) != vtables.end();
}

bool RefusedCallLog::FirstTime(std::string_view site, u64 vtable) {
    if (logged.size() >= MaxLogged) {
        return false;
    }
    return logged.emplace(std::string{site}, vtable).second;
}

std::string DescribeVtable(u64 vtable, u64 image_base, u64 image_size) {
    if (image_base != 0 && vtable >= image_base && vtable - image_base < image_size) {
        return fmt::format("eboot+{:#x}", vtable - image_base);
    }
    return fmt::format("{:#x}", vtable);
}

const std::array<u8, GuestItemPickupAllowedOffset + 8>& SoloMapItemStandIn() {
    static const auto stand_in = [] {
        std::array<u8, GuestItemPickupAllowedOffset + 8> bytes{};
        constexpr s32 no_session = -1;
        std::memcpy(bytes.data() + JoinedSessionOffset, &no_session, sizeof(no_session));
        bytes[GuestItemPickupAllowedOffset] = 1;
        return bytes;
    }();
    return stand_in;
}

bool DefersItemLotAward(bool party_configured, bool summoned_guest, u64 return_offset) {
    return party_configured && summoned_guest && return_offset == ClientItemLotAwardReturn;
}

bool MenuSeesAClient(bool party_configured, bool summoned_client) {
    return summoned_client && !party_configured;
}

bool GuestHandlesMapItemsAlone(bool party_configured) {
    return party_configured;
}

std::array<u8, 8> RetargetRipLoad(std::span<const u8, 8> original, u64 address, u64 target) {
    std::array<u8, 8> patched{};
    std::copy(original.begin(), original.end(), patched.begin());
    const s32 displacement =
        static_cast<s32>(static_cast<s64>(target) - static_cast<s64>(address + patched.size()));
    std::memcpy(patched.data() + 4, &displacement, sizeof(displacement));
    return patched;
}

u32 MapItemPacketHeaderSeen(u32 header, bool party_configured) {
    return party_configured ? 0 : header;
}

u32 PackedMapOfWarpPoint(s32 warp_point) {
    const u32 area = static_cast<u32>(warp_point / 100000);
    const u32 block = static_cast<u32>(warp_point / 10000 % 10);
    return area << 24 | (block & 0xFF) << 16;
}

bool IsHeadstoneTravelFlag(s32 flag) {
    return RuleFor(flag).Has(FlagHeadstoneTrip);
}

s32 HeadstoneWarpPoint(s32 flag) {
    return flag % 10000 / 10 * 10000 + 2950 + flag % 10;
}

bool GuestMakesItsOwnLampTrip(bool in_party_session, PartyRole role) {
    return in_party_session && role == PartyRole::Guest;
}

std::vector<MultiplayerWall> MultiplayerWallsOn(u32 packed_map) {
    // InitializeEvent(_, 7600, object, sfx) in m22-m35's event files (01.09).
    static constexpr std::array<s32, 18> Walls{
        2201999, 2301999, 2401999, 2401998, 2401997, 2401996, 2401995, 2411999, 2411998,
        2411997, 2421999, 2421998, 2701999, 2701998, 2801999, 2801998, 3201999, 3501990,
    };
    std::vector<MultiplayerWall> walls;
    for (const s32 object : Walls) {
        if (PackedMapOfWarpPoint(object) == packed_map) {
            walls.push_back(MultiplayerWall{object, object + 2000});
        }
    }
    return walls;
}

bool IsParamOfType(std::span<const u8> param, std::string_view type) {
    if (param.size() < 0x0C + type.size() + 1) {
        return false;
    }
    return std::equal(type.begin(), type.end(), param.begin() + 0x0C) &&
           param[0x0C + type.size()] == 0;
}

std::optional<u64> FindParamRow(std::span<const u8> param, s32 id) {
    if (param.size() < 0x40) {
        return std::nullopt;
    }
    u16 rows = 0;
    std::memcpy(&rows, param.data() + 0x0A, sizeof(rows));
    if (param.size() < 0x40 + std::size_t{rows} * 24) {
        return std::nullopt;
    }
    std::size_t low = 0;
    std::size_t high = rows;
    while (low < high) {
        const std::size_t middle = (low + high) / 2;
        s32 row_id = 0;
        std::memcpy(&row_id, param.data() + 0x40 + middle * 24, sizeof(row_id));
        if (row_id == id) {
            u64 offset = 0;
            std::memcpy(&offset, param.data() + 0x40 + middle * 24 + 8, sizeof(offset));
            return offset;
        }
        if (row_id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return std::nullopt;
}

std::optional<HarderEnemies> ParseHarderEnemies(std::string_view setting) {
    std::array<float, 3> rates{};
    std::size_t count = 0;
    while (true) {
        const std::size_t comma = setting.find(',');
        std::string_view part = setting.substr(0, comma);
        while (!part.empty() && IsWhitespace(part.front())) {
            part.remove_prefix(1);
        }
        while (!part.empty() && IsWhitespace(part.back())) {
            part.remove_suffix(1);
        }
        float rate = 0.0f;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), rate);
        if (count == rates.size() || part.empty() || error != std::errc{} ||
            end != part.data() + part.size() || !(rate >= 1.0f && rate <= 10.0f)) {
            return std::nullopt;
        }
        rates[count++] = rate;
        if (comma == std::string_view::npos) {
            break;
        }
        setting.remove_prefix(comma + 1);
    }
    if (count != rates.size() || (rates[0] == 1.0f && rates[1] == 1.0f && rates[2] == 1.0f)) {
        return std::nullopt;
    }
    return HarderEnemies{rates[0], rates[1], rates[2]};
}

u32 HarderEnemyHp(u32 hp, float rate) {
    if (hp <= 1) {
        return hp;
    }
    const double scaled = std::round(static_cast<double>(hp) * rate);
    return static_cast<u32>(std::min(scaled, 9'999'999.0));
}

s16 HarderEnemyPoise(s16 poise, u32 hp, float rate) {
    if (rate <= 1.0f || hp <= 1 || poise >= 9999 || poise < 0) {
        return poise;
    }
    const double floor = (hp >= 1000 ? 50.0 : 15.0) * rate;
    const double scaled = std::max(std::round(static_cast<double>(poise) * rate), floor);
    return static_cast<s16>(std::min(scaled, 9998.0));
}

u16 HarderEnemyAttack(u16 attack, float rate) {
    const double scaled = std::round(static_cast<double>(attack) * rate);
    return static_cast<u16>(std::min(scaled, 65535.0));
}

namespace {

// Calls `scale` with each distinct row's data that holds `row_end` bytes; returns how many.
template <typename Scale>
std::size_t ForEachParamRow(std::span<u8> param, std::string_view type, u64 row_end,
                            Scale&& scale) {
    if (!IsParamOfType(param, type)) {
        return 0;
    }
    u16 rows = 0;
    std::memcpy(&rows, param.data() + 0x0A, sizeof(rows));
    if (param.size() < 0x40 + std::size_t{rows} * 24) {
        return 0;
    }
    std::vector<u64> seen;
    std::size_t changed = 0;
    for (std::size_t row = 0; row < rows; ++row) {
        u64 data = 0;
        std::memcpy(&data, param.data() + 0x40 + row * 24 + 8, sizeof(data));
        if (data < 0x40 + std::size_t{rows} * 24 || data > param.size() ||
            param.size() - data < row_end ||
            std::find(seen.begin(), seen.end(), data) != seen.end()) {
            continue;
        }
        seen.push_back(data);
        if (scale(param.data() + data)) {
            ++changed;
        }
    }
    return changed;
}

} // namespace

std::size_t MakeNpcParamHarder(std::span<u8> param, const HarderEnemies& harder) {
    return ForEachParamRow(param, "NPC_PARAM_ST", NpcPoiseOffset + 2, [&](u8* row) {
        u32 hp = 0;
        s16 poise = 0;
        std::memcpy(&hp, row + NpcHpOffset, sizeof(hp));
        std::memcpy(&poise, row + NpcPoiseOffset, sizeof(poise));
        const u32 new_hp = HarderEnemyHp(hp, harder.hp);
        const s16 new_poise = HarderEnemyPoise(poise, hp, harder.poise);
        std::memcpy(row + NpcHpOffset, &new_hp, sizeof(new_hp));
        std::memcpy(row + NpcPoiseOffset, &new_poise, sizeof(new_poise));
        return new_hp != hp || new_poise != poise;
    });
}

std::size_t RemoveCoopBossHpBonus(std::span<u8> sp_effect_param) {
    if (!IsParamOfType(sp_effect_param, "SP_EFFECT_PARAM_ST")) {
        return 0;
    }
    std::size_t changed = 0;
    for (const s32 effect : CoopBossHpEffects) {
        const auto row = FindParamRow(sp_effect_param, effect);
        if (!row || *row > sp_effect_param.size() ||
            sp_effect_param.size() - *row < SpEffectMaxHpRateOffset + sizeof(float)) {
            continue;
        }
        float rate = 0.0f;
        std::memcpy(&rate, sp_effect_param.data() + *row + SpEffectMaxHpRateOffset, sizeof(rate));
        if (rate != 1.0f) {
            rate = 1.0f;
            std::memcpy(sp_effect_param.data() + *row + SpEffectMaxHpRateOffset, &rate,
                        sizeof(rate));
            ++changed;
        }
    }
    return changed;
}

std::size_t MakeNpcAttacksHarder(std::span<u8> param, float damage) {
    if (!FindParamRow(param, NpcAttackMarkerRow)) {
        return 0;
    }
    return ForEachParamRow(param, "ATK_PARAM_ST", AtkPhysOffset + 8, [&](u8* row) {
        bool changed = false;
        for (u64 field = AtkPhysOffset; field < AtkPhysOffset + 8; field += 2) {
            u16 attack = 0;
            std::memcpy(&attack, row + field, sizeof(attack));
            const u16 scaled = HarderEnemyAttack(attack, damage);
            std::memcpy(row + field, &scaled, sizeof(scaled));
            changed |= scaled != attack;
        }
        return changed;
    });
}

std::span<const PolishedMessage> PolishedMessages() {
    static constexpr std::array<PolishedMessage, 26> messages{{
        // Name plates.
        {EventTextCategory, 107000, u"Beckoner", u"Host"},
        {EventTextCategory, 107100, u"Cooperator", u"Guest"},
        // Joins.
        {EventTextCategory, 102001, u"The bell's resonance has beckoned <?joinName?>.",
         u"<?joinName?> joined."},
        {EventTextCategory, 102100, u"Entered the world of <?hostName?>,\nwho beckoned you here.",
         u"Joined <?hostName?>."},
        {EventTextCategory, 101001, u"Discovered the chime of a Small Resonant Bell.",
         u"Found a guest."},
        {EventTextCategory, 101100, u"Discovered the chime of a Beckoning Bell.", u"Found a host."},
        // Searches.
        {EventTextCategory, 100001, u"Searching for the chime of a Small Resonant Bell...",
         u"Waiting for guests..."},
        {EventTextCategory, 100100, u"Searching for the chime of a Beckoning Bell...",
         u"Looking for a host..."},
        {EventTextCategory, 100300,
         u"Could not discover the chime of a Small Resonant Bell.\nNo insight was consumed.",
         u"No guests found.\nNo Insight was used."},
        {SpDialogCategory, 302200, u"Searching for the chime of a Beckoning Bell...",
         u"Looking for a host..."},
        // Deaths, bosses and doors.
        {EventTextCategory, 103001, u"Cooperator <?deadName?> has died.",
         u"Guest <?deadName?> has died."},
        {EventTextCategory, 103101, u"Your beckoner has died.\nReturning to own world.",
         u"Your host has died.\nReturning to own world."},
        {EventTextCategory, 105100, u"Beckoner has challenged\n<?npcNameId@500000?>.",
         u"Host has challenged\n<?npcNameId@500000?>."},
        {EventTextCategory, 105199, u"The beckoner has begun the final battle.",
         u"The host has begun the final battle."},
        {EventTextCategory, 105299,
         u"The beckoner has begun the final battle.\nReturning to own world.",
         u"The host has begun the final battle.\nReturning to own world."},
        {EventTextCategory, 106200,
         u"Beckoner <?hostName?> has fallen.\nIt is time to bid farewell.",
         u"Host <?hostName?> has fallen.\nIt is time to bid farewell."},
        {EventTextCategory, 10010740, u"Only the beckoner can open this door.",
         u"Only the host can open this door."},
        {EventTextCategory, 10010741, u"Only the beckoner can operate this device.",
         u"Only the host can operate this device."},
        // Leaving.
        {EventTextCategory, 20000400, u"Sent cooperator <?leaveName?> back\nto their own world.",
         u"Sent guest <?leaveName?> home."},
        {EventTextCategory, 20000401, u"Sent back to own world by\nbeckoner <?hostName?>.",
         u"Sent home by host <?hostName?>."},
        {EventTextCategory, 20000402, u"Cooperator <?leaveName?>\nreturned to their own world.",
         u"Guest <?leaveName?> left."},
        {EventTextCategory, 20000404, u"Cooperator <?leaveName?>\nreturned to their own world.",
         u"Guest <?leaveName?> left."},
        {EventTextCategory, 20000405, u"Cooperator <?leaveName?>\nreturned to their own world.",
         u"Guest <?leaveName?> left."},
        {InGameMenuCategory, 141000, u"Send Phantom home", u"Send Guest home"},
        {DialogCategory, 18100, u"Send this phantom home?", u"Send this guest home?"},
        {DialogCategory, 18101, u"No phantom to send home", u"No guest to send home"},
    }};
    return messages;
}

std::optional<std::u16string_view> PolishedText(u32 category, s32 id, std::u16string_view current) {
    for (const PolishedMessage& message : PolishedMessages()) {
        if (message.category == category && message.id == id && message.original == current) {
            return message.replacement;
        }
    }
    return std::nullopt;
}

std::size_t RemoveBellIcons(std::span<u8> sp_effect_param) {
    if (!IsParamOfType(sp_effect_param, "SP_EFFECT_PARAM_ST")) {
        return 0;
    }
    std::size_t changed = 0;
    for (const s32 effect : CoopBellIconEffects) {
        const auto row = FindParamRow(sp_effect_param, effect);
        if (!row || *row > sp_effect_param.size() ||
            sp_effect_param.size() - *row < SpEffectIconOffset + sizeof(s32)) {
            continue;
        }
        s32 icon = 0;
        std::memcpy(&icon, sp_effect_param.data() + *row + SpEffectIconOffset, sizeof(icon));
        if (icon != -1) {
            icon = -1;
            std::memcpy(sp_effect_param.data() + *row + SpEffectIconOffset, &icon, sizeof(icon));
            ++changed;
        }
    }
    return changed;
}

const std::vector<CodePatch>& QuietSummonBannerPatches() {
    static const std::vector<CodePatch> patches = CoopPatchesOf("quiet summon banners");
    return patches;
}

std::optional<SaveBackups> ParseSaveBackups(std::string_view setting) {
    const auto trim = [](std::string_view text) {
        while (!text.empty() && IsWhitespace(text.front())) {
            text.remove_prefix(1);
        }
        while (!text.empty() && IsWhitespace(text.back())) {
            text.remove_suffix(1);
        }
        return text;
    };
    setting = trim(setting);
    if (setting == "off" || setting == "0") {
        return std::nullopt;
    }
    const std::size_t comma = setting.find(',');
    if (comma == std::string_view::npos) {
        return SaveBackups{};
    }
    const std::string_view minutes_text = trim(setting.substr(0, comma));
    const std::string_view kept_text = trim(setting.substr(comma + 1));
    int minutes = 0;
    std::size_t kept = 0;
    const auto [minutes_end, minutes_error] =
        std::from_chars(minutes_text.data(), minutes_text.data() + minutes_text.size(), minutes);
    const auto [kept_end, kept_error] =
        std::from_chars(kept_text.data(), kept_text.data() + kept_text.size(), kept);
    if (minutes_error != std::errc{} || kept_error != std::errc{} ||
        minutes_end != minutes_text.data() + minutes_text.size() ||
        kept_end != kept_text.data() + kept_text.size() || minutes < 1 || kept < 1) {
        return SaveBackups{};
    }
    return SaveBackups{std::chrono::minutes{minutes}, kept};
}

std::string SaveBackupName(std::chrono::sys_seconds when) {
    const auto day = std::chrono::floor<std::chrono::days>(when);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{when - day};
    return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       time.hours().count(), time.minutes().count(), time.seconds().count());
}

namespace {

namespace fs = std::filesystem;

bool IsSaveBackupName(const std::string& name) {
    return name.size() == 15 && name[8] == '-' &&
           std::ranges::all_of(name.substr(0, 8), [](char c) { return c >= '0' && c <= '9'; }) &&
           std::ranges::all_of(name.substr(9), [](char c) { return c >= '0' && c <= '9'; });
}

// The backups in `user_dir`, oldest first.
std::vector<fs::path> SaveBackupsIn(const fs::path& user_dir) {
    std::vector<fs::path> backups;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(user_dir / SaveBackupsFolder, error)) {
        if (entry.is_directory(error) && IsSaveBackupName(entry.path().filename().string())) {
            backups.push_back(entry.path());
        }
    }
    std::ranges::sort(backups);
    return backups;
}

// Every save file under `root` (savedata, home/<id>/savedata, the party files), by its path
// relative to `root`, with its bytes.
std::map<std::string, std::string> SaveFilesUnder(const fs::path& root) {
    std::map<std::string, std::string> files;
    std::error_code error;
    const auto add = [&](const fs::path& path) {
        std::ifstream file{path, std::ios::binary};
        if (file) {
            files[fs::relative(path, root, error).generic_string()] =
                std::string{std::istreambuf_iterator<char>{file}, {}};
        }
    };
    const auto add_tree = [&](const fs::path& folder) {
        for (auto it = fs::recursive_directory_iterator(folder, error);
             !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error)) {
                add(it->path());
            }
        }
        error.clear();
    };
    if (fs::is_directory(root / "savedata", error)) {
        add_tree(root / "savedata");
    }
    for (const auto& entry : fs::directory_iterator(root / "home", error)) {
        if (fs::is_directory(entry.path() / "savedata", error)) {
            add_tree(entry.path() / "savedata");
        }
    }
    error.clear();
    for (const auto& entry : fs::directory_iterator(root, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.starts_with("bloodborne_party_") &&
            name.ends_with(".txt")) {
            add(entry.path());
        }
    }
    return files;
}

} // namespace

std::optional<fs::path> BackUpSaves(const fs::path& user_dir, std::string_view name) {
    const auto files = SaveFilesUnder(user_dir);
    if (files.empty()) {
        return std::nullopt;
    }
    const auto backups = SaveBackupsIn(user_dir);
    if (!backups.empty() && SaveFilesUnder(backups.back()) == files) {
        return std::nullopt;
    }
    const fs::path backup = user_dir / SaveBackupsFolder / std::string{name};
    std::error_code error;
    for (const auto& [relative, bytes] : files) {
        const fs::path path = backup / fs::path{relative};
        fs::create_directories(path.parent_path(), error);
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            fs::remove_all(backup, error);
            return std::nullopt;
        }
    }
    // The game wrote while this read: the copy may be half old, half new.
    if (SaveFilesUnder(user_dir) != files) {
        fs::remove_all(backup, error);
        return std::nullopt;
    }
    return backup;
}

std::size_t PruneSaveBackups(const fs::path& user_dir, std::size_t kept) {
    const auto backups = SaveBackupsIn(user_dir);
    std::size_t removed = 0;
    std::error_code error;
    for (std::size_t index = 0; index + kept < backups.size(); ++index) {
        if (fs::remove_all(backups[index], error) > 0) {
            ++removed;
        }
    }
    return removed;
}

bool CoopTraceEnabled(const char* setting) {
    if (setting == nullptr) {
        return true;
    }
    const std::string_view value{setting};
    return value != "0" && value != "off";
}

std::optional<std::string> TraceRecord::Field(std::string_view key) const {
    for (const auto& [name, value] : fields) {
        if (name == key) {
            return value;
        }
    }
    return std::nullopt;
}

namespace {

int TraceHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

template <typename T>
std::optional<T> ParseTraceNumber(std::string_view text, int base) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> SplitTrace(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const size_t end = text.find(separator);
        parts.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(end + 1);
    }
}

std::string_view TraceKeyOf(std::string_view token) {
    return token.substr(0, token.find(':'));
}

} // namespace

std::string TraceEscape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<u8>(c);
        if (c == '\\') {
            escaped += "\\\\";
        } else if (c == ' ') {
            escaped += "\\s";
        } else if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            escaped += "\\r";
        } else if (c == '\t') {
            escaped += "\\t";
        } else if (byte < 0x20 || byte >= 0x7F) {
            escaped += fmt::format("\\x{:02x}", byte);
        } else {
            escaped += c;
        }
    }
    return escaped;
}

std::optional<std::string> TraceUnescape(std::string_view text) {
    std::string plain;
    plain.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\\') {
            plain += text[index];
            continue;
        }
        if (++index >= text.size()) {
            return std::nullopt;
        }
        switch (text[index]) {
        case '\\':
            plain += '\\';
            break;
        case 's':
            plain += ' ';
            break;
        case 'n':
            plain += '\n';
            break;
        case 'r':
            plain += '\r';
            break;
        case 't':
            plain += '\t';
            break;
        case 'x': {
            if (index + 2 >= text.size()) {
                return std::nullopt;
            }
            const int high = TraceHexDigit(text[index + 1]);
            const int low = TraceHexDigit(text[index + 2]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            plain += static_cast<char>(high * 16 + low);
            index += 2;
            break;
        }
        default:
            return std::nullopt;
        }
    }
    return plain;
}

std::string EncodeTraceRecord(const TraceRecord& record) {
    std::string line = fmt::format("{} {}", record.utc_ms, record.kind);
    for (const auto& [key, value] : record.fields) {
        line += ' ';
        line += key;
        line += '=';
        line += TraceEscape(value);
    }
    return line;
}

std::optional<TraceRecord> ParseTraceRecord(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    std::vector<std::string_view> tokens;
    for (const std::string_view token : SplitTrace(line, ' ')) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    if (tokens.size() < 2) {
        return std::nullopt;
    }
    const auto utc_ms = ParseTraceNumber<s64>(tokens[0], 10);
    if (!utc_ms) {
        return std::nullopt;
    }
    TraceRecord record{*utc_ms, std::string{tokens[1]}, {}};
    for (size_t index = 2; index < tokens.size(); ++index) {
        const size_t equals = tokens[index].find('=');
        if (equals == std::string_view::npos || equals == 0) {
            return std::nullopt;
        }
        auto value = TraceUnescape(tokens[index].substr(equals + 1));
        if (!value) {
            return std::nullopt;
        }
        record.fields.emplace_back(std::string{tokens[index].substr(0, equals)}, std::move(*value));
    }
    return record;
}

void AddTraceBody(TraceRecord& record, std::string_view body, std::size_t cap) {
    record.fields.emplace_back("size", std::to_string(body.size()));
    record.fields.emplace_back("body", std::string{body.substr(0, cap)});
}

std::string TraceHex(std::span<const u8> bytes) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        hex += Digits[byte >> 4];
        hex += Digits[byte & 0xF];
    }
    return hex;
}

std::optional<std::vector<u8>> ParseTraceHex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t index = 0; index < hex.size(); index += 2) {
        const int high = TraceHexDigit(hex[index]);
        const int low = TraceHexDigit(hex[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<u8>(high * 16 + low));
    }
    return bytes;
}

std::string TraceHandles(std::span<const u32> handles) {
    std::string text;
    for (const u32 handle : handles) {
        text += fmt::format("{}{:x}", text.empty() ? "" : ",", handle);
    }
    return text;
}

std::optional<std::vector<u32>> ParseTraceHandles(std::string_view text) {
    std::vector<u32> handles;
    if (text.empty()) {
        return handles;
    }
    for (const std::string_view part : SplitTrace(text, ',')) {
        const auto handle = ParseTraceNumber<u32>(part, 16);
        if (!handle) {
            return std::nullopt;
        }
        handles.push_back(*handle);
    }
    return handles;
}

std::string TraceToken(const SeenCharacter& character) {
    const int flags = (character.untouched_boss ? 1 : 0) | (character.others_kill ? 2 : 0);
    return fmt::format("{:x}:{}:{:x}:{}{}", character.handle, character.hp > 0 ? 1 : 0,
                       character.instance, character.settling ? 1 : 0,
                       flags != 0 ? fmt::format(":{}", flags) : std::string{});
}

std::optional<SeenCharacter> ParseSeenToken(std::string_view token) {
    const auto parts = SplitTrace(token, ':');
    if (parts.size() != 4 && parts.size() != 5) {
        return std::nullopt;
    }
    const auto handle = ParseTraceNumber<u32>(parts[0], 16);
    const auto alive = ParseTraceNumber<int>(parts[1], 10);
    const auto instance = ParseTraceNumber<u64>(parts[2], 16);
    const auto settling = ParseTraceNumber<int>(parts[3], 10);
    const auto flags = parts.size() == 5 ? ParseTraceNumber<int>(parts[4], 10) : std::optional{0};
    if (!handle || !alive || !instance || !settling || !flags || *alive > 1 || *settling > 1 ||
        *flags < 0 || *flags > 3 || (parts.size() == 5 && *flags == 0)) {
        return std::nullopt;
    }
    return SeenCharacter{*handle,           *alive,           *instance, *settling == 1,
                         (*flags & 1) != 0, (*flags & 2) != 0};
}

std::string TraceToken(const EnemyCopy& copy) {
    return fmt::format("{:x}:{}:{}:{}", copy.handle, copy.hp, copy.max,
                       copy.own_hits ? std::to_string(*copy.own_hits) : std::string{"-"});
}

std::optional<EnemyCopy> ParseCopyToken(std::string_view token) {
    const auto parts = SplitTrace(token, ':');
    if (parts.size() != 4) {
        return std::nullopt;
    }
    const auto handle = ParseTraceNumber<u32>(parts[0], 16);
    const auto hp = ParseTraceNumber<s32>(parts[1], 10);
    const auto max = ParseTraceNumber<s32>(parts[2], 10);
    const auto own = parts[3] == "-" ? std::nullopt : ParseTraceNumber<s64>(parts[3], 10);
    if (!handle || !hp || !max || (parts[3] != "-" && !own)) {
        return std::nullopt;
    }
    return EnemyCopy{*handle, *hp, *max, own};
}

std::string TraceToken(const EnemyHpReport& report) {
    std::string token = fmt::format("{:x}:", report.visit);
    for (size_t index = 0; index < report.enemies.size(); ++index) {
        const EnemyHp& enemy = report.enemies[index];
        token +=
            fmt::format("{}{:x}/{}/{}", index == 0 ? "" : "+", enemy.handle, enemy.hp, enemy.max);
    }
    return token;
}

std::optional<EnemyHpReport> ParseReportToken(std::string_view token) {
    const size_t colon = token.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto source = ParseTraceNumber<u64>(token.substr(0, colon), 16);
    if (!source) {
        return std::nullopt;
    }
    EnemyHpReport report{*source, {}};
    const std::string_view enemies = token.substr(colon + 1);
    if (enemies.empty()) {
        return report;
    }
    for (const std::string_view enemy : SplitTrace(enemies, '+')) {
        const auto parts = SplitTrace(enemy, '/');
        const auto handle = parts.size() == 3 ? ParseTraceNumber<u32>(parts[0], 16) : std::nullopt;
        const auto hp = parts.size() == 3 ? ParseTraceNumber<s32>(parts[1], 10) : std::nullopt;
        const auto max = parts.size() == 3 ? ParseTraceNumber<s32>(parts[2], 10) : std::nullopt;
        if (!handle || !hp || !max) {
            return std::nullopt;
        }
        report.enemies.push_back({*handle, *hp, *max});
    }
    return report;
}

std::string TraceListDelta::Next(std::span<const std::string> tokens) {
    std::string delta;
    const auto add = [&delta](std::string_view part) {
        if (!delta.empty() && delta != "=") {
            delta += ',';
        }
        delta += part;
    };
    if (whole) {
        whole = false;
        delta = "=";
        for (const std::string& token : tokens) {
            add(token);
        }
    } else {
        std::map<std::string, std::string, std::less<>> before;
        for (const std::string& token : last) {
            before.emplace(TraceKeyOf(token), token);
        }
        std::set<std::string, std::less<>> keys;
        for (const std::string& token : tokens) {
            keys.emplace(TraceKeyOf(token));
            const auto it = before.find(TraceKeyOf(token));
            if (it == before.end() || it->second != token) {
                add(token);
            }
        }
        for (const std::string& token : last) {
            if (!keys.contains(TraceKeyOf(token))) {
                add(fmt::format("-{}", TraceKeyOf(token)));
            }
        }
    }
    last.assign(tokens.begin(), tokens.end());
    return delta;
}

bool ApplyTraceListDelta(std::vector<std::string>& list, std::string_view delta) {
    if (delta.starts_with('=')) {
        list.clear();
        delta.remove_prefix(1);
    }
    if (delta.empty()) {
        return true;
    }
    for (const std::string_view part : SplitTrace(delta, ',')) {
        if (part.starts_with('-')) {
            std::erase_if(list, [key = part.substr(1)](const std::string& token) {
                return TraceKeyOf(token) == key;
            });
            continue;
        }
        if (part.find(':') == std::string_view::npos) {
            return false;
        }
        const auto it = std::ranges::find_if(
            list, [key = TraceKeyOf(part)](const std::string& t) { return TraceKeyOf(t) == key; });
        if (it != list.end()) {
            *it = std::string{part};
        } else {
            list.emplace_back(part);
        }
    }
    return true;
}

TraceSampler::Fields TraceSampler::Changes(
    u32 file_now, const Fields& values,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& lists) {
    if (file != file_now) {
        file = file_now;
        last.clear();
        for (auto& [key, delta] : deltas) {
            delta.Reset();
        }
    }
    Fields changes;
    for (const auto& [key, value] : values) {
        auto [it, added] = last.try_emplace(key, value);
        if (added || it->second != value) {
            it->second = value;
            changes.emplace_back(key, value);
        }
    }
    for (const auto& [key, tokens] : lists) {
        if (std::string delta = deltas[key].Next(tokens); !delta.empty()) {
            changes.emplace_back(key, std::move(delta));
        }
    }
    return changes;
}

TraceSampler::Fields TracePlaceFields(const PlaceChange& change) {
    TraceSampler::Fields fields;
    for (const auto& [side, place] : {std::pair{"from", change.from}, std::pair{"to", change.to}}) {
        fields.emplace_back(fmt::format("{}-state", side),
                            std::string{PlaceStateName(place.state)});
        fields.emplace_back(fmt::format("{}-map", side), fmt::format("{:x}", place.map));
        fields.emplace_back(fmt::format("{}-visit", side), std::to_string(place.visit));
        fields.emplace_back(fmt::format("{}-since", side), std::to_string(place.since_ms));
    }
    fields.emplace_back("cause", std::string{PlaceCauseName(change.cause)});
    return fields;
}

TraceSampler::Fields WorldLogTraceFields(std::string_view role, std::string_view op,
                                         std::span<const u8> payload) {
    return {{"role", std::string{role}}, {"op", std::string{op}}, {"payload", TraceHex(payload)}};
}

std::optional<PlaceChange> ParsePlaceFields(const TraceRecord& record) {
    const auto state_of = [](std::string_view name) -> std::optional<PlaceState> {
        for (const PlaceState state : {PlaceState::Unknown, PlaceState::Loading,
                                       PlaceState::Cutscene, PlaceState::Playing}) {
            if (PlaceStateName(state) == name) {
                return state;
            }
        }
        return std::nullopt;
    };
    const auto place_of = [&](std::string_view side) -> std::optional<Place> {
        const auto field = [&](const char* name) {
            return record.Field(fmt::format("{}-{}", side, name)).value_or("");
        };
        const auto state = state_of(field("state"));
        const auto map = ParseTraceNumber<u32>(field("map"), 16);
        const auto visit = ParseTraceNumber<u64>(field("visit"), 10);
        const auto since = ParseTraceNumber<s64>(field("since"), 10);
        if (!state || !map || !visit || !since) {
            return std::nullopt;
        }
        return Place{*state, *map, *visit, *since};
    };
    const auto from = place_of("from");
    const auto to = place_of("to");
    const std::string cause = record.Field("cause").value_or("");
    for (const PlaceCause known :
         {PlaceCause::Load, PlaceCause::Border, PlaceCause::Lamp, PlaceCause::Teleport,
          PlaceCause::DeathReload, PlaceCause::Cutscene, PlaceCause::CutsceneEnd}) {
        if (from && to && PlaceCauseName(known) == cause) {
            return PlaceChange{*from, *to, known};
        }
    }
    return std::nullopt;
}

bool TraceQueue::Push(std::string line) {
    {
        std::scoped_lock lock{mutex};
        if (lines.size() >= capacity) {
            ++dropped;
            return false;
        }
        lines.push_back(std::move(line));
    }
    ready.notify_one();
    return true;
}

std::vector<std::string> TraceQueue::Take(std::chrono::milliseconds wait) {
    std::unique_lock lock{mutex};
    ready.wait_for(lock, wait, [this] { return !lines.empty(); });
    std::vector<std::string> taken;
    taken.swap(lines);
    return taken;
}

u64 TraceQueue::TakeDropped() {
    std::scoped_lock lock{mutex};
    return std::exchange(dropped, 0);
}

std::string CoopTraceFileName(std::chrono::sys_seconds local_when, u32 process) {
    return fmt::format("{}{}-{}.txt", CoopTracePrefix, SaveBackupName(local_when), process);
}

std::size_t PruneCoopTraces(const fs::path& log_dir, std::size_t kept) {
    const std::string_view prefix{CoopTracePrefix};
    std::vector<fs::path> traces;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(log_dir, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.starts_with(prefix) && name.ends_with(".txt") &&
            name.size() > prefix.size() + 15 && IsSaveBackupName(name.substr(prefix.size(), 15))) {
            traces.push_back(entry.path());
        }
    }
    std::ranges::sort(traces);
    std::size_t removed = 0;
    for (std::size_t index = 0; index + kept < traces.size(); ++index) {
        if (fs::remove(traces[index], error)) {
            ++removed;
        }
    }
    return removed;
}

const std::vector<DeathFlagSite>& DeathFlagSites() {
    static const std::vector<DeathFlagSite> sites{
        {0x018BF941, {0x80, 0x8B, 0xE3, 0x01, 0x00, 0x00, 0x01}, DeathRegister::Rbx, "death"},
        {0x018CE6C5,
         {0x80, 0x89, 0xE3, 0x01, 0x00, 0x00, 0x01},
         DeathRegister::Rcx,
         "death in the character update"},
        {0x018BFAD3,
         {0x41, 0x80, 0x8F, 0xE3, 0x01, 0x00, 0x00, 0x01},
         DeathRegister::R15,
         "the game's kill"},
    };
    return sites;
}

bool TracesDeathOf(s32 max_hp) {
    return max_hp >= DeathTraceMinMaxHp;
}

bool IsPartySession(bool party_configured, s32 matching_state) {
    return party_configured &&
           (matching_state == MatchingStateHosting || matching_state == MatchingStateSummonedGuest);
}

void LampRestState::Rest(s32 rested_lamp, const LampPosition& at) {
    lamp = rested_lamp;
    rested_at = at;
    joined = false;
}

LampUse LampRestState::Use(s32 used_lamp, const LampPosition& at) {
    if (lamp == used_lamp) {
        lamp.reset();
        return LampUse::Travel;
    }
    Rest(used_lamp, at);
    return LampUse::Rest;
}

namespace {
float DistanceSquared(const LampPosition& a, const LampPosition& b) {
    float sum = 0.0f;
    for (std::size_t axis = 0; axis < a.size(); ++axis) {
        sum += (a[axis] - b[axis]) * (a[axis] - b[axis]);
    }
    return sum;
}

std::string FormatCoordinate(float value) {
    std::array<char, 32> text{};
    const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} ? std::string{text.data(), end} : std::string{"0"};
}

std::optional<float> ParseCoordinate(std::string_view text) {
    float value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}
} // namespace

bool LampRestState::JoinNearby(std::span<const LampRest> board,
                               const std::optional<LampPosition>& player) {
    if (lamp || !player) {
        return false;
    }
    for (const LampRest& rest : board) {
        if (rest.lamp != NotResting && rest.at &&
            DistanceSquared(*rest.at, *player) <= JoinDistance * JoinDistance) {
            Rest(rest.lamp, *rest.at);
            joined = true;
            return true;
        }
    }
    return false;
}

std::optional<LampPosition> LampRestState::JoinablePosition() const {
    return lamp && !joined ? std::optional{rested_at} : std::nullopt;
}

u64 RestoreScriptFlagsAfterRespawn(u64 flags_before, u64 flags_after, s32 hp_before) {
    if (hp_before <= 0) {
        return flags_after;
    }
    return (flags_after & ~ScriptCharacterFlags) | (flags_before & ScriptCharacterFlags);
}

bool RespawnsAtPartyRest(s32 hp, bool alive_this_visit) {
    return hp <= 0 && alive_this_visit;
}

bool LeftDeadByItsSave(const RespawnCandidate& character) {
    // Bits 0 and 2: killed (IsRealDeath); bit 1 comes with either; bit 5: put down by its world.
    constexpr u8 DeathMarks = 0x27;
    return character.hp <= 0 && character.max_hp > 0 && (character.death_flags & DeathMarks) == 0 &&
           ((!character.disabled && !character.hidden) || character.changed_by_party);
}

bool RespawnsInWorldRespawn(WorldRespawnKind kind, const RespawnCandidate& character,
                            bool alive_this_visit) {
    switch (kind) {
    case WorldRespawnKind::PartyRest:
        return RespawnsAtPartyRest(character.hp, alive_this_visit);
    case WorldRespawnKind::AfterWipe:
        return LeftDeadByItsSave(character);
    }
    return false;
}

bool KeepDisabledAfterRespawn(bool disabled_before, s32 hp_before) {
    return disabled_before && hp_before > 0;
}

std::optional<s32> LampRestState::Tick(const std::optional<LampPosition>& player,
                                       bool in_party_session) {
    if (!in_party_session) {
        lamp.reset();
    } else if (lamp && player) {
        if (DistanceSquared(*player, rested_at) > LeaveDistance * LeaveDistance) {
            lamp.reset();
        }
    }
    return lamp;
}

std::string EncodeLampRestUpdate(std::string_view member, s32 lamp,
                                 const std::optional<LampPosition>& at) {
    std::string update = std::string{member} + ' ' + std::to_string(lamp);
    if (lamp != NotResting && at) {
        for (const float coordinate : *at) {
            update += ' ' + FormatCoordinate(coordinate);
        }
    }
    return update;
}

std::optional<std::vector<LampRest>> ParseLampRestBoard(std::string_view body) {
    std::vector<LampRest> board;
    while (!body.empty()) {
        const std::size_t end = body.find('\n');
        std::string_view line = body.substr(0, end);
        body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1);
        std::vector<std::string_view> fields;
        while (!line.empty()) {
            const std::size_t space = line.find(' ');
            fields.push_back(line.substr(0, space));
            line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
        }
        if (fields.empty()) {
            continue;
        }
        if ((fields.size() != 2 && fields.size() != 5) || fields[0].empty()) {
            return std::nullopt;
        }
        LampRest rest{.member = std::string{fields[0]}};
        const auto [lamp_end, error] =
            std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), rest.lamp);
        if (error != std::errc{} || lamp_end != fields[1].data() + fields[1].size()) {
            return std::nullopt;
        }
        if (fields.size() == 5) {
            LampPosition at{};
            for (std::size_t axis = 0; axis < at.size(); ++axis) {
                const auto coordinate = ParseCoordinate(fields[2 + axis]);
                if (!coordinate) {
                    return std::nullopt;
                }
                at[axis] = *coordinate;
            }
            rest.at = at;
        }
        board.push_back(std::move(rest));
    }
    return board;
}

bool LampRestCoordinator::Tick(std::optional<s32> resting_lamp, s32 session_members,
                               std::span<const LampRest> board) {
    bool everyone_resting = false;
    if (resting_lamp && session_members > 1) {
        const auto resting_here = std::ranges::count_if(
            board, [&](const LampRest& rest) { return rest.lamp == *resting_lamp; });
        everyone_resting = resting_here >= session_members;
    }
    const bool refresh = everyone_resting && !everyone_was_resting;
    everyone_was_resting = everyone_resting;
    return refresh;
}

bool RefreshAtLamp(const LampRestCalls& calls) {
    if (!calls.request_full_recover || !calls.clamp_restock_item || !calls.restock_from_storage ||
        !calls.request_world_respawn) {
        return false;
    }
    calls.request_full_recover();
    calls.clamp_restock_item();
    calls.restock_from_storage();
    calls.request_world_respawn();
    return true;
}

bool SkipsLeaveLimit(s32 character_type, size_t other_members) {
    return character_type != WhiteGhostCharacterType || other_members > 0;
}

// A host with room whose bell stays on: on the port the effect outlasted the game's search after
// the first guest arrived (rig join3 2026-10-09), so a bell the game no longer searches with is
// rung again until the session is full. Stale only when the game posted no search for
// StaleSearchAfter (rig join3 at 30 FPS 2026-10-12: ringing again every RetryDelay ended searches
// that still ran, and the game posted none for 2m45s). The effect is ended first and the search
// started on a later tick, once it is off: the game starts a search when the effect goes on.
std::optional<PartyAction> PartyController::RingAgain(const PartyObservation& observation,
                                                      Clock::time_point now) {
    if (role != PartyRole::Host || !observation.room_for_more || observation.white_phantom) {
        ringing_since.reset();
        ending_since.reset();
        return std::nullopt;
    }
    if (ending_since) {
        if (!observation.search_effect_active) {
            ending_since.reset();
            ringing_since.reset();
            stable_ticks = 0;
            last_search = now;
            return PartyAction::StartSearch;
        }
        if (now - *ending_since < RetryDelay) {
            return PartyAction::Wait;
        }
        ending_since = now; // the effect did not go: end it again
        return PartyAction::EndSearch;
    }
    if (!observation.search_effect_active) {
        ringing_since.reset();
        return std::nullopt;
    }
    if (!ringing_since) {
        ringing_since = now;
    }
    const Clock::time_point heard = observation.last_search_post
                                        ? std::max(*ringing_since, *observation.last_search_post)
                                        : *ringing_since;
    if (now - heard < StaleSearchAfter) {
        return PartyAction::Wait;
    }
    ending_since = now;
    return PartyAction::EndSearch;
}

PartyAction PartyController::Tick(const PartyObservation& observation, Clock::time_point now) {
    if (!observation.player_valid || !observation.map_usable || observation.stage_transition ||
        observation.boss_sequence) {
        stable_ticks = 0;
        ending_since.reset();
        return PartyAction::Wait;
    }
    const bool may_search =
        observation.matching_idle || (role == PartyRole::Host && observation.room_for_more);
    if (const auto action = RingAgain(observation, now)) {
        return *action;
    }
    if (!may_search || observation.search_effect_active || observation.white_phantom) {
        // In (or joining) a session, or our search already took: the next time we are back home
        // alone is a fresh start, so forget the old cooldown.
        stable_ticks = 0;
        last_search.reset();
        return PartyAction::Wait;
    }
    if (++stable_ticks < StableTicksBeforeSearch) {
        return PartyAction::Wait;
    }
    if (last_search && now - *last_search < RetryDelay) {
        return PartyAction::Wait;
    }
    last_search = now;
    stable_ticks = 0;
    return PartyAction::StartSearch;
}

std::vector<AutoplayCommand> ParseAutoplayScript(std::string_view text) {
    std::vector<AutoplayCommand> script;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (const auto comment = line.find('#'); comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        std::vector<std::string> words;
        size_t at = 0;
        while (at < line.size()) {
            const auto start = line.find_first_not_of(" \t\r", at);
            if (start == std::string_view::npos) {
                break;
            }
            const auto stop = line.find_first_of(" \t\r", start);
            words.emplace_back(line.substr(start, stop - start));
            at = stop == std::string_view::npos ? line.size() : stop;
        }
        if (!words.empty()) {
            script.push_back({words.front(), {words.begin() + 1, words.end()}});
        }
    }
    return script;
}

u32 AutoplayButton(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, u32>, 15> Buttons{{
        {"cross", 0x4000},
        {"circle", 0x2000},
        {"square", 0x8000},
        {"triangle", 0x1000},
        {"options", 0x8},
        {"up", 0x10},
        {"right", 0x20},
        {"down", 0x40},
        {"left", 0x80},
        {"l1", 0x400},
        {"r1", 0x800},
        {"l2", 0x100},
        {"r2", 0x200},
        {"l3", 0x2},
        {"r3", 0x4},
    }};
    for (const auto& [button, bit] : Buttons) {
        if (button == name) {
            return bit;
        }
    }
    return 0;
}

bool TitlePressesCross(u64 ms) {
    constexpr u64 Period = 2000;
    constexpr u64 Held = 150;
    return ms >= Period && ms % Period < Held;
}

namespace {
// A file name a script gives (shot, sync): 1 to 40 letters, digits, '-' or '_'.
bool IsPlainAutoplayName(std::string_view name) {
    const auto plain = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    };
    return !name.empty() && name.size() <= 40 && std::ranges::all_of(name, plain);
}
} // namespace

std::optional<std::string> AutoplayShotPath(std::string_view request, std::string_view name) {
    if (request.empty() || !IsPlainAutoplayName(name)) {
        return std::nullopt;
    }
    const std::filesystem::path shots = std::filesystem::path{request}.parent_path() / "shots";
    return (shots / (std::string{name} + ".png")).string();
}

std::optional<std::string> AutoplaySyncPath(std::string_view script, std::string_view name) {
    if (script.empty() || !IsPlainAutoplayName(name)) {
        return std::nullopt;
    }
    return (std::filesystem::path{script}.parent_path() / "sync" / std::string{name}).string();
}

void PositionTrail::Note(u64 ms, const LampPosition& at) {
    if (last_ms && ms < *last_ms + IntervalMs) {
        return;
    }
    last_ms = ms;
    points.push_back(at);
    if (points.size() > Kept) {
        points.erase(points.begin());
    }
}

std::string FormatTrail(const std::vector<LampPosition>& points) {
    std::string text;
    for (const LampPosition& point : points) {
        text += fmt::format("{}{:.1f} {:.1f} {:.1f}", text.empty() ? "" : " ", point[0], point[1],
                            point[2]);
    }
    return text;
}

std::optional<LampPosition> GlidePoint(const std::vector<LampPosition>& path, double elapsed) {
    double left = elapsed * GlideMetersPerSecond;
    for (size_t at = 1; at < path.size(); ++at) {
        const LampPosition& from = path[at - 1];
        const LampPosition& to = path[at];
        const double length = std::hypot(to[0] - from[0], to[1] - from[1], to[2] - from[2]);
        if (left < length) {
            const float part = static_cast<float>(left / length);
            return LampPosition{from[0] + (to[0] - from[0]) * part,
                                from[1] + (to[1] - from[1]) * part,
                                from[2] + (to[2] - from[2]) * part};
        }
        left -= length;
    }
    return std::nullopt;
}

std::optional<size_t> NextAutoplayChapter(const std::vector<AutoplayCommand>& script,
                                          size_t failed) {
    for (size_t at = failed + 1; at < script.size(); ++at) {
        if (script[at].verb == "chapter") {
            return at;
        }
    }
    return std::nullopt;
}

std::optional<u32> ParseAutoplayTarget(std::string_view text) {
    if (text == "nearest") {
        return 0;
    }
    if (text == "self") {
        return AutoplaySelf;
    }
    if (text == "marked") {
        return AutoplayMarked;
    }
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
    }
    u32 handle = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), handle, 16);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return handle;
}

std::optional<bool> CompareAutoplay(s64 value, std::string_view op, s64 limit) {
    if (op == "<") {
        return value < limit;
    }
    if (op == "<=") {
        return value <= limit;
    }
    if (op == "==") {
        return value == limit;
    }
    if (op == "!=") {
        return value != limit;
    }
    if (op == ">=") {
        return value >= limit;
    }
    if (op == ">") {
        return value > limit;
    }
    return std::nullopt;
}

AttackStance AttackStanceFor(float target_x, float target_z, u32 cycle) {
    static constexpr std::array<std::pair<float, float>, 4> Sides{
        {{0.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {-1.0f, 0.0f}}};
    const auto [x, z] = Sides[cycle % Sides.size()];
    return {target_x + AttackDistance * x, target_z + AttackDistance * z};
}

float HeadingTowards(float x, float z, float to_x, float to_z) {
    return std::atan2(-(to_x - x), to_z - z);
}

u32 AttackCycleAt(double elapsed) {
    return elapsed <= 0.0 ? 0u : static_cast<u32>(elapsed / AttackCycleSeconds);
}

bool AttackHoldsR1(double elapsed) {
    const double into = std::max(elapsed, 0.0) - AttackCycleAt(elapsed) * AttackCycleSeconds;
    return into >= 0.3 && into < 0.5;
}

bool PartyDeathEnabled(const char* setting, bool by_default) {
    if (setting == nullptr || *setting == '\0') {
        return by_default;
    }
    return std::string_view{setting} != "0";
}

namespace {
// Read from the 01.09 eboot: each is the address after a `call IsNoDead` (see the eboot tests).
constexpr std::array<NoDeadCaller, 12> NoDeadCallerTable{{
    // Hit damage (0x01A2A770: HP minus the hit, asked only when that is 0 or less), and its check
    // once the hit is applied, asked after every hit (0x01A2C03B skips only the kill).
    {0x01A2BF7D, NoDeadCheck::Lethal},
    {0x01A2C05F, NoDeadCheck::AfterHit},
    // Special effect HP changes (0x01A2FA40: poison, rapid poison, frenzy and the like).
    {0x01A2FB4A, NoDeadCheck::Lethal},
    {0x01A2FBE6, NoDeadCheck::Lethal},
    {0x01A2FDD4, NoDeadCheck::Lethal},
    // Fall damage (0x01A35A00, from the landing at 0x01516138), and its check afterwards.
    {0x01A35E10, NoDeadCheck::Lethal},
    {0x01A35E7E, NoDeadCheck::Lethal},
    // The game's kills: an event's ForceCharacterDeath (KillCharacterOffset), the character's
    // own kill (0x018BF760, from its vtable) and another (0x018CDE10).
    {0x018BF9EE, NoDeadCheck::Instant},
    {0x018BF86C, NoDeadCheck::Instant},
    {0x018CE5EE, NoDeadCheck::Instant},
    // HP set outright (0x018BCD40, which the revive and the event bindings use, and 0x018BC470).
    {0x018BCD9C, NoDeadCheck::Instant},
    {0x018BC5C5, NoDeadCheck::Instant},
}};

template <typename T>
std::optional<T> ParseNumber(std::string_view text, int base = 10) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> SplitFields(std::string_view line) {
    std::vector<std::string_view> fields;
    while (!line.empty()) {
        const std::size_t space = line.find(' ');
        fields.push_back(line.substr(0, space));
        line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
    }
    return fields;
}
} // namespace

std::span<const NoDeadCaller> NoDeadCallers() {
    return NoDeadCallerTable;
}

NoDeadCheck ClassifyNoDeadCheck(u64 return_offset) {
    const auto caller =
        std::ranges::find(NoDeadCallerTable, return_offset,
                          [](const NoDeadCaller& entry) { return entry.return_offset; });
    return caller == NoDeadCallerTable.end() ? NoDeadCheck::Other : caller->check;
}

NoDeadCheck NoDeadCheckAt(NoDeadCheck check, s32 hp) {
    if (check != NoDeadCheck::AfterHit) {
        return check;
    }
    return hp <= 0 ? NoDeadCheck::Lethal : NoDeadCheck::Other;
}

bool KeepsPlayerAlive(NoDeadCheck check, bool armed, bool down, bool answer) {
    return (check == NoDeadCheck::Lethal && (armed || down)) || answer;
}

std::string_view VitalName(Vital vital) {
    switch (vital) {
    case Vital::Up:
        return "up";
    case Vital::Down:
        return "down";
    case Vital::Dead:
        return "dead";
    case Vital::Off:
        break;
    }
    return "off";
}

std::optional<Vital> ParseVital(std::string_view name) {
    for (const Vital vital : {Vital::Off, Vital::Up, Vital::Down, Vital::Dead}) {
        if (VitalName(vital) == name) {
            return vital;
        }
    }
    return std::nullopt;
}

std::optional<std::string> VitalsPostLog::Next(Vital vital, u32 map) {
    const std::pair now{vital, map};
    if (last == now) {
        return std::nullopt;
    }
    std::string line = fmt::format("posting {} (map {:#x})", VitalName(vital), map);
    if (last) {
        line += fmt::format(", was {} (map {:#x})", VitalName(last->first), last->second);
    }
    last = now;
    return line;
}

u32 ReviveDamageNeeded(u32 downs, s32 max) {
    const u32 bar = static_cast<u32>(std::max(max, 1));
    return downs <= 1 ? bar : downs == 2 ? bar * 2 : bar * 4;
}

u32 ReviveHitDamage(s32 attack_rating, bool heavy) {
    const u32 rating = static_cast<u32>(std::max(attack_rating, 1));
    return heavy ? std::max(rating * 3 / 2, 1u) : rating;
}

u32 RealReviveHitDamage(u32 dealt, s32 attack_rating, bool heavy) {
    return std::max(dealt, ReviveHitDamage(attack_rating, heavy));
}

float ReviveShare(u32 landed, u32 needed) {
    return needed == 0 ? 0.0f
                       : std::min(static_cast<float>(landed) / static_cast<float>(needed), 1.0f);
}

s32 RevivedHp(s32 max) {
    return std::max(max / 2, 1);
}

std::string GameHudString(std::span<const u8, GameHudStringSize> raw,
                          const std::function<std::vector<char16_t>(u64, size_t)>& read) {
    constexpr size_t MaxChars = 64;
    u64 capacity = 0;
    std::memcpy(&capacity, raw.data() + 0x20, sizeof(capacity));
    std::vector<char16_t> chars;
    if (capacity < 8) {
        chars.resize(8);
        std::memcpy(chars.data(), raw.data() + 0x08, 16);
    } else {
        u64 pointer = 0;
        std::memcpy(&pointer, raw.data() + 0x08, sizeof(pointer));
        chars = read(pointer, static_cast<size_t>(std::min<u64>(capacity, MaxChars)));
    }
    std::string text;
    for (const char16_t c : chars) {
        if (c == 0 || text.size() == MaxChars) {
            break;
        }
        text += c < 0x80 ? static_cast<char>(c) : '?';
    }
    return text;
}

std::optional<std::string> NameplateRoleLine(const PartyPanel& panel, std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const PanelMember& member : panel.members) {
        const std::string_view id = member.name.empty() ? member.member : member.name;
        if (id == name && member.vital == Vital::Down) {
            return fmt::format("<font color=\"#e74141\">Downed {}%</font>",
                               std::lround(ReviveShare(member.landed, member.needed) * 100.0f));
        }
    }
    return std::nullopt;
}

std::optional<int> RemoteTagIndex(u64 tag_data, u64 fe_data) {
    const u64 first = fe_data + HudRemoteTagsAt;
    if (tag_data < first || (tag_data - first) % HudTagDataSize != 0) {
        return std::nullopt;
    }
    const u64 index = (tag_data - first) / HudTagDataSize;
    return index < HudRemoteTagCount ? std::optional<int>{static_cast<int>(index)} : std::nullopt;
}

s32 DownedHp(s32 max, u32 landed, u32 needed) {
    if (max <= 1 || needed == 0) {
        return 1;
    }
    const s64 done = std::min<s64>(landed, needed);
    return static_cast<s32>(std::min<s64>(1 + (s64{max} - 1) * done / needed, s64{max} - 1));
}

u32 DownedState::GoDown(s64 now_ms, s32 max) {
    if (up_since && now_ms - *up_since >= PenaltyResetMs) {
        downs = 0;
    }
    ++downs;
    vital = Vital::Down;
    down_id = next_id++;
    needed = ReviveDamageNeeded(downs, max);
    drain_step = static_cast<u32>(std::max(max / 3, 1));
    landed = 0;
    seen_damage = 0;
    return down_id;
}

bool DownedState::Damage(u32 total, s64 now_ms) {
    if (vital != Vital::Down || total <= seen_damage) {
        return false;
    }
    landed = std::min(needed, landed + (total - seen_damage));
    seen_damage = total;
    drain_at = now_ms + RefillDelayMs;
    return landed >= needed;
}

void DownedState::Tick(s64 now_ms) {
    if (vital != Vital::Down || landed == 0 || now_ms < drain_at) {
        return;
    }
    // One drain step per RefillStepMs due since drain_at, worked out at once: as a loop, clang made
    // it a 128-bit division (__udivti3), which clang-cl's links on Windows lack.
    const s64 due = (now_ms - drain_at) / RefillStepMs + 1;
    const s64 steps_left = (static_cast<s64>(landed) + drain_step - 1) / drain_step;
    const s64 steps = due < steps_left ? due : steps_left;
    const s64 drained = steps * drain_step;
    landed = drained < static_cast<s64>(landed) ? landed - static_cast<u32>(drained) : 0;
    drain_at += steps * RefillStepMs;
}

void DownedState::GetUp(s64 now_ms) {
    vital = Vital::Up;
    landed = 0;
    up_since = now_ms;
}

void DownedState::Died() {
    vital = Vital::Dead;
    landed = 0;
}

void DownedState::Alive(s64 now_ms) {
    vital = Vital::Up;
    downs = 0;
    up_since = now_ms;
}

void DownedState::ClearPenalty() {
    downs = 0;
}

bool HoldToConfirm::Tick(bool held, s64 now_ms) {
    if (!held) {
        held_since.reset();
        fired = false;
        return false;
    }
    if (!held_since) {
        held_since = now_ms;
    }
    if (fired || now_ms - *held_since < hold_ms) {
        return false;
    }
    fired = true;
    return true;
}

void HoldToConfirm::Restart(bool held) {
    held_since.reset();
    fired = held;
}

float HoldToConfirm::Progress(s64 now_ms) const {
    if (!held_since || fired || hold_ms <= 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(now_ms - *held_since) / static_cast<float>(hold_ms), 0.0f,
                      1.0f);
}

std::string EncodeVitalsUpdate(const MemberVitals& self, std::span<const ReviveHitTotal> hits) {
    std::string update =
        fmt::format("{} {:x} {} {} {} {} {} {}", self.member, self.map, VitalName(self.vital),
                    self.hp, self.max, self.down, self.landed, self.needed);
    for (const float coordinate : self.at) {
        update += ' ' + FormatCoordinate(coordinate);
    }
    for (const ReviveHitTotal& total : hits) {
        update += fmt::format(" {}:{}:{}", total.target, total.down, total.damage);
    }
    return update;
}

std::optional<VitalsBoard> ParseVitalsBoard(std::string_view body) {
    VitalsBoard board;
    while (!body.empty()) {
        const std::size_t end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1);
        const std::vector<std::string_view> fields = SplitFields(line);
        if (fields.empty()) {
            continue;
        }
        if (fields[0] == "wipe") {
            const auto id = fields.size() >= 4 ? ParseNumber<u32>(fields[1]) : std::nullopt;
            const auto map = fields.size() >= 4 ? ParseNumber<u32>(fields[2], 16) : std::nullopt;
            if (!id || !map) {
                return std::nullopt;
            }
            board.wipes.push_back(PartyWipe{*id, *map, {fields.begin() + 3, fields.end()}});
            continue;
        }
        if (fields.size() != 12 || fields[0].empty()) {
            return std::nullopt;
        }
        const auto map = ParseNumber<u32>(fields[1], 16);
        const auto vital = ParseVital(fields[2]);
        const auto hp = ParseNumber<s32>(fields[3]);
        const auto max = ParseNumber<s32>(fields[4]);
        const auto down = ParseNumber<u32>(fields[5]);
        const auto landed = ParseNumber<u32>(fields[6]);
        const auto needed = ParseNumber<u32>(fields[7]);
        const auto x = ParseCoordinate(fields[8]);
        const auto y = ParseCoordinate(fields[9]);
        const auto z = ParseCoordinate(fields[10]);
        const auto revive_damage = ParseNumber<u32>(fields[11]);
        if (!map || !vital || !hp || !max || !down || !landed || !needed || !x || !y || !z ||
            !revive_damage) {
            return std::nullopt;
        }
        board.members.push_back(MemberVitals{.member = std::string{fields[0]},
                                             .map = *map,
                                             .vital = *vital,
                                             .hp = *hp,
                                             .max = *max,
                                             .down = *down,
                                             .landed = *landed,
                                             .needed = *needed,
                                             .at = {*x, *y, *z},
                                             .revive_damage = *revive_damage});
    }
    return board;
}

bool IsCoopFrameType(u8 type) {
    return (type & 0xF0) == 0x30;
}

std::vector<u8> EncodeCoopFrame(u8 type, std::span<const u8> payload) {
    std::vector<u8> frame{'S', 'H', 'A', 'D', type};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::optional<std::pair<u8, std::vector<u8>>> DecodeCoopFrame(std::span<const u8> frame) {
    if (frame.size() < 5 || !std::ranges::equal(frame.first(4), std::string_view{"SHAD"}) ||
        !IsCoopFrameType(frame[4])) {
        return std::nullopt;
    }
    return std::pair{frame[4], std::vector<u8>(frame.begin() + 5, frame.end())};
}

std::vector<u8> EncodeDirectKills(std::string_view member, std::span<const u32> handles) {
    std::vector<u8> payload;
    const size_t name = std::min<size_t>(member.size(), 0xFF);
    const size_t count = std::min<size_t>(handles.size(), 300);
    payload.push_back(static_cast<u8>(name));
    payload.insert(payload.end(), member.begin(), member.begin() + name);
    AppendLittleEndian(payload, static_cast<u16>(count));
    for (size_t index = 0; index < count; ++index) {
        AppendLittleEndian(payload, handles[index]);
    }
    return payload;
}

std::optional<DirectKills> DecodeDirectKills(std::span<const u8> payload) {
    if (payload.empty() || payload.size() < 1u + payload[0] + 2u) {
        return std::nullopt;
    }
    const size_t name = payload[0];
    DirectKills kills{std::string(payload.begin() + 1, payload.begin() + 1 + name), {}};
    const size_t count = ReadLittleEndian<u16>(payload, 1 + name);
    const size_t start = 1 + name + 2;
    if (payload.size() != start + count * 4) {
        return std::nullopt;
    }
    for (size_t index = 0; index < count; ++index) {
        kills.handles.push_back(ReadLittleEndian<u32>(payload, start + index * 4));
    }
    return kills;
}

std::vector<u8> EncodePeerHello(const PeerHello& hello) {
    std::vector<u8> payload{hello.protocol};
    const size_t revision = std::min<size_t>(hello.revision.size(), 0xFF);
    payload.push_back(static_cast<u8>(revision));
    payload.insert(payload.end(), hello.revision.begin(), hello.revision.begin() + revision);
    AppendLittleEndian(payload, hello.event_files);
    const size_t member = std::min<size_t>(hello.member.size(), 0xFF);
    payload.push_back(static_cast<u8>(member));
    payload.insert(payload.end(), hello.member.begin(), hello.member.begin() + member);
    payload.push_back(hello.features);
    return payload;
}

std::optional<PeerHello> DecodePeerHello(std::span<const u8> payload) {
    if (payload.size() < 2 || payload.size() < 2u + payload[1] + 8u + 1u) {
        return std::nullopt;
    }
    PeerHello hello{.protocol = payload[0]};
    const size_t revision = payload[1];
    hello.revision.assign(payload.begin() + 2, payload.begin() + 2 + revision);
    hello.event_files = ReadLittleEndian<u64>(payload, 2 + revision);
    const size_t member_at = 2 + revision + 8;
    const size_t member = payload[member_at];
    if (payload.size() < member_at + 1 + member) {
        return std::nullopt;
    }
    hello.member.assign(payload.begin() + member_at + 1, payload.begin() + member_at + 1 + member);
    if (const size_t features_at = member_at + 1 + member; payload.size() > features_at) {
        hello.features = payload[features_at];
    }
    return hello;
}

std::optional<std::string> PeerBuildCheck::Take(const PeerHello& theirs, const PeerHello& mine) {
    // A game that could not read its event files (0) says nothing about them.
    const bool events_differ =
        theirs.event_files != 0 && mine.event_files != 0 && theirs.event_files != mine.event_files;
    if (theirs.protocol == mine.protocol && theirs.revision == mine.revision && !events_differ) {
        return std::nullopt;
    }
    if (!warned.emplace(theirs.member, theirs.protocol, theirs.revision, theirs.event_files)
             .second) {
        return std::nullopt;
    }
    std::string line = fmt::format("Co-op: {} runs build {}, this game runs {}",
                                   theirs.member.empty() ? "a peer" : theirs.member,
                                   theirs.revision.empty() ? "?" : theirs.revision,
                                   mine.revision.empty() ? "?" : mine.revision);
    if (theirs.protocol != mine.protocol) {
        line += fmt::format(" (co-op protocol {}, this game's {})", theirs.protocol, mine.protocol);
    }
    if (events_differ) {
        line += fmt::format("; event files {:016x}, this game's {:016x}", theirs.event_files,
                            mine.event_files);
    }
    return line;
}

u64 EventFilesHash(const std::filesystem::path& folder) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{folder, error}) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".emevd.dcx")) {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        return 0;
    }
    std::ranges::sort(files);
    // FNV-1a over each file's name and bytes.
    u64 hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](std::span<const char> bytes) {
        for (const char byte : bytes) {
            hash = (hash ^ static_cast<u8>(byte)) * 0x100000001b3ull;
        }
    };
    std::vector<char> chunk(1 << 16);
    for (const auto& path : files) {
        const std::string name = path.filename().string();
        mix(name);
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            return 0;
        }
        while (file.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
               file.gcount() > 0) {
            mix(std::span{chunk.data(), static_cast<size_t>(file.gcount())});
        }
    }
    return hash != 0 ? hash : 1;
}

std::unordered_set<u32> DirectKillHandles(std::span<const DirectKills> kills) {
    std::unordered_set<u32> handles;
    for (const DirectKills& member : kills) {
        handles.insert(member.handles.begin(), member.handles.end());
    }
    return handles;
}

std::string FormatWorldLogEntry(const WorldLogEntry& entry) {
    switch (entry.kind) {
    case WorldLogKind::Reset:
        return entry.map == 0 ? fmt::format("seq {} reset every map", entry.seq)
                              : fmt::format("seq {} reset {:#x}", entry.seq, entry.map);
    case WorldLogKind::Kill:
        return fmt::format("seq {} kill {:#x} on {:#x}", entry.seq, entry.subject, entry.map);
    case WorldLogKind::Unkill:
        return fmt::format("seq {} unkill {:#x} on {:#x}", entry.seq, entry.subject, entry.map);
    }
    return fmt::format("seq {} kind {} on {:#x}", entry.seq, static_cast<int>(entry.kind),
                       entry.map);
}

size_t WorldLogKillCount(const WorldLogKills& kills) {
    size_t count = 0;
    for (const auto& [map, handles] : kills) {
        count += handles.size();
    }
    return count;
}

std::unordered_set<u32> WorldLogHandles(const WorldLogKills& kills) {
    std::unordered_set<u32> handles;
    for (const auto& [map, on_map] : kills) {
        handles.insert(on_map.begin(), on_map.end());
    }
    return handles;
}

u64 WorldLogEpoch(u32 run, u32 count) {
    return (u64{run} << 32) | count;
}

bool WorldLogEpochFollows(u64 next, u64 current) {
    if (next == 0) {
        return false;
    }
    if (current == 0 || (next >> 32) != (current >> 32)) {
        return true;
    }
    return static_cast<u32>(next) > static_cast<u32>(current);
}

bool IsWorldLogFrameType(u8 type) {
    return type >= WorldLogProposeType && type <= WorldLogSnapshotType;
}

bool WorldLogEnabled(const char* setting) {
    const std::string_view value = setting != nullptr ? setting : "";
    return value != "0" && value != "off";
}

namespace {

// Reads a world log payload front to back; a read past the end fails it.
class PayloadReader {
public:
    explicit PayloadReader(std::span<const u8> bytes) : bytes{bytes} {}
    template <typename T>
    std::optional<T> Next() {
        if (failed || at + sizeof(T) > bytes.size()) {
            failed = true;
            return std::nullopt;
        }
        const T value = ReadLittleEndian<T>(bytes, at);
        at += sizeof(T);
        return value;
    }
    std::optional<std::string> Name() {
        const auto size = Next<u8>();
        if (!size || at + *size > bytes.size()) {
            failed = true;
            return std::nullopt;
        }
        std::string name(bytes.begin() + at, bytes.begin() + at + *size);
        at += *size;
        return name;
    }
    // Everything read, and nothing left over.
    bool Done() const {
        return !failed && at == bytes.size();
    }

private:
    std::span<const u8> bytes;
    size_t at = 0;
    bool failed = false;
};

void AppendName(std::vector<u8>& payload, std::string_view name) {
    const size_t size = std::min<size_t>(name.size(), 0xFF);
    payload.push_back(static_cast<u8>(size));
    payload.insert(payload.end(), name.begin(), name.begin() + size);
}

// u16 maps, then per map: u32 map, u16 count, the handles (u32). At most WorldLogHandlesPerFrame
// handles in all.
void AppendKills(std::vector<u8>& payload, const WorldLogKills& kills) {
    std::vector<std::pair<u32, std::vector<u32>>> kept;
    size_t room = WorldLogHandlesPerFrame;
    for (const auto& [map, handles] : kills) {
        const auto end =
            std::next(handles.begin(), static_cast<std::ptrdiff_t>(std::min(room, handles.size())));
        std::vector<u32> some(handles.begin(), end);
        room -= some.size();
        if (!some.empty()) {
            kept.emplace_back(map, std::move(some));
        }
    }
    AppendLittleEndian(payload, static_cast<u16>(kept.size()));
    for (const auto& [map, handles] : kept) {
        AppendLittleEndian(payload, map);
        AppendLittleEndian(payload, static_cast<u16>(handles.size()));
        for (const u32 handle : handles) {
            AppendLittleEndian(payload, handle);
        }
    }
}

std::optional<WorldLogKills> ReadKills(PayloadReader& reader) {
    const auto maps = reader.Next<u16>();
    if (!maps) {
        return std::nullopt;
    }
    WorldLogKills kills;
    for (u16 index = 0; index < *maps; ++index) {
        const auto map = reader.Next<u32>();
        const auto count = reader.Next<u16>();
        if (!map || !count || *count == 0) {
            return std::nullopt;
        }
        std::set<u32>& handles = kills[*map];
        for (u16 at = 0; at < *count; ++at) {
            const auto handle = reader.Next<u32>();
            if (!handle) {
                return std::nullopt;
            }
            handles.insert(*handle);
        }
    }
    return kills;
}

} // namespace

std::vector<u8> EncodeWorldLogProposal(const WorldLogProposal& proposal) {
    std::vector<u8> payload;
    AppendName(payload, proposal.member);
    AppendLittleEndian(payload, proposal.epoch);
    AppendLittleEndian(payload, proposal.run);
    AppendLittleEndian(payload, proposal.number);
    AppendKills(payload, proposal.kills);
    return payload;
}

std::optional<WorldLogProposal> DecodeWorldLogProposal(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto member = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto run = reader.Next<u32>();
    const auto number = reader.Next<u32>();
    auto kills = ReadKills(reader);
    if (!member || !epoch || !run || !number || !kills || !reader.Done()) {
        return std::nullopt;
    }
    return WorldLogProposal{std::move(*member), *epoch, *run, *number, std::move(*kills)};
}

// The host's name, epoch, last seq, the entry count (u8) and the first entry's seq (u64), then
// each entry: kind (u8), map, subject.
std::vector<u8> EncodeWorldLogBatch(const WorldLogBatch& batch) {
    std::vector<u8> payload;
    AppendName(payload, batch.host);
    AppendLittleEndian(payload, batch.epoch);
    AppendLittleEndian(payload, batch.last);
    const size_t count = std::min<size_t>(batch.entries.size(), 0xFF);
    payload.push_back(static_cast<u8>(count));
    AppendLittleEndian(payload, count > 0 ? batch.entries.front().seq : batch.last + 1);
    for (size_t index = 0; index < count; ++index) {
        const WorldLogEntry& entry = batch.entries[index];
        payload.push_back(static_cast<u8>(entry.kind));
        AppendLittleEndian(payload, entry.map);
        AppendLittleEndian(payload, entry.subject);
    }
    return payload;
}

std::optional<WorldLogBatch> DecodeWorldLogBatch(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto count = reader.Next<u8>();
    const auto first = reader.Next<u64>();
    if (!host || !epoch || !last || !count || !first ||
        (*count > 0 && (*first == 0 || *first + *count - 1 > *last))) {
        return std::nullopt;
    }
    WorldLogBatch batch{std::move(*host), *epoch, *last, {}};
    for (u8 index = 0; index < *count; ++index) {
        const auto kind = reader.Next<u8>();
        const auto map = reader.Next<u32>();
        const auto subject = reader.Next<u32>();
        if (!kind || !map || !subject) {
            return std::nullopt;
        }
        batch.entries.push_back(
            {*first + index, *epoch, *map, static_cast<WorldLogKind>(*kind), *subject});
    }
    if (!reader.Done()) {
        return std::nullopt;
    }
    return batch;
}

std::vector<u8> EncodeWorldLogSnapshotRequest(const WorldLogSnapshotRequest& request) {
    std::vector<u8> payload;
    AppendName(payload, request.member);
    AppendLittleEndian(payload, request.epoch);
    AppendLittleEndian(payload, request.seq);
    return payload;
}

std::optional<WorldLogSnapshotRequest> DecodeWorldLogSnapshotRequest(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto member = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto seq = reader.Next<u64>();
    if (!member || !epoch || !seq || !reader.Done()) {
        return std::nullopt;
    }
    return WorldLogSnapshotRequest{std::move(*member), *epoch, *seq};
}

std::vector<u8> EncodeWorldLogSnapshot(const WorldLogSnapshot& snapshot) {
    std::vector<u8> payload;
    AppendName(payload, snapshot.host);
    AppendLittleEndian(payload, snapshot.epoch);
    AppendLittleEndian(payload, snapshot.last);
    payload.push_back(snapshot.part);
    payload.push_back(snapshot.parts);
    AppendKills(payload, snapshot.kills);
    return payload;
}

std::optional<WorldLogSnapshot> DecodeWorldLogSnapshot(std::span<const u8> payload) {
    PayloadReader reader{payload};
    auto host = reader.Name();
    const auto epoch = reader.Next<u64>();
    const auto last = reader.Next<u64>();
    const auto part = reader.Next<u8>();
    const auto parts = reader.Next<u8>();
    auto kills = ReadKills(reader);
    if (!host || !epoch || !last || !part || !parts || *part >= *parts || !kills ||
        !reader.Done()) {
        return std::nullopt;
    }
    return WorldLogSnapshot{std::move(*host), *epoch, *last, *part, *parts, std::move(*kills)};
}

std::vector<WorldLogBatch> BatchWorldLog(std::string_view host, u64 epoch, u64 last,
                                         std::span<const WorldLogEntry> entries) {
    std::vector<WorldLogBatch> batches;
    for (size_t at = 0; at < entries.size(); at += WorldLogEntriesPerFrame) {
        const size_t count = std::min(WorldLogEntriesPerFrame, entries.size() - at);
        const auto from = entries.begin() + static_cast<std::ptrdiff_t>(at);
        batches.push_back(
            {std::string{host}, epoch, last,
             std::vector<WorldLogEntry>(from, from + static_cast<std::ptrdiff_t>(count))});
    }
    if (batches.empty()) {
        batches.push_back({std::string{host}, epoch, last, {}});
    }
    return batches;
}

WorldLogEntry WorldLogHost::Append(WorldLogKind kind, u32 map, u32 subject) {
    return {++last, epoch, map, kind, subject};
}

void WorldLogHost::Own(const std::string& member, u32 map, u32 handle,
                       std::vector<WorldLogEntry>& entries) {
    const auto [it, added] = standing.try_emplace(handle, Standing{map, {}});
    it->second.by.insert(member);
    if (added) {
        entries.push_back(Append(WorldLogKind::Kill, map, handle));
    }
}

void WorldLogHost::Disown(const std::string& member, u32 handle,
                          std::vector<WorldLogEntry>& entries) {
    const auto it = standing.find(handle);
    if (it == standing.end()) {
        return;
    }
    it->second.by.erase(member);
    if (it->second.by.empty()) {
        entries.push_back(Append(WorldLogKind::Unkill, it->second.map, handle));
        standing.erase(it);
    }
}

std::vector<WorldLogEntry> WorldLogHost::Start(u64 new_epoch, bool keep_members) {
    epoch = new_epoch;
    last = 0;
    if (!keep_members) {
        members.clear();
        standing.clear();
    }
    std::vector<WorldLogEntry> entries{Append(WorldLogKind::Reset, 0, 0)};
    for (const auto& [map, handles] : Kills()) {
        for (const u32 handle : handles) {
            entries.push_back(Append(WorldLogKind::Kill, map, handle));
        }
    }
    return entries;
}

WorldLogHost::Taken WorldLogHost::Propose(const WorldLogProposal& proposal) {
    Taken taken;
    const auto known = members.find(proposal.member);
    const bool late = known != members.end() && known->second.run == proposal.run &&
                      proposal.number <= known->second.number;
    if (late || (proposal.epoch != 0 && proposal.epoch != epoch)) {
        taken.stale = true;
        return taken;
    }
    Member& member = members[proposal.member];
    const std::unordered_set<u32> before = WorldLogHandles(member.kills);
    const std::unordered_set<u32> after = WorldLogHandles(proposal.kills);
    for (const auto& [map, handles] : member.kills) {
        for (const u32 handle : handles) {
            if (!after.contains(handle)) {
                Disown(proposal.member, handle, taken.entries);
            }
        }
    }
    for (const auto& [map, handles] : proposal.kills) {
        for (const u32 handle : handles) {
            if (!before.contains(handle)) {
                Own(proposal.member, map, handle, taken.entries);
            }
        }
    }
    member = Member{proposal.run, proposal.number, proposal.kills};
    return taken;
}

WorldLogKills WorldLogHost::Kills() const {
    WorldLogKills kills;
    for (const auto& [handle, kill] : standing) {
        kills[kill.map].insert(handle);
    }
    return kills;
}

std::vector<WorldLogSnapshot> WorldLogHost::Snapshot(std::string_view host) const {
    std::vector<WorldLogSnapshot> parts{{std::string{host}, epoch, last, 0, 1, {}}};
    size_t in_part = 0;
    for (const auto& [map, handles] : Kills()) {
        for (const u32 handle : handles) {
            if (in_part == WorldLogHandlesPerFrame && parts.size() < 0xFF) {
                parts.push_back(
                    {std::string{host}, epoch, last, static_cast<u8>(parts.size()), 1, {}});
                in_part = 0;
            }
            parts.back().kills[map].insert(handle);
            ++in_part;
        }
    }
    for (WorldLogSnapshot& part : parts) {
        part.parts = static_cast<u8>(parts.size());
    }
    return parts;
}

void WorldLogMember::Reset() {
    epoch = 0;
    applied = 0;
    host_last = 0;
    based = false;
    host.clear();
    waiting.clear();
    heard_ms.reset();
    asked_ms.reset();
    partial.reset();
    kills.clear();
}

bool WorldLogMember::Follow(u64 next_epoch, std::string_view next_host, s64 now_ms, Step& step) {
    if (next_epoch != epoch) {
        if (!WorldLogEpochFollows(next_epoch, epoch)) {
            step.stale = true;
            return false;
        }
        Reset();
        epoch = next_epoch;
        step.new_epoch = true;
    }
    host = next_host;
    heard_ms = now_ms;
    return true;
}

bool WorldLogMember::Behind() const {
    return !based || !waiting.empty() || host_last > applied;
}

void WorldLogMember::Apply(const WorldLogEntry& entry) {
    switch (entry.kind) {
    case WorldLogKind::Reset:
        if (entry.map == 0) {
            kills.clear();
        } else {
            kills.erase(entry.map);
        }
        return;
    case WorldLogKind::Kill:
        kills[entry.map].insert(entry.subject);
        return;
    case WorldLogKind::Unkill:
        if (const auto on_map = kills.find(entry.map); on_map != kills.end()) {
            on_map->second.erase(entry.subject);
            if (on_map->second.empty()) {
                kills.erase(on_map);
            }
        }
        return;
    }
}

void WorldLogMember::Drain(Step& step) {
    std::erase_if(waiting, [this](const auto& entry) { return entry.first <= applied; });
    while (based && !waiting.empty() && waiting.begin()->first == applied + 1) {
        Apply(waiting.begin()->second);
        step.applied.push_back(waiting.begin()->second);
        applied = waiting.begin()->first;
        waiting.erase(waiting.begin());
    }
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogBatch& batch, s64 now_ms) {
    Step step;
    if (!Follow(batch.epoch, batch.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, batch.last);
    for (const WorldLogEntry& entry : batch.entries) {
        if (entry.seq > applied && waiting.size() < WorldLogWaitingCap) {
            waiting.emplace(entry.seq, entry);
        }
    }
    // A log from its start needs no snapshot.
    if (!based && applied == 0 && !waiting.empty() && waiting.begin()->first == 1) {
        based = true;
    }
    Drain(step);
    step.gap = Behind();
    return step;
}

WorldLogMember::Step WorldLogMember::Take(const WorldLogSnapshot& part, s64 now_ms) {
    Step step;
    if (!Follow(part.epoch, part.host, now_ms, step)) {
        return step;
    }
    host_last = std::max(host_last, part.last);
    if (based && part.last <= applied) {
        step.gap = Behind();
        return step;
    }
    if (!partial || partial->last != part.last || partial->parts != part.parts) {
        partial = PartialSnapshot{part.last, part.parts, {}};
    }
    partial->got[part.part] = part.kills;
    if (partial->got.size() < partial->parts) {
        step.gap = Behind();
        return step;
    }
    kills.clear();
    for (const auto& [index, share] : partial->got) {
        for (const auto& [map, handles] : share) {
            kills[map].insert(handles.begin(), handles.end());
        }
    }
    applied = partial->last;
    based = true;
    partial.reset();
    step.snapshot = true;
    Drain(step);
    step.gap = Behind();
    return step;
}

bool WorldLogMember::SnapshotDue(s64 now_ms) {
    if (!heard_ms || !Behind() || (asked_ms && now_ms - *asked_ms < WorldLogSnapshotRetryMs)) {
        return false;
    }
    asked_ms = now_ms;
    return true;
}

bool WorldLogMember::Fresh(s64 now_ms) const {
    return !Behind() && heard_ms && now_ms - *heard_ms <= WorldLogLifeMs;
}

namespace {

// What the peers' hellos say: a definite no (a peer without the log), or unsure (no peers, or a
// peer's hello not here yet).
struct PeerVerdict {
    std::optional<std::string> no;
    std::optional<std::string> unsure;
};
PeerVerdict JudgePeers(std::span<const WorldLogPeer> peers) {
    PeerVerdict verdict;
    if (peers.empty()) {
        verdict.unsure = "no peers";
    }
    for (const WorldLogPeer& peer : peers) {
        if (!peer.hello) {
            verdict.unsure = verdict.unsure.value_or("no hello from " + peer.name);
        } else if (peer.hello->protocol < 2) {
            verdict.no = fmt::format("{} runs co-op protocol {}", peer.name, peer.hello->protocol);
            return verdict;
        } else if ((peer.hello->features & WorldLogFeature) == 0) {
            verdict.no = peer.name + " has it switched off";
            return verdict;
        }
    }
    return verdict;
}

} // namespace

bool WorldLogPath::Decide(bool enabled, std::span<const WorldLogPeer> peers, s64 now_ms) {
    const PeerVerdict verdict =
        enabled ? JudgePeers(peers) : PeerVerdict{std::string{"switched off here"}, {}};
    const std::string before = why;
    bool next = true;
    if (verdict.no) {
        next = false;
        why = *verdict.no;
        unsure_since.reset();
    } else if (verdict.unsure) {
        unsure_since = unsure_since.value_or(now_ms);
        if (!active || now_ms - *unsure_since >= WorldLogUnsureMs) {
            next = false;
            why = *verdict.unsure;
        }
    } else {
        unsure_since.reset();
        why = peers.size() == 1 ? "1 peer has it" : fmt::format("{} peers have it", peers.size());
    }
    changed = changed || !decided || next != active || why != before;
    decided = true;
    active = next;
    return active;
}

std::optional<std::string> WorldLogPath::TakeChange() {
    if (!changed) {
        return std::nullopt;
    }
    changed = false;
    return active ? fmt::format("Co-op world log: on, kills go through the host's log ({})", why)
                  : fmt::format("Co-op world log: off, kills go through the kill board ({})", why);
}

std::vector<u8> EncodeWorldLogStart(u64 epoch, bool keep_members) {
    std::vector<u8> payload;
    AppendLittleEndian(payload, epoch);
    payload.push_back(keep_members ? 1 : 0);
    return payload;
}

std::optional<std::pair<u64, bool>> DecodeWorldLogStart(std::span<const u8> payload) {
    PayloadReader reader{payload};
    const auto epoch = reader.Next<u64>();
    const auto keep = reader.Next<u8>();
    if (!epoch || !keep || *keep > 1 || !reader.Done()) {
        return std::nullopt;
    }
    return std::pair{*epoch, *keep == 1};
}

std::optional<DirectVitals> ParseVitalsUpdate(std::string_view line) {
    const std::vector<std::string_view> fields = SplitFields(line);
    if (fields.size() < 11 || fields[0].empty()) {
        return std::nullopt;
    }
    const auto map = ParseNumber<u32>(fields[1], 16);
    const auto vital = ParseVital(fields[2]);
    const auto hp = ParseNumber<s32>(fields[3]);
    const auto max = ParseNumber<s32>(fields[4]);
    const auto down = ParseNumber<u32>(fields[5]);
    const auto landed = ParseNumber<u32>(fields[6]);
    const auto needed = ParseNumber<u32>(fields[7]);
    const auto x = ParseCoordinate(fields[8]);
    const auto y = ParseCoordinate(fields[9]);
    const auto z = ParseCoordinate(fields[10]);
    if (!map || !vital || !hp || !max || !down || !landed || !needed || !x || !y || !z) {
        return std::nullopt;
    }
    DirectVitals update{MemberVitals{.member = std::string{fields[0]},
                                     .map = *map,
                                     .vital = *vital,
                                     .hp = *hp,
                                     .max = *max,
                                     .down = *down,
                                     .landed = *landed,
                                     .needed = *needed,
                                     .at = {*x, *y, *z}},
                        {}};
    for (size_t index = 11; index < fields.size(); ++index) {
        const std::string_view hit = fields[index];
        const size_t first = hit.find(':');
        const size_t second = first == std::string_view::npos ? first : hit.find(':', first + 1);
        if (second == std::string_view::npos) {
            return std::nullopt;
        }
        const auto hit_down = ParseNumber<u32>(hit.substr(first + 1, second - first - 1));
        const auto damage = ParseNumber<u32>(hit.substr(second + 1));
        if (first == 0 || !hit_down || !damage) {
            return std::nullopt;
        }
        update.hits.push_back({std::string{hit.substr(0, first)}, *hit_down, *damage});
    }
    return update;
}

VitalsBoard MergeDirectVitals(const VitalsBoard& board, std::span<const DirectVitals> direct) {
    VitalsBoard merged = board;
    for (const DirectVitals& update : direct) {
        const auto same =
            std::ranges::find(merged.members, update.self.member, &MemberVitals::member);
        if (same == merged.members.end()) {
            merged.members.push_back(update.self);
            merged.members.back().revive_damage = 0;
            continue;
        }
        const bool same_down = same->down == update.self.down;
        const u32 board_damage = same_down ? same->revive_damage : 0;
        *same = update.self;
        same->revive_damage = board_damage;
    }
    for (MemberVitals& member : merged.members) {
        u32 hits = 0;
        for (const DirectVitals& update : direct) {
            for (const ReviveHitTotal& total : update.hits) {
                if (total.target == member.member && total.down == member.down) {
                    hits += total.damage;
                }
            }
        }
        member.revive_damage = std::max(member.revive_damage, hits);
    }
    return merged;
}

std::optional<u32> WipeFor(const VitalsBoard& board, std::string_view self) {
    for (const PartyWipe& wipe : board.wipes) {
        if (std::ranges::find(wipe.members, self) != wipe.members.end()) {
            return wipe.id;
        }
    }
    return std::nullopt;
}

bool SomeoneElseStands(std::span<const MemberVitals> board, std::string_view self, u32 map) {
    return map != 0 && std::ranges::any_of(board, [&](const MemberVitals& member) {
               return member.member != self && member.map == map && member.vital == Vital::Up;
           });
}

std::vector<MemberVitals> OnlySessionMembers(
    std::span<const MemberVitals> board, std::string_view self,
    const std::optional<std::vector<std::string>>& session) {
    std::vector<MemberVitals> kept;
    for (const MemberVitals& member : board) {
        if (!session || member.member == self || std::ranges::contains(*session, member.member)) {
            kept.push_back(member);
        }
    }
    return kept;
}

std::optional<std::vector<std::string>> SessionBoardNames(
    const std::vector<std::optional<std::string>>& found) {
    std::vector<std::string> names;
    for (const auto& name : found) {
        if (!name) {
            return std::nullopt;
        }
        names.push_back(*name);
    }
    return names;
}

bool SessionArrival::Tick(bool in_session, bool summoned, u64 visit, bool playing, s64 now_ms) {
    if (!in_session) {
        joined.reset();
        arrived = false;
        return false;
    }
    if (!joined) {
        joined = std::pair{visit, now_ms};
        arrived = !summoned;
    }
    if (!arrived) {
        arrived = (playing && visit != joined->first) || now_ms - joined->second >= LimitMs;
    }
    return arrived;
}

bool ReviveHits::Press(std::string_view target, u32 down, u32 damage, s64 now_ms) {
    if (last_press && now_ms - *last_press < GapMs) {
        return false;
    }
    last_press = now_ms;
    const auto total = std::ranges::find(totals, target, &ReviveHitTotal::target);
    if (total == totals.end()) {
        totals.push_back(ReviveHitTotal{std::string{target}, down, damage});
    } else if (total->down != down) {
        *total = ReviveHitTotal{std::string{target}, down, damage};
    } else {
        total->damage += damage;
    }
    return true;
}

void ReviveHits::Hit(std::string_view target, u32 down, u32 damage) {
    if (damage == 0) {
        return;
    }
    const auto total = std::ranges::find(totals, target, &ReviveHitTotal::target);
    if (total == totals.end()) {
        totals.push_back(ReviveHitTotal{std::string{target}, down, damage});
    } else if (total->down != down) {
        *total = ReviveHitTotal{std::string{target}, down, damage};
    } else {
        total->damage += damage;
    }
}

std::vector<CopyTeam> DownedTeams::Tick(const std::vector<CopyTeam>& downed,
                                        const std::vector<CopyTeam>& present) {
    std::vector<CopyTeam> writes;
    for (const CopyTeam& body : downed) {
        if (body.team == Unchangeable) {
            continue;
        }
        if (std::ranges::find(saved, body.handle, &CopyTeam::handle) == saved.end()) {
            saved.push_back(body);
        }
        if (body.team != Hostile) {
            writes.push_back(CopyTeam{body.handle, Hostile});
        }
    }
    std::erase_if(saved, [&](const CopyTeam& own) {
        if (std::ranges::find(downed, own.handle, &CopyTeam::handle) != downed.end()) {
            return false;
        }
        const auto body = std::ranges::find(present, own.handle, &CopyTeam::handle);
        if (body != present.end() && body->team == Hostile) {
            writes.push_back(own);
        }
        return true;
    });
    return writes;
}

std::vector<ReviveHitTotal> ReviveHits::Totals(std::span<const MemberVitals> board) {
    std::erase_if(totals, [&](const ReviveHitTotal& total) {
        return std::ranges::none_of(board, [&](const MemberVitals& member) {
            return member.member == total.target && member.vital == Vital::Down &&
                   member.down == total.down;
        });
    });
    return totals;
}

std::optional<DownedMember> ReviveTarget(std::span<const MemberVitals> board, std::string_view self,
                                         u32 map, const LampPosition& me) {
    std::optional<DownedMember> nearest;
    float nearest_distance = ReviveHits::Reach * ReviveHits::Reach;
    for (const MemberVitals& member : board) {
        if (member.member == self || member.map != map || member.vital != Vital::Down) {
            continue;
        }
        const float distance = DistanceSquared(member.at, me);
        if (distance <= nearest_distance) {
            nearest_distance = distance;
            nearest = DownedMember{member.member, member.down};
        }
    }
    return nearest;
}

PadInput DownedPad(const PadInput& pad) {
    return PadInput{.buttons = pad.buttons & PadOptions,
                    .sticks = {128, 128, pad.sticks[2], pad.sticks[3]}};
}

bool AttackPressed(u32 before, u32 now) {
    return (now & ~before & (PadR1 | PadR2)) != 0;
}

bool HeavyAttackPressed(u32 before, u32 now) {
    return (now & ~before & PadR2) != 0 && (now & PadR1) == 0;
}

DownEnd EndOfDown(bool wiped, bool gave_up, bool revived, s64 board_age_ms) {
    if (wiped) {
        return DownEnd::Wipe;
    }
    if (gave_up) {
        return DownEnd::GiveUp;
    }
    if (revived) {
        return DownEnd::Revive;
    }
    return board_age_ms >= BoardLostMs ? DownEnd::BoardLost : DownEnd::None;
}

bool ForgetsTargetNode(u8 slot, s32 kind, u32 handle, std::span<const u32> downed) {
    return (slot & 0x7F) == 1 && kind == 1 && std::ranges::find(downed, handle) != downed.end();
}

std::vector<LampPosition> OthersDownOnMap(std::span<const MemberVitals> board,
                                          std::string_view self, u32 map) {
    std::vector<LampPosition> downed;
    for (const MemberVitals& member : board) {
        if (map != 0 && member.member != self && member.map == map && member.vital == Vital::Down) {
            downed.push_back(member.at);
        }
    }
    return downed;
}

bool DeathResetsMap(bool wiped, bool someone_else_stands) {
    return wiped || !someone_else_stands;
}

PartyPanel BuildPartyPanel(std::span<const MemberVitals> board, std::string_view self, u32 map,
                           const std::optional<LampPosition>& me) {
    PartyPanel panel;
    const auto target = me ? ReviveTarget(board, self, map, *me) : std::nullopt;
    for (const MemberVitals& entry : board) {
        if (entry.member == self || entry.vital == Vital::Off) {
            continue;
        }
        if (target && entry.member == target->member) {
            panel.revive_target = static_cast<s32>(panel.members.size());
        }
        panel.members.push_back(PanelMember{
            .member = entry.member,
            .name = OnlineIdOfBoardMember(entry.member),
            .vital = entry.vital,
            .here = map != 0 && entry.map == map,
            .hp = entry.hp,
            .max = entry.max,
            .landed = entry.landed,
            .needed = entry.needed,
        });
    }
    return panel;
}

} // namespace Core::Bloodborne
