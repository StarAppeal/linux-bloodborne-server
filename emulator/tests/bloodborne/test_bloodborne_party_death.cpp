// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Party death, after Elden Ring Nightreign: a lethal hit puts a party member down instead of
// killing it, the others revive it by dealing it damage, and a map where nobody is left standing is
// wiped. The rules here are plain functions; the hooks that use them are in the emulator.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

namespace {

const std::string Kaniel = "4b616e69656c";
const std::string Lisara = "4c6973617261";
const std::string Third = "7468697264";
constexpr u32 CentralYharnam = 0x18010000;
constexpr u32 HuntersDream = 0x15000000;

MemberVitals Member(const std::string& member, u32 map, Vital vital, u32 down = 0,
                    LampPosition at = {}) {
    return MemberVitals{.member = member,
                        .map = map,
                        .vital = vital,
                        .hp = vital == Vital::Down ? 1 : 900,
                        .max = 1000,
                        .down = down,
                        .at = at};
}

} // namespace

// The setting: "1" on, "0" off, unset or empty the build's default (the port's is on, Kaniel
// 2026-10-10; the emulator's off, it has no party HUD).
TEST(BloodbornePartyDeath, FollowsTheSettingOrTheBuildsDefault) {
    for (const bool by_default : {false, true}) {
        EXPECT_TRUE(PartyDeathEnabled("1", by_default));
        EXPECT_FALSE(PartyDeathEnabled("0", by_default));
        EXPECT_EQ(PartyDeathEnabled(nullptr, by_default), by_default);
        EXPECT_EQ(PartyDeathEnabled("", by_default), by_default);
    }
}

// IsNoDead's callers by return address: the hit, special effect and fall damage checks may put
// the player down; the game's kills stay real deaths; anything else is left alone.
TEST(BloodbornePartyDeath, KnowsWhichLethalChecksPutThePlayerDown) {
    EXPECT_EQ(ClassifyNoDeadCheck(0x01A2BF7D), NoDeadCheck::Lethal);  // hit damage
    EXPECT_EQ(ClassifyNoDeadCheck(0x01A2FDD4), NoDeadCheck::Lethal);  // poison, frenzy
    EXPECT_EQ(ClassifyNoDeadCheck(0x01A35E10), NoDeadCheck::Lethal);  // a fall
    EXPECT_EQ(ClassifyNoDeadCheck(0x018BF9EE), NoDeadCheck::Instant); // an event's kill
    EXPECT_EQ(ClassifyNoDeadCheck(0x018BCD9C), NoDeadCheck::Instant); // HP set to 0
    EXPECT_EQ(ClassifyNoDeadCheck(0x01A2FCD8), NoDeadCheck::Other);   // asks, kills nobody
    EXPECT_EQ(ClassifyNoDeadCheck(0), NoDeadCheck::Other);
    for (const NoDeadCaller& caller : NoDeadCallers()) {
        EXPECT_EQ(ClassifyNoDeadCheck(caller.return_offset), caller.check);
    }
}

// The hit's second check (0x01A2C05F) is asked after every hit, whatever HP is left: only a hit
// that left 0 HP or less is lethal there (the rig: any enemy hit put a full HP player down).
TEST(BloodbornePartyDeath, TheCheckAfterEveryHitIsLethalOnlyWithNoHpLeft) {
    EXPECT_EQ(ClassifyNoDeadCheck(0x01A2C05F), NoDeadCheck::AfterHit);
    EXPECT_EQ(NoDeadCheckAt(NoDeadCheck::AfterHit, 594), NoDeadCheck::Other);
    EXPECT_EQ(NoDeadCheckAt(NoDeadCheck::AfterHit, 1), NoDeadCheck::Other);
    EXPECT_EQ(NoDeadCheckAt(NoDeadCheck::AfterHit, 0), NoDeadCheck::Lethal);
    EXPECT_EQ(NoDeadCheckAt(NoDeadCheck::AfterHit, -40), NoDeadCheck::Lethal);
    // The others are asked only when HP would run out, so HP does not change them.
    EXPECT_EQ(NoDeadCheckAt(NoDeadCheck::Lethal, 594), NoDeadCheck::Lethal);
    EXPECT_EQ(NoDeadCheckAt(NoDeadCheck::Instant, 594), NoDeadCheck::Instant);
    EXPECT_EQ(NoDeadCheckAt(NoDeadCheck::Other, 0), NoDeadCheck::Other);
}

TEST(BloodbornePartyDeath, KeepsTheDownedOrArmedPlayerAliveOnlyAtLethalChecks) {
    // Armed (another member stands on the map): a lethal hit leaves 1 HP.
    EXPECT_TRUE(KeepsPlayerAlive(NoDeadCheck::Lethal, true, false, false));
    // Down: nothing lethal finishes the player off.
    EXPECT_TRUE(KeepsPlayerAlive(NoDeadCheck::Lethal, false, true, false));
    // The game's kills and other questions get the game's own answer.
    EXPECT_FALSE(KeepsPlayerAlive(NoDeadCheck::Instant, true, true, false));
    EXPECT_FALSE(KeepsPlayerAlive(NoDeadCheck::Other, true, true, false));
    EXPECT_TRUE(KeepsPlayerAlive(NoDeadCheck::Other, false, false, true));
    // Not armed and not down: today's death.
    EXPECT_FALSE(KeepsPlayerAlive(NoDeadCheck::Lethal, false, false, false));
    EXPECT_TRUE(KeepsPlayerAlive(NoDeadCheck::Lethal, false, false, true));
}

// A revive takes damage, not a number of hits: the downed player's max HP the first time, twice
// that the second, four times from the third, so a stronger weapon revives sooner.
TEST(BloodbornePartyDeath, NeedsMoreDamageForEachDown) {
    EXPECT_EQ(ReviveDamageNeeded(1, 594), 594u);
    EXPECT_EQ(ReviveDamageNeeded(2, 594), 1188u);
    EXPECT_EQ(ReviveDamageNeeded(3, 594), 2376u);
    EXPECT_EQ(ReviveDamageNeeded(9, 594), 2376u);
    EXPECT_EQ(ReviveDamageNeeded(0, 594), 594u);
    EXPECT_EQ(ReviveDamageNeeded(1, 0), 1u); // a max HP not read yet still needs a hit
}

// A revive hit deals the striker's attack rating (the menu's, for the weapon in its right hand),
// half again for R2; at least 1.
TEST(BloodbornePartyDeath, AReviveHitDealsTheWeaponsAttackRating) {
    EXPECT_EQ(ReviveHitDamage(300, false), 300u);
    EXPECT_EQ(ReviveHitDamage(300, true), 450u);
    EXPECT_EQ(ReviveHitDamage(0, false), 1u);
    EXPECT_EQ(ReviveHitDamage(-5, true), 1u);
}

