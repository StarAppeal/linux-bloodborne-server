// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A party of three: the host and two guests. Every rule that used to think of "the other
// player" has to hold with two others, who can be on different maps.

#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

namespace {

PartyObservation HostingWithOneGuest() {
    return PartyObservation{
        .player_valid = true,
        .map_usable = true,
        .stage_transition = false,
        .matching_idle = false, // hosting
        .search_effect_active = false,
        .room_for_more = true, // a third member is online and not in the session
    };
}

const PartyController::Clock::time_point T0{};

PartyAction TickUntilDecision(PartyController& controller, const PartyObservation& observation) {
    for (int i = 0; i < 100; ++i) {
        if (controller.Tick(observation, T0) == PartyAction::StartSearch) {
            return PartyAction::StartSearch;
        }
    }
    return PartyAction::Wait;
}

std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

} // namespace

// The Beckoning Bell's effect ends when a cooperator arrives. With a second guest still out of
// the session the host rings again; a guest in the session, or a full session, does not.
TEST(BloodborneThreePlayers, TheHostRingsAgainForTheSecondGuest) {
    PartyController host(PartyRole::Host);
    EXPECT_EQ(TickUntilDecision(host, HostingWithOneGuest()), PartyAction::StartSearch);

    PartyController full(PartyRole::Host);
    PartyObservation everyone_in = HostingWithOneGuest();
    everyone_in.room_for_more = false;
    EXPECT_EQ(TickUntilDecision(full, everyone_in), PartyAction::Wait);

    PartyController ringing(PartyRole::Host);
    PartyObservation already = HostingWithOneGuest();
    already.search_effect_active = true;
    EXPECT_EQ(TickUntilDecision(ringing, already), PartyAction::Wait);

    PartyController guest(PartyRole::Guest);
    EXPECT_EQ(TickUntilDecision(guest, HostingWithOneGuest()), PartyAction::Wait)
        << "a guest in a session never searches";
}

// On the port the bell's effect stayed on after the first guest arrived while the game had
// stopped searching (rig join3 2026-10-09: the second guest's sign was never taken). A host with
// room whose bell is on and whose game has posted no search for StaleSearchAfter ends the bell
// (EndSearch), and rings again once the effect is off.
TEST(BloodborneThreePlayers, TheHostRingsAgainWhenItsBellOutlastsTheSearch) {
    PartyController host(PartyRole::Host);
    PartyObservation ringing = HostingWithOneGuest();
    ringing.search_effect_active = true;
    EXPECT_EQ(TickUntilDecision(host, ringing), PartyAction::Wait) << "just rung";
    EXPECT_EQ(host.Tick(ringing, T0 + std::chrono::seconds{10}), PartyAction::Wait);
    EXPECT_EQ(host.Tick(ringing, T0 + PartyController::StaleSearchAfter), PartyAction::EndSearch);
    EXPECT_EQ(host.Tick(ringing, T0 + PartyController::StaleSearchAfter + std::chrono::seconds{1}),
              PartyAction::Wait)
        << "the effect is still on: waits for it to go";
    PartyObservation ended = ringing;
    ended.search_effect_active = false;
    EXPECT_EQ(host.Tick(ended, T0 + PartyController::StaleSearchAfter + std::chrono::seconds{2}),
              PartyAction::StartSearch)
        << "rung again on a later tick, once the effect is off";

    PartyController full(PartyRole::Host);
    PartyObservation everyone_in = ringing;
    everyone_in.room_for_more = false;
    EXPECT_EQ(full.Tick(everyone_in, T0), PartyAction::Wait);
    EXPECT_EQ(full.Tick(everyone_in, T0 + PartyController::RetryDelay * 2), PartyAction::Wait)
        << "a full session never rings";
}

// Rig join3 at 30 FPS (2026-10-12, run 131858): the host's game was still searching (a
// /summon_messenger/get every 5 s) when the bell had been on for RetryDelay; ending the effect
// and ringing in the same tick stopped the game's search for 2m45s, and the third guest waited.
// A bell is stale only while the game posts no search.
TEST(BloodborneThreePlayers, ABellWhoseSearchStillPostsIsNeverRungAgain) {
    PartyController host(PartyRole::Host);
    PartyObservation ringing = HostingWithOneGuest();
    ringing.search_effect_active = true;
    EXPECT_EQ(host.Tick(ringing, T0), PartyAction::Wait);
    for (auto at = T0; at < T0 + std::chrono::minutes{3}; at += std::chrono::seconds{1}) {
        // The game's searches, every SummonSearchInterval.
        ringing.last_search_post = T0 + (at - T0) / PartyController::SummonSearchInterval *
                                            PartyController::SummonSearchInterval;
        ASSERT_EQ(host.Tick(ringing, at), PartyAction::Wait)
            << std::chrono::duration_cast<std::chrono::seconds>(at - T0).count() << " s";
    }
    // The game stops searching: stale StaleSearchAfter after its last search.
    const auto last = T0 + std::chrono::minutes{3};
    ringing.last_search_post = last;
    EXPECT_EQ(
        host.Tick(ringing, last + PartyController::StaleSearchAfter - std::chrono::seconds{1}),
        PartyAction::Wait);
    EXPECT_EQ(host.Tick(ringing, last + PartyController::StaleSearchAfter), PartyAction::EndSearch);
}

