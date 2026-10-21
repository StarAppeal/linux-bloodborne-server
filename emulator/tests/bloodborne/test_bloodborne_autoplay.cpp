// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Autoplay (SHADPS4_BLOODBORNE_AUTOPLAY=<script>): the emulator presses the buttons itself, from
// the title screen into the world, then runs a test script, so a test needs no one at the pad.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

TEST(BloodborneAutoplay, ReadsAScriptOneCommandPerLine) {
    const auto script = ParseAutoplayScript(
        "# the host\nwait-session 2 300\n\n  travel 2102962  \r\nkill nearest\n");
    ASSERT_EQ(script.size(), 3u);
    EXPECT_EQ(script[0], (AutoplayCommand{"wait-session", {"2", "300"}}));
    EXPECT_EQ(script[1], (AutoplayCommand{"travel", {"2102962"}}));
    EXPECT_EQ(script[2], (AutoplayCommand{"kill", {"nearest"}}));
    EXPECT_TRUE(ParseAutoplayScript("").empty());
    EXPECT_TRUE(ParseAutoplayScript("# only a comment\r\n   \n").empty());
}

TEST(BloodborneAutoplay, NamesThePadButtons) {
    EXPECT_EQ(AutoplayButton("cross"), 0x4000u);
    EXPECT_EQ(AutoplayButton("circle"), 0x2000u);
    EXPECT_EQ(AutoplayButton("square"), 0x8000u);
    EXPECT_EQ(AutoplayButton("triangle"), 0x1000u);
    EXPECT_EQ(AutoplayButton("options"), 0x8u);
    EXPECT_EQ(AutoplayButton("up"), 0x10u);
    EXPECT_EQ(AutoplayButton("right"), 0x20u);
    EXPECT_EQ(AutoplayButton("down"), 0x40u);
    EXPECT_EQ(AutoplayButton("left"), 0x80u);
    EXPECT_EQ(AutoplayButton("l1"), 0x400u);
    EXPECT_EQ(AutoplayButton("r1"), 0x800u);
    EXPECT_EQ(AutoplayButton("l2"), 0x100u);
    EXPECT_EQ(AutoplayButton("r2"), 0x200u);
    EXPECT_EQ(AutoplayButton("l3"), 0x2u);
    EXPECT_EQ(AutoplayButton("r3"), 0x4u) << "lock on";
    EXPECT_EQ(AutoplayButton("start"), 0u) << "not a PS4 button";
}

// "Press any button", then Continue (the title menu's first entry): Cross for 150 ms every two
// seconds, from two seconds in, until the player is in the world.
TEST(BloodborneAutoplay, TheTitleScreenGetsCrossNowAndThen) {
    EXPECT_FALSE(TitlePressesCross(0));
    EXPECT_FALSE(TitlePressesCross(1999));
    EXPECT_TRUE(TitlePressesCross(2000));
    EXPECT_TRUE(TitlePressesCross(2149));
    EXPECT_FALSE(TitlePressesCross(2150));
    EXPECT_TRUE(TitlePressesCross(4100));
    EXPECT_FALSE(TitlePressesCross(5000));
}

TEST(BloodborneAutoplay, ReadsTargetsAndComparisons) {
    EXPECT_EQ(ParseAutoplayTarget("nearest"), std::optional<u32>{0}) << "0 means the nearest";
    EXPECT_EQ(ParseAutoplayTarget("self"), std::optional<u32>{AutoplaySelf}) << "the player";
    EXPECT_EQ(ParseAutoplayTarget("marked"), std::optional<u32>{AutoplayMarked})
        << "the one a mark command picked";
    EXPECT_EQ(ParseAutoplayTarget("0x1001409c"), std::optional<u32>{0x1001409c});
    EXPECT_EQ(ParseAutoplayTarget("1001409c"), std::optional<u32>{0x1001409c});
    EXPECT_EQ(ParseAutoplayTarget("zz"), std::nullopt);
    EXPECT_EQ(CompareAutoplay(5, "<=", 5), std::optional<bool>{true});
    EXPECT_EQ(CompareAutoplay(5, "<", 5), std::optional<bool>{false});
    EXPECT_EQ(CompareAutoplay(6, ">", 5), std::optional<bool>{true});
    EXPECT_EQ(CompareAutoplay(5, ">=", 6), std::optional<bool>{false});
    EXPECT_EQ(CompareAutoplay(5, "==", 5), std::optional<bool>{true});
    EXPECT_EQ(CompareAutoplay(5, "!=", 5), std::optional<bool>{false});
    EXPECT_EQ(CompareAutoplay(5, "~", 5), std::nullopt);
}

