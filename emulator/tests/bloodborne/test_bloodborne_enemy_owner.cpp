// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Phase 4 (docs/SERVER_COOP_PLAN.md): the server names one game per occupied area that runs its
// enemies; the others follow it. What each game does with that: its role, the state the game's
// host-only checks see, the ownership words and what happens to the enemy packets.

#include <map>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

namespace {

constexpr u32 CentralYharnam = 0x18010000;
constexpr u32 CathedralWard = 0x18000000;
constexpr u32 ForbiddenWoods = 0x15000000;
constexpr s32 Hosting = 3;
constexpr s32 Guest = 6;

const std::vector<EnemyOwnerEntry> Owners{{CentralYharnam, "aa01", 4}, {ForbiddenWoods, "bb02", 7}};

} // namespace

TEST(EnemyOwner, AnOwnerRunsOneMap) {
    // Not an area: the game wakes a block's enemies only near a player in that world.
    EXPECT_EQ(EnemyOwnerArea(CentralYharnam), CentralYharnam);
    EXPECT_EQ(EnemyOwnerArea(CathedralWard), CathedralWard);
    EXPECT_EQ(EnemyOwnerArea(0), 0u);
}

TEST(EnemyOwner, TheServersOwnerOfTheAreaDecidesTheRole) {
    EXPECT_EQ(EnemyRoleOn(Owners, "aa01", CentralYharnam), EnemyRole::Owner);
    EXPECT_EQ(EnemyRoleOn(Owners, "bb02", CentralYharnam), EnemyRole::Follower);
    EXPECT_EQ(EnemyRoleOn(Owners, "aa01", ForbiddenWoods), EnemyRole::Follower);
    // The other block of the area is another map: nobody runs it here.
    EXPECT_EQ(EnemyRoleOn(Owners, "aa01", CathedralWard), EnemyRole::Unknown);
    // Not in the world, an area nobody runs, no name yet, no owners heard: the game's own way.
    EXPECT_EQ(EnemyRoleOn(Owners, "aa01", 0), EnemyRole::Unknown);
    EXPECT_EQ(EnemyRoleOn(Owners, "aa01", 0x21000000), EnemyRole::Unknown);
    EXPECT_EQ(EnemyRoleOn(Owners, "", CentralYharnam), EnemyRole::Unknown);
    EXPECT_EQ(EnemyRoleOn({}, "aa01", CentralYharnam), EnemyRole::Unknown);
    const EnemyOwnerEntry* owner = EnemyOwnerOf(Owners, CentralYharnam);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->member, "aa01");
    EXPECT_EQ(owner->epoch, 4u);
    EXPECT_EQ(EnemyOwnerOf(Owners, 0x21000000), nullptr);
}

TEST(EnemyOwner, TheOwnerSendsTheEnemiesHpAsAHostAFollowerTakesItAsAGuest) {
    // The host-only check of the HP sends (+0x124 == 3) and the guest-only one of the receive.
    EXPECT_EQ(EnemySyncState(EnemyRole::Owner, Guest), std::optional<s32>{Hosting});
    EXPECT_EQ(EnemySyncState(EnemyRole::Follower, Hosting), std::optional<s32>{Guest});
    // Already so, or no role: unchanged.
    EXPECT_EQ(EnemySyncState(EnemyRole::Owner, Hosting), std::nullopt);
    EXPECT_EQ(EnemySyncState(EnemyRole::Follower, Guest), std::nullopt);
    EXPECT_EQ(EnemySyncState(EnemyRole::Unknown, Guest), std::nullopt);
    EXPECT_EQ(EnemySyncState(EnemyRole::Unknown, Hosting), std::nullopt);
    // Out of a session (state 0) nothing changes.
    EXPECT_EQ(EnemySyncState(EnemyRole::Owner, 0), std::nullopt);
    EXPECT_EQ(EnemySyncState(EnemyRole::Follower, 0), std::nullopt);
}

TEST(EnemyOwner, AHostThatFollowsSendsItsHitsAsAGuestDoes) {
    // The hit on an enemy (0x01A29E40) sends it (ch 0x15) when the state reads 6.
    EXPECT_EQ(EnemyHitState(EnemyRole::Follower, Hosting), std::optional<s32>{Guest});
    EXPECT_EQ(EnemyHitState(EnemyRole::Owner, Hosting), std::nullopt);
    EXPECT_EQ(EnemyHitState(EnemyRole::Owner, Guest), std::nullopt); // its 0x15 is dropped
    EXPECT_EQ(EnemyHitState(EnemyRole::Follower, Guest), std::nullopt);
    EXPECT_EQ(EnemyHitState(EnemyRole::Unknown, Hosting), std::nullopt);
}

