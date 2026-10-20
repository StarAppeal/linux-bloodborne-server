// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Phase 5: the title's load held for the team (TeamGate) and a party controller that waits for
// the team to say its role.
#include <algorithm>
#include <set>

#include <gtest/gtest.h>

#include "core/bloodborne_party.h"
#include "core/bloodborne_team.h"

using namespace Core::Bloodborne;

namespace {

bbcoop::LobbyState InTeam(const std::string& leader, const std::string& id = "t1") {
    bbcoop::LobbyState state;
    state.mutable_team()->set_id(id);
    state.mutable_team()->set_leader(leader);
    // The world it plays (a world of the server's, apart from the team).
    state.mutable_team()->set_world("w-" + id);
    state.mutable_team()->set_party("world:w-" + id);
    return state;
}

bbcoop::TeamStart StartOf(const std::string& host, u32 slot) {
    bbcoop::TeamStart start;
    start.set_team("t1");
    start.set_party("world:w-t1");
    start.set_host(host);
    start.set_slot(slot);
    return start;
}

// The team screen decides: nothing loads until its player plays alone or the team starts.
TEST(TeamGate, NothingLoadsUntilTheScreenSaysSo) {
    TeamGate gate;
    EXPECT_EQ(gate.Tick("me", 0, 1000).step, TeamGate::Step::Hold); // no word from the lobby
    gate.TakeLobby({});
    const TeamGate::Decision decision = gate.Tick("me", 0, 600'000);
    EXPECT_EQ(decision.step, TeamGate::Step::Hold); // not in a team: create, accept or alone
    EXPECT_TRUE(gate.Holding());
}

TEST(TeamGate, PlayAloneLoadsAlone) {
    TeamGate gate;
    gate.TakeLobby(InTeam("leader"));
    gate.Tick("me", 1, 1000);
    EXPECT_TRUE(gate.Choose(TeamGate::Choice::Alone, "me").empty());
    const TeamGate::Decision decision = gate.Tick("me", 1, 1100);
    EXPECT_EQ(decision.step, TeamGate::Step::Load);
    EXPECT_FALSE(decision.start);
}

TEST(TeamGate, ReadyAsksWithTheSlotAndWaitsForTheStart) {
    TeamGate gate;
    gate.TakeLobby(InTeam("leader"));
    EXPECT_EQ(gate.Tick("me", 2, 1000).step, TeamGate::Step::Hold);
    const auto requests = gate.Choose(TeamGate::Choice::Ready, "me");
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].action(), bbcoop::TeamRequest::READY);
    EXPECT_EQ(requests[0].slot(), 2u);
    EXPECT_EQ(gate.Tick("me", 2, 60'000).step, TeamGate::Step::Hold);
    gate.TakeStart(StartOf("leader", 2));
    const TeamGate::Decision decision = gate.Tick("me", 2, 61'000);
    EXPECT_EQ(decision.step, TeamGate::Step::Load);
    ASSERT_TRUE(decision.start);
    EXPECT_EQ(decision.start->party(), "world:w-t1");
}

TEST(TeamGate, TheLeadersStartIsItsReadyAndTheTeamsStart) {
    TeamGate gate;
    gate.TakeLobby(InTeam("me"));
    gate.Tick("me", 0, 1000);
    const auto requests = gate.Choose(TeamGate::Choice::Start, "me");
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].action(), bbcoop::TeamRequest::READY);
    EXPECT_EQ(requests[1].action(), bbcoop::TeamRequest::START);
    // A member's Start is only its Ready (the leader starts).
    TeamGate member;
    member.TakeLobby(InTeam("leader"));
    member.Tick("me", 0, 1000);
    EXPECT_EQ(member.Choose(TeamGate::Choice::Start, "me").size(), 1u);
    EXPECT_EQ(member.Choose(TeamGate::Choice::Unready, "me")[0].action(),
              bbcoop::TeamRequest::UNREADY);
}

TEST(TeamGate, AnEarlierLoadsChoicesDoNotCount) {
    TeamGate gate;
    gate.TakeLobby(InTeam("leader"));
    gate.Tick("me", 0, 1000);
    gate.Choose(TeamGate::Choice::Alone, "me");
    gate.TakeStart(StartOf("leader", 0));
    // Back at the title: the next load asks again.
    gate.Reset();
    EXPECT_FALSE(gate.Holding());
    EXPECT_EQ(gate.Tick("me", 0, 2000).step, TeamGate::Step::Hold);
}

TEST(PartyControllerTeam, AWaitingControllerRingsNothingUntilItsRoleComes) {
    PartyController controller{PartyRole::Host, true};
    EXPECT_TRUE(controller.Waiting());
    PartyObservation idle{.player_valid = true,
                          .map_usable = true,
                          .stage_transition = false,
                          .matching_idle = true,
                          .search_effect_active = false};
    auto now = PartyController::Clock::now();
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(controller.Tick(idle, now), PartyAction::Wait);
        now += std::chrono::milliseconds{33};
    }
    controller.Assign(PartyRole::Guest);
    EXPECT_FALSE(controller.Waiting());
    EXPECT_EQ(controller.SearchEffectId(), SmallResonantBellEffect);
    PartyAction action = PartyAction::Wait;
    for (int i = 0; i < 100 && action == PartyAction::Wait; ++i) {
        action = controller.Tick(idle, now);
        now += std::chrono::milliseconds{33};
    }
    EXPECT_EQ(action, PartyAction::StartSearch);
    // A save alone after the team's: nothing rings again.
    controller.Wait();
    EXPECT_TRUE(controller.Waiting());
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(controller.Tick(idle, now), PartyAction::Wait);
        now += std::chrono::milliseconds{33};
    }
}

} // namespace