// attack <target>: a real hit (the kill reward and the death run as in play). The teleport that
// puts the player beside it turns the player towards it (HeadingTowards; a written heading does
// not stay). Each cycle tries the next side, +z first, then +x, -z and -x, in case a wall or the
// enemy's own swing is in the way.
TEST(BloodborneAutoplay, AnAttackCirclesTheTarget) {
    const AttackStance first = AttackStanceFor(10.0f, 20.0f, 0);
    EXPECT_NEAR(first.x, 10.0f, 1e-4f);
    EXPECT_NEAR(first.z, 20.0f + AttackDistance, 1e-4f) << "first on the +z side";
    const AttackStance second = AttackStanceFor(10.0f, 20.0f, 1);
    EXPECT_NEAR(second.x, 10.0f + AttackDistance, 1e-4f) << "then the +x side";
    EXPECT_NEAR(second.z, 20.0f, 1e-4f);
    EXPECT_NEAR(AttackStanceFor(10.0f, 20.0f, 2).z, 20.0f - AttackDistance, 1e-4f);
    EXPECT_NEAR(AttackStanceFor(10.0f, 20.0f, 3).x, 10.0f - AttackDistance, 1e-4f);
    EXPECT_NEAR(AttackStanceFor(10.0f, 20.0f, 4).z, first.z, 1e-4f) << "then all over again";
    EXPECT_NEAR(AttackStanceFor(10.0f, 20.0f, 4).x, first.x, 1e-4f);
}

// A teleport's heading h faces (-sin h, cos h): rig 2026-10-10 by the second Central Yharnam
// lamp, -0.85 from beside the lamp opened the lamp's menu and 2.29 at Gilbert's window his talk.
TEST(BloodborneAutoplay, TheHeadingTowardsAPointFacesIt) {
    EXPECT_NEAR(HeadingTowards(-198.45f, 63.96f, -199.2f, 63.3f), 2.29f, 0.01f) << "the window";
    EXPECT_NEAR(HeadingTowards(-197.6f, 64.7f, -196.8f, 65.4f), -0.85f, 0.01f) << "the lamp";
    EXPECT_NEAR(HeadingTowards(0.0f, 0.0f, 0.0f, 5.0f), 0.0f, 1e-4f) << "+z";
    EXPECT_NEAR(HeadingTowards(0.0f, 0.0f, 5.0f, 0.0f), -1.5708f, 1e-3f) << "+x";
}

TEST(BloodborneAutoplay, AnAttackPressesR1OncePerCycle) {
    EXPECT_EQ(AttackCycleAt(0.0), 0u);
    EXPECT_EQ(AttackCycleAt(AttackCycleSeconds - 0.01), 0u);
    EXPECT_EQ(AttackCycleAt(AttackCycleSeconds), 1u);
    EXPECT_EQ(AttackCycleAt(-1.0), 0u);
    EXPECT_FALSE(AttackHoldsR1(0.0)) << "standing there first";
    EXPECT_FALSE(AttackHoldsR1(0.29));
    EXPECT_TRUE(AttackHoldsR1(0.3));
    EXPECT_TRUE(AttackHoldsR1(0.49));
    EXPECT_FALSE(AttackHoldsR1(0.5)) << "released";
    EXPECT_FALSE(AttackHoldsR1(AttackCycleSeconds - 0.01));
    EXPECT_TRUE(AttackHoldsR1(AttackCycleSeconds + 0.4)) << "again in the next cycle";
}

TEST(BloodborneAutoplay, AShotGoesIntoTheShotsFolderBesideTheRequest) {
    const std::filesystem::path request =
        std::filesystem::path{"rig"} / "port" / "host" / "shot.request";
    EXPECT_EQ(AutoplayShotPath(request.string(), "host-sees-down"),
              (request.parent_path() / "shots" / "host-sees-down.png").string());
    EXPECT_EQ(AutoplayShotPath(request.string(), "Guest_2"),
              (request.parent_path() / "shots" / "Guest_2.png").string());
    EXPECT_EQ(AutoplayShotPath(request.string(), std::string(40, 'a')),
              (request.parent_path() / "shots" / (std::string(40, 'a') + ".png")).string());
    // Only a plain name: never a path of its own, nothing else in the folder.
    for (const char* name : {"", "a b", "../host", "x.png", "a/b", "a\\b", "down!"}) {
        EXPECT_EQ(AutoplayShotPath(request.string(), name), std::nullopt) << name;
    }
    EXPECT_EQ(AutoplayShotPath(request.string(), std::string(41, 'a')), std::nullopt);
    EXPECT_EQ(AutoplayShotPath("", "down"), std::nullopt) << "no request file, no picture";
}

