// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/bloodborne_lamp_points.h"
#include "core/bloodborne_party.h"
#include "core/libraries/np/np_signaling/np_signaling_reuse.h"

using namespace Core::Bloodborne;
using namespace std::chrono_literals;

namespace {

// A player standing still in their own world: loaded, no transition, matching idle, no search.
PartyObservation Idle() {
    return PartyObservation{
        .player_valid = true,
        .map_usable = true,
        .stage_transition = false,
        .matching_idle = true,
        .search_effect_active = false,
    };
}

PartyAction TickUntilDecision(PartyController& controller, const PartyObservation& observation,
                              PartyController::Clock::time_point now, int max_ticks = 100) {
    for (int i = 0; i < max_ticks; ++i) {
        const PartyAction action = controller.Tick(observation, now);
        if (action == PartyAction::StartSearch) {
            return action;
        }
    }
    return PartyAction::Wait;
}

const PartyController::Clock::time_point T0{};

} // namespace

TEST(BloodbornePartyController, StartsTheRoleSearchOnceTheWorldHasBeenStableForAWhile) {
    PartyController controller(PartyRole::Guest);
    EXPECT_EQ(controller.SearchEffectId(), SmallResonantBellEffect);

    for (u32 i = 1; i < PartyController::StableTicksBeforeSearch; ++i) {
        EXPECT_EQ(controller.Tick(Idle(), T0), PartyAction::Wait) << "tick " << i;
    }
    EXPECT_EQ(controller.Tick(Idle(), T0), PartyAction::StartSearch);
}

TEST(BloodbornePartyController, HostSearchesWithTheBeckoningBellEffect) {
    PartyController controller(PartyRole::Host);
    // A real Beckoning Bell leaves 9000 (special state 189) on the host, which the game then
    // pairs with 9003. 9001 is not a search effect and does nothing visible.
    EXPECT_EQ(controller.SearchEffectId(), 9000);
    EXPECT_EQ(TickUntilDecision(controller, Idle(), T0), PartyAction::StartSearch);
}

TEST(BloodbornePartyController, NeverSearchesWhileInASessionLoadingOrAlreadySearching) {
    for (auto broken : {&PartyObservation::player_valid, &PartyObservation::map_usable,
                        &PartyObservation::matching_idle}) {
        PartyController controller(PartyRole::Guest);
        PartyObservation observation = Idle();
        observation.*broken = false;
        EXPECT_EQ(TickUntilDecision(controller, observation, T0), PartyAction::Wait);
    }
    for (auto blocking :
         {&PartyObservation::stage_transition, &PartyObservation::search_effect_active}) {
        PartyController controller(PartyRole::Guest);
        PartyObservation observation = Idle();
        observation.*blocking = true;
        EXPECT_EQ(TickUntilDecision(controller, observation, T0), PartyAction::Wait);
    }
}

// Independent travel: a guest whose host left stays where it is, still a white phantom in the
// host's old world. Searching from there would pull it to wherever the host went.
TEST(BloodbornePartyController, AGuestLeftBehindByItsHostDoesNotSearch) {
    PartyController controller(PartyRole::Guest);
    PartyObservation left_behind = Idle();
    left_behind.white_phantom = true;
    EXPECT_EQ(TickUntilDecision(controller, left_behind, T0), PartyAction::Wait);
}

// A white phantom whose host is gone (OnLeave_Limit, event 4046) is reset into its own world.
// A party guest stayed where it was instead, which a host's trip once needed; now a trip keeps
// the session and never gets there. The rig's host crashed: its guest stood on as a phantom
// with no host, and a phantom never rings, so they never joined again. It stays only while the
// session still has someone; otherwise it goes home (and resumes where it was, see
// ResumeWhereTheGuestLeft) and rings from there.
TEST(BloodbornePartyController, AGuestWhoseSessionIsGoneGoesHome) {
    EXPECT_TRUE(SkipsLeaveLimit(WhiteGhostCharacterType, 1))
        << "the session goes on (another guest, a trip)";
    EXPECT_TRUE(SkipsLeaveLimit(WhiteGhostCharacterType, 2));
    EXPECT_FALSE(SkipsLeaveLimit(WhiteGhostCharacterType, 0))
        << "the host crashed or quit: nobody left";
}

// The rig's crash-apart test: a guest's game died while it was in the Ward, apart from its host,
// and the host's game ran the leave limit too (from eboot+0x1393323): its warp "home" found no
// map and took the host to the title screen. A live player (the host, or a guest after its own
// trip) is in its own world already, so a party member never takes that warp.
TEST(BloodbornePartyController, ALivePlayerNeverTakesTheLeaveWarp) {
    EXPECT_TRUE(SkipsLeaveLimit(0, 0)) << "the host, its guest gone";
    EXPECT_TRUE(SkipsLeaveLimit(0, 1));
}

// The rig's host-crash-apart test: the guest joined in Central Yharnam (the placement its sign got
// with the claim is its own spot there), travelled to the Ward and lost its host; once out of the
// session, that old placement warped it back to where it had joined. A placement is for its one
// join: spent once the session it brought is over.
TEST(BloodbornePartyController, AJoinsPlacementIsSpentWhenItsSessionEnds) {
    JoinPlacementLife life;
    life.Received();
    EXPECT_FALSE(life.Spent(0, true)) << "not joined yet: the claim is on its way";
    EXPECT_FALSE(life.Spent(MatchingStateSummonedGuest, false)) << "in the session it brought";
    EXPECT_FALSE(life.Spent(MatchingStateSummonedGuest, false));
    EXPECT_FALSE(life.Spent(0, false)) << "still leaving (a reload or the room's close pending)";
    EXPECT_TRUE(life.Spent(0, true)) << "the session is over";
    EXPECT_FALSE(life.Spent(0, true)) << "spent once";
    life.Received();
    EXPECT_FALSE(life.Spent(0, true)) << "the next join's placement";
}

TEST(BloodbornePartyController, AnyDisturbanceRestartsTheStabilityCount) {
    PartyController controller(PartyRole::Guest);
    for (u32 i = 1; i < PartyController::StableTicksBeforeSearch; ++i) {
        ASSERT_EQ(controller.Tick(Idle(), T0), PartyAction::Wait);
    }
    PartyObservation loading = Idle();
    loading.stage_transition = true;
    EXPECT_EQ(controller.Tick(loading, T0), PartyAction::Wait);
    // Needs a full stable window again, not just one more tick.
    EXPECT_EQ(controller.Tick(Idle(), T0), PartyAction::Wait);
    EXPECT_EQ(TickUntilDecision(controller, Idle(), T0), PartyAction::StartSearch);
}

TEST(BloodbornePartyController, WaitsForTheCooldownBeforeSearchingAgain) {
    PartyController controller(PartyRole::Guest);
    ASSERT_EQ(TickUntilDecision(controller, Idle(), T0), PartyAction::StartSearch);

    // The search did not take (no effect appeared): retry only after the cooldown.
    EXPECT_EQ(TickUntilDecision(controller, Idle(), T0 + PartyController::RetryDelay - 1s),
              PartyAction::Wait);
    EXPECT_EQ(TickUntilDecision(controller, Idle(), T0 + PartyController::RetryDelay),
              PartyAction::StartSearch);
}

TEST(BloodbornePartyController, SearchesAgainAfterTheSessionEnds) {
    PartyController controller(PartyRole::Guest);
    ASSERT_EQ(TickUntilDecision(controller, Idle(), T0), PartyAction::StartSearch);

    // The search took, we got summoned and played for a while...
    PartyObservation summoned = Idle();
    summoned.search_effect_active = true;
    EXPECT_EQ(controller.Tick(summoned, T0 + 1s), PartyAction::Wait);
    PartyObservation in_session = Idle();
    in_session.matching_idle = false;
    EXPECT_EQ(TickUntilDecision(controller, in_session, T0 + 2s), PartyAction::Wait);

    // ...then died and were sent home. The next search must not wait for the old cooldown.
    EXPECT_EQ(TickUntilDecision(controller, Idle(), T0 + 3s), PartyAction::StartSearch);
}

namespace {

struct AddSpEffectCall {
    int calls = 0;
    u64 character = 0;
    s32 effect_id = 0;
    u64 source = 0;
    s32 arg3 = -1;
    s32 arg4 = -1;
    s32 arg5 = -1;
    std::array<float, 5> scales{};
    s32 stack_arg = -1;
};
AddSpEffectCall last_add;

void PS4_SYSV_ABI FakeAddSpEffect(u64 character, s32 effect_id, u64 source, s32 arg3, s32 arg4,
                                  s32 arg5, float scale0, float scale1, float scale2, float scale3,
                                  float scale4, s32 stack_arg) {
    last_add = {
        .calls = last_add.calls + 1,
        .character = character,
        .effect_id = effect_id,
        .source = source,
        .arg3 = arg3,
        .arg4 = arg4,
        .arg5 = arg5,
        .scales = {scale0, scale1, scale2, scale3, scale4},
        .stack_arg = stack_arg,
    };
}

} // namespace

// 01.09 gives the local player 9005 itself (0x01C0D3B0) through the character's add-effect
// virtual with exactly these arguments. Doing the same starts the search with no bell item, no
// animation and no Insight.
TEST(BloodborneSummonSearch, AddsTheSearchEffectToThePlayerTheWayTheGameDoes) {
    last_add = {};
    constexpr u64 Player = 0x213cdffe0;
    EXPECT_TRUE(StartSummonSearch(Player, SmallResonantBellEffect, FakeAddSpEffect));

    EXPECT_EQ(last_add.calls, 1);
    EXPECT_EQ(last_add.character, Player);
    EXPECT_EQ(last_add.effect_id, SmallResonantBellEffect);
    EXPECT_EQ(last_add.source, Player);
    EXPECT_EQ(last_add.arg3, 0);
    EXPECT_EQ(last_add.arg4, 1);
    EXPECT_EQ(last_add.arg5, 0);
    for (const float scale : last_add.scales) {
        EXPECT_EQ(scale, 1.0f);
    }
    EXPECT_EQ(last_add.stack_arg, 0);
}

TEST(BloodborneSummonSearch, DoesNothingWithoutAPlayerOrTheGameFunction) {
    last_add = {};
    EXPECT_FALSE(StartSummonSearch(0, BeckoningBellEffect, FakeAddSpEffect));
    EXPECT_FALSE(StartSummonSearch(0x213cdffe0, BeckoningBellEffect, nullptr));
    EXPECT_EQ(last_add.calls, 0);
}

// The seamless patch keeps the session over every world teardown so the cross-map handoff
// survives its warp. A summoned guest whose world is torn down outside a summon reload is
// leaving the host's world (quit to the title screen, for one), and must end the session the
// way the unpatched game does, or the host keeps a ghost member and the guest never rejoins.
TEST(BloodborneWorldTeardown, ASummonedGuestLeavingTheHostWorldEndsTheSession) {
    EXPECT_TRUE(ShouldEndSessionOnWorldTeardown(MatchingStateSummonedGuest, 0, false));
}

// The same goes for a host whose world goes away (quit to the title screen; a host with a party
// no longer reloads on death): unpatched, the game stops matching there, and keeping the room
// leaves the guests in it with a host that is gone.
TEST(BloodborneWorldTeardown, AHostLeavingItsWorldEndsTheSession) {
    EXPECT_TRUE(ShouldEndSessionOnWorldTeardown(MatchingStateHosting, 0, false));
}

TEST(BloodborneWorldTeardown, KeepsTheSessionDuringASummonReload) {
    for (const s32 state : {MatchingStateSummonedGuest, MatchingStateHosting}) {
        EXPECT_FALSE(ShouldEndSessionOnWorldTeardown(state, SummonReloadInProgress, false));
        EXPECT_FALSE(ShouldEndSessionOnWorldTeardown(state, 0, true));
    }
}

TEST(BloodborneWorldTeardown, LeavesSearchesAndIdlePlayersAlone) {
    for (const s32 state : {0, 1, 2, 4, 5, 7}) {
        EXPECT_FALSE(ShouldEndSessionOnWorldTeardown(state, 0, false)) << "state " << state;
    }
}

// The game's sign poster waits 5 s before a new sign's first post and 30 s between re-posts,
// whatever ss.info says. The encoder must reproduce 01.09's own stores exactly...
// Seamless travel: a party member's trip (the game's stage transition, 0x013CDE30) keeps the
// session through the world teardown and through the leave 01.09 does once the new stage loads
// (0x0193C689). Quitting to the title is no trip, so it still ends the session.
TEST(BloodbornePartyTrip, KeepsTheSessionFromTheTransitionUntilTheNewStageHasLoaded) {
    PartyTrip trip;
    EXPECT_FALSE(trip.Active(T0));
    trip.Start(T0, 0x18010000, 0x1000);
    EXPECT_TRUE(trip.Active(T0 + 1s));
    trip.Finish();
    EXPECT_FALSE(trip.Active(T0 + 2s));
}

// Rig 2026-10-21 (mist-ambush-guest): the guest's lamp trip began before the session formed, and
// the join landed during its load; the world teardown found no trip and ended the session. A trip
// begun outside a session is none (the session's leave is the game's own then), but the party's
// once a session forms before it arrives.
TEST(BloodbornePartyTrip, ATripUnderWayWhenTheSessionFormsBecomesTheParty) {
    constexpr u32 CentralYharnam = 0x18010000;
    constexpr u32 Hemwick = 0x16000000;
    PartyTrip trip;
    trip.StartOutOfSession(T0, CentralYharnam, 0x1000);
    EXPECT_FALSE(trip.Active(T0 + 1s)) << "no session: no party trip";
    trip.ObserveStage(Hemwick, true, 0x1000);
    EXPECT_TRUE(trip.JoinSession(T0 + 2s)) << "the join landed during its load";
    EXPECT_TRUE(trip.Active(T0 + 2s));
    EXPECT_FALSE(trip.JoinSession(T0 + 3s)) << "once";
    trip.ObserveStage(Hemwick, false, 0x2000);
    EXPECT_FALSE(trip.Active(T0 + 4s)) << "arrived";
}

// One that arrived before the session formed is over: the join keeps no trip.
TEST(BloodbornePartyTrip, ATripThatArrivedBeforeTheSessionIsNone) {
    constexpr u32 CentralYharnam = 0x18010000;
    constexpr u32 Hemwick = 0x16000000;
    PartyTrip trip;
    trip.StartOutOfSession(T0, CentralYharnam, 0x1000);
    trip.ObserveStage(Hemwick, false, 0x2000);
    EXPECT_FALSE(trip.JoinSession(T0 + 2s));
    EXPECT_FALSE(trip.Active(T0 + 2s));
    trip.StartOutOfSession(T0, CentralYharnam, 0x1000);
    EXPECT_FALSE(trip.JoinSession(T0 + PartyTrip::Timeout)) << "timed out";
}

// The trip ends at the first tick on the new stage (the party tick does not run while loading).
TEST(BloodbornePartyTrip, EndsOnceThePlayerIsOnTheNewStage) {
    constexpr u32 CentralYharnam = 0x18010000;
    constexpr u32 HuntersDream = 0x15000000;
    PartyTrip trip;
    trip.Start(T0, CentralYharnam, 0x1000);
    trip.ObserveStage(CentralYharnam, false, 0x1000);
    EXPECT_TRUE(trip.Active(T0 + 1s)) << "still on the old stage";
    trip.ObserveStage(HuntersDream, true, 0x1000);
    EXPECT_TRUE(trip.Active(T0 + 2s)) << "still loading";
    trip.ObserveStage(HuntersDream, false, 0x1000);
    EXPECT_FALSE(trip.Active(T0 + 3s));
}

// Kaniel 2026-10-13: after Father Gascoigne, the walk from Central Yharnam into Cathedral Ward
// called the world load's StopRequest (0x0193C65C) three times in 70 ms while the map flipped
// back and forth, with no load at all. Each flip ended the "trip" as an arrival and re-created
// the guest (its join task stuck in the create step: the host lost sight of it), and each
// StopRequest started another. A world reload's trip ends only after a load, and is dropped when
// none comes.
TEST(BloodbornePartyTrip, AWorldReloadWithNoLoadIsNoTrip) {
    constexpr u32 CentralYharnam = 0x18010000;
    constexpr u32 CathedralWard = 0x18000000;
    PartyTrip trip;
    trip.StartWorldReload(T0, CentralYharnam, 0x1000);
    trip.ObserveStage(CathedralWard, false, 0x1000);
    trip.ObserveStage(CentralYharnam, false, 0x1000);
    trip.ObserveStage(CathedralWard, false, 0x1000);
    EXPECT_TRUE(trip.Active(T0 + 1s)) << "a map flip is no arrival: the next StopRequest is kept";
    EXPECT_FALSE(trip.DropIfNoLoad(T0 + 1s));
    EXPECT_TRUE(trip.DropIfNoLoad(T0 + PartyTrip::NoLoadGrace)) << "dropped, no arrival";
    EXPECT_FALSE(trip.Active(T0 + PartyTrip::NoLoadGrace));
    EXPECT_FALSE(trip.DropIfNoLoad(T0 + PartyTrip::NoLoadGrace + 1s)) << "once";
}

// The time-of-day cutscene's reload (Gascoigne's 24000020, the Ward door) loads: a trip as before.
TEST(BloodbornePartyTrip, AWorldReloadThatLoadsArrives) {
    constexpr u32 CentralYharnam = 0x18010000;
    PartyTrip trip;
    trip.StartWorldReload(T0, CentralYharnam, 0x1000);
    trip.ObserveStage(CentralYharnam, true, 0);
    EXPECT_FALSE(trip.DropIfNoLoad(T0 + PartyTrip::NoLoadGrace)) << "it loaded";
    EXPECT_TRUE(trip.Active(T0 + PartyTrip::NoLoadGrace));
    trip.ObserveStage(CentralYharnam, false, 0x2000);
    EXPECT_FALSE(trip.Active(T0 + PartyTrip::NoLoadGrace + 1s)) << "arrived: a new character";
    // A lamp's trip is never dropped for want of a load seen (the party tick stops while loading).
    trip.Start(T0, CentralYharnam, 0x1000);
    EXPECT_FALSE(trip.DropIfNoLoad(T0 + PartyTrip::NoLoadGrace));
    EXPECT_TRUE(trip.Active(T0 + PartyTrip::NoLoadGrace));
}

// Rig 2026-10-13 (crashes): a guest alone took over 302 enemies in its own world, then the join
// reloaded the same map (0x18000000) and the claims, kept by map only, outlived their world: the
// hand-back wrote the old ownership words into freed memory (a heap pointer read ffff0010...), and
// the next same-map reload crashed in the game at 0x15913d8. A new player character is a new
// world.
TEST(BloodborneLoneGuest, ClaimsEndWithTheirWorld) {
    constexpr u32 CathedralWard = 0x18000000;
    constexpr u32 CentralYharnam = 0x18010000;
    ClaimedWorld world;
    EXPECT_TRUE(world.NewWorld(CathedralWard, 0x1000)) << "the first";
    EXPECT_FALSE(world.NewWorld(CathedralWard, 0x1000));
    EXPECT_FALSE(world.NewWorld(CathedralWard, 0)) << "loading: no player yet";
    EXPECT_TRUE(world.NewWorld(CathedralWard, 0x2000)) << "the same map loaded again";
    EXPECT_FALSE(world.NewWorld(CathedralWard, 0x2000));
    EXPECT_TRUE(world.NewWorld(CentralYharnam, 0x2000)) << "a border: another map";
}

// Rig 2026-10-13 (crashes, host-crash): the host's game died while a guest that had travelled (a
// live player, type 0) was with it; no leave limit ran, the session effect 9001 stayed on the
// guest, and with it on, its bell (9005) never posted a sign: 420 s and no join. A guest out of
// its session takes 9001 off before it rings.
TEST(BloodbornePartyController, AGuestOutOfItsSessionDropsTheSessionEffect) {
    const std::vector<s32> stale{6300, 9006, 40, 9001};
    EXPECT_TRUE(EndsStaleSessionEffect(PartyRole::Guest, false, stale));
    EXPECT_FALSE(EndsStaleSessionEffect(PartyRole::Guest, true, stale)) << "still in it";
    EXPECT_FALSE(EndsStaleSessionEffect(PartyRole::Host, false, stale)) << "the host's own";
    EXPECT_FALSE(EndsStaleSessionEffect(PartyRole::Guest, false, std::vector<s32>{6300, 9005}));
}

// A trip reloads the traveller's world, which destroys the other players' characters, while
// their join tasks (CSMultiPlayerIns) stay in the in-session step. Sending a remote player's task
// back to the step that creates its character brings it back on the new stage.
TEST(BloodbornePartyTrip, RecreatesRemotePlayersThatWereInTheSession) {
    EXPECT_TRUE(ShouldRecreateAfterTrip(RemoteHostTask, InSessionStep));
    EXPECT_TRUE(ShouldRecreateAfterTrip(RemoteGuestTask, InSessionStep));
    EXPECT_EQ(CreateCharacterStep, InSessionStep - 2);
}

// The reload also empties the join-task list, so a member without a task gets a new one of the
// kind the game gives it on join: a host makes remote guests, a guest makes the remote host.
TEST(BloodbornePartyTrip, RebuildsMissingTasksWithTheKindTheGameUsesOnJoin) {
    EXPECT_EQ(RemoteTaskKind(true), RemoteGuestTask);
    EXPECT_EQ(RemoteTaskKind(false), RemoteHostTask);
}

// On a guest, its own join task (kind 2) is the one that creates the host's character; a host's
// own task is kind 3. A guest's trip loses it like the others.
TEST(BloodbornePartyTrip, RebuildsTheGuestsOwnTaskToo) {
    EXPECT_EQ(LocalTaskKind(false), 2);
    EXPECT_EQ(LocalTaskKind(true), 3);
}

TEST(BloodbornePartyTrip, LeavesLocalAndUnfinishedJoinsAlone) {
    for (const s32 local : {2, 3}) {
        EXPECT_FALSE(ShouldRecreateAfterTrip(local, InSessionStep)) << local;
    }
    for (s32 step = 0; step <= 10; ++step) {
        if (step != InSessionStep) {
            EXPECT_FALSE(ShouldRecreateAfterTrip(RemoteGuestTask, step)) << step;
        }
    }
}

// A reload of the same map (a guest's death far from its lamp) is a trip too: the player is a
// new character once it is over.
TEST(BloodbornePartyTrip, EndsAfterAReloadOfTheSameMap) {
    constexpr u32 CentralYharnam = 0x18010000;
    PartyTrip trip;
    trip.Start(T0, CentralYharnam, 0x1000);
    trip.ObserveStage(CentralYharnam, false, 0x1000);
    EXPECT_TRUE(trip.Active(T0 + 1s)) << "same map, same character";
    trip.ObserveStage(CentralYharnam, false, 0);
    EXPECT_TRUE(trip.Active(T0 + 2s)) << "no character yet";
    trip.ObserveStage(CentralYharnam, false, 0x2000);
    EXPECT_FALSE(trip.Active(T0 + 3s));
}

TEST(BloodbornePartyTrip, ATripThatNeverLoadsStopsCountingAfterAWhile) {
    PartyTrip trip;
    trip.Start(T0, 0x18010000, 0x1000);
    EXPECT_TRUE(trip.Active(T0 + PartyTrip::Timeout - 1s));
    EXPECT_FALSE(trip.Active(T0 + PartyTrip::Timeout));
}

// The party's map board on shadNet (POST /bloodborne/party/map): "<member> <map>" in hex.
TEST(BloodbornePartyMaps, PostsAndReadsTheMapEachMemberIsOn) {
    EXPECT_EQ(EncodeMapUpdate("a1b2", 0x18010000), "a1b2 18010000");
    const auto board = ParseMapBoard("a1b2 18010000\nc3d4 15000000\n");
    ASSERT_TRUE(board.has_value());
    ASSERT_EQ(board->size(), 2u);
    EXPECT_EQ((*board)[1].member, "c3d4");
    EXPECT_EQ((*board)[1].map, 0x15000000u);
    EXPECT_FALSE(ParseMapBoard("a1b2\n").has_value());
    EXPECT_FALSE(ParseMapBoard("a1b2 zz\n").has_value());
}

// A trip's re-created players are only made once every other member is on this map: creating
// a player into a map it is not on makes its game leave the session.
TEST(BloodbornePartyMaps, RecreatesOnlyWhenEveryoneElseIsOnThisMap) {
    const std::vector<MemberMap> apart{{"me", 0x15000000}, {"them", 0x18010000}};
    const std::vector<MemberMap> together{{"me", 0x18010000}, {"them", 0x18010000}};
    EXPECT_FALSE(EveryoneElseOnMap(apart, "me", 0x15000000));
    EXPECT_TRUE(EveryoneElseOnMap(together, "me", 0x18010000));
    const std::vector<MemberMap> alone{{"me", 0x18010000}};
    EXPECT_FALSE(EveryoneElseOnMap(alone, "me", 0x18010000)) << "nobody else";
}

// A guest alone on a map (the others elsewhere): the host has none of its enemies.
TEST(BloodbornePartyMaps, AGuestCanBeAloneOnItsMap) {
    const std::vector<MemberMap> apart{{"me", 0x18000000}, {"them", 0x18010000}};
    const std::vector<MemberMap> together{{"me", 0x18010000}, {"them", 0x18010000}};
    EXPECT_TRUE(NoOneElseOnMap(apart, "me", 0x18000000));
    EXPECT_FALSE(NoOneElseOnMap(together, "me", 0x18010000));
    EXPECT_FALSE(NoOneElseOnMap(apart, "me", 0)) << "loading";
}

// The new stage's load drops the bell's search effect (9000/9003 for the host), which the game
// only adds when a bell is rung. After a trip in a party session it is put back.
// The multiplayer effects a player carries into a trip (the host's 9000/9003, the guest's
// 9005/9006) are lost with the old stage; the ones it had are put back on arrival.
TEST(BloodbornePartyTrip, PutsBackTheMultiplayerEffectsThePlayerHad) {
    const std::vector<s32> before{6300, 9000, 40, 9003, 9001};
    const std::vector<s32> after{6300, 40};
    EXPECT_EQ(EffectsToRestoreAfterTrip(before, after), (std::vector<s32>{9000, 9003}));
    const std::vector<s32> guest_before{6300, 40, 9006};
    EXPECT_EQ(EffectsToRestoreAfterTrip(guest_before, after), (std::vector<s32>{9006}));
    EXPECT_TRUE(EffectsToRestoreAfterTrip(before, before).empty());
}

TEST(BloodbornePartyTrip, PutsTheSearchEffectBackAfterTheTrip) {
    EXPECT_TRUE(ShouldRestoreSearchAfterTrip(true, false));
    EXPECT_FALSE(ShouldRestoreSearchAfterTrip(true, true)) << "still there";
    EXPECT_FALSE(ShouldRestoreSearchAfterTrip(false, false)) << "no party session";
}

// Player report 2026-10-22: the League's "Impurity" rune (more HP while cooperating) did nothing
// in co-op. Its effect 6142 gets the bonus 180040 from common event 9035 only while the wearer is
// a white phantom, which a party guest is only until its first trip and a host never is. Anyone in
// a party session wearing the rune has the bonus; off again once not (only the one put on here).
TEST(BloodbornePartyRunes, APartyMemberWearingImpurityHasItsHpBonus) {
    EXPECT_EQ(ImpurityRuneEffect, 6142);
    EXPECT_EQ(ImpurityHpEffect, 180040);
    const std::vector<s32> wearing{6300, 40, 6142, 9006};
    const std::vector<s32> with_bonus{6300, 6142, 180040};
    const std::vector<s32> bonus_only{6300, 180040};
    const std::vector<s32> none{6300, 40};
    EXPECT_EQ(ImpurityBonusChange(true, wearing, false), EffectChange::Add) << "after a trip";
    EXPECT_EQ(ImpurityBonusChange(true, wearing, true), EffectChange::Add) << "a load took it off";
    EXPECT_EQ(ImpurityBonusChange(true, with_bonus, false), EffectChange::None) << "the game's";
    EXPECT_EQ(ImpurityBonusChange(true, with_bonus, true), EffectChange::None);
    EXPECT_EQ(ImpurityBonusChange(true, none, false), EffectChange::None) << "no rune";
    EXPECT_EQ(ImpurityBonusChange(true, bonus_only, true), EffectChange::Remove) << "rune off";
    EXPECT_EQ(ImpurityBonusChange(false, with_bonus, true), EffectChange::Remove) << "party left";
    EXPECT_EQ(ImpurityBonusChange(false, with_bonus, false), EffectChange::None)
        << "the game's own (a phantom outside our party)";
    EXPECT_EQ(ImpurityBonusChange(false, wearing, false), EffectChange::None)
        << "no guest in a session: the host, or alone";

    EXPECT_TRUE(GetsImpurityBonus(true, true)) << "a guest in its session";
    EXPECT_FALSE(GetsImpurityBonus(false, true)) << "a guest not joined yet";
    EXPECT_FALSE(GetsImpurityBonus(false, false)) << "alone";
    EXPECT_TRUE(GetsImpurityBonus(true, false)) << "the host, cooperated with (Kaniel)";
}

