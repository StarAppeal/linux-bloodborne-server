// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/bloodborne_flags.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_team.h"

using namespace Core::Bloodborne;

namespace {

constexpr s32 MinSampled = std::numeric_limits<s32>::min() + 2; // flag - 1 stays in range
constexpr s32 MaxFlag = std::numeric_limits<s32>::max();

void AddAround(std::vector<s32>& sample, s64 flag, s64 reach = 2) {
    for (s64 value = flag - reach; value <= flag + reach; ++value) {
        if (value >= MinSampled && value <= MaxFlag) {
            sample.push_back(static_cast<s32>(value));
        }
    }
}

void AddEvery(std::vector<s32>& sample, s64 from, s64 to, s64 stride) {
    for (s64 value = from; value <= to; value += stride) {
        AddAround(sample, value, 0);
    }
}

// The flags the characterization looks at: every table row's edges, every flag number the
// co-op code names, a sweep of the whole range and the map flag families, each flag of the
// lamps' and boss fogs' windows of every map.
std::vector<s32> FlagSample() {
    std::vector<s32> sample;
    for (const FlagRange& row : FlagTable) {
        AddAround(sample, row.first);
        AddAround(sample, row.last);
    }
    static constexpr s32 Named[]{
        4000,     4047,     4050,     4063,     4999,     9000,     9180,     9181,     9198,
        9199,     9200,     9300,     9337,     9345,     9348,     9350,     9400,     9401,
        9402,     9403,     9404,     10007810, 12100000, 12101000, 12101009, 12101010, 12101020,
        12101029, 12101030, 12107000, 12107810, 12110000, 12410900, 12414810, 12414811, 13998190,
        50000000, 52900000, 53000000, 54000000, 70000000, 70000100, 70000140, 72100000, 72100100,
        72101000, 72102000, 72104000, 72110000, 79900100, 94005001, 94105001, 94905999,
    };
    for (const s32 flag : Named) {
        AddAround(sample, flag);
    }
    for (const s32 flag : BossFogEntryFlags) {
        AddAround(sample, flag);
    }
    for (const s32 flag : DoorFlags) {
        AddAround(sample, flag);
    }
    for (const EncounterInsight& boss : EncounterInsights()) {
        AddAround(sample, boss.kill_event);
        AddAround(sample, boss.once_flag);
    }
    for (const s32 flag : ShopLineFlags()) {
        AddAround(sample, flag);
    }
    AddAround(sample, MinSampled, 0);
    AddAround(sample, MaxFlag, 0);
    AddEvery(sample, -1000, MaxFlag, 9973);
    AddEvery(sample, -100, 20000, 1);
    AddEvery(sample, 12000000 - 100, 14000000 + 100, 101);
    AddEvery(sample, 12100000, 12110000, 1);
    AddEvery(sample, 49999000, 54001000, 97);
    AddEvery(sample, 72100000, 72110000, 1);
    AddEvery(sample, 79900000, 79901000, 1);
    for (s64 block = 1190; block <= 1410; ++block) {
        const s64 base = block * 10000;
        AddEvery(sample, base + 4690, base + 4870, 1); // boss fogs
        // The lamps' active flags: the first and last two lamps of every map, all of every tenth.
        AddEvery(sample, base + 7800, base + 7835, 1);
        AddEvery(sample, base + 8165, base + 8200, 1);
        if (block % 10 == 0) {
            AddEvery(sample, base + 7836, base + 8164, 1);
        }
    }
    for (s64 map = 0; map < 1000; ++map) {
        AddEvery(sample, 70000000 + map * 10000 + 90, 70000000 + map * 10000 + 210, 1); // lamps
    }
    for (s64 prefix = 1; prefix < 100; ++prefix) {
        AddEvery(sample, prefix * 1000000, prefix * 1000000 + 20, 1); // xx000000 families
    }
    std::ranges::sort(sample);
    const auto [end, last] = std::ranges::unique(sample);
    sample.erase(end, last);
    return sample;
}

// Every flag decision of the party code, one bit each.
u32 Decisions(s32 flag) {
    constexpr u32 SomeMap = 0x18000000;
    const auto progress = GuestProgressFlags(true, true, SomeMap, true, true);
    const auto shop = ShopLineFlags();
    u32 bits = 0;
    bits |= IsLampTravelFlag(flag) ? 1u << 0 : 0;
    bits |= IsHeadstoneTravelFlag(flag) ? 1u << 1 : 0;
    bits |= IsMapPickupFlag(flag) ? 1u << 2 : 0;
    bits |= CarriesToGuestWorld(flag) ? 1u << 3 : 0;
    bits |= IsBossFogEntryFlag(flag) ? 1u << 4 : 0;
    bits |= KeepsLocalFlag(true, flag) ? 1u << 5 : 0;
    bits |= KeepsLocalFlag(false, flag) ? 1u << 6 : 0;
    bits |= TakesHostMapFlag(flag) ? 1u << 7 : 0;
    bits |= SharedWithParty(flag) ? 1u << 8 : 0;
    bits |= LampOfActiveFlag(flag).has_value() ? 1u << 9 : 0;
    bits |= std::ranges::find(progress, flag) != progress.end() ? 1u << 10 : 0;
    bits |= std::ranges::find(shop, flag) != shop.end() ? 1u << 11 : 0;
    return bits;
}

// "<flag> <decisions>[ <lamp>]" at each sampled flag where the decisions (or the lamp of an
// active flag) differ from the sampled flag before it.
std::vector<std::string> DecisionChanges() {
    std::vector<std::string> lines;
    u32 previous_bits = ~0u;
    s32 previous_lamp = -1;
    for (const s32 flag : FlagSample()) {
        const u32 bits = Decisions(flag);
        const s32 lamp = LampOfActiveFlag(flag).value_or(0);
        if (bits != previous_bits || lamp != previous_lamp) {
            lines.push_back(lamp != 0 ? fmt::format("{} {:x} {}", flag, bits, lamp)
                                      : fmt::format("{} {:x}", flag, bits));
            previous_bits = bits;
            previous_lamp = lamp;
        }
    }
    return lines;
}

} // namespace

