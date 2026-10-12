// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The rig's autoplay: its script verbs, census and pad. Split from bloodborne_re.cpp; the
// shared declarations are in bloodborne_re_internal.h.

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

// Autoplay (SHADPS4_BLOODBORNE_AUTOPLAY=<script>, see ParseAutoplayScript): tests with no one at
// the pad. The pad reader (AutoplayPad) presses Cross on the title screen until the player is in
// the world; the party tick then runs the script's commands one after the other, logging each
// ("Bloodborne autoplay: step 3/9 travel 2102962 ok"), and "Bloodborne autoplay: done (ok)" or
// "done (failed)" at the end, which the rig's test driver waits for.
struct AutoplayRun {
    enum class Phase { Off, Title, World, Done };
    Phase phase = Phase::Off;
    std::vector<AutoplayCommand> script;
    size_t next = 0;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point step_started{};
    bool step_begun = false;
    std::optional<std::chrono::steady_clock::time_point> hold_until;
    u32 travel_map = 0;
    std::optional<double> travel_fixed_at;     // seconds into the step it was moved to the lamp
    const LampPoint* travel_planned = nullptr; // the lamp the travel was placed at, if any
    u32 attack_handle = 0;
    std::optional<u32> attack_placed; // the attack cycle the player was last put in place for
    s32 noted_max_hp = 0;             // note-max-hp: the player's max HP then
    u32 pickup_handle = 0;            // pickup: the map item it went for
    s32 hit_from_hp = 0;              // hit: the target's HP when it began
    std::string script_path;          // sync: the go files are beside it (AutoplaySyncPath)
    std::string chapter;              // the chapter the script is in (a rig suite's section)
    bool keep_going = false;          // keep-going on: a failed step gives up only its chapter
    bool failed = false;              // a step failed and the script went on: "done (failed)"
    LampPosition glide_from{};        // glide: where the player was when it began
};
std::mutex autoplay_mutex;
AutoplayRun autoplay_run;
std::atomic<u32> autoplay_buttons{0};
std::atomic<bool> autoplay_godmode{false};
std::atomic<u32> autoplay_sticks{0x80808080}; // lx, ly, rx, ry
std::once_flag autoplay_loaded;

void LoadAutoplay() {
    const char* path = std::getenv("SHADPS4_BLOODBORNE_AUTOPLAY");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>{file}, {}};
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.script = ParseAutoplayScript(text);
    autoplay_run.script_path = path;
    autoplay_run.phase = AutoplayRun::Phase::Title;
    autoplay_run.started = std::chrono::steady_clock::now();
    LOG_INFO(Debug, "Bloodborne autoplay: {} steps from {}", autoplay_run.script.size(), path);
}

// The rig's checks and reports read the game's own map (GetCurrentPackedMap), not the place
// tracker: they test the game, the tracker included; only comparisons with a board's maps use
// the tracker's, as the boards do.
bool AutoplayInWorld() {
    const u64 global = GetGlobalState();
    return GetLocalPlayer() >= 0x10000 && IsUsablePackedMap(GetCurrentPackedMap()) &&
           global >= 0x10000 && HasMemoryAccess(global, 0x09, MemoryProt::CpuRead) &&
           ReadValue<u8>(global, 0x08) == 0 && LocalPlayerPosition().has_value();
}

size_t AutoplaySessionMembers() {
    const u64 session = ReadValue<u64>(image_base + SessionManagerPointerOffset, 0);
    if (session < 0x10000 || !HasMemoryAccess(session, 0x128, MemoryProt::CpuRead)) {
        return 1;
    }
    const u64 begin = ReadValue<u64>(session, 0xF8);
    const u64 end = ReadValue<u64>(session, 0x100);
    return begin >= 0x10000 && end > begin && end - begin <= 0x20 * 8 ? (end - begin) / 0x20 : 1;
}

size_t AutoplayMembersOnMyMap() {
    const u32 map = PlayingMap();
    std::scoped_lock lock{map_board_mutex};
    if (std::chrono::steady_clock::now() - map_board_time > std::chrono::seconds{3}) {
        return 0;
    }
    return static_cast<size_t>(
        std::ranges::count_if(map_board, [&](const MemberMap& entry) { return entry.map == map; }));
}

struct AutoplayCharacter {
    u64 character = 0;
    u64 data = 0;
    float distance = 0.0f;
};

// The character a command names: by handle, or the nearest living one within 30 m (0).
std::atomic<u32> autoplay_marked{0};

std::optional<AutoplayCharacter> FindAutoplayCharacter(u32 handle) {
    if (handle == AutoplayMarked) {
        handle = autoplay_marked.load();
        if (handle == 0) {
            return std::nullopt;
        }
    }
    if (handle == AutoplaySelf) {
        const u64 player = GetLocalPlayer();
        const u64 modules = player >= 0x10000 ? ReadValue<u64>(player, 0x3B0) : 0;
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        return data >= 0x10000 ? std::optional{AutoplayCharacter{player, data, 0.0f}}
                               : std::nullopt;
    }
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const auto me = LocalPlayerPosition();
    if (world < 0x10000 || !me) {
        return std::nullopt;
    }
    std::optional<AutoplayCharacter> found;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character == GetLocalPlayer()) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (data < 0x10000 || physics < 0x10000) {
            return;
        }
        const float dx = ReadValue<float>(physics, 0x1E0) - (*me)[0];
        const float dy = ReadValue<float>(physics, 0x1E4) - (*me)[1];
        const float dz = ReadValue<float>(physics, 0x1E8) - (*me)[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (handle != 0) {
            if (ReadValue<u32>(character, 0x08) == handle) {
                found = AutoplayCharacter{character, data, distance};
            }
            return;
        }
        if (ReadValue<s32>(data, 0xF8) > 0 && distance < 30.0f &&
            (!found || distance < found->distance)) {
            found = AutoplayCharacter{character, data, distance};
        }
    });
    return found;
}