namespace {

// A store of three blocks of 1000 flags (125 bytes): 1 (1000-1999, the world's), 4 (4000-4999,
// the session's bookkeeping) and 9 (9000-9999: 9300-9307 are each player's own).
struct Store {
    std::vector<std::vector<u8>> bytes{3, std::vector<u8>(125)};
    std::vector<u32> ids{1, 4, 9};
    void Set(s32 flag, bool on = true) {
        const size_t block = std::find(ids.begin(), ids.end(), u32(flag / 1000)) - ids.begin();
        const u32 within = u32(flag % 1000);
        const u8 mask = u8(0x80 >> (within % 8));
        auto& byte = bytes[block][within / 8];
        byte = on ? u8(byte | mask) : u8(byte & ~mask);
    }
    std::vector<FlagBlockBits> Blocks() const {
        std::vector<FlagBlockBits> out;
        for (size_t i = 0; i < ids.size(); ++i) {
            out.push_back({ids[i], bytes[i]});
        }
        return out;
    }
};

TEST(TeamWorld, TheWorldsFlagsOnlyAreCaptured) {
    EXPECT_TRUE(IsWorldFlag(1500));
    EXPECT_FALSE(IsWorldFlag(4000)); // the session's
    EXPECT_FALSE(IsWorldFlag(9300)); // a player's own
    // Holds the Sword Hunter Badge (Kaniel 2026-10-18): the host's put in a guest's save kept
    // the Cleric Beast from giving the guest its badge.
    EXPECT_FALSE(IsWorldFlag(6645));
    Store store;
    store.Set(1000);
    store.Set(1999);
    store.Set(4000);
    store.Set(9300);
    store.Set(9500);
    const WorldFlags world = CaptureWorldFlags(1000, store.Blocks());
    EXPECT_EQ(world.on, (std::vector<s32>{1000, 1999, 9500}));
    EXPECT_EQ(world.blocks, (std::vector<u32>{1, 4, 9}));
}

TEST(TeamWorld, ChangesMakeTheWorldTheTargetAndLeaveTheRest) {
    Store store;
    store.Set(1000);
    store.Set(1001);
    store.Set(4000); // the session's: left alone
    store.Set(9300); // a player's own: left alone
    WorldFlags target;
    target.blocks = {1, 9, 12};
    target.on = {1001, 1002, 9301, 9600, 12000};
    const auto changes = WorldFlagChanges(1000, store.Blocks(), target);
    // 1000 off, 1002 on, 9600 on; 9301 is a player's own (never set by a world); block 12 is
    // not in this store; block 4 is not in the target.
    EXPECT_EQ(changes,
              (std::vector<std::pair<s32, bool>>{{1000, false}, {1002, true}, {9600, true}}));
    for (const auto& [flag, on] : changes) {
        store.Set(flag, on);
    }
    EXPECT_TRUE(WorldFlagChanges(1000, store.Blocks(), target).empty());
}

// A team world the server kept from before (Kaniel 2026-10-18) still lists the host's "holds the
// Sword Hunter Badge" (6645): a member's save takes none of it, nor clears its own.
TEST(TeamWorld, AnOldWorldsHeldItemFlagsStayThePlayers) {
    Store store{.bytes = {std::vector<u8>(125)}, .ids = {6}}; // flags 6000-6999
    store.Set(6640);
    WorldFlags target;
    target.blocks = {6};
    target.on = {6045, 6645};
    EXPECT_EQ(WorldFlagChanges(1000, store.Blocks(), target),
              (std::vector<std::pair<s32, bool>>{{6045, true}}));
}

// Kaniel's play test 2026-10-18: Kaniel took the Blood Gem Workshop Tool from its chest (item lot
// flag 52411005), and later the chest opened again and gave its NG+ lot. The host's save takes the
// team's world as it loads, and the world it took did not have the chest. A map item pickup goes
// with the character that made it, as its item does: never in a team's world, never set or cleared
// by one, never kept aside with a save's own world.
TEST(TeamWorld, MapPickupsStayTheCharactersOwn) {
    EXPECT_FALSE(IsWorldFlag(52411005)); // the Workshop Tool's chest
    EXPECT_FALSE(IsWorldFlag(50000000));
    EXPECT_FALSE(IsWorldFlag(53000010));
    Store store{.bytes = {std::vector<u8>(125), std::vector<u8>(125)}, .ids = {12410, 52411}};
    store.Set(52411005); // this save opened the chest
    store.Set(12410005);
    EXPECT_EQ(CaptureWorldFlags(1000, store.Blocks()).on, std::vector<s32>{12410005});
    // A team's world without the chest leaves it open; one with another pickup opens nothing.
    WorldFlags target;
    target.blocks = {12410, 52411};
    target.on = {12410005, 52411010};
    EXPECT_TRUE(WorldFlagChanges(1000, store.Blocks(), target).empty());
}

// Kaniel's play test 2026-10-18: a guest's trip gave its game its own saved world back, and the
// opening's flag (12410000, only the team's world had it) was off again. 2026-10-19: the global
// flags came back too, so krolebord and Lisara saw NPCs by their own saves' progress, not the
// team's. A trip takes every block the team's world covers again.
TEST(TeamWorld, ALoadTakesNewBlocksOnceAndAGuestsTripTakesEveryBlockAgain) {
    const std::vector<u32> covered{2, 12100, 12410, 12419, 12420};
    const std::set<u32> taken{2, 12100, 12410, 12419, 12420};
    EXPECT_FALSE(TakesTeamWorldBlock(12410, taken, covered, false));
    EXPECT_TRUE(TakesTeamWorldBlock(12410, {}, covered, false)) << "no earlier load took it";
    EXPECT_TRUE(TakesTeamWorldBlock(12410, taken, covered, true));
    EXPECT_TRUE(TakesTeamWorldBlock(12419, taken, covered, true));
    EXPECT_TRUE(TakesTeamWorldBlock(12420, taken, covered, true)) << "the next map";
    EXPECT_TRUE(TakesTeamWorldBlock(12100, taken, covered, true)) << "the Dream's";
    EXPECT_TRUE(TakesTeamWorldBlock(2, taken, covered, true)) << "global flags (NPCs' steps)";
    // A block the team's world does not cover is never taken, trip or not.
    EXPECT_FALSE(TakesTeamWorldBlock(12411, taken, covered, true));
    EXPECT_FALSE(TakesTeamWorldBlock(12411, {}, covered, false));
}

// Kaniel's play test 2026-10-18: krolebord joined the team and his lamps listed only Cathedral
// Ward. A lamp stays lit only for a character whose lamp list holds it, so the team world's lit
// lamps in the blocks a load took go on the guest's list too (an active flag, 10007810 + map *
// 10000 + lamp * 20, to its lamp entity, map * 10000 + 1950 + lamp).
TEST(TeamWorld, TheTeamWorldsLitLampsInTheTakenBlocksGoOnTheList) {
    WorldFlags team;
    team.blocks = {12207, 12410, 12417, 12427};
    team.on = {12207810, 12410000, 12417810, 12417811, 12417830, 12427810};
    EXPECT_EQ(TeamWorldLamps(1000, std::vector<u32>{12410, 12417, 12427}, team),
              (std::vector<s32>{2411950, 2411951, 2421950}))
        << "not the flag after a lamp's, not a story flag, not a block this load did not take";
    EXPECT_TRUE(TeamWorldLamps(1000, std::vector<u32>{}, team).empty());
}

// The guest's join reload and its trips give its game its own saved world back (the rig's
// team-world-lamps: the host's lamp, 12707810, went out at the guest's join), so every guest load
// sets the team world's lamp flags again: a lamp's active flag and the one after, on in the team's
// world and off here, in every block. Never one off.
TEST(TeamWorld, AGuestsLoadSetsTheTeamWorldsLampFlagsItLacks) {
    WorldFlags team;
    team.blocks = {12410, 12417, 12707};
    team.on = {12410000, 12417810, 12417811, 12707810, 12707811, 12707812};
    const std::set<s32> here{12417810, 12417811, 12707830};
    EXPECT_EQ(TeamLampFlagsToSet(team, [&](s32 flag) { return here.contains(flag); }),
              (std::vector<s32>{12707810, 12707811}))
        << "not a story flag, not one already on, not a flag two after the lamp's";
}

TEST(TeamWorld, ThePersonalWorldFileRoundTripsAndACutOneIsNone) {
    WorldFlags world;
    world.blocks = {1, 9};
    world.on = {1000, 9500};
    const std::string text = FormatWorldFlags(world);
    EXPECT_EQ(ParseWorldFlags(text), world);
    EXPECT_FALSE(ParseWorldFlags(text.substr(0, text.size() - 4))); // no "end"
    EXPECT_FALSE(ParseWorldFlags(""));
    EXPECT_FALSE(ParseWorldFlags("blocks\n1\non\nx\nend\n"));
}

// Kaniel's playtest 2026-10-17: the night fell only in the host's world (Vicar Amelia's skull). A
// guest away from the cutscene takes the team world's time of day (9800 evening, 9801 night,
// 9802 blood moon), never turning one back.
TEST(TeamWorld, AGuestTakesTheTeamsTimeOfDay) {
    WorldFlags team;
    team.blocks = {9};
    team.on = {9301, 9800, 9801};
    EXPECT_EQ(TimeOfDayToFollow(team, [](s32) { return false; }), (std::vector<s32>{9800, 9801}));
    EXPECT_EQ(TimeOfDayToFollow(team, [](s32 flag) { return flag == 9800; }),
              (std::vector<s32>{9801}));
    EXPECT_TRUE(TimeOfDayToFollow(team, [](s32 flag) { return flag <= 9801; }).empty());
    // The guest's blood moon stays: the team's world is behind, not this one's to undo.
    team.on = {9800};
    EXPECT_TRUE(TimeOfDayToFollow(team, [](s32 flag) { return flag >= 9800; }).empty());
    EXPECT_TRUE(TimeOfDayToFollow(WorldFlags{}, [](s32) { return false; }).empty());
}

// Kaniel 2026-10-20: NPCs showed up for guests on maps without the host. A guest away from the host
// takes the story steps the host's reports change (quest and NPC flags), and only those: its own
// game's changes, the maps' flags, items, the time of day and the flags with rules of their own
// stay as they are.
TEST(TeamWorld, AGuestFollowsTheStoryStepsTheHostChanged) {
    WorldFlags before;
    before.blocks = {1, 9, 1240};
    before.on = {1100, 1205, 9401};
    WorldFlags after = before;
    after.on = {1100, 1300, 6630, 9401, 9403, 9801, 12404000, 50000100};
    EXPECT_EQ(TeamStoryToFollow(before, after),
              (std::vector<std::pair<s32, bool>>{{1205, false}, {1300, true}}));
    EXPECT_TRUE(TeamStoryToFollow(before, before).empty());
    // Nothing taken yet, or the blocks changed (another host's world): the loads take it.
    EXPECT_TRUE(TeamStoryToFollow(WorldFlags{}, after).empty());
    WorldFlags other = after;
    other.blocks = {1, 9};
    EXPECT_TRUE(TeamStoryToFollow(before, other).empty());
}

// Phase 6: the team join without signs or a bell. The host opens its session once it stands in
// its world and says so once (the server keeps it for its guests); a guest joins the session the
// server sent when it stands alone in its world.
TeamJoin::Frame HostFrame(s32 matching_state, bool ready) {
    return {.host = true, .matching_state = matching_state, .ready = ready};
}
TeamJoin::Frame GuestFrame(s32 matching_state, bool ready, bool session_known) {
    return {.host = false,
            .matching_state = matching_state,
            .ready = ready,
            .session_known = session_known};
}

TEST(TeamJoin, TheHostOpensItsSessionWhenReadyAndAlone) {
    TeamJoin join;
    EXPECT_EQ(join.Tick(HostFrame(0, false)), TeamJoin::Action::None);
    EXPECT_EQ(join.Tick(HostFrame(0, true)), TeamJoin::Action::CreateSession);
    // Opening (the game's own states on the way): nothing more until it hosts.
    EXPECT_EQ(join.Tick(HostFrame(1, true)), TeamJoin::Action::None);
}

TEST(TeamJoin, TheHostSaysItsSessionOnceAndThatItClosed) {
    TeamJoin join;
    EXPECT_EQ(join.Tick(HostFrame(MatchingStateHosting, false)), TeamJoin::Action::Publish);
    EXPECT_EQ(join.Tick(HostFrame(MatchingStateHosting, false)), TeamJoin::Action::None);
    // A member still out of the session (the controller's room for more): the server has it.
    EXPECT_EQ(join.Tick(HostFrame(MatchingStateHosting, true)), TeamJoin::Action::None);
    EXPECT_EQ(join.Tick(HostFrame(0, false)), TeamJoin::Action::PublishClosed);
    EXPECT_EQ(join.Tick(HostFrame(0, false)), TeamJoin::Action::None);
    EXPECT_EQ(join.Tick(HostFrame(0, true)), TeamJoin::Action::CreateSession);
    EXPECT_EQ(join.Tick(HostFrame(MatchingStateHosting, false)), TeamJoin::Action::Publish);
}

TEST(TeamJoin, AGuestJoinsTheHostsSessionWhenReadyAndAlone) {
    TeamJoin join;
    EXPECT_EQ(join.Tick(GuestFrame(0, true, false)), TeamJoin::Action::None);
    EXPECT_EQ(join.Tick(GuestFrame(0, false, true)), TeamJoin::Action::None);
    EXPECT_EQ(join.Tick(GuestFrame(0, true, true)), TeamJoin::Action::Join);
    // Joining or in the session: nothing.
    EXPECT_EQ(join.Tick(GuestFrame(MatchingStateSummonedGuest, true, true)),
              TeamJoin::Action::None);
    // A guest never opens a session or says one.
    EXPECT_EQ(join.Tick(GuestFrame(MatchingStateHosting, true, true)), TeamJoin::Action::None);
}

// Phase 7: only the server ends a session, so a guest whose host's game died is left in a session
// of one. After a few seconds alone it leaves it (the room says the others went), so it can join
// the session its host opens again (rig crashes/host-crash: it waited in the old one forever).
TEST(TeamJoin, AGuestAloneInItsSessionLeavesIt) {
    TeamJoin join;
    TeamJoin::Frame frame = GuestFrame(MatchingStateSummonedGuest, true, false);
    frame.session_members = 2;
    frame.now_ms = 1'000;
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::None);
    frame.session_members = 1;
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::None);
    frame.now_ms = 1'000 + TeamJoin::AloneLeaveMs - 1;
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::None);
    frame.now_ms = 1'000 + TeamJoin::AloneLeaveMs;
    // Down or dead, it waits: its death and respawn run in the session (rig party-death: leaving
    // mid-death left it dead for good).
    frame.standing = false;
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::None);
    frame.standing = true;
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::LeaveAlone);
    // A host alone keeps its session open for its members.
    TeamJoin host;
    TeamJoin::Frame hosting = HostFrame(MatchingStateHosting, true);
    hosting.session_members = 1;
    hosting.now_ms = 1'000'000;
    EXPECT_EQ(host.Tick(hosting), TeamJoin::Action::Publish);
    hosting.now_ms += 60'000;
    EXPECT_EQ(host.Tick(hosting), TeamJoin::Action::None);
}

