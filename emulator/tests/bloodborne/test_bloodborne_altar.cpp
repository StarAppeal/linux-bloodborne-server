// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/bloodborne_altar.h"
#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

namespace {

AltarRecord RecordStarting(std::initializer_list<u8> bytes) {
    AltarRecord record{};
    std::ranges::copy(bytes, record.begin());
    return record;
}

// Lisara's story Pthumeru Chalice record (1D 0A 5A 00, ChannelId 2000000010) and the one the
// team world held after the merges (Kaniel's playtest 2026-10-24).
const AltarRecord Pthumeru = RecordStarting({0x1D, 0x0A, 0x5A, 0x00, 0x77, 0x35, 0x94, 0x0A});
const AltarRecord Merged = RecordStarting({0x1D, 0x0F, 0xFF, 0xFF, 0x77, 0x35, 0x94, 0x0A});

// The rig's altar-record-capture (2026-10-24): the Pthumeru Chalice ritual's record (altar 1, the
// same after the ritual, in the dungeon and back in the Dream) and an untouched altar's; and
// Kaniel's altar 6 in the team world after the merges (team_world.pb, 2026-10-24).
constexpr std::string_view CapturedPthumeru =
    "1d0a5a007735940a000000001d0a5a00000017dd0097370effffffff00973713000000230097372200000032ffffff"
    "ff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff746e75"
    "48"
    "000072650000000000000000746e754800007265000000000000000002003000000000063273686300000063000000"
    "00"
    "0000000000000000000000000000000000000000001837df001837df00000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0060000000000000000000000008000000000000000000000000800000000000000000000000080000000100020000"
    "00"
    "0000000000000000000000000000000800000000000000000000000000000000000000000000000000000000000000"
    "00"
    "000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view CapturedEmpty =
    "ffffffff0000000080000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000002003000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view KanielsAltar6 =
    "1d0fffff7735940a80000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    "ff"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff746e75"
    "48"
    "000072650000000000000000746e754800007265000000000000000002003000000000063273686300000063000000"
    "00"
    "0000000000000000000000000000000000000000001837df001837df00000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00"
    "0060000000000000000000000008000000000000000000000000800000000000000000000000080000000100020000"
    "00"
    "0000000000000000000000000000000800000000000000000000000000000000000000000000000000000000000000"
    "00"
    "000000000000000000000000000000000000000000000000000000000000";

AltarRecord FromHex(std::string_view hex) {
    AltarRecord record{};
    for (size_t byte = 0; byte < record.size(); ++byte) {
        record[byte] =
            static_cast<u8>(std::stoi(std::string{hex.substr(byte * 2, 2)}, nullptr, 16));
    }
    return record;
}

bool FlagOn(const AltarRecord& record, s32 flag) {
    return (record[flag / 8] & (0x80 >> (flag % 8))) != 0;
}

u32 Word(const AltarRecord& record, size_t at) {
    return u32{record[at]} << 24 | u32{record[at + 1]} << 16 | u32{record[at + 2]} << 8 |
           record[at + 3];
}

// The capture's record with the Pthumeru Chalice's own feature slots (HolygrailExParam 6109:
// slot 2 995010110, code 90; slot 5 994500000, code 45) and their flags (5378, 5345) for the root
// features the old server gave it (codes 0x23 and 0x32, flags 5335 and 5350).
AltarRecord WithPthumeruFeatures(AltarRecord record) {
    for (size_t at = 20; at < 92; ++at) {
        record[at] = 0xFF;
    }
    const auto put = [&record](size_t at, u32 value) {
        for (size_t byte = 0; byte < 4; ++byte) {
            record[at + byte] = static_cast<u8>(value >> (24 - 8 * byte));
        }
    };
    put(20 + 2 * 8, 995'010'110);
    put(24 + 2 * 8, 90);
    put(20 + 5 * 8, 994'500'000);
    put(24 + 5 * 8, 45);
    for (const s32 flag : {5335, 5350}) {
        record[flag / 8] &= static_cast<u8>(~(0x80 >> (flag % 8)));
    }
    for (const s32 flag : {5345, 5378}) {
        record[flag / 8] |= static_cast<u8>(0x80 >> (flag % 8));
    }
    return record;
}

} // namespace

TEST(BloodborneAltar, TheRigsRecordsAreTheBuiltOnes) {
    EXPECT_EQ(EmptyAltarRecord(), FromHex(CapturedEmpty));
    EXPECT_EQ(StandardAltarRecord(10), WithPthumeruFeatures(FromHex(CapturedPthumeru)));
    EXPECT_EQ(ClassifyAltarRecord(FromHex(CapturedPthumeru)), AltarRecordKind::Dungeon);
    EXPECT_EQ(ClassifyAltarRecord(FromHex(CapturedEmpty)), AltarRecordKind::Empty);
}

