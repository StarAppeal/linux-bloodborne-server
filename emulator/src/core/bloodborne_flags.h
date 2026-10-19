// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string_view>

#include "common/types.h"

namespace Core::Bloodborne {

// Who decides an event flag's value in a party (see docs/FLAGS.md).
enum class FlagOwner : u8 {
    // The host's world: a guest's copy follows the host in a session.
    Host,
    // Each player's own: the other player's "set this event flag" and "event ended" messages
    // leave it alone (KeepsLocalFlag).
    PerPlayer,
    // The party's: posted to the party's flag board, and every member sets it when one sees it
    // set (SharedWithParty), since the game does not sync it itself.
    Shared,
    // Only this session or map visit: never kept for a guest's own world nor shared.
    Session,
};

// What else the co-op runtime does with a flag (bits of FlagRule::behaviour).
enum FlagBehaviour : u32 {
    // A guest keeps its change in a session for its own world (CarriesToGuestWorld).
    FlagCarriesHome = 1u << 0,
    // A guest loading the host's map takes the host's value (TakesHostMapFlag).
    FlagTakesHostMap = 1u << 1,
    // A map item's pickup: a guest keeps its own pickups in their own list (IsMapPickupFlag).
    FlagMapPickup = 1u << 2,
    // A lamp's "travel to the Hunter's Dream" request (IsLampTravelFlag).
    FlagLampTrip = 1u << 3,
    // A Dream headstone's "travel to <lamp>" request (IsHeadstoneTravelFlag).
    FlagHeadstoneTrip = 1u << 4,
    // A boss fog's "host entered" flag (IsBossFogEntryFlag).
    FlagBossFogEntry = 1u << 5,
    // A guest out in the world in a session sets it for itself (GuestProgressFlags).
    FlagGuestSetsInWorld = 1u << 6,
    // A messenger weapon line cleared once per save slot (ShopLineFlags).
    FlagShopLine = 1u << 7,
    // A Shared flag's clears go to the party too, not only its sets (FlagsToShare).
    FlagSharesClears = 1u << 8,
};

struct FlagRule {
    FlagOwner owner = FlagOwner::Host;
    u32 behaviour = 0;
    constexpr bool Has(u32 bits) const {
        return (behaviour & bits) == bits;
    }
    bool operator==(const FlagRule&) const = default;
};

// Flags first..last (inclusive). With a period, only the flags whose flag % period lies in
// [low, high], and with a stride too, of those only the first `width` of every `stride`.
struct FlagRange {
    s32 first = 0;
    s32 last = 0;
    s32 period = 0;
    s32 low = 0;
    s32 high = 0;
    s32 stride = 0;
    s32 width = 0;
    FlagRule rule{};
    std::string_view why;
    constexpr bool Contains(s32 flag) const {
        if (flag < first || flag > last) {
            return false;
        }
        if (period == 0) {
            return true;
        }
        const s32 within = flag % period;
        if (within < low || within > high) {
            return false;
        }
        return stride == 0 || (within - low) % stride < width;
    }
};

// The flag each boss fog's host event sets and its phantom event waited for (found by
// party_boss_events.py's open_boss_fog_for_guests in the 01.09 map event files). Party guests
// set it too, and it holds for one map visit; the flag after it is "a phantom entered" (local
// music).
inline constexpr std::array<s32, 21> BossFogEntryFlags{
    12104800, 12104850, 12204800, 12304700, 12304800, 12404800, 12414700,
    12414800, 12424700, 12424800, 12504800, 12604800, 12604850, 12704800,
    12804800, 13304800, 13404808, 13404858, 13504808, 13504858, 13604808,
};

// A door's flags: a map event that waits for an object act (5:2: a door, gate, lever, kicked
// ladder) keeps the door open by the object act's own flag, the event's "done" flag (its id plus
// the start's slot: the next load reads it and sets the door open at once) and the flags it sets
// on (Cathedral Ward's gate event 12400760 sets 12400160). tools/re/door_flags.py lists them from
// the 01.09 map event files, leaving out a switch (a flag an event turns back off). The game only
// syncs an object act to players on the same map, so a door a teammate opened elsewhere stayed
// shut for the others (Kaniel 2026-10-19): the party shares them.
inline constexpr std::array<s32, 390> DoorFlags{
    12110000, 12110200, 12200100, 12200101, 12200110, 12200112, 12200122, 12200123, 12200127,
    12200128, 12200310, 12200500, 12200900, 12300120, 12300121, 12300122, 12300180, 12300211,
    12300212, 12300213, 12300214, 12300310, 12300900, 12400070, 12400100, 12400101, 12400102,
    12400103, 12400112, 12400113, 12400114, 12400123, 12400125, 12400126, 12400130, 12400131,
    12400132, 12400133, 12400135, 12400136, 12400137, 12400138, 12400147, 12400148, 12400155,
    12400156, 12400160, 12400161, 12400162, 12400163, 12400164, 12400165, 12400170, 12400172,
    12400173, 12400175, 12400185, 12400190, 12400191, 12400200, 12400350, 12400352, 12400353,
    12400354, 12400355, 12400356, 12400357, 12400358, 12400359, 12400360, 12400450, 12400451,
    12400452, 12400453, 12400454, 12400455, 12400456, 12400457, 12400458, 12400760, 12400824,
    12400825, 12400850, 12400851, 12400852, 12400853, 12400854, 12410112, 12410113, 12410114,
    12410115, 12410116, 12410117, 12410130, 12410140, 12410150, 12410200, 12410202, 12410203,
    12410205, 12410206, 12410285, 12410320, 12410321, 12410322, 12410323, 12410337, 12410601,
    12410602, 12410603, 12410604, 12410605, 12411200, 12411202, 12411203, 12411204, 12411300,
    12411301, 12411302, 12411303, 12411304, 12411306, 12411307, 12411308, 12411309, 12420000,
    12420001, 12420030, 12420032, 12420033, 12420050, 12420051, 12420102, 12420110, 12420111,
    12420112, 12420120, 12420121, 12420122, 12420123, 12420124, 12420126, 12420127, 12420151,
    12420152, 12420850, 12420851, 12420852, 12420853, 12421000, 12421001, 12500031, 12500032,
    12500033, 12500034, 12500035, 12500036, 12500037, 12500076, 12500077, 12500500, 12500501,
    12501200, 12501204, 12501205, 12501208, 12501209, 12501210, 12501211, 12600120, 12600121,
    12600122, 12600123, 12600200, 12600201, 12600202, 12601000, 12601001, 12601002, 12601003,
    12601050, 12601051, 12601211, 12601221, 12601231, 12601241, 12601261, 12601281, 12601291,
    12601320, 12601321, 12601322, 12601323, 12601324, 12601325, 12601330, 12601331, 12601332,
    12601333, 12601334, 12601335, 12601853, 12700010, 12700110, 12700111, 12700112, 12700120,
    12700121, 12700122, 12700123, 12700131, 12700132, 12700141, 12700142, 12700150, 12700171,
    12700172, 12700900, 12705095, 12705300, 12705720, 12705721, 12705722, 12800100, 12800101,
    12800102, 12800103, 12800104, 12800105, 12800201, 12800202, 12800203, 12800206, 12800209,
    12800250, 12800251, 12800252, 12800253, 12800254, 12800255, 12800280, 12800281, 12800480,
    12800481, 12800482, 12800483, 12800484, 12800602, 12800604, 13200010, 13200040, 13200041,
    13200042, 13200043, 13200050, 13200051, 13200053, 13200054, 13200060, 13200062, 13200063,
    13200064, 13200065, 13200066, 13200067, 13200068, 13200069, 13200070, 13200071, 13200072,
    13200073, 13200074, 13200077, 13200078, 13200079, 13200100, 13200120, 13200122, 13200123,
    13200124, 13200200, 13200201, 13200203, 13200204, 13200205, 13200206, 13200207, 13200208,
    13200209, 13200210, 13200211, 13200212, 13200213, 13200214, 13200215, 13200216, 13200217,
    13200218, 13200219, 13200220, 13200221, 13200222, 13200223, 13200224, 13200225, 13200226,
    13200227, 13200228, 13200229, 13200234, 13200235, 13200236, 13200237, 13200238, 13200239,
    13200310, 13200311, 13400000, 13400001, 13400002, 13401210, 13401211, 13401220, 13500020,
    13500021, 13500022, 13500100, 13500420, 13500421, 13500941, 13501102, 13501103, 13501112,
    13501113, 13501122, 13501123, 13501200, 13501201, 13501202, 13501203, 13501204, 13501205,
    13501206, 13501207, 13501208, 13501209, 13501210, 13501211, 13501212, 13501213, 13501250,
    13501400, 13501401, 13501402, 13504101, 13504102, 13504103, 13504111, 13504112, 13504121,
    13504122, 13504220, 13504230, 13504241, 13504242, 13504245, 13504246, 13504261, 13504262,
    13504263, 13504264, 13504265, 13504266, 13504269, 13504270, 13504271, 13504272, 13504273,
    13504274, 13504275, 13504276, 13504280, 13601102, 13601103, 13601200, 13601201, 13604101,
    13604102, 13604200, 13604201,
};

// The Dream's m21_00 event 12100450 sets each of these flags once the player holds its goods (the
// workshop's tools, the hunter badges). The badges' givers read them: a boss event gives its badge
// only while the flag is off (the Cleric Beast's 12411700 its Sword Hunter Badge by 6645, m21_00
// 12101800 by 6642, m23_00 12301700 its Spark Hunter Badge by 6644); common event 9910 marks the
// NPC badge gifts' lots taken by 6641 6643 6647 6649; the maps' start events switch objects by
// 6630 6631 6640 6646 6648.
struct HeldItemFlag {
    s32 flag;
    u32 goods;
};
inline constexpr std::array HeldItemFlags{
    HeldItemFlag{6630, 4103}, // Blood Gem Workshop Tool
    HeldItemFlag{6631, 4104}, // Rune Workshop Tool
    HeldItemFlag{6632, 4105}, // Short Ritual Root Chalice
    HeldItemFlag{6633, 4102}, // Workshop Haze Extractor
    HeldItemFlag{6640, 4110}, // Saw Hunter Badge
    HeldItemFlag{6641, 4112}, // Powder Keg Hunter Badge
    HeldItemFlag{6642, 4113}, // Old Hunter Badge
    HeldItemFlag{6643, 4111}, // Crow Hunter Badge
    HeldItemFlag{6644, 4118}, // Spark Hunter Badge
    HeldItemFlag{6645, 4114}, // Sword Hunter Badge
    HeldItemFlag{6646, 4115}, // Radiant Sword Hunter Badge
    HeldItemFlag{6647, 4116}, // Wheel Hunter Badge
    HeldItemFlag{6648, 4119}, // Cosmic Eye Watcher Badge
    HeldItemFlag{6649, 4117}, // Cainhurst Badge
};

namespace FlagTableDetail {

constexpr FlagRange Range(s32 first, s32 last, FlagOwner owner, u32 behaviour,
                          std::string_view why) {
    return FlagRange{.first = first, .last = last, .rule = {owner, behaviour}, .why = why};
}

// Rows before the boss fogs'.
inline constexpr std::array FirstRows{
    Range(std::numeric_limits<s32>::min(), 0, FlagOwner::Session, 0, "not an event flag"),
    Range(4000, 4999, FlagOwner::Session, 0,
          "death and multiplayer bookkeeping (4000 a boss fight on, 4047 a player on the way "
          "out): the session's own"),
    Range(6630, 6633, FlagOwner::PerPlayer, FlagCarriesHome,
          "holds a workshop tool (HeldItemFlags, the Dream's 12100450): what this player holds"),
    Range(6640, 6649, FlagOwner::PerPlayer, FlagCarriesHome,
          "holds a hunter badge (HeldItemFlags): what this player holds; as the host's (a team "
          "world) a guest never got its own Sword Hunter Badge from the Cleric Beast"),
    Range(9180, 9199, FlagOwner::Session, 0,
          "the bell's bookkeeping (common events 9181-9198), not progress; kept for the "
          "guest's world it gave Insight again"),
    Range(9300, 9307, FlagOwner::PerPlayer, FlagCarriesHome,
          "a boss's \"found\" flag (EncounterInsights): each player gets the Insight for "
          "finding a boss"),
    Range(9336, 9336, FlagOwner::PerPlayer, FlagCarriesHome, "a boss's \"found\" flag"),
    Range(9338, 9344, FlagOwner::PerPlayer, FlagCarriesHome, "a boss's \"found\" flag"),
    Range(9346, 9348, FlagOwner::PerPlayer, FlagCarriesHome, "a boss's \"found\" flag"),
    Range(9401, 9401, FlagOwner::Host, FlagCarriesHome | FlagGuestSetsInWorld,
          "Central Yharnam reached (the Dream's first visit, its shop): a phantom in the "
          "host's world never sets it, so a guest out in the world sets it for itself"),
    Range(9403, 9403, FlagOwner::Host, FlagCarriesHome | FlagGuestSetsInWorld,
          "the workshop's doors open (Central Yharnam's 12410900 sets it for a live player "
          "only): set by a guest out in the world for itself"),
    Range(12101000, 12101009, FlagOwner::PerPlayer, FlagCarriesHome | FlagShopLine,
          "the messengers' weapon lines (Dream event 12101000, slot 0-9 once the player has its "
          "badge; run in each player's own game): each player's follow its own badges; cleared "
          "once per save slot after a broken patch set them all"),
    Range(12101020, 12101029, FlagOwner::PerPlayer, FlagCarriesHome,
          "the Dream messengers' gift events and flags: what one player received is not given "
          "to the other"),
};

// Rows after the boss fogs': the general ranges the earlier rows cut into.
inline constexpr std::array LastRows{
    FlagRange{.first = 12107810,
              .last = 13998191,
              .period = 10000,
              .low = 7810,
              .high = 8191,
              .stride = 20,
              .width = 2,
              .rule = {FlagOwner::Shared, FlagCarriesHome | FlagTakesHostMap},
              .why = "a lamp's active flag (10007810 + map * 10000 + lamp * 20, maps 210-399, "
                     "lamps 0-19) and the one after: the game does not sync them"},
    Range(12000000, 13999999, FlagOwner::Host, FlagCarriesHome | FlagTakesHostMap,
          "the maps' flags (1AABxxxx: switches, bosses, enemies; doors are DoorFlags): the host's "
          "world"),
    Range(50000000, 52899999, FlagOwner::Host, FlagMapPickup,
          "map item pickups (ItemLotParam getItemFlagId 5[0-3]MMxxxx): a guest's own list"),
    Range(52900000, 52999999, FlagOwner::Host, FlagCarriesHome,
          "chalice dungeon item flags: left out of the pickups, kept like any other change"),
    Range(53000000, 53999999, FlagOwner::Host, FlagMapPickup, "map item pickups"),
    Range(72101000, 72101099, FlagOwner::PerPlayer, FlagCarriesHome,
          "the pick in the Dream messengers' talk: each player's own"),
    Range(72102000, 72103999, FlagOwner::Session, FlagHeadstoneTrip,
          "a Dream headstone's \"travel to <lamp>\" (7210MMML for event 12107000): a request, "
          "not progress"),
    FlagRange{.first = 72110000,
              .last = 73999999,
              .period = 10000,
              .low = 100,
              .high = 199,
              .rule = {FlagOwner::Session, FlagLampTrip},
              .why = "a lamp's \"travel to the Hunter's Dream\" (7MMM01LL, map codes 211-399; "
                     "the Dream's 721001xx is the Doll's, 70000140 Gilbert's talk, 799001xx "
                     "the NPCs'): a request, not progress"},
    FlagRange{.first = 94000000,
              .last = 94905999,
              .period = 100000,
              .low = 0,
              .high = 5999,
              .rule = {FlagOwner::Shared, FlagCarriesHome | FlagSharesClears},
              .why = "a ritual altar's dungeon (altar k: 94k00000-94k05999, its record packed "
                     "into the flags, 94k05001 a chalice on it): team altars, a ritual or a join "
                     "at one member's altar puts the dungeon on every member's"},
};

constexpr std::array<FlagRange, BossFogEntryFlags.size() * 2> BossFogRows() {
    std::array<FlagRange, BossFogEntryFlags.size() * 2> rows{};
    for (size_t index = 0; index < BossFogEntryFlags.size(); ++index) {
        const s32 flag = BossFogEntryFlags[index];
        rows[index * 2] =
            Range(flag, flag, FlagOwner::Shared, FlagBossFogEntry,
                  "a boss fog's \"host entered\" flag: a guest's (unsynced) fog event sets it "
                  "to start the boss; holds for one map visit");
        rows[index * 2 + 1] = Range(flag + 1, flag + 1, FlagOwner::Session, 0,
                                    "a boss fog's \"phantom entered\" flag (local music)");
    }
    return rows;
}

constexpr std::array<FlagRange, DoorFlags.size()> DoorRows() {
    std::array<FlagRange, DoorFlags.size()> rows{};
    for (size_t index = 0; index < DoorFlags.size(); ++index) {
        rows[index] = Range(DoorFlags[index], DoorFlags[index], FlagOwner::Shared,
                            FlagCarriesHome | FlagTakesHostMap,
                            "a door's flag (DoorFlags): the game does not sync a door opened on "
                            "another map");
    }
    return rows;
}

constexpr auto MakeFlagTable() {
    constexpr auto fogs = BossFogRows();
    constexpr auto doors = DoorRows();
    std::array<FlagRange, FirstRows.size() + fogs.size() + doors.size() + LastRows.size()> table{};
    size_t at = 0;
    for (const FlagRange& row : FirstRows) {
        table[at++] = row;
    }
    for (const FlagRange& row : fogs) {
        table[at++] = row;
    }
    for (const FlagRange& row : doors) {
        table[at++] = row;
    }
    for (const FlagRange& row : LastRows) {
        table[at++] = row;
    }
    return table;
}

} // namespace FlagTableDetail

// Every event flag's party rule, most specific row first: a flag takes the first row that
// contains it. Rows only overlap when a later row's range holds an earlier one whole (the map
// flags hold the boss fogs, the doors and the lamps' active flags); a test checks that.
inline constexpr auto FlagTable = FlagTableDetail::MakeFlagTable();

// Any other flag: the host's world, kept for the guest's own (quest steps, NPCs, the Dream).
inline constexpr FlagRule EveryOtherFlag{FlagOwner::Host, FlagCarriesHome};

constexpr const FlagRange* RowFor(s32 flag) {
    // The door rows (one flag each, in order) are looked up by a binary search: a map load
    // asks for each of its 10000 flags (ApplyHostMapState).
    constexpr size_t DoorsAt = FlagTableDetail::FirstRows.size() + BossFogEntryFlags.size() * 2;
    constexpr size_t DoorsEnd = DoorsAt + DoorFlags.size();
    for (size_t index = 0; index < DoorsAt; ++index) {
        if (FlagTable[index].Contains(flag)) {
            return &FlagTable[index];
        }
    }
    const auto door = std::ranges::lower_bound(DoorFlags, flag);
    if (door != DoorFlags.end() && *door == flag) {
        return &FlagTable[DoorsAt + static_cast<size_t>(door - DoorFlags.begin())];
    }
    for (size_t index = DoorsEnd; index < FlagTable.size(); ++index) {
        if (FlagTable[index].Contains(flag)) {
            return &FlagTable[index];
        }
    }
    return nullptr;
}

constexpr FlagRule RuleFor(s32 flag) {
    const FlagRange* row = RowFor(flag);
    return row != nullptr ? row->rule : EveryOtherFlag;
}

// The flags of the rows with these behaviour bits (rows without a period), in table order.
constexpr size_t CountFlagsWith(u32 bits) {
    size_t count = 0;
    for (const FlagRange& row : FlagTable) {
        if (row.rule.Has(bits) && row.period == 0) {
            count += static_cast<size_t>(row.last - row.first) + 1;
        }
    }
    return count;
}

template <u32 Bits>
constexpr std::array<s32, CountFlagsWith(Bits)> FlagsWith() {
    std::array<s32, CountFlagsWith(Bits)> flags{};
    size_t at = 0;
    for (const FlagRange& row : FlagTable) {
        if (row.rule.Has(Bits) && row.period == 0) {
            for (s32 flag = row.first; flag <= row.last; ++flag) {
                flags[at++] = flag;
            }
        }
    }
    return flags;
}

} // namespace Core::Bloodborne