void LogAutoplaySnapshot() {
    const auto at = LocalPlayerPosition();
    std::string enemies;
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    std::vector<std::pair<float, std::string>> close_by;
    if (world >= 0x10000 && at) {
        ForEachWorldCharacter(world, [&](u64 character) {
            if (character == GetLocalPlayer()) {
                return;
            }
            const u64 modules = ReadValue<u64>(character, 0x3B0);
            const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
            const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
            if (data < 0x10000 || physics < 0x10000) {
                return;
            }
            const float dx = ReadValue<float>(physics, 0x1E0) - (*at)[0];
            const float dy = ReadValue<float>(physics, 0x1E4) - (*at)[1];
            const float dz = ReadValue<float>(physics, 0x1E8) - (*at)[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance < 30.0f) {
                close_by.emplace_back(
                    distance,
                    fmt::format("{:#x} hp {}/{} dead {:#x} at {:.1f} m ({:.1f}, {:.1f}, {:.1f})",
                                ReadValue<u32>(character, 0x08), ReadValue<s32>(data, 0xF8),
                                ReadValue<s32>(data, 0xFC), ReadValue<u8>(character, 0x1E3),
                                distance, ReadValue<float>(physics, 0x1E0),
                                ReadValue<float>(physics, 0x1E4),
                                ReadValue<float>(physics, 0x1E8)));
            }
        });
    }
    std::ranges::sort(close_by);
    for (size_t index = 0; index < close_by.size() && index < 6; ++index) {
        enemies += " [" + close_by[index].second + "]";
    }
    const auto self = FindAutoplayCharacter(AutoplaySelf);
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    LOG_INFO(Debug,
             "Bloodborne autoplay snapshot: map {:#x} at ({:.1f}, {:.1f}, {:.1f}) heading {:.2f} "
             "hp {}/{} session {} members on map {} in party session {} nearby:{}",
             GetCurrentPackedMap(), at ? (*at)[0] : 0.0f, at ? (*at)[1] : 0.0f,
             at ? (*at)[2] : 0.0f, LocalPlayerHeading().value_or(0.0f),
             self ? ReadValue<s32>(self->data, 0xF8) : 0,
             self ? ReadValue<s32>(self->data, 0xFC) : 0, AutoplaySessionMembers(),
             AutoplayMembersOnMyMap(), InPartySession(), enemies.empty() ? " none" : enemies);
}

// census [<meters>]: every character that close (default 60 m), nearest first, with its HP, death
// flags (+0x1E3), story hide (+0x28) and explicit disable ([+0x18]+0x20 bit 0): which world
// shows or killed what (the clinic's beast, Iosefka).
// A character's backread level ([+0x18] +0x10 now, +0x14 wanted; 4 full, 0 none: the state
// machine 0x018BD320) and its draw flags ([+0x58] +0x114, bit 0x40 cleared by the enable switch),
// for the census: which copy of a character is drawn in one world and not the other.
s32 BackreadLevel(u64 character, u64 offset) {
    const u64 control = ReadValue<u64>(character, 0x18);
    return control >= 0x10000 ? ReadValue<s32>(control, offset) : -1;
}

u32 DrawFlags(u64 character) {
    const u64 model = ReadValue<u64>(character, 0x58);
    return model >= 0x10000 && HasMemoryAccess(model, 0x116, MemoryProt::CpuRead)
               ? ReadValue<u16>(model, 0x114)
               : 0xFFFFFFFF;
}

void LogAutoplayCensus(float meters) {
    const auto at = LocalPlayerPosition();
    const u64 world = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    if (world < 0x10000 || !at) {
        LOG_INFO(Debug, "Bloodborne autoplay census: no world");
        return;
    }
    std::vector<std::pair<float, std::string>> close_by;
    ForEachWorldCharacter(world, [&](u64 character) {
        if (character == GetLocalPlayer()) {
            return;
        }
        const u64 modules = ReadValue<u64>(character, 0x3B0);
        const u64 data = modules >= 0x10000 ? ReadValue<u64>(modules, 0x20) : 0;
        const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
        if (data < 0x10000 || physics < 0x10000) {
            return;
        }
        const float x = ReadValue<float>(physics, 0x1E0);
        const float y = ReadValue<float>(physics, 0x1E4);
        const float z = ReadValue<float>(physics, 0x1E8);
        const float dx = x - (*at)[0];
        const float dy = y - (*at)[1];
        const float dz = z - (*at)[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance < meters) {
            close_by.emplace_back(
                distance,
                fmt::format("{:#x} hp {}/{} dead {:#x} hidden {} disabled {} at {:.1f} m "
                            "({:.1f}, {:.1f}, {:.1f}) backread {}/{} draw {:#x}",
                            ReadValue<u32>(character, 0x08), ReadValue<s32>(data, 0xF8),
                            ReadValue<s32>(data, 0xFC), ReadValue<u8>(character, 0x1E3),
                            ReadValue<u8>(character, 0x28), IsCharacterDisabled(character) ? 1 : 0,
                            distance, x, y, z, BackreadLevel(character, 0x10),
                            BackreadLevel(character, 0x14), DrawFlags(character)));
        }
    });
    std::ranges::sort(close_by);
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    LOG_INFO(Debug,
             "Bloodborne autoplay census: map {:#x}, {} characters within {:.0f} m of "
             "({:.1f}, {:.1f}, {:.1f})",
             GetCurrentPackedMap(), close_by.size(), meters, (*at)[0], (*at)[1], (*at)[2]);
    for (const auto& [distance, line] : close_by) {
        LOG_INFO(Debug, "Bloodborne autoplay census: {}", line);
    }
}

std::string AutoplayText(const AutoplayCommand& command) {
    std::string text = command.verb;
    for (const std::string& arg : command.args) {
        text += ' ' + arg;
    }
    return text;
}

// One script command's tick, as RunAutoplayStep hands it to the command's verb: its words, whether
// it ran before (begun), the seconds since it began, and where a failure goes.
struct AutoplayStep {
    const AutoplayCommand& command;
    bool begun;
    double elapsed;
    std::string& failure;

    std::string_view Arg(size_t index) const {
        return index < command.args.size() ? std::string_view{command.args[index]}
                                           : std::string_view{};
    }
    double Number(size_t index, double fallback) const {
        const std::string_view text = Arg(index);
        double value = fallback;
        if (!text.empty()) {
            std::from_chars(text.data(), text.data() + text.size(), value);
        }
        return value;
    }
    // Not done yet; failed once `limit` seconds have gone by.
    bool WaitedOut(double limit) const {
        if (elapsed > limit) {
            failure = fmt::format("timed out after {:.0f} s", elapsed);
        }
        return false;
    }
};

bool AutoplayWait(const AutoplayStep& step) {
    return step.elapsed >= step.Number(0, 1.0);
}

bool AutoplayWaitSession(const AutoplayStep& step) {
    return AutoplaySessionMembers() >= static_cast<size_t>(step.Number(0, 2)) ||
           step.WaitedOut(step.Number(1, 180));
}

// wait-solo [<timeout>]: until the session is gone (a member quit or crashed).
bool AutoplayWaitSolo(const AutoplayStep& step) {
    return AutoplaySessionMembers() <= 1 || step.WaitedOut(step.Number(0, 180));
}

bool AutoplayWaitMembers(const AutoplayStep& step) {
    return AutoplayMembersOnMyMap() >= static_cast<size_t>(step.Number(0, 2)) ||
           step.WaitedOut(step.Number(1, 180));
}

bool AutoplayWaitMap(const AutoplayStep& step) {
    const auto map = ParseAutoplayTarget(step.Arg(0));
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    return (map && AutoplayInWorld() && GetCurrentPackedMap() == *map) ||
           step.WaitedOut(step.Number(1, 180));
}

// wait-vital <up|down|dead> [<timeout>]: until this player's party death state is that.
bool AutoplayWaitVital(const AutoplayStep& step) {
    const auto vital = ParseVital(step.Arg(0));
    if (!vital) {
        step.failure = "no such vital";
        return false;
    }
    return party_death_run.downed.Current() == *vital || step.WaitedOut(step.Number(1, 60));
}

