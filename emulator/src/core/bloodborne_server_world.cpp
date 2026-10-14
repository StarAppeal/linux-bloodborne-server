// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The party's world on the server (docs/SERVER_COOP_PLAN.md phase 3): in the port (always, over
// the server link; never in shadPS4) shadNet keeps the party's world log, not the host's game.
// Every member, the host too, proposes to it and follows its log as a WorldLogMember
// (server/coop/world_log.h): the kills (3a; the kill board is not used then), the host's
// characters, map state and return point, every member's shared flags and HP reports (3b; the
// host's own log is not run then), and what the HTTP boards carried: maps, rests, trophies, kill
// echoes, vitals and portraits (3c; the boards are not used then).
//
// The messages (server/coop/coop.proto) go in the server link's Coop datagrams, in their reliable
// stream (server/coop/coop_stream.h). A thread of its own runs the stream (receives, resends,
// acks) every StreamTickMs; the game's thread only queues messages and takes what came (the kill
// step). No hook on the game's per-frame network update (phase 2: a hook there stalled joins).

#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_trade.h"

#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>

#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "coop/coop_messages.h"
#include "coop/coop_stream.h"
#include "core/bloodborne_team.h"
#include "core/libraries/network/server_link.h"

namespace Core::Bloodborne {
namespace {

constexpr s64 StreamTickMs = 20;
// A member's standing kills and the host's reports go again this often while they stay the same (a
// restart of the server drops what was in flight; the same again changes nothing there).
constexpr s64 KillsResendMs = 2'000;
constexpr s64 ReportResendMs = 5'000;

// The server's world, phase 4's enemy owners (see EnemyRole) and phase 5's teams: the port's only
// way since phase 6 (no switches); shadPS4 keeps the old path (docs/SERVER_COOP_PLAN.md).
#ifdef BLOODBORNE_NATIVE_PORT
constexpr bool server_world_switch = true;
#else
constexpr bool server_world_switch = false;
#endif
constexpr bool enemy_owner_switch = server_world_switch;

// The server's enemy owners as last heard (the stream's thread writes, any thread reads).
std::mutex owners_mutex;
std::vector<EnemyOwnerEntry> server_owners;
s64 owners_heard_ms = 0;

const u32 run = [] {
    std::random_device random;
    return static_cast<u32>(random()) | 1u;
}();

// The stream and its queues: the world thread and the game thread.
std::mutex stream_mutex;
std::optional<CoopStream> stream;
std::deque<bbcoop::Envelope> inbox;
std::deque<bbcoop::Envelope> outbox;
// Something for the kill step came (see TakeServerWorldNews).
std::atomic<bool> inbox_news{false};
std::string hello_party;
std::string hello_member;
bool hello_due = true;
std::once_flag thread_started;

// Phase 5: the team lobby (bloodborne_team.h). The lobby's messages go on the same stream, before
// any party hello; the party is the team's from its Start.
constexpr bool team_switch = server_world_switch;
std::mutex team_mutex;
TeamGate team_gate;
std::string team_party;      // the team's party key once its Start let this game's load go
std::string lobby_member;    // this player in the lobby (its board name), once known
std::string lobby_name;      // its online id
bool lobby_playing = false;  // in a world (the load went)
bool lobby_hello_due = true; // stream restarts and changes of lobby_playing
std::deque<bbcoop::Envelope> lobby_outbox;
// Phase 5b: the team's world as the server last sent it (none before), and what this game does
// with it: a save playing in a team takes it as its loads bring each flag block (team_world_due;
// a block once, team_world_blocks: the area's own changes stay), the team's host reports its own
// once its save took the team's (team_host, team_world_taken), a save loaded alone takes its own
// world back (personal_world_due).
std::optional<bbcoop::TeamWorld> team_world;
bool team_world_due = false;
bool team_world_taken = false;
std::set<u32> team_world_blocks;
bool team_host = false;
bool personal_world_due = false;
WorldFlags team_world_reported;
s64 team_world_reported_ms = 0;
constexpr s64 TeamWorldReportMs = 10'000;
// A load held for the team's world after the Start gives up waiting after this long.
constexpr s64 TeamWorldWaitMs = 10'000;
s64 team_world_wait_ms = 0;
// Phase 6: the team join without signs or a bell (see TeamJoin). The host's open session as the
// server last sent it (a guest joins it), the game thread's step, and the sessions this host
// opened (each one's number).
std::optional<bbcoop::TeamSession> team_session;
TeamJoin team_join;
u64 team_sessions_opened = 0;
// What the summon packet carries before the join data (0x01873754): GameMan +0x16F0 and +0x16F4
// on the guest, the same for every co-operator (rig 2026-10-14: 0xFFFFFFFF and 7).
constexpr u32 TeamSessionKind = 0xFFFFFFFFu;
constexpr u32 TeamSessionSign = 7;

std::string PartyKey() {
    if (team_switch) {
        std::scoped_lock lock{team_mutex};
        return team_party;
    }
    const char* setting = std::getenv("SHADPS4_BLOODBORNE_PARTY");
    return setting != nullptr ? ParsePartySetting(setting).value_or("") : "";
}

void QueueLobby(bbcoop::Envelope envelope) {
    std::scoped_lock lock{team_mutex};
    lobby_outbox.push_back(std::move(envelope));
}

// On the stream's thread, as they come.
bool TakeLobbyMessage(const bbcoop::Envelope& envelope) {
    if (envelope.has_lobby_state()) {
        std::scoped_lock lock{team_mutex};
        team_gate.TakeLobby(envelope.lobby_state());
        if (!envelope.lobby_state().notice().empty()) {
            LOG_WARNING(Debug, "Co-op team lobby: the server refused: {}",
                        envelope.lobby_state().notice());
        }
        return true;
    }
    if (envelope.has_team_world()) {
        std::scoped_lock lock{team_mutex};
        team_world = envelope.team_world();
        if (!team_world->exists() && team_world_due) {
            team_world_taken = true; // nothing to take: the host's save makes it
        }
        LOG_INFO(Debug, "Co-op team world: the server's has {} ({} flags on, version {})",
                 team_world->exists() ? "a world" : "no world yet", team_world->on_size(),
                 team_world->version());
        return true;
    }
    if (envelope.has_team_session()) {
        const bbcoop::TeamSession& session = envelope.team_session();
        LOG_INFO(Debug, "Co-op team join: {}'s session {} {}", session.host(), session.session(),
                 session.join().empty() ? "closed"
                                        : fmt::format("is open ({} bytes)", session.join().size()));
        std::scoped_lock lock{team_mutex};
        team_session = session;
        return true;
    }
    if (envelope.has_team_start()) {
        const bbcoop::TeamStart& start = envelope.team_start();
        LOG_INFO(Debug, "Co-op team lobby: team {} starts (party {}, host {}, slot {})",
                 start.team(), start.party(), start.host(), start.slot());
        std::scoped_lock lock{team_mutex};
        team_gate.TakeStart(start);
        return true;
    }
    return false;
}

// The lobby's hello when due, and its requests: on the stream's thread, stream_mutex held.
void SendLobby(s64 now_ms) {
    std::scoped_lock lock{team_mutex};
    if (!team_switch || lobby_member.empty()) {
        return;
    }
    if (lobby_hello_due) {
        lobby_hello_due = false;
        bbcoop::Envelope hello;
        hello.mutable_lobby_hello()->set_protocol(CoopProtocol);
        hello.mutable_lobby_hello()->set_member(lobby_member);
        hello.mutable_lobby_hello()->set_name(lobby_name);
        hello.mutable_lobby_hello()->set_playing(lobby_playing);
        stream->Send(EncodeEnvelope(hello), now_ms);
    }
    for (const bbcoop::Envelope& envelope : lobby_outbox) {
        stream->Send(EncodeEnvelope(envelope), now_ms);
    }
    lobby_outbox.clear();
}

void Queue(bbcoop::Envelope envelope) {
    std::scoped_lock lock{stream_mutex};
    outbox.push_back(std::move(envelope));
}

// The boards (3c): taken on the stream's thread as they come (not on the kill step's).
bool IsBoard(const bbcoop::Envelope& envelope) {
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kMapBoard:
    case bbcoop::Envelope::kRestBoard:
    case bbcoop::Envelope::kTrophies:
    case bbcoop::Envelope::kEchoes:
    case bbcoop::Envelope::kVitalsBoard:
    case bbcoop::Envelope::kPortrait:
    case bbcoop::Envelope::kEnemyOwners:
        return true;
    default:
        return false;
    }
}

LampPosition PositionOf(const bbcoop::Position& at) {
    return {at.x(), at.y(), at.z()};
}

void PutPosition(const LampPosition& at, bbcoop::Position* out) {
    out->set_x(at[0]);
    out->set_y(at[1]);
    out->set_z(at[2]);
}

void TakeBoard(const bbcoop::Envelope& envelope) {
    switch (envelope.body_case()) {
    case bbcoop::Envelope::kMapBoard: {
        std::vector<MemberMap> maps;
        for (const auto& entry : envelope.map_board().members()) {
            maps.push_back({entry.member(), entry.map()});
        }
        TakeServerMapBoard(std::move(maps));
        return;
    }
    case bbcoop::Envelope::kRestBoard: {
        std::vector<LampRest> rests;
        for (const auto& entry : envelope.rest_board().rests()) {
            rests.push_back(
                {entry.member(), entry.lamp(),
                 entry.has_at() ? std::optional{PositionOf(entry.at())} : std::nullopt});
        }
        TakeServerRestBoard(std::move(rests));
        return;
    }
    case bbcoop::Envelope::kTrophies: {
        const std::vector<s32> ids(envelope.trophies().ids().begin(),
                                   envelope.trophies().ids().end());
        ReadTrophyBoard(ids);
        return;
    }
    case bbcoop::Envelope::kEchoes: {
        std::vector<KillEchoes> echoes;
        for (const auto& kill : envelope.echoes().kills()) {
            echoes.push_back({kill.echoes(), kill.map()});
        }
        TakeServerEchoes(std::move(echoes));
        return;
    }
    case bbcoop::Envelope::kVitalsBoard: {
        VitalsBoard board;
        for (const auto& entry : envelope.vitals_board().members()) {
            const bbcoop::Vitals& vitals = entry.vitals();
            board.members.push_back(
                MemberVitals{.member = entry.member(),
                             .map = vitals.map(),
                             .vital = static_cast<Vital>(std::min<u32>(vitals.vital(), 3)),
                             .hp = vitals.hp(),
                             .max = vitals.max(),
                             .down = vitals.down(),
                             .landed = vitals.landed(),
                             .needed = vitals.needed(),
                             .at = PositionOf(vitals.at()),
                             .revive_damage = entry.revive_damage()});
        }
        for (const auto& wipe : envelope.vitals_board().wipes()) {
            board.wipes.push_back(
                {wipe.id(), wipe.map(), {wipe.members().begin(), wipe.members().end()}});
        }
        TakeServerVitals(std::move(board));
        return;
    }
    case bbcoop::Envelope::kPortrait: {
        const std::string& pixels = envelope.portrait().pixels();
        TakeServerPortrait(envelope.portrait().member(), envelope.portrait().id(),
                           std::vector<u8>(pixels.begin(), pixels.end()));
        return;
    }
    case bbcoop::Envelope::kEnemyOwners: {
        std::vector<EnemyOwnerEntry> owners;
        for (const bbcoop::EnemyOwner& owner : envelope.enemy_owners().owners()) {
            owners.push_back({owner.map(), owner.member(), owner.epoch()});
        }
        std::scoped_lock lock{owners_mutex};
        server_owners = std::move(owners);
        owners_heard_ms = SteadyMilliseconds();
        return;
    }
    default:
        return;
    }
}

void StreamTick(s64 now_ms) {
    std::vector<std::vector<u8>> received;
    std::vector<bbcoop::Envelope> boards;
    std::array<u8, 4096> buffer{};
    for (int count = 0; count < 512; ++count) {
        const int size =
            Libraries::Net::ServerCoopReceive(buffer.data(), static_cast<u32>(buffer.size()));
        if (size < 0) {
            break;
        }
        received.emplace_back(buffer.begin(), buffer.begin() + size);
    }
    std::vector<std::vector<u8>> out;
    bool restarted = false;
    {
        std::scoped_lock lock{stream_mutex};
        for (const std::vector<u8>& body : received) {
            for (const std::vector<u8>& message : stream->Receive(body, now_ms)) {
                if (auto envelope = DecodeEnvelope(message)) {
                    if (TakeLobbyMessage(*envelope)) {
                        continue;
                    }
                    if (IsTradeMessage(*envelope)) {
                        TakeServerTrade(std::move(*envelope));
                    } else if (IsBoard(*envelope)) {
                        boards.push_back(std::move(*envelope));
                    } else {
                        inbox.push_back(std::move(*envelope));
                        inbox_news.store(true);
                    }
                }
            }
        }
        if (stream->TakeRestarted()) {
            restarted = true;
            hello_due = true;
            std::scoped_lock team_lock{team_mutex};
            lobby_hello_due = true;
        }
        SendLobby(now_ms);
        if (hello_due && !hello_party.empty() && !hello_member.empty()) {
            hello_due = false;
            bbcoop::Envelope hello;
            hello.mutable_hello()->set_protocol(CoopProtocol);
            hello.mutable_hello()->set_party(hello_party);
            hello.mutable_hello()->set_member(hello_member);
            hello.mutable_hello()->set_run(run);
            hello.mutable_hello()->set_host(!IsPartyGuestRole());
            hello.mutable_hello()->set_enemy_owner(enemy_owner_switch);
            stream->Send(EncodeEnvelope(hello), now_ms);
        }
        if (!hello_due) {
            for (const bbcoop::Envelope& envelope : outbox) {
                stream->Send(EncodeEnvelope(envelope), now_ms);
            }
            outbox.clear();
        }
        out = stream->Flush(now_ms);
    }
    for (const bbcoop::Envelope& board : boards) {
        TakeBoard(board);
    }
    if (restarted) {
        LOG_WARNING(Debug, "Co-op server world: shadNet started over (or lost this game): "
                           "saying hello again");
        Trace("srvworld", {{"event", "restarted"}});
    }
    for (const std::vector<u8>& body : out) {
        Libraries::Net::ServerCoopSend(body.data(), static_cast<u32>(body.size()));
    }
}

void StartThread() {
    std::call_once(thread_started, [] {
        {
            std::scoped_lock lock{stream_mutex};
            stream.emplace(LocalOnlineId(), run);
        }
        std::thread{[] {
            while (true) {
                StreamTick(SteadyMilliseconds());
                std::this_thread::sleep_for(std::chrono::milliseconds{StreamTickMs});
            }
        }}.detach();
        LOG_INFO(Debug, "Co-op server world: on, shadNet keeps the party's world (run {:#x})", run);
    });
}

// This member's copy of the party's log (the game thread, and the copies others read).
std::mutex member_mutex;
WorldLogMember member;
std::atomic<bool> member_reset{false};
WorldLogKills last_proposed;
u32 proposals = 0;
s64 proposed_ms = 0;

void SendLifecycle(bbcoop::Lifecycle::Event event) {
    if (!server_world_switch) {
        return;
    }
    bbcoop::Envelope envelope;
    envelope.mutable_lifecycle()->set_event(event);
    envelope.mutable_lifecycle()->set_host(!IsPartyGuestRole());
    Queue(std::move(envelope));
    LOG_INFO(Debug, "Co-op server world: told shadNet this member's {} ({})",
             bbcoop::Lifecycle::Event_Name(event), IsPartyGuestRole() ? "a guest" : "the host");
}

// This member's session events, for the party's world: a wipe starts its log over once for the
// party; the host's join, leave or new character starts a new one, a guest's takes its share out.
// A new character out of a session is no event of the party's.
[[maybe_unused]] const bool wipe_registered = RegisterLifecycleReset(
    "server world (wipe)", {Lifecycle::Wipe}, [] { SendLifecycle(bbcoop::Lifecycle::WIPE); });
[[maybe_unused]] const bool join_registered =
    RegisterLifecycleReset("server world (join)", {Lifecycle::Join}, [] {
        member_reset.store(true);
        SendLifecycle(bbcoop::Lifecycle::JOIN);
    });
[[maybe_unused]] const bool leave_registered =
    RegisterLifecycleReset("server world (leave)", {Lifecycle::Leave}, [] {
        member_reset.store(true);
        SendLifecycle(bbcoop::Lifecycle::LEAVE);
    });
[[maybe_unused]] const bool new_character_registered =
    RegisterLifecycleReset("server world (new character)", {Lifecycle::NewCharacter}, [] {
        member_reset.store(true);
        if (InPartySession()) {
            SendLifecycle(bbcoop::Lifecycle::NEW_CHARACTER);
        }
    });

std::string EntriesText(std::span<const WorldLogEntry> entries) {
    std::string text;
    for (size_t index = 0; index < entries.size() && index < 8; ++index) {
        text += (text.empty() ? "" : ", ") + FormatWorldLogEntry(entries[index]);
    }
    return entries.size() > 8 ? fmt::format("{}, ... ({} in all)", text, entries.size()) : text;
}

void LogStep(const WorldLogMember::Step& step, std::string_view what) {
    if (step.new_epoch) {
        LOG_INFO(Debug, "Co-op server world: following the party's log {:#x}", member.Epoch());
    }
    if (step.snapshot) {
        LOG_INFO(Debug, "Co-op server world: took the party's world to seq {} ({} kills)",
                 member.Applied(), WorldLogKillCount(member.Kills()));
    }
    if (!step.applied.empty()) {
        LOG_INFO(Debug, "Co-op server world: applied {}", EntriesText(step.applied));
    }
    if (step.stale || step.new_epoch || step.snapshot || step.gap || !step.applied.empty()) {
        Trace("srvworld", {{"event", std::string{what}},
                           {"epoch", fmt::format("{:#x}", member.Epoch())},
                           {"seq", std::to_string(member.Applied())},
                           {"applied", std::to_string(step.applied.size())},
                           {"gap", step.gap ? "1" : "0"},
                           {"stale", step.stale ? "1" : "0"}});
    }
}

struct Taken {
    bool after_wipe = false; // the party's log started over after a wipe in it
    std::vector<WorldLogEntry> applied;
};

// What came from the server, on the game thread (member_mutex held).
Taken TakeInbox(s64 now_ms) {
    std::deque<bbcoop::Envelope> taken;
    {
        std::scoped_lock lock{stream_mutex};
        taken.swap(inbox);
    }
    Taken result;
    const auto took = [&](const WorldLogMember::Step& step, std::string_view what) {
        result.after_wipe = result.after_wipe || step.after_wipe || StartsAfterAWipe(step.applied);
        result.applied.insert(result.applied.end(), step.applied.begin(), step.applied.end());
        LogStep(step, what);
    };
    for (const bbcoop::Envelope& envelope : taken) {
        if (envelope.has_welcome()) {
            const bbcoop::Welcome& welcome = envelope.welcome();
            TakeServerTradeWelcome(welcome.accepted() && welcome.trades());
            if (welcome.accepted()) {
                LOG_INFO(Debug,
                         "Co-op server world: shadNet took this member (log {:#x} at seq {})",
                         welcome.epoch(), welcome.last());
            } else {
                LOG_ERROR(Debug, "Co-op server world: shadNet refused this member: {}",
                          welcome.text());
            }
            Trace("srvworld", {{"event", welcome.accepted() ? "welcome" : "refused"},
                               {"text", welcome.text()}});
        } else if (envelope.has_entries()) {
            took(member.Take(BatchOf(envelope.entries()), now_ms), "entries");
        } else if (envelope.has_snapshot()) {
            const SnapshotParts parts = PartsOf(envelope.snapshot());
            member.Take(parts.kills, now_ms);
            member.Take(parts.shown, now_ms);
            member.Take(parts.map_state, now_ms);
            member.Take(parts.hp, now_ms);
            took(member.Take(parts.flags, now_ms), "snapshot");
        }
    }
    return result;
}

// The host's last reports, sent again on a change or after ReportResendMs.
template <typename T>
struct LastReport {
    std::optional<T> value;
    s64 sent_ms = 0;
    bool Due(const T& now, s64 now_ms) {
        if (value == now && now_ms - sent_ms < ReportResendMs) {
            return false;
        }
        value = now;
        sent_ms = now_ms;
        return true;
    }
};
std::mutex reports_mutex;
LastReport<std::pair<u32, std::vector<CharacterShown>>> last_shown;
LastReport<std::pair<u32, std::vector<u8>>> last_map_state;
LastReport<WorldLogReturnPoint> last_return_point;
WorldLogFlagOutbox flag_outbox;
s64 flags_sent_ms = 0;
u32 hp_proposals = 0;
s64 hp_sent_ms = 0;

// The member's flag events (any thread queues them, the kill step sends them; member_mutex held).
void SendFlags(s64 now_ms) {
    std::vector<WorldLogFlagEvent> events;
    {
        std::scoped_lock lock{reports_mutex};
        if (!flag_outbox.TakeNew() && now_ms - flags_sent_ms < WorldLogResendMs) {
            return;
        }
        events = flag_outbox.Pending(now_ms);
        if (events.empty()) {
            return;
        }
        flags_sent_ms = now_ms;
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_flags() =
        ToMessage(WorldLogFlagProposal{{}, member.Epoch(), run, std::move(events)});
    Queue(std::move(envelope));
}

} // namespace

namespace Coop {

bool ServerWorldOn() {
    return server_world_switch;
}

bool EnemyOwnerOn() {
    return enemy_owner_switch;
}

bool TakeServerWorldNews() {
    return enemy_owner_switch && inbox_news.exchange(false);
}

std::vector<EnemyOwnerEntry> ServerEnemyOwners(s64 now_ms) {
    std::scoped_lock lock{owners_mutex};
    if (owners_heard_ms == 0 || now_ms - owners_heard_ms > EnemyOwnersFreshMs) {
        return {};
    }
    return server_owners;
}

ServerWorldKills TickServerWorld(const WorldLogKills& mine, bool propose, s64 now_ms) {
    StartThread();
    std::scoped_lock member_lock{member_mutex};
    {
        std::scoped_lock lock{stream_mutex};
        const std::string party = PartyKey();
        if (party != hello_party && !hello_party.empty()) {
            hello_due = true; // another team's world (phase 5): a new hello
        }
        hello_party = party;
        std::scoped_lock vitals{vitals_mutex};
        if (!vitals_member.empty()) {
            hello_member = vitals_member;
        }
    }
    if (member_reset.exchange(false)) {
        member.Reset();
        last_proposed.clear();
        proposed_ms = 0;
    }
    TickTeamWorld(now_ms);
    ServerWorldKills world;
    Taken taken = TakeInbox(now_ms);
    world.host_wiped = taken.after_wipe;
    world.applied = std::move(taken.applied);
    if (propose) {
        SendFlags(now_ms);
    }
    if (propose && (mine != last_proposed || now_ms - proposed_ms >= KillsResendMs)) {
        last_proposed = mine;
        proposed_ms = now_ms;
        bbcoop::Envelope envelope;
        *envelope.mutable_kills() =
            ToMessage(WorldLogProposal{{}, member.Epoch(), run, ++proposals, mine});
        Queue(std::move(envelope));
    }
    if (member.SnapshotDue(now_ms)) {
        bbcoop::Envelope envelope;
        envelope.mutable_snapshot_request()->set_epoch(member.Epoch());
        envelope.mutable_snapshot_request()->set_seq(member.Applied());
        Queue(std::move(envelope));
        LOG_INFO(Debug,
                 "Co-op server world: asked shadNet for the party's world (log {:#x}, "
                 "seq {})",
                 member.Epoch(), member.Applied());
    }
    world.kills = WorldLogHandles(member.Kills());
    world.fresh = member.Fresh(now_ms);
    world.return_point = member.HostReturnPoint();
    return world;
}

void ShowToServer(u32 map, std::span<const CharacterShown> characters) {
    const s64 now_ms = SteadyMilliseconds();
    {
        std::scoped_lock lock{reports_mutex};
        if (!last_shown.Due({map, {characters.begin(), characters.end()}}, now_ms)) {
            return;
        }
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_shown() = ToMessage(map, characters);
    Queue(std::move(envelope));
}

void MapStateToServer(u32 map, std::span<const u8> bits) {
    const s64 now_ms = SteadyMilliseconds();
    {
        std::scoped_lock lock{reports_mutex};
        if (!last_map_state.Due({map, {bits.begin(), bits.end()}}, now_ms)) {
            return;
        }
    }
    bbcoop::Envelope envelope;
    envelope.mutable_map_state()->set_map(map);
    envelope.mutable_map_state()->set_bits(std::string(bits.begin(), bits.end()));
    Queue(std::move(envelope));
}

void ReturnPointToServer(s32 point, u32 map) {
    if (point <= 0) {
        return; // a load: the last one stands
    }
    {
        std::scoped_lock lock{reports_mutex};
        if (!last_return_point.Due({point, map}, SteadyMilliseconds())) {
            return;
        }
    }
    bbcoop::Envelope envelope;
    envelope.mutable_return_point()->set_point(point);
    envelope.mutable_return_point()->set_map(map);
    Queue(std::move(envelope));
}

void FlagsToServer(std::span<const std::pair<s32, bool>> shared) {
    std::scoped_lock lock{reports_mutex};
    flag_outbox.Add(shared, SteadyMilliseconds());
}

void HpToServer(u32 map, const EnemyHpReport& report, bool changed) {
    const s64 now_ms = SteadyMilliseconds();
    u64 epoch = 0;
    {
        std::scoped_lock lock{member_mutex};
        epoch = member.Epoch();
    }
    {
        std::scoped_lock lock{reports_mutex};
        if (!changed && now_ms - hp_sent_ms < WorldLogResendMs) {
            return;
        }
        hp_sent_ms = now_ms;
    }
    bbcoop::Envelope envelope;
    *envelope.mutable_hp() =
        ToMessage(WorldLogHpProposal{{}, epoch, run, ++hp_proposals, map, report});
    Queue(std::move(envelope));
}

std::optional<WorldLogShown> ServerShown() {
    std::scoped_lock lock{member_mutex};
    if (!member.ShownKnown()) {
        return std::nullopt;
    }
    return member.Shown();
}

std::optional<WorldLogMapState> ServerMapState() {
    std::scoped_lock lock{member_mutex};
    if (!member.MapStateKnown()) {
        return std::nullopt;
    }
    return member.MapState();
}

WorldLogHp ServerHp() {
    std::scoped_lock lock{member_mutex};
    return member.Hp();
}

void QueueServerWorld(bbcoop::Envelope envelope) {
    Queue(std::move(envelope));
}

void StartServerWorld() {
    if (server_world_switch) {
        StartThread();
    }
}

void SendServerMap(u32 map) {
    bbcoop::Envelope envelope;
    envelope.mutable_map()->set_map(map);
    Queue(std::move(envelope));
}

void SendServerRest(s32 lamp, const std::optional<LampPosition>& at) {
    bbcoop::Envelope envelope;
    envelope.mutable_rest()->set_lamp(lamp);
    if (at) {
        PutPosition(*at, envelope.mutable_rest()->mutable_at());
    }
    Queue(std::move(envelope));
}

void SendServerTrophies(std::span<const s32> trophies) {
    bbcoop::Envelope envelope;
    for (const s32 trophy : trophies) {
        envelope.mutable_trophies()->add_ids(trophy);
    }
    Queue(std::move(envelope));
}

void SendServerEchoes(std::span<const KillEchoes> echoes) {
    bbcoop::Envelope envelope;
    for (const KillEchoes& kill : echoes) {
        bbcoop::EchoKill* out = envelope.mutable_echoes()->add_kills();
        out->set_echoes(kill.amount);
        out->set_map(kill.map);
    }
    Queue(std::move(envelope));
}

void SendServerVitals(const MemberVitals& mine, std::span<const ReviveHitTotal> hits) {
    bbcoop::Envelope envelope;
    bbcoop::Vitals& vitals = *envelope.mutable_vitals();
    vitals.set_map(mine.map);
    vitals.set_vital(static_cast<u32>(mine.vital));
    vitals.set_hp(mine.hp);
    vitals.set_max(mine.max);
    vitals.set_down(mine.down);
    vitals.set_landed(mine.landed);
    vitals.set_needed(mine.needed);
    PutPosition(mine.at, vitals.mutable_at());
    for (const ReviveHitTotal& hit : hits) {
        bbcoop::ReviveHit* out = vitals.add_hits();
        out->set_target(hit.target);
        out->set_down(hit.down);
        out->set_damage(hit.damage);
    }
    Queue(std::move(envelope));
}

void SendServerPortrait(u32 id, std::span<const u8> pixels) {
    bbcoop::Envelope envelope;
    envelope.mutable_portrait()->set_id(id);
    envelope.mutable_portrait()->set_pixels(std::string(pixels.begin(), pixels.end()));
    Queue(std::move(envelope));
}

bool TeamModeOn() {
    return team_switch;
}

void StartTeamLobby() {
    if (!team_switch) {
        return;
    }
    std::thread{[] {
        // The online id comes once the game signed in (a few seconds into the start).
        std::string online_id;
        for (int attempt = 0; attempt < 600 && online_id.empty(); ++attempt) {
            online_id = LocalOnlineId();
            if (online_id.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
            }
        }
        if (online_id.empty()) {
            LOG_ERROR(Debug, "Co-op team lobby: no online id: not signed in to the lobby");
            return;
        }
        const std::string member = BoardMemberOfOnlineId(online_id);
        {
            std::scoped_lock lock{team_mutex};
            lobby_member = member;
            lobby_name = online_id;
            lobby_hello_due = true;
        }
        StartThread();
        LOG_INFO(Debug, "Co-op team lobby: signing in as {} ({})", online_id, member);
    }}.detach();
}

bool HoldTitleLoadForTeam(u32 slot) {
    if (!team_switch) {
        return false;
    }
    TeamGate::Decision decision;
    std::string me;
    bool new_party = false;
    bool hold_for_world = false;
    {
        std::scoped_lock lock{team_mutex};
        me = lobby_member;
        const bool first = !team_gate.Holding();
        decision = team_gate.Tick(me, slot, SteadyMilliseconds());
        if (first) {
            LOG_INFO(Debug,
                     "Co-op team lobby: the title loads slot {}: the team screen decides "
                     "({})",
                     slot,
                     !team_gate.Lobby()              ? "no word from the lobby yet"
                     : team_gate.Lobby()->has_team() ? "in a team"
                                                     : "not in a team");
        }
        if (decision.step == TeamGate::Step::Hold) {
            return true;
        }
        if (decision.start && team_party != decision.start->party()) {
            // The team's party from its Start: the party's hello goes now, and the load waits
            // for the team's world (TeamWorldWaitMs at most) so the save takes it as it loads.
            team_party = decision.start->party();
            team_world.reset();
            team_session.reset();
            team_world_wait_ms = SteadyMilliseconds();
            new_party = true;
        }
        hold_for_world = decision.start && !team_world &&
                         SteadyMilliseconds() - team_world_wait_ms < TeamWorldWaitMs;
    }
    if (new_party) {
        const bool host = decision.start->host() == me;
        if (party_controller) {
            party_controller->Assign(host ? PartyRole::Host : PartyRole::Guest); // before the hello
        }
        std::scoped_lock lock{stream_mutex};
        if (hello_party != decision.start->party()) {
            hello_party = decision.start->party();
            hello_due = true;
        }
        if (hello_member.empty()) {
            hello_member = me; // the board name (the kill step sets it in the world)
        }
    }
    if (hold_for_world) {
        return true;
    }
    {
        std::scoped_lock lock{team_mutex};
        if (!team_world && decision.start) {
            LOG_WARNING(Debug,
                        "Co-op team world: no word of the team's world in {} ms: the save "
                        "loads without it",
                        TeamWorldWaitMs);
        }
        lobby_playing = true;
        lobby_hello_due = true;
        // A save alone after a team's plays no team's world.
        if (!decision.start) {
            team_party.clear();
            team_world.reset();
        }
        team_world_due = decision.start.has_value();
        team_world_taken = false;
        team_world_blocks.clear();
        team_host = decision.start && decision.start->host() == me;
        personal_world_due = !decision.start.has_value();
        team_world_reported = {};
        team_world_reported_ms = 0;
    }
    if (decision.start && party_controller) {
        const bool host = decision.start->host() == me;
        party_controller->Assign(host ? PartyRole::Host : PartyRole::Guest);
        LOG_INFO(Debug, "Co-op team lobby: the save plays in team {} as its {}",
                 decision.start->team(), host ? "host" : "guest");
    } else {
        if (party_controller) {
            party_controller->Wait();
        }
        LOG_INFO(Debug, "Co-op team lobby: the save plays alone");
    }
    return false;
}

bool TeamScreenUp() {
    if (!team_switch) {
        return false;
    }
    std::scoped_lock lock{team_mutex};
    return team_gate.Holding() && !lobby_playing;
}

void TeamChoose(TeamGate::Choice choice) {
    std::scoped_lock lock{team_mutex};
    if (!team_gate.Holding() || lobby_playing) {
        return;
    }
    static constexpr std::array<const char*, 4> Names{"ready", "not ready", "start", "alone"};
    LOG_INFO(Debug, "Co-op team lobby: the team screen chose {}",
             Names[static_cast<size_t>(choice)]);
    for (bbcoop::TeamRequest& request : team_gate.Choose(choice, lobby_member)) {
        bbcoop::Envelope envelope;
        *envelope.mutable_team_request() = std::move(request);
        lobby_outbox.push_back(std::move(envelope));
    }
}

void TeamBackAtTitle() {
    if (!team_switch) {
        return;
    }
    std::scoped_lock lock{team_mutex};
    if (lobby_playing || team_gate.Holding()) {
        team_gate.Reset();
        lobby_playing = false;
        lobby_hello_due = true;
    }
    team_session.reset();
}

bool TickTeamJoin(bool ready) {
    if (!team_switch) {
        return false;
    }
    const u64 manager = GetMatchingState();
    if (manager < 0x10000 || !HasMemoryAccess(manager, 0x128, MemoryProt::CpuRead)) {
        return true;
    }
    std::optional<bbcoop::TeamSession> session;
    bool in_team = false;
    {
        std::scoped_lock lock{team_mutex};
        in_team = !team_party.empty() && lobby_playing;
        session = team_session;
    }
    if (!in_team) {
        return true; // alone: no session to open or join
    }
    const TeamJoin::Frame frame{
        .host = !IsPartyGuestRole(),
        .matching_state = ReadValue<s32>(manager, 0x124),
        .ready = ready,
        .session_known = session.has_value() && !session->join().empty(),
    };
    const auto say = [](std::string join, u64 number) {
        bbcoop::Envelope envelope;
        envelope.mutable_team_session()->set_join(std::move(join));
        envelope.mutable_team_session()->set_kind(TeamSessionKind);
        envelope.mutable_team_session()->set_sign(TeamSessionSign);
        envelope.mutable_team_session()->set_session(number);
        Queue(std::move(envelope));
    };
    FinishInPlaceJoin();
    switch (team_join.Tick(frame)) {
    case TeamJoin::Action::None:
        break;
    case TeamJoin::Action::CreateSession:
        OpenTeamSession();
        break;
    case TeamJoin::Action::Publish:
        if (const auto join = TeamSessionJoinData()) {
            ++team_sessions_opened;
            say(std::string{join->begin(), join->end()}, team_sessions_opened);
            LOG_INFO(Debug, "Co-op team join: this host's session {} to the server ({} bytes)",
                     team_sessions_opened, join->size());
        } else {
            LOG_WARNING(Debug, "Co-op team join: this host's session has no join data");
        }
        break;
    case TeamJoin::Action::PublishClosed:
        say({}, team_sessions_opened);
        LOG_INFO(Debug, "Co-op team join: this host's session {} closed", team_sessions_opened);
        break;
    case TeamJoin::Action::Join: {
        const std::string& join = session->join();
        JoinTeamSession({reinterpret_cast<const u8*>(join.data()), join.size()}, session->kind(),
                        session->sign());
        break;
    }
    }
    return true;
}

std::optional<bbcoop::LobbyState> TeamLobbyState() {
    std::scoped_lock lock{team_mutex};
    return team_gate.Lobby();
}

std::string TeamLobbyMember() {
    std::scoped_lock lock{team_mutex};
    return lobby_member;
}

void TeamLobbyRequest(bbcoop::TeamRequest::Action action, const std::string& id) {
    bbcoop::Envelope envelope;
    bbcoop::TeamRequest& request = *envelope.mutable_team_request();
    request.set_action(action);
    if (action == bbcoop::TeamRequest::ACCEPT || action == bbcoop::TeamRequest::DECLINE) {
        request.set_team(id);
    } else {
        request.set_member(id);
    }
    LOG_INFO(Debug, "Co-op team lobby: the menu asks {} {}",
             bbcoop::TeamRequest::Action_Name(action), id);
    QueueLobby(std::move(envelope));
}

namespace {

std::filesystem::path PersonalWorldFile(int slot) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           SlotFileName("personal_world", slot);
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, {}};
}

// Written whole beside it, then moved over it: a crash leaves the old file or the new one.
void WriteWholeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path partial = path;
    partial += ".part";
    {
        std::ofstream file{partial, std::ios::binary | std::ios::trunc};
        file << text;
    }
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    if (error) {
        LOG_ERROR(Debug, "Co-op team world: could not write {}: {}", path.string(),
                  error.message());
    }
}

struct WorldStore {
    FlagStore store;
    std::vector<FlagBlockBits> blocks;
};
std::optional<WorldStore> ReadWorldStore() {
    WorldStore world{ReadFlagStore(), {}};
    if (!world.store.whole || world.store.flags_per_block == 0 || world.store.blocks.empty()) {
        return std::nullopt;
    }
    for (const FlagBlock& block : world.store.blocks) {
        world.blocks.push_back({block.id, block.bytes});
    }
    return world;
}

// Sets the store's world flags to `target`'s: how many changed.
size_t SetWorldFlags(const WorldStore& world, const WorldFlags& target) {
    using SetFlag = void PS4_SYSV_ABI (*)(u64, s32, bool);
    const auto changes = WorldFlagChanges(world.store.flags_per_block, world.blocks, target);
    for (const auto& [flag, on] : changes) {
        reinterpret_cast<SetFlag>(image_base + PartyGhostSetEventFlagOffset)(0, flag, on);
    }
    return changes.size();
}

} // namespace

