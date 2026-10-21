// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The map screen (Kaniel 2026-10-17): its lamps, their flags and names, when a trip from the map
// may start and when it has arrived.

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include "core/bloodborne_map.h"
#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

TEST(BloodborneMap, ListsEachLampOnceAndTheDreamOnce) {
    const auto& lamps = MapLamps();
    // Central Yharnam's map (m24_01) has its four lamps; 2412959 is a boss arena's point.
    std::vector<s32> m24_01;
    for (const MapLamp& lamp : lamps) {
        if (lamp.packed_map == 0x18010000) {
            m24_01.push_back(lamp.warp_point);
        }
    }
    EXPECT_EQ(m24_01, (std::vector<s32>{2412950, 2412951, 2412952, 2412953}));
    // The Dream's thirteen respawn points are one lamp.
    EXPECT_EQ(
        std::ranges::count_if(lamps, [](const MapLamp& l) { return l.packed_map == 0x15000000; }),
        1);
    ASSERT_NE(FindMapLamp(HuntersDreamWarpPoint), nullptr);
    EXPECT_EQ(FindMapLamp(2203998), nullptr); // not a lamp (an event's point)
    const MapLamp* bridge = FindMapLamp(2412952);
    ASSERT_NE(bridge, nullptr);
    EXPECT_FLOAT_EQ(bridge->x, -124.8f);
    EXPECT_FLOAT_EQ(bridge->z, 64.3f);
    EXPECT_TRUE(std::ranges::is_sorted(lamps, {}, &MapLamp::warp_point));
}

TEST(BloodborneMap, ALampsActiveFlagIsTheOneThatSaysItIsLit) {
    EXPECT_EQ(LampActiveFlag(2412950), 12417810);
    EXPECT_EQ(LampActiveFlag(2412953), 12417870);
    EXPECT_EQ(LampActiveFlag(HuntersDreamWarpPoint), std::nullopt);
    for (const MapLamp& lamp : MapLamps()) {
        if (const auto flag = LampActiveFlag(lamp.warp_point)) {
            EXPECT_EQ(LampOfActiveFlag(*flag), lamp.warp_point - 1000); // the lamp object's entity
        }
    }
}

TEST(BloodborneMap, NamesComeFromTheGamesText) {
    EXPECT_EQ(LampNameId(2412951), 70002411); // "Central Yharnam"
    EXPECT_EQ(LampNameId(2402951), 70002401); // "Grand Cathedral"
    EXPECT_EQ(LampNameId(HuntersDreamWarpPoint), 70002100);
    EXPECT_EQ(AreaNameId(0x18010000), 2410);
    EXPECT_EQ(AreaNameId(0x18000000), 2400);
    EXPECT_EQ(AreaNameId(0x20000000), 3200); // Byrgenwerth
}

TEST(BloodborneMap, ATripStartsOnlyToALitLampFromTheWorld) {
    const MapTravelState ready{.lit = true, .in_world = true};
    EXPECT_EQ(MapTravelCheck(ready), MapTravelBlock::None);
    auto state = ready;
    state.lit = false;
    EXPECT_EQ(MapTravelCheck(state), MapTravelBlock::NotLit);
    state = ready;
    state.in_world = false;
    EXPECT_EQ(MapTravelCheck(state), MapTravelBlock::NotInWorld);
    state = ready;
    state.down = true;
    EXPECT_EQ(MapTravelCheck(state), MapTravelBlock::Down);
    state = ready;
    state.boss_fight = true;
    EXPECT_EQ(MapTravelCheck(state), MapTravelBlock::BossFight);
    state = ready;
    state.travelling = true;
    EXPECT_EQ(MapTravelCheck(state), MapTravelBlock::Travelling);
}

TEST(BloodborneMap, AnAreaIsClearedByItsMainBosses) {
    EXPECT_EQ(std::vector<s32>(AreaBossFlags(0x18010000).begin(), AreaBossFlags(0x18010000).end()),
              std::vector<s32>{12411800});
    EXPECT_EQ(AreaBossFlags(0x18020000).size(), 2u); // Upper Cathedral Ward: both its bosses
    EXPECT_TRUE(AreaBossFlags(0x15000000).empty());  // the Dream has none
    EXPECT_TRUE(AreaBossFlags(0x1d000000).empty());  // nor the chalice dungeons
}