// wait-member-vital <vital> [<timeout>]: until another member's is, on the vitals board.
bool AutoplayWaitMemberVital(const AutoplayStep& step) {
    const auto vital = ParseVital(step.Arg(0));
    if (!vital) {
        step.failure = "no such vital";
        return false;
    }
    const VitalsView view = ReadVitalsBoard();
    return (view.Fresh() && std::ranges::any_of(view.board.members,
                                                [&](const MemberVitals& member) {
                                                    return member.member != view.member &&
                                                           member.vital == *vital;
                                                })) ||
           step.WaitedOut(step.Number(1, 60));
}

// travel <warp point> [<timeout>], and ensure-map's travel: the warp, then the player moved to
// the lamp once the map is in, then done once it stands there.
bool AutoplayTravelTo(const AutoplayStep& step, bool ensure) {
    const s32 point = static_cast<s32>(step.Number(ensure ? 1 : 0, HuntersDreamWarpPoint));
    if (!step.begun) {
        const u64 global = GetGlobalState();
        if (global < 0x10000) {
            step.failure = "no game state";
            return false;
        }
        // A guest's own trip (TickGuestLampTrip), at the lamp when it is known.
        const LampPoint* planned = StartTripToLamp(point);
        // As a guest's own lamp trip in a session does; out of one it is the game's own travel,
        // as a player's at a lamp.
        if (IsPartyGuestRole() && InPartySession()) {
            guest_trip_in_flight = true;
        }
        std::scoped_lock lock{autoplay_mutex};
        autoplay_run.travel_map = PackedMapOfWarpPoint(point);
        autoplay_run.travel_planned = planned;
        autoplay_run.travel_fixed_at.reset();
        return false;
    }
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    const bool arrived = step.elapsed > 5.0 && AutoplayInWorld() && !PartyTripActive() &&
                         GetCurrentPackedMap() == autoplay_run.travel_map;
    if (arrived && !autoplay_run.travel_fixed_at) {
        // The warp alone leaves the player at (0, 0, 0), above the void: to the lamp, as a
        // guest's trip does (guest_trip_fixup_at). The load put the save's last lamp back as
        // the respawn point, so the lamp travelled to goes in again first.
        if (const u64 global = GetGlobalState(); global >= 0x10000) {
            WriteValue(global, 0x1528, point);
        }
        const auto at = LocalPlayerPosition();
        const LampPoint* planned = nullptr;
        {
            std::scoped_lock lock{autoplay_mutex};
            planned = autoplay_run.travel_planned;
        }
        LOG_INFO(
            Debug, "Bloodborne autoplay: travel arrived at ({}, {}, {}) planned at the lamp={}",
            at ? (*at)[0] : 0.0f, at ? (*at)[1] : 0.0f, at ? (*at)[2] : 0.0f, planned != nullptr);
        if (ArrivalNeedsMove(planned, at)) {
            MoveToRespawnPoint(true);
        }
        std::scoped_lock lock{autoplay_mutex};
        autoplay_run.travel_fixed_at = step.elapsed;
        return false;
    }
    if (arrived && step.elapsed > *autoplay_run.travel_fixed_at + 2.0) {
        const auto at = LocalPlayerPosition();
        if (!at || (*at)[1] < -500.0f) {
            step.failure = "arrived in the void";
            return false;
        }
        return true;
    }
    return step.WaitedOut(step.Number(ensure ? 2 : 1, 180));
}

bool AutoplayTravel(const AutoplayStep& step) {
    return AutoplayTravelTo(step, false);
}

// ensure-map <map> <warp point>: travel there unless already on that map (a test's start).
bool AutoplayEnsureMap(const AutoplayStep& step) {
    if (!step.begun) {
        const auto map = ParseAutoplayTarget(step.Arg(0));
        if (!map) {
            step.failure = "no such map";
            return false;
        }
        // Already there, unless falling through the void (a save made after a bad warp).
        const auto at = LocalPlayerPosition();
        // Raw, as all of the rig's checks (see AutoplayInWorld).
        if (GetCurrentPackedMap() == *map && at && (*at)[1] > -500.0f) {
            return true;
        }
    }
    return AutoplayTravelTo(step, true);
}

// hurt <target> [<hp>], kill <target>.
bool AutoplayHurtOrKill(const AutoplayStep& step) {
    const std::string& verb = step.command.verb;
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found || !HasMemoryAccess(found->data, 0xFC, MemoryProt::CpuWrite)) {
        step.failure = "no such character";
        return false;
    }
    const s32 hp = ReadValue<s32>(found->data, 0xF8);
    const s32 target_hp = verb == "kill" ? 0 : std::min(hp, static_cast<s32>(step.Number(1, 1)));
    // A kill goes through the game's own (writing 0 HP leaves a standing corpse); checked
    // against its prologue first.
    static constexpr std::array<u8, 6> KillPrologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    if (verb == "kill" && IsCharacter(found->character, "autoplay kill") &&
        std::ranges::equal(
            std::span<const u8>{reinterpret_cast<const u8*>(image_base + KillCharacterOffset),
                                KillPrologue.size()},
            KillPrologue)) {
        using Kill = void PS4_SYSV_ABI (*)(u64 character);
        reinterpret_cast<Kill>(image_base + KillCharacterOffset)(found->character);
    } else {
        WriteValue(found->data, 0xF8, target_hp);
        // It stands for this player's hits (see own_damage), which the HP board shares.
        std::scoped_lock lock{own_hits_mutex};
        own_damage.Hit(ReadValue<u32>(found->character, 0x08), true, hp - target_hp);
    }
    LOG_INFO(Debug, "Bloodborne autoplay: {} {:#x} hp {} -> {} at {:.1f} m", verb,
             ReadValue<u32>(found->character, 0x08), hp, target_hp, found->distance);
    return true;
}

// mark <target>: the character later commands call "marked" (the nearest one now, say).
bool AutoplayMark(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    autoplay_marked.store(ReadValue<u32>(found->character, 0x08));
    LOG_INFO(Debug, "Bloodborne autoplay: marked {:#x} hp {} at {:.1f} m", autoplay_marked.load(),
             ReadValue<s32>(found->data, 0xF8), found->distance);
    return true;
}

// approach <target> [<dx> [<dz> [<heading>]]]: stand there, beside it, turned to <heading>.
bool AutoplayApproach(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    const u64 modules = found ? ReadValue<u64>(found->character, 0x3B0) : 0;
    const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (physics < 0x10000) {
        step.failure = "no such character";
        return false;
    }
    TeleportLocalPlayer({ReadValue<float>(physics, 0x1E0) + static_cast<float>(step.Number(1, 1.5)),
                         ReadValue<float>(physics, 0x1E4) + 0.2f,
                         ReadValue<float>(physics, 0x1E8) + static_cast<float>(step.Number(2, 0))},
                        static_cast<float>(step.Number(3, 0)));
    return true;
}