// A real hit counts what it dealt, but never less than the weapon's attack rating.
TEST(BloodbornePartyDeath, ARealHitCountsAtLeastTheAttackRating) {
    EXPECT_EQ(RealReviveHitDamage(373, 300, false), 373u);
    EXPECT_EQ(RealReviveHitDamage(3, 25, false), 25u) << "bare fists";
    EXPECT_EQ(RealReviveHitDamage(3, 25, true), 37u);
    EXPECT_EQ(RealReviveHitDamage(0, 0, false), 1u);
}

TEST(BloodbornePartyDeath, AHeavyAttackIsR2GoingDownWithoutR1) {
    EXPECT_TRUE(HeavyAttackPressed(0, PadR2));
    EXPECT_FALSE(HeavyAttackPressed(0, PadR1));
    EXPECT_FALSE(HeavyAttackPressed(0, PadR1 | PadR2));
    EXPECT_FALSE(HeavyAttackPressed(PadR2, PadR2)) << "held, not pressed";
}

// The panel's ring and text show the share of the damage dealt.
TEST(BloodbornePartyDeath, ShowsReviveProgressAsAShare) {
    EXPECT_FLOAT_EQ(ReviveShare(297, 594), 0.5f);
    EXPECT_FLOAT_EQ(ReviveShare(0, 594), 0.0f);
    EXPECT_FLOAT_EQ(ReviveShare(700, 594), 1.0f);
    EXPECT_FLOAT_EQ(ReviveShare(5, 0), 0.0f);
}

TEST(BloodbornePartyDeath, GetsUpWithHalfItsHp) {
    EXPECT_EQ(RevivedHp(1520), 760);
    EXPECT_EQ(RevivedHp(3), 1);
    EXPECT_EQ(RevivedHp(1), 1);
    EXPECT_EQ(RevivedHp(0), 1);
}