TEST(EnemyOwner, TheOwnerRunsTheAiAFollowerTheNetworkController) {
    EXPECT_EQ(EnemyOwnershipWord(EnemyRole::Owner), std::optional<u16>{0});
    EXPECT_EQ(EnemyOwnershipWord(EnemyRole::Follower), std::optional<u16>{0xFFFF});
    EXPECT_EQ(EnemyOwnershipWord(EnemyRole::Unknown), std::nullopt);
}

// Playtest 2026-10-18: krolebord was levelling up at the Doll when the Dream's owner changed; the
// Doll went to the new owner's game in the middle of the talk (and to where she stood there, 16 m
// away) and he stayed kneeling for good. The Doll runs in each player's own game.
TEST(EnemyOwner, TheDollNeverChangesHands) {
    EXPECT_FALSE(ChangesHandsWithTheMap(HuntersDreamDoll));
    EXPECT_FALSE(ChangesHandsWithTheMap(2100700));
    // Gehrman and the Dream's other characters (team 26 too), its messengers, an enemy without an
    // entity id: as before.
    for (const s32 other : {2100600, 2100200, 2100211, 2100950, 2100215, -1, 0}) {
        EXPECT_TRUE(ChangesHandsWithTheMap(other)) << other;
    }
}

// The game itself moves her too: the host's ownership step hands the network every character its
// player is not near, a guest's hand-out (0x17) sets the rest of its table to 0xFFFF. Right after
// that step her word goes back to this game's own, whatever this member's role on the map.
TEST(EnemyOwner, TheDollStaysWithEachGameAfterTheOwnershipStep) {
    for (const EnemyRole role : {EnemyRole::Owner, EnemyRole::Follower}) {
        EXPECT_EQ(KeptLocalWord(role, HuntersDreamDoll, 0xFFFF), std::optional<u16>{1});
        EXPECT_EQ(KeptLocalWord(role, HuntersDreamDoll, 0x8000), std::optional<u16>{1});
        EXPECT_EQ(KeptLocalWord(role, HuntersDreamDoll, 1), std::nullopt);
        EXPECT_EQ(KeptLocalWord(role, HuntersDreamDoll, 0), std::nullopt);
        EXPECT_EQ(KeptLocalWord(role, 2100600, 0xFFFF), std::nullopt);
    }
    // No owner on the map: the game's own way.
    EXPECT_EQ(KeptLocalWord(EnemyRole::Unknown, HuntersDreamDoll, 0xFFFF), std::nullopt);
}

// What the game sends at all; which member gets it is shadNet's since phase 9
// (server/tests/test_packet_routes.cpp, which also keeps a follower from taking another
// follower's enemies: krolebord 2026-10-15).
TEST(EnemyOwner, OnlyTheOwnerSendsTheEnemiesAndAFollowersHitGoesToIt) {
    // HP, catch-up and join animations: the owner's alone.
    for (const u32 channel : {0x05u, 0x29u, 0x36u}) {
        EXPECT_EQ(EnemyPacketOut(channel, EnemyRole::Owner), EnemyPacketFate::Keep) << channel;
        EXPECT_EQ(EnemyPacketOut(channel, EnemyRole::Follower), EnemyPacketFate::Drop) << channel;
        EXPECT_EQ(EnemyPacketOut(channel, EnemyRole::Unknown), EnemyPacketFate::Keep) << channel;
    }
    // A hit (0x15): the owner applied its own already, a follower's goes to the owner.
    EXPECT_EQ(EnemyPacketOut(0x15, EnemyRole::Owner), EnemyPacketFate::Drop);
    EXPECT_EQ(EnemyPacketOut(0x15, EnemyRole::Follower), EnemyPacketFate::ToOwner);
    EXPECT_EQ(EnemyPacketOut(0x15, EnemyRole::Unknown), EnemyPacketFate::Keep);
    // A follower's near list (0x16) goes to the owner (rig apart-hurt 2026-10-15). Kaniel
    // 2026-10-15 (enemies by a far guest lagged, run by the host): the game's own hand-out (the
    // owner's answer 0x17) goes out again, so a guest runs the enemies only it is near, and sends
    // their positions (ch 4).
    EXPECT_EQ(EnemyPacketOut(0x16, EnemyRole::Follower), EnemyPacketFate::ToOwner);
    EXPECT_EQ(EnemyPacketOut(0x16, EnemyRole::Owner), EnemyPacketFate::Drop);
    EXPECT_EQ(EnemyPacketOut(0x16, EnemyRole::Unknown), EnemyPacketFate::Drop);
    EXPECT_EQ(EnemyPacketOut(0x17, EnemyRole::Owner), EnemyPacketFate::Keep);
    EXPECT_EQ(EnemyPacketOut(0x17, EnemyRole::Follower), EnemyPacketFate::Drop);
    EXPECT_EQ(EnemyPacketOut(0x17, EnemyRole::Unknown), EnemyPacketFate::Drop);
    for (const EnemyRole role : {EnemyRole::Owner, EnemyRole::Follower, EnemyRole::Unknown}) {
        EXPECT_EQ(EnemyPacketOut(0x04, role), EnemyPacketFate::Keep);
    }
    for (const u32 channel : {0x3Du, 0x41u, 0x34u, 0x06u, 0x14u, 0x24u, 0x26u}) {
        EXPECT_EQ(EnemyPacketOut(channel, EnemyRole::Follower), EnemyPacketFate::Keep) << channel;
        EXPECT_EQ(EnemyPacketOut(channel, EnemyRole::Owner), EnemyPacketFate::Keep) << channel;
    }
}