// attack and hit: the player put beside the target for this attack cycle (AttackStanceFor),
// once per cycle, and R1 held in the cycle's swing.
void AutoplayAttackCycle(const AutoplayCharacter& found, double elapsed, bool log_missed) {
    const u32 cycle = AttackCycleAt(elapsed);
    const u64 modules = ReadValue<u64>(found.character, 0x3B0);
    const u64 physics = modules >= 0x10000 ? ReadValue<u64>(modules, 0x68) : 0;
    if (autoplay_run.attack_placed != cycle && physics >= 0x10000) {
        if (log_missed && autoplay_run.attack_placed) {
            LOG_INFO(Debug, "Bloodborne autoplay: attack cycle {} missed (heading {:.2f})",
                     *autoplay_run.attack_placed, LocalPlayerHeading().value_or(0.0f));
        }
        const float target_x = ReadValue<float>(physics, 0x1E0);
        const float target_z = ReadValue<float>(physics, 0x1E8);
        const AttackStance stance = AttackStanceFor(target_x, target_z, cycle);
        TeleportLocalPlayer({stance.x, ReadValue<float>(physics, 0x1E4) + 0.2f, stance.z},
                            HeadingTowards(stance.x, stance.z, target_x, target_z));
        autoplay_run.attack_placed = cycle;
    }
    autoplay_buttons.store(AttackHoldsR1(elapsed) ? AutoplayButton("r1") : 0);
}

// attack <target> [<timeout>]: one HP left, then light attacks from each side in turn until
// it dies (AttackStanceFor): a real hit, so the kill reward and the death run as in play.
bool AutoplayAttack(const AutoplayStep& step) {
    if (!step.begun) {
        const auto target = ParseAutoplayTarget(step.Arg(0));
        const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
        if (!found || !HasMemoryAccess(found->data, 0xFC, MemoryProt::CpuWrite)) {
            step.failure = "no such character";
            return false;
        }
        const s32 hp = ReadValue<s32>(found->data, 0xF8);
        WriteValue(found->data, 0xF8, std::min(hp, s32{1}));
        autoplay_run.attack_handle = ReadValue<u32>(found->character, 0x08);
        autoplay_run.attack_placed.reset();
        LOG_INFO(Debug, "Bloodborne autoplay: attacking {:#x} (hp {} -> 1) at {:.1f} m",
                 autoplay_run.attack_handle, hp, found->distance);
    }
    // By its handle from here on: "nearest" would be the next enemy once this one is dead.
    const auto found = FindAutoplayCharacter(autoplay_run.attack_handle);
    const s32 hp = found ? ReadValue<s32>(found->data, 0xF8) : 0;
    if (hp <= 0) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: {:#x} {} in attack cycle {}",
                 autoplay_run.attack_handle, found ? "died" : "is gone",
                 autoplay_run.attack_placed.value_or(0));
        return true;
    }
    AutoplayAttackCycle(*found, step.elapsed, true);
    if (step.elapsed > step.Number(1, 40)) {
        step.failure = fmt::format("still alive after {:.0f} s: hp {}", step.elapsed, hp);
    }
    return false;
}

// hit <target> [<timeout>]: light attacks from each side in turn (as attack, without taking
// its HP down first) until one lands: a real hit that the game sends to the other players.
bool AutoplayHit(const AutoplayStep& step) {
    if (!step.begun) {
        const auto target = ParseAutoplayTarget(step.Arg(0));
        const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
        if (!found) {
            step.failure = "no such character";
            return false;
        }
        autoplay_run.attack_handle = ReadValue<u32>(found->character, 0x08);
        autoplay_run.attack_placed.reset();
        autoplay_run.hit_from_hp = ReadValue<s32>(found->data, 0xF8);
        LOG_INFO(Debug, "Bloodborne autoplay: hitting {:#x} (hp {}) at {:.1f} m",
                 autoplay_run.attack_handle, autoplay_run.hit_from_hp, found->distance);
    }
    const auto found = FindAutoplayCharacter(autoplay_run.attack_handle);
    if (!found) {
        autoplay_buttons.store(0);
        step.failure = "the target is gone";
        return false;
    }
    const s32 hp = ReadValue<s32>(found->data, 0xF8);
    if (hp < autoplay_run.hit_from_hp) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: hit {:#x}: hp {} -> {} in attack cycle {}",
                 autoplay_run.attack_handle, autoplay_run.hit_from_hp, hp,
                 autoplay_run.attack_placed.value_or(0));
        return true;
    }
    AutoplayAttackCycle(*found, step.elapsed, false);
    if (step.elapsed > step.Number(1, 30)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format("no hit landed in {:.0f} s: hp {}", step.elapsed, hp);
    }
    return false;
}

bool AutoplayTeleport(const AutoplayStep& step) {
    TeleportLocalPlayer({static_cast<float>(step.Number(0, 0)),
                         static_cast<float>(step.Number(1, 0)),
                         static_cast<float>(step.Number(2, 0))},
                        static_cast<float>(step.Number(3, 0)));
    return true;
}

// glide <x y z>...: from where the player is along those points at walking pace, by small
// teleports facing the way it goes (a far teleport drops it through the floor): a path a "map
// change" log line gave (its trail), to cross a border between two maps.
bool AutoplayGlide(const AutoplayStep& step) {
    const auto& args = step.command.args;
    if (args.empty() || args.size() % 3 != 0) {
        step.failure = "glide takes x y z points";
        return false;
    }
    if (!step.begun) {
        const auto at = LocalPlayerPosition();
        if (!at) {
            step.failure = "no player";
            return false;
        }
        std::scoped_lock lock{autoplay_mutex};
        autoplay_run.glide_from = *at;
    }
    std::vector<LampPosition> path;
    {
        std::scoped_lock lock{autoplay_mutex};
        path.push_back(autoplay_run.glide_from);
    }
    for (size_t at = 0; at < args.size(); at += 3) {
        path.push_back({static_cast<float>(step.Number(at, 0)),
                        static_cast<float>(step.Number(at + 1, 0)),
                        static_cast<float>(step.Number(at + 2, 0))});
    }
    const auto point = GlidePoint(path, step.elapsed);
    const LampPosition to = point.value_or(path.back());
    const LampPosition ahead = GlidePoint(path, step.elapsed + 0.25).value_or(path.back());
    const float dx = ahead[0] - to[0];
    const float dz = ahead[2] - to[2];
    // A heading h faces (-sin h, cos h).
    TeleportLocalPlayer(to, dx == 0.0f && dz == 0.0f ? 0.0f : std::atan2(-dx, dz));
    return !point.has_value();
}