TEST(BloodborneMap, TheFogOfWarKeepsTheCellsWalked) {
    EXPECT_EQ(ExploredCellAt(0x18010000, -193.4f, 68.5f), (ExploredCell{0x18010000, -25, 8}));
    EXPECT_EQ(ExploredCellAt(0x18010000, 0.0f, -0.1f), (ExploredCell{0x18010000, 0, -1}));
    const std::vector<ExploredCell> cells{{0x18010000, -25, 8}, {0x17000000, 15, 4}};
    EXPECT_EQ(LoadExplored(SaveExplored(cells)), cells);
    auto broken = SaveExplored(cells);
    broken[0] = 'X';
    EXPECT_EQ(LoadExplored(broken), std::nullopt);
    EXPECT_EQ(LoadExplored(std::vector<u8>(5, 0)), std::nullopt);
}

TEST(BloodborneMap, ATripArrivesOnceItsMapIsInAfterALoad) {
    MapTripProgress trip{.target_map = 0x18010000, .seconds = 1.0};
    // Still on the target map before the load (a lamp on the same map): not yet.
    EXPECT_FALSE(MapTripArrived(trip, 0x18010000, false));
    trip.load_seen = true;
    EXPECT_FALSE(MapTripArrived(trip, 0x18010000, true));  // still loading
    EXPECT_FALSE(MapTripArrived(trip, 0x18000000, false)); // another map
    EXPECT_TRUE(MapTripArrived(trip, 0x18010000, false));
    // No load seen at all: arrived once on the map a while after the start.
    trip = {.target_map = 0x18010000, .seconds = 9.0};
    EXPECT_TRUE(MapTripArrived(trip, 0x18010000, false));
}