void ApplyTeamWorldOnLoad() {
    if (!team_switch) {
        return;
    }
    std::optional<bbcoop::TeamWorld> world;
    bool due = false;
    bool personal = false;
    {
        std::scoped_lock lock{team_mutex};
        due = team_world_due && team_world.has_value();
        personal = personal_world_due;
        if (due) {
            world = team_world;
        }
        if (team_world_due && !team_world) {
            static s64 warned_ms = 0;
            if (SteadyMilliseconds() - warned_ms > 5'000) {
                warned_ms = SteadyMilliseconds();
                LOG_WARNING(Debug, "Co-op team world: the map loads before the server's world "
                                   "came; it goes in at the next load");
            }
        }
    }
    const int slot = character_slot.load();
    if (personal && slot >= 0) {
        // Alone again: this save's own world back where a team's was in it, block by block as
        // the loads bring them; the file keeps the blocks still to come.
        const std::string text = ReadWholeFile(PersonalWorldFile(slot));
        std::scoped_lock lock{team_mutex};
        if (text.empty()) {
            personal_world_due = false;
            return;
        }
        auto own = ParseWorldFlags(text);
        std::optional<WorldStore> store = ReadWorldStore();
        if (!own || !store) {
            LOG_WARNING(Debug, "Co-op team world: slot {}'s own world {}", slot,
                        own ? "waits (the flags are not readable now)" : "does not read: left");
            personal_world_due = own.has_value();
            return;
        }
        std::erase_if(store->blocks, [&](const FlagBlockBits& block) {
            return !std::binary_search(own->blocks.begin(), own->blocks.end(), block.id);
        });
        if (store->blocks.empty()) {
            return;
        }
        const size_t changed = SetWorldFlags(*store, *own);
        const u32 per_block = store->store.flags_per_block;
        for (const FlagBlockBits& block : store->blocks) {
            std::erase(own->blocks, block.id);
            std::erase_if(own->on, [&](s32 flag) { return u32(flag) / per_block == block.id; });
        }
        if (own->blocks.empty()) {
            std::error_code error;
            std::filesystem::remove(PersonalWorldFile(slot), error);
            personal_world_due = false;
        } else {
            WriteWholeFile(PersonalWorldFile(slot), FormatWorldFlags(*own));
        }
        LOG_INFO(Debug,
                 "Co-op team world: slot {} plays alone: its own world is back in {} "
                 "blocks ({} flags changed, {} blocks to come)",
                 slot, store->blocks.size(), changed, own->blocks.size());
        return;
    }
    if (!due) {
        return;
    }
    std::optional<WorldStore> store = ReadWorldStore();
    if (!store || slot < 0) {
        return; // the next call (the flags readable, the slot known)
    }
    std::scoped_lock lock{team_mutex};
    if (!world->exists()) {
        if (!team_world_taken) {
            LOG_INFO(Debug, "Co-op team world: the team has no world yet: {}",
                     team_host ? "this save's makes it" : "the host's save makes it");
        }
        team_world_taken = true;
        return;
    }
    const WorldFlags target = WorldFlagsOf(*world);
    // The blocks this load brings that the team's world covers and no earlier load took.
    std::erase_if(store->blocks, [&](const FlagBlockBits& block) {
        return team_world_blocks.contains(block.id) ||
               !std::binary_search(target.blocks.begin(), target.blocks.end(), block.id);
    });
    if (store->blocks.empty()) {
        return;
    }
    // This save's own world aside first, block by block as the loads bring them: a block the file
    // has already (a save that held a team's before) keeps what the file says.
    {
        WorldFlags own =
            ParseWorldFlags(ReadWholeFile(PersonalWorldFile(slot))).value_or(WorldFlags{});
        std::vector<FlagBlockBits> new_blocks;
        for (const FlagBlockBits& block : store->blocks) {
            if (!std::binary_search(own.blocks.begin(), own.blocks.end(), block.id)) {
                new_blocks.push_back(block);
            }
        }
        if (!new_blocks.empty()) {
            const WorldFlags added = CaptureWorldFlags(store->store.flags_per_block, new_blocks);
            own.blocks.insert(own.blocks.end(), added.blocks.begin(), added.blocks.end());
            own.on.insert(own.on.end(), added.on.begin(), added.on.end());
            std::sort(own.blocks.begin(), own.blocks.end());
            std::sort(own.on.begin(), own.on.end());
            WriteWholeFile(PersonalWorldFile(slot), FormatWorldFlags(own));
            LOG_INFO(Debug,
                     "Co-op team world: slot {}'s own world kept aside ({} more blocks, {} "
                     "flags on in all)",
                     slot, added.blocks.size(), own.on.size());
        }
    }
    const size_t changed = SetWorldFlags(*store, target);
    for (const FlagBlockBits& block : store->blocks) {
        team_world_blocks.insert(block.id);
    }
    team_world_taken = true;
    LOG_INFO(Debug,
             "Co-op team world: the team's world is in this save: {} blocks, {} flags "
             "changed (version {})",
             store->blocks.size(), changed, world->version());
}