// pickup [<meters>] [<timeout>]: onto the nearest usable map item within reach (30 m), then
// X for 0.3 s of every 1.5 s until the item leaves the usable list: a real pickup through the
// item button (the host could not pick up items in the Forbidden Woods, 2026-10-09).
bool AutoplayPickup(const AutoplayStep& step) {
    const auto position = MapItemSearchPlayerPosition();
    const auto items =
        position ? UsableMapItemsNear(*position) : std::vector<std::pair<float, MapItemSpot>>{};
    if (!step.begun) {
        if (items.empty() || items.front().first > step.Number(0, 30)) {
            step.failure = "no usable map item near";
            return false;
        }
        const MapItemSpot& spot = items.front().second;
        autoplay_run.pickup_handle = spot.handle;
        TeleportLocalPlayer({spot.at[0], spot.at[1] + 0.2f, spot.at[2]});
        LOG_INFO(Debug, "Bloodborne autoplay: picking up map item {:#x} {:.1f} m away", spot.handle,
                 items.front().first);
    }
    const bool there = std::ranges::any_of(
        items, [](const auto& item) { return item.second.handle == autoplay_run.pickup_handle; });
    if (!there) {
        autoplay_buttons.store(0);
        LOG_INFO(Debug, "Bloodborne autoplay: map item {:#x} picked up after {:.1f} s",
                 autoplay_run.pickup_handle, step.elapsed);
        return true;
    }
    autoplay_buttons.store(
        step.elapsed > 1.0 && std::fmod(step.elapsed, 1.5) < 0.3 ? AutoplayButton("cross") : 0);
    if (step.elapsed > step.Number(1, 20)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format(
            "map item {:#x} still there after {:.0f} s (item button found {} not busy {} "
            "pressed {})",
            autoplay_run.pickup_handle, step.elapsed, item_button_counts[1].load(),
            item_button_counts[2].load(), item_button_counts[3].load());
    }
    return false;
}

// rise <metres>: straight up from where the player stands, to fall back down.
bool AutoplayRise(const AutoplayStep& step) {
    const auto at = LocalPlayerPosition();
    if (!at) {
        step.failure = "not in the world";
        return false;
    }
    TeleportLocalPlayer({(*at)[0], (*at)[1] + static_cast<float>(step.Number(0, 8)), (*at)[2]},
                        LocalPlayerHeading().value_or(0.0f));
    return true;
}

// approach-downed: a metre from the nearest downed party member on this map, where the
// vitals board has it.
bool AutoplayApproachDowned(const AutoplayStep& step) {
    const VitalsView view = ReadVitalsBoard();
    const auto me = LocalPlayerPosition();
    const auto distance = [&](const LampPosition& at) {
        return std::hypot(at[0] - (*me)[0], at[1] - (*me)[1], at[2] - (*me)[2]);
    };
    std::optional<LampPosition> nearest;
    for (const MemberVitals& member : view.board.members) {
        if (me && view.Fresh() && member.member != view.member && member.vital == Vital::Down &&
            member.map == PlayingMap() && (!nearest || distance(member.at) < distance(*nearest))) {
            nearest = member.at;
        }
    }
    if (!nearest) {
        step.failure = "nobody down on this map";
        return false;
    }
    TeleportLocalPlayer({(*nearest)[0] + 1.0f, (*nearest)[1] + 0.2f, (*nearest)[2]});
    return true;
}

// revive-downed [<timeout>]: strikes the nearest downed party member on this map until nobody
// is down (how many hits depends on the weapon): from each side in turn, turned to it, as
// `attack` does (AttackStanceFor), since only a swing that lands counts for its revive.
bool AutoplayReviveDowned(const AutoplayStep& step) {
    if (!step.begun) {
        autoplay_run.attack_placed.reset();
    }
    const VitalsView view = ReadVitalsBoard();
    const auto me = LocalPlayerPosition();
    std::optional<LampPosition> downed;
    for (const MemberVitals& member : view.board.members) {
        if (view.Fresh() && member.member != view.member && member.vital == Vital::Down &&
            member.map == PlayingMap()) {
            downed = member.at;
        }
    }
    if (!downed || !me) {
        autoplay_buttons.store(0);
        if (!step.begun) {
            step.failure = "nobody down on this map";
            return false;
        }
        LOG_INFO(Debug, "Bloodborne autoplay: nobody down after {:.1f} s of revive hits",
                 step.elapsed);
        return true;
    }
    // The body itself (the other player nearest to where the board has it), not the board's
    // position, which lags: swings aimed at it missed (rig 2026-10-12, one hit in 7 s).
    float nearest = 4.0f;
    for (const OtherPlayer& other : OtherPlayers()) {
        if (other.at) {
            const float d =
                std::hypot((*other.at)[0] - (*downed)[0], (*other.at)[2] - (*downed)[2]);
            if (d < nearest) {
                nearest = d;
                downed = *other.at;
            }
        }
    }
    const u32 cycle = AttackCycleAt(step.elapsed);
    if (autoplay_run.attack_placed != cycle) {
        const AttackStance stance = AttackStanceFor((*downed)[0], (*downed)[2], cycle);
        TeleportLocalPlayer({stance.x, (*downed)[1] + 0.2f, stance.z},
                            HeadingTowards(stance.x, stance.z, (*downed)[0], (*downed)[2]));
        autoplay_run.attack_placed = cycle;
    }
    autoplay_buttons.store(AttackHoldsR1(step.elapsed) ? AutoplayButton("r1") : 0);
    if (step.elapsed > step.Number(0, 60)) {
        autoplay_buttons.store(0);
        step.failure = fmt::format("still down after {:.0f} s of revive hits", step.elapsed);
    }
    return false;
}

// press <button> [<ms>], stick <x> <y> [<ms>]: held for that long (150 ms).
bool AutoplayPressOrStick(const AutoplayStep& step) {
    const bool press = step.command.verb == "press";
    u32 buttons = 0;
    u32 sticks = 0x80808080;
    if (press) {
        buttons = AutoplayButton(step.Arg(0));
        if (buttons == 0) {
            step.failure = "no such button";
            return false;
        }
    } else {
        const u32 x = static_cast<u32>(std::clamp(step.Number(0, 128), 0.0, 255.0));
        const u32 y = static_cast<u32>(std::clamp(step.Number(1, 128), 0.0, 255.0));
        sticks = x | (y << 8) | 0x80800000;
    }
    autoplay_buttons.store(buttons);
    autoplay_sticks.store(sticks);
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.hold_until =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds{static_cast<s64>(step.Number(press ? 1 : 2, 150))};
    return false;
}

bool AutoplaySetFlag(const AutoplayStep& step) {
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(
        0, static_cast<s32>(step.Number(0, 0)), step.Number(1, 1) != 0);
    return true;
}

// expect-flag <flag> [<0|1>], wait-flag <flag> [<0|1>] [<timeout>].
bool AutoplayExpectOrWaitFlag(const AutoplayStep& step) {
    const bool on = EventFlagOn(static_cast<s32>(step.Number(0, 0)));
    if (on == (step.Number(1, 1) != 0)) {
        return true;
    }
    if (step.command.verb == "expect-flag") {
        step.failure = fmt::format("flag is {}", on ? 1 : 0);
        return false;
    }
    return step.WaitedOut(step.Number(2, 180));
}