// Phase 7, host migration: the server passed the room to this guest (its host's game went): it
// takes the session host's role in place, once, and then says its session like any host.
TEST(TeamJoin, AGuestGivenTheRoomTakesTheHostsRole) {
    TeamJoin join;
    TeamJoin::Frame frame = GuestFrame(MatchingStateSummonedGuest, true, true);
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::None);
    frame.room_owner = true;
    // Alone in the room (a party of two): it stays a guest and runs its map alone; the host's
    // next run hosts again and it joins that (rig crashes/host-crash, 2026-10-15: taking over
    // there made the returning host a guest, whose game crashed at eboot+0x13cffba).
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::None);
    frame.room_has_others = true;
    EXPECT_EQ(join.Tick(frame), TeamJoin::Action::TakeHost);
    // Hosting now (the role and the state moved): it says its session.
    EXPECT_EQ(join.Tick(HostFrame(MatchingStateHosting, true)), TeamJoin::Action::Publish);
    // Not in a session, or not a guest's: nothing to take.
    TeamJoin alone;
    TeamJoin::Frame out = GuestFrame(0, true, false);
    out.room_owner = true;
    EXPECT_EQ(alone.Tick(out), TeamJoin::Action::None);
}

// tool that heals, 2050).
TEST(BellItems, TheCoopBellsAndNothingElse) {
    for (const u32 bell : {200u, 205u, 225u, 4312u}) {
        EXPECT_TRUE(IsBellGoods(bell)) << bell;
    }
    for (const u32 other : {2050u, 111u, 201u, 1000u}) {
        EXPECT_FALSE(IsBellGoods(other)) << other;
    }
    EXPECT_EQ(BellGoods.size(), 4u);
}