void TickTeamWorld(s64 now_ms) {
    if (!team_switch) {
        return;
    }
    {
        std::scoped_lock lock{team_mutex};
        if (!team_host || team_party.empty() || !team_world_taken || !team_world ||
            now_ms - team_world_reported_ms < TeamWorldReportMs) {
            return;
        }
        team_world_reported_ms = now_ms;
    }
    const auto store = ReadWorldStore();
    if (!store) {
        return;
    }
    WorldFlags world = CaptureWorldFlags(store->store.flags_per_block, store->blocks);
    {
        std::scoped_lock lock{team_mutex};
        if (world == team_world_reported) {
            return;
        }
        team_world_reported = world;
    }
    bbcoop::Envelope envelope;
    envelope.mutable_team_world()->mutable_on()->Add(world.on.begin(), world.on.end());
    envelope.mutable_team_world()->mutable_blocks()->Add(world.blocks.begin(), world.blocks.end());
    Queue(std::move(envelope));
    LOG_INFO(Debug, "Co-op team world: this host's world to the server ({} flags on)",
             world.on.size());
}

bool IsTeamScreenVerb(std::string_view verb) {
    return verb == "team-screen" || verb == "team-ready" || verb == "team-start" ||
           verb == "team-alone";
}

bool IsTeamTitleVerb(std::string_view verb) {
    return verb.starts_with("team-");
}