namespace {

// A camera as the game holds one: at (1, 2, 3), looking north (+z), y up, x east.
GameCamera LevelCamera(float x_sign = 1.0f) {
    GameCamera camera;
    camera.rows = {{{x_sign, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {1, 2, 3, 1}}};
    camera.fov = 0.9f;
    camera.aspect = 16.0f / 9.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 800.0f;
    return camera;
}

float Dot(const std::array<float, 4>& a, std::array<float, 3> b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

} // namespace

TEST(BloodborneMap, AGameCameraHasUnitAxesAtRightAngles) {
    EXPECT_TRUE(LooksLikeGameCamera(LevelCamera()));
    EXPECT_TRUE(LooksLikeGameCamera(LevelCamera(-1.0f)));
    GameCamera scaled = LevelCamera();
    scaled.rows[1][1] = 2.0f;
    EXPECT_FALSE(LooksLikeGameCamera(scaled));
    GameCamera skewed = LevelCamera();
    skewed.rows[2] = {0, 0.6f, 0.8f, 0};
    EXPECT_FALSE(LooksLikeGameCamera(skewed));
    GameCamera moved_w = LevelCamera();
    moved_w.rows[3][3] = 0.0f;
    EXPECT_FALSE(LooksLikeGameCamera(moved_w));
    GameCamera broken = LevelCamera();
    broken.rows[3][0] = std::nanf("");
    EXPECT_FALSE(LooksLikeGameCamera(broken));
    GameCamera no_view = LevelCamera();
    no_view.fov = 0.0f;
    EXPECT_FALSE(LooksLikeGameCamera(no_view));
    EXPECT_FALSE(LooksLikeGameCamera(GameCamera{}));
}

TEST(BloodborneMap, TheOverheadCameraLooksDownWithNorthUp) {
    for (const float x_sign : {1.0f, -1.0f}) {
        const GameCamera game = LevelCamera(x_sign);
        const GameCamera above = OverheadCamera(
            game, {.x = 10, .y = 5, .z = -20, .distance = 100, .heading = 0, .pitch = 1.5707964f});
        EXPECT_TRUE(LooksLikeGameCamera(above));
        EXPECT_NEAR(above.rows[3][0], 10.0f, 1e-3f);
        EXPECT_NEAR(above.rows[3][1], 105.0f, 1e-3f);
        EXPECT_NEAR(above.rows[3][2], -20.0f, 1e-3f);
        EXPECT_NEAR(Dot(above.rows[2], {0, -1, 0}), 1.0f, 1e-4f); // looks straight down
        EXPECT_NEAR(Dot(above.rows[1], {0, 0, 1}), 1.0f, 1e-4f);  // north at the top
        // East on the same side as the game's own x axis.
        EXPECT_NEAR(Dot(above.rows[0], {x_sign, 0, 0}), 1.0f, 1e-4f);
        EXPECT_EQ(above.fov, game.fov);
        EXPECT_EQ(above.aspect, game.aspect);
        EXPECT_EQ(above.near_plane, game.near_plane);
        EXPECT_GE(above.far_plane, 100.0f + 500.0f);
    }
}

TEST(BloodborneMap, TheOverheadCameraTurnsAndTilts) {
    // Facing east, tilted 0.9 rad down, 50 m from the spot.
    const float pitch = 0.9f;
    const GameCamera above = OverheadCamera(
        LevelCamera(),
        {.x = 0, .y = 0, .z = 0, .distance = 50, .heading = 1.5707964f, .pitch = pitch});
    EXPECT_TRUE(LooksLikeGameCamera(above));
    const std::array<float, 3> looks{std::cos(pitch), -std::sin(pitch), 0.0f};
    EXPECT_NEAR(Dot(above.rows[2], looks), 1.0f, 1e-4f);
    EXPECT_NEAR(above.rows[3][0], -50.0f * looks[0], 1e-3f);
    EXPECT_NEAR(above.rows[3][1], -50.0f * looks[1], 1e-3f);
    EXPECT_NEAR(above.rows[3][2], 0.0f, 1e-3f);
    EXPECT_GT(above.rows[1][1], 0.0f); // the top of the picture is up the slope,
    EXPECT_GT(above.rows[1][0], 0.0f); // and towards the heading
    EXPECT_NEAR(Dot(above.rows[0], {0, 0, -1}), 1.0f, 1e-4f); // south on the right
    // A far plane the game set farther stays.
    GameCamera far = LevelCamera();
    far.far_plane = 5000.0f;
    EXPECT_EQ(OverheadCamera(far, {.distance = 50, .pitch = pitch}).far_plane, 5000.0f);
}

TEST(BloodborneMap, MarksAreProjectedWithTheCamera) {
    const GameCamera above = OverheadCamera(
        LevelCamera(),
        {.x = 10, .y = 5, .z = -20, .distance = 100, .heading = 0, .pitch = 1.5707964f});
    const auto middle = ProjectWithCamera(above, 10, 5, -20);
    ASSERT_TRUE(middle.has_value());
    EXPECT_NEAR((*middle)[0], 0.0f, 1e-4f);
    EXPECT_NEAR((*middle)[1], 0.0f, 1e-4f);
    // 10 m east: right of the middle, by 10 / (100 tan(fov / 2) aspect).
    const auto east = ProjectWithCamera(above, 20, 5, -20);
    ASSERT_TRUE(east.has_value());
    EXPECT_NEAR((*east)[0], 10.0f / (100.0f * std::tan(0.45f) * above.aspect), 1e-4f);
    EXPECT_NEAR((*east)[1], 0.0f, 1e-4f);
    const auto north = ProjectWithCamera(above, 10, 5, -10);
    ASSERT_TRUE(north.has_value());
    EXPECT_NEAR((*north)[1], 10.0f / (100.0f * std::tan(0.45f)), 1e-4f);
    // Higher up is nearer the camera: farther from the middle.
    const auto roof = ProjectWithCamera(above, 20, 55, -20);
    ASSERT_TRUE(roof.has_value());
    EXPECT_GT((*roof)[0], (*east)[0]);
    // Above the camera: not in the picture.
    EXPECT_FALSE(ProjectWithCamera(above, 10, 200, -20).has_value());
}