// Ending the bell and ringing are on different ticks: the game starts a search when the effect
// goes on, so it has to be seen off first. An effect that stays on is ended again after
// RetryDelay; a guest arriving meanwhile (no room) cancels the ring.
TEST(BloodborneThreePlayers, AStaleBellIsEndedBeforeItRingsAgain) {
    PartyController host(PartyRole::Host);
    PartyObservation ringing = HostingWithOneGuest();
    ringing.search_effect_active = true;
    ASSERT_EQ(host.Tick(ringing, T0), PartyAction::Wait);
    const auto stale = T0 + PartyController::StaleSearchAfter;
    ASSERT_EQ(host.Tick(ringing, stale), PartyAction::EndSearch);
    EXPECT_EQ(host.Tick(ringing, stale + PartyController::RetryDelay - std::chrono::seconds{1}),
              PartyAction::Wait);
    EXPECT_EQ(host.Tick(ringing, stale + PartyController::RetryDelay), PartyAction::EndSearch)
        << "the effect did not go: end it again";

    PartyController joined(PartyRole::Host);
    ASSERT_EQ(joined.Tick(ringing, T0), PartyAction::Wait);
    ASSERT_EQ(joined.Tick(ringing, stale), PartyAction::EndSearch);
    PartyObservation full = ringing;
    full.room_for_more = false;
    full.search_effect_active = false;
    EXPECT_EQ(TickUntilDecision(joined, full), PartyAction::Wait) << "nobody left to ring for";
}

// Kaniel 2026-10-09: the re-ring fired right after a boss kill, a player started joining during
// the boss-clear sequence, the game kicked it and ended the session (no X, menus stuck). No bell
// rings while a boss fight or its clear sequence runs (event flags 4000 / 4047).
TEST(BloodborneThreePlayers, NoBellRingsDuringABossSequence) {
    PartyController host(PartyRole::Host);
    PartyObservation boss = HostingWithOneGuest();
    boss.boss_sequence = true;
    EXPECT_EQ(TickUntilDecision(host, boss), PartyAction::Wait);
    boss.search_effect_active = true;
    EXPECT_EQ(host.Tick(boss, T0 + PartyController::RetryDelay * 4), PartyAction::Wait)
        << "not the stale-bell ring either";
    PartyController alone(PartyRole::Host);
    PartyObservation solo{
        .player_valid = true, .map_usable = true, .matching_idle = true, .boss_sequence = true};
    EXPECT_EQ(TickUntilDecision(alone, solo), PartyAction::Wait);
}

// Kaniel 2026-10-09: after the game's kick-all (SessionManager +0x118 set) every later joiner was
// kicked on arrival (reason 0xff000019) until the room was made again: a crashed friend could
// never rejoin. A party host's session reopens.
TEST(BloodborneThreePlayers, APartyHostsSessionNeverStaysClosedToJoiners) {
    EXPECT_TRUE(ReopensClosedSession(true, MatchingStateHosting, 1));
    EXPECT_FALSE(ReopensClosedSession(true, MatchingStateHosting, 0)) << "not closed";
    EXPECT_FALSE(ReopensClosedSession(false, MatchingStateHosting, 1)) << "not a party";
    EXPECT_FALSE(ReopensClosedSession(true, 0, 1)) << "not hosting";
}

// Session members are game objects; the boards know members by online id. The member's identity
// object holds its online id, NUL-terminated.
TEST(BloodborneThreePlayers, ASessionMemberIsFoundByItsOnlineId) {
    const std::vector<std::string> candidates{"Ada", "Kaniel", "Kan"};
    std::vector<u8> identity(64, 0);
    const auto name = Bytes("Kaniel");
    std::copy(name.begin(), name.end(), identity.begin() + 0x10);
    EXPECT_EQ(FindOnlineIdIn(identity, candidates), std::optional<std::string>{"Kaniel"})
        << "not its prefix Kan";
    const std::vector<u8> none(64, 0x41);
    EXPECT_EQ(FindOnlineIdIn(none, candidates), std::nullopt);
    std::vector<u8> both(96, 0);
    std::copy(name.begin(), name.end(), both.begin() + 0x10);
    const auto other = Bytes("Ada");
    std::copy(other.begin(), other.end(), both.begin() + 0x40);
    EXPECT_EQ(FindOnlineIdIn(both, candidates), std::nullopt) << "ambiguous";
    std::vector<u8> glued(64, 0);
    const auto longer = Bytes("xKaniel");
    std::copy(longer.begin(), longer.end(), glued.begin() + 0x10);
    EXPECT_EQ(FindOnlineIdIn(glued, candidates), std::nullopt) << "part of a longer name";
    EXPECT_EQ(OnlineIdOfBoardMember(BoardMemberOfOnlineId("Kaniel")), "Kaniel");
    EXPECT_EQ(BoardMemberOfOnlineId("Kaniel"), "4b616e69656c");
    EXPECT_EQ(OnlineIdOfBoardMember("zz"), "");
}