// godmode [off]: the player's HP back to its max on every party tick (tests that stand
// around near enemies), until switched off.
bool AutoplayGodmode(const AutoplayStep& step) {
    autoplay_godmode.store(step.Arg(0) != "off");
    return true;
}

bool AutoplaySnapshot(const AutoplayStep&) {
    LogAutoplaySnapshot();
    return true;
}

// chapter <name>: a rig suite's next section; the rig judges each one on its own lines.
bool AutoplayChapter(const AutoplayStep& step) {
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.chapter = std::string{step.Arg(0)};
    LOG_INFO(Debug, "Bloodborne autoplay: chapter {}", step.Arg(0));
    return true;
}

// keep-going <on|off>: on, a failed step gives up its chapter and the script goes on at the next
// one (NextAutoplayChapter); off (a chapter the rest needs, as the join), it ends there.
bool AutoplayKeepGoing(const AutoplayStep& step) {
    std::scoped_lock lock{autoplay_mutex};
    autoplay_run.keep_going = step.Arg(0) == "on";
    return true;
}

// sync <name> [<timeout>]: wait until every game of the suite got here: the rig sees each one's
// "reached" line and writes the go file (AutoplaySyncPath) for all of them at once.
bool AutoplaySync(const AutoplayStep& step) {
    std::string script;
    {
        std::scoped_lock lock{autoplay_mutex};
        script = autoplay_run.script_path;
    }
    const auto path = AutoplaySyncPath(script, step.Arg(0));
    if (!path) {
        step.failure = "no such sync name";
        return false;
    }
    if (!step.begun) {
        LOG_INFO(Debug, "Bloodborne autoplay: sync {} reached (t={:.1f})", step.Arg(0),
                 SteadyMilliseconds() / 1000.0);
    }
    std::error_code error;
    return std::filesystem::exists(*path, error) || step.WaitedOut(step.Number(1, 600));
}

bool AutoplayCensus(const AutoplayStep& step) {
    LogAutoplayCensus(static_cast<float>(step.Number(0, 60)));
    return true;
}

// shot <name> [<timeout>]: a picture of the port's next frame (AutoplayShotPath), once its
// presenter took the request. The emulator has no request file: skipped. A picture not taken
// in time (10 s) is noted and the script goes on: it shows the HUD, it tests nothing.
bool AutoplayShot(const AutoplayStep& step) {
    static const char* request = std::getenv("BB_SHOT_TRIGGER");
    if (request == nullptr) {
        LOG_INFO(Debug, "Bloodborne autoplay: shot {} skipped (nothing takes pictures)",
                 step.Arg(0));
        return true;
    }
    if (!step.begun) {
        const auto path = AutoplayShotPath(request, step.Arg(0));
        if (!path) {
            step.failure = "no such shot name";
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path{*path}.parent_path(), error);
        // Written whole, then moved into place: the presenter never reads half a path.
        const std::string partial = std::string{request} + ".part";
        std::ofstream{partial, std::ios::binary | std::ios::trunc} << *path;
        std::filesystem::rename(partial, request, error);
        if (error) {
            step.failure = fmt::format("no shot request: {}", error.message());
            return false;
        }
        LOG_INFO(Debug, "Bloodborne autoplay: shot {} asked for", *path);
    }
    std::error_code error;
    if (!std::filesystem::exists(request, error)) {
        return true;
    }
    if (step.elapsed > step.Number(1, 10)) {
        LOG_WARNING(Debug, "Bloodborne autoplay: shot {} not taken in {:.0f} s", step.Arg(0),
                    step.elapsed);
        return true;
    }
    return false;
}

bool AutoplayLog(const AutoplayStep& step) {
    std::string text;
    for (const std::string& word : step.command.args) {
        text += (text.empty() ? "" : " ") + word;
    }
    LOG_INFO(Debug, "Bloodborne autoplay log: {}", text);
    return true;
}

bool AutoplayExpectHp(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const s32 hp = ReadValue<s32>(found->data, 0xF8);
    const auto holds = CompareAutoplay(hp, step.Arg(1), static_cast<s64>(step.Number(2, 0)));
    if (!holds || !*holds) {
        step.failure = fmt::format("hp is {}", hp);
        return false;
    }
    return true;
}

// dump <target>: a character's first 0x800 bytes, its control block (+0x18) and its module
// pointers (+0x3B0) with each module's first 0x100 bytes, logged in hex, to diff one character
// between the host's and a guest's world (Iosefka shown on a guest with the event hide byte
// set, 2026-10-10).
bool AutoplayDump(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found || !HasMemoryAccess(found->character, 0x400, MemoryProt::CpuRead)) {
        step.failure = "no such character";
        return false;
    }
    const u32 handle = ReadValue<u32>(found->character, 0x08);
    const auto dump = [&](const char* what, u64 base, u64 size) {
        if (base < 0x10000 || !HasMemoryAccess(base, size, MemoryProt::CpuRead)) {
            return;
        }
        for (u64 offset = 0; offset < size; offset += 0x20) {
            std::string row;
            for (u64 byte = 0; byte < 0x20; ++byte) {
                row += fmt::format("{:02x}", ReadValue<u8>(base, offset + byte));
            }
            LOG_INFO(Debug, "Bloodborne autoplay dump {:#x} {} +{:03x}: {}", handle, what, offset,
                     row);
        }
    };
    dump("chr", found->character, 0x800);
    dump("ctrl", ReadValue<u64>(found->character, 0x18), 0x100);
    // The model (draw flags at +0x114): how a phantom is drawn.
    dump("model", ReadValue<u64>(found->character, 0x58), 0x400);
    const u64 modules = ReadValue<u64>(found->character, 0x3B0);
    dump("modules", modules, 0x100);
    // Each module's first bytes: what draws the character is in one of them.
    for (u64 index = 0; modules >= 0x10000 && index < 0x20; ++index) {
        dump(fmt::format("module{:02x}", index).c_str(), ReadValue<u64>(modules, index * 8), 0x100);
    }
    return true;
}

// raise-stat <index> <amount>: one of the player's stats (PlayerGameData +0x40 + 8 * index,
// what channel 8 sends) raised, as a level up does (see OwnDataWatch).
bool AutoplayRaiseStat(const AutoplayStep& step) {
    const u64 mine = OwnGameData();
    const u64 at = 0x40 + 8 * static_cast<u64>(step.Number(0, 0));
    if (mine < 0x10000 || at > 0x90 || !HasMemoryAccess(mine + at, 4, MemoryProt::CpuWrite)) {
        step.failure = "no player data";
        return false;
    }
    const s32 before = ReadValue<s32>(mine, at);
    WriteValue(mine, at, static_cast<s32>(before + step.Number(1, 1)));
    LOG_INFO(Debug, "Bloodborne autoplay raise-stat: +{:#x} {} -> {}", at, before,
             ReadValue<s32>(mine, at));
    return true;
}