// Experiment (Kaniel, 2026-10-06): SHADPS4_BLOODBORNE_GUEST_AS_HOST=1 makes a party guest a live
// player (character type 0) instead of a white phantom while it is in the session.
TEST(BloodborneGuestAsHost, OnlyWhenSwitchedOnAndAWhitePhantomInAPartySession) {
    EXPECT_TRUE(SettingIsOn("1"));
    EXPECT_FALSE(SettingIsOn(nullptr));
    EXPECT_FALSE(SettingIsOn("0"));
    EXPECT_TRUE(ShouldPlayGuestAsHost(true, true, WhiteGhostCharacterType));
    EXPECT_FALSE(ShouldPlayGuestAsHost(false, true, WhiteGhostCharacterType));
    EXPECT_FALSE(ShouldPlayGuestAsHost(true, false, WhiteGhostCharacterType));
    EXPECT_FALSE(ShouldPlayGuestAsHost(true, true, 0)) << "already live";
}

// The multiplayer walls (common event 7600: wall object and its fog SFX, shown while in
// multiplayer) are what keeps a summoned guest inside an area. A party session turns them off.
TEST(BloodborneMultiplayerWalls, ListsTheWallsOfEveryMap) {
    const auto walls = MultiplayerWalls();
    ASSERT_EQ(walls.size(), 19u);
    const auto on = [&](u32 map) {
        return std::ranges::count_if(walls, [&](const MultiplayerWall& wall) {
            return PackedMapOfWarpPoint(wall.object) == map;
        });
    };
    EXPECT_EQ(on(0x18010000), 3); // Central Yharnam
    EXPECT_EQ(on(0x18000000), 5); // Cathedral Ward
    EXPECT_EQ(on(0x15000000), 0) << "the Hunter's Dream has none";
    EXPECT_EQ(on(0x22000000), 1) << "the Hunter's Nightmare's own (m34_00 event 13400010)";
    for (const MultiplayerWall& wall : walls) {
        EXPECT_EQ(wall.sfx, wall.object + 2000) << wall.object;
    }
}

// Player report 2026-10-22: the Forbidden Woods' wall stood at the foot of the stairs from
// Cathedral Ward. The player is still on Cathedral Ward there while the woods (m27) are loaded,
// so the walls of every map go off, not only the current map's (a wall whose map is not loaded
// is a miss in the game's entity lookup).
TEST(BloodborneMultiplayerWalls, IncludesTheWallsOfNeighbouringMaps) {
    const auto walls = MultiplayerWalls();
    for (const s32 forbidden_woods : {2701999, 2701998}) {
        EXPECT_TRUE(std::ranges::any_of(walls, [&](const MultiplayerWall& wall) {
            return wall.object == forbidden_woods;
        })) << forbidden_woods;
    }
}

// A summoned guest carries special effect 9006, whose max HP rate (SpEffectParam +0x10) is 0.7.
// A party guest keeps its full HP: the row is found in the loaded param and set to 1.0.
TEST(BloodborneGuestHp, FindsARowInAParamFile) {
    // PARAM: row count (u16) at +0x0A, the type name at +0x0C, then 24-byte row entries at +0x40
    // (id, padding, data offset, name offset), sorted by id.
    std::vector<u8> param(0x40 + 3 * 24 + 3 * 16, 0);
    param[0x0A] = 3;
    const std::string name = "SP_EFFECT_PARAM_ST";
    std::copy(name.begin(), name.end(), param.begin() + 0x0C);
    const std::array<s32, 3> ids{9005, 9006, 9008};
    for (std::size_t row = 0; row < ids.size(); ++row) {
        const u64 data = 0x40 + 3 * 24 + row * 16;
        std::memcpy(param.data() + 0x40 + row * 24, &ids[row], sizeof(s32));
        std::memcpy(param.data() + 0x40 + row * 24 + 8, &data, sizeof(u64));
    }
    EXPECT_TRUE(IsParamOfType(param, "SP_EFFECT_PARAM_ST"));
    EXPECT_FALSE(IsParamOfType(param, "EQUIP_PARAM_GOODS_ST"));
    EXPECT_EQ(FindParamRow(param, 9006), std::optional<u64>{0x40 + 3 * 24 + 16});
    EXPECT_EQ(FindParamRow(param, 9007), std::nullopt);
    EXPECT_EQ(PhantomHpEffect, 9006);
    EXPECT_EQ(SpEffectMaxHpRateOffset, 0x10u);
}

// A guest above the host's level band gets special effect 9200, which scales its max HP,
// stamina and attack down to the host's level. A party guest is never synced down.
TEST(BloodborneGuestHp, PartyGuestsAreNeverLevelSynced) {
    EXPECT_EQ(LevelSyncEffect, 9200);
    EXPECT_TRUE(LevelSyncsGuest(false, true)) << "outside a party the game decides";
    EXPECT_FALSE(LevelSyncsGuest(false, false));
    EXPECT_FALSE(LevelSyncsGuest(true, true));
    EXPECT_FALSE(LevelSyncsGuest(true, false));
}

// On a summoned guest's side, NPCs whose NpcParam has isChangeWanderGhost (+0x145 bit 7) are
// created as wandering ghosts (character type 4, drawn see-through). A party guest sees them as
// the host does.
TEST(BloodborneGuestNpcs, PartyGuestsSeeNpcsSolid) {
    EXPECT_EQ(NpcWanderGhostFlagOffset, 0x145u);
    EXPECT_EQ(NpcWanderGhostFlag, 0x80u);
    EXPECT_TRUE(NpcTurnsIntoWanderGhost(false, true)) << "outside a party the game decides";
    EXPECT_FALSE(NpcTurnsIntoWanderGhost(false, false));
    EXPECT_FALSE(NpcTurnsIntoWanderGhost(true, true));
    EXPECT_FALSE(NpcTurnsIntoWanderGhost(true, false));
}

// The map item module skips a summoned guest: it creates no map items (matching state 6 or a
// joined session, [session+0x98] != -1) and only lets it pick one up while GlobalState +0x1592
// is set. A party guest reads its answers from a stand-in that says "solo".
TEST(BloodborneGuestLoot, PartyGuestsHandleMapItemsLikeASoloPlayer) {
    const auto& stand_in = SoloMapItemStandIn();
    s32 state = -1;
    std::memcpy(&state, stand_in.data() + MatchingStateOffset, sizeof(state));
    EXPECT_EQ(state, 0) << "not a summoned guest";
    s32 joined = 0;
    std::memcpy(&joined, stand_in.data() + JoinedSessionOffset, sizeof(joined));
    EXPECT_EQ(joined, -1) << "no joined session";
    EXPECT_NE(stand_in[GuestItemPickupAllowedOffset], 0) << "pickups allowed";
    EXPECT_TRUE(GuestHandlesMapItemsAlone(true));
    EXPECT_FALSE(GuestHandlesMapItemsAlone(false));
}

// Map item packets (channel 0x2C, header word 1, then the message kind): the host's item sync
// clears the guest's map items, which party members keep for themselves. A configured party
// reads every such packet as empty, so each player's map items stay its own.
TEST(BloodborneGuestLoot, PartyMembersIgnoreEachOthersMapItemSync) {
    EXPECT_EQ(MapItemPacketChannel, 0x2Cu);
    constexpr u32 clear_items = 0x00030001;
    EXPECT_EQ(MapItemPacketHeaderSeen(clear_items, true), 0u);
    EXPECT_EQ(MapItemPacketHeaderSeen(clear_items, false), clear_items);
}

// Shared trophies (POST /bloodborne/party/trophy): each member posts "<member> [<id>...]" with
// what it unlocked in the party and reads back one id per line; it unlocks the ones it has not
// handled yet.
TEST(BloodborneTrophies, MembersShareWhatTheyUnlockInTheParty) {
    const std::vector<s32> earned{12, 3};
    EXPECT_EQ(EncodeTrophyUpdate("host", earned), "host 12 3");
    EXPECT_EQ(EncodeTrophyUpdate("guest", {}), "guest");
    EXPECT_EQ(ParseTrophyBoard("3\n12\n"), (std::optional<std::vector<s32>>{{3, 12}}));
    EXPECT_EQ(ParseTrophyBoard(""), (std::optional<std::vector<s32>>{std::vector<s32>{}}));
    EXPECT_EQ(ParseTrophyBoard("3\nx\n"), std::nullopt);
    const std::vector<s32> board{3, 12, 40};
    const std::vector<s32> handled{12};
    EXPECT_EQ(TrophiesToUnlock(board, handled), (std::vector<s32>{3, 40}));
}

// A guest's progress in the host's world (picked-up items: ItemLotParam getItemFlagId,
// 5[0-3]MMxxxx) is recorded and written into its own world once it is back home.
TEST(BloodborneGuestProgress, RecordsMapPickupsAndKeepsThemInAFile) {
    EXPECT_TRUE(IsMapPickupFlag(52400100));
    EXPECT_TRUE(IsMapPickupFlag(50000040));
    EXPECT_TRUE(IsMapPickupFlag(53600000));
    EXPECT_FALSE(IsMapPickupFlag(52901000)) << "chalice dungeons";
    EXPECT_FALSE(IsMapPickupFlag(72410100)) << "lamp";
    EXPECT_FALSE(IsMapPickupFlag(4000));
    const std::vector<s32> flags{52400100, 53600000};
    EXPECT_EQ(FormatFlagList(flags), "52400100\n53600000\n");
    EXPECT_EQ(ParseFlagList("52400100\n53600000\n"), flags);
    EXPECT_EQ(ParseFlagList("52400100\nbad\n\n53600000"), flags) << "bad lines are skipped";
}

// The event flag store: blocks of flags (EventFlagMan +0x1C flags each), a flag's bit at byte
// index/8, most significant bit first. Comparing two copies of a block gives the flags that
// were set or cleared in between.
TEST(BloodborneGuestProgress, FindsTheFlagsABlockChanged) {
    std::vector<u8> before(4, 0);
    std::vector<u8> after(4, 0);
    after[0] = 0x80;  // index 0
    after[2] = 0x01;  // index 23
    before[3] = 0x40; // index 25, cleared
    const FlagChanges changes = ChangedFlags(1241, 1000, before, after);
    EXPECT_EQ(changes.set, (std::vector<s32>{1241000, 1241023}));
    EXPECT_EQ(changes.cleared, (std::vector<s32>{1241025}));
}

// Shared progression: what changes in the host's world during a session (lamps, shortcuts,
// bosses, quest steps) reaches the guest's own world. A join reload changes hundreds of flags at
// once and is not progress; nor are the death/multiplayer (4xxx) flags or the
// pickups, which the pickup list keeps.
TEST(BloodborneGuestProgress, CarriesTheSessionsWorldChangesHome) {
    EXPECT_TRUE(CarriesToGuestWorld(7116)) << "lamp";
    EXPECT_TRUE(CarriesToGuestWorld(12410102)) << "shortcut";
    EXPECT_TRUE(CarriesToGuestWorld(70000030));
    EXPECT_FALSE(CarriesToGuestWorld(4000)) << "death";
    EXPECT_TRUE(CarriesToGuestWorld(9456)) << "boss defeated (set with 2411 and 7017)";
    EXPECT_FALSE(CarriesToGuestWorld(52410360)) << "pickups have their own list";
    EXPECT_FALSE(CarriesToGuestWorld(72410101)) << "a lamp trip, not progress";
    EXPECT_FALSE(CarriesToGuestWorld(72102411)) << "a headstone trip";
    // The bell's bookkeeping (common events 9181-9198): 9185 "a bell is ringing" makes 9183
    // refund 1 Insight with a "resonance ended" message once the bell effect is gone. Kept in
    // the guest's world it came back on every load (friend run 2026-10-08: the bell messages
    // kept coming and the guest's Insight went up each time).
    EXPECT_FALSE(CarriesToGuestWorld(9185));
    EXPECT_FALSE(CarriesToGuestWorld(9199));
    EXPECT_FALSE(CarriesToGuestWorld(9180));
    EXPECT_EQ(ParseWorldChanges("9185 1\n7116 1\n9199 1\n"), (std::map<s32, bool>{{7116, true}}))
        << "a saved world drops them too";
    EXPECT_TRUE(IsBulkFlagChange(65));
    EXPECT_FALSE(IsBulkFlagChange(12));

    std::map<s32, bool> world;
    RecordWorldChanges(world, FlagChanges{{7116, 4000, 12410102}, {12700000}});
    RecordWorldChanges(world, FlagChanges{{12700000}, {7116}});
    EXPECT_EQ(world, (std::map<s32, bool>{{7116, false}, {12410102, true}, {12700000, true}}));
    const std::string text = FormatWorldChanges(world);
    EXPECT_EQ(text, "7116 0\n12410102 1\n12700000 1\n");
    EXPECT_EQ(ParseWorldChanges(text), world);
}

// Kill echoes (0x018FC860, and the calculation 0x018FC9D0) are multiplied by 0.5 for a summoned
// guest: `vmovss xmm3, [rip+disp]` loads 0.5 from 0x04927EC0 (or 0x04927ED0). A party guest's
// load is pointed at the 1.0 next to it (0x04927EBC).
TEST(BloodborneGuestLoot, PartyGuestsGetFullKillEchoes) {
    const std::array<u8, 8> original{0xC5, 0xFA, 0x10, 0x1D, 0x71, 0xB5, 0x02, 0x03};
    EXPECT_EQ(RetargetRipLoad(original, 0x018FC947, 0x04927EBC),
              (std::array<u8, 8>{0xC5, 0xFA, 0x10, 0x1D, 0x6D, 0xB5, 0x02, 0x03}));
    const std::array<u8, 8> second{0xC5, 0xFA, 0x10, 0x1D, 0x21, 0xB4, 0x02, 0x03};
    EXPECT_EQ(RetargetRipLoad(second, 0x018FCAA7, 0x04927EBC),
              (std::array<u8, 8>{0xC5, 0xFA, 0x10, 0x1D, 0x0D, 0xB4, 0x02, 0x03}));
    // Boss echoes (0x01335589, `vmulss xmm0, xmm0, [rip+disp]` by 0.5 at 0x04925B1C for a
    // phantom in multiplayer).
    const std::array<u8, 8> boss{0xC5, 0xFA, 0x59, 0x05, 0x8B, 0x05, 0x5F, 0x03};
    EXPECT_EQ(RetargetRipLoad(boss, 0x01335589, 0x04927EBC),
              (std::array<u8, 8>{0xC5, 0xFA, 0x59, 0x05, 0x2B, 0x29, 0x5F, 0x03}));
}

// A guest's lost echoes (GameDataMan +0x28 "has a bloodstain", the record's echoes at +0x38 and
// block at +0x3C): joining the host clears the mark but keeps the record, so the enemy holding
// the echoes gives nothing back. Picking the echoes up clears the record too (-1, 0xFFFFFFFF).
TEST(BloodborneGuestBloodstain, AJoinKeepsTheGuestsLostEchoes) {
    EXPECT_TRUE(RestoresBloodstainMark(true, true, false, 300, 0x18010000));
    EXPECT_FALSE(RestoresBloodstainMark(false, true, false, 300, 0x18010000)) << "not a guest";
    EXPECT_FALSE(RestoresBloodstainMark(true, false, false, 300, 0x18010000)) << "had none";
    EXPECT_FALSE(RestoresBloodstainMark(true, true, true, 300, 0x18010000)) << "still set";
    EXPECT_FALSE(RestoresBloodstainMark(true, true, false, -1, 0xFFFFFFFF)) << "picked up";
}

// The guest's kept progress belongs to one character: the save slot the game writes
// (/savedata0/userdata000N, N 0-9; userdata0010 holds the system data).
TEST(BloodborneGuestProgress, BelongsToTheCharacterBeingSaved) {
    EXPECT_EQ(CharacterSlotOfSavePath("/savedata0/userdata0005"), std::optional<int>{5});
    EXPECT_EQ(CharacterSlotOfSavePath("/savedata0/userdata0000"), std::optional<int>{0});
    EXPECT_EQ(CharacterSlotOfSavePath("/savedata0/userdata0010"), std::nullopt);
    EXPECT_EQ(CharacterSlotOfSavePath("/savedata0/backup0005"), std::nullopt);
    EXPECT_EQ(CharacterSlotOfSavePath("/app0/userdata0005"), std::nullopt);
}

// Shared world changes (POST /bloodborne/party/flags): "<member> <since> [<flag>:<0|1>...]";
// the answer is the latest sequence number and the other members' changes after <since>.
TEST(BloodborneGuestProgress, SendsTheGuestsWorldChangesToTheHost) {
    const std::vector<std::pair<s32, bool>> changes{{12417830, true}, {7116, false}};
    EXPECT_EQ(EncodeFlagUpdate("guest", 4, changes), "guest 4 12417830:1 7116:0");
    EXPECT_EQ(EncodeFlagUpdate("host", 0, {}), "host 0");
    const auto board = ParseFlagBoard("6\n12417830 1\n7116 0\n");
    ASSERT_TRUE(board.has_value());
    EXPECT_EQ(board->latest, 6);
    EXPECT_EQ(board->changes, changes);
    EXPECT_EQ(ParseFlagBoard("x\n"), std::nullopt);
    EXPECT_EQ(ParseFlagBoard("6\n7116 2\n"), std::nullopt);
}

// A lamp counts as lit for a character only if it is in the character's lamp list; its
// "active" flag (10007810 + map * 10000 + lamp * 20, the talk scripts' check) belongs to lamp
// entity map * 10000 + 1950 + lamp. A lamp lit by another party member gets added to the list.
TEST(BloodborneLamps, ALampLitByAnotherMemberIsTheirsToo) {
    EXPECT_EQ(LampOfActiveFlag(12417830), std::optional<s32>{2411951});
    EXPECT_EQ(LampOfActiveFlag(12417810), std::optional<s32>{2411950});
    EXPECT_EQ(LampOfActiveFlag(12117850), std::optional<s32>{2111952});
    EXPECT_EQ(LampOfActiveFlag(12417831), std::nullopt);
    EXPECT_EQ(LampOfActiveFlag(7116), std::nullopt);
    EXPECT_EQ(LampOfActiveFlag(12415390), std::nullopt);
}

// A lamp's "light it" request (70000000 + map * 10000 + 200 + lamp, common event 7100) belongs to
// the lamp's character (map * 10000 + 950 + lamp: common event 7000 keeps it switched off until
// the lamp's boss is dead) and its active flag (lit for this player).
TEST(BloodborneLamps, ALightRequestNamesItsLamp) {
    EXPECT_EQ(LampOfLightRequest(72200201),
              (std::optional<LampLight>{{.character = 2200951, .active_flag = 12207830}}));
    EXPECT_EQ(LampOfLightRequest(72200200),
              (std::optional<LampLight>{{.character = 2200950, .active_flag = 12207810}}));
    EXPECT_EQ(LampOfLightRequest(72410219),
              (std::optional<LampLight>{{.character = 2410969, .active_flag = 12418190}}));
    for (const s32 flag : {72200101, 72200220, 72100201, 74000200, 12207830, 0}) {
        EXPECT_EQ(LampOfLightRequest(flag), std::nullopt) << flag;
    }
}

// Kaniel's playtest 2026-10-20: krolebord lit the Witch's Abode lamp while she lived. His game's
// event 7100 ran on the request, lit nothing and ended, so the lamp never lit after her kill. A
// request for a lamp whose character is still off here (its boss lives) is refused.
TEST(BloodborneLamps, NoLightRequestWhileTheLampsBossLives) {
    EXPECT_TRUE(RefusesLampLight(72200201, true));
    EXPECT_FALSE(RefusesLampLight(72200201, false));
    EXPECT_FALSE(RefusesLampLight(72200201, std::nullopt)) << "no such character: as the game";
    EXPECT_FALSE(RefusesLampLight(72200101, true)) << "a lamp trip, not a light";
    EXPECT_FALSE(RefusesLampLight(12207830, true));
}

// A world with a light request on but its lamp not lit for this player (the request lit nothing,
// or came from another player's world) never lights it again: event 7100 ends at once on a load
// with the request on. A load of the lamp's map clears such requests, so the lamp lights again.
TEST(BloodborneLamps, ALoadClearsALightRequestThatLitNothing) {
    constexpr u32 Hemwick = 0x16000000;        // m22_00: map code 220
    constexpr u32 CathedralWard = 0x18010000;  // m24_01: 241
    constexpr u32 ForbiddenWoods = 0x17000000; // m23_00: 230
    const std::set<s32> on{72200201, 72200200, 12207810, 72410219, 12418190, 72300205};
    const auto is_on = [&](s32 flag) { return on.contains(flag); };
    EXPECT_EQ(LightRequestsToClear(Hemwick, is_on), (std::vector<s32>{72200201}))
        << "the Abode's request lit nothing; Charnel Lane's lamp is lit";
    EXPECT_EQ(LightRequestsToClear(ForbiddenWoods, is_on), (std::vector<s32>{72300205}));
    EXPECT_TRUE(LightRequestsToClear(CathedralWard, is_on).empty());
    EXPECT_TRUE(LightRequestsToClear(Hemwick, [](s32) { return false; }).empty());
    EXPECT_TRUE(LightRequestsToClear(Hemwick, [](s32) { return true; }).empty());
    EXPECT_TRUE(LightRequestsToClear(0x15000000, is_on).empty()) << "the Dream has no such lamps";
}

// The lamp tracer watches the byte holding one flag: block, byte and mask, MSB first.
TEST(BloodborneLamps, TheTracerFindsTheByteOfAFlag) {
    const FlagBit bit = FlagBitLocation(12417830, 1000);
    EXPECT_EQ(bit.block, 12417u);
    EXPECT_EQ(bit.byte, 103u);
    EXPECT_EQ(bit.mask, 0x02);
    EXPECT_EQ(FlagBitLocation(12417831, 1000).mask, 0x01);
    EXPECT_EQ(FlagBitLocation(8000, 1000).mask, 0x80);
}

// Registering a lamp (0x0146DA20, first time for this character) writes its 12 flags from base
// off: the host's game did that to a lamp a guest had lit. In a party the ones already on stay.
TEST(BloodborneLamps, RegisteringALampKeepsItLitInAParty) {
    EXPECT_EQ(LampFlagsKeptAtRegistration(true, 12417820, 0b1100'0000'0000),
              (std::vector<s32>{12417830, 12417831}));
    EXPECT_EQ(LampFlagsKeptAtRegistration(true, 12417820, 0b0000'0000'0101),
              (std::vector<s32>{12417820, 12417822}));
    EXPECT_TRUE(LampFlagsKeptAtRegistration(false, 12417820, 0b1100'0000'0000).empty());
    EXPECT_TRUE(LampFlagsKeptAtRegistration(true, -1, 0b1100'0000'0000).empty());
}

// A party guest's boss item (2003:36's award, returning to 0x0132E4B5) waits until the
// boss-clear sequence is over; other awards and other players are not touched.
TEST(BloodborneBossLoot, AGuestsBossItemArrivesAfterTheBossClear) {
    EXPECT_TRUE(DefersItemLotAward(true, true, ClientItemLotAwardReturn));
    EXPECT_FALSE(DefersItemLotAward(false, true, ClientItemLotAwardReturn));
    EXPECT_FALSE(DefersItemLotAward(true, false, ClientItemLotAwardReturn));
    EXPECT_FALSE(DefersItemLotAward(true, true, 0x0132E498));
}

// A guest's changes reach the host only for lamps and boss fog entries (the game syncs the rest
// itself while both are there): its trips (the host's lamp travelled too) and gifts (the host
// lost its own) stay out. Sending every map flag (2026-10-08) flooded the host with a new
// guest character's own world (a friend run: 20 flags at a time, the dog dead for both).
TEST(BloodborneSharedProgression, OnlyLampsGoFromGuestToHost) {
    EXPECT_FALSE(SharedWithParty(12410102)) << "a shortcut";
    EXPECT_FALSE(SharedWithParty(12401800)) << "a boss killed";
    EXPECT_TRUE(SharedWithParty(12417830));
    EXPECT_TRUE(SharedWithParty(12417831));
    EXPECT_FALSE(SharedWithParty(72410101)) << "lamp trip";
    EXPECT_FALSE(SharedWithParty(72410201)) << "lighting talk; the event's flags carry it";
    EXPECT_FALSE(SharedWithParty(6620)) << "messenger gift received";
    EXPECT_FALSE(SharedWithParty(12101020)) << "messenger gift taken";
    EXPECT_FALSE(SharedWithParty(12417832));
}

// A guest walking into a boss fog sets the boss's "host entered" flag (party_boss_events.py),
// which starts the boss; the guest's fog event is not synced, so the flag goes to the host over
// the party's board. It only holds for this visit, so it never goes into the guest's own world.
TEST(BloodborneBosses, AGuestEnteringTheBossFogStartsTheBossForTheHost) {
    EXPECT_TRUE(SharedWithParty(12414700)) << "Father Gascoigne";
    EXPECT_TRUE(SharedWithParty(12104800)) << "Gehrman";
    EXPECT_TRUE(SharedWithParty(13604808)) << "Orphan of Kos";
    EXPECT_FALSE(SharedWithParty(12414701)) << "a phantom entered (local music)";
    EXPECT_FALSE(CarriesToGuestWorld(12414700));
    EXPECT_FALSE(CarriesToGuestWorld(12414701));
    EXPECT_TRUE(CarriesToGuestWorld(12411700)) << "boss defeated";
}

// A boss start gives the boss a co-op HP effect (7500-7502: max HP x1.5/x2/x2.5). A boss still
// hidden by its entrance on one side (the other side started the fight) keeps its unscaled max
// HP there, so the two games' bars disagreed (Gascoigne: 2031 on the host, 3047 on the guest,
// and his second body outlived the kill). Such a boss is scaled once it shows the effect.
TEST(BloodborneBosses, ABossMissedByTheCoopHpBonusIsScaled) {
    EXPECT_EQ(CoopHpScale(2031, 2031, 2031, std::vector<s32>{7500}),
              (std::optional<std::pair<s32, s32>>{{3047, 3047}}));
    EXPECT_EQ(CoopHpScale(2000, 2000, 1000, std::vector<s32>{7501}),
              (std::optional<std::pair<s32, s32>>{{4000, 2000}}));
    EXPECT_EQ(CoopHpScale(2000, 3000, 3000, std::vector<s32>{7500}), std::nullopt)
        << "already scaled";
    EXPECT_EQ(CoopHpScale(2000, 2000, 2000, std::vector<s32>{}), std::nullopt) << "solo";
    EXPECT_EQ(CoopHpScale(2000, 2000, 0, std::vector<s32>{7500}), std::nullopt) << "dead";
    EXPECT_EQ(CoopHpScale(0, 2000, 2000, std::vector<s32>{7500}), std::nullopt)
        << "never seen without the effect";
}