// After a trip only the members on this map are re-created; creating one into a map it is not
// on makes its game leave the session. The others follow when they arrive here.
TEST(BloodborneThreePlayers, OnlyTheMembersOnThisMapAreReCreated) {
    const std::vector<MemberMap> board{
        {"me", 0x18010000}, {"guestA", 0x18010000}, {"guestB", 0x15000000}};
    const std::vector<std::optional<std::string>> members{"guestA", "guestB"};
    std::set<std::string> handled;
    auto plan = PlanRecreate(members, board, "me", 0x18010000, handled);
    EXPECT_EQ(plan.now, (std::vector<size_t>{0}));
    EXPECT_FALSE(plan.done) << "guestB still to come";

    handled.insert("guestA");
    const std::vector<MemberMap> later{
        {"me", 0x18010000}, {"guestA", 0x18010000}, {"guestB", 0x18010000}};
    plan = PlanRecreate(members, later, "me", 0x18010000, handled);
    // Kaniel 2026-10-11: Ada came back from the Dream before the host and re-created
    // Brom, who had stayed; once the host arrived she no longer saw Brom. When the
    // host's copy is made the one made before it is lost, so a later arrival re-creates everyone
    // on this map again (as when all arrive together, which worked).
    EXPECT_EQ(plan.now, (std::vector<size_t>{0, 1}));
    EXPECT_TRUE(plan.done);

    // A member that cannot be told apart: back to waiting for everyone on this map.
    const std::vector<std::optional<std::string>> unknown{"guestA", std::nullopt};
    plan = PlanRecreate(unknown, board, "me", 0x18010000, {});
    EXPECT_TRUE(plan.now.empty());
    EXPECT_FALSE(plan.done);
    plan = PlanRecreate(unknown, later, "me", 0x18010000, {});
    EXPECT_EQ(plan.now, (std::vector<size_t>{0, 1}));
    EXPECT_TRUE(plan.done);
}

// A rebuilt join task takes its member's slot from before the trip, by identity. Only when one
// task of that kind was saved may it go to a member whose identity changed.
TEST(BloodborneThreePlayers, EachRebuiltTaskGetsItsOwnSlot) {
    const std::vector<SavedJoinTask> saved{
        {RemoteGuestTask, 0xA0, 1, 1}, {RemoteGuestTask, 0xB0, 2, 1}, {RemoteHostTask, 0xC0, 0, 1}};
    EXPECT_EQ(PickSavedJoinTask(saved, RemoteGuestTask, 0xB0), std::optional<size_t>{1});
    EXPECT_EQ(PickSavedJoinTask(saved, RemoteGuestTask, 0xEE), std::nullopt)
        << "two guests saved: no guessing";
    EXPECT_EQ(PickSavedJoinTask(saved, RemoteHostTask, 0xEE), std::optional<size_t>{2})
        << "the one host there was";
}

// "The host is on my map" comes from the host's own map state post, not from "everyone else is
// here": with two guests, the other guest may be elsewhere.
TEST(BloodborneThreePlayers, TheHostIsHereWhateverTheOtherGuestDoes) {
    const std::vector<MemberMap> board{
        {"me", 0x18010000}, {"host", 0x18010000}, {"guestB", 0x15000000}};
    EXPECT_TRUE(HostIsHere(0x18010000, board, "me", 0x18010000));
    EXPECT_FALSE(HostIsHere(0x15000000, board, "me", 0x18010000));
    EXPECT_FALSE(HostIsHere(std::nullopt, board, "me", 0x18010000));
    const std::vector<MemberMap> gone{{"me", 0x18010000}};
    EXPECT_FALSE(HostIsHere(0x18010000, gone, "me", 0x18010000)) << "a stale post";
}

// The game shares a kill's echoes with the members on the killer's map; the board covers the
// rest. A kill's echoes go to the board whenever someone is elsewhere, with the kill's map, and
// only members the game did not pay add them (see KillEchoLedger).
TEST(BloodborneThreePlayers, KillEchoesReachOnlyTheMembersElsewhere) {
    EXPECT_TRUE(SharesKillEchoes(0x018FC99B, true)) << "someone elsewhere";
    EXPECT_FALSE(SharesKillEchoes(0x018FC99B, false)) << "everyone here: the game shares them";
    const std::vector<KillEchoes> kills{{120, 0x18010000}, {4000, 0}};
    EXPECT_EQ(EncodeEchoUpdate("guest", 3, kills), "guest 3 120:402718720 4000");
    EXPECT_EQ(ParseEchoBoard("5\n120 402718720\n4000\n"),
              (std::optional<EchoBoardAnswer>{EchoBoardAnswer{5, kills}}));
    EXPECT_EQ(ParseEchoBoard("5\n120 x\n"), std::nullopt);
}