// Kaniel's playtest 2026-10-24: his altar 6 (1D 0F FF FF) crashed every member's game; repaired
// it is the ritual's record again, its state kept.
TEST(BloodborneAltar, KanielsAltarIsTheRitualsAgain) {
    const AltarRecord broken = FromHex(KanielsAltar6);
    EXPECT_EQ(ClassifyAltarRecord(broken), AltarRecordKind::Broken);
    const AltarRepair repair = RepairAltarRecord(broken);
    ASSERT_EQ(repair.action, AltarRepair::Action::Rebuild);
    EXPECT_EQ(repair.record, WithPthumeruFeatures(FromHex(CapturedPthumeru)));
}

// Kaniel's playtest 2026-10-25: the team beat the story Pthumeru Chalice's last boss and nobody got
// the next chalice. Its reward (m29 event 12901693) gives one only with flag 5385 or 5378 of the
// current altar on; a ritual sets 5300 + each feature slot's DungeonFeatureParam flag, and the
// old server's slots were root ones (5335, 5350). The game's own slots for each story chalice
// (HolygrailExParam) set 5378, or 5385 for 6309's dungeon.
TEST(BloodborneAltar, EveryStoryChaliceCanGiveTheNextChalice) {
    for (const u8 key : {10, 20, 21, 30, 31, 40, 42, 50, 52, 53}) {
        const auto record = StandardAltarRecord(key);
        ASSERT_TRUE(record) << int{key};
        EXPECT_TRUE(FlagOn(*record, 5378) || FlagOn(*record, 5385)) << int{key};
        EXPECT_FALSE(FlagOn(*record, 5335)) << int{key} << ": a root feature";
        EXPECT_FALSE(FlagOn(*record, 5350)) << int{key} << ": a root feature";
        // Slot 5 of every story chalice: 994500000, code 45.
        EXPECT_EQ(Word(*record, 20 + 5 * 8), 994'500'000u) << int{key};
        EXPECT_EQ(Word(*record, 24 + 5 * 8), 45u) << int{key};
    }
    EXPECT_TRUE(FlagOn(*StandardAltarRecord(30), 5385));
    EXPECT_FALSE(FlagOn(*StandardAltarRecord(10), 5385));
}

// The altars that hold a story dungeon from the old server get the chalice's own features, the
// rest of the record (its dungeon, its layers' state) kept: the next load into it gives the boss's
// chalice.
TEST(BloodborneAltar, AnOldStoryDungeonGetsItsChalicesFeatures) {
    const AltarRecord old = FromHex(CapturedPthumeru);
    const AltarRepair repair = RepairAltarRecord(old);
    ASSERT_EQ(repair.action, AltarRepair::Action::Features);
    EXPECT_EQ(repair.record, WithPthumeruFeatures(old));
    EXPECT_EQ(repair.chalice, std::nullopt);
    EXPECT_EQ(RepairAltarRecord(repair.record).action, AltarRepair::Action::None);
    // A root dungeon of the same key (the layout before #310) keeps its features.
    AltarRecord root = old;
    root[2] = 0;
    EXPECT_EQ(RepairAltarRecord(root).action, AltarRepair::Action::None);
    // A stored dungeon's (no standard ChannelId) too.
    AltarRecord stored = old;
    stored[7] = 17;
    EXPECT_EQ(RepairAltarRecord(stored).action, AltarRepair::Action::None);
}

TEST(BloodborneAltar, AnAltarsFlagsAreItsRecord) {
    EXPECT_EQ(AltarFlagBase(0), 94000000);
    EXPECT_EQ(AltarFlagBase(6), 94600000);
    EXPECT_EQ(AltarOfFlag(94600000), 6);
    EXPECT_EQ(AltarOfFlag(94605999), 6);
    EXPECT_EQ(AltarOfFlag(94006000), std::nullopt);
    EXPECT_EQ(AltarOfFlag(94700000), std::nullopt) << "the Dream has seven altars";
    EXPECT_EQ(AltarOfFlag(92900000), std::nullopt) << "the view is no altar's";
}

TEST(BloodborneAltar, TheFirstWordNamesTheDungeon) {
    EXPECT_EQ(AltarRecordDungeon(Pthumeru), (AltarDungeon{0x0A, 90, 0}));
    EXPECT_EQ(AltarRecordDungeon(RecordStarting({0xFF, 0xFF, 0xFF, 0xFF})), std::nullopt);
    EXPECT_EQ(AltarRecordDungeon(std::span<const u8>{}), std::nullopt);
}