// The flag rules before the flag table (2026-10-12), recorded from the hand-written predicates:
// the table must decide every flag the same way. BLOODBORNE_FLAG_RULES_WRITE=1 writes the file
// again (only for a deliberate rule change).
TEST(BloodborneFlagRules, DecisionsMatchTheRecordedOnes) {
    const std::vector<std::string> actual = DecisionChanges();
    if (std::getenv("BLOODBORNE_FLAG_RULES_WRITE") != nullptr) {
        std::ofstream file(BLOODBORNE_FLAG_RULES, std::ios::trunc | std::ios::binary);
        // (Split so the license linter does not read these lines as this file's own.)
        file << "# SPDX-"
                "FileCopyrightText: Copyright 2026 shadPS4 Emulator Project\n"
                "# SPDX-"
                "License-Identifier: GPL-2.0-or-later\n"
                "# <flag> <decision bits, hex>[ <lamp of an active flag>]: at each sampled flag "
                "where they change (test_bloodborne_flag_rules.cpp)\n";
        for (const std::string& line : actual) {
            file << line << '\n';
        }
        GTEST_SKIP() << "wrote " << actual.size() << " lines to " << BLOODBORNE_FLAG_RULES;
    }
    std::ifstream file(BLOODBORNE_FLAG_RULES);
    ASSERT_TRUE(file) << BLOODBORNE_FLAG_RULES;
    std::vector<std::string> expected;
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.front() != '#') {
            expected.push_back(line);
        }
    }
    ASSERT_GT(expected.size(), 1000u);
    const auto [mismatch, other] = std::ranges::mismatch(actual, expected);
    EXPECT_TRUE(mismatch == actual.end() && other == expected.end())
        << "first difference: actual \"" << (mismatch == actual.end() ? "end" : *mismatch)
        << "\" expected \"" << (other == expected.end() ? "end" : *other) << "\"";
}