// A guest's trip (to the Hunter's Dream, say) loads its own world while the session stays up,
// so the session's world changes go in on that load too: the lamp it lit at Gascoigne was
// missing from its headstone, the host's had it.
TEST(BloodborneGuestProgress, ATripLoadsTheSessionsWorldChangesToo) {
    EXPECT_TRUE(SetsGuestWorldChangesOnLoad(false, false)) << "home, solo";
    EXPECT_TRUE(SetsGuestWorldChangesOnLoad(true, true)) << "a party trip";
    EXPECT_FALSE(SetsGuestWorldChangesOnLoad(true, false)) << "the join's reload into the host";
}

// The trip's load can create map items more than once, and the 5 s throttle of the pickup
// pass skipped the Hunter's Dream load entirely (the Central Yharnam lamp was missing from the
// guest's headstone). The world changes go in once per trip, whatever the throttle.
TEST(BloodborneGuestProgress, EachTripGetsTheWorldChangesOnce) {
    OncePerTrip once;
    EXPECT_FALSE(once.Take(false, 0));
    EXPECT_TRUE(once.Take(true, 1));
    EXPECT_FALSE(once.Take(true, 1)) << "same trip";
    EXPECT_FALSE(once.Take(false, 1));
    EXPECT_TRUE(once.Take(true, 2)) << "the next trip";
    // Ada 2026-10-13: in a session every load is a trip, and the map-item hook that asks runs
    // only in loads, so it never saw a "no trip" between two: trips 2 and 3 took no host state.
    EXPECT_TRUE(once.Take(true, 3)) << "a trip right after a trip";
    EXPECT_FALSE(once.Take(true, 3));
}