// While down, HP shows the revive's progress: 1 with nothing landed, rising with the damage dealt,
// and never the full bar before the revive is done (the game's own HP bar and the nameplate over
// the player on the others' screens fill as it is struck).
TEST(BloodbornePartyDeath, DownedHpShowsTheReviveProgress) {
    EXPECT_EQ(DownedHp(1000, 0, 1000), 1);
    EXPECT_EQ(DownedHp(1000, 500, 1000), 500);
    EXPECT_EQ(DownedHp(1000, 640, 2000), 320);
    EXPECT_EQ(DownedHp(1000, 999, 1000), 999);
    EXPECT_EQ(DownedHp(1000, 1000, 1000), 999) << "the revive itself sets the HP";
    EXPECT_EQ(DownedHp(1000, 5000, 1000), 999);
    EXPECT_EQ(DownedHp(2, 1, 2), 1);
    EXPECT_EQ(DownedHp(1, 1, 1), 1);
    EXPECT_EQ(DownedHp(0, 3, 4), 1);
    EXPECT_EQ(DownedHp(1000, 7, 0), 1);
    EXPECT_EQ(DownedHp(2'000'000'000, 1'999'999'999, 2'000'000'000), 1'999'999'999)
        << "no overflow";
}

// The game's HUD strings (a remote player's nameplate name, TagData +0x60): UTF-16 kept in the
// struct while it is shorter than 8 (capacity +0x20 below 8), else behind the pointer at +0x08.
TEST(BloodbornePartyHud, ReadsTheGamesHudString) {
    std::array<u8, GameHudStringSize> inline_name{};
    const char16_t shortname[] = u"rig_1";
    std::memcpy(inline_name.data() + 0x08, shortname, sizeof(shortname));
    const u64 small = 7;
    std::memcpy(inline_name.data() + 0x20, &small, sizeof(small));
    EXPECT_EQ(GameHudString(inline_name, [](u64, size_t) { return std::vector<char16_t>{}; }),
              "rig_1");

    std::array<u8, GameHudStringSize> long_name{};
    const u64 pointer = 0x1234'5000, capacity = 15;
    std::memcpy(long_name.data() + 0x08, &pointer, sizeof(pointer));
    std::memcpy(long_name.data() + 0x20, &capacity, sizeof(capacity));
    const auto read = [&](u64 at, size_t count) {
        EXPECT_EQ(at, pointer);
        std::u16string text = u"rig_guest2";
        text.resize(count, u'\0');
        return std::vector<char16_t>(text.begin(), text.end());
    };
    EXPECT_EQ(GameHudString(long_name, read), "rig_guest2");
    // Characters outside ASCII are kept as '?' (online ids are ASCII; this only names the tag).
    const char16_t accented[] = u"h\u00e9";
    std::memcpy(inline_name.data() + 0x08, accented, sizeof(accented));
    EXPECT_EQ(GameHudString(inline_name, read), "h?");
}

// The nameplate over a downed member says so, with its revive, in the tag's own red; the others
// keep the game's role line.
TEST(BloodbornePartyHud, TheNameplateOfADownedMemberShowsItsRevive) {
    PartyPanel panel;
    panel.members.push_back(PanelMember{.member = "a",
                                        .name = "rig_guest",
                                        .vital = Vital::Down,
                                        .here = true,
                                        .landed = 640,
                                        .needed = 1000});
    panel.members.push_back(
        PanelMember{.member = "b", .name = "rig_host", .vital = Vital::Up, .here = true});
    EXPECT_EQ(NameplateRoleLine(panel, "rig_guest"), "<font color=\"#e74141\">Downed 64%</font>");
    EXPECT_EQ(NameplateRoleLine(panel, "rig_host"), std::nullopt);
    EXPECT_EQ(NameplateRoleLine(panel, "someone"), std::nullopt);
    EXPECT_EQ(NameplateRoleLine(panel, ""), std::nullopt);
    panel.members[0].name = "";
    EXPECT_EQ(NameplateRoleLine(panel, "a"), "<font color=\"#e74141\">Downed 64%</font>")
        << "a member without an online id goes by its board name";
}

// The native party list: the first three members as the game's own nameplate tags, under the
// game's quick items: HP while up, the revive while down, the role line saying what is going on.
TEST(BloodbornePartyHud, ThePartyListShowsEachMembersState) {
    PartyPanel panel;
    panel.members.push_back(PanelMember{.member = "a",
                                        .name = "rig_host",
                                        .vital = Vital::Up,
                                        .here = true,
                                        .hp = 400,
                                        .max = 800});
    panel.members.push_back(PanelMember{.member = "b",
                                        .name = "rig_guest",
                                        .vital = Vital::Down,
                                        .here = true,
                                        .hp = 300,
                                        .max = 800,
                                        .landed = 640,
                                        .needed = 1000});
    panel.members.push_back(
        PanelMember{.member = "c", .name = "", .vital = Vital::Dead, .here = true, .max = 900});
    panel.members.push_back(PanelMember{
        .member = "d", .name = "far", .vital = Vital::Up, .here = false, .hp = 10, .max = 20});
    const auto entries = PartyListEntries(panel);
    EXPECT_TRUE(entries[0].visible);
    EXPECT_EQ(entries[0].name, "<font color=\"#ece8df\">rig_host</font>") << "in white";
    EXPECT_EQ(entries[0].role, "") << "the icon says the state";
    EXPECT_EQ(entries[0].hp, 400);
    EXPECT_EQ(entries[0].max, 800);
    EXPECT_EQ(entries[1].role, "");
    EXPECT_EQ(entries[1].hp, 64);
    EXPECT_EQ(entries[1].max, 100);
    EXPECT_EQ(entries[2].name, "<font color=\"#ece8df\">c</font>")
        << "no online id: the board name";
    EXPECT_EQ(entries[2].role, "");
    EXPECT_EQ(entries[2].hp, 0);
    EXPECT_EQ(entries[2].max, 900);
    EXPECT_EQ(entries[0].icon, PartyListIconHunter);
    EXPECT_EQ(entries[1].icon, PartyListIconDowned0 + 6) << "64% shows the 60% ring";
    EXPECT_EQ(entries[2].icon, PartyListIconFallen);

    panel.members.erase(panel.members.begin(), panel.members.begin() + 3);
    const auto away = PartyListEntries(panel);
    EXPECT_EQ(away[0].role, "");
    EXPECT_EQ(away[0].icon, PartyListIconElsewhere);
    EXPECT_EQ(away[1].icon, PartyListIconNone);
    EXPECT_FALSE(away[1].visible);
    EXPECT_FALSE(away[2].visible);
    EXPECT_FALSE(PartyListEntries(PartyPanel{})[0].visible);
}

TEST(BloodbornePartyHud, ADownedMembersIconFillsWithItsRevive) {
    PartyPanel panel;
    panel.members.push_back(PanelMember{.member = "a", .vital = Vital::Down, .needed = 1000});
    for (const auto& [landed, frame] :
         std::vector<std::pair<u32, u32>>{{0, PartyListIconDowned0},
                                          {44, PartyListIconDowned0},
                                          {50, PartyListIconDowned0 + 1},
                                          {999, PartyListIconFrames},
                                          {1000, PartyListIconFrames}}) {
        panel.members[0].landed = landed;
        EXPECT_EQ(PartyListEntries(panel)[0].icon, frame) << landed;
    }
}

TEST(BloodbornePartyHud, ThePartyListStacksUnderTheQuickItems) {
    const auto [x0, y0] = PartyListPosition(0);
    const auto [x1, y1] = PartyListPosition(1);
    const auto [x2, y2] = PartyListPosition(2);
    EXPECT_EQ(x0, x1);
    EXPECT_EQ(x1, x2);
    EXPECT_EQ(x0 - 97, 107) << "the bar starts under the player's HP bar (hud_patch.py)";
    EXPECT_GE(y0 - 70, 240) << "the role line clears the game's quick items (1080p stage)";
    EXPECT_EQ(y1 - y0, y2 - y1);
    EXPECT_GE(y1 - y0, 90) << "each tag's role line clears the bar above";
    EXPECT_LE(y2 + 20, 700) << "above the game's notices";
}

// A TagData the game's tag update takes: shown and placed, the HP gauge's values, and the role line
// and name as game HUD strings kept out of place (our buffers).
TEST(BloodbornePartyHud, WritesTheGamesTagData) {
    std::array<u8, HudTagDataSize> data{};
    data.fill(0xCC);
    std::array<u8, HudTagDataSize> game{};
    const u64 role_allocator = 0x7777'0000, name_allocator = 0x8888'0000;
    std::memcpy(game.data() + 0x28 + 0x28, &role_allocator, 8);
    std::memcpy(game.data() + 0x60 + 0x28, &name_allocator, 8);
    game[0x28 + 0x30] = 5;
    game[0x60 + 0x30] = 6;
    const PartyListEntry entry{.visible = true, .hp = 64, .max = 100, .role = "r", .name = "n"};
    WriteTagData(data, game, entry, 160, 330, 0x1111'0000, 0x2222'0000);
    const auto u32_at = [&](size_t at) {
        u32 value;
        std::memcpy(&value, data.data() + at, 4);
        return value;
    };
    const auto u64_at = [&](size_t at) {
        u64 value;
        std::memcpy(&value, data.data() + at, 8);
        return value;
    };
    EXPECT_EQ(data[0], 1);
    EXPECT_EQ(data[1], 1);
    EXPECT_EQ(u32_at(0x04), 160u);
    EXPECT_EQ(u32_at(0x08), 330u);
    EXPECT_EQ(u32_at(0x0C), 64u);
    EXPECT_EQ(u32_at(0x14), 100u);
    EXPECT_EQ(u64_at(0x28 + 0x08), 0x1111'0000u);
    EXPECT_EQ(u64_at(0x28 + 0x18), 1u) << "its length";
    EXPECT_GE(u64_at(0x28 + 0x20), 8u) << "a capacity that says out of place";
    EXPECT_EQ(u64_at(0x60 + 0x08), 0x2222'0000u);
    EXPECT_EQ(u64_at(0x28 + 0x28), role_allocator) << "the game's string allocator, copied";
    EXPECT_EQ(u64_at(0x60 + 0x28), name_allocator);
    EXPECT_EQ(data[0x28 + 0x30], 5);
    EXPECT_EQ(data[0x60 + 0x30], 6);
    EXPECT_EQ(u64_at(0x98), 0u) << "no damage number";
    EXPECT_EQ(data[0xA0], 0);
    WriteTagData(data, game, PartyListEntry{}, 0, 0, 1, 2);
    EXPECT_EQ(data[0], 0) << "hidden";
}

// Which remote player's nameplate the game is updating: its TagData among FeData's seven.
TEST(BloodbornePartyHud, FindsTheRemotePlayersTag) {
    const u64 fe_data = 0x1000'0000;
    EXPECT_EQ(RemoteTagIndex(fe_data + 0xBA8, fe_data), 0);
    EXPECT_EQ(RemoteTagIndex(fe_data + 0xBA8 + 6 * 0xA8, fe_data), 6);
    EXPECT_EQ(RemoteTagIndex(fe_data + 0xBA8 + 7 * 0xA8, fe_data), std::nullopt);
    EXPECT_EQ(RemoteTagIndex(fe_data + 0x668, fe_data), std::nullopt) << "an enemy's tag";
    EXPECT_EQ(RemoteTagIndex(fe_data + 0xBA8 + 4, fe_data), std::nullopt);
}

// A child lookup's PathRef whose value holds a HUD object is linked into the HUD's value list
// until it is released, and the HUD's destructor releases whatever is still there: one kept for
// the HUD's life must be stored where it outlives the HUD, any other released before its storage
// goes (rig wipe3 2026-10-12: the icons' PathRefs died on the stack still linked, and a guest's
// world teardown crashed in the HUD's destructor at eboot+0x1E312).
TEST(BloodbornePartyHud, AReferencedHudValueOutlivesItsStorageOnlyWhenKept) {
    constexpr u32 DisplayObject = 0x4A; // managed (0x40) display object
    EXPECT_EQ(HudValueFate(DisplayObject, false), HudValueKeep::Release);
    EXPECT_EQ(HudValueFate(DisplayObject, true), HudValueKeep::KeepForTheHud);
    EXPECT_EQ(HudValueFate(0, true), HudValueKeep::Drop) << "not found: nothing linked";
    EXPECT_EQ(HudValueFate(0, false), HudValueKeep::Drop);
    EXPECT_EQ(HudValueFate(0x05, true), HudValueKeep::Drop) << "a plain value is not linked";
    EXPECT_EQ(HudValueFate(0x40, false), HudValueKeep::Release) << "linked, whatever it holds";
}

// A real hit on a downed member's body counts what it dealt, every hit (no gap between presses).
TEST(BloodbornePartyDeath, CountsRealHitsOnTheBodyAsReviveDamage) {
    ReviveHits hits;
    hits.Hit("a", 4, 120);
    hits.Hit("a", 4, 80);
    hits.Hit("b", 2, 300);
    hits.Hit("a", 5, 50); // a new down of the same member starts over
    hits.Hit("b", 2, 0);  // a hit for nothing changes nothing
    const std::vector<MemberVitals> board{
        MemberVitals{.member = "a", .vital = Vital::Down, .down = 5},
        MemberVitals{.member = "b", .vital = Vital::Down, .down = 2}};
    EXPECT_EQ(hits.Totals(board), (std::vector<ReviveHitTotal>{{"a", 5, 50}, {"b", 2, 300}}));
}

// On the others' screens a downed member's body takes the invaders' team (Hostile, 21) so they can
// lock on to it and strike it; its own team comes back once it is up, and a copy that left is
// forgotten.
TEST(BloodbornePartyDeath, ADownedBodyTakesTheInvadersTeamUntilItIsUp) {
    DownedTeams teams;
    EXPECT_EQ(teams.Tick({{7, 20}}, {{7, 20}, {8, 20}}),
              (std::vector<CopyTeam>{{7, DownedTeams::Hostile}}));
    EXPECT_EQ(teams.Tick({{7, DownedTeams::Hostile}}, {{7, DownedTeams::Hostile}, {8, 20}}),
              std::vector<CopyTeam>{});
    // The game set it back (a type change): it is taken again, and its own team stays known.
    EXPECT_EQ(teams.Tick({{7, 20}}, {{7, 20}}), (std::vector<CopyTeam>{{7, DownedTeams::Hostile}}));
    EXPECT_EQ(teams.Tick({}, {{7, DownedTeams::Hostile}, {8, 20}}),
              (std::vector<CopyTeam>{{7, 20}}));
    EXPECT_EQ(teams.Tick({}, {{7, 20}}), std::vector<CopyTeam>{}) << "given back once";
    // A host's body (19) gets 19 back; one that left the world is forgotten.
    EXPECT_EQ(teams.Tick({{9, 19}}, {{9, 19}}), (std::vector<CopyTeam>{{9, DownedTeams::Hostile}}));
    EXPECT_EQ(teams.Tick({}, {}), std::vector<CopyTeam>{});
    EXPECT_EQ(teams.Tick({}, {{9, DownedTeams::Hostile}}), std::vector<CopyTeam>{})
        << "a new body under an old handle is not touched";
    // A team of 6 is one the game never changes: left alone.
    EXPECT_EQ(teams.Tick({{10, 6}}, {{10, 6}}), std::vector<CopyTeam>{});
}

TEST(BloodbornePartyDeath, GoesDownWithANewDownEachTime) {
    DownedState state{100};
    EXPECT_EQ(state.Current(), Vital::Up);
    const u32 first = state.GoDown(0, 600);
    EXPECT_EQ(state.Current(), Vital::Down);
    EXPECT_EQ(state.DownId(), first);
    EXPECT_EQ(state.DamageNeeded(), 600u);
    EXPECT_EQ(state.DamageLanded(), 0u);
    state.GetUp(1'000);
    EXPECT_EQ(state.Current(), Vital::Up);
    const u32 second = state.GoDown(2'000, 600);
    EXPECT_NE(second, first);
    EXPECT_EQ(state.DamageNeeded(), 1200u);
    state.GetUp(3'000);
    state.GoDown(4'000, 600);
    EXPECT_EQ(state.DamageNeeded(), 2400u);
}

TEST(BloodbornePartyDeath, IsRevivedByThePartysDamage) {
    DownedState state{1};
    state.GoDown(0, 600);
    EXPECT_FALSE(state.Damage(250, 100));
    EXPECT_EQ(state.DamageLanded(), 250u);
    // The board repeats its total: the same hit is not counted again.
    EXPECT_FALSE(state.Damage(250, 200));
    EXPECT_EQ(state.DamageLanded(), 250u);
    EXPECT_TRUE(state.Damage(700, 300));
    EXPECT_EQ(state.DamageLanded(), 600u);
}

// Progress drains a third of the max HP each second once nobody hit for 3.5 s.
TEST(BloodbornePartyDeath, LosesReviveProgressWhenNobodyHits) {
    DownedState state{1};
    state.GoDown(0, 600);
    state.Damage(450, 1'000);
    state.Tick(4'400);
    EXPECT_EQ(state.DamageLanded(), 450u);
    state.Tick(4'500); // 3.5 s after the last hit
    EXPECT_EQ(state.DamageLanded(), 250u);
    state.Tick(5'400);
    EXPECT_EQ(state.DamageLanded(), 250u);
    state.Tick(5'500);
    EXPECT_EQ(state.DamageLanded(), 50u);
    state.Tick(9'000);
    EXPECT_EQ(state.DamageLanded(), 0u);
    // A new hit counts from there.
    EXPECT_FALSE(state.Damage(750, 9'100));
    EXPECT_EQ(state.DamageLanded(), 300u);
}

TEST(BloodbornePartyDeath, ForgetsOldDownsAfterThreeMinutesUpOrAtALamp) {
    DownedState state{1};
    state.GoDown(0, 600);
    state.GetUp(1'000);
    state.GoDown(1'000 + DownedState::PenaltyResetMs, 600);
    EXPECT_EQ(state.DamageNeeded(), 600u);
    state.GetUp(200'000);
    state.GoDown(201'000, 600);
    EXPECT_EQ(state.DamageNeeded(), 1200u);
    state.GetUp(202'000);
    state.ClearPenalty();
    state.GoDown(203'000, 600);
    EXPECT_EQ(state.DamageNeeded(), 600u);
}

TEST(BloodbornePartyDeath, ARealDeathEndsTheDownAndRespawnsFresh) {
    DownedState state{1};
    state.GoDown(0, 600);
    state.GetUp(1'000);
    state.GoDown(2'000, 600);
    state.Died();
    EXPECT_EQ(state.Current(), Vital::Dead);
    EXPECT_FALSE(state.Damage(10, 2'500));
    state.Alive(10'000);
    EXPECT_EQ(state.Current(), Vital::Up);
    state.GoDown(11'000, 600);
    EXPECT_EQ(state.DamageNeeded(), 600u);
}

TEST(BloodbornePartyDeath, IgnoresHitsWhileUp) {
    DownedState state{1};
    EXPECT_FALSE(state.Damage(5, 0));
    EXPECT_EQ(state.DamageLanded(), 0u);
}

TEST(BloodbornePartyDeath, GivesUpOnlyAfterAFullHold) {
    HoldToConfirm hold{3'000};
    EXPECT_FALSE(hold.Tick(true, 0));
    EXPECT_FALSE(hold.Tick(true, 2'999));
    EXPECT_FLOAT_EQ(hold.Progress(1'500), 0.5f);
    EXPECT_TRUE(hold.Tick(true, 3'000));
    // Once per hold.
    EXPECT_FALSE(hold.Tick(true, 6'000));
    EXPECT_FALSE(hold.Tick(false, 6'100));
    EXPECT_FLOAT_EQ(hold.Progress(6'100), 0.0f);
    // Letting go early starts over.
    EXPECT_FALSE(hold.Tick(true, 7'000));
    EXPECT_FALSE(hold.Tick(false, 9'000));
    EXPECT_FALSE(hold.Tick(true, 9'500));
    EXPECT_FALSE(hold.Tick(true, 12'000));
    EXPECT_TRUE(hold.Tick(true, 12'500));
}

TEST(BloodbornePartyDeath, CountsAttackPressesAsReviveHits) {
    ReviveHits hits;
    EXPECT_TRUE(hits.Press(Lisara, 4, 300, 0));
    // Faster than an attack: one swing.
    EXPECT_FALSE(hits.Press(Lisara, 4, 300, ReviveHits::GapMs - 1));
    EXPECT_TRUE(hits.Press(Lisara, 4, 450, ReviveHits::GapMs));
    EXPECT_TRUE(hits.Press(Third, 2, 300, ReviveHits::GapMs * 2));
    const std::vector<MemberVitals> board{Member(Lisara, CentralYharnam, Vital::Down, 4),
                                          Member(Third, CentralYharnam, Vital::Down, 2)};
    EXPECT_EQ(hits.Totals(board), (std::vector<ReviveHitTotal>{{Lisara, 4, 750}, {Third, 2, 300}}));
    // A down that ended (revived, or a new one) is dropped.
    const std::vector<MemberVitals> later{Member(Lisara, CentralYharnam, Vital::Up, 4),
                                          Member(Third, CentralYharnam, Vital::Down, 3)};
    EXPECT_TRUE(hits.Totals(later).empty());
    EXPECT_TRUE(hits.Press(Third, 3, 300, ReviveHits::GapMs * 3));
    EXPECT_EQ(hits.Totals(later), (std::vector<ReviveHitTotal>{{Third, 3, 300}}));
}

TEST(BloodbornePartyDeath, RevivesTheNearestDownedMemberInReach) {
    const std::vector<MemberVitals> board{
        Member(Kaniel, CentralYharnam, Vital::Up, 0, {0.0f, 0.0f, 0.0f}),
        Member(Lisara, CentralYharnam, Vital::Down, 4, {2.0f, 0.0f, 0.0f}),
        Member(Third, CentralYharnam, Vital::Down, 2, {1.0f, 0.5f, 0.0f}),
    };
    const LampPosition me{0.0f, 0.0f, 0.0f};
    const auto target = ReviveTarget(board, Kaniel, CentralYharnam, me);
    ASSERT_TRUE(target);
    EXPECT_EQ(target->member, Third);
    EXPECT_EQ(target->down, 2u);
    // Out of reach, on another map, standing, or this player itself: nobody.
    EXPECT_FALSE(ReviveTarget(board, Kaniel, CentralYharnam, LampPosition{10.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(ReviveTarget(board, Kaniel, HuntersDream, me));
    const std::vector<MemberVitals> standing{
        Member(Lisara, CentralYharnam, Vital::Up, 4, {1.0f, 0.0f, 0.0f})};
    EXPECT_FALSE(ReviveTarget(standing, Kaniel, CentralYharnam, me));
    const std::vector<MemberVitals> self{
        Member(Kaniel, CentralYharnam, Vital::Down, 4, {0.0f, 0.0f, 0.0f})};
    EXPECT_FALSE(ReviveTarget(self, Kaniel, CentralYharnam, me));
}

TEST(BloodbornePartyDeath, GoesDownOnlyWithSomeoneElseStandingOnTheMap) {
    EXPECT_TRUE(SomeoneElseStands(std::vector{Member(Lisara, CentralYharnam, Vital::Up)}, Kaniel,
                                  CentralYharnam));
    EXPECT_FALSE(SomeoneElseStands(std::vector{Member(Lisara, CentralYharnam, Vital::Down, 1)},
                                   Kaniel, CentralYharnam));
    EXPECT_FALSE(SomeoneElseStands(std::vector{Member(Lisara, HuntersDream, Vital::Up)}, Kaniel,
                                   CentralYharnam));
    EXPECT_FALSE(SomeoneElseStands(std::vector{Member(Kaniel, CentralYharnam, Vital::Up)}, Kaniel,
                                   CentralYharnam));
    EXPECT_FALSE(SomeoneElseStands(std::vector{Member(Lisara, CentralYharnam, Vital::Off)}, Kaniel,
                                   CentralYharnam));
    EXPECT_FALSE(SomeoneElseStands(std::vector{Member(Lisara, 0, Vital::Up)}, Kaniel, 0));
    EXPECT_FALSE(SomeoneElseStands({}, Kaniel, CentralYharnam));
}

// Rig 2026-10-12 (run 132915, Kaniel: "the third player just didn't die"): only the members in
// this member's session count. A line from a member out of it (its game restarted, back in its
// own world on the same map; or one that left, until its line expires) is dropped before
// anything reads the board. The session's names unknown (one not found): nothing is dropped.
TEST(BloodbornePartyDeath, OnlyTheSessionsMembersCount) {
    const std::vector<MemberVitals> board{Member(Kaniel, CentralYharnam, Vital::Down, 1),
                                          Member(Lisara, CentralYharnam, Vital::Down, 2),
                                          Member(Third, CentralYharnam, Vital::Up)};
    const auto session = OnlySessionMembers(board, Kaniel, std::vector<std::string>{Lisara});
    EXPECT_EQ(session, (std::vector<MemberVitals>{board[0], board[1]})) << "self stays";
    EXPECT_FALSE(SomeoneElseStands(session, Kaniel, CentralYharnam)) << "a wipe";
    EXPECT_FALSE(SomeoneElseStands(session, Lisara, CentralYharnam));
    EXPECT_EQ(OnlySessionMembers(board, Kaniel, std::nullopt), board);
    EXPECT_EQ(OnlySessionMembers(board, Kaniel, std::vector<std::string>{}),
              (std::vector<MemberVitals>{board[0]}))
        << "alone in the session";

    EXPECT_EQ(SessionBoardNames({std::optional<std::string>{Lisara}, std::optional{Third}}),
              (std::vector<std::string>{Lisara, Third}));
    EXPECT_EQ(SessionBoardNames({std::optional<std::string>{Lisara}, std::nullopt}), std::nullopt);
    EXPECT_EQ(SessionBoardNames({}), std::vector<std::string>{});
}

// The same run: the guest whose game restarted was summoned while the others were going down. It
// posted "up" from its own world for the 7 s of its summon load (matching state 6 already, map
// 0x18010000 like the host's), and the host's lethal hit counted it as standing: down, no wipe.
// A member is in the session's world (and posts its vitals) once it arrived: a host at once, a
// summoned guest once a load ends after its join, or ArrivalLimitMs after it whatever happens.
TEST(BloodbornePartyDeath, ASummonedGuestStandsOnlyOnceItArrived) {
    SessionArrival guest;
    EXPECT_FALSE(guest.Tick(false, false, 3, true, 0)) << "out of the session";
    EXPECT_FALSE(guest.Tick(true, true, 3, true, 1'000)) << "joined: still in its own world";
    EXPECT_FALSE(guest.Tick(true, true, 3, false, 3'000)) << "its summon load";
    EXPECT_TRUE(guest.Tick(true, true, 4, true, 7'900)) << "arrived in the host's world";
    EXPECT_TRUE(guest.Tick(true, true, 5, false, 20'000)) << "a trip later: still in the session";
    EXPECT_FALSE(guest.Tick(false, false, 5, true, 30'000)) << "left";

    SessionArrival host;
    EXPECT_TRUE(host.Tick(true, false, 3, true, 0)) << "a host is in its own world";

    SessionArrival stuck;
    EXPECT_FALSE(stuck.Tick(true, true, 3, true, 0));
    EXPECT_FALSE(stuck.Tick(true, true, 3, true, SessionArrival::LimitMs - 1));
    EXPECT_TRUE(stuck.Tick(true, true, 3, true, SessionArrival::LimitMs))
        << "a join with no load counts after a while";
}

// The rig checks SessionArrival by what each game posts: its vitals are logged when the vital or
// the map changes ("off" to "up" once a summoned guest arrived), not on every post.
TEST(BloodbornePartyDeath, VitalsPostsAreLoggedOnChange) {
    VitalsPostLog log;
    EXPECT_EQ(log.Next(Vital::Off, 0), "posting off (map 0x0)") << "the first post";
    EXPECT_EQ(log.Next(Vital::Off, 0), std::nullopt) << "the same again";
    EXPECT_EQ(log.Next(Vital::Up, CentralYharnam),
              "posting up (map 0x18010000), was off (map 0x0)");
    EXPECT_EQ(log.Next(Vital::Up, CentralYharnam), std::nullopt);
    EXPECT_EQ(log.Next(Vital::Down, CentralYharnam),
              "posting down (map 0x18010000), was up (map 0x18010000)");
    EXPECT_EQ(log.Next(Vital::Down, 0x15000000),
              "posting down (map 0x15000000), was down (map 0x18010000)");
}

TEST(BloodbornePartyDeath, EncodesTheVitalsUpdate) {
    MemberVitals self = Member(Kaniel, CentralYharnam, Vital::Down, 7, {-12.5f, 3.0f, 40.25f});
    self.hp = 1;
    self.max = 1520;
    self.landed = 300;
    self.needed = 1520;
    EXPECT_EQ(EncodeVitalsUpdate(self, {}),
              "4b616e69656c 18010000 down 1 1520 7 300 1520 -12.5 3 40.25");
    const std::vector<ReviveHitTotal> hits{{Lisara, 4, 620}, {Third, 9, 1}};
    EXPECT_EQ(EncodeVitalsUpdate(self, hits),
              "4b616e69656c 18010000 down 1 1520 7 300 1520 -12.5 3 40.25 4c6973617261:4:620 "
              "7468697264:9:1");
}

TEST(BloodbornePartyDeath, ParsesTheVitalsBoard) {
    const auto board =
        ParseVitalsBoard("4b616e69656c 18010000 up 1200 1520 0 0 0 -12.5 3 40.25 0\n"
                         "4c6973617261 18010000 down 1 1380 4 620 1380 -11 3 41 620\n"
                         "wipe 5 15000000 4b616e69656c 7468697264\n");
    ASSERT_TRUE(board);
    ASSERT_EQ(board->members.size(), 2u);
    EXPECT_EQ(board->members[0].member, Kaniel);
    EXPECT_EQ(board->members[0].vital, Vital::Up);
    EXPECT_EQ(board->members[0].hp, 1200);
    EXPECT_EQ(board->members[0].at, (LampPosition{-12.5f, 3.0f, 40.25f}));
    const MemberVitals& lisara = board->members[1];
    EXPECT_EQ(lisara.member, Lisara);
    EXPECT_EQ(lisara.map, CentralYharnam);
    EXPECT_EQ(lisara.vital, Vital::Down);
    EXPECT_EQ(lisara.max, 1380);
    EXPECT_EQ(lisara.down, 4u);
    EXPECT_EQ(lisara.landed, 620u);
    EXPECT_EQ(lisara.needed, 1380u);
    EXPECT_EQ(lisara.revive_damage, 620u);
    ASSERT_EQ(board->wipes.size(), 1u);
    EXPECT_EQ(board->wipes[0].id, 5u);
    EXPECT_EQ(board->wipes[0].map, HuntersDream);
    EXPECT_EQ(board->wipes[0].members, (std::vector<std::string>{Kaniel, Third}));
    EXPECT_EQ(WipeFor(*board, Kaniel), std::optional<u32>{5});
    EXPECT_EQ(WipeFor(*board, Lisara), std::nullopt);

    EXPECT_TRUE(ParseVitalsBoard(""));
    EXPECT_FALSE(ParseVitalsBoard("4b616e69656c 18010000 sleeping 1 1 0 0 0 0 0 0 0\n"));
    EXPECT_FALSE(ParseVitalsBoard("4b616e69656c 18010000 up 1 1 0 0 0 0 0 0\n"));
    EXPECT_FALSE(ParseVitalsBoard("4b616e69656c zz up 1 1 0 0 0 0 0 0 0\n"));
    EXPECT_FALSE(ParseVitalsBoard("wipe x 15000000 4b616e69656c\n"));
    EXPECT_FALSE(ParseVitalsBoard("wipe 5 15000000\n"));
}

TEST(BloodbornePartyDeath, NamesTheVitals) {
    for (const Vital vital : {Vital::Off, Vital::Up, Vital::Down, Vital::Dead}) {
        EXPECT_EQ(ParseVital(VitalName(vital)), std::optional<Vital>{vital});
    }
    EXPECT_EQ(VitalName(Vital::Down), "down");
    EXPECT_EQ(ParseVital("Down"), std::nullopt);
}

// A wipe, or a real death with nobody else standing on the map (a wipe of one), resets the map:
// the respawn travels to the lamp even when it is in a loaded block, so the map reloads as it
// does after a solo death. Otherwise a death is today's party death.
TEST(BloodbornePartyDeath, AResetDeathReloadsAtTheLamp) {
    EXPECT_TRUE(DeathResetsMap(true, true));
    EXPECT_TRUE(DeathResetsMap(false, false));
    EXPECT_FALSE(DeathResetsMap(false, true));
    EXPECT_EQ(ChooseDeathRespawn(2412951, true, false, true), DeathRespawn::Travel);
    EXPECT_EQ(ChooseDeathRespawn(2102962, false, false, true), DeathRespawn::Travel);
    EXPECT_EQ(ChooseDeathRespawn(2412951, true, false, false), DeathRespawn::MoveToPoint);
    EXPECT_EQ(ChooseDeathRespawn(-1, true, false, true), DeathRespawn::StayPut);
    // The arrival after that trip puts the player at the lamp; it never travels again.
    EXPECT_EQ(ChooseDeathRespawn(2412951, true, true, true), DeathRespawn::MoveToPoint);
    EXPECT_EQ(ChooseDeathRespawn(2412951, false, true, true), DeathRespawn::StayPut);
}

// Only the trip of a death that resets the map asks for the save's dead to come back on arrival.
TEST(BloodbornePartyDeath, OnlyAWipeTripRespawnsTheSavesDead) {
    EXPECT_TRUE(WipeTripRespawnsOnArrival(true, DeathRespawn::Travel));
    EXPECT_FALSE(WipeTripRespawnsOnArrival(false, DeathRespawn::Travel)); // a lamp elsewhere
    EXPECT_FALSE(WipeTripRespawnsOnArrival(true, DeathRespawn::StayPut));
    EXPECT_FALSE(WipeTripRespawnsOnArrival(false, DeathRespawn::MoveToPoint));
}

// Rig smoke 2026-10-12: a guest whose game restarted mid-session loaded its own save, where enemy
// 0x1001400b had died; the wipe's trip took it home from the host's world and the enemy stayed at
// 0 HP with no death mark (+0x1E3 0), while the host's and the other guest's copies came back.
TEST(BloodbornePartyDeath, AWipeBringsBackWhatTheMembersSaveLeftDead) {
    const RespawnCandidate save_dead{.hp = 0, .max_hp = 484};
    EXPECT_TRUE(LeftDeadByItsSave(save_dead));
    EXPECT_TRUE(RespawnsInWorldRespawn(WorldRespawnKind::AfterWipe, save_dead, false));
    // Alive, or with no HP to restore: nothing to bring back.
    EXPECT_FALSE(LeftDeadByItsSave({.hp = 484, .max_hp = 484}));
    EXPECT_FALSE(LeftDeadByItsSave({.hp = 0, .max_hp = 0}));
    // Put down by its world (a boss beaten before the session, Father Gascoigne's 0x26) or killed
    // now (0x6 by damage, 0x1 by an event's kill): the scripts and the kill board own those.
    for (const u8 flags : {u8{0x26}, u8{0x20}, u8{0x06}, u8{0x04}, u8{0x02}, u8{0x01}}) {
        EXPECT_FALSE(LeftDeadByItsSave({.hp = 0, .max_hp = 3645, .death_flags = flags}))
            << static_cast<int>(flags);
    }
    // Switched off or hidden by the story: left as the scripts have it.
    EXPECT_FALSE(LeftDeadByItsSave({.hp = 0, .max_hp = 143, .disabled = true}));
    EXPECT_FALSE(LeftDeadByItsSave({.hp = 0, .max_hp = 143, .hidden = true}));
    // Rig smoke 2026-10-12 (run 153353): the restarted guest had hidden 0x1001400b "as the host
    // has them" (hid and switched it off) just before the check, so it was skipped. Changed by
    // the party, not the story: it is still the save's dead.
    EXPECT_TRUE(LeftDeadByItsSave(
        {.hp = 0, .max_hp = 484, .disabled = true, .hidden = true, .changed_by_party = true}));
    // A party rest is unchanged: what the party killed during this visit, nothing from the save.
    EXPECT_FALSE(RespawnsInWorldRespawn(WorldRespawnKind::PartyRest, save_dead, false));
    EXPECT_TRUE(RespawnsInWorldRespawn(WorldRespawnKind::PartyRest,
                                       {.hp = 0, .max_hp = 484, .death_flags = 0x06}, true));
    EXPECT_FALSE(
        RespawnsInWorldRespawn(WorldRespawnKind::PartyRest, {.hp = 70, .max_hp = 70}, true));
    // The wipe's respawn leaves this visit's kills alone (the wipe's reload brought those back).
    EXPECT_FALSE(RespawnsInWorldRespawn(WorldRespawnKind::AfterWipe,
                                        {.hp = 0, .max_hp = 484, .death_flags = 0x06}, true));
}

// A downed player keeps the menu (Options) and the camera: it can't move, attack, roll, heal or
// switch items until it is up.
TEST(BloodbornePartyDeath, ADownedPlayerOnlyKeepsTheMenuAndTheCamera) {
    const PadInput pad{.buttons = PadOptions | PadR1 | PadCircle | AutoplayButton("up") |
                                  AutoplayButton("square"),
                       .sticks = {20, 240, 60, 200},
                       .l2 = 255,
                       .r2 = 128};
    const PadInput kept = DownedPad(pad);
    EXPECT_EQ(kept.buttons, PadOptions);
    EXPECT_EQ(kept.sticks, (std::array<u8, 4>{128, 128, 60, 200}));
    EXPECT_EQ(kept.l2, 0);
    EXPECT_EQ(kept.r2, 0);
}

TEST(BloodbornePartyDeath, AnAttackIsR1OrR2GoingDown) {
    EXPECT_EQ(PadR1, AutoplayButton("r1"));
    EXPECT_EQ(PadR2, AutoplayButton("r2"));
    EXPECT_EQ(PadCircle, AutoplayButton("circle"));
    EXPECT_EQ(PadOptions, AutoplayButton("options"));
    EXPECT_TRUE(AttackPressed(0, PadR1));
    EXPECT_TRUE(AttackPressed(PadCircle, PadCircle | PadR2));
    EXPECT_FALSE(AttackPressed(PadR1, PadR1)) << "held, not pressed";
    EXPECT_FALSE(AttackPressed(PadR1, 0));
    EXPECT_FALSE(AttackPressed(0, PadCircle));
}

// What ends a down, in this order: a wipe of the map, the give-up hold, the party's revive hits,
// and a vitals board not heard from for BoardLostMs (shadNet gone: nobody stays down for good).
TEST(BloodbornePartyDeath, EndsADownByWipeGiveUpReviveOrALostBoard) {
    EXPECT_EQ(EndOfDown(false, false, false, 0), DownEnd::None);
    EXPECT_EQ(EndOfDown(true, true, true, 0), DownEnd::Wipe);
    EXPECT_EQ(EndOfDown(false, true, true, 0), DownEnd::GiveUp);
    EXPECT_EQ(EndOfDown(false, false, true, BoardLostMs), DownEnd::Revive);
    EXPECT_EQ(EndOfDown(false, false, false, BoardLostMs - 1), DownEnd::None);
    EXPECT_EQ(EndOfDown(false, false, false, BoardLostMs), DownEnd::BoardLost);
}

// Circle held from before the down (a sprint) gives up only once it is let go and held again.
TEST(BloodbornePartyDeath, AHoldFromBeforeTheDownDoesNotGiveUp) {
    HoldToConfirm hold{GiveUpHoldMs};
    hold.Restart(true);
    EXPECT_FALSE(hold.Tick(true, 0));
    EXPECT_FALSE(hold.Tick(true, GiveUpHoldMs * 2));
    EXPECT_FLOAT_EQ(hold.Progress(GiveUpHoldMs * 2), 0.0f);
    EXPECT_FALSE(hold.Tick(false, GiveUpHoldMs * 2 + 10));
    EXPECT_FALSE(hold.Tick(true, GiveUpHoldMs * 3));
    EXPECT_TRUE(hold.Tick(true, GiveUpHoldMs * 4));

    HoldToConfirm fresh{GiveUpHoldMs};
    EXPECT_FALSE(fresh.Tick(true, 0));
    fresh.Restart(false);
    EXPECT_FALSE(fresh.Tick(true, 100)) << "the hold starts over";
    EXPECT_FALSE(fresh.Tick(true, GiveUpHoldMs));
    EXPECT_TRUE(fresh.Tick(true, GiveUpHoldMs + 100));
}

// The port's party panel lists the other members playing party death in the board's order, by
// their online ids, and points at the downed one an attack would revive now.
TEST(BloodbornePartyDeath, ListsTheOthersOnThePartyPanel) {
    MemberVitals lisara = Member(Lisara, CentralYharnam, Vital::Down, 4, {1.0f, 0.0f, 0.0f});
    lisara.landed = 2;
    lisara.needed = 3;
    const std::vector<MemberVitals> board{
        Member(Kaniel, CentralYharnam, Vital::Up),    lisara,
        Member(Third, HuntersDream, Vital::Up),       Member("00ff", CentralYharnam, Vital::Dead),
        Member("6f6666", CentralYharnam, Vital::Off),
    };
    const PartyPanel panel = BuildPartyPanel(board, Kaniel, CentralYharnam, LampPosition{});
    ASSERT_EQ(panel.members.size(), 3u);
    EXPECT_EQ(panel.members[0], (PanelMember{.member = Lisara,
                                             .name = "Lisara",
                                             .vital = Vital::Down,
                                             .here = true,
                                             .hp = 1,
                                             .max = 1000,
                                             .landed = 2,
                                             .needed = 3}));
    EXPECT_EQ(panel.members[1].name, "third");
    EXPECT_FALSE(panel.members[1].here);
    EXPECT_EQ(panel.members[2].name, ""); // its board name is not an online id
    EXPECT_EQ(panel.members[2].vital, Vital::Dead);
    EXPECT_EQ(panel.revive_target, 0);
    EXPECT_EQ(panel.vital, Vital::Off); // the local player's own down is the caller's to fill

    EXPECT_EQ(BuildPartyPanel(board, Kaniel, CentralYharnam, LampPosition{9.0f, 0.0f, 0.0f})
                  .revive_target,
              -1);
    EXPECT_EQ(BuildPartyPanel(board, Kaniel, CentralYharnam, std::nullopt).revive_target, -1);
    EXPECT_FALSE(BuildPartyPanel(board, Kaniel, 0, std::nullopt).members[0].here); // loading
    EXPECT_TRUE(BuildPartyPanel({}, Kaniel, CentralYharnam, std::nullopt).members.empty());
}

// Enemies forget a downed player at once: a target node of theirs that is a live character target
// (slot type 1 with the alive bit, kind 1) on a downed player's handle has its memory run out.
TEST(BloodbornePartyDeath, EnemiesForgetADownedPlayersTargetNode) {
    const std::vector<u32> downed{0x10068000, 0x10068001};
    EXPECT_TRUE(ForgetsTargetNode(0x81, 1, 0x10068000, downed));
    EXPECT_TRUE(ForgetsTargetNode(0x01, 1, 0x10068001, downed));
    EXPECT_FALSE(ForgetsTargetNode(0x81, 1, 0x10068002, downed)) << "another target";
    EXPECT_FALSE(ForgetsTargetNode(0x83, 1, 0x10068000, downed)) << "a last-known place";
    EXPECT_FALSE(ForgetsTargetNode(0x81, 0, 0x10068000, downed)) << "not a character";
    EXPECT_FALSE(ForgetsTargetNode(0x81, 1, 0x10068000, {}));
}

// The others down on this map, where the board has them (the game finds their characters there).
TEST(BloodbornePartyDeath, FindsTheOthersDownOnThisMap) {
    const std::vector<MemberVitals> board{
        Member(Kaniel, CentralYharnam, Vital::Down, 1, {1.0f, 2.0f, 3.0f}),
        Member(Lisara, CentralYharnam, Vital::Down, 4, {4.0f, 5.0f, 6.0f}),
        Member(Third, HuntersDream, Vital::Down, 2, {7.0f, 8.0f, 9.0f}),
        Member("00ff", CentralYharnam, Vital::Up, 0, {0.0f, 0.0f, 0.0f}),
        Member("6f6666", 0, Vital::Down, 3, {0.0f, 0.0f, 0.0f}), // loading
    };
    EXPECT_EQ(OthersDownOnMap(board, Kaniel, CentralYharnam),
              (std::vector<LampPosition>{{4.0f, 5.0f, 6.0f}}));
    EXPECT_TRUE(OthersDownOnMap(board, Kaniel, 0).empty());
}