TEST(BloodborneFlagRules, PredicatesFollowTheTable) {
    for (const s32 flag : FlagSample()) {
        const FlagRule rule = RuleFor(flag);
        ASSERT_EQ(IsLampTravelFlag(flag), rule.Has(FlagLampTrip)) << flag;
        ASSERT_EQ(IsHeadstoneTravelFlag(flag), rule.Has(FlagHeadstoneTrip)) << flag;
        ASSERT_EQ(IsMapPickupFlag(flag), rule.Has(FlagMapPickup)) << flag;
        ASSERT_EQ(CarriesToGuestWorld(flag), rule.Has(FlagCarriesHome)) << flag;
        ASSERT_EQ(IsBossFogEntryFlag(flag), rule.Has(FlagBossFogEntry)) << flag;
        ASSERT_EQ(KeepsLocalFlag(true, flag), rule.owner == FlagOwner::PerPlayer) << flag;
        ASSERT_FALSE(KeepsLocalFlag(false, flag)) << flag;
        ASSERT_EQ(TakesHostMapFlag(flag), rule.Has(FlagTakesHostMap)) << flag;
        ASSERT_EQ(SharedWithParty(flag), rule.owner == FlagOwner::Shared) << flag;
    }
}

TEST(BloodborneFlagRules, RowsNestOrStayApart) {
    for (size_t earlier = 0; earlier < FlagTable.size(); ++earlier) {
        const FlagRange& inner = FlagTable[earlier];
        ASSERT_LE(inner.first, inner.last) << inner.why;
        for (size_t later = earlier + 1; later < FlagTable.size(); ++later) {
            const FlagRange& outer = FlagTable[later];
            const bool apart = inner.last < outer.first || outer.last < inner.first;
            const bool nested = outer.first <= inner.first && inner.last <= outer.last &&
                                (outer.first != inner.first || outer.last != inner.last);
            EXPECT_TRUE(apart || nested)
                << "row " << earlier << " (" << inner.first << "-" << inner.last << ") and row "
                << later << " (" << outer.first << "-" << outer.last << ")";
        }
    }
}

TEST(BloodborneFlagRules, EveryRowDecidesSomeFlag) {
    for (const FlagRange& row : FlagTable) {
        bool decides = false;
        for (s64 flag = row.first; flag <= row.last && !decides; ++flag) {
            const s32 value = static_cast<s32>(flag);
            decides = row.Contains(value) && RowFor(value) == &row;
            if (flag - row.first > 100000) {
                break;
            }
        }
        EXPECT_TRUE(decides) << row.why;
    }
}

TEST(BloodborneFlagRules, SessionFlagsAreNeitherKeptNorShared) {
    for (const FlagRange& row : FlagTable) {
        if (row.rule.owner == FlagOwner::Session) {
            EXPECT_FALSE(row.rule.Has(FlagCarriesHome)) << row.why;
            EXPECT_FALSE(row.rule.Has(FlagTakesHostMap)) << row.why;
        }
    }
}

// Kaniel 2026-10-19: a door one member opened stayed shut for a teammate on another map. The game
// syncs an object act only to players on the same map, so a door's flags go to the whole party,
// as a lamp's do: Old Yharnam's gates (m28 12800480: object act flags 12800201..., done flags
// 12800480 + slot), Cathedral Ward's doors (12400130: 12400112, 12400130...) and its gate
// (12400760 sets 12400160).
TEST(BloodborneFlagRules, DoorsOneMemberOpenedOpenForTheParty) {
    for (const s32 flag : {12800201, 12800202, 12800209, 12800480, 12800481, 12800484, 12400112,
                           12400130, 12400190, 12400760, 12400160}) {
        EXPECT_TRUE(SharedWithParty(flag)) << flag;
        EXPECT_FALSE(SharesClearsWithParty(flag)) << flag;
        EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
        EXPECT_TRUE(TakesHostMapFlag(flag)) << flag;
    }
    const auto shared = FlagsToShare(FlagChanges{{12800201, 12800200}, {12400112}});
    EXPECT_EQ(shared, (std::vector<std::pair<s32, bool>>{{12800201, true}}));
    for (const s32 flag : DoorFlags) {
        EXPECT_TRUE(flag >= 12000000 && flag < 14000000) << flag;
        EXPECT_FALSE(IsBossFogEntryFlag(flag)) << flag;
        EXPECT_FALSE(LampOfActiveFlag(flag).has_value()) << flag;
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Shared) << flag;
    }
    EXPECT_TRUE(std::ranges::is_sorted(DoorFlags));
    EXPECT_EQ(std::ranges::adjacent_find(DoorFlags), DoorFlags.end());
}