// Ada 2026-10-13: Iosefka stood in the guest's Central Yharnam for 6 s after a trip. The host
// posted its board the moment its own load ended, before its story hid her, and the guest showed
// her again over its own story's hide. The host posts its characters once its world has settled.
TEST(BloodborneShownBoard, TheHostPostsOnlyASettledWorld) {
    EXPECT_FALSE(PostsShownState(10'000 + ShownSettleMs - 1, 10'000));
    EXPECT_TRUE(PostsShownState(10'000 + ShownSettleMs, 10'000));
    EXPECT_GE(ShownSettleMs, 6'000) << "the host's story took about 4 s";
}

// A party death wakes where the game says (GlobalState +0x1528): the events set it on a death
// (the first one: the Hunter's Dream 2102962; a kidnapping: the Gaol 2802959). In a loaded block
// the player is moved there, otherwise it travels; after that trip it never travels again.
TEST(BloodborneDeaths, APartyDeathWakesWhereTheGameSays) {
    EXPECT_EQ(ChooseDeathRespawn(2412951, true, false, false), DeathRespawn::MoveToPoint);
    EXPECT_EQ(ChooseDeathRespawn(2102962, false, false, false), DeathRespawn::Travel);
    EXPECT_EQ(ChooseDeathRespawn(2102962, false, true, false), DeathRespawn::StayPut);
    EXPECT_EQ(ChooseDeathRespawn(-1, false, false, false), DeathRespawn::StayPut);
    EXPECT_EQ(ChooseDeathRespawn(0, false, false, false), DeathRespawn::StayPut);
}

// SHADPS4_BLOODBORNE_TRACE_FLAG=<flag> points the flag tracer at one flag in a session.
TEST(BloodborneFlagTracer, TheTracedFlagComesFromTheSetting) {
    EXPECT_EQ(TracedFlagSetting("12101020"), std::optional<s32>{12101020});
    EXPECT_EQ(TracedFlagSetting(nullptr), std::nullopt);
    EXPECT_EQ(TracedFlagSetting(""), std::nullopt);
    EXPECT_EQ(TracedFlagSetting("lamp"), std::nullopt);
    EXPECT_EQ(TracedFlagSetting("-5"), std::nullopt);
}

// The game's "set this event flag" message from the other player (0x017C0570) copied the
// Hunter's Dream messengers' "gift given" flags, so one pick took the other's gift. In a party
// those stay as they are locally.
// NPC gifts are common event 9100, started per gift with its talk's trigger flag and item lot
// (common.emevd event 0); it ends unless the local player is alive, so a party guest's own talk
// set the trigger and got nothing (Kaniel 2026-10-13: Iosefka's blood vials). A guest's talk
// setting a trigger gets that gift (the host's own event gives the host its own).
TEST(BloodborneSharedLoot, AGuestsOwnTalkGivesItsNpcGift) {
    EXPECT_EQ(NpcGiftLot(72410310), std::optional<s32>{23000}) << "Iosefka's blood vials";
    EXPECT_EQ(NpcGiftLot(72410359), std::optional<s32>{24060});
    EXPECT_EQ(NpcGiftLot(72400398), std::optional<s32>{32020});
    EXPECT_EQ(NpcGiftLot(72400489), std::optional<s32>{28000});
    EXPECT_EQ(NpcGiftLot(9043), std::optional<s32>{17010});
    EXPECT_EQ(NpcGiftLot(72410300), std::nullopt) << "her talk's story, no gift";
    EXPECT_EQ(NpcGiftLot(0), std::nullopt);
    EXPECT_TRUE(GivesGuestNpcGift(true, true, 72410310));
    EXPECT_FALSE(GivesGuestNpcGift(true, false, 72410310)) << "the host's event gives its own";
    EXPECT_FALSE(GivesGuestNpcGift(false, true, 72410310)) << "alone, the game's event gives it";
    EXPECT_FALSE(GivesGuestNpcGift(true, true, 72410300));
}

// Kaniel 2026-10-13: "I hope this will work with all npc's". Every event that awards an item lot
// once an NPC's talk sets its trigger and ends unless the local player is alive (common 9100 and
// 9040, the maps' gift events) is in the table, with what the event does once it has given it.
TEST(BloodborneSharedLoot, EveryNpcTalkGiftReachesAGuest) {
    const auto gift = [](s32 trigger) { return NpcGiftFor(trigger); };
    // Common event 9100: gives again whenever the talk sets the trigger; clears it.
    ASSERT_TRUE(gift(72410310));
    EXPECT_EQ(gift(72410310)->after, NpcGiftAfter::ClearTrigger);
    // Common event 9040: once; its trigger stays on and ends the event (Gilbert's Flamesprayer,
    // Djura's Powder Keg Hunter Badge, Eileen's Bold Hunter's Mark, the little girl's music box).
    for (const auto [trigger, lot] :
         {std::pair{9066, 29000}, std::pair{9071, 33000}, std::pair{9074, 35000},
          std::pair{9067, 30000}, std::pair{9042, 17000}, std::pair{9077, 39010}}) {
        ASSERT_TRUE(gift(trigger)) << trigger;
        EXPECT_EQ(gift(trigger)->lot, lot) << trigger;
        EXPECT_EQ(gift(trigger)->after, NpcGiftAfter::KeepTrigger) << trigger;
    }
    // Map events that end once their own flag is on (the event id, or id + slot): the guest's
    // flag is set, else its own world's event gives the gift again at home.
    ASSERT_TRUE(gift(72400441));
    EXPECT_EQ(gift(72400441)->lot, 37000);
    EXPECT_EQ(gift(72400441)->after, NpcGiftAfter::SetDoneFlag);
    EXPECT_EQ(gift(72400441)->done_flag, 12400403);
    ASSERT_TRUE(gift(72100112)) << "the Doll's Tear Stone";
    EXPECT_EQ(gift(72100112)->done_flag, 12100123);
    ASSERT_TRUE(gift(73500322));
    EXPECT_EQ(gift(73500322)->done_flag, 13501922) << "event 13501920, slot 2";
    EXPECT_EQ(gift(73400424)->done_flag, 13400980) << "event 13400980, slot 0";
    // Map events like 9040 and like 9100.
    EXPECT_EQ(gift(72700321)->after, NpcGiftAfter::KeepTrigger);
    EXPECT_EQ(gift(73600300)->after, NpcGiftAfter::KeepTrigger);
    EXPECT_EQ(gift(73500320)->after, NpcGiftAfter::ClearTrigger) << "Adeline's blood";
    // Not gifts, or not decided by a talk (an NPC's death, Queenly Flesh's own event).
    EXPECT_FALSE(gift(72410300));
    EXPECT_FALSE(gift(1790));
    EXPECT_FALSE(gift(73400430)) << "Simon's bowblade waits on his death too";
    for (const NpcGift& each : NpcGifts) {
        EXPECT_EQ(std::ranges::count(NpcGifts, each.trigger, &NpcGift::trigger), 1) << each.trigger;
        EXPECT_GT(each.lot, 0);
        EXPECT_EQ(each.after == NpcGiftAfter::SetDoneFlag, each.done_flag != 0) << each.trigger;
    }
}

TEST(BloodborneSharedLoot, AMessengerGiftIsEachPlayersOwn) {
    EXPECT_TRUE(KeepsLocalFlag(true, 12101020));
    EXPECT_TRUE(KeepsLocalFlag(true, 12101029));
    EXPECT_FALSE(KeepsLocalFlag(false, 12101020));
    EXPECT_FALSE(KeepsLocalFlag(true, 12101030));
    EXPECT_FALSE(KeepsLocalFlag(true, 12417830)) << "a lamp is shared";
    // The pick in the messenger's talk (721010N0-N2 for messenger N) ended the other player's
    // gift event too when both stood there.
    EXPECT_TRUE(KeepsLocalFlag(true, 72101011));
    EXPECT_TRUE(KeepsLocalFlag(true, 72101092));
    EXPECT_FALSE(KeepsLocalFlag(true, 72101100));
}

// Kaniel 2026-10-11: in her own Hunter's Dream Ada found the workshop shut. Its doors open with
// flag 9403, which Central Yharnam's event 12410900 sets for a live player walking by; she went
// that way only as a phantom in the host's world. A guest out in the world in a session has been
// there, so it sets the Dream's flags for itself, and its world keeps them.
TEST(BloodborneSharedProgression, AGuestOutInTheWorldOpensItsOwnDream) {
    constexpr u32 CentralYharnam = 0x18010000;
    constexpr u32 HuntersDream = 0x15000000;
    EXPECT_EQ(GuestProgressFlags(true, true, CentralYharnam, true, true),
              (std::vector<s32>{9401, 9403}));
    EXPECT_TRUE(GuestProgressFlags(true, true, HuntersDream, true, true).empty())
        << "the Dream alone is no trip out";
    EXPECT_TRUE(GuestProgressFlags(true, false, CentralYharnam, true, true).empty())
        << "not in a session";
    EXPECT_TRUE(GuestProgressFlags(false, true, CentralYharnam, true, true).empty())
        << "the host's own";
    EXPECT_TRUE(GuestProgressFlags(true, true, 0, true, true).empty()) << "loading";
    // Ada's new character (2026-10-13): 9401 set before its first death showed the clinic's
    // lamp (common event 7000 waits for it) and ended the first-death event 9400 (end if 9401 on),
    // so the death woke it at the clinic, not in the Dream. Only a guest whose own character has
    // been to the Dream gets it.
    EXPECT_EQ(GuestProgressFlags(true, true, CentralYharnam, false, true),
              (std::vector<s32>{9403}));
    // Ada 2026-10-13: her new character's workshop in the Dream stood open. 9403 is what
    // Central Yharnam's event 12410900 sets for a live player walking by: only once the session's
    // world has that flag (the party walked there).
    EXPECT_EQ(GuestProgressFlags(true, true, CentralYharnam, true, false),
              (std::vector<s32>{9401}));
    for (const s32 flag : GuestProgressFlags(true, true, CentralYharnam, true, true)) {
        EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
    }
}

// Kaniel 2026-10-11: after the Cleric Beast the host had 4 insight, the guests 3: the kill's 3
// and not the 1 for finding the boss. That award (event 12411702) ends once flag 9300 is on, and
// the host's game sent its 9300 to the guests. Each player keeps its own "found this boss" flag,
// and a guest that saw the boss die in the session without one gets that insight then.
TEST(BloodborneSharedLoot, FindingABossIsEachPlayersOwn) {
    EXPECT_TRUE(KeepsLocalFlag(true, 9300)) << "the Cleric Beast";
    EXPECT_TRUE(KeepsLocalFlag(true, 9336)) << "Father Gascoigne";
    EXPECT_FALSE(KeepsLocalFlag(false, 9300));
    EXPECT_FALSE(KeepsLocalFlag(true, 9180)) << "the bell flags are another matter";
}

TEST(BloodborneSharedLoot, AGuestGetsTheInsightForFindingABossWhenItDies) {
    std::set<s32> on;
    const auto flag_on = [&](s32 flag) { return on.contains(flag); };
    EncounterInsightWatch watch;
    EXPECT_TRUE(watch.Tick(true, true, flag_on).empty());
    on.insert(12411700); // the Cleric Beast dies (its defeat event ran)
    const auto award = watch.Tick(true, true, flag_on);
    ASSERT_EQ(award.size(), 1u);
    EXPECT_EQ(award[0], (EncounterInsight{12411700, 9300, 1}));
    EXPECT_TRUE(watch.Tick(true, true, flag_on).empty()) << "once";

    EncounterInsightWatch found_before;
    on = {9336};
    found_before.Tick(true, true, flag_on);
    on.insert(12411800); // Gascoigne dies; this guest found him before
    EXPECT_TRUE(found_before.Tick(true, true, flag_on).empty());

    EncounterInsightWatch dead_already;
    on = {12411700};
    EXPECT_TRUE(dead_already.Tick(true, true, flag_on).empty()) << "dead before the session";
    EXPECT_TRUE(dead_already.Tick(true, true, flag_on).empty());

    EncounterInsightWatch host;
    on.clear();
    host.Tick(false, true, flag_on);
    on.insert(12411700);
    EXPECT_TRUE(host.Tick(false, true, flag_on).empty()) << "the host's own event gives it";
}

// Every member posts the lamps it lit and sets the others' (the game copies only a lamp's
// counters, so a lamp the host lit was missing from the guest's headstone). Only lighting
// travels: a lamp never goes out because another player's game cleared it.
TEST(BloodborneSharedProgression, LampsLitAnywhereAreLitForEveryone) {
    const auto shared = FlagsToShare(FlagChanges{{12417830, 12417831, 7116, 12101020}, {12417811}});
    EXPECT_EQ(shared, (std::vector<std::pair<s32, bool>>{{12417830, true}, {12417831, true}}));
}

// Friend run 2026-10-10: after the host opened the Cathedral Ward door (its cutscene reloads the
// world for the time of day) the guest's lamp read "entity missing" until it travelled on.
TEST(BloodborneLamps, APartyMemberUsesItsLampsBlockedOrMissing) {
    EXPECT_EQ(LampBytesToClear(true, false, false, true), (LampUnblock{true, false}));
    EXPECT_EQ(LampBytesToClear(true, true, false, true), (LampUnblock{true, true}));
    EXPECT_EQ(LampBytesToClear(false, true, false, true), (LampUnblock{false, true}));
    EXPECT_EQ(LampBytesToClear(true, true, true, false), (LampUnblock{true, false}))
        << "a multiplayer host outside a party: as the game's lamp fix always was";
    EXPECT_EQ(LampBytesToClear(true, true, false, false), LampUnblock{})
        << "nobody else's lamps change";
}

// A save slot's kept lists belong to the character that was there: once its pickups have been
// set in its own world, a load where none of them is set is a new character in that slot.
TEST(BloodborneGuestProgress, ANewCharacterInASlotStartsClean) {
    const std::vector<s32> confirmed{52410360, 52410120};
    EXPECT_TRUE(IsNewCharacterInSlot(confirmed, 0));
    EXPECT_FALSE(IsNewCharacterInSlot(confirmed, 1));
    EXPECT_FALSE(IsNewCharacterInSlot({}, 0)) << "nothing confirmed yet";
}

// Friend run 2026-10-10: a new character in the slot of one that never had a pickup confirmed
// in its own world got the old one's 9 kept pickups, so those items were gone from its world.
// Event flags only add up as a character plays: a slot loading with far fewer set than when its
// lists were kept holds another character.
TEST(BloodborneGuestProgress, AFreshCharacterHasFarFewerFlagsThanTheOneBefore) {
    EXPECT_TRUE(IsOtherCharacter(1200, 5400)) << "a new character";
    EXPECT_FALSE(IsOtherCharacter(5400, 5400));
    EXPECT_FALSE(IsOtherCharacter(5390, 5400)) << "a few flags cleared by events";
    EXPECT_FALSE(IsOtherCharacter(6000, 5400)) << "played on";
    EXPECT_FALSE(IsOtherCharacter(1200, 0)) << "nothing kept: no character to compare with";
    // Brom 2026-10-11: every restart read as a new character (5800 set against 5859 kept,
    // 5672 against 5739: a load clears a few dozen area flags), the kept pickups were dropped
    // and he could loot the same items again.
    EXPECT_FALSE(IsOtherCharacter(5800, 5859));
    EXPECT_FALSE(IsOtherCharacter(5672, 5739));
    EXPECT_FALSE(IsOtherCharacter(5150, 5400)) << "a load can clear a few hundred";
    // A store read mid-load counts nothing: no world has 0 flags set, so that is no character.
    EXPECT_FALSE(IsOtherCharacter(0, 5859));
}

// The count is read twice a moment apart; a read that missed part of the store (the game
// changing it on another thread, a node not mapped) gives none, and two reads far apart are a
// torn read: the count is known only when both read the whole store and agree.
TEST(BloodborneGuestProgress, AFlagCountIsKnownOnlyWhenTwoWholeReadsAgree) {
    EXPECT_EQ(SettledFlagCount(5800, 5803), std::optional<s32>{5803});
    EXPECT_EQ(SettledFlagCount(1200, 1201), std::optional<s32>{1201}) << "a new character";
    EXPECT_EQ(SettledFlagCount(std::nullopt, 5800), std::nullopt);
    EXPECT_EQ(SettledFlagCount(5800, std::nullopt), std::nullopt);
    EXPECT_EQ(SettledFlagCount(0, 0), std::nullopt) << "an empty store";
    EXPECT_EQ(SettledFlagCount(1200, 5800), std::nullopt) << "one read half done";
}

// A load drops a slot's kept lists (pickups, world changes) only on a known count: an unknown
// one keeps them, since dropping them by mistake loses what the player looted.
TEST(BloodborneGuestProgress, AnUnknownFlagCountNeverDropsTheKeptLists) {
    EXPECT_FALSE(ForgetsSlotLists(std::nullopt, 5859, false));
    EXPECT_FALSE(ForgetsSlotLists(std::nullopt, 5859, true))
        << "confirmed pickups read as missing while the store can't be read";
    EXPECT_TRUE(ForgetsSlotLists(1200, 5400, false)) << "a new character";
    EXPECT_TRUE(ForgetsSlotLists(5800, 5859, true)) << "its confirmed pickups are all gone";
    EXPECT_FALSE(ForgetsSlotLists(5800, 5859, false)) << "a restart";
}

// Before a slot's kept files are dropped they are copied aside, name.bak-<time>, and the
// newest few copies of each file stay.
TEST(BloodborneGuestProgress, DroppedListsAreCopiedAsideFirst) {
    EXPECT_EQ(SlotBackupName("bloodborne_party_progress_slot2.txt", "20261012-024501"),
              "bloodborne_party_progress_slot2.txt.bak-20261012-024501");
    const std::vector<std::string> names{
        "bloodborne_party_progress_slot2.txt.bak-20261012-010000",
        "bloodborne_party_progress_slot2.txt.bak-20261011-230000",
        "bloodborne_party_progress_slot2.txt",
        "bloodborne_party_progress_slot2.txt.bak-20261012-020000",
        "bloodborne_party_progress_slot3.txt.bak-20261001-000000",
        "bloodborne_party_world_slot2.txt.bak-20261001-000000",
        "bloodborne_party_progress_slot2.txt.bak-20261012-030000",
    };
    EXPECT_EQ(
        SlotBackupsToRemove("bloodborne_party_progress_slot2.txt", names, 2),
        (std::vector<std::string>{"bloodborne_party_progress_slot2.txt.bak-20261011-230000",
                                  "bloodborne_party_progress_slot2.txt.bak-20261012-010000"}));
    EXPECT_TRUE(SlotBackupsToRemove("bloodborne_party_progress_slot2.txt", names, 5).empty());
}

// The menu scripts' "am I a summoned client" answer (0x01A13FD1): a party guest's item dialog
// waited for a confirmation it never offered, so a party member answers "no".
TEST(BloodborneGuestMenus, PartyGuestsAreNotClientsToTheMenus) {
    EXPECT_TRUE(MenuSeesAClient(false, true));
    EXPECT_FALSE(MenuSeesAClient(true, true));
    EXPECT_FALSE(MenuSeesAClient(false, false));
}

// Kaniel 2026-10-11: the party died at Father Gascoigne, everyone quit and came back, and only
// the host found a bloodstain. A load shows a player's own stain only in its own world (0x017C243F
// tests GlobalState +0x1592), and a guest joined back in the host's world, so its stain (the
// record and its echoes, kept through the join) was never shown there. Its game shows it there.
TEST(BloodborneGuestBloodstain, AGuestSeesItsStainInTheHostsWorld) {
    constexpr u32 Ward = 0x18000000;
    EXPECT_TRUE(ShowsGuestBloodstain(true, true, false, true, 4087, Ward, Ward, false, 0));
    EXPECT_FALSE(ShowsGuestBloodstain(false, true, false, true, 4087, Ward, Ward, false, 0))
        << "the host's own world shows its stain itself";
    EXPECT_FALSE(ShowsGuestBloodstain(true, false, false, true, 4087, Ward, Ward, false, 0))
        << "out of a session the guest is in its own world";
    EXPECT_FALSE(ShowsGuestBloodstain(true, true, true, true, 4087, Ward, Ward, false, 0))
        << "loading";
    EXPECT_FALSE(ShowsGuestBloodstain(true, true, false, false, 4087, Ward, Ward, false, 0))
        << "no stain";
    EXPECT_FALSE(ShowsGuestBloodstain(true, true, false, true, 4087, 0x18010000, Ward, false, 0))
        << "the stain lies on another map";
    EXPECT_TRUE(ShowsGuestBloodstain(true, true, false, true, 4087, 0x18000001, Ward, false, 0))
        << "another block of the same map";
    EXPECT_FALSE(ShowsGuestBloodstain(true, true, false, true, 4087, Ward, Ward, true, 0))
        << "shown already";
    EXPECT_FALSE(ShowsGuestBloodstain(true, true, false, true, 4087, Ward, Ward, false,
                                      GuestBloodstainTries))
        << "the game would not show it: no more tries this visit";
}

// Kaniel 2026-10-12: the party killed a Wandering Nightmare (NpcParam 118020, item lot 11800200,
// drop flag 52300990) and only the host got its Blood Stone Shards. A guest's copy died by the
// host's kill (hidden or the game's kill, no drop). A member's kill of an enemy whose item lot
// drops once (a lot flag) gives the others that lot while their flag is off.
TEST(BloodborneSharedKills, AMembersKillGivesTheOneTimeDrop) {
    EXPECT_TRUE(GetsMemberKillDrop(true, true, 52300990, false));
    EXPECT_FALSE(GetsMemberKillDrop(true, true, 52300990, true)) << "got it already";
    EXPECT_FALSE(GetsMemberKillDrop(true, true, 0, false)) << "a random drop is each player's own";
    EXPECT_FALSE(GetsMemberKillDrop(true, false, 52300990, false))
        << "dead in the host's save, not killed by a member now";
    EXPECT_FALSE(GetsMemberKillDrop(false, true, 52300990, false)) << "no session";
}

// Kaniel 2026-10-13: Ada killed a hag by the Cathedral Ward lamp (item lot 22010, flag
// 50000600): the host's copy dropped the item, the guest's game made no drop at all. A guest's own
// kill gives it the one-time drop too; a host's own kill drops by the game.
TEST(BloodborneSharedKills, AGuestsOwnKillGivesTheOneTimeDrop) {
    EXPECT_TRUE(GetsKillDrop(true, false, true, 50000600, false));
    EXPECT_FALSE(GetsKillDrop(true, false, true, 50000600, true)) << "got it already";
    EXPECT_FALSE(GetsKillDrop(true, false, true, 0, false)) << "a random drop is the world's own";
    EXPECT_FALSE(GetsKillDrop(true, false, false, 50000600, false)) << "the host's own kill";
    EXPECT_FALSE(GetsKillDrop(false, false, true, 50000600, false)) << "no session";
    EXPECT_TRUE(GetsKillDrop(true, true, false, 50000600, false)) << "a member's kill, as before";
}

// Kaniel 2026-10-12: a broken Dream event patch opened the messengers' weapon lines (flags
// 12101000-12101009) whatever badges the player had, and those flags stay in the save. Once per
// save slot, out of the Dream, they are cleared; the next Dream visit opens the lines again for
// the badges the player has.
TEST(BloodborneDreamShop, ShopLinesAreRecheckedOncePerSlot) {
    EXPECT_EQ(ShopLineFlags().size(), 10u);
    EXPECT_EQ(ShopLineFlags().front(), 12101000);
    EXPECT_EQ(ShopLineFlags().back(), 12101009);
    constexpr u32 Ward = 0x18000000, Dream = 0x15000000;
    EXPECT_TRUE(RechecksShopLines(true, 2, false, Ward));
    EXPECT_FALSE(RechecksShopLines(true, 2, true, Ward)) << "done for this slot";
    EXPECT_FALSE(RechecksShopLines(true, 2, false, Dream)) << "in the Dream the lines are live";
    EXPECT_FALSE(RechecksShopLines(true, -1, false, Ward)) << "no character yet";
    EXPECT_FALSE(RechecksShopLines(false, 2, false, Ward)) << "no party";
    EXPECT_FALSE(RechecksShopLines(true, 2, false, 0)) << "loading";
}

// Kaniel 2026-10-12: the game stopped (no game-thread log for 30 s, the others' games dropped the
// session) and he closed the window; nothing said where it hung. Closing a game whose thread has
// not ticked for GameStallMs writes a freeze report.
TEST(BloodborneFreezeReport, ClosingAStalledGameReportsTheFreeze) {
    EXPECT_TRUE(GameThreadStalled(100'000, 100'000 - GameStallMs));
    EXPECT_FALSE(GameThreadStalled(100'000, 100'000 - GameStallMs + 1)) << "ticking";
    EXPECT_FALSE(GameThreadStalled(100'000, 0)) << "never ticked: the title screen, a load";
}

// Kaniel 2026-10-12: a guest went back for its bloodstain and it gave nothing. Its game had
// GameDataMan's stain mark on with no echoes in the record (-1, picked up already), and the
// guest's show drew that empty stain. The game shows a stain only with echoes (UpDateBloodMark
// passes echoes >= 0).
TEST(BloodborneGuestBloodstain, AStainWithoutEchoesIsNotShown) {
    constexpr u32 Woods = 0x17000000;
    EXPECT_FALSE(ShowsGuestBloodstain(true, true, false, true, -1, Woods, Woods, false, 0));
    EXPECT_FALSE(ShowsGuestBloodstain(true, true, false, true, 0, Woods, Woods, false, 0));
    EXPECT_TRUE(ShowsGuestBloodstain(true, true, false, true, 5266, Woods, Woods, false, 0));
}

// Kaniel 2026-10-12: an Old Yharnam hunter died twice in a row, then its body and its drop were
// gone. A member's kill reached the killer's own copy while that copy was in its death animation
// (HP 0, the death flags not set yet), so it was killed again and hidden. A member's kill applies
// only to a copy that is alive.
TEST(BloodborneSharedKills, AMembersKillSkipsACopyAlreadyDying) {
    EXPECT_TRUE(AppliesMemberKill(true, 430));
    EXPECT_FALSE(AppliesMemberKill(true, 0)) << "dying here already";
    EXPECT_FALSE(AppliesMemberKill(false, 430)) << "nobody killed it";
}

// Kaniel's v0.3.11 play log (2026-10-18 16:16): a guest's game crashed in the game's kill
// (0x018BF9A0, its death step reading a null part at +0x10). Central Yharnam's characters had
// just been made again in place (new characters, no load), and a member's kill reached a new copy
// near the guest 0.1 s after it was made. A member's kill waits until the copy has been in the
// world for NewCopySettlesMs.
TEST(BloodborneSharedKills, AMembersKillWaitsForACopyJustMade) {
    EXPECT_TRUE(CopyJustMade(100'000, 100'000 - 100)) << "made 0.1 s ago, as in the crash";
    EXPECT_TRUE(CopyJustMade(100'000, 100'000 - NewCopySettlesMs + 1));
    EXPECT_FALSE(CopyJustMade(100'000, 100'000 - NewCopySettlesMs)) << "settled";
}

TEST(BloodborneSignTimers, EncodesTheGamesOwnTimerStores) {
    EXPECT_EQ(EncodeSignTimerStore(0x47, 5.0f),
              (std::array<u8, 7>{0xC7, 0x47, 0x28, 0x00, 0x00, 0xA0, 0x40}));
    EXPECT_EQ(EncodeSignTimerStore(0x43, 30.0f),
              (std::array<u8, 7>{0xC7, 0x43, 0x28, 0x00, 0x00, 0xF0, 0x41}));
}

// ...so a party member's sign goes up at once and is re-posted every second. A guest only hears
// of a summon (the host's claim) when it re-posts its sign, while the host's connection reaches
// it at once, and its game drops a connection it cannot place after about 5 s: with a re-post
// every 5 s, a claim that came just after one was heard too late (the rig's dog test: the claim
// 73 ms after the guest's post, the guest's game closed the connection 5.2 s later, and the
// delivered claim found nothing to join).
TEST(BloodborneSignTimers, PostsTheFirstSignAtOnceAndRepostsEverySecond) {
    EXPECT_EQ(FirstSignPostDelaySeconds, 0.0f);
    EXPECT_EQ(SignRepostSeconds, 1.0f);
}

// The bundled "Uncap FPS++" game patch takes the re-post stores for frame timers and writes 60 s
// into them (0x01CA975A and 0x01CA9892 in its addresses): the sign then went up once a minute, a
// host's claim found the guest's game long past its 5 s, and Brom's join took many bells.
TEST(BloodborneSignTimers, TakesTheStoresWhateverTimeAGamePatchWroteInThem) {
    const SignTimerSite& repost = SignTimerSites[2];
    const auto game = EncodeSignTimerStore(repost.modrm, repost.original_seconds);
    const auto uncapped = EncodeSignTimerStore(repost.modrm, 60.0f);
    EXPECT_TRUE(IsSignTimerStore(game, repost));
    EXPECT_TRUE(IsSignTimerStore(uncapped, repost));

    auto other_register = game;
    other_register[1] = 0x47;
    EXPECT_FALSE(IsSignTimerStore(other_register, repost));
    auto other_field = game;
    other_field[2] = 0x30;
    EXPECT_FALSE(IsSignTimerStore(other_field, repost));
    EXPECT_FALSE(IsSignTimerStore(std::span<const u8>{game}.first(6), repost));
}

// 01.09 runs the event scripts' death flow natively (global_event.lua in eventcommon.luabnd is
// only the source it was built from). When a white phantom dies, PartyGhostDeath broadcasts
// event 4063, which makes the host drop the guest from the session (OnGameLeave), and
// PartyGhostDeath_1 opens the "returned to your world" menu that leads to the warp home.
TEST(BloodborneGhostDeath, KeepsTheSessionByDroppingTheLeaveBroadcastAndItsFlags) {
    const auto& patches = GhostDeathKeepSessionPatches();
    ASSERT_EQ(patches.size(), 5u);
    for (const auto& patch : patches) {
        EXPECT_EQ(patch.original.size(), patch.patched.size()) << std::hex << patch.offset;
    }
    // The three calls: SetEventFlag(4047) twice and CustomLuaCallStart(4063, player).
    const std::array<std::pair<u64, u64>, 3> calls{{
        {0x01381FE7, PartyGhostSetEventFlagOffset},
        {0x01381FF6, PartyGhostSetEventFlagOffset},
        {0x0138201B, PartyGhostCustomLuaCallStartOffset},
    }};
    for (std::size_t i = 0; i < calls.size(); ++i) {
        EXPECT_EQ(patches[i].offset, calls[i].first);
        EXPECT_EQ(DecodeNearCallTarget(patches[i].offset, patches[i].original), calls[i].second);
        EXPECT_EQ(patches[i].patched, (std::vector<u8>{0x0F, 0x1F, 0x44, 0x00, 0x00}));
    }
    // SetFlagInitState(2), inlined as `mov dword ptr [rax], 2`.
    EXPECT_EQ(patches[3].original, (std::vector<u8>{0xC7, 0x00, 0x02, 0x00, 0x00, 0x00}));
    EXPECT_EQ(patches[3].patched, (std::vector<u8>{0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}));
    // PartyGhostDeath_1: `if not ClearBoss then <menu, then warp home>` always skips the menu.
    EXPECT_EQ(patches[4].offset, PartyGhostDeathMenuBranchOffset);
    EXPECT_EQ(patches[4].original, (std::vector<u8>{0x75, 0x2C}));
    EXPECT_EQ(patches[4].patched, (std::vector<u8>{0xEB, 0x2C}));
}

TEST(BloodborneGhostDeath, DecodesOnlyNearCalls) {
    EXPECT_EQ(DecodeNearCallTarget(0x1000, std::vector<u8>{0xE8, 0x00, 0x00, 0x00, 0x00}), 0x1005u);
    EXPECT_EQ(DecodeNearCallTarget(0x1000, std::vector<u8>{0xE8, 0xFB, 0xFF, 0xFF, 0xFF}), 0x1000u);
    EXPECT_EQ(DecodeNearCallTarget(0x1000, std::vector<u8>{0xE9, 0x00, 0x00, 0x00, 0x00}), 0u);
    EXPECT_EQ(DecodeNearCallTarget(0x1000, std::vector<u8>{0xE8, 0x00}), 0u);
}

namespace {

std::vector<std::string> revive_calls;
bool white_ghost = true;

bool PS4_SYSV_ABI FakeIsWhiteGhost() {
    revive_calls.push_back("IsWhiteGhost");
    return white_ghost;
}
bool PS4_SYSV_ABI FakeSetEventFlag(u64, s32 flag, bool value) {
    revive_calls.push_back("SetEventFlag " + std::to_string(flag) + (value ? " on" : " off"));
    return true;
}
bool PS4_SYSV_ABI FakeRequestFullRecover() {
    revive_calls.push_back("RequestFullRecover");
    return true;
}
bool PS4_SYSV_ABI FakeUpDateBloodMark() {
    revive_calls.push_back("UpDateBloodMark");
    return true;
}
bool PS4_SYSV_ABI FakeRevivePlayer() {
    revive_calls.push_back("RevivePlayer");
    return true;
}
void FakeClearReturnState() {
    revive_calls.push_back("ClearReturnState");
}
void PS4_SYSV_ABI FakeReviveLocalCharacter() {
    revive_calls.push_back("ReviveLocalCharacter");
}
void FakeRestoreAliveState() {
    revive_calls.push_back("RestoreAliveState");
}
void PS4_SYSV_ABI FakeSetChrType(u64, s32 entity, s32 type) {
    revive_calls.push_back("SetChrType " + std::to_string(entity) + " " + std::to_string(type));
}
void PS4_SYSV_ABI FakeChrResetAnimation(u64, s32 entity) {
    revive_calls.push_back("ChrResetAnimation " + std::to_string(entity));
}
void FakeWarpToLastLamp() {
    revive_calls.push_back("WarpToLastLamp");
}

constexpr GhostDeathCalls FakeCalls{
    .is_white_ghost = FakeIsWhiteGhost,
    .set_event_flag = FakeSetEventFlag,
    .clear_return_state = FakeClearReturnState,
    .request_full_recover = FakeRequestFullRecover,
    .update_blood_mark = FakeUpDateBloodMark,
    .revive_player = FakeRevivePlayer,
    .revive_local_character = FakeReviveLocalCharacter,
    .restore_alive_state = FakeRestoreAliveState,
    .set_chr_type = FakeSetChrType,
    .chr_reset_animation = FakeChrResetAnimation,
    .warp_to_last_lamp = FakeWarpToLastLamp,
};

} // namespace

// Where the game would open that menu, a party guest gets up again instead: the death event
// (4000) and the "returning to its world" state OnEvent_4000 sets ([SprjEventMan+0x10]+8 = 4;
// any death is ignored while it is above 0, and only a load resets it) are cleared so the next
// death runs, the guest drops its echoes in a bloodstain the way a
// host death does (UpDateBloodMark) and is revived in place. RevivePlayer alone leaves the
// character in its death pose and makes it a live player (type 0), so the white phantom type is
// set back (SetChrType also refreshes the character) and its behaviour is reset to idle.
// RevivePlayer also leaves what the kill and the death processing set (the dead bit at +0x1E3 and
// the action state the death sets): play-testing showed a guest that could attack but not move,
// took no damage and that enemies ignored, so the state saved while it was alive goes back. The
// lock itself is the death's controller flags ([+0x58]: +0x114 bit 0, [+0x38]+0x48 bit 0), which
// 01.09's own local-player revive (0x013179B0) clears along with refilling HP.
// (A summon reload instead needs the host's side of the summon handshake and drops the session.)
// Last, like a death in its own world, it is moved to the last lamp it rested at, which works
// whenever that lamp is in the map the host's world has loaded; otherwise it stays where it fell.
TEST(BloodborneGhostDeath, ADeadPartyGuestRevivesWhereItFell) {
    revive_calls.clear();
    white_ghost = true;
    EXPECT_TRUE(ReviveDeadPartyGuest(FakeCalls));
    EXPECT_EQ(revive_calls, (std::vector<std::string>{
                                "IsWhiteGhost",
                                "SetEventFlag 4000 off",
                                "ClearReturnState",
                                "RequestFullRecover",
                                "UpDateBloodMark",
                                "RevivePlayer",
                                "ReviveLocalCharacter",
                                "RestoreAliveState",
                                "SetChrType 10000 1",
                                "ChrResetAnimation 10000",
                                "WarpToLastLamp",
                            }));
}

TEST(BloodborneGhostDeath, LeavesEveryoneElseAlone) {
    revive_calls.clear();
    white_ghost = false;
    EXPECT_FALSE(ReviveDeadPartyGuest(FakeCalls));
    EXPECT_EQ(revive_calls, std::vector<std::string>{"IsWhiteGhost"});

    revive_calls.clear();
    GhostDeathCalls missing = FakeCalls;
    missing.chr_reset_animation = nullptr;
    EXPECT_FALSE(ReviveDeadPartyGuest(missing));
    EXPECT_TRUE(revive_calls.empty());
}

namespace {

std::vector<std::string> placement_calls;

std::string Vector(const float* value) {
    return std::to_string(static_cast<int>(value[0])) + "," +
           std::to_string(static_cast<int>(value[1])) + "," +
           std::to_string(static_cast<int>(value[2]));
}
void PS4_SYSV_ABI FakeTeleport(u64 character, const float* position, const float* rotation,
                               s32 mode) {
    placement_calls.push_back("Teleport " + std::to_string(character) + " " + Vector(position) +
                              " " + Vector(rotation) + " mode " + std::to_string(mode));
}
void PS4_SYSV_ABI FakeSnapPosition(u64 character, const float* position) {
    placement_calls.push_back("SnapPosition " + std::to_string(character) + " " + Vector(position));
}
void FakeSetRotation(u64 character, const float* rotation) {
    placement_calls.push_back("SetRotation " + std::to_string(character) + " " + Vector(rotation));
}
void PS4_SYSV_ABI FakeResetFollow(u64 character, bool placed) {
    placement_calls.push_back("ResetFollow " + std::to_string(character) +
                              (placed ? " placed" : ""));
}
void FakeSnapCamera() {
    placement_calls.push_back("SnapCamera");
}

constexpr PlacementCalls FakePlacement{
    .teleport = FakeTeleport,
    .snap_position = FakeSnapPosition,
    .set_rotation = FakeSetRotation,
    .reset_follow = FakeResetFollow,
    .snap_camera = FakeSnapCamera,
};

} // namespace

// Kaniel 2026-10-12: after every trip the camera flew in from a random spot to the player. A
// guest's trip arrives at the map's origin and is moved to its lamp a second later; the move was
// only the teleport, so the camera stayed behind and eased over. The game's own placement at a
// lamp snaps the position (and the one before it) and resets the follow right after it. Kaniel
// still saw it fly in with those: the camera itself eases in unless the camera manager snaps it,
// as the game has it do when a map starts.
TEST(BloodbornePlacement, AMoveSnapsLikeTheGamesOwnPlacement) {
    placement_calls.clear();
    alignas(16) const std::array<float, 4> position{-199.0f, -50.0f, 179.0f, 1.0f};
    alignas(16) const std::array<float, 4> rotation{0.0f, 2.0f, 0.0f, 0.0f};
    EXPECT_TRUE(PlaceCharacter(FakePlacement, 0x20000, position.data(), rotation.data()));
    EXPECT_EQ(placement_calls, (std::vector<std::string>{
                                   "Teleport 131072 -199,-50,179 0,2,0 mode 1",
                                   "SnapPosition 131072 -199,-50,179",
                                   "SetRotation 131072 0,2,0",
                                   "ResetFollow 131072 placed",
                                   "SnapCamera",
                               }));
}

TEST(BloodbornePlacement, NothingMovesWithoutEveryCall) {
    alignas(16) const std::array<float, 4> position{1.0f, 2.0f, 3.0f, 1.0f};
    alignas(16) const std::array<float, 4> rotation{};
    std::array<PlacementCalls, 5> missing;
    missing.fill(FakePlacement);
    missing[0].teleport = nullptr;
    missing[1].snap_position = nullptr;
    missing[2].set_rotation = nullptr;
    missing[3].reset_follow = nullptr;
    missing[4].snap_camera = nullptr;
    for (const PlacementCalls& calls : missing) {
        placement_calls.clear();
        EXPECT_FALSE(PlaceCharacter(calls, 0x20000, position.data(), rotation.data()));
        EXPECT_TRUE(placement_calls.empty());
    }
}

// Kaniel 2026-10-12: after a trip, textures were still loading on arrival. A guest's lamp trip
// landed at the map's origin (rig: Cathedral Ward at 0, 0, 0) and was moved 270 m to its lamp a
// second later, so the area that loaded first was the wrong one. The lamps' respawn points from
// the map files let the trip land at the lamp.
TEST(BloodborneLampPoints, ALampTripKnowsWhereTheLampIs) {
    const LampPoint* ward = FindLampPoint(2412950); // Cathedral Ward, the rig's trip
    ASSERT_NE(ward, nullptr);
    EXPECT_EQ(ward->packed_map, 0x18010000u);
    EXPECT_FLOAT_EQ(ward->x, -199.74f);
    EXPECT_FLOAT_EQ(ward->y, -50.76f);
    EXPECT_FLOAT_EQ(ward->z, 179.42f);
    const LampPoint* dream = FindLampPoint(HuntersDreamWarpPoint);
    ASSERT_NE(dream, nullptr);
    EXPECT_FLOAT_EQ(dream->heading, -2.356f); // what the game's resolver gave the rig
    EXPECT_EQ(FindLampPoint(0), nullptr);
    EXPECT_EQ(FindLampPoint(2412999), nullptr);
}

// The trip's stage comes from the warp point (PackedMapOfWarpPoint); the point must be in it.
TEST(BloodborneLampPoints, EveryPointIsOnTheMapItsWarpLoads) {
    for (const LampPoint& point : LampPoints) {
        EXPECT_EQ(PackedMapOfWarpPoint(point.warp_point), point.packed_map) << point.warp_point;
        EXPECT_EQ(FindLampPoint(point.warp_point), &point);
    }
}

TEST(BloodborneLampPoints, OnlyAnArrivalAwayFromTheLampIsMoved) {
    const LampPoint* ward = FindLampPoint(2412950);
    EXPECT_FALSE(ArrivalNeedsMove(ward, LampPosition{-199.7f, -50.7f, 179.4f}));
    EXPECT_FALSE(ArrivalNeedsMove(ward, LampPosition{-198.0f, -50.0f, 180.5f}));
    EXPECT_TRUE(ArrivalNeedsMove(ward, LampPosition{0.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(ArrivalNeedsMove(ward, std::nullopt));
    EXPECT_TRUE(ArrivalNeedsMove(nullptr, LampPosition{-199.7f, -50.7f, 179.4f}));
}

// When the host dies, every guest runs HostDead (event 4000 param 1): for a white phantom it
// shows MissionFailed and five seconds later warps home (HostDead_1). In seamless mode the white
// phantom branch returns straight away, so the guests stay in the session.
TEST(BloodborneHostDeath, GuestsIgnoreTheHostsDeath) {
    const auto& patches = HostDeathKeepGuestsPatches();
    ASSERT_EQ(patches.size(), 2u);
    const auto& patch = patches[0];
    EXPECT_EQ(patch.offset, 0x013819C6u);
    // The branch starts with SetEventFlag(4047, true): `mov esi, 0xFCF`.
    EXPECT_EQ(patch.original, (std::vector<u8>{0xBE, 0xCF, 0x0F, 0x00, 0x00}));
    EXPECT_EQ(DecodeNearJumpTarget(patch.offset, patch.patched), HostDeadEpilogueOffset);
}

TEST(BloodborneHostDeath, DecodesOnlyNearJumps) {
    EXPECT_EQ(DecodeNearJumpTarget(0x1000, std::vector<u8>{0xE9, 0x00, 0x00, 0x00, 0x00}), 0x1005u);
    EXPECT_EQ(DecodeNearJumpTarget(0x1000, std::vector<u8>{0xE8, 0x00, 0x00, 0x00, 0x00}), 0u);
}

// The alive state to put back is captured from the guest itself while it is alive, since the
// values the death overwrites are not constants.
TEST(BloodborneAliveState, KeepsTheLastStateSeenWhileAlive) {
    AliveStateSnapshot snapshot;
    EXPECT_FALSE(snapshot.Saved().has_value());

    snapshot.Observe(true, AliveState{.flags = 0x11, .action = 0, .action_sub = 1});
    snapshot.Observe(true, AliveState{.flags = 0x22, .action = 0, .action_sub = 1});
    snapshot.Observe(false, AliveState{.flags = 0x23, .action = 2, .action_sub = 2}); // dying
    ASSERT_TRUE(snapshot.Saved().has_value());
    EXPECT_EQ(snapshot.Saved()->flags, 0x22u);
    EXPECT_EQ(snapshot.Saved()->action, 0);
    EXPECT_EQ(snapshot.Saved()->action_sub, 1);
}

// A dead guest's bloodstain in the host's world: the native g_Initialize registers the
// self-bloodstain pickup (event 4032, SelfBloodMark) only for ATTR_LIVE | ATTR_GREY (0x30), so a
// white phantom could never touch it. ATTR_WHITE (0x40) is added.
TEST(BloodborneGuestBloodstain, LetsAWhitePhantomPickUpItsOwnBloodstain) {
    const auto& patches = GuestBloodstainPatches();
    ASSERT_EQ(patches.size(), 1u);
    EXPECT_EQ(patches[0].offset, 0x0137EBC7u);
    EXPECT_EQ(patches[0].original, (std::vector<u8>{0xB9, 0x30, 0x00, 0x00, 0x00}));
    EXPECT_EQ(patches[0].patched, (std::vector<u8>{0xB9, 0x70, 0x00, 0x00, 0x00}));
}

// No echo theft, everywhere (Kaniel): the steal's `je` (to the store of the stolen flag) is
// nopped, so it always returns without taking the bloodstain.
TEST(BloodborneGuestBloodstain, NoEnemyStealsTheBloodstain) {
    const auto& patches = NoEchoTheftPatches();
    ASSERT_EQ(patches.size(), 1u);
    EXPECT_EQ(patches[0].offset, 0x018E7613u);
    EXPECT_EQ(patches[0].original, (std::vector<u8>{0x74, 0x09}));
    EXPECT_EQ(patches[0].patched, (std::vector<u8>{0x90, 0x90}));
}

// Host death (Kaniel): like a guest's, with no world reload. Native SoloPlayDeath_2 (the death
// respawn that reloads the world at the lamp) is replaced for a host with a party: same echo
// drop, revive and lamp teleport as a guest, but it stays a live player and is not checked for
// being a white phantom.
TEST(BloodborneHostDeath, AHostWithAPartyRevivesLikeAGuestWithoutAReload) {
    revive_calls.clear();
    white_ghost = false;
    ReviveDeadPartyHost(FakeCalls);
    EXPECT_EQ(revive_calls, (std::vector<std::string>{
                                "SetEventFlag 4000 off",
                                "ClearReturnState",
                                "RequestFullRecover",
                                "UpDateBloodMark",
                                "RevivePlayer",
                                "ReviveLocalCharacter",
                                "RestoreAliveState",
                                "ChrResetAnimation 10000",
                                "WarpToLastLamp",
                            }));
}

// Only while hosting: a solo player (or a host whose room is gone) dies the game's way.
// A guest that travelled is a live player (type 0), so its death takes the live player's path
// (SoloPlayDeath_2, a reload at the lamp) too; it revives in place like the host.
TEST(BloodborneHostDeath, OnlyAPlayerInASessionSkipsTheReload) {
    EXPECT_TRUE(ShouldReviveHostInPlace(MatchingStateHosting));
    EXPECT_TRUE(ShouldReviveHostInPlace(MatchingStateSummonedGuest));
    for (const s32 state : {0, 1, 2, 4, 5, 7}) {
        EXPECT_FALSE(ShouldReviveHostInPlace(state)) << "state " << state;
    }
}

// Play-test: after a host death the guest still saw the host, but the host no longer had the
// guest's character. The character death step (0x018C49F0) walks the session's members when the
// local player is hosting (state 3) and removes them, since unpatched the host is about to
// reload. With no reload that branch is skipped: `jne` past it becomes a plain `jmp`.
TEST(BloodborneHostDeath, TheHostKeepsItsGuestsCharactersWhenItDies) {
    const auto& patch = HostDeathKeepGuestsPatches().at(1);
    EXPECT_EQ(patch.offset, 0x018C4D1Du);
    EXPECT_EQ(patch.original, (std::vector<u8>{0x0F, 0x85, 0x97, 0x00, 0x00, 0x00}));
    EXPECT_EQ(DecodeNearJumpTarget(patch.offset, std::span{patch.patched}.first(5)), 0x018C4DBAu);
    EXPECT_EQ(patch.patched.back(), 0x90);
}

// Random crashes left logs that just stop (the logger never flushed). The crash catcher writes
// one line itself: the exception, where it happened and, inside the game, the eboot offset that
// the RE notes use.
TEST(BloodborneCrashes, TheCrashLineNamesTheGameOffset) {
    EXPECT_EQ(FormatCrashLine(0xC0000005, 0x8018BF9A0, 0x800000000, 0x50DC000, "Game:Main"),
              "crash: exception 0xc0000005 at 0x8018bf9a0 (eboot+0x18bf9a0) thread Game:Main");
    EXPECT_EQ(FormatCrashLine(0xC0000005, 0x7FF612345678, 0x800000000, 0x50DC000, ""),
              "crash: exception 0xc0000005 at 0x7ff612345678 (emulator) thread ?");
}

// The host summoned the guest a second time 48 s after both closed their connection; the host's
// emulator reused the lingering connection and the guest's game never heard the invite. And in
// the rig's guest-crash test the guest's game died without closing anything: the host's game
// closed its connection, opened it again for the restarted guest, got the old one back, and the
// new guest never got a handshake. A connection this game closed is never reused.
TEST(BloodborneSignaling, AConnectionThisGameClosedIsNotReused) {
    using Libraries::Np::NpSignaling::LingeringConnectionIsFinished;
    EXPECT_TRUE(LingeringConnectionIsFinished(true, true, true, false));
    EXPECT_TRUE(LingeringConnectionIsFinished(true, true, false, false))
        << "the peer never closed it (its game died)";
    EXPECT_FALSE(LingeringConnectionIsFinished(true, false, true, false)) << "we are still on";
    EXPECT_FALSE(LingeringConnectionIsFinished(false, true, true, false)) << "never established";
    EXPECT_FALSE(LingeringConnectionIsFinished(true, true, true, true)) << "already dead";
}

// The guest's stack is not in the minidump (it is not a host thread stack), so the report
// lists the eboot addresses found on it: the return addresses of the crashing call chain.
TEST(BloodborneCrashes, TheReportListsTheGameAddressesOnTheStack) {
    const std::vector<u64> stack{0x30FD32890, 0x8018E6D48, 0x7EFB4E310, 0x801A2BFA8, 0x900000000};
    EXPECT_EQ(FormatGameAddresses(stack, 0x800000000, 0x50DC000, 8),
              " eboot+0x18e6d48 eboot+0x1a2bfa8");
    EXPECT_EQ(FormatGameAddresses(stack, 0x800000000, 0x50DC000, 1), " eboot+0x18e6d48");
}

// Side by side the game shares kill echoes itself; apart (a guest alone in the Cathedral Ward,
// the host in Central Yharnam) only the killer got them. A kill made with no other member on the
// map posts its echoes to the party's echo board, and the others add them.
TEST(BloodborneSharedEchoes, KillsMadeApartRewardTheWholeParty) {
    EXPECT_TRUE(SharesKillEchoes(0x018FC99B, true)) << "an enemy kill";
    EXPECT_TRUE(SharesKillEchoes(0x01385263, true)) << "a boss kill";
    EXPECT_FALSE(SharesKillEchoes(0x018FC99B, false)) << "side by side the game shares them";
    EXPECT_FALSE(SharesKillEchoes(0x0132E4B5, true)) << "not a kill (an item, a bloodstain)";
    EXPECT_EQ(EncodeEchoUpdate("guest", 3, std::vector<KillEchoes>{{120, 0}, {4000, 0}}),
              "guest 3 120 4000");
    EXPECT_EQ(EncodeEchoUpdate("host", 0, std::vector<KillEchoes>{}), "host 0");
    EXPECT_EQ(ParseEchoBoard("5\n120\n4000\n"),
              (std::optional<EchoBoardAnswer>{EchoBoardAnswer{5, {{120, 0}, {4000, 0}}}}));
    EXPECT_EQ(ParseEchoBoard("2\n"), (std::optional<EchoBoardAnswer>{EchoBoardAnswer{2, {}}}));
    EXPECT_EQ(ParseEchoBoard("x\n"), std::nullopt);
    EXPECT_EQ(ParseEchoBoard("2\n-5\n"), std::nullopt);
}

// Lamps, boss fog entries and trophies go between the members whenever they are a party, not
// only while in a session: a guest on its own trip (the Hunter's Dream) is out of one, and
// what the host did meanwhile only reached it after rejoining, if at all.
TEST(BloodborneSharedProgression, ThePartySharesInAndOutOfASession) {
    EXPECT_TRUE(SharesWithParty(true, true));
    EXPECT_FALSE(SharesWithParty(true, false)) << "loading: no player to apply them to";
    EXPECT_FALSE(SharesWithParty(false, true)) << "no party";
}

// A member arriving at a map loads it with every enemy alive; the session only carries what
// happens while both are there, so the wolf the host had killed stood alive on the guest's side
// (a leftover only the guest could fight). Every member posts the enemies it killed during its
// visit to a map (handles are the same in every game), and takes them back when the visit ends.
TEST(BloodborneSharedKills, AMembersKillsLastForItsVisit) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    const std::vector<SeenCharacter> alive{{0x10014085, 300}, {0x10014088, 250}};
    EXPECT_EQ(kills.Observe(1, alive), Changes{});
    const std::vector<SeenCharacter> one_dead{{0x10014085, 0}, {0x10014088, 250}};
    EXPECT_EQ(kills.Observe(1, one_dead), (Changes{{0x10014085, true}}));
    EXPECT_EQ(kills.Observe(1, one_dead), Changes{}) << "posted once";
    // A new visit (a trip, a reload): its kills there respawn.
    EXPECT_EQ(kills.Observe(2, alive), (Changes{{0x10014085, false}}));
    EXPECT_EQ(kills.Observe(2, alive), Changes{});
    // Seen dead from the start of a visit (already dead when it loaded): not this visit's kill.
    VisitKills fresh;
    EXPECT_EQ(fresh.Observe(3, one_dead), Changes{});
    EXPECT_EQ(fresh.Observe(3, one_dead), Changes{});
}

// Kaniel's playtest 2026-10-19 and the Witch of Hemwick: a map generator (the chalice widow's
// skeletons, the witch's Mad Ones) brings its dead enemies back, the same characters in their
// slots. A kill of one that is alive again is taken back, so the other games show it again.
TEST(BloodborneSharedKills, AnEnemyBroughtBackTakesItsKillBack) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    constexpr u32 Skeleton = 0x1002c030;
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{Skeleton, 398, 7}}), Changes{});
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{Skeleton, 0, 7}}),
              (Changes{{Skeleton, true}}));
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{Skeleton, 398, 7}}),
              (Changes{{Skeleton, false}}))
        << "the generator brought it back";
    EXPECT_TRUE(kills.Killed().empty());
    EXPECT_TRUE(kills.Standing().empty());
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{Skeleton, 0, 7}}),
              (Changes{{Skeleton, true}}))
        << "killed again";
    // Another object in the slot (the world loaded again) is no revive; nor a read while the
    // copy settles after a takeover.
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{Skeleton, 398, 8}}), Changes{});
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{Skeleton, 0, 8}}), Changes{})
        << "already this visit's kill";
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{Skeleton, 398, 8, true}}), Changes{});
}