TEST(EnemyOwner, ARoleChangeIsLoggedOnce) {
    EnemyRoleLog log;
    EXPECT_EQ(log.Change(EnemyRole::Unknown, CentralYharnam, nullptr), std::nullopt);
    const EnemyOwnerEntry mine{CentralYharnam, "aa01", 4};
    const auto first = log.Change(EnemyRole::Owner, CentralYharnam, &mine);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, "enemy owner: this game runs map 0x18010000's enemies (epoch 4)");
    EXPECT_EQ(log.Change(EnemyRole::Owner, CentralYharnam, &mine), std::nullopt);
    const EnemyOwnerEntry other{CentralYharnam, "bb02", 5};
    EXPECT_EQ(log.Change(EnemyRole::Follower, CentralYharnam, &other),
              std::optional<std::string>{
                  "enemy owner: this game follows bb02 on map 0x18010000 (epoch 5)"});
    EXPECT_EQ(log.Change(EnemyRole::Unknown, 0, nullptr),
              std::optional<std::string>{"enemy owner: none (the game's own way)"});
}

// Phase 8: the owner's game wakes the groups around every other player as around its own (the
// sets from the rig run 2026-10-15: the host by the lamp, the guest by Iosefka's clinic).
WakeGroups Groups(u32 wakes, u32 counted, u32 half) {
    WakeGroups groups;
    groups.wakes[0] = wakes;
    groups.counted[0] = counted;
    groups.half[0] = half;
    return groups;
}

TEST(EnemyOwner, TheOwnerWakesTheGroupsAroundTheOtherPlayers) {
    const WakeGroups record = Groups(0x60000, 0x20000, 0x60000);
    const WakeGroups own = Groups(0x60400, 0x20000, 0x60400);
    const std::vector<WakeGroups> far{Groups(0x2810000, 0x2000000, 0x2810000)};
    EXPECT_EQ(WithOthersGroups(record, own, far), Groups(0x2870000, 0x2020000, 0x2870000));
    // Nobody else: the game's own.
    EXPECT_EQ(WithOthersGroups(record, own, {}), record);
    // Beside the owner: nothing new.
    const std::vector<WakeGroups> near{Groups(0x60400, 0x20000, 0x60400)};
    EXPECT_EQ(WithOthersGroups(record, own, near), Groups(0x60400, 0x20000, 0x60400));
}

// Kaniel's test 2026-10-23: back alone in Central Yharnam, a guest saw the clinic's dog its story
// had hidden (rig dog-alone: hidden, wanted 0, still applied 4; the host's game let it go).
TEST(EnemyOwner, ALoneGuestSwitchesOffWhatItsStoryHidButKeptLoaded) {
    EXPECT_TRUE(LoneGuestSwitchesOff(true, true, false, 0, 4));
    EXPECT_TRUE(LoneGuestSwitchesOff(true, true, false, 0, 3));
    // Let go already, or not hidden, or off already: as it is.
    EXPECT_FALSE(LoneGuestSwitchesOff(true, true, false, 0, 0));
    EXPECT_FALSE(LoneGuestSwitchesOff(true, false, false, 0, 4));
    EXPECT_FALSE(LoneGuestSwitchesOff(true, true, true, 0, 4));
    // Still wanted (the story shows it again): the game's.
    EXPECT_FALSE(LoneGuestSwitchesOff(true, true, false, 4, 4));
    // With the host on the map its game says what is shown (ShowCharactersLikeTheHost).
    EXPECT_FALSE(LoneGuestSwitchesOff(false, true, false, 0, 4));
    // One it switched off: back on once shown by the story, or the guest no longer alone.
    EXPECT_FALSE(LoneGuestSwitchesBackOn(true, true));
    EXPECT_TRUE(LoneGuestSwitchesBackOn(true, false));
    EXPECT_TRUE(LoneGuestSwitchesBackOn(false, true));
}