// The chalice maps in the game's files (dvdroot_ps4/map/mapstudio, patch 1.09): each standard
// key's story variant 90 and its root variants, layouts 00-99 for a root dungeon.
TEST(BloodborneAltar, OnlyTheGamesDungeonsExist) {
    EXPECT_TRUE(ChaliceDungeonExists({10, 90, 0}));
    EXPECT_TRUE(ChaliceDungeonExists({10, 90, 1}));
    EXPECT_FALSE(ChaliceDungeonExists({10, 90, 2}));
    EXPECT_TRUE(ChaliceDungeonExists({10, 0, 99}));
    EXPECT_TRUE(ChaliceDungeonExists({40, 11, 37}));
    EXPECT_FALSE(ChaliceDungeonExists({40, 0, 0})) << "no m29_40_00_00";
    EXPECT_TRUE(ChaliceDungeonExists({53, 21, 5}));
    EXPECT_TRUE(ChaliceDungeonExists({21, 90, 0}));
    EXPECT_FALSE(ChaliceDungeonExists({21, 90, 1}));
    EXPECT_FALSE(ChaliceDungeonExists({15, 0xFF, 0xFF})) << "the merged record's";
    EXPECT_FALSE(ChaliceDungeonExists({1, 0, 0})) << "m29_01_*: no chalice's";
    EXPECT_FALSE(ChaliceDungeonExists({10, 0, 100}));
}

TEST(BloodborneAltar, ARecordIsEmptyADungeonOrBroken) {
    EXPECT_EQ(ClassifyAltarRecord(Pthumeru), AltarRecordKind::Dungeon);
    EXPECT_EQ(ClassifyAltarRecord(Merged), AltarRecordKind::Broken);
    EXPECT_EQ(ClassifyAltarRecord(RecordStarting({0xFF, 0xFF, 0xFF, 0xFF})),
              AltarRecordKind::Empty);
    EXPECT_EQ(ClassifyAltarRecord(RecordStarting({0x1D, 0x0A, 0x5A, 0x03})),
              AltarRecordKind::Broken);
    EXPECT_EQ(ClassifyAltarRecord(RecordStarting({0x00, 0x0A, 0x5A, 0x00})),
              AltarRecordKind::Broken);
    EXPECT_EQ(ClassifyAltarRecord(std::span<const u8>{Pthumeru}.first(3)), AltarRecordKind::Broken);
}

TEST(BloodborneAltar, AStandardChalicesChannelNamesItsKey) {
    EXPECT_EQ(StandardChannelKey(Pthumeru), 10);
    EXPECT_EQ(StandardChannelKey(Merged), 10) << "the second word was not merged";
    // 2000000053 (Isz), 2000000011 (no chalice), a stored dungeon's 17.
    EXPECT_EQ(StandardChannelKey(RecordStarting({0x1D, 0x35, 0x5A, 0, 0x77, 0x35, 0x94, 0x35})),
              53);
    EXPECT_EQ(StandardChannelKey(RecordStarting({0x1D, 0x0B, 0x5A, 0, 0x77, 0x35, 0x94, 0x0B})),
              std::nullopt);
    EXPECT_EQ(StandardChannelKey(RecordStarting({0x1D, 0x0A, 0, 0, 0, 0, 0, 17})), std::nullopt);
}

// A record goes whole: every flag of the altar, on and off, so the other altar ends exactly so.
TEST(BloodborneAltar, ARecordGoesWhole) {
    const auto changes = AltarRecordChanges(6, Pthumeru);
    ASSERT_EQ(changes.size(), static_cast<size_t>(AltarRecordFlags));
    EXPECT_EQ(changes.front(), (std::pair<s32, bool>{94600000, false}));
    // 0x1D = 00011101: flags 3, 4, 5, 7 on.
    for (s32 bit = 0; bit < 8; ++bit) {
        EXPECT_EQ(changes[bit].second, bit == 3 || bit == 4 || bit == 5 || bit == 7) << bit;
    }
    // 0x0A, the key: 12 and 14.
    EXPECT_EQ(changes[12], (std::pair<s32, bool>{94600012, true}));
    EXPECT_EQ(changes[13], (std::pair<s32, bool>{94600013, false}));
    EXPECT_EQ(changes.back(), (std::pair<s32, bool>{94605999, false}));
    EXPECT_TRUE(std::ranges::is_sorted(changes));
}