// peek <target> <offset>, poke <target> <offset> <value> [1] (hex): a character's u32 (or byte)
// at an offset, logged or written (reverse engineering on the rig: a field's meaning tried in
// play).
bool AutoplayPeekOrPoke(const AutoplayStep& step) {
    const std::string& verb = step.command.verb;
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    u32 offset = 0;
    u32 value = 0;
    const std::string_view offset_text = step.Arg(1);
    const std::string_view value_text = step.Arg(2);
    std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(), offset, 16);
    std::from_chars(value_text.data(), value_text.data() + value_text.size(), value, 16);
    if (!found || offset > 0x2000 ||
        !HasMemoryAccess(found->character + offset, 4, MemoryProt::CpuRead)) {
        step.failure = "no such character";
        return false;
    }
    const u32 before = ReadValue<u32>(found->character, offset);
    if (verb == "poke" && step.Arg(3) == "1") {
        WriteValue(found->character, offset, static_cast<u8>(value)); // one byte
    } else if (verb == "poke") {
        WriteValue(found->character, offset, value);
    }
    LOG_INFO(Debug, "Bloodborne autoplay {}: {:#x} +{:#x} = {:#x}{}", verb,
             ReadValue<u32>(found->character, 0x08), offset, before,
             verb == "poke" ? fmt::format(" -> {:#x}", value) : "");
    return true;
}

// expect-hidden <handle> <0|1> [<timeout>]: the character hidden by the story (its event
// disable byte, +0x28) or not, within the time given (default: now).
bool AutoplayExpectHidden(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const bool hidden = ReadValue<u8>(found->character, 0x28) != 0;
    if (hidden == (step.Number(1, 1) != 0)) {
        return true;
    }
    if (step.elapsed >= step.Number(2, 0)) {
        step.failure = fmt::format("hidden is {}", hidden ? 1 : 0);
    }
    return false;
}

// expect-disabled <handle> <0|1> [<timeout>]: the character's enable switch off (2004:5,
// IsCharacterDisabled) or on, within the time given (default: now).
bool AutoplayExpectDisabled(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const bool disabled = IsCharacterDisabled(found->character);
    if (disabled == (step.Number(1, 1) != 0)) {
        return true;
    }
    if (step.elapsed >= step.Number(2, 0)) {
        step.failure = fmt::format("disabled is {}", disabled ? 1 : 0);
    }
    return false;
}

// note-max-hp: the player's max HP now, for a later expect-max-hp self == noted.
bool AutoplayNoteMaxHp(const AutoplayStep& step) {
    const auto self = FindAutoplayCharacter(AutoplaySelf);
    if (!self) {
        step.failure = "no player";
        return false;
    }
    autoplay_run.noted_max_hp = ReadValue<s32>(self->data, 0xFC);
    LOG_INFO(Debug, "Bloodborne autoplay: max hp {}", autoplay_run.noted_max_hp);
    return true;
}

// expect-max-hp <target> <op> <value|noted>: its max HP (a party guest's is not cut to 70%).
bool AutoplayExpectMaxHp(const AutoplayStep& step) {
    const auto target = ParseAutoplayTarget(step.Arg(0));
    const auto found = target ? FindAutoplayCharacter(*target) : std::nullopt;
    if (!found) {
        step.failure = "no such character";
        return false;
    }
    const s32 max = ReadValue<s32>(found->data, 0xFC);
    const s64 limit =
        step.Arg(2) == "noted" ? autoplay_run.noted_max_hp : static_cast<s64>(step.Number(2, 0));
    const auto holds = CompareAutoplay(max, step.Arg(1), limit);
    if (!holds || !*holds) {
        step.failure = fmt::format("max hp is {}", max);
        return false;
    }
    return true;
}

// expect-effect <id> <0|1>: whether the player has that SpEffect (9200: the level sync).
bool AutoplayExpectEffect(const AutoplayStep& step) {
    const auto effects = LocalEffects();
    const bool has =
        std::ranges::find(effects, static_cast<s32>(step.Number(0, 0))) != effects.end();
    if (has != (step.Number(1, 1) != 0)) {
        step.failure = fmt::format("effect {} is {}", step.Arg(0), has ? "on" : "off");
        return false;
    }
    return true;
}

bool AutoplayExpectSession(const AutoplayStep& step) {
    const size_t members = AutoplaySessionMembers();
    if (members != static_cast<size_t>(step.Number(0, 1))) {
        step.failure = fmt::format("{} in the session", members);
        return false;
    }
    return true;
}

// expect-others-near <n> <meters>: at least n other players stand alive within <meters> of
// the player (a guest that got up invisible to the host: its copy dead, or left where it died).
bool AutoplayExpectOthersNear(const AutoplayStep& step) {
    const auto here = LocalPlayerPosition();
    const double limit = step.Number(1, 10);
    size_t close_by = 0;
    std::string seen;
    for (const OtherPlayer& other : OtherPlayers()) {
        const double distance =
            here && other.at ? std::hypot((*other.at)[0] - (*here)[0], (*other.at)[1] - (*here)[1],
                                          (*other.at)[2] - (*here)[2])
                             : -1.0;
        seen += fmt::format(" [type={} hp={} dead={:#x} at {:.1f} m]", other.type, other.hp,
                            other.dead_flags, distance);
        close_by +=
            other.hp > 0 && (other.dead_flags & 0x05) == 0 && distance >= 0.0 && distance <= limit;
    }
    if (close_by < static_cast<size_t>(step.Number(0, 1))) {
        step.failure =
            fmt::format("{} alive within {} m:{}", close_by, limit, seen.empty() ? " none" : seen);
        return false;
    }
    return true;
}

bool AutoplayExpectMap(const AutoplayStep& step) {
    const auto map = ParseAutoplayTarget(step.Arg(0));
    // Raw, as all of the rig's checks (see AutoplayInWorld).
    if (!map || GetCurrentPackedMap() != *map) {
        step.failure = fmt::format("on map {:#x}", GetCurrentPackedMap());
        return false;
    }
    return true;
}

bool AutoplayDone(const AutoplayStep&) {
    return true;
}