// Kaniel 2026-10-18 (krolebord's Sword Hunter Badge): in the join's state a guest's key items are
// a list of the game's own for phantoms; at the guest's first trip its own list comes back and the
// game drops that one, with the Cleric Beast's badge in it. What it gained there comes back.
TEST(PhantomKeyItems, WhatTheGuestGainedComesBack) {
    const std::vector<HeldGoods> first_look{{4000, 1}};
    const std::vector<HeldGoods> last_look{{4000, 1}, {4114, 1}, {4001, 2}, {200, 1}};
    const std::vector<HeldGoods> home{{4000, 1}, {4001, 1}};
    // The badge gained; one more Gold Pendant than home has (two gained); the key it came with
    // and home holds stays as it is; a bell never comes back (RemoveBellItems).
    EXPECT_EQ(PhantomKeyItemsToGiveBack(first_look, last_look, home),
              (std::vector<HeldGoods>{{4114, 1}, {4001, 1}}));
    // Home holds them already (a second trip): nothing twice.
    EXPECT_TRUE(PhantomKeyItemsToGiveBack(first_look, last_look, last_look).empty());
    // Nothing gained: nothing to give, whatever home lacks.
    EXPECT_TRUE(PhantomKeyItemsToGiveBack(last_look, last_look, {}).empty());
}

// Kaniel's play test 2026-10-19: krolebord's Saw Hunter Badge (4103) and Workshop Tool (4110) were
// missing all session in the join's state; the phantom's list gets the guest's own key items.
TEST(PhantomKeyItems, TheGuestsOwnKeyItemsGoInThePhantomsList) {
    const std::vector<HeldGoods> own{{4103, 1}, {4110, 1}, {4000, 1}, {4001, 2}, {200, 1}};
    const std::vector<HeldGoods> phantom{{4000, 1}, {4001, 1}};
    // Each it lacks, as many as its own; one it has already stays; never a bell.
    EXPECT_EQ(OwnKeyItemsForPhantom(own, phantom),
              (std::vector<HeldGoods>{{4103, 1}, {4110, 1}, {4001, 1}}));
    EXPECT_TRUE(OwnKeyItemsForPhantom(own, own).empty()) << "nothing twice";
    EXPECT_TRUE(OwnKeyItemsForPhantom({}, phantom).empty()) << "no own look: nothing";
    // Back home after a trip: what was put in comes back from home, never twice.
    std::vector<HeldGoods> last_look = phantom;
    for (const HeldGoods& put : OwnKeyItemsForPhantom(own, phantom)) {
        last_look.push_back(put);
    }
    EXPECT_TRUE(PhantomKeyItemsToGiveBack(phantom, last_look, own).empty());
}