// Kaniel's playtest 2026-10-24: each load into a dungeon (and the team world's sync) changed the
// view back to an older record and that went to the team. Only a ritual's or a join's: in the
// Dream, playing, settled.
TEST(BloodborneAltar, OnlyARitualInTheDreamShares) {
    const AltarShareFrame ritual{.playing = true,
                                 .map = HuntersDreamMap,
                                 .since_place_ms = 40'000,
                                 .since_own_write_ms = 1'000'000};
    EXPECT_TRUE(SharesAltarRecord(ritual));
    AltarShareFrame frame = ritual;
    frame.playing = false;
    EXPECT_FALSE(SharesAltarRecord(frame)) << "loading";
    frame = ritual;
    frame.map = 0x1d000000;
    EXPECT_FALSE(SharesAltarRecord(frame)) << "in a chalice dungeon";
    frame = ritual;
    frame.since_place_ms = AltarShareSettleMs - 1;
    EXPECT_FALSE(SharesAltarRecord(frame)) << "just loaded into the Dream";
    frame = ritual;
    frame.since_own_write_ms = AltarOwnWriteQuietMs - 1;
    EXPECT_FALSE(SharesAltarRecord(frame)) << "this game just wrote the team's record";
}

// Kaniel 2026-10-24: the altars the merges broke are repaired as a save loads.
TEST(BloodborneAltar, ABrokenStandardRecordIsMadeAgain) {
    const AltarRepair repair = RepairAltarRecord(Merged);
    ASSERT_EQ(repair.action, AltarRepair::Action::Rebuild);
    EXPECT_EQ(std::optional{repair.record}, StandardAltarRecord(10, Merged));
    EXPECT_EQ(ClassifyAltarRecord(repair.record), AltarRecordKind::Dungeon);
    EXPECT_EQ(repair.chalice, std::nullopt);
}