// Rig 2026-10-15: awake by the far guest's groups, the host's game still let the enemy go.
TEST(EnemyOwner, TheOwnerRunsEveryAwakeEnemyTheGameLetGo) {
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Owner, 4, 4, 0xFFFF, false),
              std::optional<u16>{OwnedEnemyWord});
    // Handed out to a follower by the game (0x17): that follower runs it.
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Owner, 4, 4, 0xFFFF, false, true), std::nullopt);
    // The game's own already: unchanged.
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Owner, 4, 4, 1, false), std::nullopt);
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Owner, 4, 4, 0, false), std::nullopt);
    // Not awake (yet), or kept out by the game (a boss before its fight, Iosefka): the game's.
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Owner, 2, 2, 0xFFFF, false), std::nullopt);
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Owner, 4, 2, 0xFFFF, false), std::nullopt);
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Owner, 4, 4, 0xFFFF, true), std::nullopt);
    // Only the owner runs them.
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Follower, 4, 4, 0xFFFF, false), std::nullopt);
    EXPECT_EQ(AwakeEnemyWord(EnemyRole::Unknown, 4, 4, 0xFFFF, false), std::nullopt);
}

// Kaniel's play test 2026-10-18: krolebord ran Cathedral Ward and hurt an enemy; the moment the
// host came the host's game ran it from its spawn, and krolebord's copy jumped 19.9 m there. The
// host now follows the map's owner for a while first (shadNet's HostTakeBackMs): its game runs
// nothing then, whatever its own ownership step gave its own player.
TEST(EnemyOwner, AHostThatFollowsLetsItsGameRunNothing) {
    EXPECT_EQ(HostFollowerWord(EnemyRole::Follower, true, OwnedEnemyWord),
              std::optional<u16>{0xFFFF});
    EXPECT_EQ(HostFollowerWord(EnemyRole::Follower, true, 0), std::optional<u16>{0xFFFF});
    // The network's already: unchanged.
    EXPECT_EQ(HostFollowerWord(EnemyRole::Follower, true, 0xFFFF), std::nullopt);
    // A guest's game follows by its own (TickEnemyOwner, the host's hand-out); an owner runs.
    EXPECT_EQ(HostFollowerWord(EnemyRole::Follower, false, OwnedEnemyWord), std::nullopt);
    EXPECT_EQ(HostFollowerWord(EnemyRole::Owner, true, OwnedEnemyWord), std::nullopt);
    EXPECT_EQ(HostFollowerWord(EnemyRole::Unknown, true, OwnedEnemyWord), std::nullopt);
}

// The enemies around the owner's player: awake in the following host's game too (as the owner's
// own game wakes them around the others), so its copies take the owner's places before the host
// takes the map over.
TEST(EnemyOwner, TheOwnerAndAHostThatFollowsWakeAroundTheOthers) {
    EXPECT_TRUE(WakesAroundOthers(EnemyRole::Owner, true));
    EXPECT_TRUE(WakesAroundOthers(EnemyRole::Owner, false));
    EXPECT_TRUE(WakesAroundOthers(EnemyRole::Follower, true));
    EXPECT_FALSE(WakesAroundOthers(EnemyRole::Follower, false));
    EXPECT_FALSE(WakesAroundOthers(EnemyRole::Unknown, true));
}

TEST(EnemyOwner, OnlyThisPlayersBlockTakesTheOthersGroups) {
    // Another block's record (Cathedral Ward's, by the border): its groups are its own.
    WakeGroups other_block;
    other_block.wakes[6] = 0x10000000;
    other_block.wakes[7] = 2;
    const WakeGroups own = Groups(0x60400, 0x20000, 0x60400);
    const std::vector<WakeGroups> far{Groups(0x2810000, 0x2000000, 0x2810000)};
    EXPECT_EQ(WithOthersGroups(other_block, own, far), other_block);
    // A block with no record yet (all zero) stays asleep.
    EXPECT_EQ(WithOthersGroups(WakeGroups{}, own, far), WakeGroups{});
}

TEST(EnemyOwner, APlayerWithoutAPlaceYetWakesNothing) {
    const WakeGroups record = Groups(0x60000, 0x20000, 0x60000);
    const WakeGroups own = Groups(0x60400, 0x20000, 0x60400);
    WakeGroups joining;
    joining.wakes.fill(0xFFFFFFFF);
    joining.half.fill(0xFFFFFFFF);
    EXPECT_EQ(WithOthersGroups(record, own, std::vector<WakeGroups>{joining}), record);
    EXPECT_EQ(WithOthersGroups(record, own, std::vector<WakeGroups>{WakeGroups{}}), record);
}

