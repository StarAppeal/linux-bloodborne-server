// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

namespace {

// A loaded PARAM file as the game keeps it: the type at +0x0C, the row count at +0x0A, sorted
// 24-byte row entries (id, padding, data offset) at +0x40 and the rows after them.
std::vector<u8> MakeParam(std::string_view type, std::size_t row_size,
                          const std::vector<s32>& ids) {
    const std::size_t data = 0x40 + ids.size() * 24;
    std::vector<u8> param(data + ids.size() * row_size);
    const u16 rows = static_cast<u16>(ids.size());
    std::memcpy(param.data() + 0x0A, &rows, sizeof(rows));
    std::memcpy(param.data() + 0x0C, type.data(), type.size());
    for (std::size_t row = 0; row < ids.size(); ++row) {
        const u64 offset = data + row * row_size;
        std::memcpy(param.data() + 0x40 + row * 24, &ids[row], sizeof(s32));
        std::memcpy(param.data() + 0x40 + row * 24 + 8, &offset, sizeof(offset));
    }
    return param;
}

template <typename T>
T Get(const std::vector<u8>& param, std::size_t row, std::size_t row_size, u64 field) {
    T value{};
    const std::size_t data = 0x40 + (param.size() - 0x40) / (24 + row_size) * 24;
    std::memcpy(&value, param.data() + data + row * row_size + field, sizeof(T));
    return value;
}

template <typename T>
void Set(std::vector<u8>& param, std::size_t row, std::size_t row_size, u64 field, T value) {
    const std::size_t data = 0x40 + (param.size() - 0x40) / (24 + row_size) * 24;
    std::memcpy(param.data() + data + row * row_size + field, &value, sizeof(T));
}

} // namespace

// SHADPS4_BLOODBORNE_HARDER_ENEMIES=<hp>,<damage>,<poise>: multipliers for every enemy and boss.
TEST(BloodborneHarderEnemies, ParsesThreeMultipliers) {
    const auto harder = ParseHarderEnemies("3,1.5,3");
    ASSERT_TRUE(harder);
    EXPECT_FLOAT_EQ(harder->hp, 3.0f);
    EXPECT_FLOAT_EQ(harder->damage, 1.5f);
    EXPECT_FLOAT_EQ(harder->poise, 3.0f);
    EXPECT_TRUE(ParseHarderEnemies(" 2 , 1.25 ,4 "));
}

TEST(BloodborneHarderEnemies, OffOrMalformedMeansVanilla) {
    EXPECT_FALSE(ParseHarderEnemies(""));
    EXPECT_FALSE(ParseHarderEnemies("0"));
    EXPECT_FALSE(ParseHarderEnemies("off"));
    EXPECT_FALSE(ParseHarderEnemies("1,1,1")) << "nothing to change";
    EXPECT_FALSE(ParseHarderEnemies("3,1.5"));
    EXPECT_FALSE(ParseHarderEnemies("3,1.5,3,4"));
    EXPECT_FALSE(ParseHarderEnemies("3,x,3"));
    EXPECT_FALSE(ParseHarderEnemies("0.5,1,1")) << "never easier than the game";
    EXPECT_FALSE(ParseHarderEnemies("11,1,1")) << "a typo, not a setting";
}

TEST(BloodborneHarderEnemies, HpScalesButDummiesStay) {
    EXPECT_EQ(HarderEnemyHp(1440, 3.0f), 4320u); // Father Gascoigne
    EXPECT_EQ(HarderEnemyHp(80, 1.5f), 120u);
    EXPECT_EQ(HarderEnemyHp(1, 3.0f), 1u) << "1-HP rows are event dummies";
    EXPECT_EQ(HarderEnemyHp(0, 3.0f), 0u);
    EXPECT_EQ(HarderEnemyHp(9'000'000, 10.0f), 9'999'999u);
}

// Most enemies (Gascoigne included) have no poise at all (superArmorDurability 0), so any hit
// staggers them; a multiplier alone would change nothing. They get a floor: 15 (50 for a
// boss-sized HP of 1000+) times the poise multiplier, about two R1s of a Saw Cleaver at x3 for
// an ordinary enemy and several hunters' worth for a boss.
TEST(BloodborneHarderEnemies, PoiseHasAFloorForEnemiesWithout) {
    EXPECT_EQ(HarderEnemyPoise(0, 80, 3.0f), 45);
    EXPECT_EQ(HarderEnemyPoise(0, 1440, 3.0f), 150);
    EXPECT_EQ(HarderEnemyPoise(70, 160, 3.0f), 210);
    EXPECT_EQ(HarderEnemyPoise(60, 2400, 3.0f), 180);
    EXPECT_EQ(HarderEnemyPoise(40, 100, 1.0f), 40) << "x1 keeps the game's poise";
    EXPECT_EQ(HarderEnemyPoise(0, 100, 1.0f), 0);
    EXPECT_EQ(HarderEnemyPoise(9999, 100, 3.0f), 9999) << "already unbreakable";
    EXPECT_EQ(HarderEnemyPoise(5000, 100, 3.0f), 9998);
    EXPECT_EQ(HarderEnemyPoise(0, 1, 3.0f), 0) << "event dummies stay as they are";
}