TEST(BloodborneAltar, AnotherBrokenRecordIsCleared) {
    // A root dungeon's (a stored ChannelId) with the key merged; its chalice (FormData's goods,
    // byte 16: the Pthumeru Root Chalice 6100) goes back.
    const AltarRecord root = RecordStarting(
        {0x1D, 0x0F, 0x00, 0x07, 0, 0, 0, 17, 0, 0, 0, 0, 0x1D, 0x0A, 0, 7, 0, 0, 0x17, 0xD4});
    AltarRepair repair = RepairAltarRecord(root);
    ASSERT_EQ(repair.action, AltarRepair::Action::Clear);
    EXPECT_EQ(repair.record, EmptyAltarRecord());
    EXPECT_EQ(repair.chalice, 6100u);
    // Its goods merged too: nothing to give.
    repair =
        RepairAltarRecord(RecordStarting({0x1D, 0x0A, 0x00, 0xFF, 0,    0,    0,    17,   0,   0, 0,
                                          0,    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    ASSERT_EQ(repair.action, AltarRepair::Action::Clear);
    EXPECT_EQ(repair.chalice, std::nullopt);
    // A standard chalice's ChannelId whose key is not one: the record says nothing to rebuild.
    repair = RepairAltarRecord(RecordStarting({0x1D, 0x0F, 0xFF, 0xFF, 0x77, 0x35, 0x94, 0x0F}));
    ASSERT_EQ(repair.action, AltarRepair::Action::Clear);
}

TEST(BloodborneAltar, GoodRecordsStayAsTheyAre) {
    EXPECT_EQ(RepairAltarRecord(EmptyAltarRecord()).action, AltarRepair::Action::None);
    for (const u8 key : {10, 20, 21, 30, 31, 40, 42, 50, 52, 53}) {
        const auto record = StandardAltarRecord(key);
        ASSERT_TRUE(record) << int{key};
        EXPECT_EQ(ClassifyAltarRecord(*record), AltarRecordKind::Dungeon) << int{key};
        EXPECT_EQ(StandardChannelKey(*record), key);
        EXPECT_EQ(RepairAltarRecord(*record).action, AltarRepair::Action::None) << int{key};
    }
    EXPECT_EQ(StandardAltarRecord(11), std::nullopt);
    EXPECT_EQ(ClassifyAltarRecord(EmptyAltarRecord()), AltarRecordKind::Empty);
}

// Kaniel's playtest 2026-10-28 (krolebord saw an open door and no boss room in Central Pthumeru):
// a chalice map's flags are the current altar's, and a load builds the altar its save's flags
// 9020-9026 name (9020 + altar, the game's own read: the 7 flags as one value, 9020 its top bit).
TEST(BloodborneAltar, TheAltarPickFlagsNameOneAltar) {
    EXPECT_EQ(AltarOfPickBits(0b1000000), 0);
    EXPECT_EQ(AltarOfPickBits(0b0000010), 5);
    EXPECT_EQ(AltarOfPickBits(0b0000001), 6);
    EXPECT_EQ(AltarOfPickBits(0), std::nullopt) << "no altar picked yet";
    EXPECT_EQ(AltarOfPickBits(0b0000011), std::nullopt) << "two altars: not the game's own";
    EXPECT_EQ(AltarOfPickBits(0b10000000), std::nullopt) << "past the 7 flags";
    for (s32 altar = 0; altar < RitualAltarCount; ++altar) {
        EXPECT_EQ(AltarOfPickBits(AltarPickBits(altar)), altar);
    }
    EXPECT_EQ(AltarPickedIn(std::vector<s32>{2010, 9025, 12100509}), 5);
    EXPECT_EQ(AltarPickedIn(std::vector<s32>{9020}), 0);
    EXPECT_EQ(AltarPickedIn(std::vector<s32>{9025, 9026}), std::nullopt);
    EXPECT_EQ(AltarPickedIn(std::vector<s32>{9019, 9027}), std::nullopt);
    EXPECT_EQ(AltarOfWarpRequest(72100420), 0);
    EXPECT_EQ(AltarOfWarpRequest(72100425), 5);
    EXPECT_EQ(AltarOfWarpRequest(72100426), 6);
    EXPECT_EQ(AltarOfWarpRequest(72100427), std::nullopt);
    EXPECT_EQ(AltarOfWarpRequest(72100419), std::nullopt);
    EXPECT_EQ(AltarOfWarpRequest(72100300), std::nullopt) << "a layer pick, not an altar";
}

// krolebord picked altar 5 (his own talk, then Kaniel's warp), and the party trip's load built
// altar 6, the one his save on disk still named: the load builds the altar picked last.
TEST(BloodborneAltar, ALoadBuildsTheAltarPickedLast) {
    ChaliceAltarPick pick;
    EXPECT_EQ(pick.ForLoad(6), std::nullopt) << "nothing picked: the save's own";
    pick.ObserveOwn(6); // the first look after a load: what the save had, no pick
    EXPECT_EQ(pick.ForLoad(6), std::nullopt);
    pick.ObserveOwn(6);
    pick.ObserveOwn(5); // this player's talk with altar 5
    EXPECT_EQ(pick.ForLoad(6), 5);
    EXPECT_EQ(pick.ForLoad(5), std::nullopt) << "the save names it already";
    pick.Warps(2); // another member's warp into altar 2's dungeon, which the trip follows
    EXPECT_EQ(pick.ForLoad(5), 2);
}

// A guest whose save names altar 6 joins a host on altar 5, then enters altar 6's dungeon itself:
// its talk changes no pick flag (altar 6 is its current one), its warp request does.
TEST(BloodborneAltar, ThisPlayersOwnWarpIsAPick) {
    ChaliceAltarPick pick;
    pick.ObserveOwn(6);
    pick.ObserveTeam(5);
    EXPECT_EQ(pick.ForLoad(6), 5);
    pick.Warps(6);
    EXPECT_EQ(pick.ForLoad(6), std::nullopt);
}

TEST(BloodborneAltar, TheTeamHostsAltarIsAPickWhenItChanges) {
    ChaliceAltarPick pick;
    pick.ObserveTeam(5); // a join: the host's altar is the one its dungeon is
    EXPECT_EQ(pick.ForLoad(6), 5);
    pick.ObserveOwn(5);
    pick.ObserveOwn(3);  // this player's own altar after
    pick.ObserveTeam(5); // the same host altar again: no new pick
    EXPECT_EQ(pick.ForLoad(5), 3);
    pick.ObserveTeam(4);
    EXPECT_EQ(pick.ForLoad(3), 4);
    pick.ObserveTeam(std::nullopt); // a team world without one: no pick
    EXPECT_EQ(pick.ForLoad(3), 4);
}

TEST(BloodborneAltar, ALoadsOwnAltarIsNoPick) {
    ChaliceAltarPick pick;
    pick.ObserveOwn(5);
    pick.ObserveOwn(4);
    EXPECT_EQ(pick.ForLoad(6), 4);
    pick.Loaded(); // the next load's save names altar 6: seen first, not picked
    pick.ObserveOwn(6);
    EXPECT_EQ(pick.ForLoad(6), 4);
    pick.ObserveOwn(std::nullopt); // flags not readable: nothing changes
    pick.ObserveOwn(6);
    EXPECT_EQ(pick.ForLoad(6), 4);
    pick.Reset(); // another session or character
    EXPECT_EQ(pick.ForLoad(6), std::nullopt);
}