// Rig smoke 2026-10-12 (run 143240): Father Gascoigne (0x1001409c/0x1001409e), dead in the
// guests' saves, was put down by their worlds 5.7 s after the join's load, past the settling
// time, and both guests posted him; every other game then hid his body, and the second guest
// hid 3 enemies where the host had killed 1. He went down at full HP: a boss-sized character
// dying with no damage seen before is its world putting it down, not a kill. Big ordinary
// enemies (late game, co-op scaled) hit to death still are.
// Kaniel's playtest 2026-10-17: Central Yharnam kills, the host gone to the Dream, both guests in
// Cathedral Ward (Central Yharnam's enemies loaded there too): the kills stand.
TEST(BloodborneSharedKills, KillsStandWhileSomeoneIsOnAMapLoadedWithTheirs) {
    constexpr u32 Ward = 0x18000000, Central = 0x18010000, Dream = 0x15000000;
    using Changes = std::vector<std::pair<s32, bool>>;
    const u64 visit = u64{Central} << 32 | 1;
    VisitKills kills;
    kills.Observe(visit, std::vector<SeenCharacter>{{0x10014007, 300}}, {}, Central);
    EXPECT_EQ(kills.Observe(visit, std::vector<SeenCharacter>{{0x10014007, 0}}, {}, Central),
              (Changes{{0x10014007, true}}));
    const std::vector<u32> guests_in_the_ward{Ward, Ward};
    EXPECT_EQ(kills.Observe(u64{Dream} << 32 | 2, {}, guests_in_the_ward, Dream), Changes{})
        << "held: someone is where Central Yharnam is loaded";
    const std::vector<u32> guests_elsewhere{0x16000000, 0x16000000};
    EXPECT_EQ(kills.Observe(u64{Dream} << 32 | 2, {}, guests_elsewhere, Dream),
              (Changes{{0x10014007, false}}))
        << "nobody there any more: it resets";
    EXPECT_TRUE(MapsLoadedTogether(Ward, Central));
    EXPECT_TRUE(MapsLoadedTogether(Central, Ward));
    EXPECT_FALSE(MapsLoadedTogether(Central, Dream));
}

TEST(BloodborneSharedKills, ABossSizedPutDownIsNoKill) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    const std::vector<SeenCharacter> alive{{0x1001409c, 2031, 0xA000, false, true},
                                           {0x1001400b, 484, 0xB000}};
    EXPECT_EQ(kills.Observe(1, alive), Changes{});
    const std::vector<SeenCharacter> both_dead{{0x1001409c, 0, 0xA000, false, false},
                                               {0x1001400b, 0, 0xB000}};
    EXPECT_EQ(kills.Observe(1, both_dead), (Changes{{0x1001400b, true}}))
        << "the ordinary enemy only";
    EXPECT_EQ(kills.Killed(), (std::vector<u32>{0x1001400b}));
}

TEST(BloodborneSharedKills, ABossSizedEnemyHitToDeathIsAKill) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{0x10024050, 1800, 0xC000, false, true}}),
              Changes{});
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{0x10024050, 900, 0xC000, false, false}}),
              Changes{})
        << "hurt";
    EXPECT_EQ(kills.Observe(1, std::vector<SeenCharacter>{{0x10024050, 0, 0xC000, false, false}}),
              (Changes{{0x10024050, true}}));
}

// Rig smoke 2026-10-12 (run 153353): the host killed 0x1001400b; the first guest's copy in the
// same world died of that hit 2 ms after the host's kill frame came, and the guest posted it as
// its own kill too. A death whose last hit was another player's is that player's kill.
TEST(BloodborneSharedKills, ADeathByAnotherPlayersHitIsTheirKill) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    const std::vector<SeenCharacter> alive{{0x1001400b, 278, 0xB000}, {0x10014009, 364, 0x9000}};
    EXPECT_EQ(kills.Observe(1, alive), Changes{});
    const std::vector<SeenCharacter> dead{{0x1001400b, 0, 0xB000, false, false, true},
                                          {0x10014009, 0, 0x9000, false, false, false}};
    EXPECT_EQ(kills.Observe(1, dead), (Changes{{0x10014009, true}})) << "only its own kill";

    // Who killed it, from the last hit on it (hit hooks in): this player's, another's, or none
    // (the world: the host's kill, a guest's copy follows it).
    EXPECT_TRUE(KilledByAnother(true, false, false));
    EXPECT_FALSE(KilledByAnother(true, true, false));
    EXPECT_FALSE(KilledByAnother(true, std::nullopt, true)) << "the world's kill: the host's";
    EXPECT_TRUE(KilledByAnother(true, std::nullopt, false));
    EXPECT_FALSE(KilledByAnother(false, false, false)) << "no hit hooks: as before";

    // Kaniel's play test 2026-10-19: Lisara's last hit killed an enemy on the Ward, which
    // Kaniel's game runs. shadNet takes new kills there only from Kaniel's game, which left it
    // to Lisara's: the kill never reached the log, and krolebord, arriving, had it alive and got
    // no drop. The game running the map's enemies posts every death on them.
    EXPECT_FALSE(KilledByAnother(true, false, true, true)) << "the owner's copy: its kill";
    EXPECT_FALSE(KilledByAnother(true, std::nullopt, false, true)) << "a guest that owns";
    EXPECT_TRUE(KilledByAnother(true, false, true, false)) << "a follower: the hitter's";
}

// Kaniel 2026-10-11: the host and Ada went to the Dream and back while Brom stayed in
// Central Yharnam, and half of it was alive again: the host's trip took its 82 kills back. While
// another member stays on the map, a visit's kills stand (held), hidden for this member too
// when it comes back; they go back once nobody of the party is on that map.
TEST(BloodborneSharedKills, KillsStandWhileSomeoneStaysOnTheMap) {
    constexpr u32 Yharnam = 0x18010000;
    constexpr u32 Dream = 0x15000000;
    const auto visit = [](u32 map, u64 n) { return (u64{map} << 32) | n; };
    using Changes = std::vector<std::pair<s32, bool>>;
    const std::vector<SeenCharacter> alive{{0x10014085, 300}, {0x10014088, 250}};
    const std::vector<SeenCharacter> one_dead{{0x10014085, 0}, {0x10014088, 250}};
    const std::vector<u32> Brom_here{Yharnam};
    VisitKills kills;
    kills.Observe(visit(Yharnam, 1), alive, Brom_here);
    EXPECT_EQ(kills.Observe(visit(Yharnam, 1), one_dead, Brom_here), (Changes{{0x10014085, true}}));
    EXPECT_EQ(kills.Observe(visit(Dream, 2), {}, Brom_here), Changes{})
        << "Brom stayed: the kill stands";
    EXPECT_EQ(kills.Held(Yharnam), (std::vector<u32>{0x10014085}));
    EXPECT_EQ(kills.Observe(visit(Yharnam, 3), alive, Brom_here), Changes{})
        << "back: the respawned copy is hidden, nothing posted";
    EXPECT_EQ(kills.Held(Yharnam), (std::vector<u32>{0x10014085}));
    EXPECT_EQ(kills.Observe(visit(Yharnam, 3), alive, {}), Changes{})
        << "Brom left, but this member is there";
    EXPECT_EQ(kills.Observe(visit(Dream, 4), {}, {}), (Changes{{0x10014085, false}}))
        << "nobody left on the map: the enemies respawn for everyone";
    EXPECT_TRUE(kills.Held(Yharnam).empty());
}

// Kaniel 2026-10-15 (Lisara's log, friend38): both had walked from Central Yharnam into the
// Cathedral Ward and killed two enemies there; Kaniel went to the Dream and back while Lisara stood
// in the Ward, and the enemies were alive again. A walk keeps the visit, so the kills were filed
// under the map the visit began on (Central Yharnam, where nobody was) and taken back. A kill is
// of the map it died on: it stands while someone is on that map.
TEST(BloodborneSharedKills, AKillIsOfTheMapItDiedOn) {
    constexpr u32 Yharnam = 0x18010000;
    constexpr u32 Ward = 0x18000000;
    constexpr u32 Dream = 0x15000000;
    const auto visit = [](u32 map, u64 n) { return (u64{map} << 32) | n; };
    using Changes = std::vector<std::pair<s32, bool>>;
    const std::vector<SeenCharacter> alive{{0x10014068, 210}, {0x1001005f, 300}};
    const std::vector<SeenCharacter> both_dead{{0x10014068, 0}, {0x1001005f, 0}};
    const std::vector<u32> lisara_in_ward{Ward};
    VisitKills kills;
    const u64 walk = visit(Yharnam, 1);
    kills.Observe(walk, alive, {}, Yharnam);
    EXPECT_EQ(kills.Observe(walk, std::vector<SeenCharacter>{{0x10014068, 0}, {0x1001005f, 300}},
                            {}, Yharnam),
              (Changes{{0x10014068, true}}))
        << "in Central Yharnam";
    EXPECT_EQ(kills.Observe(walk, both_dead, lisara_in_ward, Ward), (Changes{{0x1001005f, true}}))
        << "walked into the Ward: the same visit";
    EXPECT_EQ(kills.Standing(), (WorldLogKills{{Yharnam, {0x10014068}}, {Ward, {0x1001005f}}}));
    // Kaniel 2026-10-17: Central Yharnam's enemies are loaded in the Ward too, so Lisara there
    // holds both kills (the Central one came back for the host alone).
    EXPECT_EQ(kills.Observe(visit(Dream, 2), {}, lisara_in_ward, Dream), Changes{})
        << "to the Dream: Lisara holds both";
    EXPECT_EQ(kills.Held(Ward), (std::vector<u32>{0x1001005f}));
    EXPECT_EQ(kills.Held(Yharnam), (std::vector<u32>{0x10014068}));
    EXPECT_EQ(kills.Observe(visit(Ward, 3), alive, lisara_in_ward, Ward), Changes{})
        << "back at the Ward's lamp: the respawned copies are hidden";
    EXPECT_EQ(kills.Observe(visit(Ward, 3), alive, {}, Ward), Changes{})
        << "Lisara left, but this member is there";
    EXPECT_EQ(kills.Observe(visit(Ward, 3), alive, {}, Yharnam), Changes{})
        << "walked into Central Yharnam: the Ward is loaded with it";
    EXPECT_EQ(kills.Observe(visit(Dream, 4), {}, {}, Dream),
              (Changes{{0x1001005f, false}, {0x10014068, false}}))
        << "nobody in either: both reset";
}

// Kaniel 2026-10-11: an enemy another member killed vanished in front of the player (hidden with
// the event disable byte) instead of dying. One near this player dies by the game's own kill.
TEST(BloodborneSharedKills, AnotherMembersKillNearbyDiesInsteadOfVanishing) {
    const LampPosition me{0.0f, 0.0f, 0.0f};
    EXPECT_TRUE(DiesInsteadOfHiding(me, LampPosition{20.0f, 0.0f, 20.0f}));
    EXPECT_FALSE(DiesInsteadOfHiding(me, LampPosition{40.0f, 0.0f, 40.0f})) << "far: hidden";
    EXPECT_FALSE(DiesInsteadOfHiding(std::nullopt, LampPosition{1.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(DiesInsteadOfHiding(me, std::nullopt));
}

// What ends a visit is what respawns the enemies for that member: a trip (lamp travel, the
// Dream, a death sent home) or a reload outside a session. A guest pulled into the host's world
// when the host arrives reloads the same map in a session: its kills there stand (friend run
// 2026-10-08: the guest killed the dog, the host came back, the guest's summon withdrew the kill
// and the host's dog stood alive).
TEST(BloodborneSharedKills, ASummonIntoTheHostsWorldKeepsTheGuestsVisit) {
    VisitTracker visits;
    const u64 first = visits.Update(0x18010000, 0x1000, false, 0);
    EXPECT_EQ(visits.Update(0x18010000, 0x1000, false, 0), first);
    EXPECT_EQ(visits.Update(0x18010000, 0x2000, true, 0), first) << "summoned on the same map";
    const u64 tripped = visits.Update(0x18010000, 0x3000, true, 1);
    EXPECT_NE(tripped, first) << "a trip to a lamp on the same map";
    const u64 moved = visits.Update(0x15000000, 0x3000, true, 2);
    EXPECT_NE(moved, tripped) << "a trip to another map";
    const u64 back = visits.Update(0x18010000, 0x4000, true, 3);
    EXPECT_NE(back, first);
    EXPECT_NE(back, moved);
    EXPECT_NE(visits.Update(0x18010000, 0x5000, false, 3), back) << "a reload out of a session";

    // The killer hides its own kills too while it stands in another world's copy of the map.
    VisitKills kills;
    kills.Observe(back, std::vector<SeenCharacter>{{0x10014085, 300}});
    kills.Observe(back, std::vector<SeenCharacter>{{0x10014085, 0}});
    EXPECT_EQ(kills.Killed(), (std::vector<u32>{0x10014085}));
    kills.Observe(back + 1, std::vector<SeenCharacter>{{0x10014085, 300}});
    EXPECT_TRUE(kills.Killed().empty());
}

// Kaniel's friend run 2026-10-11: the host walked from Central Yharnam (0x18010000) into the
// Cathedral Ward (0x18000000), its visit ended with the map, and it withdrew all 72 kills of that
// visit from the kill board while they lay dead in its world; the guest's copies came back to
// life (duplicate enemies, and no echoes for the host when the guest killed them again). Walking
// across a border loads nothing and respawns nothing: the visit goes on, under the map it began on.
// Kaniel 2026-10-11: after the whole party died in the Cathedral Ward the map did not reset. The
// wipe sent everyone to the lamp one after another, so each member's kills were held for the
// others still on the map, and the host hid 16 enemies on the reloaded map. A death that resets
// the map takes back every kill of this member, the held ones too.
TEST(BloodborneSharedKills, AMapResetTakesBackEveryKill) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    using Seen = std::vector<SeenCharacter>;
    const u64 ward = (u64{0x18000000} << 32) | 7;
    const std::vector<u32> others_there{0x18000000};
    kills.Observe(ward, Seen{{0x10010060, 300}, {0x10010020, 200}}, others_there);
    kills.Observe(ward, Seen{{0x10010060, 0}, {0x10010020, 200}}, others_there);
    const u64 after_lamp = ward + 2;
    kills.Observe(after_lamp, Seen{{0x10010020, 200}}, others_there);
    kills.Observe(after_lamp, Seen{{0x10010020, 0}}, others_there);
    ASSERT_EQ(kills.Held(0x18000000), (std::vector<u32>{0x10010060}));
    ASSERT_EQ(kills.Killed(), (std::vector<u32>{0x10010020}));

    auto back = kills.Forget();
    std::ranges::sort(back);
    EXPECT_EQ(back, (Changes{{0x10010020, false}, {0x10010060, false}}));
    EXPECT_TRUE(kills.Killed().empty());
    EXPECT_TRUE(kills.Held(0x18000000).empty());

    const u64 reloaded = after_lamp + 1;
    EXPECT_EQ(kills.Observe(reloaded, Seen{{0x10010060, 300}, {0x10010020, 200}}, others_there),
              Changes{})
        << "nothing left to hold or take back";
    EXPECT_TRUE(kills.Held(0x18000000).empty());
}

TEST(BloodborneSharedKills, WalkingAcrossABorderKeepsTheVisit) {
    VisitTracker visits;
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    const u64 first = visits.Update(0x18010000, 0x1000, true, 1);
    kills.Observe(first, std::vector<SeenCharacter>{{0x10014068, 210}});
    EXPECT_EQ(kills.Observe(first, std::vector<SeenCharacter>{{0x10014068, 0}}),
              (Changes{{0x10014068, true}}));

    const u64 ward = visits.Update(0x18000000, 0x1000, true, 1);
    EXPECT_EQ(ward, first) << "into the Cathedral Ward";
    EXPECT_EQ(kills.Observe(ward, std::vector<SeenCharacter>{{0x10014068, 0}}), Changes{})
        << "the kill stands across the border";
    EXPECT_EQ(visits.Update(0x16000000, 0x1000, true, 1), first) << "on into Hemwick";
    EXPECT_EQ(visits.Update(0x18010000, 0x1000, true, 1), first) << "and back";
    EXPECT_EQ(visits.Update(0x18000000, 0x1000, false, 1), first) << "out of a session too";

    const u64 tripped = visits.Update(0x18000000, 0x2000, true, 2);
    EXPECT_NE(tripped, first) << "a lamp in the Ward";
    EXPECT_EQ(tripped >> 32, 0x18000000u) << "the map the visit began on";
    EXPECT_NE(visits.Update(0x15000000, 0x3000, true, 2), tripped)
        << "a summon into the host's world on another map is a load";
}

// Rig test "dog" 2026-10-09: the host left, the guest's own world loaded on the same map (the
// visit goes on: a reload in a session keeps it), and two enemies dead in the guest's save but
// alive in the host's world a moment before were posted as kills; the host hid 2 living
// enemies. A kill is one character object going from alive to dead: a reload makes new ones.
// The rig's dog test, with the kills named: when the host left for the Dream its guest took over
// the map's enemies, and its own world (where it had killed Father Gascoigne, 0x1001409c and his
// beast form 0x1001409e, dead flags 0x26) put him down on the spot; posted as kills, they hid the
// host's living Gascoigne when the host came back. A death while a character settles after its
// takeover or hand-back is the member's world state, not a kill: seen, not posted. Only that
// character: a guest alone keeps taking over enemies as they load, and fights the others.
TEST(BloodborneSharedKills, ATakeoverIsNoKill) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    using Seen = std::vector<SeenCharacter>;
    EXPECT_EQ(kills.Observe(1, Seen{{0x1001409c, 2031, 0xA000}, {0x1001400b, 484, 0xB000}}),
              Changes{});
    EXPECT_EQ(kills.Observe(1, Seen{{0x1001409c, 0, 0xA000, true}, {0x1001400b, 0, 0xB000}}),
              (Changes{{0x1001400b, true}}))
        << "Gascoigne settling after his takeover; the other one killed meanwhile";
    EXPECT_EQ(kills.Observe(1, Seen{{0x1001409c, 0, 0xA000}, {0x1001400b, 0, 0xB000}}), Changes{})
        << "still dead once settled: not a kill either";
    EXPECT_GT(EnemyOwnerChangeSettles, std::chrono::seconds{0});
}

// With the kill board naming them: the two false kills came as the guest was summoned into the
// host's world (226 flags changed at once, a load that keeps the map's objects), where Father
// Gascoigne was dead and the guest's copy alive a moment before. A character settles for
// EnemyOwnerChangeSettles after such a load, and after its takeover or hand-back.
// Kaniel's test 2026-10-23: the hag a lone guest killed 9 s after its trip, its alone state just
// changed (settling), was never a kill.
TEST(BloodborneSharedKills, OwnKillCountsWhileSettling) {
    EXPECT_FALSE(SettlingHidesDeath(true, true)) << "this player's own hit killed it";
    EXPECT_TRUE(SettlingHidesDeath(true, false)) << "another's";
    EXPECT_TRUE(SettlingHidesDeath(true, std::nullopt)) << "the world put it down";
    EXPECT_FALSE(SettlingHidesDeath(false, std::nullopt));
}

TEST(BloodborneSharedKills, ACharacterSettlesAfterALoadOrATakeover) {
    constexpr s64 Settle =
        std::chrono::duration_cast<std::chrono::milliseconds>(EnemyOwnerChangeSettles).count();
    EXPECT_TRUE(CharacterSettles(100'000, 100'000 - Settle + 1, std::nullopt)) << "after a load";
    EXPECT_FALSE(CharacterSettles(100'000, 100'000 - Settle, std::nullopt)) << "settled";
    EXPECT_TRUE(CharacterSettles(100'000, 0, 99'000)) << "just taken over";
    EXPECT_FALSE(CharacterSettles(100'000, 0, 100'000 - Settle)) << "taken over long ago";
    EXPECT_FALSE(CharacterSettles(100'000, 0, std::nullopt)) << "nothing happened";
}

TEST(BloodborneSharedKills, AWorldThatLoadsIsNoKill) {
    VisitKills kills;
    using Changes = std::vector<std::pair<s32, bool>>;
    using Seen = std::vector<SeenCharacter>;
    EXPECT_EQ(kills.Observe(1, Seen{{0x1001409c, 2031, 0xA000}}), Changes{});
    EXPECT_EQ(kills.Observe(1, Seen{{0x1001409c, 0, 0xB000}}), Changes{})
        << "dead already in the world that loaded";
    EXPECT_EQ(kills.Observe(1, Seen{{0x1001409c, 2031, 0xC000}}), Changes{});
    EXPECT_EQ(kills.Observe(1, Seen{{0x1001409c, 0, 0xC000}}), (Changes{{0x1001409c, true}}))
        << "killed in this world";
    EXPECT_EQ(kills.Killed(), (std::vector<u32>{0x1001409c}));
}

// Friend run 2026-10-09, Gascoigne: the host's copy went 2031 -> 131 -> 0, the guest's only
// moved with the guest's own hits (2031 -> 1753). Each member posts the enemies it hurt this
// visit and takes the lowest share of HP left.
TEST(BloodborneSharedEnemyHp, EveryMemberSeesTheLowestHpLeft) {
    const std::vector<EnemyHp> hurt{{0x1001409c, 1595, 2031}, {0x10014085, 120, 300}};
    const auto bytes = EncodeEnemyHp(0x1801000000000003, hurt);
    EXPECT_EQ(bytes.size(), 8u + 2 * 12u);
    EXPECT_EQ(DecodeEnemyHp(bytes), (EnemyHpReport{0x1801000000000003, hurt}));
    EXPECT_EQ(DecodeEnemyHp(EncodeEnemyHp(7, {})), (EnemyHpReport{7, {}})) << "nothing hurt";
    EXPECT_EQ(DecodeEnemyHp(std::vector<u8>{1, 2, 3}), std::nullopt);
    EXPECT_EQ(DecodeEnemyHp(std::vector<u8>(8 + 5)), std::nullopt) << "a cut entry";

    EXPECT_EQ(SharedEnemyHp(1753, 2031, 1595, 2031), 1595) << "the host hit harder";
    EXPECT_EQ(SharedEnemyHp(1595, 2031, 1753, 2031), std::nullopt) << "never back up";
    EXPECT_EQ(SharedEnemyHp(3047, 3047, 1016, 2031), 1525) << "a third left, co-op bonus here";
    EXPECT_EQ(SharedEnemyHp(500, 2031, 0, 2031), 1) << "the game kills it, not the board";
    EXPECT_EQ(SharedEnemyHp(0, 2031, 100, 2031), std::nullopt) << "already dead";
    EXPECT_EQ(SharedEnemyHp(500, 2031, 100, 0), std::nullopt);
}

// Kaniel 2026-10-10, Iosefka's Clinic: the guest saw Iosefka standing at her door while the
// host's story hid her (event disable byte +0x28). After its join the guest's world is rebuilt as
// the host's client and does not run the events that hide her, so every character the host's
// world hides or shows is mirrored to the guests on its map.
TEST(BloodborneHiddenCharacters, TheHostsHiddenCharactersTravelAsHandlesAndAByte) {
    const std::vector<CharacterShown> host{{0x10014005, true}, {0x1001400b, false}};
    const auto bytes = EncodeShownCharacters(host);
    EXPECT_EQ(bytes.size(), 2 * 5u);
    EXPECT_EQ(DecodeShownCharacters(bytes), host);
    EXPECT_EQ(DecodeShownCharacters(EncodeShownCharacters({})), std::vector<CharacterShown>{});
    EXPECT_EQ(DecodeShownCharacters(std::vector<u8>{1, 2, 3}), std::nullopt) << "a cut entry";
    // The enable switch rides in the same byte (bit 1): the host's story switched off the copy
    // of Iosefka by her door (0x10014000) on Kaniel's new characters.
    const std::vector<CharacterShown> switched{{0x10014000, false, true}, {0x10014005, true, true}};
    EXPECT_EQ(EncodeShownCharacters(switched)[4], 2);
    EXPECT_EQ(EncodeShownCharacters(switched)[9], 3);
    EXPECT_EQ(DecodeShownCharacters(EncodeShownCharacters(switched)), switched);
}

TEST(BloodborneHiddenCharacters, AGuestHidesAndShowsWhatTheHostDoes) {
    const std::vector<CharacterShown> host{
        {0x10014005, true},  // Iosefka, hidden by the host's story
        {0x100140ae, true},  // hidden on both already
        {0x1001400b, false}, // an enemy both show
        {0x10014053, false}, // shown by the host, hidden on the guest
    };
    const std::vector<CharacterShown> guest{
        {0x10014005, false}, {0x100140ae, true}, {0x1001400b, false},
        {0x10014053, true},  {0x10014099, true}, // the host has no such character: left alone
    };
    EXPECT_EQ(ShownLikeTheHost(host, guest, {}),
              (std::vector<CharacterShown>{{0x10014005, true}, {0x10014053, false}}));
    EXPECT_EQ(ShownLikeTheHost(host, guest, {0x10014053}),
              (std::vector<CharacterShown>{{0x10014005, true}}))
        << "one the kill board hid here (another member killed it) stays hidden";
    EXPECT_EQ(ShownLikeTheHost({}, guest, {}), std::vector<CharacterShown>{})
        << "nothing from the host: nothing changes";
}

// Kaniel 2026-10-10: on new characters the guest still saw Iosefka upstairs in the clinic. The
// host's story removes her (2004:29, the +0x28 byte) before her model loads; the guest's copy,
// built at its join, stays loaded and drawn with the byte set. The guest turns such characters
// off with the game's enable switch, and back on only the ones it turned off.
TEST(BloodborneHiddenCharacters, AGuestSwitchesOffWhatTheHostRemoved) {
    const std::vector<CharacterShown> host{
        {0x10014005, true},        // Iosefka, removed by the host's story
        {0x100140ae, true},        // removed, and already off here
        {0x1001400b, false},       // an enemy both show
        {0x10014053, false},       // shown by the host again
        {0x10014000, false, true}, // switched off by the host's story (by Iosefka's door)
    };
    const std::vector<CharacterSwitch> guest{
        {0x10014005, false}, {0x100140ae, true},  {0x1001400b, false},
        {0x10014053, true},  {0x10014099, false}, // not in the host's world
        {0x10014000, false},
    };
    const std::unordered_set<u32> switched_off{0x10014053};
    EXPECT_EQ(
        SwitchedLikeTheHost(host, guest, switched_off, {}),
        (std::vector<CharacterSwitch>{{0x10014005, true}, {0x10014053, false}, {0x10014000, true}}))
        << "what this guest turned off comes back on";
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {}, {0x10014005, 0x10014000}),
              (std::vector<CharacterSwitch>{{0x10014053, false}}))
        << "ones the kill board hid are left alone";
    const std::vector<CharacterSwitch> back_on{{0x10014005, false}};
    EXPECT_EQ(SwitchedLikeTheHost(host, back_on, {0x10014005}, {}),
              (std::vector<CharacterSwitch>{{0x10014005, true}}))
        << "turned on again by the guest's world: off again";
    EXPECT_EQ(SwitchedLikeTheHost({}, guest, switched_off, {}), std::vector<CharacterSwitch>{})
        << "nothing from the host: nothing changes";
}

// Rig 2026-10-21 (mist-ambush-guest): with a host under 15 Insight, Hemwick's mist ambush
// (m22_00 event 12204000) switches its nine enemies off for good and ends, its slot's done flag
// on; the guest's copies stayed on, since the host's switch is not followed far from the host
// (its events park characters there). An ambush enemy the host has off with its slot done is off
// for good at any distance; one still waiting (slot not done) is left to the guest's own event.
TEST(BloodborneHiddenCharacters, AGuestSwitchesOffAnAmbushTheHostEndedAtAnyDistance) {
    EXPECT_EQ(AmbushSlotFlag(2200500), 12204000);
    EXPECT_EQ(AmbushSlotFlag(2200508), 12204008);
    EXPECT_EQ(AmbushSlotFlag(2200509), std::nullopt);
    EXPECT_EQ(AmbushSlotFlag(2200810), std::nullopt) << "the Witch's Mad One";
    const std::vector<CharacterShown> host{
        {0x10008022, false, true, true}, // off, far from the host, its slot done
        {0x10008023, false, true, true}, // off, far, still waiting
    };
    const std::vector<CharacterSwitch> guest{{0x10008022, false}, {0x10008023, false}};
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {}, {}, {0x10008022}),
              (std::vector<CharacterSwitch>{{0x10008022, true}}));
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {}, {}), std::vector<CharacterSwitch>{})
        << "far from the host otherwise: not followed";
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {}, {0x10008022}, {0x10008022}),
              std::vector<CharacterSwitch>{})
        << "one the kill board hid is left alone";
}