// Phase 8 (one HP path): another game's hit shows on a follower's copy at once.
TEST(EnemyOwner, AFollowersCopyTakesAnotherGamesHitAtOnce) {
    EXPECT_EQ(HitNewsHp(EnemyRole::Follower, 484, 484, 300, 484), std::optional<s32>{300});
    // Scaled by each copy's max (the co-op HP bonus may differ), rounded up.
    EXPECT_EQ(HitNewsHp(EnemyRole::Follower, 968, 968, 300, 484), std::optional<s32>{600});
    // A killing hit: dead here too, at once.
    EXPECT_EQ(HitNewsHp(EnemyRole::Follower, 120, 484, 0, 484), std::optional<s32>{0});
    // Already as low (its own hit, or the owner's HP came first): nothing.
    EXPECT_EQ(HitNewsHp(EnemyRole::Follower, 300, 484, 300, 484), std::nullopt);
    EXPECT_EQ(HitNewsHp(EnemyRole::Follower, 250, 484, 300, 484), std::nullopt);
    // Dead already, or nonsense: nothing.
    EXPECT_EQ(HitNewsHp(EnemyRole::Follower, 0, 484, 0, 484), std::nullopt);
    EXPECT_EQ(HitNewsHp(EnemyRole::Follower, 484, 484, 300, 0), std::nullopt);
    // The owner applies the hit itself (the game's own 0x15): never twice.
    EXPECT_EQ(HitNewsHp(EnemyRole::Owner, 484, 484, 300, 484), std::nullopt);
    EXPECT_EQ(HitNewsHp(EnemyRole::Unknown, 484, 484, 300, 484), std::nullopt);
}

// Phase 8: no guest runs an enemy on a map that has an owner, at any distance; the owner wakes the
// groups around every member instead. A follower's hits always go to the owner.
TEST(EnemyOwner, AFollowerNeverRunsAnEnemy) {
    constexpr s32 Hosting = MatchingStateHosting;
    constexpr s32 Guest = MatchingStateSummonedGuest;
    EXPECT_EQ(EnemyHitState(EnemyRole::Follower, Guest), std::nullopt);
    EXPECT_EQ(EnemyHitState(EnemyRole::Follower, Hosting), std::optional<s32>{Guest});
}

// Kaniel 2026-10-15: a guest's hit did not count at once: its copy took the hit, then the host's
// older HP (from before the hit reached it) put it back until the host's answer came. For a while
// after its own hit, a guest's copy keeps what its hit left, unless the host's is lower.
TEST(EnemyOwner, AGuestsOwnHitStaysWhileTheHostCatchesUp) {
    EXPECT_EQ(HeldOwnHitHp(484, 382, 100), std::optional<s32>{382}) << "the host's HP from before";
    EXPECT_EQ(HeldOwnHitHp(382, 382, 100), std::nullopt) << "the host has it";
    EXPECT_EQ(HeldOwnHitHp(300, 382, 100), std::nullopt) << "another's hit on top";
    EXPECT_EQ(HeldOwnHitHp(484, 382, OwnHitHoldMs), std::nullopt) << "too long: the host's stands";
    EXPECT_EQ(HeldOwnHitHp(0, 382, 100), std::nullopt) << "dead";
}

// Kaniel 2026-10-15: the dog died as the team joined: the host, the map's first owner, took HP a
// guest had reported from its own world before it joined (1093 -> 413). Only an owner that takes
// a map over from another takes the log's HP; the first owner's world is the enemy.
TEST(EnemyOwner, OnlyAnOwnerThatTakesOverTakesTheLogsHp) {
    EXPECT_FALSE(TakesOverEnemyHp(EnemyOwnerEntry{CentralYharnam, "host", 1}));
    EXPECT_TRUE(TakesOverEnemyHp(EnemyOwnerEntry{CentralYharnam, "guest", 2}));
}

// Kaniel 2026-10-15: with the guest away from the host on one map, every enemy lagged (about
// 200 ms) and acted dumb. The game sends positions (ch 4) 8 to a packet, one packet every other
// frame, a round at most every 0.25 s: the guest's area woke three times the enemies, so each got
// a position about twice a second. The owner sends a round's full packets at once, 10 rounds a
// second.
TEST(EnemyOwner, TheOwnerSendsEveryPositionOfARoundAtOnce) {
    // Full, with slots left in the round: the owner sends it and the game fills another.
    EXPECT_TRUE(SendsFullPositionPacket(EnemyRole::Owner, 8, 8, 40, 281, 0));
    EXPECT_TRUE(SendsFullPositionPacket(EnemyRole::Owner, 8, 8, 40, 281, 5));
    // Not full yet, or the round's last slot (the game sends that one itself).
    EXPECT_FALSE(SendsFullPositionPacket(EnemyRole::Owner, 7, 8, 40, 281, 0));
    EXPECT_FALSE(SendsFullPositionPacket(EnemyRole::Owner, 8, 8, 280, 281, 0));
    // At most MaxPositionPacketsPerFrame a frame, the game's own among them.
    EXPECT_TRUE(
        SendsFullPositionPacket(EnemyRole::Owner, 8, 8, 40, 281, MaxPositionPacketsPerFrame - 2));
    EXPECT_FALSE(
        SendsFullPositionPacket(EnemyRole::Owner, 8, 8, 40, 281, MaxPositionPacketsPerFrame - 1));
    // Nonsense: the game's own way.
    EXPECT_FALSE(SendsFullPositionPacket(EnemyRole::Owner, 0, 0, 40, 281, 0));
    // A follower sends the enemies the owner handed it the same way; without an owner, the game's.
    EXPECT_TRUE(SendsFullPositionPacket(EnemyRole::Follower, 8, 8, 40, 281, 0));
    EXPECT_FALSE(SendsFullPositionPacket(EnemyRole::Unknown, 8, 8, 40, 281, 0));
}