// Rig 2026-10-14 (host-crash-apart, team-world): the host's game gone, the guest's game set 4047
// ("a player on the way out") and, with no summon reload to clear it, never joined again.
TEST(LeavingFlag, ClearedOnceAGuestStandsAloneAWhile) {
    LeavingFlag leaving;
    EXPECT_FALSE(leaving.Clear(true, true, 0)) << "in the session: the game's own";
    EXPECT_FALSE(leaving.Clear(true, false, 1000));
    EXPECT_FALSE(leaving.Clear(true, false, 1000 + LeavingFlag::ClearAfterMs - 1));
    EXPECT_TRUE(leaving.Clear(true, false, 1000 + LeavingFlag::ClearAfterMs));
    EXPECT_FALSE(leaving.Clear(false, false, 9000)) << "off: nothing to clear";
    EXPECT_FALSE(leaving.Clear(true, false, 10'000)) << "on again: its own wait again";
}

TEST(PhantomHome, APhantomAloneAWhileIsAPlayerAgainNeverWhileItJoins) {
    PhantomHome home;
    EXPECT_FALSE(home.Due(true, false, 0)) << "in a session (or joining): the game's phantom";
    EXPECT_FALSE(home.Due(true, true, 1000));
    EXPECT_FALSE(home.Due(true, false, 2000)) << "a join: the wait starts over";
    EXPECT_FALSE(home.Due(true, true, 3000));
    EXPECT_FALSE(home.Due(true, true, 3000 + PhantomHome::ReturnAfterMs - 1));
    EXPECT_TRUE(home.Due(true, true, 3000 + PhantomHome::ReturnAfterMs));
    EXPECT_FALSE(home.Due(false, true, 20'000)) << "a player already";
}

// Kaniel 2026-10-14: a team loading screen as in League of Legends: each hunter's progress, and
// the hunt opens for everyone at once.
using LoadStage = bbcoop::LoadProgress::Stage;
bbcoop::LoadProgress OtherLoad(const std::string& member, LoadStage stage, u32 percent) {
    bbcoop::LoadProgress load;
    load.set_member(member);
    load.set_stage(stage);
    load.set_percent(percent);
    return load;
}
const std::vector<std::pair<std::string, std::string>> Team2{{"h", "Host"}, {"g", "Guest"}};

TEST(TeamLoad, NotUpBeforeTheTeamsLoad) {
    TeamLoad load;
    EXPECT_FALSE(load.Tick(Team2, "g", 0).up);
}

TEST(TeamLoad, ShowsEveryHunterAndHoldsUntilAllAreIn) {
    TeamLoad load;
    load.Start(1000);
    auto view = load.Tick(Team2, "g", 1000);
    ASSERT_TRUE(view.up);
    ASSERT_EQ(view.rows.size(), 2u);
    EXPECT_EQ(view.rows[0].name, "Host");
    EXPECT_EQ(view.rows[1].name, "Guest");
    EXPECT_TRUE(view.rows[1].self);
    EXPECT_EQ(view.rows[1].stage, bbcoop::LoadProgress::LOADING);
    EXPECT_EQ(view.rows[0].stage, bbcoop::LoadProgress::WAITING)
        << "nothing heard from the host yet";
    // The own bar grows while it loads, never past the load's share.
    EXPECT_GT(load.Tick(Team2, "g", 1000 + TeamLoad::LoadingGuessMs / 2).rows[1].percent, 0u);
    EXPECT_LE(load.Tick(Team2, "g", 1000 + 3 * TeamLoad::LoadingGuessMs).rows[1].percent,
              TeamLoad::LoadedPercent);
    load.Take(OtherLoad("h", bbcoop::LoadProgress::READY, 100), 2000);
    load.SetStage(bbcoop::LoadProgress::JOINING, 3000);
    view = load.Tick(Team2, "g", 3000);
    EXPECT_TRUE(view.up);
    EXPECT_EQ(view.rows[0].percent, 100u);
    load.SetStage(bbcoop::LoadProgress::READY, 4000);
    EXPECT_FALSE(load.Tick(Team2, "g", 4000).up) << "everyone in: it opens";
    EXPECT_FALSE(load.Tick(Team2, "g", 5000).up) << "and stays open";
}

TEST(TeamLoad, OpensAnywayWhenSomeoneNeverComes) {
    TeamLoad load;
    load.Start(0);
    load.SetStage(bbcoop::LoadProgress::READY, 100);
    EXPECT_TRUE(load.Tick(Team2, "h", 100).up) << "the guest is still loading";
    EXPECT_TRUE(load.Tick(Team2, "h", TeamLoad::LimitMs - 1).up);
    EXPECT_FALSE(load.Tick(Team2, "h", TeamLoad::LimitMs).up);
}

// Kaniel 2026-10-15: the loading screen stays until every guest has fully joined, its character
// already spawned: a game is ready only once each other hunter's character stands in its world.
TEST(TeamLoad, ReadyOnlyOnceEveryHuntersCharacterIsInItsWorld) {
    using P = bbcoop::LoadProgress;
    EXPECT_EQ(TeamLoad::OwnStage(true, false, 0, 1), P::LOADING);
    EXPECT_EQ(TeamLoad::OwnStage(true, true, 1, 1), P::LOADING) << "its world still loading";
    EXPECT_EQ(TeamLoad::OwnStage(false, false, 1, 1), P::JOINING) << "not in the session yet";
    EXPECT_EQ(TeamLoad::OwnStage(false, true, 0, 1), P::JOINING)
        << "in the session, the other hunter's character not spawned yet";
    EXPECT_EQ(TeamLoad::OwnStage(false, true, 1, 2), P::JOINING) << "one of two still missing";
    EXPECT_EQ(TeamLoad::OwnStage(false, true, 2, 2), P::READY);
    EXPECT_EQ(TeamLoad::OwnStage(false, true, 3, 2), P::READY);
    EXPECT_EQ(TeamLoad::OwnStage(false, true, 0, 0), P::READY) << "nobody else coming";
}

// Release thread 2026-10-16 (the dog scenario): the host travelled to the Dream before it saw the
// guest's character, never said ready, and the guest stayed covered (its attacks missed) until the
// host came back. A member on another map is not waited for, and ready stays ready.
TEST(TeamLoad, DoesNotWaitForMembersOnAnotherMap) {
    const std::vector<std::pair<std::string, std::string>> team3{
        {"h", "Host"}, {"g", "Guest"}, {"g2", "Guest 2"}};
    using Maps = std::vector<std::pair<std::string, u32>>;
    EXPECT_EQ(TeamLoad::OthersHere(team3, "h", Maps{}, 0x18010000), 2u) << "nothing known: all";
    EXPECT_EQ(
        TeamLoad::OthersHere(team3, "h", Maps{{"h", 0x18010000}, {"g", 0x18010000}}, 0x18010000),
        2u)
        << "g2 not on the board yet: waited for";
    EXPECT_EQ(TeamLoad::OthersHere(team3, "h", Maps{{"g", 0x15000000}, {"g2", 0}}, 0x18010000), 1u)
        << "g in the Dream: not waited for; g2 loading (map 0): waited for";
    EXPECT_EQ(
        TeamLoad::OthersHere(team3, "g", Maps{{"h", 0x15000000}, {"g2", 0x15000000}}, 0x18010000),
        0u);
}

// Kaniel 2026-10-16: the screen opened as the others connected, their characters only starting
// to appear. Ready holds for ReadySettle::SettleMs first: everyone stands in the world.
TEST(TeamLoad, ReadyOnlyAfterTheOthersHaveStoodThereAWhile) {
    using P = bbcoop::LoadProgress;
    ReadySettle settle;
    EXPECT_EQ(settle.Settle(P::JOINING, 1, 0), P::JOINING);
    EXPECT_EQ(settle.Settle(P::READY, 1, 1000), P::JOINING) << "just seen: not yet";
    EXPECT_EQ(settle.Settle(P::READY, 1, 1000 + ReadySettle::SettleMs - 1), P::JOINING);
    EXPECT_EQ(settle.Settle(P::READY, 1, 1000 + ReadySettle::SettleMs), P::READY);
    // Gone again for longer than a flicker before then: it starts over.
    ReadySettle again;
    again.Settle(P::READY, 2, 0);
    again.Settle(P::JOINING, 2, ReadySettle::FlickerMs + 100);
    EXPECT_EQ(again.Settle(P::READY, 2, ReadySettle::SettleMs), P::JOINING);
    // A flicker (a copy missing for a frame or two) does not.
    ReadySettle flicker;
    flicker.Settle(P::READY, 1, 0);
    flicker.Settle(P::JOINING, 1, 500);
    EXPECT_EQ(flicker.Settle(P::READY, 1, ReadySettle::SettleMs), P::READY);
    // A cutscene playing: not ready, however long the others have stood there.
    ReadySettle cutscene;
    cutscene.Settle(P::READY, 1, 0);
    EXPECT_EQ(cutscene.Settle(P::READY, 1, ReadySettle::SettleMs, true), P::JOINING);
    EXPECT_EQ(cutscene.Settle(P::READY, 0, ReadySettle::SettleMs, true), P::JOINING);
    EXPECT_EQ(cutscene.Settle(P::READY, 1, ReadySettle::SettleMs + 100), P::JOINING)
        << "over: it settles again";
    // Nobody else coming: ready at once; loading stays loading.
    ReadySettle alone;
    EXPECT_EQ(alone.Settle(P::READY, 0, 0), P::READY);
    EXPECT_EQ(alone.Settle(P::LOADING, 1, 0), P::LOADING);
}

TEST(TeamLoad, ReadyStaysReady) {
    TeamLoad load;
    load.Start(0);
    load.SetStage(bbcoop::LoadProgress::READY, 100);
    load.SetStage(bbcoop::LoadProgress::JOINING, 200);
    load.Take(OtherLoad("h", bbcoop::LoadProgress::READY, 100), 300);
    EXPECT_FALSE(load.Tick(Team2, "g", 300).up)
        << "the host left, this game no longer sees it: still ready, the screen opens";
}

// Kaniel 2026-10-15: the opening cutscene under the team's loading screen could not be skipped:
// it is skipped by itself, the game's own skip (Options) pressed for the player.
TEST(CutsceneSkip, PressesOptionsWhileACutscenePlaysUnderTheScreen) {
    using S = CutsceneSkip;
    CutsceneSkip skip;
    EXPECT_FALSE(skip.Press(false, S::PlayStep, 1000, 1000)) << "no screen: the player's own";
    EXPECT_FALSE(skip.Press(true, -1, 0, 1000)) << "no cutscene";
    EXPECT_FALSE(skip.Press(true, S::PrologueStep - 1, 1000, 1000)) << "still loading";
    EXPECT_TRUE(skip.Press(true, S::PrologueStep, 1000, 1000)) << "its prologue: pressed";
    EXPECT_FALSE(skip.Press(true, S::PrologueStep, 1000, 1000 + S::HoldMs)) << "released";
    skip = {};
    EXPECT_TRUE(skip.Press(true, S::PlayStep, 2000, 2000)) << "it plays: pressed";
    EXPECT_TRUE(skip.Press(true, S::PlayStep, 2000, 2000 + S::HoldMs - 10));
    EXPECT_FALSE(skip.Press(true, S::PlayStep, 2000, 2000 + S::HoldMs)) << "released";
    EXPECT_FALSE(skip.Press(true, S::PlayStep, 2000, 2000 + S::EveryMs - 1));
    EXPECT_TRUE(skip.Press(true, S::PlayStep, 2000, 2000 + S::EveryMs)) << "pressed again";
    EXPECT_EQ(skip.Presses(), 2);
    EXPECT_FALSE(skip.Press(true, S::PlayStep, 2000, 2000 + S::LongestMs + 1))
        << "stuck in its playing step: left alone";
    EXPECT_FALSE(skip.Press(true, S::PlayStep + 1, 5000, 5000)) << "it ends by itself";
}

TEST(CutsceneSkip, GivesUpAfterSoManyPresses) {
    using S = CutsceneSkip;
    CutsceneSkip skip;
    s64 now = 0;
    int pressed = 0;
    for (int i = 0; i < 4 * S::MostPresses; ++i, now += S::EveryMs) {
        pressed += skip.Press(true, S::PlayStep, now, now) ? 1 : 0;
    }
    EXPECT_EQ(pressed, S::MostPresses) << "an unskippable cutscene is left alone";
}

// Kaniel 2026-10-17: when every party member on the map sees the same cutscene, nobody skips it
// for the others: Options is a vote ("1/3 want to skip"), and the cutscene is skipped once every
// member watching it voted.
CutsceneVote::Frame Watching(bool options, u32 others, u32 others_skip, s64 now_ms) {
    return {.watching = true,
            .options = options,
            .others_watching = others,
            .others_skip = others_skip,
            .now_ms = now_ms};
}

TEST(CutsceneVote, AloneOptionsSkipsAsBefore) {
    using Pad = CutsceneVote::Pad;
    CutsceneVote vote;
    EXPECT_EQ(vote.Tick({.watching = false, .options = true, .now_ms = 0}), Pad::Leave)
        << "no cutscene: Options is the menu";
    EXPECT_EQ(vote.Tick(Watching(false, 0, 0, 100)), Pad::Hide) << "watching: no stray skip";
    EXPECT_EQ(vote.Own(), CutsceneVote::Watching);
    EXPECT_EQ(vote.Tick(Watching(true, 0, 0, 200)), Pad::Press) << "alone: its own vote skips";
    EXPECT_FALSE(vote.Shown()) << "alone: no count on the screen";
}

TEST(CutsceneVote, SkipsOnlyOnceEveryoneWatchingVoted) {
    using Pad = CutsceneVote::Pad;
    using V = CutsceneVote;
    CutsceneVote vote;
    EXPECT_EQ(vote.Tick(Watching(false, 2, 0, 0)), Pad::Hide);
    EXPECT_FALSE(vote.Shown()) << "nobody wants to skip yet";
    EXPECT_EQ(vote.Tick(Watching(true, 2, 0, 100)), Pad::Hide) << "1 of 3: held back";
    EXPECT_EQ(vote.Own(), V::WantsSkip);
    EXPECT_TRUE(vote.Shown());
    EXPECT_EQ(vote.Want(), 1u);
    EXPECT_EQ(vote.Watchers(), 3u);
    EXPECT_EQ(vote.Tick(Watching(false, 2, 1, 200)), Pad::Hide) << "2 of 3, its vote stands";
    EXPECT_EQ(vote.Want(), 2u);
    EXPECT_EQ(vote.Tick(Watching(false, 2, 2, 300)), Pad::Press) << "3 of 3: skipped";
    EXPECT_EQ(vote.Tick(Watching(false, 2, 2, 300 + V::HoldMs)), Pad::Hide) << "released";
    EXPECT_EQ(vote.Tick(Watching(false, 2, 2, 300 + V::EveryMs)), Pad::Press) << "again";
    EXPECT_EQ(vote.Tick({.watching = false, .now_ms = 2000}), Pad::Leave) << "it ended";
    EXPECT_EQ(vote.Own(), V::NotWatching);
    EXPECT_EQ(vote.Tick(Watching(false, 2, 2, 3000)), Pad::Hide)
        << "the next cutscene: its own vote starts over";
}

TEST(CutsceneVote, AMemberWhoseCutsceneEndedNoLongerCounts) {
    using Pad = CutsceneVote::Pad;
    CutsceneVote vote;
    EXPECT_EQ(vote.Tick(Watching(true, 1, 0, 0)), Pad::Hide) << "1 of 2";
    EXPECT_EQ(vote.Tick(Watching(false, 0, 0, 100)), Pad::Press)
        << "the other's ended (or it left): everyone left watching voted";
}

TEST(TeamLoad, SaysItsOwnProgressWhenItChanges) {
    TeamLoad load;
    EXPECT_FALSE(load.ToSend(0).has_value()) << "not loading";
    load.Start(0);
    const auto first = load.ToSend(0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->stage(), bbcoop::LoadProgress::LOADING);
    EXPECT_FALSE(load.ToSend(100).has_value()) << "nothing new";
    load.SetStage(bbcoop::LoadProgress::JOINING, 200);
    const auto joining = load.ToSend(200);
    ASSERT_TRUE(joining.has_value());
    EXPECT_EQ(joining->stage(), bbcoop::LoadProgress::JOINING);
    EXPECT_GE(joining->percent(), TeamLoad::LoadedPercent);
}

// Kaniel 2026-10-14: the hunt begins only once every member is ready (online and Ready); a started
// team's leader back from a crash goes in at once.
TEST(TeamGate, TheHuntBeginsOnlyWhenEveryoneIsReady) {
    bbcoop::LobbyState lobby = InTeam("a");
    const auto add = [&](const std::string& member, bool ready, bbcoop::LobbyPlayer::State state) {
        bbcoop::TeamMember& m = *lobby.mutable_team()->add_members();
        m.set_member(member);
        m.set_ready(ready);
        m.set_state(state);
    };
    add("a", false, bbcoop::LobbyPlayer::TITLE);
    add("b", true, bbcoop::LobbyPlayer::TITLE);
    add("c", false, bbcoop::LobbyPlayer::TITLE);
    EXPECT_FALSE(TeamCanBegin(lobby, "a")) << "c is not ready";
    lobby.mutable_team()->mutable_members(2)->set_ready(true);
    EXPECT_TRUE(TeamCanBegin(lobby, "a")) << "the leader's own Ready comes with its Begin";
    lobby.mutable_team()->mutable_members(2)->set_state(bbcoop::LobbyPlayer::OFFLINE);
    EXPECT_FALSE(TeamCanBegin(lobby, "a")) << "c went offline";
    lobby.mutable_team()->set_started(true);
    EXPECT_TRUE(TeamCanBegin(lobby, "a")) << "the team plays already";
    EXPECT_FALSE(TeamCanBegin(bbcoop::LobbyState{}, "a")) << "no team";
}

// Kaniel 2026-10-15: between two guests, one guest's actions played on the other's own character
// (a pick-up's gesture, a backstab's teleport): both joined with the same player slot (7), so each
// took the other's packets as its own. Each guest joins with its own: 7 for the first, then down.
TEST(TeamGate, EachGuestJoinsWithItsOwnPlayerSlot) {
    bbcoop::LobbyState lobby = InTeam("a");
    for (const char* member : {"a", "b", "c", "d"}) {
        lobby.mutable_team()->add_members()->set_member(member);
    }
    EXPECT_EQ(TeamGuestSign(lobby, "b"), 7u);
    EXPECT_EQ(TeamGuestSign(lobby, "c"), 6u);
    EXPECT_EQ(TeamGuestSign(lobby, "d"), 5u);
    EXPECT_EQ(TeamGuestSign(lobby, "x"), 7u) << "not in the team: the game's own";
    EXPECT_EQ(TeamGuestSign(bbcoop::LobbyState{}, "b"), 7u);
}

// Phase 7: the server says each guest's slot (TeamMember.sign), kept while others come and go:
// b keeps 7 after c (6) left, and d keeps 5, whatever their order in the list.
TEST(TeamGate, TheServerSaysEachGuestsPlayerSlot) {
    bbcoop::LobbyState lobby = InTeam("a");
    const auto add = [&](const char* member, u32 sign) {
        bbcoop::TeamMember& m = *lobby.mutable_team()->add_members();
        m.set_member(member);
        m.set_sign(sign);
    };
    add("a", 0);
    add("d", 5);
    add("b", 7);
    EXPECT_EQ(TeamGuestSign(lobby, "d"), 5u);
    EXPECT_EQ(TeamGuestSign(lobby, "b"), 7u);
    // Phase 7, host migration: b hosts the session now (its game took over); the leader, back,
    // joins as a guest on the slot b left (rig 2026-10-15: on 7 too, the two games took each
    // other's place and the leader's game left the session).
    EXPECT_EQ(TeamGuestSign(lobby, "a", "b"), 7u);
    EXPECT_EQ(TeamGuestSign(lobby, "a", "d"), 5u);
    EXPECT_EQ(TeamGuestSign(lobby, "d", "b"), 5u);
}

// Kaniel 2026-10-15: Lisara back in after a drop still had the loading bars on top: they waited
// for a member that had gone (offline) or stayed at the title. The screen shows the members coming.
TEST(TeamLoad, ShowsOnlyTheMembersComing) {
    bbcoop::LobbyState lobby = InTeam("a");
    const auto add = [&](const char* member, bool ready, bbcoop::LobbyPlayer::State state) {
        bbcoop::TeamMember& m = *lobby.mutable_team()->add_members();
        m.set_member(member);
        m.set_name(std::string("Name ") + member);
        m.set_ready(ready);
        m.set_state(state);
    };
    add("a", true, bbcoop::LobbyPlayer::PLAYING);
    add("b", true, bbcoop::LobbyPlayer::TITLE);   // ready: on its way in
    add("c", false, bbcoop::LobbyPlayer::TITLE);  // at the title, not coming
    add("d", true, bbcoop::LobbyPlayer::OFFLINE); // gone
    const auto members = TeamLoadMembers(lobby, "c");
    ASSERT_EQ(members.size(), 3u) << "this game's own place always shows";
    EXPECT_EQ(members[0].first, "a");
    EXPECT_EQ(members[1].first, "b");
    EXPECT_EQ(members[2].first, "c");
    EXPECT_EQ(members[1].second, "Name b");
    EXPECT_EQ(TeamLoadMembers(lobby, "a").size(), 2u);
    EXPECT_TRUE(TeamLoadMembers(bbcoop::LobbyState{}, "a").empty());
}

} // namespace

// Phase 7: a game the server refuses (another build) says why on the team screen.
TEST(TeamLobby, ARefusedLinkIsShownOnTheTeamScreen) {
    EXPECT_EQ(LinkRefusalNotice(""), "");
    // The screen has one short line (bbcoop_lobby::notice): the reason, not the builds.
    const std::string notice = LinkRefusalNotice(
        "another build: everyone needs the same release (this game runs b, the others run a)");
    EXPECT_EQ(notice, "The co-op server refused this game: another build: everyone needs the same "
                      "release");
    EXPECT_LT(notice.size(), 96u);
}

namespace {

bbcoop::LobbyState WithMember(bbcoop::LobbyState state, const std::string& member, bool ready) {
    bbcoop::TeamMember& m = *state.mutable_team()->add_members();
    m.set_member(member);
    m.set_ready(ready);
    return state;
}

} // namespace

// Kaniel's playtest 2026-10-16: a guest's lever pull moved the other guest to the lever. The host's
// game gives a joining member its player number (0x01A6C160: its counter, moved on only past a
// number in use), and a member's number counts as used only once its character data came; the
// team join brings both guests in before that, so both got 2. The counter goes on past each
// number it hands out.
namespace {
// The game's hand-out: the counter, unless in use (then the next free one, the counter moved).
s32 HandOut(s32& counter, const std::vector<s32>& used) {
    while (std::ranges::find(used, counter) != used.end()) {
        counter = NextPlayerNumber(counter);
    }
    return counter;
}
} // namespace

TEST(TeamJoin, EachGuestJoiningAtOnceGetsItsOwnPlayerNumber) {
    s32 counter = 2;
    const std::vector<s32> used{1}; // the host; the guests' data not here yet
    const s32 first = HandOut(counter, used);
    counter = NextPlayerNumber(first); // the hook after the hand-out
    const s32 second = HandOut(counter, used);
    EXPECT_EQ(first, 2);
    EXPECT_EQ(second, 3);
    // As the game wraps its counter.
    EXPECT_EQ(NextPlayerNumber(0x7FFD), 0x7FFE);
    EXPECT_EQ(NextPlayerNumber(0x7FFE), 2);
}

// Kaniel 2026-10-16, the team menu in game: a save playing in a team whose lobby says it is out of
// that team (the leader removed it, or it left) leaves the team's session where it stands.
TEST(TeamPlay, OutOfTheTeamItPlaysInItLeaves) {
    const bbcoop::LobbyState in = WithMember(InTeam("leader"), "me", true);
    EXPECT_EQ(TeamPlayStepOf(&in, "world:w-t1", nullptr), TeamPlayStep::None);
    const bbcoop::LobbyState out;
    EXPECT_EQ(TeamPlayStepOf(&out, "world:w-t1", nullptr), TeamPlayStep::Leave);
    // In another team now (it left and joined another): out of this one all the same.
    const bbcoop::LobbyState other = WithMember(InTeam("leader", "t2"), "me", false);
    EXPECT_EQ(TeamPlayStepOf(&other, "world:w-t1", nullptr), TeamPlayStep::Leave);
    // No word from the lobby yet, or a save playing alone: nothing to leave.
    EXPECT_EQ(TeamPlayStepOf(nullptr, "world:w-t1", nullptr), TeamPlayStep::None);
    EXPECT_EQ(TeamPlayStepOf(&out, "", nullptr), TeamPlayStep::None);
}

// A save playing alone takes its team's party when the team's Start for it comes (it joined the
// hunt from the menu), never an old Start of a team it is no longer ready in.
TEST(TeamPlay, AStartWhileAloneTakesTheTeamsParty) {
    const bbcoop::TeamStart start = StartOf("leader", 2);
    const bbcoop::LobbyState ready = WithMember(InTeam("leader"), "me", true);
    EXPECT_EQ(TeamPlayStepOf(&ready, "", &start, "me"), TeamPlayStep::Adopt);
    EXPECT_EQ(TeamPlayStepOf(&ready, "", nullptr, "me"), TeamPlayStep::None);
    // Played alone by choice (not ready), or out of that team: the Start is an old one.
    const bbcoop::LobbyState not_ready = WithMember(InTeam("leader"), "me", false);
    EXPECT_EQ(TeamPlayStepOf(&not_ready, "", &start, "me"), TeamPlayStep::None);
    const bbcoop::LobbyState out;
    EXPECT_EQ(TeamPlayStepOf(&out, "", &start, "me"), TeamPlayStep::None);
    const bbcoop::LobbyState other = WithMember(InTeam("leader", "t2"), "me", true);
    EXPECT_EQ(TeamPlayStepOf(&other, "", &start, "me"), TeamPlayStep::None);
    // Already in that party: nothing more.
    EXPECT_EQ(TeamPlayStepOf(&ready, "world:w-t1", &start, "me"), TeamPlayStep::None);
}

// The menu's "Join the hunt" in game: an invitation accepted first (when in no team), then this
// save ready with its slot and character; the leader's starts the team too.
TEST(TeamPlay, JoiningTheHuntFromTheMenuReadiesThisSave) {
    const bbcoop::LobbyState alone;
    auto requests = TeamJoinHuntRequests(alone, "me", "t1", 3, "Eileen");
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].action(), bbcoop::TeamRequest::ACCEPT);
    EXPECT_EQ(requests[0].team(), "t1");
    EXPECT_EQ(requests[1].action(), bbcoop::TeamRequest::READY);
    EXPECT_EQ(requests[1].slot(), 3u);
    EXPECT_EQ(requests[1].character(), "Eileen");
    // No team and no invitation: nothing to join.
    EXPECT_TRUE(TeamJoinHuntRequests(alone, "me", "", 3, "Eileen").empty());
    // In a team already: ready.
    requests = TeamJoinHuntRequests(WithMember(InTeam("leader"), "me", false), "me", "", 1, "");
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].action(), bbcoop::TeamRequest::READY);
    // The leader: ready and start (a team with no world yet, this save none either).
    bbcoop::LobbyState lead = InTeam("me");
    lead.mutable_team()->clear_world();
    lead.mutable_team()->clear_party();
    requests = TeamJoinHuntRequests(lead, "me", "", 1, "");
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1].action(), bbcoop::TeamRequest::START);
}