// sync <name>: every game of a rig suite waits there until the rig sees all of them at it and
// writes the go file <the script's folder>/sync/<name> (run_scenario.py).
TEST(BloodborneAutoplay, ASyncWaitsForItsGoFileBesideTheScript) {
    const std::filesystem::path script =
        std::filesystem::path{"rig"} / "port" / "guest" / "autoplay.txt";
    EXPECT_EQ(AutoplaySyncPath(script.string(), "host-back"),
              (script.parent_path() / "sync" / "host-back").string());
    EXPECT_EQ(AutoplaySyncPath(script.string(), "trips_2"),
              (script.parent_path() / "sync" / "trips_2").string());
    for (const char* name : {"", "a b", "../host", "a/b", "a\\b", "go!"}) {
        EXPECT_EQ(AutoplaySyncPath(script.string(), name), std::nullopt) << name;
    }
    EXPECT_EQ(AutoplaySyncPath(script.string(), std::string(41, 'a')), std::nullopt);
    EXPECT_EQ(AutoplaySyncPath("", "go"), std::nullopt) << "no script file, nothing to wait for";
}

// keep-going on: a step that fails gives up its chapter, not the whole suite; the script goes on
// at the next chapter, or ends (failed) when none is left.
TEST(BloodborneAutoplay, AFailedStepGoesOnAtTheNextChapter) {
    const auto script = ParseAutoplayScript("keep-going on\nchapter join\nwait-session 2 300\n"
                                            "chapter trip\ntravel 2102962\nsync back\n"
                                            "chapter wipe\nrise 25\n");
    EXPECT_EQ(NextAutoplayChapter(script, 2), std::optional<size_t>{3});
    EXPECT_EQ(NextAutoplayChapter(script, 4), std::optional<size_t>{6}) << "past its syncs";
    EXPECT_EQ(NextAutoplayChapter(script, 3), std::optional<size_t>{6})
        << "a failed chapter step itself goes on at the next one";
    EXPECT_EQ(NextAutoplayChapter(script, 7), std::nullopt) << "the last chapter";
    EXPECT_EQ(NextAutoplayChapter({}, 0), std::nullopt);
}

// Kaniel 2026-10-15: short rig runs. "chapter <name> <seconds>": a chapter still going after its
// budget fails its step there (keep-going gives the chapter up), instead of holding the suite.
TEST(BloodborneAutoplay, AChapterCarriesItsTimeBudget) {
    const auto script = ParseAutoplayScript("chapter join 120\nchapter trip\nchapter a 0\n"
                                            "chapter b x\nchapter c 90.5\nwait 120\n");
    EXPECT_EQ(AutoplayChapterBudget(script[0]), std::optional<double>{120.0});
    EXPECT_EQ(AutoplayChapterBudget(script[1]), std::nullopt) << "no budget: as before";
    EXPECT_EQ(AutoplayChapterBudget(script[2]), std::nullopt);
    EXPECT_EQ(AutoplayChapterBudget(script[3]), std::nullopt);
    EXPECT_EQ(AutoplayChapterBudget(script[4]), std::optional<double>{90.5});
    EXPECT_EQ(AutoplayChapterBudget(script[5]), std::nullopt) << "not a chapter";
}