// Every frame did not help (Kaniel 2026-10-15) and arrives in bunches without a de-jitter buffer:
// 10 rounds a second, from whichever game runs an enemy.
TEST(EnemyOwner, PositionRoundsComeTenTimesASecond) {
    EXPECT_FLOAT_EQ(PositionRoundSeconds(EnemyRole::Owner), 0.1f);
    EXPECT_FLOAT_EQ(PositionRoundSeconds(EnemyRole::Follower), 0.1f);
    // Without an owner, the game's 0.25 s (put back when a map has none).
    EXPECT_FLOAT_EQ(PositionRoundSeconds(EnemyRole::Unknown), GamePositionRoundSeconds);
    EXPECT_FLOAT_EQ(GamePositionRoundSeconds, 0.25f);
}

// The old hitch when the host came near an enemy the guest ran (Kaniel 2026-10-15: "lagging for a
// second before moving back"): the game hands enemies out every 3 s (the host's 0x17,
// NetWorldChrSync +0x630) on near lists sent every 5 s (a guest's 0x16, +0x638). Both every 0.25 s
// with an owner.
TEST(EnemyOwner, EnemiesChangeHandsWithinAQuarterSecond) {
    for (const EnemyRole role : {EnemyRole::Owner, EnemyRole::Follower}) {
        EXPECT_FLOAT_EQ(HandOutSeconds(role), 0.25f);
        EXPECT_FLOAT_EQ(NearListSeconds(role), 0.25f);
    }
    EXPECT_FLOAT_EQ(HandOutSeconds(EnemyRole::Unknown), GameHandOutSeconds);
    EXPECT_FLOAT_EQ(NearListSeconds(EnemyRole::Unknown), GameNearListSeconds);
    EXPECT_FLOAT_EQ(GameHandOutSeconds, 3.0f);
    EXPECT_FLOAT_EQ(GameNearListSeconds, 5.0f);
}

// A follower runs exactly the enemies the owner's latest hand-out (0x17) gave it; the rest follow
// the owner. Never two games on one enemy: the owner keeps what it did not hand out.
TEST(EnemyOwner, AFollowerRunsWhatTheOwnerHandedIt) {
    const std::vector<u32> handed{0x280b, 0x2871};
    EXPECT_TRUE(HandedOut(handed, 0x280b));
    EXPECT_FALSE(HandedOut(handed, 0x2809));
    EXPECT_FALSE(HandedOut({}, 0x280b));
    // The 0x17 payload: u32 n, then n entries, the chr +0x90 index in the low 16 bits (the high
    // ones change from one hand-out to the next; rig 2026-10-15: 6f728a0, 6e1280b).
    const std::vector<u8> payload{2, 0, 0, 0, 0x0b, 0x28, 0xe1, 0x06, 0x71, 0x28, 0xf7, 0x06};
    EXPECT_EQ(HandOutIds(payload), handed);
    // A short or nonsense one names nobody.
    EXPECT_EQ(HandOutIds(std::vector<u8>{3, 0, 0, 0, 0x0b, 0x28, 0, 0}), std::vector<u32>{});
    EXPECT_EQ(HandOutIds(std::vector<u8>{1, 0}), std::vector<u32>{});
}