// Kaniel 2026-10-16: a new character got the team's old world. Kaniel 2026-10-17: the worlds are
// the server's (any team plays any of them) and the leader picks one; the screen starts on this
// save's own (the world it last played), a new one for a new character or a save that never
// played in a team.
namespace {
bbcoop::LobbyState WithWorlds(bbcoop::LobbyState lobby, std::initializer_list<const char*> ids) {
    for (const char* id : ids) {
        lobby.add_worlds()->set_id(id);
    }
    return lobby;
}
} // namespace

TEST(TeamWorlds, TheScreenStartsOnThisSavesOwnWorld) {
    const bbcoop::LobbyState lobby = WithWorlds(InTeam("me"), {"w2", "w1"});
    EXPECT_EQ(DefaultTeamWorld(lobby, "w1", false), "w1");
    // A new character (the game starts it at the beginning): a new world, whatever its slot
    // played before.
    EXPECT_EQ(DefaultTeamWorld(lobby, "w1", true), "");
    // A save that never played in a team, or whose world the server no longer lists.
    EXPECT_EQ(DefaultTeamWorld(lobby, "", false), "");
    EXPECT_EQ(DefaultTeamWorld(lobby, "w9", false), "");
}

TEST(TeamWorlds, AGameInAnotherWorldIsOutOfTheTeam) {
    // Playing the team's world is playing in the team; another world (the team picked another
    // since, or another team's) is not.
    const bbcoop::LobbyState in = WithMember(InTeam("leader"), "me", true);
    EXPECT_EQ(TeamPlayStepOf(&in, "world:w-t1", nullptr), TeamPlayStep::None);
    EXPECT_EQ(TeamPlayStepOf(&in, "world:w-t10", nullptr), TeamPlayStep::Leave);
    // A team with no world yet (a new one at its Start) has no game in it.
    bbcoop::LobbyState fresh = in;
    fresh.mutable_team()->clear_world();
    fresh.mutable_team()->clear_party();
    EXPECT_EQ(TeamPlayStepOf(&fresh, "world:w-t1", nullptr), TeamPlayStep::Leave);
    bbcoop::TeamStart start = StartOf("leader", 2);
    EXPECT_EQ(TeamPlayStepOf(&in, "", &start, "me"), TeamPlayStep::Adopt);
    start.set_party("world:w-t10");
    EXPECT_EQ(TeamPlayStepOf(&in, "", &start, "me"), TeamPlayStep::None);
}