// Kaniel's friend Lisara 2026-10-15 (v0.3.1-local): in the Forbidden Woods she never saw the enemy
// that comes out of the red mist (0x1000801e), though Kaniel fought it beside her. It starts
// switched off in both worlds; Kaniel's event switched it on, hers never did, and a guest only
// switched back on what it had switched off itself. A copy near the host is now on wherever the
// host's is, unless it is dead in either world.
TEST(BloodborneHiddenCharacters, AGuestSwitchesOnWhatTheHostsStorySwitchedOn) {
    const std::vector<CharacterShown> host{
        {0x1000801e, false},                     // the red mist's enemy, on in the host's world
        {0x1000801f, false, false, true},        // on, but far from the host
        {0x10008040, false, false, false, true}, // on and dead in the host's world
        {0x10008041, false},                     // on, and dead in this guest's world
    };
    const std::vector<CharacterSwitch> guest{
        {0x1000801e, true}, {0x1000801f, true}, {0x10008040, true}, {0x10008041, true, true}};
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {}, {}),
              (std::vector<CharacterSwitch>{{0x1000801e, false}}));
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {}, {0x1000801e}), std::vector<CharacterSwitch>{})
        << "one the kill board hid stays off";
}

// Kaniel's playtest 2026-10-20: the Witch of Hemwick flickered for krolebord. Her vanish (m22_00
// event 12204812, waiting on her own AI) switches her off and back on in every game; the guest
// switched its copy along with the host's board as well, about 25 times out of step with its own
// fight. A boss's switch is its fight's: the guest leaves it to its own game, but still undoes a
// switch-off of its own.
TEST(BloodborneHiddenCharacters, AGuestLeavesABossSwitchToItsOwnFight) {
    constexpr bool Boss = true;
    const std::vector<CharacterShown> host{
        {0x10008024, false, true}, // the Witch, vanished in the host's world
        {0x10008025, false},       // her clone, on in the host's world
        {0x1000801c, false, true}, // a Mad One, off in the host's world
    };
    const std::vector<CharacterSwitch> guest{
        {0x10008024, false, false, Boss}, {0x10008025, true, false, Boss}, {0x1000801c, false}};
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {}, {}),
              (std::vector<CharacterSwitch>{{0x1000801c, true}}));
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {0x10008025}, {}),
              (std::vector<CharacterSwitch>{{0x10008025, false}, {0x1000801c, true}}))
        << "a boss this guest switched off itself is switched back on";
}

// Kaniel's friend 2026-10-11: no enemies in the Forbidden Woods. While the guest was in the
// Cathedral Ward, the host's world had the woods' enemies by the border switched off (its events
// park characters far from the player), and the guest switched its copies off too; they stayed off
// when it walked in. The host marks characters far from it, and a guest follows only the near
// ones, turning back on any far one it turned off.
TEST(BloodborneHiddenCharacters, FarFromTheHostRidesInBitTwo) {
    const std::vector<CharacterShown> host{{0x1000801b, false, true, true}, {0x10014005, true}};
    EXPECT_EQ(EncodeShownCharacters(host)[4], 6);
    EXPECT_EQ(DecodeShownCharacters(EncodeShownCharacters(host)), host);
}

// Kaniel's playtest 2026-10-20, the Witch of Hemwick: only the host saw the Mad Ones she summons.
// They wait switched off with their hits off (event command 2004:39); her summon (m22_00 event
// 12204839) switches one on and turns its hits on, in the host's game only, since it waits on
// her AI. The guests switched their copies on like the host's (SwitchedLikeTheHost) but their
// hits stayed off. A guest now turns a character's hits on or off as the host has them, for one
// that is on near the host.
TEST(BloodborneHiddenCharacters, AGuestTurnsHitsOnAsTheHostHasThem) {
    const std::vector<CharacterShown> host{
        {0x1000801c, false, false, false, false, false}, // summoned: on, hits on
        {0x1000801d, false, true, false, false, true},   // waiting: off, hits off
        {0x1000801e, false, false, true, false, false},  // far from the host
        {0x1000801f, false, false, false, true, false},  // dead in the host's world
        {0x10008020, false, false, false, false, true},  // on with its hits off in the host's
    };
    const std::vector<CharacterHits> guest{{0x1000801c, true},  {0x1000801d, true},
                                           {0x1000801e, true},  {0x1000801f, true},
                                           {0x10008020, false}, {0x10008021, true}};
    EXPECT_EQ(HitsLikeTheHost(host, guest, {}),
              (std::vector<CharacterHits>{{0x1000801c, false}, {0x10008020, true}}));
    EXPECT_EQ(HitsLikeTheHost(host, guest, {0x1000801c}),
              (std::vector<CharacterHits>{{0x10008020, true}}))
        << "one the kill board hid is left alone";
    EXPECT_EQ(HitsLikeTheHost({}, guest, {}), std::vector<CharacterHits>{});
}

TEST(BloodborneHiddenCharacters, AGuestLeavesCharactersFarFromTheHostAsTheyAre) {
    const std::vector<CharacterShown> host{
        {0x1000801b, false, true, true}, // switched off far from the host: the woods' border
        {0x1000801c, false, true, true}, // the same, and this guest switched it off before
        {0x10014000, false, true},       // switched off near the host: Iosefka's door
    };
    const std::vector<CharacterSwitch> guest{
        {0x1000801b, false}, {0x1000801c, true}, {0x10014000, false}};
    EXPECT_EQ(SwitchedLikeTheHost(host, guest, {0x1000801c}, {}),
              (std::vector<CharacterSwitch>{{0x1000801c, false}, {0x10014000, true}}));

    const std::vector<CharacterShown> host_hidden{
        {0x10010000, true, false, true}, // hidden far from the host (Cathedral Ward on arrival)
        {0x10010001, true, false, true}, // the same, hidden here by this guest before
        {0x10014005, true},              // hidden near the host: Iosefka
    };
    const std::vector<CharacterShown> mine{
        {0x10010000, false}, {0x10010001, true}, {0x10014005, false}};
    EXPECT_EQ(ShownLikeTheHost(host_hidden, mine, {}, {0x10010001}),
              (std::vector<CharacterShown>{{0x10010001, false}, {0x10014005, true}}));
    EXPECT_EQ(ShownLikeTheHost(host_hidden, mine, {}, {}),
              (std::vector<CharacterShown>{{0x10014005, true}}))
        << "a far one this guest's own world hid stays hidden";
}

// The same run: the woods' enemies were switched off while the guest's map was the Cathedral
// Ward's, and they stay loaded across the border. On the guest's map change the list of what it
// switched off was dropped, so nothing ever turned them back on, even with both players in the
// woods hitting them. A map change now turns back on what this guest changed that is still here.
TEST(BloodborneHiddenCharacters, AMapChangeUndoesWhatTheGuestSwitchedOff) {
    const std::vector<CharacterSwitch> mine{
        {0x1000801b, true},  // switched off by this guest, still loaded across the border
        {0x1000801c, false}, // switched off by this guest, on again since
        {0x10008030, true},  // off by the guest's own world
    };
    EXPECT_EQ(UndoneOnMapChange({0x1000801b, 0x1000801c, 0x10010000}, mine),
              (std::vector<CharacterSwitch>{{0x1000801b, false}}));
    EXPECT_EQ(UndoneOnMapChange({}, mine), std::vector<CharacterSwitch>{});
}

TEST(BloodborneHiddenCharacters, FarMeansBeyondFiftyMetresOfTheHost) {
    const LampPosition host{0.0f, 0.0f, 0.0f};
    EXPECT_FALSE(FarFromHost(host, LampPosition{30.0f, 0.0f, 40.0f}));
    EXPECT_TRUE(FarFromHost(host, LampPosition{30.0f, 1.0f, 40.0f}));
    EXPECT_FALSE(FarFromHost(std::nullopt, LampPosition{500.0f, 0.0f, 0.0f}))
        << "no host position: nothing is far";
    EXPECT_FALSE(FarFromHost(host, std::nullopt));
}

// Kaniel 2026-10-11, a party of three in Central Yharnam: nobody saw a character by a window
// (0x1001400c) while together; one guest saw it once the host was by the bridge, and it went away
// again as the host came back. The host's world has it removed (+0x28, switched off) even with the
// host beside it: its story, not parking. Far from the host, a guest went by its own world. What
// the host's world had for a character while near the host now stands once the host moves away.
TEST(BloodborneHiddenCharacters, WhatTheHostHadNearItStandsWhenItMovesAway) {
    constexpr u32 Map = 0x18010000;
    HostNearStates states;
    const std::vector<CharacterShown> near{{0x1001400c, true, true, false}};
    EXPECT_EQ(states.Fill(Map, near), near);
    const std::vector<CharacterShown> far{{0x1001400c, true, true, true}};
    EXPECT_EQ(states.Fill(Map, far), near) << "removed near the host: still removed";

    // Hidden and switched off by this guest while the host was near: nothing to undo.
    const std::vector<CharacterShown> mine{{0x1001400c, true, true}};
    EXPECT_EQ(ShownLikeTheHost(states.Fill(Map, far), mine, {}, {0x1001400c}),
              std::vector<CharacterShown>{});
    EXPECT_EQ(SwitchedLikeTheHost(states.Fill(Map, far), {{{0x1001400c, true}}}, {0x1001400c}, {}),
              std::vector<CharacterSwitch>{});
}

TEST(BloodborneHiddenCharacters, ParkedFarFromTheHostStaysAsTheHostLastSawIt) {
    constexpr u32 Map = 0x15010000;
    HostNearStates states;
    // Never near the host: as posted (the guest goes by its own world).
    const std::vector<CharacterShown> parked{{0x1000801b, false, true, true}};
    EXPECT_EQ(states.Fill(Map, parked), parked);
    // The host walked by it (shown), then left and its events parked it again.
    EXPECT_EQ(states.Fill(Map, {{{0x1000801b, false, false, false}}}),
              (std::vector<CharacterShown>{{0x1000801b, false, false, false}}));
    EXPECT_EQ(states.Fill(Map, parked),
              (std::vector<CharacterShown>{{0x1000801b, false, false, false}}))
        << "shown near the host: shown";
    // Another map: nothing carries over.
    EXPECT_EQ(states.Fill(0x18010000, {{{0x1000801b, false, true, true}}}),
              (std::vector<CharacterShown>{{0x1000801b, false, true, true}}));
    EXPECT_EQ(states.Fill(Map, parked), parked);
}

// Kaniel 2026-10-11: after the guests' trip to the Dream and back (their own worlds now), they
// saw enemies that were dead in the host's world. Those died before this session (dead in the
// host's save), so nobody's kill board had them. The host's shown board says which characters
// are dead in its world (bit 3), and a guest takes them as killed.
TEST(BloodborneHiddenCharacters, DeadInTheHostsWorldRidesInBitThree) {
    const std::vector<CharacterShown> host{{0x1001408f, false, false, false, true},
                                           {0x10014090, true, false, false, false}};
    EXPECT_EQ(EncodeShownCharacters(host)[4], 8);
    EXPECT_EQ(DecodeShownCharacters(EncodeShownCharacters(host)), host);
    EXPECT_EQ(DeadInTheHostsWorld(host), (std::unordered_set<u32>{0x1001408f}));
    EXPECT_TRUE(DeadInTheHostsWorld({}).empty());
}

TEST(BloodborneHiddenCharacters, DeadMeansKilledOrPutDownInTheSave) {
    EXPECT_TRUE(IsDeadInItsWorld(0x01)) << "killed by an event";
    EXPECT_TRUE(IsDeadInItsWorld(0x06)) << "killed by damage";
    EXPECT_TRUE(IsDeadInItsWorld(0x26)) << "dead in the save, put down at load";
    EXPECT_FALSE(IsDeadInItsWorld(0x00));
    EXPECT_FALSE(IsDeadInItsWorld(0x20));
}

// Kaniel's play test 2026-10-18: after his game crashed and loaded again, two Ward enemies dead in
// his save were not on his screen, yet his game told the guests they stood (krolebord killed them
// again, and Kaniel's game paid no echoes). Rig 2026-10-18: an enemy killed before the game starts
// again loads with HP 0/484 and death flags 0x0, neither hidden nor switched off.
TEST(BloodborneHiddenCharacters, AnEnemyLoadedAtNoHpIsDeadInItsWorld) {
    EXPECT_TRUE(IsDeadInItsWorld(0x00, 0, 484)) << "dead in the save, loaded at 0 HP";
    EXPECT_TRUE(IsDeadInItsWorld(0x06, 0, 484));
    EXPECT_TRUE(IsDeadInItsWorld(0x01, 300, 484)) << "the death flags still count";
    EXPECT_FALSE(IsDeadInItsWorld(0x00, 300, 484));
    EXPECT_FALSE(IsDeadInItsWorld(0x00, 0, 0)) << "no HP read yet (a character still loading)";
}

// Kaniel 2026-10-11: Ada joined a session Brom was already in and did not see him; after
// both went to the Dream and back (where the other players are re-created) they saw each other.
// A guest that joins where another guest already is re-creates the others once, as after a trip.
TEST(BloodbornePartyController, AGuestJoiningAnotherGuestRecreatesThemOnce) {
    RecreateAfterJoin recreate;
    EXPECT_FALSE(recreate.Tick(true, false, 0, true, 0)) << "not in a session yet";
    EXPECT_FALSE(recreate.Tick(true, true, 1, true, 1000)) << "only the host: nothing to fix";
    EXPECT_FALSE(recreate.Tick(true, true, 2, true, 1500)) << "settling first";
    EXPECT_TRUE(recreate.Tick(true, true, 2, true, 1000 + RecreateAfterJoin::SettleMs + 1));
    EXPECT_FALSE(recreate.Tick(true, true, 2, true, 20000)) << "once";
    // Out of the session and in again: a new join.
    EXPECT_FALSE(recreate.Tick(true, false, 0, true, 30000));
    EXPECT_FALSE(recreate.Tick(true, true, 2, true, 31000));
    EXPECT_TRUE(recreate.Tick(true, true, 2, true, 31000 + RecreateAfterJoin::SettleMs + 1));
}

TEST(BloodbornePartyController, OnlyAGuestRecreatesAfterItsJoinAndNotLongAfter) {
    RecreateAfterJoin host;
    EXPECT_FALSE(host.Tick(false, true, 2, true, 0));
    EXPECT_FALSE(host.Tick(false, true, 2, true, 10000)) << "the host's guests join it";
    RecreateAfterJoin late;
    EXPECT_FALSE(late.Tick(true, true, 1, true, 0));
    EXPECT_FALSE(late.Tick(true, true, 2, true, RecreateAfterJoin::WindowMs + 1))
        << "a third player joining much later builds its own copies";
}

// Krolebord 2026-10-18: two guests joined Kaniel from the team lobby together. Krolebord's
// re-create came 5 s after the join, still in his old world; the join's reload ~20 s later threw
// the copy of Lisara away and he did not see her until the Dream. The settle clock starts once
// the guest is in the session's world.
TEST(BloodbornePartyController, AJoinRecreatesOnlyAfterTheJoinsWorldReload) {
    RecreateAfterJoin recreate;
    EXPECT_FALSE(recreate.Tick(true, true, 2, false, 0)) << "joined: still in its own world";
    EXPECT_FALSE(recreate.Tick(true, true, 2, false, RecreateAfterJoin::SettleMs + 1))
        << "the join's reload is still to come";
    EXPECT_FALSE(recreate.Tick(true, true, 2, false, 20000));
    EXPECT_FALSE(recreate.Tick(true, true, 2, true, 21000)) << "in the session's world: settling";
    EXPECT_TRUE(recreate.Tick(true, true, 2, true, 21000 + RecreateAfterJoin::SettleMs + 1));
    EXPECT_FALSE(recreate.Tick(true, true, 2, true, 40000)) << "once";
}

// Kaniel 2026-10-10: after trips, two players hitting one enemy saw different HP bars, and their
// damage did not add up (each copy took only its own hits; the board kept the lowest). Each
// member posts its own damage and takes the others' on top.
// Kaniel 2026-10-11, after the party wipe in the Cathedral Ward: the reset map's 0x1001006e came
// back at 2116 HP and was lowered to 1837 by the damage the others had posted before the wipe
// (their visits went on until their own reloads), and each member's copy took a different part
// of it. After a death that reset the map, the damage reported before it is not taken.
TEST(BloodborneSharedEnemyHp, AMapResetLeavesTheOldDamageBehind) {
    SharedDamage host;
    constexpr u64 GuestBefore = 0xB0000001, GuestAfter = 0xB0000002;
    const auto copies = [](s32 hp) { return std::vector<EnemyCopy>{{0x1001006e, hp, 2116}}; };
    const std::vector<EnemyHpReport> before{{GuestBefore, {{0x1001006e, 279, 2116}}}};
    EXPECT_EQ(host.Update(1, true, copies(2116), before).writes,
              (std::vector<EnemyWrite>{{0x1001006e, 1837}}))
        << "before the wipe the guest's damage counts";

    host.ForgetDamageBefore(before);
    EXPECT_TRUE(host.Update(2, true, copies(2116), before).writes.empty())
        << "the reloaded copy keeps its full HP";
    const std::vector<EnemyHpReport> after{{GuestBefore, {{0x1001006e, 400, 2116}}},
                                           {GuestAfter, {{0x1001006e, 100, 2116}}}};
    EXPECT_EQ(host.Update(2, true, copies(2116), after).writes,
              (std::vector<EnemyWrite>{{0x1001006e, 2016}}))
        << "only the guest's hits after its own reload";
}

TEST(BloodborneSharedEnemyHp, TwoPlayersHittingOneEnemyAddUp) {
    SharedDamage host;
    SharedDamage guest;
    constexpr u64 HostSource = 0xA0000001, GuestSource = 0xB0000001;
    const auto copies = [](s32 hp) { return std::vector<EnemyCopy>{{0x10014066, hp, 500}}; };
    EXPECT_TRUE(host.Update(1, true, copies(500), {}).post.empty()) << "nothing hit yet";
    EXPECT_TRUE(guest.Update(1, true, copies(500), {}).post.empty());

    // The host hits for 100, the guest for 60, each on its own copy.
    host.Update(1, true, copies(400), {}); // a hit is posted on the second read
    auto from_host = host.Update(1, true, copies(400), {});
    ASSERT_EQ(from_host.post.size(), 1u);
    EXPECT_EQ(from_host.post[0].hp, 100) << "the host's own damage";
    guest.Update(1, true, copies(440), {});
    auto from_guest = guest.Update(1, true, copies(440), {});
    ASSERT_EQ(from_guest.post.size(), 1u);
    EXPECT_EQ(from_guest.post[0].hp, 60);

    // Each takes the other's damage on top: both at 340 (500 - 100 - 60).
    const std::vector<EnemyHpReport> to_host{{GuestSource, from_guest.post}};
    const std::vector<EnemyHpReport> to_guest{{HostSource, from_host.post}};
    EXPECT_EQ(host.Update(1, true, copies(400), to_host).writes,
              (std::vector<EnemyWrite>{{0x10014066, 340}}));
    EXPECT_EQ(guest.Update(1, true, copies(440), to_guest).writes,
              (std::vector<EnemyWrite>{{0x10014066, 340}}));

    // The same reports again change nothing, and the written damage is not posted as one's own.
    const auto again = host.Update(1, true, copies(340), to_host);
    EXPECT_TRUE(again.writes.empty());
    ASSERT_EQ(again.post.size(), 1u);
    EXPECT_EQ(again.post[0].hp, 100);

    // The host hits again (340 -> 290): it posts 150; the guest takes the 50 more.
    host.Update(1, true, copies(290), to_host);
    from_host = host.Update(1, true, copies(290), to_host);
    EXPECT_EQ(from_host.post[0].hp, 150);
    EXPECT_EQ(
        guest.Update(1, true, copies(340), std::vector<EnemyHpReport>{{HostSource, from_host.post}})
            .writes,
        (std::vector<EnemyWrite>{{0x10014066, 290}}));
}

// Kaniel's friend run 2026-10-11: after trips the guest's hits still reach the host's copy
// through the game. The host counted that as its own damage and posted it, so the guest's copy
// took each of the guest's hits a second time half a second later (0x10014066 450 -> 401, its
// own hit, then 401 -> 350 from the board), and the host's copy took the guest's post on top of
// the game's hit. A member's own damage is now what its own hits took off (EnemyCopy::own_hits,
// from the game's hit function); the rest of a drop (another player's hit the game sent) pays for
// the others' posts before anything is written.
TEST(BloodborneSharedEnemyHp, AHitTheGameSendsCountsOnce) {
    SharedDamage host;
    SharedDamage guest;
    constexpr u64 HostSource = 0xA0000001, GuestSource = 0xB0000001;
    const auto board = [](u64 source, const std::vector<EnemyHp>& posted) {
        return std::vector<EnemyHpReport>{{source, posted}};
    };
    const auto at = [](s32 hp, s64 own_hits) {
        return std::vector<EnemyCopy>{{0x10014066, hp, 500, own_hits}};
    };
    host.Update(1, true, at(500, 7), {}); // counted up from before this visit
    guest.Update(1, true, at(500, 0), {});

    // The guest hits for 50: its copy and, through the game, the host's.
    const auto from_guest = guest.Update(1, true, at(450, 50), {});
    ASSERT_EQ(from_guest.post.size(), 1u) << "its own hit: no second read needed";
    EXPECT_EQ(from_guest.post[0].hp, 50);
    const auto from_host = host.Update(1, true, at(450, 7), {});
    EXPECT_TRUE(from_host.post.empty()) << "the guest's hit is not the host's damage";

    const std::vector<EnemyHpReport> to_host{{GuestSource, from_guest.post}};
    EXPECT_TRUE(host.Update(1, true, at(450, 7), to_host).writes.empty())
        << "the host's copy already took it from the game";
    EXPECT_TRUE(
        guest.Update(1, true, at(450, 50), board(HostSource, from_host.post)).writes.empty())
        << "no echo";

    // The host hits for 100 (the game does not send it): the guest takes it from the board.
    const auto hit = host.Update(1, true, at(350, 107), to_host);
    ASSERT_EQ(hit.post.size(), 1u);
    EXPECT_EQ(hit.post[0].hp, 100);
    EXPECT_EQ(guest.Update(1, true, at(450, 50), board(HostSource, hit.post)).writes,
              (std::vector<EnemyWrite>{{0x10014066, 350}}));

    // A guest hit the game did not send (the host's copy was not loaded yet) still comes over
    // the board: only what the game sent is taken off.
    const auto more = guest.Update(1, true, at(320, 80), board(HostSource, hit.post));
    ASSERT_EQ(more.post.size(), 1u);
    EXPECT_EQ(more.post[0].hp, 80);
    EXPECT_EQ(host.Update(1, true, at(350, 107), board(GuestSource, more.post)).writes,
              (std::vector<EnemyWrite>{{0x10014066, 320}}));
}

// What a member's own hits took off each enemy (EnemyCopy::own_hits), and its poison: a status
// effect's HP tick is this member's when its own hit was the last to land on that enemy (Kaniel
// 2026-10-11: keep sharing poison).
TEST(BloodborneSharedEnemyHp, PoisonIsTheLastHittersDamage) {
    OwnDamage own;
    own.Hit(0x10014066, true, 50);
    own.Effect(0x10014066, 8);
    EXPECT_EQ(own.Total(0x10014066), 58) << "poisoned by this member's knife";
    own.Hit(0x10014066, false, 40);
    own.Effect(0x10014066, 8);
    EXPECT_EQ(own.Total(0x10014066), 58) << "another player hit it last";
    own.Hit(0x10014066, true, 0); // a knife that only builds poison up
    own.Effect(0x10014066, 8);
    EXPECT_EQ(own.Total(0x10014066), 66);
    own.Effect(0x10014085, 8);
    EXPECT_EQ(own.Total(0x10014085), 0) << "never hit: the world's own poison (a swamp)";
    own.Hit(0x10014066, true, -5);
    own.Effect(0x10014066, -3);
    EXPECT_EQ(own.Total(0x10014066), 66) << "nothing takes HP back";
    EXPECT_EQ(own.All().size(), 1u);
}

// Kaniel 2026-10-13: his fire bomb took 781 off an enemy and Ada's bar never moved. The game's
// hit had no attacker (a thrown item's hit), so it was no one's: counted as nobody's damage, it
// was never posted. A hit with no attacker is this member's, unless another player hit it last.
TEST(BloodborneSharedEnemyHp, AHitWithNoAttackerIsThisMembers) {
    OwnDamage own;
    own.Hit(0x10014099, HitBy::Nobody, 781);
    EXPECT_EQ(own.Total(0x10014099), 781) << "never hit before: the fire bomb";
    own.Hit(0x10014099, HitBy::Me, 20);
    own.Hit(0x10014099, HitBy::Nobody, 30);
    EXPECT_EQ(own.Total(0x10014099), 831) << "after this member's own hit";
    own.Hit(0x10014099, HitBy::OtherPlayer, 50);
    own.Hit(0x10014099, HitBy::Nobody, 30);
    EXPECT_EQ(own.Total(0x10014099), 831) << "after another player's hit: theirs";
    EXPECT_EQ(own.LastHitMine(0x10014099), std::optional<bool>{false})
        << "a hit with no attacker says nothing about who hit last";
}