// Kaniel's play test 2026-10-17: the dog near Iosefka's clinic (+0x90 index 0x2809) lagged while
// both players fought it. The owner's game handed it out and took it back every 0.3-1.3 s (every
// 0.25 s for minutes the day before), and each change of hands jumps it. The owner keeps an enemy
// until its game has wanted to hand it out for HandOverSettleMs; taking one back stays at once
// (the follower's game drops what a hand-out leaves out, so nobody would run it otherwise).
TEST(EnemyOwner, AnEnemyGoesToAFollowerOnlyOnceTheHandOutSettles) {
    HandOutSettle settle;
    const u32 dog = 0x06e12809; // a changing high half, the index in the low one
    const u32 held = HandOutHeldEntry;
    // The first hand-out to a member (or after a gap) goes at once: nothing went back and forth
    // yet. Kaniel's play test 2026-10-18: a host taking the map over from a guest (it followed
    // the guest's game a while first) ran the enemies by that guest a second more, two changes of
    // hands instead of none.
    const u32 by_krolebord = 0x2062;
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{by_krolebord}, 0),
              std::vector<u32>{by_krolebord});
    // Later ones settle: wanted at 0 ms, held (the owner runs it) until a settle time has passed.
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{by_krolebord, dog}, 0),
              (std::vector<u32>{by_krolebord, held}));
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{dog}, 500), std::vector<u32>{held});
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{dog}, HandOverSettleMs),
              std::vector<u32>{dog});
    // Handed out: it stays out while the game hands it out, high half as the game wrote it.
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{0x07002809}, HandOverSettleMs + 250),
              std::vector<u32>{0x07002809});
    // Taken back by the game: gone at once. Wanted again 0.3 s later: held again (ping-pong).
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{}, 2000), std::vector<u32>{});
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{dog}, 2300), std::vector<u32>{held});
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{}, 2550), std::vector<u32>{});
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{dog}, 2800), std::vector<u32>{held});
    // Other enemies and other members settle on their own; held ones keep their place.
    const u32 hunter = 0x2871;
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{hunter, dog}, 3000),
              (std::vector<u32>{held, held}));
    EXPECT_EQ(settle.Filter("lisara", std::vector<u32>{0x2805}, 3000), std::vector<u32>{0x2805});
    EXPECT_EQ(settle.Filter("lisara", std::vector<u32>{0x2805, hunter}, 3250),
              (std::vector<u32>{0x2805, held}));
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{hunter, dog}, 3800),
              (std::vector<u32>{held, dog}));
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{hunter, dog}, 4000),
              (std::vector<u32>{hunter, dog}));
    // A member whose last hand-out is stale starts over (a load, a trip away, a host that followed
    // the map's owner): its first goes at once.
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{dog}, 60'000), std::vector<u32>{dog});
    EXPECT_EQ(settle.Filter("krolebord", std::vector<u32>{dog, hunter}, 60'250),
              (std::vector<u32>{dog, held}));
    // Held entries name no enemy: the follower's game skips them (block 31, slot 0x7FF).
    const std::vector<u8> payload{2, 0, 0, 0, 0xff, 0xff, 0, 0, 0x09, 0x28, 0xe1, 0x06};
    EXPECT_EQ(HandOutIds(payload), std::vector<u32>{0x2809});
}

// Kaniel 2026-10-28: bosses stood passive in co-op. The owner's game handed Father Gascoigne
// (+0x90 index 0x289c) to the guest beside him and took him back every few seconds, against the
// boss pick (BossRunnerPick: the game's own runner), and each change of hands restarted his AI
// with no target. A boss goes where the pick says, never by the game's hand-out alone: held from
// every member but the pick's runner, on the first hand-out too and past the settle time. Its
// runner's game keeps it only while the hand-out names it (the follower's game drops an enemy a
// hand-out leaves out), so to the runner it goes as the game wrote it.
TEST(EnemyOwner, ABossGoesOnlyToTheMemberThePickNames) {
    const std::vector<BossHand> picks{{0x289c, ""}, {0x289e, "rig_guest"}, {0x2900, "krolebord"}};
    EXPECT_EQ(BossesKeptFrom(picks, "rig_guest"), (std::vector<u32>{0x289c, 0x2900}));
    EXPECT_EQ(BossesKeptFrom(picks, "krolebord"), (std::vector<u32>{0x289c, 0x289e}));
    EXPECT_EQ(BossesKeptFrom({}, "rig_guest"), std::vector<u32>{});

    HandOutSettle settle;
    const u32 held = HandOutHeldEntry;
    const u32 gascoigne = 0x0711289c;
    const u32 dog = 0x2809;
    const std::vector<u32> bosses{0x289c, 0x289e};
    EXPECT_EQ(settle.Filter("rig_guest", std::vector<u32>{dog, gascoigne}, 0, bosses),
              (std::vector<u32>{dog, held}));
    EXPECT_EQ(settle.Filter("rig_guest", std::vector<u32>{dog, gascoigne}, 500, bosses),
              (std::vector<u32>{dog, held}));
    EXPECT_EQ(settle.Filter("rig_guest", std::vector<u32>{dog, gascoigne}, 5000, bosses),
              (std::vector<u32>{dog, held}));
    // Without the boss list it settles as any enemy (the other enemies' rule is unchanged).
    EXPECT_EQ(settle.Filter("rig_guest", std::vector<u32>{dog, gascoigne}, 6000),
              (std::vector<u32>{dog, held}));
    EXPECT_EQ(settle.Filter("rig_guest", std::vector<u32>{dog, gascoigne}, 7000),
              (std::vector<u32>{dog, gascoigne}));
}