TEST(TeamWorlds, ALeaderJoiningTheHuntPicksItsSavesWorld) {
    bbcoop::LobbyState lobby = WithWorlds(InTeam("me"), {"w2", "w1"});
    lobby.mutable_team()->set_world("w2");
    auto requests = TeamJoinHuntRequests(lobby, "me", "", 1, "", "w1");
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_EQ(requests[0].action(), bbcoop::TeamRequest::READY);
    EXPECT_EQ(requests[1].action(), bbcoop::TeamRequest::WORLD);
    EXPECT_EQ(requests[1].world(), "w1");
    EXPECT_EQ(requests[2].action(), bbcoop::TeamRequest::START);
    // The world picked already: no pick.
    requests = TeamJoinHuntRequests(lobby, "me", "", 1, "", "w2");
    EXPECT_EQ(requests.size(), 2u);
}

// Kaniel 2026-10-17: the world is picked before the team is made. The screen starts on this save's
// world for a player in no team too (its team takes the pick), and for a leader whose team does
// not play yet; a member, or a team playing, picks nothing.
TEST(TeamWorlds, TheScreensFirstPickGoesBeforeATeamToo) {
    bbcoop::LobbyState alone = WithWorlds({}, {"w2", "w1"});
    EXPECT_EQ(TeamScreenWorldPick(alone, "me", "w1", false), std::optional<std::string>{"w1"});
    alone.set_world("w1");
    EXPECT_EQ(TeamScreenWorldPick(alone, "me", "w1", false), std::nullopt); // picked already
    EXPECT_EQ(TeamScreenWorldPick(alone, "me", "w1", true), std::optional<std::string>{""});
    bbcoop::LobbyState lead = WithWorlds(InTeam("me"), {"w2", "w1"});
    EXPECT_EQ(TeamScreenWorldPick(lead, "me", "w1", false), std::optional<std::string>{"w1"});
    lead.mutable_team()->set_started(true);
    EXPECT_EQ(TeamScreenWorldPick(lead, "me", "w1", false), std::nullopt);
    const bbcoop::LobbyState member = WithWorlds(InTeam("leader"), {"w1"});
    EXPECT_EQ(TeamScreenWorldPick(member, "me", "w1", false), std::nullopt);
}