TEST(BloodborneFlagRules, BossFoundFlagsArePerPlayer) {
    std::set<s32> found;
    for (const EncounterInsight& boss : EncounterInsights()) {
        found.insert(boss.once_flag);
        EXPECT_EQ(RuleFor(boss.once_flag).owner, FlagOwner::PerPlayer) << boss.once_flag;
    }
    for (s32 flag = 9300; flag < 9400; ++flag) {
        EXPECT_EQ(RuleFor(flag).owner == FlagOwner::PerPlayer, found.contains(flag)) << flag;
    }
}

// Kaniel 2026-10-12: the messengers' weapon lines follow each player's own hunter badges, as in
// the base game. The Dream's 12101000 runs in each player's own game (no network sync) and reads
// that player's items, so a guest keeps its own lines and never takes or carries the host's.
TEST(BloodborneFlagRules, WeaponLinesFollowEachPlayersOwnBadges) {
    for (const s32 flag : ShopLineFlags()) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::PerPlayer) << flag;
        EXPECT_TRUE(KeepsLocalFlag(true, flag)) << flag;
        EXPECT_FALSE(TakesHostMapFlag(flag)) << flag;
        EXPECT_FALSE(SharedWithParty(flag)) << flag;
    }
}

// Kaniel 2026-10-18: a guest never got the Sword Hunter Badge. The Dream's 12100450 sets 6645
// once the player holds the badge, and the Cleric Beast's 12411700 gives the badge only while
// 6645 is off. As a world flag, the team's world (the host's) put the host's 6645 into every
// member's save. A held-item flag says what that player holds: its own, never the host's.
TEST(BloodborneFlagRules, HeldItemFlagsAreEachPlayersOwn) {
    for (const HeldItemFlag& held : HeldItemFlags) {
        EXPECT_EQ(RuleFor(held.flag).owner, FlagOwner::PerPlayer) << held.flag;
        EXPECT_TRUE(KeepsLocalFlag(true, held.flag)) << held.flag;
        EXPECT_TRUE(CarriesToGuestWorld(held.flag)) << held.flag;
        EXPECT_FALSE(TakesHostMapFlag(held.flag)) << held.flag;
        EXPECT_FALSE(SharedWithParty(held.flag)) << held.flag;
    }
    // m21_00 event 12100450 (01.09): the workshop's tools, then the badges.
    const std::vector<std::pair<s32, u32>> dream{
        {6630, 4103}, {6631, 4104}, {6632, 4105}, {6633, 4102}, {6640, 4110},
        {6641, 4112}, {6642, 4113}, {6643, 4111}, {6644, 4118}, {6645, 4114},
        {6646, 4115}, {6647, 4116}, {6648, 4119}, {6649, 4117},
    };
    std::vector<std::pair<s32, u32>> table;
    for (const HeldItemFlag& held : HeldItemFlags) {
        table.emplace_back(held.flag, held.goods);
    }
    EXPECT_EQ(table, dream);
    // Only those: the flags between them stay the host's.
    for (s32 flag = 6600; flag < 6700; ++flag) {
        const bool held = std::ranges::any_of(
            HeldItemFlags, [&](const HeldItemFlag& item) { return item.flag == flag; });
        EXPECT_EQ(RuleFor(flag).owner == FlagOwner::PerPlayer, held) << flag;
    }
}