// The rig 2026-10-28 (boss-targets-runner-dies): the host died, the pick gave Gascoigne to the
// guest, but the host's game handed him to nobody. A follower's game sets every enemy word to
// 0xFFFF on each hand-out and 1 for those it names (0x018ED0ED, 0x018ED146, by the low 16 bits),
// so the guest's game let go of him every 0.25 s and took him again on its next tick. The pick's
// runner gets its bosses in the hand-out: in place of a held entry, else of another enemy (the
// owner runs what it did not hand out); a hand-out with no entries has no room.
TEST(EnemyOwner, ABossThePickGivesAMemberIsInItsHandOut) {
    const std::vector<BossHand> picks{{0x289c, "7269675f6775657374"}, {0x289e, ""}};
    EXPECT_EQ(BossesGivenTo(picks, "7269675f6775657374"), std::vector<u32>{0x289c});
    EXPECT_EQ(BossesGivenTo(picks, "6b726f6c65626f7264"), std::vector<u32>{});
    const u32 held = HandOutHeldEntry;
    const std::vector<u32> given{0x289c};
    // Already there (the game wrote it, high half and all): as it is.
    EXPECT_EQ(GiveBosses(std::vector<u32>{0x2809, 0x0711289c}, given),
              (std::vector<u32>{0x2809, 0x0711289c}));
    // A held entry makes room first, else the last other enemy.
    EXPECT_EQ(GiveBosses(std::vector<u32>{0x2809, held, 0x2871}, given),
              (std::vector<u32>{0x2809, 0x289c, 0x2871}));
    EXPECT_EQ(GiveBosses(std::vector<u32>{0x2809, 0x2871}, given),
              (std::vector<u32>{0x2809, 0x289c}));
    EXPECT_EQ(GiveBosses(std::vector<u32>{}, given), std::vector<u32>{});
    // Two bosses given, room for one: the first; never in place of another given boss.
    EXPECT_EQ(GiveBosses(std::vector<u32>{0x2809}, std::vector<u32>{0x289c, 0x2900}),
              std::vector<u32>{0x289c});
    EXPECT_EQ(GiveBosses(std::vector<u32>{0x2900}, std::vector<u32>{0x289c, 0x2900}),
              std::vector<u32>{0x2900});
}

// A follower's measure: the positions it took for the enemies by its player, per second.
TEST(EnemyOwner, AFollowerCountsThePositionsOfTheEnemiesByIt) {
    // The packets name an enemy by its +0x90 index (map index << 11 | slot), the log by chr id.
    const std::map<u32, u32> taken{{0x280b, 40}, {0x2809, 10}, {0x2871, 99}};
    using Near = std::vector<std::pair<u32, u32>>;
    // Fastest first; an enemy by it without a position yet shows 0.
    EXPECT_EQ(
        PositionRatesText(
            taken, Near{{0x10014009, 0x2809}, {0x1001400b, 0x280b}, {0x10014066, 0x2866}}, 10.0),
        "1001400b 4.0, 10014009 1.0, 10014066 0.0");
    EXPECT_EQ(PositionRatesText(taken, Near{}, 10.0), "none");
    EXPECT_EQ(PositionRatesText(taken, Near{{0x1001400b, 0x280b}}, 0.0), "none");
}

// Rig 2026-10-15 (a write watch): the game copies an enemy's place into the sync's position record
// and marks it to send (0x0151BCA0) only for the enemies by its own player; the ones awake by
// another player never went out. The owner does it for every awake enemy it runs.
TEST(EnemyOwner, TheOwnerSendsThePositionOfEveryAwakeEnemyItRuns) {
    EXPECT_TRUE(OwnerSendsPositionOf(EnemyRole::Owner, 4, 4, OwnedEnemyWord, false));
    EXPECT_TRUE(OwnerSendsPositionOf(EnemyRole::Owner, 4, 4, 0, false));
    // Let go (the network's), asleep, half awake or kept out: not this game's to send.
    EXPECT_FALSE(OwnerSendsPositionOf(EnemyRole::Owner, 4, 4, 0xFFFF, false));
    EXPECT_FALSE(OwnerSendsPositionOf(EnemyRole::Owner, 0, 0, OwnedEnemyWord, false));
    EXPECT_FALSE(OwnerSendsPositionOf(EnemyRole::Owner, 2, 2, OwnedEnemyWord, false));
    EXPECT_FALSE(OwnerSendsPositionOf(EnemyRole::Owner, 4, 4, OwnedEnemyWord, true));
    // A follower sends none.
    EXPECT_FALSE(OwnerSendsPositionOf(EnemyRole::Follower, 4, 4, OwnedEnemyWord, false));
}

// The old hitch (Kaniel 2026-10-15): an enemy that changed hands jumped back. Each game logs an
// enemy by its player that moves farther in one frame than any enemy runs (the rig forbids it).
TEST(EnemyOwner, AnEnemyThatJumpsInOneFrameIsSeen) {
    const std::array<float, 3> at{-216.7f, -54.7f, 178.1f};
    EXPECT_FALSE(EnemyJumped(at, at));
    EXPECT_FALSE(EnemyJumped(at, {-216.5f, -54.7f, 178.2f})); // a run, one frame
    EXPECT_TRUE(EnemyJumped(at, {-215.8f, -54.7f, 178.1f}));  // 0.9 m at once
    EXPECT_TRUE(EnemyJumped(at, {-212.0f, -54.7f, 178.1f}));  // 4.7 m at once
    EXPECT_TRUE(EnemyJumped(at, {-216.7f, -58.0f, 178.1f}));  // up or down too
}