// keys <token>... (and title-keys on the title screen): the port's menu keys a scripted test
// presses (BB_MENU_KEYS_FILE), e.g. Insert ("toggle") while the team's load screen is up.
TEST(BloodborneAutoplay, MenuKeysAreTheOverlaysTokens) {
    EXPECT_EQ(AutoplayMenuKeys({"toggle", "r1", "r1", "enter", "back"}),
              std::optional<std::string>{"toggle r1 r1 enter back\n"});
    EXPECT_EQ(AutoplayMenuKeys({"toggle"}), std::optional<std::string>{"toggle\n"});
    EXPECT_EQ(AutoplayMenuKeys({}), std::nullopt) << "nothing to press";
    EXPECT_EQ(AutoplayMenuKeys({"toggle", "insert"}), std::nullopt) << "no such key";
    // Kaniel 2026-10-17, the map screen: M opens it, Circle closes it, Square centres it.
    EXPECT_EQ(AutoplayMenuKeys({"triangle"}), std::optional<std::string>{"triangle\n"})
        << "the whole map";
    EXPECT_EQ(AutoplayMenuKeys({"map", "r1", "circle", "square"}),
              std::optional<std::string>{"map r1 circle square\n"});
    // Kaniel 2026-10-18: R3 switches the map between the roofs and the floors.
    EXPECT_EQ(AutoplayMenuKeys({"r3"}), std::optional<std::string>{"r3\n"}) << "floors or roofs";
    // Kaniel 2026-10-19: L3 turns the map's 3D view on and off.
    EXPECT_EQ(AutoplayMenuKeys({"l3"}), std::optional<std::string>{"l3\n"}) << "the 3D view";
    // Kaniel 2026-10-21: the map's held controls (the game camera's view zoomed, turned, tilted).
    EXPECT_EQ(AutoplayMenuKeys({"hold-zoomout-120", "hold-tiltup-9"}),
              std::optional<std::string>{"hold-zoomout-120 hold-tiltup-9\n"});
    EXPECT_EQ(AutoplayMenuKeys({"hold-jump-5"}), std::nullopt) << "no such control";
    EXPECT_EQ(AutoplayMenuKeys({"hold-zoomin-0"}), std::nullopt) << "no frames";
    EXPECT_EQ(AutoplayMenuKeys({"hold-zoomin-x"}), std::nullopt);
}

// Kaniel 2026-10-12: the hardest bugs come where two maps meet (a fog border crossed back and
// forth). Where a player walked before a map change is logged (PositionTrail): a path the rig
// replays with `glide` to cross that border itself.
TEST(BloodborneAutoplay, ThePlayersTrailKeepsAPointEveryHalfSecond) {
    PositionTrail trail;
    trail.Note(0, {0.0f, 0.0f, 0.0f});
    trail.Note(200, {1.0f, 0.0f, 0.0f}); // too soon: not kept
    trail.Note(500, {2.0f, 0.0f, 0.0f});
    trail.Note(1000, {3.0f, 0.0f, 0.0f});
    EXPECT_EQ(trail.Points(), (std::vector<LampPosition>{{0, 0, 0}, {2, 0, 0}, {3, 0, 0}}));
    for (u64 ms = 1500; ms <= 30000; ms += 500) {
        trail.Note(ms, {static_cast<float>(ms) / 1000.0f, 0.0f, 0.0f});
    }
    ASSERT_EQ(trail.Points().size(), PositionTrail::Kept) << "only the last few seconds";
    EXPECT_EQ(trail.Points().back(), (LampPosition{30.0f, 0.0f, 0.0f}));
    EXPECT_EQ(trail.Points().front(),
              (LampPosition{30.0f - 0.5f * (PositionTrail::Kept - 1), 0.0f, 0.0f}));
    EXPECT_EQ(FormatTrail({{1.25f, -2.0f, 3.0f}, {4.0f, 5.5f, -6.0f}}), "1.2 -2.0 3.0 4.0 5.5 -6.0")
        << "glide's own arguments";
    trail.Clear();
    EXPECT_TRUE(trail.Points().empty());
}

// glide <x y z>...: along a path, at walking pace, by small teleports (a far teleport drops the
// player through the floor): where the player is `elapsed` seconds in.
TEST(BloodborneAutoplay, AGlideMovesAlongItsPathAtWalkingPace) {
    const std::vector<LampPosition> path{
        {0, 0, 0}, {GlideMetersPerSecond, 0, 0}, {GlideMetersPerSecond, 0, GlideMetersPerSecond}};
    EXPECT_EQ(GlidePoint(path, 0.0), (std::optional<LampPosition>{{0, 0, 0}}));
    EXPECT_EQ(GlidePoint(path, 0.5),
              (std::optional<LampPosition>{{GlideMetersPerSecond / 2, 0, 0}}));
    EXPECT_EQ(GlidePoint(path, 1.5),
              (std::optional<LampPosition>{{GlideMetersPerSecond, 0, GlideMetersPerSecond / 2}}));
    EXPECT_EQ(GlidePoint(path, 2.0), std::nullopt) << "there: the step is done";
    EXPECT_EQ(GlidePoint({{1, 2, 3}}, 0.0), std::nullopt) << "nowhere to go";
}