TEST(BloodborneHarderEnemies, AttackScalesAndSaturates) {
    EXPECT_EQ(HarderEnemyAttack(136, 1.5f), 204);
    EXPECT_EQ(HarderEnemyAttack(0, 1.5f), 0);
    EXPECT_EQ(HarderEnemyAttack(60000, 2.0f), 65535);
}

// Kaniel 2026-10-28 (boss tempo): a boss row's (npcType 1) turnVellocity turns faster.
TEST(BloodborneBossTempo, TurnsOnlyBossRowsFaster) {
    constexpr std::size_t RowSize = 0x184;
    auto param = MakeParam("NPC_PARAM_ST", RowSize, {100000, 271000, 271005});
    Set<float>(param, 0, RowSize, NpcTurnVelocityOffset, 60.0f);
    Set<float>(param, 1, RowSize, NpcTurnVelocityOffset, 120.0f);
    Set<u8>(param, 1, RowSize, NpcTypeOffset, NpcTypeBoss);
    Set<float>(param, 2, RowSize, NpcTurnVelocityOffset, 0.0f); // a boss that never turns
    Set<u8>(param, 2, RowSize, NpcTypeOffset, NpcTypeBoss);
    EXPECT_EQ(ScaleBossTurnSpeed(param, 1.3f), 1u);
    EXPECT_FLOAT_EQ(Get<float>(param, 0, RowSize, NpcTurnVelocityOffset), 60.0f);
    EXPECT_FLOAT_EQ(Get<float>(param, 1, RowSize, NpcTurnVelocityOffset), 156.0f);
    EXPECT_FLOAT_EQ(Get<float>(param, 2, RowSize, NpcTurnVelocityOffset), 0.0f);
    EXPECT_EQ(ScaleBossTurnSpeed(param, 1.0f), 0u);
    auto attacks = MakeParam("ATK_PARAM_ST", 0xA4, {271000});
    EXPECT_EQ(ScaleBossTurnSpeed(attacks, 1.3f), 0u);
}

TEST(BloodborneHarderEnemies, ScalesNpcParamRows) {
    constexpr std::size_t RowSize = 0x184;
    auto param = MakeParam("NPC_PARAM_ST", RowSize, {100000, 271000, 455000});
    Set<u32>(param, 0, RowSize, NpcHpOffset, 80);
    Set<u32>(param, 1, RowSize, NpcHpOffset, 1440);
    Set<u32>(param, 2, RowSize, NpcHpOffset, 1);
    Set<s16>(param, 1, RowSize, NpcPoiseOffset, 0);
    const HarderEnemies harder{3.0f, 1.5f, 3.0f};
    EXPECT_EQ(MakeNpcParamHarder(param, harder), 2u);
    EXPECT_EQ(Get<u32>(param, 0, RowSize, NpcHpOffset), 240u);
    EXPECT_EQ(Get<s16>(param, 0, RowSize, NpcPoiseOffset), 45);
    EXPECT_EQ(Get<u32>(param, 1, RowSize, NpcHpOffset), 4320u);
    EXPECT_EQ(Get<s16>(param, 1, RowSize, NpcPoiseOffset), 150);
    EXPECT_EQ(Get<u32>(param, 2, RowSize, NpcHpOffset), 1u);
}

TEST(BloodborneHarderEnemies, ScalesNpcAttackRows) {
    constexpr std::size_t RowSize = 0xA4;
    auto param = MakeParam("ATK_PARAM_ST", RowSize, {271000, 271003});
    Set<u16>(param, 0, RowSize, AtkPhysOffset, 136);
    Set<u16>(param, 1, RowSize, AtkPhysOffset, 159);
    Set<u16>(param, 1, RowSize, AtkPhysOffset + 4, 40); // fire
    EXPECT_EQ(MakeNpcAttacksHarder(param, 1.5f), 2u);
    EXPECT_EQ(Get<u16>(param, 0, RowSize, AtkPhysOffset), 204);
    EXPECT_EQ(Get<u16>(param, 1, RowSize, AtkPhysOffset), 239);
    EXPECT_EQ(Get<u16>(param, 1, RowSize, AtkPhysOffset + 4), 60);
}