// Rig smoke 2026-10-12 (run 151918): the host killed an enemy beside the first guest while the
// second was in the Dream; the guest posted the +995 at 342049 and the second guest read it at
// 345662, mid-trip back. Its grant ran once it stood on the kill's map again (345821), which the
// old rule (the kill's map is not mine now) took for "the game paid it": 0 echoes. A member is
// owed a posted kill unless the game paid it the same echoes on that map around then.
TEST(BloodborneThreePlayers, PostedEchoesAreOwedUnlessTheGamePaidThem) {
    constexpr u32 Yharnam = 0x18010000;
    constexpr u32 Dream = 0x15000000;
    using Kills = std::vector<KillEchoes>;
    KillEchoLedger guest2;
    guest2.Received({995, Yharnam}, 345'662);
    EXPECT_EQ(guest2.Due(345'700), Kills{}) << "a moment for the game's own reward";
    EXPECT_EQ(guest2.Due(345'900), Kills{}) << "back on the kill's map: still owed";
    EXPECT_EQ(guest2.Due(347'700), (Kills{{995, Yharnam}}));
    EXPECT_EQ(guest2.Due(347'800), Kills{}) << "once";

    KillEchoLedger host; // beside the first guest: the game paid it
    host.Paid({995, Yharnam}, 341'900);
    host.Received({995, Yharnam}, 344'087);
    EXPECT_EQ(host.Due(344'100), Kills{});
    EXPECT_EQ(host.Due(350'000), Kills{}) << "paid, not owed later";

    KillEchoLedger two_kills; // one reward pays one post
    two_kills.Paid({995, Yharnam}, 400'000);
    two_kills.Received({995, Yharnam}, 402'000);
    two_kills.Received({995, Yharnam}, 402'000);
    EXPECT_EQ(two_kills.Due(405'000), (Kills{{995, Yharnam}}));

    KillEchoLedger elsewhere; // an old reward, or one on another map, pays nothing
    elsewhere.Paid({995, Yharnam}, 0);
    elsewhere.Paid({995, Dream}, 59'000);
    elsewhere.Received({995, Yharnam}, 60'000);
    EXPECT_EQ(elsewhere.Due(63'000), (Kills{{995, Yharnam}}));

    KillEchoLedger old_post; // a post without a map: any map's reward pays it
    old_post.Paid({995, Yharnam}, 10'000);
    old_post.Received({995, 0}, 12'000);
    EXPECT_EQ(old_post.Due(15'000), Kills{});
}

// Rig smoke 2026-10-12 (run 143240): the host killed an enemy beside the first guest while the
// second was in the Dream. The game gave both of them the kill's echoes, both posted them, and
// the second guest got +995 twice. Of the members on the killer's map only the first by board
// name posts; with nobody elsewhere nobody does.
TEST(BloodborneThreePlayers, OneMemberOnTheKillersMapPostsItsEchoes) {
    constexpr u32 Yharnam = 0x18010000;
    constexpr u32 Dream = 0x15000000;
    const std::vector<MemberMap> board{
        {"rig_host", Yharnam}, {"rig_guest", Yharnam}, {"rig_guest2", Dream}};
    EXPECT_TRUE(PostsKillEchoes(board, "rig_guest", Yharnam));
    EXPECT_FALSE(PostsKillEchoes(board, "rig_host", Yharnam)) << "the guest beside it posts";
    EXPECT_TRUE(PostsKillEchoes(board, "rig_guest2", Dream)) << "alone on its map";
    const std::vector<MemberMap> together{{"rig_host", Yharnam}, {"rig_guest", Yharnam}};
    EXPECT_FALSE(PostsKillEchoes(together, "rig_guest", Yharnam)) << "the game shares them";
    const std::vector<MemberMap> host_alone{
        {"rig_host", Yharnam}, {"rig_guest", Dream}, {"rig_guest2", Dream}};
    EXPECT_TRUE(PostsKillEchoes(host_alone, "rig_host", Yharnam));
}

// The kill board names its posters: one member's visit ending takes back only its own kill.
TEST(BloodborneThreePlayers, AKillStandsUntilEveryKillerTakesItBack) {
    EXPECT_EQ(ParseKillBoard("4\n268517532 1 guestA\n268517532 1 guestB\n7 0\n"),
              (std::optional<KillBoardAnswer>{KillBoardAnswer{
                  4, {{268517532, true, "guestA"}, {268517532, true, "guestB"}, {7, false, ""}}}}));
    EXPECT_EQ(ParseKillBoard("x\n"), std::nullopt);
    PartyKills kills;
    kills.Apply(std::vector<KillChange>{{268517532, true, "guestA"}, {268517532, true, "guestB"}});
    kills.Apply(std::vector<KillChange>{{268517532, false, "guestA"}});
    EXPECT_TRUE(kills.Contains(268517532)) << "guestB's kill stands";
    kills.Apply(std::vector<KillChange>{{268517532, false, "guestB"}});
    EXPECT_FALSE(kills.Contains(268517532));
}

// Kaniel's playtest 2026-10-21 (v1.1.2): krolebord and the host went to Yahar'gul while Lisara
// was in Hemwick. Her link went quiet and she fell off the map board, so the only name left to
// look for was the host's, and her session member came out as the host's name too. Both looked
// handled, the re-create ended, and when she arrived nobody made her character: she saw them,
// they never saw her and her hits did nothing.
TEST(BloodborneThreePlayers, AMemberOffTheBoardKeepsTheReCreateWaiting) {
    const std::vector<MemberMap> quiet{{"me", 0x1c000000}, {"host", 0x1c000000}};
    const std::vector<std::optional<std::string>> both_host{"host", "host"};
    auto plan = PlanRecreate(both_host, quiet, "me", 0x1c000000, {"host"});
    EXPECT_TRUE(plan.now.empty());
    EXPECT_FALSE(plan.done) << "two members with one name: one of them is not known";

    // A member not told apart: "everyone else is here" counts the session, not the board.
    const std::vector<std::optional<std::string>> unknown{"host", std::nullopt};
    plan = PlanRecreate(unknown, quiet, "me", 0x1c000000, {"host"});
    EXPECT_TRUE(plan.now.empty()) << "creating her into a map she is not on drops her";
    EXPECT_FALSE(plan.done);

    const std::vector<MemberMap> arrived{
        {"me", 0x1c000000}, {"host", 0x1c000000}, {"lisara", 0x1c000000}};
    const std::vector<std::optional<std::string>> members{"host", "lisara"};
    plan = PlanRecreate(members, arrived, "me", 0x1c000000, {"host"});
    EXPECT_EQ(plan.now, (std::vector<size_t>{0, 1}));
    EXPECT_TRUE(plan.done);
}

// The names to look for in session members: every member seen on the board in this session,
// so one that drops off it for a moment is still found as itself.
TEST(BloodborneThreePlayers, BoardNamesSeenStayCandidates) {
    BoardMembersSeen seen;
    seen.Take(std::vector<MemberMap>{{"me", 1}, {"host", 1}, {"lisara", 2}}, "me");
    seen.Take(std::vector<MemberMap>{{"me", 1}, {"host", 1}}, "me");
    EXPECT_EQ(seen.Names(), (std::vector<std::string>{"host", "lisara"}));
    seen.Clear();
    EXPECT_TRUE(seen.Names().empty());
}

// More of the same rules at their edges.
TEST(BloodborneThreePlayers, ReCreationWaitsForMembersNotOnTheBoard) {
    const std::vector<MemberMap> board{{"me", 0x18010000}, {"guestA", 0x18010000}};
    // guestB is in the session but posts no map (loading, or its game is gone).
    const std::vector<std::optional<std::string>> members{"guestA", "guestB"};
    const auto plan = PlanRecreate(members, board, "me", 0x18010000, {"guestA"});
    EXPECT_TRUE(plan.now.empty()) << "guestA is done already";
    EXPECT_FALSE(plan.done);
    const auto alone = PlanRecreate({}, board, "me", 0x18010000, {});
    EXPECT_TRUE(alone.now.empty());
    EXPECT_TRUE(alone.done) << "nobody else in the session";
}

TEST(BloodborneThreePlayers, OnlineIdsAtTheEdgesOfTheObject) {
    const std::vector<std::string> candidates{"Kaniel"};
    std::vector<u8> at_start(16, 0);
    const std::string name = "Kaniel";
    std::copy(name.begin(), name.end(), at_start.begin());
    EXPECT_EQ(FindOnlineIdIn(at_start, candidates), std::optional<std::string>{"Kaniel"});
    std::vector<u8> cut(name.begin(), name.end()); // no NUL after it: not known to end there
    EXPECT_EQ(FindOnlineIdIn(cut, candidates), std::nullopt);
    EXPECT_EQ(FindOnlineIdIn(at_start, std::vector<std::string>{}), std::nullopt);
    std::vector<u8> twice(48, 0);
    std::copy(name.begin(), name.end(), twice.begin() + 2);
    std::copy(name.begin(), name.end(), twice.begin() + 24);
    EXPECT_EQ(FindOnlineIdIn(twice, candidates), std::optional<std::string>{"Kaniel"})
        << "the same member twice";
    EXPECT_EQ(OnlineIdOfBoardMember("4b6"), "") << "odd length";
    EXPECT_EQ(OnlineIdOfBoardMember("4B"), "") << "the boards use lowercase";
    EXPECT_EQ(OnlineIdOfBoardMember("20"), "") << "a space is not an online id";
    EXPECT_EQ(OnlineIdOfBoardMember("a1b2c3d4e5f60718"), "") << "an old random member";
    EXPECT_EQ(OnlineIdOfBoardMember(BoardMemberOfOnlineId("my_name-1")), "my_name-1");
}

TEST(BloodborneThreePlayers, SavedTasksOfAnotherKindAreNeverTaken) {
    EXPECT_EQ(PickSavedJoinTask(std::vector<SavedJoinTask>{}, RemoteGuestTask, 0xA0), std::nullopt);
    const std::vector<SavedJoinTask> host_only{{RemoteHostTask, 0xA0, 0, 1}};
    EXPECT_EQ(PickSavedJoinTask(host_only, RemoteGuestTask, 0xA0), std::nullopt)
        << "same identity, other kind";
}

TEST(BloodborneThreePlayers, KillBoardEdges) {
    EXPECT_EQ(ParseKillBoard("5\n7 2\n"), std::nullopt);
    EXPECT_EQ(ParseKillBoard("5\n7 1x\n"), std::nullopt);
    EXPECT_EQ(ParseKillBoard("\n7 1\n"), std::nullopt);
    PartyKills kills;
    kills.Apply(std::vector<KillChange>{{9, false, "nobody"}}); // nothing to take back
    kills.Apply(std::vector<KillChange>{{7, true, "guestA"}, {8, true, "guestB"}});
    EXPECT_EQ(kills.All(), (std::unordered_set<u32>{7, 8}));
    EXPECT_FALSE(kills.Contains(9));
}

TEST(BloodborneThreePlayers, EchoAndHpBoardEdges) {
    EXPECT_EQ(ParseEchoBoard("1\n120 0\n"),
              (std::optional<EchoBoardAnswer>{EchoBoardAnswer{1, {{120, 0}}}}));
    EXPECT_EQ(SharedEnemyHp(500, 2031, 3000, 2031), std::nullopt) << "a report above max";
    EXPECT_EQ(SharedEnemyHp(2'000'000'000, 2'000'000'000, 1, 2'000'000'000), 1) << "no overflow";
    const std::vector<EnemyHp> dead{{0x10014085, -1, 300}};
    EXPECT_EQ(DecodeEnemyHp(EncodeEnemyHp(1, dead)), (EnemyHpReport{1, dead}));
}

// The decisions the game code makes from the boards.
TEST(BloodborneThreePlayers, WhoRunsTheEnemiesAndWhoWaits) {
    const std::vector<MemberMap> two_guests{
        {"me", 0x18010000}, {"guestB", 0x18010000}, {"host", 0x15000000}};
    EXPECT_TRUE(HostAway(0x15000000, two_guests, "me", 0x18010000))
        << "two guests without the host: both run the enemies";
    const std::vector<MemberMap> with_host{
        {"me", 0x18010000}, {"host", 0x18010000}, {"guestB", 0x15000000}};
    EXPECT_FALSE(HostAway(0x18010000, with_host, "me", 0x18010000))
        << "the other guest being elsewhere does not matter";
    // No host post yet: the two-player rules.
    EXPECT_FALSE(HostAway(std::nullopt, with_host, "me", 0x18010000)) << "someone is here";
    const std::vector<MemberMap> alone{{"me", 0x18010000}, {"host", 0x15000000}};
    EXPECT_TRUE(HostAway(std::nullopt, alone, "me", 0x18010000));

    EXPECT_TRUE(SomeoneElsewhere(with_host, "me", 0x18010000));
    const std::vector<MemberMap> together{{"me", 0x18010000}, {"host", 0x18010000}};
    EXPECT_FALSE(SomeoneElsewhere(together, "me", 0x18010000));
    EXPECT_FALSE(SomeoneElsewhere(std::vector<MemberMap>{{"me", 0x18010000}}, "me", 0x18010000));
}

TEST(BloodborneThreePlayers, TheHostRingsOnlyWhileTheSessionHasRoom) {
    EXPECT_TRUE(RoomForMore(true, 2, 3)) << "one guest in, one outside";
    EXPECT_TRUE(RoomForMore(true, 1, 2)) << "a guest dropped out";
    EXPECT_FALSE(RoomForMore(true, 2, 2)) << "everyone is in";
    EXPECT_FALSE(RoomForMore(true, 3, 4)) << "three is the game's limit";
    EXPECT_FALSE(RoomForMore(false, 1, 3)) << "not hosting: the idle search does it";
}

TEST(BloodborneThreePlayers, EachCopyTakesTheLowestHpOnItsMap) {
    const u32 here = 0x18010000;
    const std::vector<MemberMapState> others{
        {here, EncodeEnemyHp(1, std::vector<EnemyHp>{{0x1001409c, 1500, 2031}})},
        {here, EncodeEnemyHp(2, std::vector<EnemyHp>{{0x1001409c, 900, 2031}})},
        {0x15000000, EncodeEnemyHp(3, std::vector<EnemyHp>{{0x1001409c, 10, 2031}})},
        {here, std::vector<u8>{1, 2, 3}}, // a cut post
    };
    EXPECT_EQ(LowestSharedHp(0x1001409c, 2031, 2031, here, others), 900)
        << "the lowest on this map, not the copy on another map";
    EXPECT_EQ(LowestSharedHp(0x1001409c, 800, 2031, here, others), std::nullopt) << "lower here";
    EXPECT_EQ(LowestSharedHp(0x10014085, 300, 300, here, others), std::nullopt) << "not hurt";
}

TEST(BloodborneThreePlayers, AGuestRebuildsTheOtherGuestAsAGuest) {
    // Kaniel 2026-10-09: after a trip the two guests did not see each other. A guest rebuilt
    // every other member as the remote host (kind 0); the other guest is a remote guest (kind 1),
    // as the game made it at the join.
    const std::vector<SavedJoinTask> saved{
        {RemoteHostTask, 0xA0, 0, 0x4},
        {RemoteGuestTask, 0xB0, 1, 0xC},
        {LocalTaskKind(false), 0xC0, 2, 0xC},
    };
    const std::vector<SavedJoinTask> none;
    EXPECT_EQ(RebuiltTaskKind(false, saved, none, 0xA0), RemoteHostTask) << "the host";
    EXPECT_EQ(RebuiltTaskKind(false, saved, none, 0xB0), RemoteGuestTask) << "the other guest";
    EXPECT_EQ(RebuiltTaskKind(true, saved, none, 0xB0), RemoteGuestTask) << "the host's guests";
    // Not saved (it joined during the trip): a guest when the host is someone else.
    EXPECT_EQ(RebuiltTaskKind(false, saved, none, 0xD0), RemoteGuestTask);
    EXPECT_EQ(RebuiltTaskKind(false, none, none, 0xD0), RemoteHostTask)
        << "nothing known: the one other member of a two-player party is the host";
}

TEST(BloodborneThreePlayers, AGuestBuildsANewGuestAsAGuestFromItsLiveTasks) {
    // Rig smoke 2026-10-12 (40ea3277): the second guest joined, the first guest's game made no
    // task for it, and the re-create after the join built it, with no saved tasks yet, as the
    // remote host (kind 0). Step 4 (0x01E50090) only creates kinds 1 and 2, so the first guest
    // never saw it ("1 alive within 15 m"). The live host task says the host is someone else.
    const std::vector<SavedJoinTask> none;
    const std::vector<SavedJoinTask> live{
        {RemoteHostTask, 0xA0, 7, 0x4},
        {LocalTaskKind(false), 0xC0, 0, 0xC},
    };
    EXPECT_EQ(RebuiltTaskKind(false, none, live, 0xB0), RemoteGuestTask) << "the new guest";
    EXPECT_EQ(RebuiltTaskKind(false, none, live, 0xA0), RemoteHostTask) << "the host";
    EXPECT_EQ(RebuiltTaskKind(true, none, live, 0xB0), RemoteGuestTask) << "on the host";
    // A wrong saved kind (the bug above, saved at the next trip) loses to the live host task.
    const std::vector<SavedJoinTask> stale{{RemoteHostTask, 0xB0, 7, 0x4D}};
    EXPECT_EQ(RebuiltTaskKind(false, stale, live, 0xB0), RemoteGuestTask);
    // Nothing live (a trip's rebuild): the saved kinds, as before.
    EXPECT_EQ(RebuiltTaskKind(false, stale, none, 0xB0), RemoteHostTask);
    EXPECT_EQ(RebuiltTaskKind(false, none, none, 0xB0), RemoteHostTask);
}

TEST(BloodborneThreePlayers, AJoinsReCreateLeavesANewMemberToTheGamesJoin) {
    // The game's join (0x0178C1E3) makes no task for a member that has one; the first guest's
    // re-create after the second guest joined had built one first (rig smoke 2026-10-12).
    EXPECT_TRUE(RecreateIsForAJoin(false, false)) << "nothing pending: the join's";
    EXPECT_TRUE(RecreateIsForAJoin(true, true)) << "a join's still pending";
    EXPECT_FALSE(RecreateIsForAJoin(true, false)) << "a trip's still pending builds what is gone";
}

TEST(BloodborneThreePlayers, ARebuiltTaskDropsTheAllocatorsHighFlagBits) {
    // The constructor (0x01E4F9F0) keeps +0xED bits 6-7 (`and 0xc0`) of the allocation and no
    // join code sets or tests them: the 0x40 seen on rebuilt tasks (0x4c) is leftover memory.
    EXPECT_EQ(RebuiltTaskFlags(0x40, nullptr, false), 0x00);
    EXPECT_EQ(RebuiltTaskFlags(0x44, nullptr, false), 0x04);
    const SavedJoinTask synced{RemoteGuestTask, 0xB0, 7, 0x4C, 6};
    EXPECT_EQ(RebuiltTaskFlags(0xC0, &synced, false), 0x0D) << "synced, without bit 6";
    const SavedJoinTask joining{RemoteGuestTask, 0xB0, 7, 0x44, 3};
    EXPECT_EQ(RebuiltTaskFlags(0x00, &joining, true), 0x04) << "still waiting for its data";
}

TEST(BloodborneThreePlayers, AMemberAwayDuringATripKeepsItsSavedSlot) {
    // Rig trip3 2026-10-10: the second guest went to the Dream, where only the host came; its
    // trip back saved the host's task alone and dropped the first guest's slot, so back on the
    // first guest's map that guest was rebuilt without a slot and never appeared.
    const std::vector<SavedJoinTask> before{
        {RemoteHostTask, 0xA0, 0, 0x4},
        {RemoteGuestTask, 0xB0, 1, 0xC},
        {LocalTaskKind(false), 0xC0, 2, 0xC},
    };
    const std::vector<SavedJoinTask> in_the_dream{
        {RemoteHostTask, 0xA0, 0, 0x5},
        {LocalTaskKind(false), 0xC0, 2, 0xD},
    };
    const auto merged = MergeSavedJoinTasks(before, in_the_dream);
    ASSERT_EQ(merged.size(), 3u);
    const auto guest = PickSavedJoinTask(merged, RemoteGuestTask, 0xB0);
    ASSERT_TRUE(guest.has_value()) << "the first guest's slot from before";
    EXPECT_EQ(merged[*guest].slot, 1);
    const auto host = PickSavedJoinTask(merged, RemoteHostTask, 0xA0);
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(merged[*host].flags, 0x5) << "a member there now: its newest state";
    EXPECT_EQ(MergeSavedJoinTasks(before, {}), before)
        << "a trip from a map with no remote players keeps what was saved";
}

TEST(BloodborneThreePlayers, AGuestStillJoiningAtATripIsRebuiltAsAFreshJoin) {
    // Rig hp-after-host-trip with a ping, 2026-10-10 (Kaniel's screenshot): the host left for the
    // Dream while its join task for the guest was at step 3, its data not synced (flags 0x4). Back
    // on the map the host rebuilt it from that save straight into the character step with the
    // data marked synced: a guest with no data behind it, a "Beckoner" name plate and no body.
    const SavedJoinTask joined{RemoteGuestTask, 0xB0, 1, 0xC, InSessionStep};
    const SavedJoinTask joining{RemoteGuestTask, 0xB0, 0, 0x4, 3};
    EXPECT_TRUE(RebuildsFromSave(joined)) << "its data is there: straight to its character";
    EXPECT_FALSE(RebuildsFromSave(joining)) << "the game's own join, from the start";
    EXPECT_TRUE(RebuildsFromSave(SavedJoinTask{RemoteGuestTask, 0xB0, 1, 0x5, 5}))
        << "arriving: its character is made";
}

TEST(BloodborneThreePlayers, AnotherBlockOfTheSameAreaSharesItsEnemyHp) {
    // Friend run 2026-10-09: the guest joined in place in Central Yharnam (0x18010000) while the
    // host stood in Cathedral Ward (0x18000000), and no HP was shared for the rest of the game.
    // The blocks of an area (m24_00, m24_01) give their enemies distinct handles.
    const u32 here = 0x18010000;
    const std::vector<MemberMapState> others{
        {0x18000000, EncodeEnemyHp(1, std::vector<EnemyHp>{{0x1001400b, 100, 484}})},
        {0x15000000, EncodeEnemyHp(2, std::vector<EnemyHp>{{0x1001400b, 10, 484}})},
    };
    EXPECT_EQ(LowestSharedHp(0x1001400b, 484, 484, here, others), 100)
        << "the Ward's report, not the other area's";
}

TEST(BloodborneThreePlayers, OnlyAWorldReloadInAPartySessionStartsATrip) {
    EXPECT_TRUE(StartsTripOnWorldReload(0x0193C65C, false, true));
    EXPECT_FALSE(StartsTripOnWorldReload(0x0193C65C, true, true)) << "a trip already";
    EXPECT_FALSE(StartsTripOnWorldReload(0x0193C65C, false, false)) << "no party session";
    EXPECT_FALSE(StartsTripOnWorldReload(0x013CDEFB, false, true)) << "a lamp trip's own call";
}