bool RunTeamTitleVerb(const AutoplayCommand& command, bool begun, double elapsed,
                      std::string& failure) {
    constexpr double TimeoutSeconds = 180.0;
    std::optional<bbcoop::LobbyState> lobby;
    {
        std::scoped_lock lock{team_mutex};
        lobby = team_gate.Lobby();
    }
    const auto request = [](bbcoop::TeamRequest::Action action, std::string member = {},
                            std::string team = {}) {
        bbcoop::Envelope envelope;
        envelope.mutable_team_request()->set_action(action);
        envelope.mutable_team_request()->set_member(std::move(member));
        envelope.mutable_team_request()->set_team(std::move(team));
        QueueLobby(std::move(envelope));
    };
    bool done = false;
    if (command.verb == "team-create") {
        // Once the lobby answered: create, then wait for the team in the state.
        done = lobby && lobby->has_team();
        if (lobby && !lobby->has_team() && (!begun || elapsed > 5.0)) {
            request(bbcoop::TeamRequest::CREATE);
        }
    } else if (command.verb == "team-invite" && command.args.size() == 1) {
        const std::string member = BoardMemberOfOnlineId(command.args[0]);
        if (lobby && lobby->has_team()) {
            for (const bbcoop::LobbyPlayer& player : lobby->players()) {
                if (player.member() == member) {
                    request(bbcoop::TeamRequest::INVITE, member);
                    done = true;
                }
            }
        }
    } else if (command.verb == "team-accept") {
        done = lobby && lobby->has_team();
        if (!done && lobby && lobby->invites_size() > 0) {
            request(bbcoop::TeamRequest::ACCEPT, {}, lobby->invites(0).team());
        }
    } else if (IsTeamScreenVerb(command.verb)) {
        if (!TeamScreenUp()) {
            return false; // the title's Cross first (TickAutoplayTitle presses it)
        }
        if (command.verb != "team-screen") {
            TeamChoose(command.verb == "team-ready"   ? TeamGate::Choice::Ready
                       : command.verb == "team-start" ? TeamGate::Choice::Start
                                                      : TeamGate::Choice::Alone);
        }
        done = true;
    } else if (command.verb == "team-leave") {
        done = lobby && !lobby->has_team();
        if (lobby && lobby->has_team() && (!begun || elapsed > 5.0)) {
            request(bbcoop::TeamRequest::LEAVE);
        }
    } else if (command.verb == "team-wait" && command.args.size() == 1) {
        done = lobby && lobby->has_team() &&
               lobby->team().members_size() >= std::atoi(command.args[0].c_str());
    } else {
        failure = "unknown team verb";
        return false;
    }
    if (!done && elapsed > TimeoutSeconds) {
        failure = "timed out in the lobby";
    }
    return done;
}

} // namespace Coop

} // namespace Core::Bloodborne