// The script's verbs (tools/rig/tests/test_rig.py keeps the same list for the scenarios).
using AutoplayVerb = bool (*)(const AutoplayStep& step);
constexpr auto AutoplayVerbs = std::to_array<std::pair<std::string_view, AutoplayVerb>>({
    {"wait", AutoplayWait},
    {"wait-session", AutoplayWaitSession},
    {"wait-solo", AutoplayWaitSolo},
    {"wait-members", AutoplayWaitMembers},
    {"wait-map", AutoplayWaitMap},
    {"wait-vital", AutoplayWaitVital},
    {"wait-member-vital", AutoplayWaitMemberVital},
    {"ensure-map", AutoplayEnsureMap},
    {"travel", AutoplayTravel},
    {"hurt", AutoplayHurtOrKill},
    {"kill", AutoplayHurtOrKill},
    {"mark", AutoplayMark},
    {"approach", AutoplayApproach},
    {"attack", AutoplayAttack},
    {"hit", AutoplayHit},
    {"teleport", AutoplayTeleport},
    {"glide", AutoplayGlide},
    {"pickup", AutoplayPickup},
    {"rise", AutoplayRise},
    {"approach-downed", AutoplayApproachDowned},
    {"revive-downed", AutoplayReviveDowned},
    {"press", AutoplayPressOrStick},
    {"stick", AutoplayPressOrStick},
    {"set-flag", AutoplaySetFlag},
    {"expect-flag", AutoplayExpectOrWaitFlag},
    {"wait-flag", AutoplayExpectOrWaitFlag},
    {"godmode", AutoplayGodmode},
    {"snapshot", AutoplaySnapshot},
    {"census", AutoplayCensus},
    {"shot", AutoplayShot},
    {"log", AutoplayLog},
    {"expect-hp", AutoplayExpectHp},
    {"dump", AutoplayDump},
    {"raise-stat", AutoplayRaiseStat},
    {"peek", AutoplayPeekOrPoke},
    {"poke", AutoplayPeekOrPoke},
    {"expect-hidden", AutoplayExpectHidden},
    {"expect-disabled", AutoplayExpectDisabled},
    {"note-max-hp", AutoplayNoteMaxHp},
    {"expect-max-hp", AutoplayExpectMaxHp},
    {"expect-effect", AutoplayExpectEffect},
    {"expect-session", AutoplayExpectSession},
    {"expect-others-near", AutoplayExpectOthersNear},
    {"expect-map", AutoplayExpectMap},
    {"chapter", AutoplayChapter},
    {"keep-going", AutoplayKeepGoing},
    {"sync", AutoplaySync},
    {"done", AutoplayDone},
});

// Runs one script command for this tick: true once it is done, `failure` set if it failed.
bool RunAutoplayStep(const AutoplayCommand& command, bool begun, double elapsed,
                     std::string& failure) {
    const AutoplayStep step{command, begun, elapsed, failure};
    for (const auto& [verb, run] : AutoplayVerbs) {
        if (command.verb == verb) {
            return run(step);
        }
    }
    failure = "unknown command";
    return false;
}

} // namespace

namespace Coop {

// From the party tick (the game thread), once the player is in the world.
void TickAutoplay() {
    if (autoplay_godmode.load()) {
        if (const auto self = FindAutoplayCharacter(AutoplaySelf);
            self && ReadValue<s32>(self->data, 0xF8) > 0 &&
            HasMemoryAccess(self->data, 0x100, MemoryProt::CpuWrite)) {
            WriteValue(self->data, 0xF8, ReadValue<s32>(self->data, 0xFC));
        }
    }
    AutoplayCommand command;
    size_t index = 0;
    size_t total = 0;
    bool begun = false;
    double elapsed = 0.0;
    const auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock{autoplay_mutex};
        if (autoplay_run.phase != AutoplayRun::Phase::World) {
            return;
        }
        if (autoplay_run.hold_until) {
            if (now < *autoplay_run.hold_until) {
                return;
            }
            autoplay_run.hold_until.reset();
            autoplay_buttons.store(0);
            autoplay_sticks.store(0x80808080);
            LOG_INFO(Debug, "Bloodborne autoplay: step {}/{} {} ok", autoplay_run.next + 1,
                     autoplay_run.script.size(),
                     AutoplayText(autoplay_run.script[autoplay_run.next]));
            ++autoplay_run.next;
            autoplay_run.step_started = now;
            autoplay_run.step_begun = false;
            return;
        }
        if (autoplay_run.next >= autoplay_run.script.size() ||
            autoplay_run.script[autoplay_run.next].verb == "done") {
            autoplay_run.phase = AutoplayRun::Phase::Done;
            LOG_INFO(Debug, "Bloodborne autoplay: done ({})",
                     autoplay_run.failed ? "failed" : "ok");
            return;
        }
        command = autoplay_run.script[autoplay_run.next];
        index = autoplay_run.next;
        total = autoplay_run.script.size();
        begun = autoplay_run.step_begun;
        autoplay_run.step_begun = true;
        elapsed = std::chrono::duration<double>(now - autoplay_run.step_started).count();
    }
    std::string failure;
    const bool finished = RunAutoplayStep(command, begun, elapsed, failure);
    std::scoped_lock lock{autoplay_mutex};
    if (!failure.empty()) {
        LOG_ERROR(Debug, "Bloodborne autoplay: step {}/{} {} FAILED: {}", index + 1, total,
                  AutoplayText(command), failure);
        autoplay_buttons.store(0);
        autoplay_sticks.store(0x80808080);
        if (autoplay_run.keep_going) {
            if (const auto next = NextAutoplayChapter(autoplay_run.script, index)) {
                // The rig lets the others past this chapter's syncs (they would wait in vain).
                LOG_INFO(Debug, "Bloodborne autoplay: chapter {} given up", autoplay_run.chapter);
                autoplay_run.failed = true;
                autoplay_run.next = *next;
                autoplay_run.step_started = std::chrono::steady_clock::now();
                autoplay_run.step_begun = false;
                return;
            }
        }
        LOG_INFO(Debug, "Bloodborne autoplay: done (failed)");
        autoplay_run.phase = AutoplayRun::Phase::Done;
        return;
    }
    if (finished && !autoplay_run.hold_until) {
        LOG_INFO(Debug, "Bloodborne autoplay: step {}/{} {} ok (t={:.1f})", index + 1, total,
                 AutoplayText(command), SteadyMilliseconds() / 1000.0);
        ++autoplay_run.next;
        autoplay_run.step_started = std::chrono::steady_clock::now();
        autoplay_run.step_begun = false;
    }
}

} // namespace Coop

namespace {

// From the pad reader: on the title screen, Cross now and then until the player is in the world.
void TickAutoplayTitle() {
    std::scoped_lock lock{autoplay_mutex};
    if (autoplay_run.phase != AutoplayRun::Phase::Title) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (AutoplayInWorld()) {
        autoplay_run.phase = AutoplayRun::Phase::World;
        autoplay_run.step_started = now;
        autoplay_run.step_begun = false;
        autoplay_buttons.store(0);
        // Raw, as all of the rig's checks (see AutoplayInWorld).
        LOG_INFO(Debug, "Bloodborne autoplay: in the world on map {:#x}", GetCurrentPackedMap());
        return;
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - autoplay_run.started);
    autoplay_buttons.store(TitlePressesCross(static_cast<u64>(ms.count())) ? AutoplayButton("cross")
                                                                           : 0);
}

} // namespace

bool AutoplayPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks) {
    MakeEnemiesHarder();
    std::call_once(autoplay_loaded, LoadAutoplay);
    if (autoplay_run.phase == AutoplayRun::Phase::Off) {
        return false;
    }
    TickAutoplayTitle();
    buttons = autoplay_buttons.load();
    const u32 packed = autoplay_sticks.load();
    sticks = {static_cast<u8>(packed), static_cast<u8>(packed >> 8), static_cast<u8>(packed >> 16),
              static_cast<u8>(packed >> 24)};
    return true;
}

} // namespace Core::Bloodborne