TEST(TeamWorlds, AWorldsNameSaysWhoseItIs) {
    bbcoop::TeamWorldInfo world;
    world.set_id("w1");
    world.set_character("Kaniel");
    EXPECT_EQ(TeamWorldName(world), "Kaniel's world");
    world.set_character("");
    EXPECT_EQ(TeamWorldName(world), "A world");
    // A name of its own (the server's) comes first.
    world.set_name("Old Yharnam run");
    EXPECT_EQ(TeamWorldName(world), "Old Yharnam run");
}

// The team menu closes: opened from the game's System page, the pad presses Circle a moment later
// so that page closes too; opened from the port's Insert menu (no page of the game under it), none.
TEST(TeamMenuBack, CircleOnlyForTheGamesPage) {
    TeamMenuBack back;
    EXPECT_FALSE(back.Pressed(1000));
    back.Closed(1000, true);
    EXPECT_FALSE(back.Pressed(1000 + TeamMenuBack::AfterMs - 1));
    EXPECT_TRUE(back.Pressed(1000 + TeamMenuBack::AfterMs));
    EXPECT_FALSE(back.Pressed(1000 + TeamMenuBack::AfterMs + TeamMenuBack::HoldMs));
    back.Closed(5000, false);
    EXPECT_FALSE(back.Pressed(5000 + TeamMenuBack::AfterMs));
}

// Kaniel 2026-10-19: from the title the team screen called everyone Hunter until the load. The
// lobby hears the name last seen in the slot the title loads; nothing for a new character (the
// slot's old one is not it) or a slot never played with names kept.
TEST(TeamLobbyName, TheTitleLoadNamesTheSlotsCharacter) {
    EXPECT_EQ(TitleLoadCharacterName(false, "Lady Maria"), "Lady Maria");
    EXPECT_EQ(TitleLoadCharacterName(true, "Lady Maria"), "") << "a new character";
    EXPECT_EQ(TitleLoadCharacterName(false, ""), "") << "never the last character's name";
    EXPECT_EQ(TitleLoadCharacterName(false, "Lady Maria\r\n"), "Lady Maria") << "a file's line";
    EXPECT_EQ(TitleLoadCharacterName(false, std::string(17, 'x')), "") << "longer than a name";
}

// The game holds the last character's name until the title's load has read the new save: the
// name is read again only a while after the save file opened, so the old one never names the new
// character's slot.
TEST(TeamLobbyName, TheNameIsReadOnceTheLoadedSaveIsIn) {
    EXPECT_TRUE(CharacterNameReadable(0, 0, 50'000)) << "no title load since the start";
    EXPECT_FALSE(CharacterNameReadable(10'000, 0, 50'000)) << "the save not opened yet";
    EXPECT_FALSE(CharacterNameReadable(10'000, 9'000, 50'000)) << "an open before the load";
    EXPECT_FALSE(CharacterNameReadable(10'000, 12'000, 12'000 + CharacterNameSettleMs - 1));
    EXPECT_TRUE(CharacterNameReadable(10'000, 12'000, 12'000 + CharacterNameSettleMs));
}