// Kaniel 2026-10-20: back in the Dream the guests' headstones all said "no place to awaken",
// and his first one did after a rejoin. Each headstone's talk (t210000-t210004) asks one flag,
// "a lamp of mine is lit" (12105030-12105034), which m21_00's 12105020-12105024 work out in
// every player's game from its own lamps. As the host's, the other players' values came in over
// the game's flag messages and flipped about once a second in all three logs. Those messages are
// also how a game's own event sets it in a session (rig 2026-10-20: kept off against the other's,
// it never came on), so a game neither keeps nor takes a value: it takes its own lamps' one.
TEST(BloodborneFlagRules, DreamHeadstonesReadEachPlayersOwnLamps) {
    for (const HeadstoneLamps& headstone : DreamHeadstones) {
        EXPECT_EQ(RuleFor(headstone.flag).owner, FlagOwner::PerPlayer) << headstone.flag;
        EXPECT_FALSE(CarriesToGuestWorld(headstone.flag)) << headstone.flag;
        EXPECT_FALSE(TakesHostMapFlag(headstone.flag)) << headstone.flag;
        EXPECT_FALSE(SharedWithParty(headstone.flag)) << headstone.flag;
        for (const s32 lamp : headstone.lamps) {
            if (lamp == 0) {
                continue;
            }
            EXPECT_EQ(RuleFor(lamp).owner, FlagOwner::Shared) << lamp << " a lamp's active flag";
        }
    }
    std::vector<s32> flags;
    for (const HeadstoneLamps& headstone : DreamHeadstones) {
        flags.push_back(headstone.flag);
    }
    EXPECT_EQ(flags, (std::vector<s32>{12105030, 12105031, 12105032, 12105033, 12105034}));
    // The flags beside them stay the host's; the Doll's 12100122 too.
    for (const s32 flag : {12100122, 12105029, 12105035}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
    }
}

// m21_00 12105020-12105024 (01.09): on while any of the headstone's lamps is lit.
TEST(BloodborneFlagRules, AHeadstoneIsLitByThisPlayersOwnLamps) {
    const std::set<s32> lit{12407830, 13207810};
    const auto on = [&](s32 flag) { return lit.contains(flag); };
    EXPECT_EQ(HeadstoneLampsLit(12105030, on), std::optional<bool>{true}); // Cathedral Ward
    EXPECT_EQ(HeadstoneLampsLit(12105031, on), std::optional<bool>{true}); // Yahar'gul's first
    EXPECT_EQ(HeadstoneLampsLit(12105032, on), std::optional<bool>{false});
    EXPECT_EQ(HeadstoneLampsLit(12105033, on), std::optional<bool>{false});
    EXPECT_EQ(HeadstoneLampsLit(12105034, on), std::optional<bool>{false});
    EXPECT_EQ(HeadstoneLampsLit(12105035, on), std::nullopt);
    EXPECT_EQ(HeadstoneLampsLit(12100122, on), std::nullopt);
    EXPECT_EQ(HeadstoneLampsLit(12105034, [](s32 flag) { return flag == 13607850; }),
              std::optional<bool>{true});
}

// A held-item flag follows what this player holds: on while it holds the item (as 12100450 sets
// it), off while it does not (a save that took the host's value from a team world got it back
// wrong, so the boss, NPC or pickup never gave the item). Outside a party, the game's own.
TEST(BloodborneFlagRules, HeldItemFlagFollowsThePlayersItems) {
    EXPECT_EQ(HeldItemFlagFix(true, true, false), std::optional<bool>{false});
    EXPECT_EQ(HeldItemFlagFix(true, false, true), std::optional<bool>{true});
    EXPECT_EQ(HeldItemFlagFix(true, true, true), std::nullopt);
    EXPECT_EQ(HeldItemFlagFix(true, false, false), std::nullopt);
    EXPECT_EQ(HeldItemFlagFix(false, true, false), std::nullopt);
    EXPECT_EQ(HeldItemFlagFix(false, false, true), std::nullopt);
}