TEST(BloodborneHarderEnemies, RefusesOtherTablesAndBrokenRows) {
    auto spec = MakeParam("SP_EFFECT_PARAM_ST", 0x210, {9006});
    EXPECT_EQ(MakeNpcParamHarder(spec, HarderEnemies{3.0f, 1.5f, 3.0f}), 0u);
    EXPECT_EQ(MakeNpcAttacksHarder(spec, 1.5f), 0u);
    auto player_attacks = MakeParam("ATK_PARAM_ST", 0xA4, {100000});
    Set<u16>(player_attacks, 0, 0xA4, AtkPhysOffset, 100);
    EXPECT_EQ(MakeNpcAttacksHarder(player_attacks, 1.5f), 0u) << "AtkParam_Pc stays";
    EXPECT_EQ(Get<u16>(player_attacks, 0, 0xA4, AtkPhysOffset), 100);
    auto npc = MakeParam("NPC_PARAM_ST", 0x184, {100000});
    const u64 outside = npc.size();
    std::memcpy(npc.data() + 0x48, &outside, sizeof(outside));
    EXPECT_EQ(MakeNpcParamHarder(npc, HarderEnemies{3.0f, 1.5f, 3.0f}), 0u);
}

// Two row entries may share one row's data; it is scaled once.
TEST(BloodborneHarderEnemies, SharedRowDataScalesOnce) {
    constexpr std::size_t RowSize = 0x184;
    auto param = MakeParam("NPC_PARAM_ST", RowSize, {100000, 100001});
    Set<u32>(param, 0, RowSize, NpcHpOffset, 80);
    u64 first = 0;
    std::memcpy(&first, param.data() + 0x48, sizeof(first));
    std::memcpy(param.data() + 0x40 + 24 + 8, &first, sizeof(first));
    EXPECT_EQ(MakeNpcParamHarder(param, HarderEnemies{3.0f, 1.0f, 1.0f}), 1u);
    EXPECT_EQ(Get<u32>(param, 0, RowSize, NpcHpOffset), 240u);
}

// Bosses get one HP multiplier: the game's co-op bonus (7500-7503) is set to x1.
TEST(BloodborneHarderEnemies, RemovesTheCoopBossHpBonus) {
    constexpr std::size_t RowSize = 0x210;
    auto param = MakeParam("SP_EFFECT_PARAM_ST", RowSize, {7500, 7501, 7502, 7503, 9006});
    const std::array<float, 5> rates{1.5f, 2.0f, 2.5f, 3.0f, 0.7f};
    for (std::size_t row = 0; row < rates.size(); ++row) {
        Set<float>(param, row, RowSize, SpEffectMaxHpRateOffset, rates[row]);
        Set<float>(param, row, RowSize, 0x108, rates[row]); // poison resistance stays
    }
    EXPECT_EQ(RemoveCoopBossHpBonus(param), 4u);
    for (std::size_t row = 0; row < 4; ++row) {
        EXPECT_FLOAT_EQ(Get<float>(param, row, RowSize, SpEffectMaxHpRateOffset), 1.0f);
        EXPECT_FLOAT_EQ(Get<float>(param, row, RowSize, 0x108), rates[row]);
    }
    EXPECT_FLOAT_EQ(Get<float>(param, 4, RowSize, SpEffectMaxHpRateOffset), 0.7f);
    EXPECT_EQ(RemoveCoopBossHpBonus(param), 0u) << "already done";
    auto npc = MakeParam("NPC_PARAM_ST", 0x184, {7500});
    EXPECT_EQ(RemoveCoopBossHpBonus(npc), 0u);
}

// Kaniel 2026-10-25 (the enemy scaling rule): the co-op boss HP bonus comes back when a team is
// scaled down to the normal game.
TEST(BloodborneHarderEnemies, TheCoopBossBonusComesBack) {
    constexpr std::size_t RowSize = 0x210;
    std::vector<u8> param = MakeParam("SP_EFFECT_PARAM_ST", RowSize, {7500, 7501, 7502, 7503});
    for (std::size_t row = 0; row < 4; ++row) {
        const float rate = 1.5f + static_cast<float>(row);
        std::memcpy(param.data() + 0x40 + 4 * 24 + row * RowSize + SpEffectMaxHpRateOffset, &rate,
                    sizeof(rate));
    }
    const auto own = CoopBossHpRates(param);
    ASSERT_EQ(own.size(), 4u);
    EXPECT_EQ(own[1], (std::pair<s32, float>{7501, 2.5f}));
    EXPECT_EQ(RemoveCoopBossHpBonus(param), 4u);
    EXPECT_FLOAT_EQ(CoopBossHpRates(param)[3].second, 1.0f);
    SetCoopBossHpRates(param, own);
    EXPECT_EQ(CoopBossHpRates(param), own);
}