// Rig 2026-10-11, damage straight over the P2P link: the guest's report reached the host 0.1 s
// after its hit, before the game's own copy of the hit, so the host wrote 484 -> 382 and the
// game's hit took it to 280 (one hit, counted twice). When the game's copy lands after a write,
// the write is given back; from then on that member's damage waits SharedDamageHoldMs for the
// game's copy before it is written.
TEST(BloodborneSharedEnemyHp, AReportAheadOfTheGamesHitIsGivenBack) {
    SharedDamage host;
    constexpr u64 Guest = 0xB0000001;
    const auto at = [](s32 hp) { return std::vector<EnemyCopy>{{0x1001400b, hp, 484, 0}}; };
    const auto report = [](s32 damage) {
        return std::vector<EnemyHpReport>{{Guest, {{0x1001400b, damage, 484}}}};
    };
    host.Update(1, true, at(484), {}, 1'000);
    EXPECT_EQ(host.Update(1, true, at(484), report(102), 1'100).writes,
              (std::vector<EnemyWrite>{{0x1001400b, 382}}))
        << "the report first: written at once";
    EXPECT_EQ(host.Update(1, true, at(280), report(102), 1'200).writes,
              (std::vector<EnemyWrite>{{0x1001400b, 382}}))
        << "the game's copy of the same hit: the write given back";
    EXPECT_TRUE(host.Update(1, true, at(382), report(102), 1'300).writes.empty());

    // The guest hits again (123): its report now waits for the game's copy.
    EXPECT_TRUE(host.Update(1, true, at(382), report(225), 2'000).writes.empty()) << "held";
    EXPECT_TRUE(host.Update(1, true, at(259), report(225), 2'100).writes.empty())
        << "the game's copy came: nothing to write";
    EXPECT_TRUE(host.Update(1, true, at(259), report(225), 3'000).writes.empty());

    // A hit the game never sends (the host's copy not loaded then) is written after the hold.
    EXPECT_TRUE(host.Update(1, true, at(259), report(275), 4'000).writes.empty());
    EXPECT_EQ(host.Update(1, true, at(259), report(275), 4'000 + SharedDamageHoldMs).writes,
              (std::vector<EnemyWrite>{{0x1001400b, 209}}));
    EXPECT_LE(SharedDamageHoldMs, 300);
}

// The friend run's other half: for two reads the guest's world showed 97 characters far below
// their HP (most never hit), it posted that as its damage, and the host's 97 copies went to 1 HP.
// Counted from its own hits, a member posts nothing for them.
TEST(BloodborneSharedEnemyHp, AWorldShowingLowHpPostsNoDamage) {
    SharedDamage copy;
    const auto at = [](s32 hp) { return std::vector<EnemyCopy>{{0x10024000, hp, 3617, 0}}; };
    copy.Update(1, true, at(3617), {});
    copy.Update(1, true, at(1), {});
    EXPECT_TRUE(copy.Update(1, true, at(1), {}).post.empty());
    EXPECT_TRUE(copy.Update(1, true, at(3617), {}).post.empty());
}

// Kaniel's forest run 2026-10-11: the host's hits (285 on 456 HP enemies) showed their damage
// numbers on the guests at once, but the guests' bars dropped a second later or never (the second
// hit killed it first). A change of this member's damage is posted at once; otherwise the board is
// read every HpBoardRound, and the world is read as often.
TEST(BloodborneSharedEnemyHp, TheBoardCarriesAHitWithinARound) {
    EXPECT_EQ(HpBoardWait(true), std::chrono::milliseconds{0}) << "own damage changed: post now";
    EXPECT_EQ(HpBoardWait(false), HpBoardRound);
    EXPECT_LE(HpBoardRound, std::chrono::milliseconds{100});
}

// Kaniel 2026-10-11: damage straight from game to game over the P2P link (the port's UDP transport,
// "SHAD" frames of type 0x30) instead of through the server. A frame carries the map and this
// member's HP board report, as the board does.
TEST(BloodborneDirectDamage, AFrameCarriesTheMapAndTheReport) {
    const std::vector<EnemyHp> hurt{{0x10008040, 285, 456}};
    const std::vector<u8> report = EncodeEnemyHp(0xA000000500000003, hurt);
    const std::vector<u8> frame = EncodeDirectDamage(0x16000000, report);
    ASSERT_GE(frame.size(), 10u);
    EXPECT_EQ((std::vector<u8>(frame.begin(), frame.begin() + 5)),
              (std::vector<u8>{'S', 'H', 'A', 'D', DirectDamageType}));
    const auto back = DecodeDirectDamage(frame);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->map, 0x16000000u);
    EXPECT_EQ(DecodeEnemyHp(back->bits), (EnemyHpReport{0xA000000500000003, hurt}));
    EXPECT_FALSE(DecodeDirectDamage(std::vector<u8>{'S', 'H', 'A', 'D', 0x10, 1, 0, 0, 0, 0}))
        << "another SHAD frame (signaling control)";
    EXPECT_FALSE(DecodeDirectDamage(std::vector<u8>(frame.begin(), frame.begin() + 8)))
        << "cut short";
}

// A sender's frames stand until they stop coming (it left the room or its game died): its last
// one is dropped DirectDamageLifeMs after it arrived. Frames come again every half second.
TEST(BloodborneDirectDamage, ASendersLastFrameStandsUntilItGoesQuiet) {
    DirectDamageInbox inbox;
    inbox.Take(1, {0x16000000, {1, 2}}, 1'000);
    inbox.Take(2, {0x16000000, {3}}, 1'200);
    inbox.Take(1, {0x16000000, {4}}, 1'400);
    EXPECT_EQ(inbox.Fresh(1'500),
              (std::vector<MemberMapState>{{0x16000000, {4}}, {0x16000000, {3}}}));
    EXPECT_EQ(inbox.Fresh(1'200 + DirectDamageLifeMs + 1),
              (std::vector<MemberMapState>{{0x16000000, {4}}}))
        << "sender 2 went quiet";
    EXPECT_GE(DirectDamageLifeMs, 4 * DirectDamageResendMs);
}

// A member that left the session (MemberLeft): its last frames go at once instead of standing
// for DirectDamageLifeMs (its last "up" counted it as standing), the remaining peers' stay.
TEST(BloodborneDirectDamage, AMemberThatLeftLosesItsFramesAtOnce) {
    DirectDamageInbox damage;
    damage.Take(1, {0x16000000, {1}}, 1'000);
    damage.Take(2, {0x16000000, {2}}, 1'000);
    damage.KeepOnly(std::vector<u64>{2});
    EXPECT_EQ(damage.Fresh(1'100), (std::vector<MemberMapState>{{0x16000000, {2}}}));

    DirectInbox<DirectKills> kills;
    kills.Take(1, DirectKills{"6b", {0x10014068}}, 1'000);
    kills.Take(2, DirectKills{"4c", {0x10014075}}, 1'000);
    kills.KeepOnly(std::vector<u64>{2});
    EXPECT_EQ(DirectKillHandles(kills.Fresh(1'100)), (std::unordered_set<u32>{0x10014075}));
    kills.KeepOnly({});
    EXPECT_TRUE(kills.Fresh(1'100).empty()) << "no peers left";
}

// The same member's report comes both ways (directly and over the board, which lags): per enemy
// the most damage (its counts only grow) or, for HP from one world, the least HP.
TEST(BloodborneDirectDamage, TheNewerOfTwoCopiesOfAReportWins) {
    const std::vector<EnemyHpReport> both{
        {7, {{0x10008040, 100, 456}, {0x10008041, 50, 456}}}, // the board, older
        {7, {{0x10008040, 285, 456}}},                        // direct, newer
        {9, {{0x10008040, 20, 456}}},                         // another member
        {HostHpReport | 5, {{0x10010075, 600, 999}}},         // HP (one world), older
        {HostHpReport | 5, {{0x10010075, 422, 999}}},         // HP, newer
    };
    EXPECT_EQ(MergeEnemyHpReports(both), (std::vector<EnemyHpReport>{
                                             {7, {{0x10008040, 285, 456}, {0x10008041, 50, 456}}},
                                             {9, {{0x10008040, 20, 456}}},
                                             {HostHpReport | 5, {{0x10010075, 422, 999}}},
                                         }));
}

// Kaniel 2026-10-11: party death and kills straight from game to game too ("SHAD" frames of
// DirectVitalsType and DirectKillsType on the same P2P link), the boards underneath.
TEST(BloodborneDirectBoards, CoopFramesCarryTheirTypeAndPayload) {
    const std::vector<u8> payload{1, 2, 3};
    const std::vector<u8> frame = EncodeCoopFrame(DirectVitalsType, payload);
    EXPECT_EQ(frame, (std::vector<u8>{'S', 'H', 'A', 'D', DirectVitalsType, 1, 2, 3}));
    const auto back = DecodeCoopFrame(frame);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->first, DirectVitalsType);
    EXPECT_EQ(back->second, payload);
    EXPECT_FALSE(DecodeCoopFrame(std::vector<u8>{'S', 'H', 'A', 'D', 0x10, 1})) << "signaling";
    EXPECT_FALSE(DecodeCoopFrame(std::vector<u8>{'S', 'H', 'A'})) << "cut short";
    EXPECT_TRUE(IsCoopFrameType(DirectDamageType) && IsCoopFrameType(DirectKillsType));
}

// A member's own vitals line (the board update's format) goes to the peers as it is.
TEST(BloodborneDirectBoards, AVitalsUpdateReadsBack) {
    const MemberVitals self{.member = "6b616e69656c",
                            .map = 0x18010000,
                            .vital = Vital::Down,
                            .hp = 1,
                            .max = 1900,
                            .down = 87180471,
                            .landed = 25,
                            .needed = 1900,
                            .at = {-199.5f, -50.75f, 179.25f}};
    const std::vector<ReviveHitTotal> hits{{"416461", 574863293, 75}};
    const auto back = ParseVitalsUpdate(EncodeVitalsUpdate(self, hits));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->self, self);
    EXPECT_EQ(back->hits, hits);
    EXPECT_FALSE(ParseVitalsUpdate("6b616e69656c 18010000 down"));
}

// The board answer with the peers' own lines put in (fresher), and the revive damage on each
// member's down the most of the board's sum and the direct hits' sum; wipes stay the board's
// (shadNet declares them, once for everyone).
TEST(BloodborneDirectBoards, DirectVitalsGoOverTheBoardsCopy) {
    VitalsBoard board;
    board.members = {
        {.member = "aa", .map = 0x18010000, .vital = Vital::Up, .hp = 400, .max = 500},
        {.member = "bb", .map = 0x18010000, .vital = Vital::Up, .hp = 300, .max = 500, .down = 4},
    };
    board.wipes = {PartyWipe{3, 0x16000000, {"cc"}}};
    std::vector<DirectVitals> direct{
        {MemberVitals{.member = "bb",
                      .map = 0x18010000,
                      .vital = Vital::Down,
                      .hp = 1,
                      .max = 500,
                      .down = 5,
                      .needed = 500},
         {}},
        {MemberVitals{.member = "aa", .map = 0x18010000, .vital = Vital::Up, .hp = 380, .max = 500},
         {{"bb", 5, 120}, {"cc", 9, 40}}},
    };
    const VitalsBoard merged = MergeDirectVitals(board, direct);
    ASSERT_EQ(merged.members.size(), 2u);
    EXPECT_EQ(merged.members[0].hp, 380) << "aa's own line";
    EXPECT_EQ(merged.members[1].vital, Vital::Down) << "bb went down: seen before the board";
    EXPECT_EQ(merged.members[1].revive_damage, 120u) << "aa's hits on bb's down 5";
    EXPECT_EQ(merged.wipes, board.wipes);

    board.members[1] = {.member = "bb", .vital = Vital::Down, .down = 5, .revive_damage = 200};
    EXPECT_EQ(MergeDirectVitals(board, direct).members[1].revive_damage, 200u)
        << "the board had more (another member's hits)";
    direct.push_back({MemberVitals{.member = "dd", .map = 0x18010000, .vital = Vital::Up}, {}});
    EXPECT_EQ(MergeDirectVitals(board, direct).members.size(), 3u) << "not on the board yet";

    // Kaniel 2026-10-17: a member's cutscene vote comes through the server only; its direct line
    // (without one) must not wipe it.
    board.members[0].cutscene = 2;
    EXPECT_EQ(MergeDirectVitals(board, direct).members[0].cutscene, 2u);
    // Kaniel 2026-10-18: so do its vials and bullets (the party list shows them by the name).
    board.members[0].vials = 12;
    board.members[0].bullets = 7;
    EXPECT_EQ(MergeDirectVitals(board, direct).members[0].vials, 12u);
    EXPECT_EQ(MergeDirectVitals(board, direct).members[0].bullets, 7u);
    // Kaniel 2026-10-25: and the bosses its game runs (every game's boss pick reads them).
    board.members[0].bosses = {0x1002405F};
    EXPECT_EQ(MergeDirectVitals(board, direct).members[0].bosses, std::vector<u32>{0x1002405F});
}

// A member's standing kills go to the peers whole (at once on a change, again every half
// second); a peer's frames stand until it goes quiet, and its kills hide enemies at once.
TEST(BloodborneDirectBoards, AMembersKillsTravelWhole) {
    const std::vector<u32> kills{0x10014068, 0x10008040};
    const auto back = DecodeDirectKills(EncodeDirectKills("6b616e69656c", kills));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->member, "6b616e69656c");
    EXPECT_EQ(back->handles, kills);
    EXPECT_FALSE(DecodeDirectKills(std::vector<u8>{3, 'a', 'b'}));

    DirectInbox<DirectKills> inbox;
    inbox.Take(1, *back, 1'000);
    inbox.Take(2, DirectKills{"4c69", {0x10014075}}, 1'100);
    EXPECT_EQ(DirectKillHandles(inbox.Fresh(1'200)),
              (std::unordered_set<u32>{0x10014068, 0x10008040, 0x10014075}));
    inbox.Take(1, DirectKills{"6b616e69656c", {}}, 1'300);
    EXPECT_EQ(DirectKillHandles(inbox.Fresh(1'400)), (std::unordered_set<u32>{0x10014075}))
        << "its visit ended: the enemies came back for it";
    EXPECT_TRUE(DirectKillHandles(inbox.Fresh(1'100 + DirectDamageLifeMs + 1)).empty())
        << "both went quiet";
}

// Robustness review 2026-10-12: two games on different builds share a party without a word. Each
// game's hello (PeerHelloType on the same link) names its co-op protocol, build and event files.
TEST(BloodbornePeerHello, AHelloReadsBack) {
    const PeerHello hello{.protocol = CoopProtocolVersion,
                          .revision = "c01b49d3a1b2-dirty",
                          .event_files = 0x0123456789abcdefull,
                          .member = "6b616e69656c",
                          .build = "bb-build 1;version=1.2.0;harder=off;"};
    const std::vector<u8> payload = EncodePeerHello(hello);
    EXPECT_EQ(DecodePeerHello(payload), hello);
    const auto frame = DecodeCoopFrame(EncodeCoopFrame(PeerHelloType, payload));
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->first, PeerHelloType);
    EXPECT_TRUE(IsCoopFrameType(PeerHelloType));
    EXPECT_NE(PeerHelloType, DirectDamageType);
    EXPECT_NE(PeerHelloType, DirectVitalsType);
    EXPECT_NE(PeerHelloType, DirectKillsType);

    std::vector<u8> longer = payload;
    longer.push_back(7);
    EXPECT_EQ(DecodePeerHello(longer), hello) << "a later build may add fields at the end";
    // Without the features it is a protocol 1 hello (see AHelloAdvertisesTheWorldLog), without the
    // build fingerprint (u16 length, text) an older release's: either reads. Any other cut does
    // not.
    const size_t build_at = payload.size() - 2 - hello.build.size();
    for (size_t size = 0; size < payload.size(); ++size) {
        const auto cut = DecodePeerHello(std::span{payload}.first(size));
        if (size == build_at - 1 || size == build_at) {
            ASSERT_TRUE(cut) << "cut at " << size;
            EXPECT_TRUE(cut->build.empty());
        } else {
            EXPECT_FALSE(cut) << "cut at " << size;
        }
    }
    std::vector<u8> overlong = payload;
    overlong[1] = 0xFF; // the revision's length runs past the end
    EXPECT_FALSE(DecodePeerHello(overlong));
}

// One warning line per member and build: the peer's hello comes every few seconds.
TEST(BloodbornePeerHello, ADifferentBuildIsReportedOncePerPeer) {
    const PeerHello mine{.protocol = CoopProtocolVersion,
                         .revision = "c01b49d3",
                         .event_files = 0x1111,
                         .member = "6b616e69656c"};
    PeerBuildCheck check;
    PeerHello same = mine;
    same.member = "4c69";
    EXPECT_FALSE(check.Take(same, mine)) << "the same build";

    PeerHello older = same;
    older.revision = "7f3343f0";
    const auto warning = check.Take(older, mine);
    ASSERT_TRUE(warning);
    EXPECT_EQ(*warning, "Co-op: 4c69 runs build 7f3343f0, this game runs c01b49d3");
    EXPECT_FALSE(check.Take(older, mine)) << "said once";

    PeerHello other = older;
    other.member = "6c6f7265";
    EXPECT_TRUE(check.Take(other, mine)) << "another peer on that build";
    PeerHello newer = older;
    newer.revision = "0c772ac9";
    EXPECT_TRUE(check.Take(newer, mine)) << "the peer restarted on another build";

    PeerHello protocol = same;
    protocol.member = "70";
    protocol.protocol = CoopProtocolVersion + 1;
    const auto protocol_warning = check.Take(protocol, mine);
    ASSERT_TRUE(protocol_warning);
    EXPECT_NE(protocol_warning->find(fmt::format("co-op protocol {}", CoopProtocolVersion + 1)),
              std::string::npos)
        << *protocol_warning;

    PeerHello events = same;
    events.member = "71";
    events.event_files = 0x2222;
    const auto events_warning = check.Take(events, mine);
    ASSERT_TRUE(events_warning);
    EXPECT_NE(events_warning->find("event files 0000000000002222"), std::string::npos)
        << *events_warning;
    PeerHello unknown = same;
    unknown.member = "72";
    unknown.event_files = 0;
    EXPECT_FALSE(check.Take(unknown, mine)) << "a game that could not read its event files";
}

// The event files a game reads go into its hello: a party on different co-op event files sees
// different worlds (boss fogs, NPCs).
TEST(BloodbornePeerHello, EventFilesHashFollowsTheirBytes) {
    const auto folder = std::filesystem::temp_directory_path() / "bb_event_files_hash_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    EXPECT_EQ(EventFilesHash(folder), 0u) << "no event files";
    EXPECT_EQ(EventFilesHash(folder / "absent"), 0u);
    const auto write = [&](const char* name, std::string_view bytes) {
        std::ofstream{folder / name, std::ios::binary} << bytes;
    };
    write("m21_00_00_00.emevd.dcx", "first");
    write("m24_01_00_00.emevd.dcx", "second");
    write("m21_00_00_00.emeld.dcx", "names only");
    const u64 hash = EventFilesHash(folder);
    EXPECT_NE(hash, 0u);
    EXPECT_EQ(EventFilesHash(folder), hash) << "the same files";
    write("m21_00_00_00.emeld.dcx", "other names");
    EXPECT_EQ(EventFilesHash(folder), hash) << "only the event scripts count";
    write("m24_01_00_00.emevd.dcx", "changed");
    EXPECT_NE(EventFilesHash(folder), hash);
    std::filesystem::remove_all(folder);
}

// Build check on join: one file's bytes (the eboot, patches.bin), 0 when it cannot be read.
TEST(BloodborneBuildCheck, FileHashFollowsTheBytes) {
    const auto file = std::filesystem::temp_directory_path() / "bb_file_hash_test.bin";
    std::filesystem::remove(file);
    EXPECT_EQ(FileHash(file), 0u);
    std::ofstream{file, std::ios::binary} << "eboot";
    const u64 hash = FileHash(file);
    EXPECT_NE(hash, 0u);
    EXPECT_EQ(FileHash(file), hash);
    std::ofstream{file, std::ios::binary} << "other";
    EXPECT_NE(FileHash(file), hash);
    std::filesystem::remove(file);
}

// The game's build fingerprint: its release (the launcher's BB_RELEASE_VERSION), build, files and
// the settings that change the game; the visual ones only for the log.
TEST(BloodborneBuildCheck, TheFingerprintSaysTheBuildAndTheRules) {
    const BuildIdentity identity{.revision = "08e56cd82c5c4f7d1d2e3f4a5b6c7d8e9f0a1b2c",
                                 .eboot = 0x1234567890abcdefull,
                                 .event_files = 0xfedcba0987654321ull,
                                 .patches_bin = 0x42};
    const auto none = [](const char*) -> const char* { return nullptr; };
    const auto plain = CoopBuildFingerprint(identity, none);
    EXPECT_EQ(BloodborneCoop::EncodeBuildFingerprint(plain),
              "bb-build 1;version=dev;revision=08e56cd82c5c;protocol=9;eboot=567890abcdef;"
              "events=ba0987654321;harder=off;cheats=none;rules=lamp rests off, revive hits real;"
              "fps=?;mods=on;patches=none;pbin=000000000042;");

    const std::map<std::string, std::string> settings{
        {"BB_RELEASE_VERSION", "1.2.0"},
        {"SHADPS4_BLOODBORNE_HARDER_ENEMIES", " 3, 1.50 ,3"},
        {"BB_PATCH_GAMEPLAY", "no death, no rally"},
        {"SHADPS4_BLOODBORNE_LAMP_REST", "1"},
        {"SHADPS4_BLOODBORNE_REVIVE_HITS", "presses"},
        {"SHADPS4_BLOODBORNE_GUEST_AS_HOST", "1"},
        {"BB_FPS_PRESET", "90"},
        {"BB_ONLINE_UNLOCKED_FPS", "1"},
        {"BB_MODS_ENABLED", "0"},
        {"BB_PATCH_OTHER", "90 FPS++, Sprint Fix (High FPS)"}};
    const auto env = [&](const char* name) -> const char* {
        const auto found = settings.find(name);
        return found == settings.end() ? nullptr : found->second.c_str();
    };
    const auto set = CoopBuildFingerprint(identity, env);
    EXPECT_EQ(set.Get("version"), "1.2.0");
    EXPECT_EQ(set.Get("harder"), "3,1.5,3") << "the multipliers, as the game reads them";
    EXPECT_EQ(set.Get("cheats"), "no death, no rally");
    EXPECT_EQ(set.Get("rules"), "lamp rests on, revive hits presses, guest as host");
    EXPECT_EQ(set.Get("fps"), "90 unlocked");
    EXPECT_EQ(set.Get("mods"), "off");
    EXPECT_EQ(set.Get("patches"), "90 FPS++, Sprint Fix (High FPS)");

    // Harder settings the game takes as the normal game are "off".
    for (const char* vanilla : {"off", "1,1,1", "0", "nonsense"}) {
        const auto harder = [&](const char* name) -> const char* {
            return std::string_view{name} == "SHADPS4_BLOODBORNE_HARDER_ENEMIES" ? vanilla
                                                                                 : nullptr;
        };
        EXPECT_EQ(CoopBuildFingerprint(identity, harder).Get("harder"), "off") << vanilla;
    }
    // Unknown files and revision say nothing (never compared).
    const auto unknown = CoopBuildFingerprint(BuildIdentity{.revision = "0000000"}, none);
    EXPECT_EQ(unknown.Get("revision"), "");
    EXPECT_EQ(unknown.Get("eboot"), "");
    EXPECT_EQ(unknown.Get("events"), "");
}

// The peers' hellos carry the fingerprint: another game's differences are logged by field (the
// server warns the game of them; this is for the logs of the games already in).
TEST(BloodborneBuildCheck, APeerOnOtherSettingsIsLoggedByField) {
    BloodborneCoop::BuildFingerprint build;
    build.Set("version", "1.2.0");
    build.Set("harder", "off");
    build.Set("fps", "60");
    const PeerHello mine{.protocol = CoopProtocolVersion,
                         .revision = "c01b49d3",
                         .event_files = 0x1111,
                         .member = "6b616e69656c",
                         .build = BloodborneCoop::EncodeBuildFingerprint(build)};
    PeerBuildCheck check;
    PeerHello same = mine;
    same.member = "4c69";
    EXPECT_FALSE(check.Take(same, mine));

    PeerHello harder = same;
    build.Set("harder", "3,1.5,3");
    build.Set("fps", "90 unlocked");
    harder.build = BloodborneCoop::EncodeBuildFingerprint(build);
    const auto warning = check.Take(harder, mine);
    ASSERT_TRUE(warning);
    EXPECT_EQ(*warning, "Co-op: 4c69's build differs from this game's: harder-enemies setting off, "
                        "theirs HP x3 damage x1.5 poise x3; not checked: frame rate 60, theirs "
                        "90 unlocked");
    EXPECT_FALSE(check.Take(harder, mine)) << "said once";

    // A hello with no fingerprint (an older release): its revision, as before.
    PeerHello older = same;
    older.member = "70";
    older.revision = "7f3343f0";
    older.build.clear();
    const auto old_warning = check.Take(older, mine);
    ASSERT_TRUE(old_warning);
    EXPECT_EQ(*old_warning, "Co-op: 70 runs build 7f3343f0 (no build fingerprint: an older "
                            "release), this game runs c01b49d3");
}

TEST(BloodborneSharedEnemyHp, InOneWorldAGuestTakesTheHostsNpcHp) {
    const std::vector<EnemyCopy> guest{{0x10010038, 910, 910}, {0x1001001f, 100, 123}};
    const std::vector<EnemyHpReport> host{
        {HostHpReport | 7, {{0x10010038, 422, 910}, {0x1001001f, 120, 123}}},
        {7, {{0x10010038, 300, 910}}}, // a damage report (worlds apart): not HP
    };
    EXPECT_EQ(LowerToHostHp(guest, host), (std::vector<EnemyWrite>{{0x10010038, 422}}))
        << "down to the host's HP, never up";
    EXPECT_TRUE(LowerToHostHp(guest, {}).empty());
}

TEST(BloodborneSummonJoinWatch, ASummonThatNeverBecomesAJoinIsReportedOnce) {
    using Clock = SummonJoinWatch::Clock;
    const Clock::time_point t0{};
    SummonJoinWatch watch;
    EXPECT_FALSE(watch.Stalled(false, t0 + 60s)) << "no summon yet";
    for (auto t = 0s; t <= 8s; t += 1s) {
        watch.Delivered(t0 + t); // the server hands it over on every sign refresh
        EXPECT_EQ(watch.Stalled(false, t0 + t), t == 8s);
    }
    watch.Delivered(t0 + 9s);
    EXPECT_FALSE(watch.Stalled(false, t0 + 20s)) << "once per summon";
    watch.Delivered(t0 + 40s); // after a gap: the server's next summon
    EXPECT_FALSE(watch.Stalled(false, t0 + 47s));
    EXPECT_TRUE(watch.Stalled(false, t0 + 48s));
}

TEST(BloodborneSummonJoinWatch, AJoinEndsTheWait) {
    using Clock = SummonJoinWatch::Clock;
    const Clock::time_point t0{};
    SummonJoinWatch watch;
    watch.Delivered(t0);
    EXPECT_FALSE(watch.Stalled(true, t0 + 2s));
    EXPECT_FALSE(watch.Stalled(false, t0 + 30s)) << "it joined";
}

// Friend run 2026-10-10 by Vicar Amelia: for one read the host's world showed 77 enemies at 1 HP
// (characters being hidden and loaded), it posted that as its damage, the guest's copies went to
// 1 HP and a boss died ("Prey Slaughtered", +11800 echoes). Damage counts once two reads agree.
TEST(BloodborneSharedEnemyHp, AOneReadBlipIsNoDamage) {
    SharedDamage copy;
    const auto at = [](s32 hp) { return std::vector<EnemyCopy>{{0x10008000, hp, 979}}; };
    EXPECT_TRUE(copy.Update(1, true, at(979), {}).post.empty());
    EXPECT_TRUE(copy.Update(1, true, at(1), {}).post.empty()) << "one read: not yet";
    EXPECT_TRUE(copy.Update(1, true, at(979), {}).post.empty()) << "back: it was a blip";
    EXPECT_TRUE(copy.Update(1, true, at(800), {}).post.empty());
    const auto hit = copy.Update(1, true, at(800), {});
    ASSERT_EQ(hit.post.size(), 1u) << "two reads agree: a hit";
    EXPECT_EQ(hit.post[0].hp, 179);
    const auto more = copy.Update(1, true, at(700), {});
    ASSERT_EQ(more.post.size(), 1u);
    EXPECT_EQ(more.post[0].hp, 179) << "the newest read waits for the next";
}

TEST(BloodborneSharedEnemyHp, OtherMembersDamageNeverKillsAndScalesByMaxHp) {
    SharedDamage copy;
    const std::vector<EnemyCopy> one{{0x10014066, 1000, 1000}};
    copy.Update(1, true, one, {});
    // Another member with a different max HP (another HP setting): half its max is half of ours.
    EXPECT_EQ(
        copy.Update(1, true, one, std::vector<EnemyHpReport>{{7, {{0x10014066, 250, 500}}}}).writes,
        (std::vector<EnemyWrite>{{0x10014066, 500}}));
    EXPECT_EQ(copy.Update(1, true, std::vector<EnemyCopy>{{0x10014066, 500, 1000}},
                          std::vector<EnemyHpReport>{{7, {{0x10014066, 600, 500}}}})
                  .writes,
              (std::vector<EnemyWrite>{{0x10014066, 1}}))
        << "more than is left: 1 HP, the game kills it";
}

TEST(BloodborneSharedEnemyHp, ASessionWithOneWorldSharesNothing) {
    SharedDamage copy;
    const std::vector<EnemyCopy> hurt{{0x10014066, 300, 500}};
    const std::vector<EnemyHpReport> others{{7, {{0x10014066, 100, 500}}}};
    const auto step = copy.Update(1, false, hurt, others);
    EXPECT_TRUE(step.post.empty() && step.writes.empty())
        << "out of a session, or one world: the game sends the HP itself";
    copy.Update(1, true, hurt, {});
    EXPECT_TRUE(copy.Update(1, true, hurt, {}).post.empty())
        << "damage from before counts from where it stood then";
    EXPECT_TRUE(
        copy.Update(2, true, std::vector<EnemyCopy>{{0x10014066, 500, 500}}, {}).post.empty())
        << "a new visit starts over";
}

// Rig run 20261013-011544 (joins, hp-across-blocks): in one world the host hurt 0x1001400b
// 484 -> 200; the guest then lamp-travelled to the host's map and the worlds went apart. The host
// counted its damage from 200 (its counts started over at the switch) while its one-world HP
// report went away, so the guest's fresh copy stayed at 484. The host's copy is the one world's:
// all of its drop there (its own hits and the guest's, which the game sent) is posted once apart.
TEST(BloodborneSharedEnemyHp, TheHostsOneWorldDamageCarriesIntoWorldsApart) {
    constexpr u32 Wolf = 0x1001400b;
    const auto at = [](s32 hp) { return std::vector<EnemyCopy>{{Wolf, hp, 484}}; };
    SharedDamage host;
    EXPECT_TRUE(host.Update(1, false, at(484), {}).post.empty());
    const auto one_world = host.Update(1, false, at(200), {});
    EXPECT_TRUE(one_world.post.empty() && one_world.writes.empty()) << "one world: nothing";
    EXPECT_EQ(host.Update(1, true, at(200), {}).post, (std::vector<EnemyHp>{{Wolf, 284, 484}}));
    EXPECT_EQ(host.Update(1, true, at(200), {}).post, (std::vector<EnemyHp>{{Wolf, 284, 484}}));

    // Counted hit by hit: the guest's hits the game sent in one world count as the host's too
    // (the guest starts over apart and never posts them).
    const auto hit = [](s32 hp, s64 hits) { return std::vector<EnemyCopy>{{Wolf, hp, 484, hits}}; };
    SharedDamage counted;
    counted.Update(1, false, hit(484, 0), {});
    counted.Update(1, false, hit(300, 100), {}); // 100 by the host, 84 by the guest
    counted.Update(1, false, hit(300, 100), {});
    EXPECT_EQ(counted.Update(1, true, hit(300, 100), {}).post,
              (std::vector<EnemyHp>{{Wolf, 184, 484}}));
    EXPECT_EQ(counted.Update(1, true, hit(250, 150), {}).post,
              (std::vector<EnemyHp>{{Wolf, 234, 484}}))
        << "and its hits apart on top";
}

// The guest's side of that switch: its copy took the host's HP in one world (the game's sync or
// LowerToHostHp), and its own hits there reached the host by the game. None of that is its damage
// apart: the host posts it.
TEST(BloodborneSharedEnemyHp, AGuestsOneWorldDropIsNotItsDamageApart) {
    constexpr u32 Wolf = 0x1001400b;
    const auto at = [](s32 hp) { return std::vector<EnemyCopy>{{Wolf, hp, 484}}; };
    SharedDamage guest;
    guest.Update(1, false, at(484), {}, 0, PartyRole::Guest);
    guest.Update(1, false, at(200), {}, 0, PartyRole::Guest);
    guest.Update(1, false, at(200), {}, 0, PartyRole::Guest);
    EXPECT_TRUE(guest.Update(1, true, at(200), {}, 0, PartyRole::Guest).post.empty());
    EXPECT_TRUE(guest.Update(1, true, at(200), {}, 0, PartyRole::Guest).post.empty());
    guest.Update(1, true, at(150), {}, 0, PartyRole::Guest);
    EXPECT_EQ(guest.Update(1, true, at(150), {}, 0, PartyRole::Guest).post,
              (std::vector<EnemyHp>{{Wolf, 50, 484}}))
        << "its hits apart are its own";
}

// Rig test 2026-10-09 ("apart"): a guest's trip out of the host's map posted 2 kills at its
// start (the old map's characters going away), so the host hid 2 living enemies while the guest
// was in the Dream. Deaths are only watched while the world stands: not on a trip or a load.
TEST(BloodborneSharedKills, NoKillsAreWatchedWhileTheWorldGoesAway) {
    EXPECT_TRUE(WatchesKills(true, false, false));
    EXPECT_FALSE(WatchesKills(true, true, false)) << "a party trip under way";
    EXPECT_FALSE(WatchesKills(true, false, true)) << "a load";
    EXPECT_FALSE(WatchesKills(false, false, false)) << "not sharing";
}

// What counts as a kill is a real death, from a character's death flags (+0x1E3): bit 0 from the
// game's kill (an event's ForceCharacterDeath, 0x018BFAD3), bit 2 from a death by damage (set
// after the kill reward, 0x018C52D1). Only bit 0 counted at first, so a fight's kills never
// reached the kill board (the rig: the host killed an enemy, +995 echoes, no kill posted; a
// friend run: 67 kill rewards, 2 kills posted). 0 HP alone is not one: a map torn down reads 0.
TEST(BloodborneSharedKills, AKillIsADeathTheGameMarked) {
    EXPECT_TRUE(IsRealDeath(0x01)) << "the game's kill";
    EXPECT_TRUE(IsRealDeath(0x04)) << "killed in a fight";
    EXPECT_TRUE(IsRealDeath(0x05));
    EXPECT_FALSE(IsRealDeath(0x00)) << "alive, or a map going away";
    EXPECT_FALSE(IsRealDeath(0x08)) << "bit 3 is something else (set and cleared at 0x018C2605)";
    EXPECT_FALSE(IsRealDeath(0xF2));
}

// Playtest 2026-10-13: Father Gascoigne's copy on a guest sat at 0 HP without the game's death
// (its death flags 0x40) after the guest's own last hit, so the boss never ended there and his lamp
// and talk stayed as in the fight. A member's kill of a copy stuck at 0 HP for StuckDeathMs gets
// the game's kill; not a real death already, not one its world put down, not before the time.
TEST(BloodborneSharedKills, AMemberKillStuckAtZeroHpGetsTheGamesKill) {
    EXPECT_TRUE(FinishesStuckDeath(true, 0, 0x40, 1'000, 1'000 + StuckDeathMs));
    EXPECT_FALSE(FinishesStuckDeath(true, 0, 0x40, 1'000, 1'000 + StuckDeathMs - 1)) << "too soon";
    EXPECT_FALSE(FinishesStuckDeath(false, 0, 0x40, 1'000, 9'000)) << "nobody killed it";
    EXPECT_FALSE(FinishesStuckDeath(true, 5, 0x00, 1'000, 9'000)) << "alive";
    EXPECT_FALSE(FinishesStuckDeath(true, 0, 0x05, 1'000, 9'000)) << "the game's death came";
    EXPECT_FALSE(FinishesStuckDeath(true, 0, 0x20, 1'000, 9'000)) << "put down by its world";
    EXPECT_FALSE(FinishesStuckDeath(true, 0, 0x40, std::nullopt, 9'000));
}

// Ada 2026-10-13: an enemy the host killed stayed on the guest at its last HP for 1-2.4 s, the
// time from the killing blow to the game's death bit (set after the kill reward). A hit seen to
// leave it at 0 HP counts at once; 0 HP with no such hit (a map torn down) still does not, nor a
// world's put-down (bit 5), nor a zero hit seconds old.
TEST(BloodborneSharedKills, AHitToZeroIsAKillAtOnce) {
    EXPECT_TRUE(SeenDead(0x00, 0, 1'000, 1'200)) << "hit to 0, no death bit yet";
    EXPECT_FALSE(SeenDead(0x00, 0, std::nullopt, 1'200)) << "a map going away";
    EXPECT_FALSE(SeenDead(0x20, 0, 1'000, 1'200)) << "put down by its world";
    EXPECT_FALSE(SeenDead(0x00, 0, 1'000, 1'000 + ZeroHitCountsMs + 1)) << "an old zero hit";
    EXPECT_FALSE(SeenDead(0x00, 12, 1'000, 1'200)) << "alive";
    EXPECT_TRUE(SeenDead(0x04, 0, std::nullopt, 1'200)) << "the game's death bit";

    OwnDamage damage;
    EXPECT_EQ(damage.ZeroHitMs(0x10014087), std::nullopt);
    damage.HitLeft(0x10014087, 0, 5'000);
    EXPECT_EQ(damage.ZeroHitMs(0x10014087), std::optional<s64>{5'000});
    damage.HitLeft(0x10014087, 30, 6'000);
    EXPECT_EQ(damage.ZeroHitMs(0x10014087), std::nullopt) << "it came back up";
}

// The dog test again (2026-10-09, after a reboot): the host left for the Dream and the guest's own
// world put down Father Gascoigne, beaten in its save, a second before the guest saw the host
// gone, so the settle after its takeover came too late. A world's put-down has bit 5 (0x26, 48
// times in the day's rig logs); a fight's kill reads 0x6 and stays so (21 later snapshots).
TEST(BloodborneSharedKills, AnEnemyTheWorldPutsDownIsNoKill) {
    EXPECT_FALSE(IsRealDeath(0x26)) << "Gascoigne, dead in the guest's save";
    EXPECT_TRUE(IsRealDeath(0x06)) << "killed in a fight";
}

// A guest's trip loads the host's map with the guest's own save: a dog killed for good on the
// host's save stood alive for it. The host posts its map's flags (10000 from the map's base:
// 1241xxxx for m24_01) and the guest loading that map takes them, except its own pickups, the
// messengers' gifts and a boss fog's entered flags.
TEST(BloodborneSharedWorld, AGuestOnTheHostsMapTakesTheHostsState) {
    EXPECT_EQ(MapFlagBase(0x18010000), 12410000) << "Central Yharnam";
    EXPECT_EQ(MapFlagBase(0x18000000), 12400000) << "Cathedral Ward";
    EXPECT_EQ(MapFlagBase(0x15000000), 12100000) << "Hunter's Dream";
    EXPECT_TRUE(TakesHostMapFlag(12410102)) << "a shortcut";
    EXPECT_TRUE(TakesHostMapFlag(12411700)) << "a boss killed";
    EXPECT_FALSE(TakesHostMapFlag(52410360)) << "outside the map's range";
    EXPECT_FALSE(TakesHostMapFlag(12101020)) << "a messenger gift";
    EXPECT_FALSE(TakesHostMapFlag(12414700)) << "a boss fog entered";
    EXPECT_FALSE(TakesHostMapFlag(12414701));
    const std::vector<u8> bits{0x00, 0xFF, 0x10};
    EXPECT_EQ(EncodeMapStateUpdate("host", 0x18010000, bits), "host 402718720 00ff10");
    EXPECT_EQ(EncodeMapStateUpdate("guest", 0, {}), "guest 0");
    const auto board = ParseMapStateBoard("402718720 00ff10\n");
    ASSERT_TRUE(board.has_value());
    ASSERT_EQ(board->size(), 1u);
    EXPECT_EQ((*board)[0].map, 0x18010000u);
    EXPECT_EQ((*board)[0].bits, bits);
    EXPECT_EQ(ParseMapStateBoard(""), (std::optional<std::vector<MemberMapState>>{{}}));
    EXPECT_EQ(ParseMapStateBoard("1 0g\n"), std::nullopt);
}

// A guest that played for hours in a session and quit restarted where it was first summoned
// from (its save's position) and had to walk back. Its last map and position are remembered
// (every few seconds, in and out of a session) and the next start goes back there: a trip to the
// map's first lamp when it loads elsewhere, then a move to the position. A new character in the
// same slot (far fewer event flags set than when the point was kept) starts where it starts.
TEST(BloodborneGuestResume, AGuestResumesWhereItLeft) {
    const ResumePoint point{0x18000000, {-120.5f, 10.25f, 33.0f}, 900};
    EXPECT_EQ(EncodeResumePoint(point), "402653184 -120.5 10.25 33 900");
    EXPECT_EQ(ParseResumePoint("402653184 -120.5 10.25 33 900"), std::optional<ResumePoint>{point});
    EXPECT_EQ(ParseResumePoint("402653184 1 2 3"), std::nullopt);
    EXPECT_EQ(ParseResumePoint(""), std::nullopt);

    EXPECT_EQ(MapWarpPoint(0x18000000), 2402950) << "the Cathedral Ward's first lamp";
    EXPECT_EQ(MapWarpPoint(0x18010000), 2412950);
    EXPECT_EQ(MapWarpPoint(0x15000000), 2102962) << "the Hunter's Dream: its headstones' point";

    using Kind = ResumeStep::Kind;
    const LampPosition near{-121.0f, 10.0f, 33.5f};
    const LampPosition far{50.0f, 0.0f, 0.0f};
    EXPECT_EQ(ChooseResumeStep(point, 0x18000000, near, 905).kind, Kind::Stay) << "already there";
    EXPECT_EQ(ChooseResumeStep(point, 0x18000000, far, 905).kind, Kind::Move);
    const ResumeStep travel = ChooseResumeStep(point, 0x18010000, far, 900);
    EXPECT_EQ(travel.kind, Kind::Travel);
    EXPECT_EQ(travel.warp_point, 2402950);
    EXPECT_EQ(ChooseResumeStep(ResumePoint{0, {}, 0}, 0x18010000, far, 900).kind, Kind::Stay)
        << "nothing remembered";
    EXPECT_EQ(ChooseResumeStep(point, 0x18010000, far, 120).kind, Kind::Forget)
        << "a new character in the slot";
}

// Phase 7: a guest whose game died joins its team at once on its next run, so its own file's
// point (kept only out of a session) never counts there; the server keeps where it last stood in
// the team's world, and that point counts, in a session or not (it is the newest).
TEST(BloodborneGuestResume, TheServersPointCountsInATeamSession) {
    const ResumePoint file{0x18000000, {-120.5f, 10.25f, 33.0f}, 900};
    const ResumePoint server{0x18010000, {-215.5f, -51.0f, 172.25f}, 0};
    EXPECT_EQ(PickResumePoint(false, file, std::nullopt), std::optional{file});
    EXPECT_EQ(PickResumePoint(true, file, std::nullopt), std::nullopt);
    EXPECT_EQ(PickResumePoint(true, file, server), std::optional{server});
    EXPECT_EQ(PickResumePoint(false, std::nullopt, server), std::optional{server});
    EXPECT_EQ(PickResumePoint(false, std::nullopt, std::nullopt), std::nullopt);
}

// Play test 2026-10-18 (v0.3.11-local): Krolebord's team load put him back where he had left the
// world, in the Hunter's Dream, while Kaniel, the team's host, loaded in Central Yharnam; the
// team's screen opened with them apart. Kaniel: after the team's loading screen they are together.
// A guest's team load takes its host's place (up or down where it stands) over its own last one.
TEST(BloodborneGuestResume, ATeamLoadPutsTheGuestWhereItsHostStands) {
    const ResumePoint own{0x15000000, {-2.1f, -6.4f, -21.4f}, 0};
    MemberVitals host{.member = "kaniel", .map = 0x18010000, .vital = Vital::Up};
    host.at = {-215.5f, -51.0f, 172.25f};
    MemberVitals guest{.member = "krolebord", .map = 0x15000000, .vital = Vital::Up};
    const std::vector<MemberVitals> board{guest, host};
    const auto place = HostPlaceOf(board, "kaniel");
    ASSERT_TRUE(place.has_value());
    EXPECT_EQ(place->map, 0x18010000u);
    EXPECT_EQ(place->position, host.at);
    EXPECT_EQ(PickResumePoint(true, std::nullopt, own, place), place) << "the host's, not its own";
    EXPECT_EQ(PickResumePoint(false, std::nullopt, own, std::nullopt), std::optional{own});
    // Down where it stands still counts; off (loading, at the title) or nowhere does not.
    host.vital = Vital::Down;
    EXPECT_TRUE(HostPlaceOf(std::vector{host}, "kaniel").has_value());
    host.vital = Vital::Off;
    EXPECT_FALSE(HostPlaceOf(std::vector{host}, "kaniel").has_value());
    host.vital = Vital::Up;
    host.map = 0;
    EXPECT_FALSE(HostPlaceOf(std::vector{host}, "kaniel").has_value());
    EXPECT_FALSE(HostPlaceOf(board, "").has_value()) << "no team host";
    EXPECT_FALSE(HostPlaceOf(board, "someone").has_value());
    // On another map it travels there (its first lamp), then stands at the host's spot.
    const ResumeStep step =
        ChooseResumeStep(*place, 0x15000000, own.position, 0, 0x15000000, own.position);
    EXPECT_EQ(step.kind, ResumeStep::Kind::Travel);
    EXPECT_EQ(step.warp_point, 2412950);
}

// Rig resume 2026-10-18: a guest whose game died in its host's world, 15 m from the lamp, came
// back at the lamp beside the host instead of where it was (phase 7). Its own last place is on
// the host's map already: together, so it counts; the host's place counts only from elsewhere.
TEST(BloodborneGuestResume, ATeamGuestOnItsHostsMapKeepsItsOwnPlace) {
    const ResumePoint own{0x18010000, {-215.0f, -54.5f, 178.0f}, 0};
    const ResumePoint host{0x18010000, {-199.7f, -50.7f, 179.0f}, 0};
    EXPECT_EQ(PickResumePoint(true, std::nullopt, own, host), std::optional{own});
    const ResumePoint file{0x18010000, {-215.0f, -54.5f, 178.0f}, 900};
    EXPECT_EQ(PickResumePoint(false, file, std::nullopt, host), std::optional{file});
    EXPECT_EQ(PickResumePoint(true, file, std::nullopt, host), std::optional{host})
        << "its file's point does not count in a session";
    const ResumePoint dream{0x15000000, {-2.1f, -6.4f, -21.4f}, 0};
    EXPECT_EQ(PickResumePoint(true, std::nullopt, dream, host), std::optional{host});
}

// The host's place may not be known yet as the guest's world comes in (both loading under the
// team's screen): the guest waits for it a while before it falls back to its own last place.
TEST(BloodborneGuestResume, ATeamGuestWaitsForItsHostsPlaceAWhile) {
    EXPECT_TRUE(WaitsForHostPlace(true, false, 0));
    EXPECT_TRUE(WaitsForHostPlace(true, false, HostPlaceWaitMs - 1));
    EXPECT_FALSE(WaitsForHostPlace(true, false, HostPlaceWaitMs)) << "falls back";
    EXPECT_FALSE(WaitsForHostPlace(true, true, 0)) << "known";
    EXPECT_FALSE(WaitsForHostPlace(false, false, 0)) << "not a team's load";
}

// Kaniel's play test 2026-10-19: krolebord's team load moved him to his last place, then his join's
// reload put him back where his save stood. A team load resumes once the guest is in the session
// (its join's reload done), or falls back after a while without one.
TEST(BloodborneGuestResume, ATeamGuestResumesOnceItIsInTheSession) {
    EXPECT_TRUE(WaitsForTeamSession(true, false, 0));
    EXPECT_TRUE(WaitsForTeamSession(true, false, TeamSessionWaitMs - 1));
    EXPECT_FALSE(WaitsForTeamSession(true, false, TeamSessionWaitMs)) << "falls back";
    EXPECT_FALSE(WaitsForTeamSession(true, true, 0)) << "in the session";
    EXPECT_FALSE(WaitsForTeamSession(false, false, 0)) << "not a team's load";
}

// Phase 7: the resume moves a guest only while it still stands where its load put it; one that
// already moved (a player who set off, the rig's own steps) stays (rig party-death 2026-10-15:
// the resume teleported a guest in the middle of its fall).
TEST(BloodborneGuestResume, AGuestThatMovedSinceItsLoadStays) {
    using Kind = ResumeStep::Kind;
    const ResumePoint point{0x18010000, {-215.5f, -51.0f, 172.25f}, 0};
    const LampPosition loaded{-199.7f, -50.7f, 179.4f};
    const LampPosition walked{-196.7f, -50.5f, 179.4f}; // 3 m on
    EXPECT_EQ(ChooseResumeStep(point, 0x18010000, loaded, 0, 0x18010000, loaded).kind, Kind::Move);
    EXPECT_EQ(ChooseResumeStep(point, 0x18010000, walked, 0, 0x18010000, loaded).kind, Kind::Stay);
}

// Ada's new character (2026-10-12) started in Central Yharnam: slot 5 still had the old
// character's resume point, and its event flag count was too close to tell them apart. Once the
// slot is found to hold another character (ForgetsSlotLists), its resume point goes with the
// kept lists.
TEST(BloodborneGuestResume, ANewCharacterForgetsTheSlotsResumePoint) {
    EXPECT_EQ(SlotFileName("resume", 5), "bloodborne_party_resume_slot5.txt");
    EXPECT_EQ(SlotFileName("progress", 0), "bloodborne_party_progress_slot0.txt");
    for (const std::string_view list : {"progress", "world", "confirmed", "resume"}) {
        EXPECT_NE(std::ranges::find(CharacterSlotFiles, list), CharacterSlotFiles.end()) << list;
    }
}

// The Dream messengers' gifts are each player's own (12101020-29, KeepsLocalFlag): the host's
// story hides its messengers once it took its gifts (2004:29), and a guest copying that hid its
// own before it took them (Ada 2026-10-12). Their entity ids (m21_00 MSB: the right- and
// left-hand weapon givers and their weapon displays, the note and the multiplayer items).
TEST(BloodborneShownBoard, TheDreamMessengersAreEachPlayersOwn) {
    for (const s32 entity : {2100215, 2100216, 2100220, 2100221, 2100230, 2100231}) {
        EXPECT_FALSE(FollowsTheHostsShown(entity)) << entity;
    }
    EXPECT_TRUE(FollowsTheHostsShown(2100700)) << "the Doll";
    EXPECT_TRUE(FollowsTheHostsShown(-1)) << "no entity id";
    EXPECT_TRUE(FollowsTheHostsShown(2410000));
}

// Review 2026-10-12: a messenger the guest hid or switched off before it was known as its own (a
// reload of the Dream, the entity table filled a moment later) is shown and switched on again
// once it is, and leaves the lists; nothing else would undo it until a map change.
TEST(BloodborneShownBoard, AnOwnCharacterHiddenBeforeItWasKnownComesBack) {
    const std::unordered_set<u32> hidden_here{0x10000016, 0x10000040};
    const std::unordered_set<u32> switched_off{0x10000017, 0x10000041};
    const std::vector<u32> own{0x10000016, 0x10000017, 0x1000001d};
    const OwnRestore restore = RestoresOwnCharacters(own, hidden_here, switched_off);
    EXPECT_EQ(restore.show, std::vector<u32>{0x10000016});
    EXPECT_EQ(restore.switch_on, std::vector<u32>{0x10000017});
}

// The point is about where the game loaded the guest: a guest that travels on (a lamp, a trip)
// before it is settled there has chosen where to be. The rig's guest went to the Cathedral Ward
// right after loading, and the resume took it back to Central Yharnam, away from its host.
TEST(BloodborneGuestResume, NotOnceTheGuestTravelledOn) {
    using Kind = ResumeStep::Kind;
    const ResumePoint point{0x18010000, {-199.7f, -50.8f, 179.4f}, 900};
    const LampPosition ward{17.6f, -9.5f, 102.7f};
    EXPECT_EQ(ChooseResumeStep(point, 0x18000000, ward, 900, 0x18010000).kind, Kind::Stay)
        << "loaded in Central Yharnam, then travelled to the Ward";
    EXPECT_EQ(ChooseResumeStep(point, 0x18000000, ward, 900, 0x18000000).kind, Kind::Travel)
        << "loaded in the Ward: back to the point";
    EXPECT_EQ(ChooseResumeStep(point, 0x18000000, ward, 900).kind, Kind::Travel)
        << "where it loaded not known: as before";
}

// The game is called with an object only when its vtable says it is the kind the function takes:
// on 2026-10-11 the bloodstain show was given another object and two guests' games crashed.
TEST(BloodborneGameObjects, AnObjectIsTheKindItsVtableSays) {
    constexpr u64 Base = 0x8'0000'0000;
    const std::array stain{BloodstainVtable};
    EXPECT_TRUE(IsGameVtable(Base + BloodstainVtable, Base, stain));
    EXPECT_FALSE(IsGameVtable(Base + MapItemManVtable, Base, stain)) << "another kind of object";
    EXPECT_FALSE(IsGameVtable(0, Base, stain)) << "no vtable (unreadable or freed)";
    EXPECT_FALSE(IsGameVtable(BloodstainVtable, Base, stain)) << "not in the game's image";
    EXPECT_FALSE(IsGameVtable(Base + BloodstainVtable, 0, stain)) << "no game loaded";
    for (const u64 vtable : CharacterVtables) {
        EXPECT_TRUE(IsGameVtable(Base + vtable, Base, CharacterVtables)) << std::hex << vtable;
    }
    for (const u64 vtable : PlayerVtables) {
        EXPECT_TRUE(IsGameVtable(Base + vtable, Base, CharacterVtables))
            << "a player is a character " << std::hex << vtable;
    }
    EXPECT_FALSE(IsGameVtable(Base + EventFlagManVtable, Base, CharacterVtables));
}

// A refused game call is logged once per call site and vtable seen, and never more than
// RefusedCallLog::MaxLogged times, so a wrong vtable list shows in the log without spamming it.
TEST(BloodborneGameObjects, ARefusalIsLoggedOncePerSiteAndVtable) {
    RefusedCallLog log;
    EXPECT_TRUE(log.FirstTime("EventFlagOn", 0x8'0532'0000));
    EXPECT_FALSE(log.FirstTime("EventFlagOn", 0x8'0532'0000)) << "the same pair again";
    EXPECT_TRUE(log.FirstTime("EventFlagOn", 0x8'0533'0000)) << "another vtable";
    EXPECT_TRUE(log.FirstTime("ShowGuestBloodstain", 0x8'0532'0000)) << "another call site";
    for (u64 vtable = 1; vtable < 100; ++vtable) {
        log.FirstTime("Kill", vtable);
    }
    EXPECT_FALSE(log.FirstTime("Teleport", 7)) << "capped";
    EXPECT_FALSE(log.FirstTime("EventFlagOn", 0x8'0532'0000));
}

TEST(BloodborneGameObjects, ASeenVtableIsNamedByItsImageOffset) {
    constexpr u64 Base = 0x8'0000'0000;
    EXPECT_EQ(DescribeVtable(Base + EventFlagManVtable, Base, 0x6000000), "eboot+0x5320b60");
    EXPECT_EQ(DescribeVtable(0x12'3456'7890, Base, 0x6000000), "0x1234567890")
        << "outside the image";
    EXPECT_EQ(DescribeVtable(0, Base, 0x6000000), "0x0");
}