// Team altars (Kaniel 2026-10-17): a ritual at any member's altar puts its dungeon on every
// member's altar. Ritual altar k keeps its dungeon in event flags 94k00000-94k05999 (8-bit values
// packed into the flags, and switches such as "a chalice on it", 94k05001): those are the party's,
// clears as well as sets, so the other altars take the same values.
TEST(BloodborneFlagRules, RitualAltarsAreTheTeams) {
    for (s32 altar = 0; altar < 10; ++altar) {
        const s32 base = 94000000 + altar * 100000;
        for (const s32 flag :
             {base, base + 31, base + 5000, base + 5001, base + 5500, base + 5999}) {
            EXPECT_TRUE(SharedWithParty(flag)) << flag;
            EXPECT_TRUE(RuleFor(flag).Has(FlagSharesClears)) << flag;
        }
        EXPECT_FALSE(SharedWithParty(base + 6000)) << base + 6000;
        EXPECT_FALSE(SharedWithParty(base - 1)) << base - 1;
    }
}

// Kaniel 2026-10-18: a teammate holding no chalice is given the chalice of the dungeon that came
// to its altar (the altar's talk opens only for a player holding one, as the conductor does).
TEST(BloodborneFlagRules, AnAltarsDungeonNamesItsChalice) {
    EXPECT_EQ(AltarChaliceGoods(10), 6109u) << "Pthumeru Chalice, depth 1";
    EXPECT_EQ(AltarChaliceGoods(21), 6219u) << "Hintertomb Chalice, depth 2";
    EXPECT_EQ(AltarChaliceGoods(53), 6539u) << "Isz Chalice, depth 5";
    EXPECT_FALSE(AltarChaliceGoods(0xFF)) << "an altar with no record";
    EXPECT_FALSE(AltarChaliceGoods(11)) << "no standard chalice there";
}

// Kaniel's playtest 2026-10-19: one player's step into a chalice dungeon took everyone in the Dream
// along, into a void. One player's talk with a ritual altar runs in every game, and each game's
// talk set "warp to the layer" (72100420 + altar) and the layer (72100300 + layer) there, so each
// game warped its own player. Only the game whose own player talks sets them; the warp event's
// own clear (2026-10-20: #276 kept that from happening) is no talk and is not touched.
TEST(BloodborneFlagRules, OnlyTheTalkersGameSetsTheAltarWarp) {
    for (const s32 flag : {72100420, 72100421, 72100429, 72100300, 72100309}) {
        EXPECT_TRUE(SkipsOthersAltarWarp(flag, true)) << flag << ": another player talks";
        EXPECT_FALSE(SkipsOthersAltarWarp(flag, false)) << flag << ": this player, or not known";
    }
    for (const s32 flag : {72100419, 72100430, 72100299, 72100310, 70000230, 6708}) {
        EXPECT_FALSE(SkipsOthersAltarWarp(flag, true)) << flag;
    }
}

// A ritual altar's warp request is each game's own: as the host's world, a guest's copy took the
// host's and every teammate in the Dream warped with it, into the void (rig chalice-team3,
// 2026-10-20).
TEST(BloodborneFlagRules, AnAltarsWarpRequestStaysInItsGame) {
    for (const s32 flag : {72100300, 72100305, 72100309, 72100420, 72100421, 72100429}) {
        EXPECT_FALSE(IsWorldFlag(flag)) << flag;
        EXPECT_FALSE(CarriesToGuestWorld(flag)) << flag;
        EXPECT_FALSE(SharedWithParty(flag)) << flag;
        EXPECT_FALSE(KeepsLocalFlag(true, flag)) << flag << ": the warp event clears it";
    }
    EXPECT_TRUE(IsWorldFlag(72100299));
    EXPECT_TRUE(IsWorldFlag(72100430));
}

TEST(BloodborneFlagRules, ListsComeFromTheTable) {
    EXPECT_EQ(GuestProgressFlags(true, true, 0x18000000, true, true),
              (std::vector<s32>{9401, 9403}));
    const auto shop = ShopLineFlags();
    EXPECT_EQ(std::vector<s32>(shop.begin(), shop.end()),
              (std::vector<s32>{12101000, 12101001, 12101002, 12101003, 12101004, 12101005,
                                12101006, 12101007, 12101008, 12101009}));
    const auto from_table = FlagsWith<FlagShopLine>();
    EXPECT_TRUE(std::ranges::equal(from_table, shop));
}
