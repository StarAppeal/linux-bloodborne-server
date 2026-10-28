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
// Kaniel's test 2026-10-23: a guest alone in Cathedral Ward killed the hag and opened a door; the
// host who came later found her alive and the door shut. A lone guest's game runs its map's story,
// so the changes it makes to that map's flags (once the load settled) go to the whole team.
TEST(BloodborneFlagRules, ALoneGuestsMapStepsGoToTheTeam) {
    constexpr u32 CathedralWard = 0x18000000;
    const FlagChanges changes{{12400800, 12400565, 12410602, 52411000, 2011, 12400112},
                              {12400701, 1101}};
    EXPECT_EQ(
        LoneGuestFlagsToShare(changes, CathedralWard),
        (std::vector<std::pair<s32, bool>>{{12400565, true}, {12400701, false}, {12400800, true}}));
    // A door's flag goes to the party anyway (FlagsToShare); another map's flags, pickups, the
    // game's own and the story's other flags stay its own.
    EXPECT_TRUE(
        LoneGuestFlagsToShare(FlagChanges{{12410602, 52411000, 2011}, {1101}}, CathedralWard)
            .empty());
}

// Kaniel's test 2026-10-23 again: the hag died 9 s after the guest's load and stayed alive for the
// host; with no wait the map's own load flags (the boss's per-visit 12414802...) went out too.
TEST(BloodborneFlagRules, ALoneGuestSharesItsMapStepsButNotItsLoadsFlags) {
    constexpr u32 CentralYharnam = 0x18010000;
    LoneGuestShares shares;
    // The load sets up the map: nothing goes, those flags are its load's.
    EXPECT_TRUE(
        shares.Take(FlagChanges{{12414802, 12414803}, {12410286}}, CentralYharnam, 3'000).empty());
    EXPECT_TRUE(shares.Take(FlagChanges{{12414810}, {}}, CentralYharnam, 7'900).empty());
    // After it, a step goes at once; the load's flags (late this time) still do not.
    EXPECT_EQ(
        shares.Take(FlagChanges{{12410999, 12414802, 12414810}, {12410286}}, CentralYharnam, 9'100),
        (std::vector<std::pair<s32, bool>>{{12410999, true}}));
    // Another visit's load: remembered from the first.
    EXPECT_TRUE(shares.Take(FlagChanges{{12414803}, {}}, CentralYharnam, 20'000).empty());
    // Another map's load flags are its own.
    EXPECT_EQ(shares.Take(FlagChanges{{12400800}, {}}, 0x18000000, 9'100),
              (std::vector<std::pair<s32, bool>>{{12400800, true}}));
}

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

// Kaniel 2026-10-20: the enemies rising out of the mist in the Hemwick woods rose only for the
// host's approach. A guest's own copy of the ambush event (m22_00 12204000, slot k for enemy
// 2200500 + k) woke the enemy in its game only (rig mist-ambush-guest); the host's copy also rises
// once its slot's done flag 12204000 + k is on, so the party shares those.
TEST(BloodborneFlagRules, AnAmbushAGuestWokeRisesForTheHost) {
    for (s32 flag = 12204000; flag <= 12204008; ++flag) {
        EXPECT_TRUE(SharedWithParty(flag)) << flag;
        EXPECT_FALSE(SharesClearsWithParty(flag)) << flag;
        EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
        EXPECT_TRUE(TakesHostMapFlag(flag)) << flag;
    }
    EXPECT_FALSE(SharedWithParty(12204009));
    EXPECT_FALSE(SharedWithParty(12204011)); // the ambush armed: the host's Insight decides
}

// Kaniel's playtest 2026-10-22: the party wiped in Darkbeast Paarl's fight; the host's reload
// put his fight flags (a map's 4xxx and 5xxx flags: 12304700 the fog entered, 12304702 and 12304704
// his start) off, as every load does, but the team's world and the guests' kept changes still had
// them on, so each guest's trip load set them again and his fight started over at the lamp (his
// health bar up, rig 2026-10-23). A map's load-reset flags are no world flags: the team's world
// leaves them out, a guest does not keep them, and a guest loading the host's map does not take
// them; the fog's entry is still shared with the party while it holds.
TEST(BloodborneFlagRules, AMapsLoadResetFlagsStayOutOfTheWorld) {
    for (const s32 flag :
         {12304700, 12304702, 12304704, 12304732, 12305021, 12305701, 12805465, 12805676}) {
        EXPECT_FALSE(IsWorldFlag(flag)) << flag;
        EXPECT_FALSE(CarriesToGuestWorld(flag)) << flag;
        EXPECT_FALSE(TakesHostMapFlag(flag)) << flag;
    }
    EXPECT_TRUE(IsBossFogEntryFlag(12304700));
    EXPECT_TRUE(SharedWithParty(12304700)) << "a guest entering the fog starts the boss";
    for (const s32 flag : {12301700, 12301702, 12300100, 12303999, 12306000, 12204000}) {
        EXPECT_TRUE(IsWorldFlag(flag)) << flag;
        EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
    }
}

// Audit 2026-10-25: the League's covenant flags come from each player's own runes and items:
// common events 6788 and 6789 count goods 1700 into 6780-6787 and 6790-6797 under SpEffect 6141
// or 6143, 6800-6808 sum them, 6809 sets the ranks 6810-6812 from that; 6813 is the League
// joined in the Forbidden Woods (m27_00 12700902), 6814 by Valtr's talk (t270503); 6815 and
// 6816 put the League's effects on by them. As the host's world, a guest's rank and membership
// followed the host's in a session.
TEST(BloodborneFlagRules, TheLeaguesCovenantFlagsAreEachPlayersOwn) {
    for (s32 flag = 6780; flag <= 6816; ++flag) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::PerPlayer) << flag;
        EXPECT_TRUE(KeepsLocalFlag(true, flag)) << flag;
        EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
    }
    EXPECT_NE(RuleFor(6779).owner, FlagOwner::PerPlayer);
    EXPECT_NE(RuleFor(6817).owner, FlagOwner::PerPlayer);
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

// Kaniel's playtest 2026-10-20: the Witch's Abode lamp broke for the whole party. A lamp's "light
// it" request (7MMM02LL, common event 7100: the talk sets it, the event lights the lamp once and
// ends at once on any later load with it on) is each player's own: krolebord's, set while she
// lived, lit nothing and still reached the host, and the host's reached Lisara as a story step.
// The lit lamp itself is shared by its active flags and the lamp lists.
TEST(BloodborneFlagRules, ALampsLightRequestIsEachPlayersOwn) {
    for (const s32 flag : {72110200, 72200200, 72200201, 72410219, 73990200, 73990219}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::PerPlayer) << flag;
        EXPECT_TRUE(KeepsLocalFlag(true, flag)) << flag;
        EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
        EXPECT_FALSE(TakesHostMapFlag(flag)) << flag;
        EXPECT_FALSE(SharedWithParty(flag)) << flag;
        EXPECT_FALSE(IsLampTravelFlag(flag)) << flag;
    }
    for (const s32 flag : {72200199, 72200220, 72100201}) {
        EXPECT_NE(RuleFor(flag).owner, FlagOwner::PerPlayer) << flag;
    }
    EXPECT_TRUE(IsLampTravelFlag(72200101));
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
    // Only those and the owned-weapon flags: the flags between them stay the host's.
    for (s32 flag = 6600; flag < 6700; ++flag) {
        const bool held = std::ranges::any_of(
            HeldItemFlags, [&](const HeldItemFlag& item) { return item.flag == flag; });
        const bool owned = std::ranges::any_of(
            OwnedWeaponFlags, [&](const OwnedWeaponFlag& item) { return item.flag == flag; });
        EXPECT_EQ(RuleFor(flag).owner == FlagOwner::PerPlayer, held || owned) << flag;
    }
}

// Kaniel's playtest 2026-10-21: krolebord (level 45) saw Ludwig's Holy Blade, the Uncanny and Lost
// Holy Moonlight Swords, Bowblades and Rakuyo in the Bath shop. ShopLineupParam (01.09) shows a
// weapon once its flag is on: a chalice item lot's getItemFlagId, or a common event's "had this
// weapon" flag (6680-6697, 5500 + slot). As host world flags, the team world put the host's into
// the guest's save. They are each player's own, and so are the chalice lots' taken flags.
TEST(BloodborneFlagRules, OwnedWeaponFlagsAreEachPlayersOwn) {
    for (const OwnedWeaponFlag& owned : OwnedWeaponFlags) {
        EXPECT_EQ(RuleFor(owned.flag).owner, FlagOwner::PerPlayer) << owned.flag;
        EXPECT_TRUE(KeepsLocalFlag(true, owned.flag)) << owned.flag;
        EXPECT_TRUE(CarriesToGuestWorld(owned.flag)) << owned.flag;
        EXPECT_FALSE(IsWorldFlag(owned.flag)) << owned.flag << ": not in a team's world";
        EXPECT_FALSE(TakesHostMapFlag(owned.flag)) << owned.flag;
        EXPECT_FALSE(SharedWithParty(owned.flag)) << owned.flag;
    }
    EXPECT_EQ(OwnedWeaponFlags.size(), 70u);
    // Ludwig's Holy Blade (common event 6680), the Uncanny one (chalice lot 200420000), the
    // Uncanny Holy Moonlight Sword's (event 5500 slot 0, 23010000), Rakuyo (6697).
    const auto weapon_of = [](s32 flag) -> u32 {
        for (const OwnedWeaponFlag& owned : OwnedWeaponFlags) {
            if (owned.flag == flag) {
                return owned.weapon;
            }
        }
        return 0;
    };
    EXPECT_EQ(weapon_of(6680), 9000000u);
    EXPECT_EQ(weapon_of(5220), 9010000u);
    EXPECT_EQ(weapon_of(5500), 23010000u);
    EXPECT_EQ(weapon_of(5521), 38020000u);
    EXPECT_EQ(weapon_of(6697), 19100000u);
    for (const s32 flag : {5000, 5053, 5294, 5299}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::PerPlayer) << flag << ": a chalice lot's";
    }
    for (const s32 flag : {5300, 5499, 5522, 5901, 6603, 6679, 6698}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
    }
}

// The common events check the weapon at any upgrade level: base id + 100 * level, up to +10.
TEST(BloodborneFlagRules, AnyUpgradeOfTheWeaponCounts) {
    const std::vector<u32> held{9000300, 23020000, 7000000};
    EXPECT_TRUE(HoldsWeapon(9000000, held));
    EXPECT_TRUE(HoldsWeapon(23020000, held));
    EXPECT_FALSE(HoldsWeapon(23010000, held)) << "the Lost one, not the Uncanny one";
    EXPECT_FALSE(HoldsWeapon(7010000, held)) << "Uncanny Saw Cleaver, not the Saw Cleaver";
    EXPECT_TRUE(HoldsWeapon(9010000, std::vector<u32>{9011000}));
    EXPECT_FALSE(HoldsWeapon(9010000, std::vector<u32>{9011100}));
    EXPECT_FALSE(HoldsWeapon(9010000, std::vector<u32>{9010050}));
    EXPECT_FALSE(HoldsWeapon(9000000, std::vector<u32>{}));
}

// krolebord's log (2026-10-21): at the join's load in the session his game set 5500-5521 at once.
// Common event 5500 ends unless the local player is a host-type character, and an ended event's
// own flag comes on. Those follow the weapons every second; the others only on the recheck.
TEST(BloodborneFlagRules, APhantomsEndedWeaponEventsAreUndoneAnyTime) {
    for (s32 flag = 5500; flag <= 5521; ++flag) {
        EXPECT_TRUE(RechecksOwnedWeaponFlagAlways(flag)) << flag;
    }
    for (const s32 flag : {5499, 5522, 5100, 5220, 6680, 6697}) {
        EXPECT_FALSE(RechecksOwnedWeaponFlagAlways(flag)) << flag;
    }
}

// An owned-weapon flag in a party comes on once the player holds the weapon; it goes off only on
// the save slot's one recheck (a weapon in the storage box is not seen, so not every second).
TEST(BloodborneFlagRules, OwnedWeaponFlagFollowsThePlayersWeapons) {
    EXPECT_EQ(OwnedWeaponFlagFix(true, false, true, false), std::optional<bool>{true});
    EXPECT_EQ(OwnedWeaponFlagFix(true, false, true, true), std::optional<bool>{true});
    EXPECT_EQ(OwnedWeaponFlagFix(true, true, false, true), std::optional<bool>{false});
    EXPECT_EQ(OwnedWeaponFlagFix(true, true, false, false), std::nullopt) << "after the recheck";
    EXPECT_EQ(OwnedWeaponFlagFix(true, true, true, true), std::nullopt);
    EXPECT_EQ(OwnedWeaponFlagFix(true, false, false, true), std::nullopt);
    EXPECT_EQ(OwnedWeaponFlagFix(false, true, false, true), std::nullopt) << "no party";
    EXPECT_EQ(OwnedWeaponFlagFix(false, false, true, false), std::nullopt) << "no party";
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
    // The flags beside them stay the host's.
    for (const s32 flag : {12105029, 12105035}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
    }
}

// Kaniel 2026-10-21, several players in the Dream: the Doll's "Give" (12100122, on while the
// player holds the Small Hair Ornament) flipped between a player with it and one without, and the
// shop messengers (12105040, risen while this player's shop menu is open: 12105041) rose and
// played their tune again and again. Each player's own, and none of them carried home or shared.
TEST(BloodborneFlagRules, TheDollsGiveAndTheShopMessengersAreEachPlayersOwn) {
    for (const s32 flag : {12100122, 12105040, 12105041}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::PerPlayer) << flag;
        EXPECT_TRUE(KeepsLocalFlag(true, flag)) << flag;
        EXPECT_FALSE(KeepsLocalFlag(false, flag)) << flag;
        EXPECT_FALSE(CarriesToGuestWorld(flag)) << flag;
        EXPECT_FALSE(TakesHostMapFlag(flag)) << flag;
        EXPECT_FALSE(SharedWithParty(flag)) << flag;
    }
    // The insight messenger's (12105043-12105045) and the Doll's other flags stay the host's.
    for (const s32 flag : {12100121, 12100123, 12105042, 12105043, 12105045}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
    }
}

// Kaniel's playtest 2026-10-25: right after the team connected nobody could level up at the Doll.
// Her talk (m21_00 t210304) opens no level-up menu while 6700 is on, and the team's world had
// the host's on (its team_world.pb): every load put it in each guest's save. It is each game's
// own (no team's world, host message or session change sets it), and off as a map loads in a
// party.
TEST(BloodborneFlagRules, TheDollsLevelUpFlagIsEachGamesOwnAndOffAsAMapLoads) {
    EXPECT_EQ(DollLevelUpFlag, 6700);
    EXPECT_EQ(RuleFor(DollLevelUpFlag).owner, FlagOwner::PerPlayer);
    EXPECT_FALSE(IsWorldFlag(DollLevelUpFlag));
    EXPECT_TRUE(KeepsLocalFlag(true, DollLevelUpFlag));
    EXPECT_FALSE(CarriesToGuestWorld(DollLevelUpFlag));
    EXPECT_FALSE(TakesHostMapFlag(DollLevelUpFlag));
    EXPECT_FALSE(SharedWithParty(DollLevelUpFlag));
    EXPECT_TRUE(ClearsDollLevelUpFlag(true, true));
    EXPECT_FALSE(ClearsDollLevelUpFlag(true, false));
    EXPECT_FALSE(ClearsDollLevelUpFlag(false, true));
    // The flags beside it stay the host's.
    for (const s32 flag : {6699, 6701}) {
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

// Kaniel's play test 2026-10-21: a team guest's join reload (back as the host's phantom) gave its
// game a new flag store with no lamp lit (rig: Central Yharnam's 12417810/30/70 on before, none
// after), and every headstone said "no place to awaken". The guest keeps the lamps it had lit
// and lights them again, with the team's world's; a lamp's light request (the flag after its
// active flag) and other flags are no lamps.
TEST(BloodborneFlagRules, AGuestLightsItsOwnAndTheTeamsLampsAgain) {
    const std::set<s32> before{12417810, 12417830, 12417870, 12105030, 12410999};
    const auto was_on = [&](s32 flag) { return before.contains(flag); };
    EXPECT_EQ(LitHeadstoneLamps(was_on), (std::vector<s32>{12417810, 12417830, 12417870}));

    const std::set<s32> kept{12417810, 12417830, 12417870};
    const std::vector<s32> team{12407810, 12407811, 12417810, 12410999, 9401};
    const std::set<s32> after{12417830};
    const auto on = [&](s32 flag) { return after.contains(flag); };
    EXPECT_EQ(LampsToRelight(kept, team, on), (std::vector<s32>{12407810, 12417810, 12417870}));
    EXPECT_TRUE(LampsToRelight(kept, team, [](s32) { return true; }).empty());
    EXPECT_TRUE(LampsToRelight({}, {}, on).empty());
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

// An outside player on v1.1.4 (2026-10-23): a party guest never learned Eileen's and Alfred's
// gestures, the host did. Each talk's "gesture get" step (talk scripts 01.09) skips its gesture
// while its flag is on and sets it with the gesture; as world flags, the guest's copy was the
// host's. Each of them is the player's own, kept for its own world.
TEST(BloodborneFlagRules, GestureFlagsAreEachPlayersOwn) {
    // Every talk's gesture step: (gesture, its flag).
    const std::vector<std::pair<s32, s32>> talks{
        {4, 6744},  {5, 6745},  {6, 6746},  {7, 6747},  {7, 70000007}, {9, 6749},
        {10, 6750}, {11, 6751}, {13, 6753}, {14, 6754}, {15, 6755},    {16, 6756},
        {17, 6757}, {18, 6758}, {19, 6759}, {20, 6760}, {22, 6770},
    };
    std::vector<std::pair<s32, s32>> table;
    for (const GestureFlag& gesture : GestureFlags) {
        table.emplace_back(gesture.gesture, gesture.flag);
    }
    EXPECT_EQ(table, talks);
    for (const GestureFlag& gesture : GestureFlags) {
        EXPECT_EQ(RuleFor(gesture.flag).owner, FlagOwner::PerPlayer) << gesture.flag;
        EXPECT_TRUE(KeepsLocalFlag(true, gesture.flag)) << gesture.flag;
        EXPECT_TRUE(CarriesToGuestWorld(gesture.flag)) << gesture.flag;
        EXPECT_FALSE(TakesHostMapFlag(gesture.flag)) << gesture.flag;
        EXPECT_FALSE(SharedWithParty(gesture.flag)) << gesture.flag;
    }
    // The flags around them stay the host's.
    for (const s32 flag : {6743, 6748, 6752, 6761, 6769, 6771, 70000006, 70000008}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
    }
}

// A gesture's flag says this player learned it: on without the gesture (a save that took the
// host's value from a team world), it goes off, so the NPC's talk gives the gesture again. A
// gesture learned with its flag off stays so: two talks give Church Bow (Female), each by its own
// flag, and the other one's step must still run.
TEST(BloodborneFlagRules, GestureFlagGoesOffWithoutTheGesture) {
    EXPECT_EQ(GestureFlagFix(true, true, false), std::optional<bool>{false});
    EXPECT_EQ(GestureFlagFix(true, false, true), std::nullopt);
    EXPECT_EQ(GestureFlagFix(true, true, true), std::nullopt);
    EXPECT_EQ(GestureFlagFix(true, false, false), std::nullopt);
    EXPECT_EQ(GestureFlagFix(false, true, false), std::nullopt);
}

// Kaniel's playtest 2026-10-25: the voice in the Forbidden Woods house gave its Tonsil Stone to the
// host only. Every Tonsil Stone giver's talk (talk scripts 01.09) gives the stone (goods 4310) on
// the player's first talk with it ("初回か": its own flag off), sets that flag, and skips the gift
// while the player holds the stone. As world flags, the host's first talk put the flag into the
// guests' games before theirs (krolebord's log: "the host's story steps followed here: 73200355"),
// so their talks took the "talked before" line. Each is the player's own, kept for its own world.
TEST(BloodborneFlagRules, TonsilStoneTalkFlagsAreEachPlayersOwn) {
    // Each giver's talk: its first-talk flag (t241350 ... t270375, and the talks that share them).
    const std::vector<s32> talks{
        73200330, 73200331, 73200332, 73200333, 73200334, 73200335, 73200340, 73200341, 73200342,
        73200343, 73200344, 73200350, 73200351, 73200352, 73200353, 73200354, 73200355,
    };
    std::vector<s32> table;
    for (const GiftTalkFlag& talk : GiftTalkFlags) {
        table.push_back(talk.flag);
        EXPECT_EQ(talk.goods, 4310u) << talk.flag;
        EXPECT_EQ(RuleFor(talk.flag).owner, FlagOwner::PerPlayer) << talk.flag;
        EXPECT_TRUE(KeepsLocalFlag(true, talk.flag)) << talk.flag;
        EXPECT_TRUE(CarriesToGuestWorld(talk.flag)) << talk.flag;
        EXPECT_FALSE(TakesHostMapFlag(talk.flag)) << talk.flag;
        EXPECT_FALSE(SharedWithParty(talk.flag)) << talk.flag;
    }
    EXPECT_EQ(table, talks);
    // The flags around them stay the host's.
    for (const s32 flag : {73200329, 73200336, 73200339, 73200345, 73200349, 73200356}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
    }
}

// A giver's first-talk flag says this player had the stone from it: on while the player holds no
// Tonsil Stone (a save that took the host's value from a team world), it goes off, so the talk
// gives the stone. On with the stone it stays: the talk would not give it again anyway.
TEST(BloodborneFlagRules, TonsilStoneTalkFlagGoesOffWithoutTheStone) {
    EXPECT_EQ(GiftTalkFlagFix(true, true, false), std::optional<bool>{false});
    EXPECT_EQ(GiftTalkFlagFix(true, true, true), std::nullopt);
    EXPECT_EQ(GiftTalkFlagFix(true, false, true), std::nullopt);
    EXPECT_EQ(GiftTalkFlagFix(true, false, false), std::nullopt);
    EXPECT_EQ(GiftTalkFlagFix(false, true, false), std::nullopt);
}

// Kaniel's playtest 2026-10-25: Valtr's rune reached Lisara (she talked) and the host, not
// krolebord. His own talk set the gift's trigger and the emulator gave him the item lot, but the
// lot (43120) gives nothing while its getItemFlagId (6676) is on, and the host's 6676 had come
// over from the team world a moment before. A guest's own NPC gift goes out with its lot's flag
// off when that flag is the host's world (Valtr's 43120 and 43130: 6676, 6678); a flag of the
// guest's own (its pickup list, a per-player flag) says it has the item and stays.
TEST(BloodborneFlagRules, AGuestsNpcGiftIgnoresTheHostsTakenFlag) {
    EXPECT_TRUE(ClearsNpcGiftLotFlag(6676, true));
    EXPECT_TRUE(ClearsNpcGiftLotFlag(6678, true));
    EXPECT_FALSE(ClearsNpcGiftLotFlag(6676, false));
    // The other gifts' lots: none, or a map pickup flag (the guest's own list).
    for (const s32 flag : {-1, 0, 50000200, 50001700, 50002000, 50002500}) {
        EXPECT_FALSE(ClearsNpcGiftLotFlag(flag, true)) << flag;
    }
    // A per-player flag is this player's own.
    EXPECT_FALSE(ClearsNpcGiftLotFlag(6645, true));
    EXPECT_FALSE(ClearsNpcGiftLotFlag(73200355, true));
}

// The gesture list's slots (PlayerGameData +0x5B8, 30 at +0x10), as talk command 66 (0x02042AD1)
// reads them: bit 0 learned, the bits above it a row of the gesture table (0x014EC980: row 0 is
// none, row k gesture k - 1, its goods 800 + k - 1).
TEST(BloodborneFlagRules, GestureSlotsAsTheTalkReadsThem) {
    EXPECT_EQ(DecodeGestureSlot(0x14), (GestureSlot{9, false})) << "Pray, row 10";
    EXPECT_EQ(DecodeGestureSlot(0x15), (GestureSlot{9, true}));
    EXPECT_EQ(DecodeGestureSlot(0x02), (GestureSlot{0, false}));
    EXPECT_EQ(DecodeGestureSlot(0x2F), (GestureSlot{22, true})) << "League Oath, row 23";
    EXPECT_EQ(DecodeGestureSlot(0x3D), (GestureSlot{29, true})) << "the last row, 30";
    EXPECT_EQ(DecodeGestureSlot(0x00010015), (GestureSlot{9, true})) << "the high half aside";
    EXPECT_EQ(DecodeGestureSlot(0x00), std::nullopt) << "row 0";
    EXPECT_EQ(DecodeGestureSlot(0x01), std::nullopt);
    EXPECT_EQ(DecodeGestureSlot(0x3E), std::nullopt) << "past the table";
    EXPECT_EQ(DecodeGestureSlot(0x8000), std::nullopt) << "a negative row";
    EXPECT_EQ(DecodeGestureSlot(0xFFFFFFFF), std::nullopt);
    EXPECT_EQ(DecodeGestureSlot(0xFFFF0014), std::nullopt);
    EXPECT_EQ(DecodeGestureSlot(0x0000FFFE), std::nullopt);
}

// Team altars (Kaniel 2026-10-17): a ritual at any member's altar puts its dungeon on every
// member's altar. Ritual altar k keeps its dungeon in event flags 94k00000-94k05999 (a record
// packed into the flags, and switches such as "a chalice on it", 94k05001): the world's, in the
// team world as the host has them, and a ritual's record goes to the team whole, never flag by
// flag (Kaniel's playtest 2026-10-24: the diffs merged the records). The current altar's view
// (92900000-92905999) is each game's own: neither in the team world nor shared.
TEST(BloodborneFlagRules, RitualAltarsAreTheTeams) {
    for (s32 altar = 0; altar < 10; ++altar) {
        const s32 base = 94000000 + altar * 100000;
        for (const s32 flag :
             {base, base + 31, base + 5000, base + 5001, base + 5500, base + 5999}) {
            EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
            EXPECT_FALSE(SharedWithParty(flag)) << flag;
            EXPECT_TRUE(IsWorldFlag(flag)) << flag;
            EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
        }
    }
    for (const s32 flag : {92900000, 92900031, 92905001, 92905999}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Session) << flag;
        EXPECT_FALSE(SharedWithParty(flag)) << flag;
        EXPECT_FALSE(IsWorldFlag(flag)) << flag;
        EXPECT_FALSE(CarriesToGuestWorld(flag)) << flag;
    }
    EXPECT_NE(RuleFor(92906000).owner, FlagOwner::Session);
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

// Kaniel's playtest 2026-10-28 (lamps lit as the party entered Central Pthumeru, krolebord's
// door open and no boss room): a chalice map's flags (12900000-12909999, its items 52900000-
// 52909999) are the current altar's view, the game keeps them in altar k's (14k0xxxx, 54k0xxxx).
// The view is each game's own: as the host's world it carried an earlier dungeon's lamps, and the
// host's view went over a teammate's on another altar. The altars' own flags are the world, the
// lamps' shared, as any map's lamps.
TEST(BloodborneFlagRules, AChaliceMapsFlagsAreTheCurrentAltarsView) {
    for (const s32 flag : {12900000, 12901800, 12904700, 12906824, 12907810, 12907811, 12907830,
                           12909999, 52900000, 52905000, 52909999}) {
        EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Session) << flag;
        EXPECT_FALSE(SharedWithParty(flag)) << flag;
        EXPECT_FALSE(IsWorldFlag(flag)) << flag;
        EXPECT_FALSE(CarriesToGuestWorld(flag)) << flag;
        EXPECT_FALSE(TakesHostMapFlag(flag)) << flag;
    }
    EXPECT_FALSE(LampOfActiveFlag(12907810) && SharedWithParty(12907810));
    EXPECT_TRUE(TakesHostMapFlag(12899999));
    EXPECT_TRUE(TakesHostMapFlag(12910000));
    EXPECT_TRUE(SharedWithParty(12917810)) << "another map's lamp";
    EXPECT_EQ(RuleFor(52910000).owner, FlagOwner::Host);
    EXPECT_TRUE(CarriesToGuestWorld(52910000));
    for (s32 altar = 0; altar < 7; ++altar) {
        const s32 base = 14000000 + altar * 100000;
        for (const s32 flag : {base + 7810, base + 7811, base + 7830, base + 7970}) {
            EXPECT_TRUE(SharedWithParty(flag)) << flag << ": an altar's dungeon's lamp";
            EXPECT_TRUE(IsWorldFlag(flag)) << flag;
            EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
        }
        for (const s32 flag : {base, base + 1800, base + 7210, base + 7812, base + 9999}) {
            EXPECT_EQ(RuleFor(flag).owner, FlagOwner::Host) << flag;
            EXPECT_TRUE(IsWorldFlag(flag)) << flag;
            EXPECT_TRUE(CarriesToGuestWorld(flag)) << flag;
        }
    }
    EXPECT_FALSE(SharedWithParty(14707810)) << "no altar 7";
}

// The team's lamps lit again at a guest's load and each second are the shared ones: the chalice
// map's view lamps were an earlier dungeon's (Kaniel's playtest 2026-10-28: 7 lamps lit as the
// party entered).
TEST(BloodborneFlagRules, TheChaliceViewsLampsAreNotLitAgain) {
    const std::set<s32> kept{12907810, 12417810};
    const std::vector<s32> team{12907830, 12907831, 12407810};
    EXPECT_EQ(LampsToRelight(kept, team, [](s32) { return false; }),
              (std::vector<s32>{12407810, 12417810}));
    WorldFlags world;
    world.blocks = {12407, 12907};
    world.on = {12407810, 12907810, 12907811};
    EXPECT_EQ(TeamLampFlagsToSet(world, [](s32) { return false; }), (std::vector<s32>{12407810}));
    EXPECT_EQ(TeamWorldLamps(1000, std::vector<u32>{12407, 12907}, world),
              (std::vector<s32>{2401950}));
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
