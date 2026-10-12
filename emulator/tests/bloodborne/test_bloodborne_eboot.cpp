// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Checks the patch tables against the game itself: every patch's original bytes, and the calls
// the comments name, in the 01.09 eboot. The game is not in the repository, so these run only
// where BLOODBORNE_EBOOT names an eboot.bin (CUSA03173, 01.09); elsewhere they are skipped.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/bloodborne_party.h"

using namespace Core::Bloodborne;

namespace {

template <typename T>
T ReadAt(const std::vector<u8>& data, size_t at) {
    T value{};
    std::memcpy(&value, data.data() + at, sizeof(T));
    return value;
}

// The executable segment of a fake-signed SELF: shadPS4 loads it at the image base, so the
// patch offsets are offsets into it.
std::vector<u8> TextSegment(const std::vector<u8>& self) {
    constexpr u32 SelfMagic = 0x1D3D154F;
    if (self.size() < 0x20 || ReadAt<u32>(self, 0) != SelfMagic) {
        return {};
    }
    const u16 segments = ReadAt<u16>(self, 0x18);
    const size_t elf = 0x20 + size_t{segments} * 0x20;
    if (elf + 0x40 > self.size() || std::memcmp(self.data() + elf,
                                                "\x7F"
                                                "ELF",
                                                4) != 0) {
        return {};
    }
    const u64 phoff = ReadAt<u64>(self, elf + 0x20);
    const u16 phnum = ReadAt<u16>(self, elf + 0x38);
    for (u16 segment = 0; segment < segments; ++segment) {
        const size_t entry = 0x20 + size_t{segment} * 0x20;
        const u64 flags = ReadAt<u64>(self, entry);
        const u64 offset = ReadAt<u64>(self, entry + 0x08);
        const u64 size = ReadAt<u64>(self, entry + 0x10);
        const u32 index = static_cast<u32>((flags >> 20) & 0xFFF);
        const size_t header = elf + phoff + size_t{index} * 0x38;
        if (index >= phnum || header + 0x38 > self.size()) {
            continue;
        }
        constexpr u32 Loadable = 1;
        constexpr u32 Executable = 1;
        if (ReadAt<u32>(self, header) == Loadable && (ReadAt<u32>(self, header + 4) & Executable) &&
            ReadAt<u64>(self, header + 0x10) == 0 && offset + size <= self.size()) {
            return {self.begin() + offset, self.begin() + offset + size};
        }
    }
    return {};
}

class BloodborneEboot : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const char* path = std::getenv("BLOODBORNE_EBOOT");
        if (path == nullptr || *path == '\0') {
            return;
        }
        std::ifstream file(path, std::ios::binary);
        const std::vector<u8> self{std::istreambuf_iterator<char>{file}, {}};
        text = new std::vector<u8>(TextSegment(self));
    }
    static void TearDownTestSuite() {
        delete text;
        text = nullptr;
    }
    void SetUp() override {
        if (text == nullptr) {
            GTEST_SKIP() << "BLOODBORNE_EBOOT does not name the game's eboot.bin";
        }
        ASSERT_FALSE(text->empty()) << "not a fake-signed SELF with a text segment";
    }

    static std::vector<u8> Bytes(u64 offset, size_t size) {
        if (offset + size > text->size()) {
            return {};
        }
        return {text->begin() + offset, text->begin() + offset + size};
    }
    static u64 CallTarget(u64 offset) {
        return DecodeNearCallTarget(offset, Bytes(offset, 5));
    }
    static u64 JumpTarget(u64 offset) {
        return DecodeNearJumpTarget(offset, Bytes(offset, 5));
    }

    static inline std::vector<u8>* text = nullptr;
};

constexpr u64 MultiPlayStopRequest = 0x01ED07A0; // CSMultiPlayMan StopRequest
constexpr u64 LeaveSession = 0x015BED10;
const std::vector<u8> FunctionPrologue{0x55, 0x48, 0x89, 0xE5}; // push rbp; mov rbp, rsp

} // namespace

TEST_F(BloodborneEboot, EveryPatchFindsTheGamesOwnBytes) {
    const std::vector<std::pair<const char*, const std::vector<CodePatch>*>> tables{
        {"ghost death", &GhostDeathKeepSessionPatches()},
        {"guest bloodstain", &GuestBloodstainPatches()},
        {"no echo theft", &NoEchoTheftPatches()},
        {"host death", &HostDeathKeepGuestsPatches()},
        {"boss clear", &BossClearKeepSessionPatches()},
        {"join anywhere", &JoinAnywherePatches()},
        {"quiet summon banners", &QuietSummonBannerPatches()},
    };
    for (const auto& [name, patches] : tables) {
        for (const CodePatch& patch : *patches) {
            EXPECT_EQ(Bytes(patch.offset, patch.original.size()), patch.original)
                << name << " patch at " << std::hex << patch.offset;
        }
    }
}

// The co-op site table (bloodborne_patch_sites.h): every site holds its original bytes in the
// game as it ships (the default game patches are checked by bbport/tests/test_coop_sites.py).
TEST_F(BloodborneEboot, EveryCoopSiteHoldsItsOriginalBytes) {
    for (const CoopSite& site : CoopSites) {
        const auto original = HexBytes(site.original);
        EXPECT_EQ(Bytes(site.offset, original.size()), original)
            << site.feature << ": " << site.name << " at " << std::hex << site.offset;
    }
}

// The joiner kick of a closing session tests SessionManager +0x118 (ReopensClosedSession):
// cmp byte [r12 + 0x118], 0 at 0x01ED121E.
TEST_F(BloodborneEboot, TheClosingSessionKickTestsTheFlagWeClear) {
    EXPECT_EQ(Bytes(0x01ED121E, 9),
              (std::vector<u8>{0x41, 0x80, 0xBC, 0x24, 0x18, 0x01, 0x00, 0x00, 0x00}));
    EXPECT_EQ(SessionClosingOffset, 0x118u);
}

TEST_F(BloodborneEboot, TheSignTimersAreTheGamesStores) {
    for (const SignTimerSite& site : SignTimerSites) {
        const auto expected = EncodeSignTimerStore(site.modrm, site.original_seconds);
        EXPECT_EQ(Bytes(site.offset, expected.size()),
                  std::vector<u8>(expected.begin(), expected.end()))
            << "sign timer at " << std::hex << site.offset;
    }
}

TEST_F(BloodborneEboot, TheNamedGameFunctionsStartThere) {
    for (const u64 offset :
         {EventFlagIsSetOffset, PartyGhostSetEventFlagOffset, PartyGhostCustomLuaCallStartOffset,
          OnKeyTime2Offset, MultiPlayStopRequest, LeaveSession, KillCharacterOffset}) {
        EXPECT_EQ(Bytes(offset, FunctionPrologue.size()), FunctionPrologue) << std::hex << offset;
    }
}

// The downed banner is the game's SetTextEffect (the event binding that queues the HUD's big
// banners, ids 1 to 17): `lea eax, [rsi - 1]; cmp eax, 0x10`, and the Lua table registers it.
TEST_F(BloodborneEboot, TheDownedBannerIsQueuedBySetTextEffect) {
    EXPECT_EQ(Bytes(SetTextEffectOffset, SetTextEffectStart.size()),
              std::vector<u8>(SetTextEffectStart.begin(), SetTextEffectStart.end()));
    EXPECT_LE(DownedBannerTextEffect, 17);
    // lea rax, [rip - 0xC745] at 0x013427EE: the binding's registration takes this function.
    EXPECT_EQ(Bytes(0x013427EE, 3), (std::vector<u8>{0x48, 0x8D, 0x05}));
    EXPECT_EQ(0x013427F5 + ReadAt<s32>(*text, 0x013427F1), SetTextEffectOffset);
}

// The party HUD's nameplate hook is the HUD tag update's one exit (`add rsp, 0xC8; pop rbx...`),
// with the tag update's own start where the comment says (it keeps the tag in r14, data in r13).
TEST_F(BloodborneEboot, TheNameplateHookIsTheTagUpdatesExit) {
    EXPECT_EQ(Bytes(HudTagUpdateExitOffset, HudTagUpdateExit.size()),
              std::vector<u8>(HudTagUpdateExit.begin(), HudTagUpdateExit.end()));
    EXPECT_EQ(Bytes(HudTagUpdateExitOffset + HudTagUpdateExit.size(), 1), std::vector<u8>{0x5B});
    // mov r13, rsi; mov r14, rdi in the update's prologue.
    EXPECT_EQ(Bytes(0x01B59E34, 6), (std::vector<u8>{0x49, 0x89, 0xF5, 0x49, 0x89, 0xFE}));
}

// A hit's final damage is stored at the hit record's +0x22C (`mov [rcx+0x22C], eax`), before any
// HP changes; the remote-player flag and the attacker are in the hit's frame.
TEST_F(BloodborneEboot, TheReviveHitHookIsWhereTheHitsDamageIsStored) {
    EXPECT_EQ(Bytes(HitDamageStoredOffset, HitDamageStored.size()),
              std::vector<u8>(HitDamageStored.begin(), HitDamageStored.end()));
    // The team setter (SetTeamType): mov [rax + 0x88], ebx.
    EXPECT_EQ(Bytes(0x01334C73, 6), (std::vector<u8>{0x89, 0x98, 0x88, 0x00, 0x00, 0x00}));
    EXPECT_EQ(CharacterTeamOffset, 0x88u);
}

// The calls the patches and hooks are about are where the comments say.
TEST_F(BloodborneEboot, ThePatchedCallsGoWhereTheCommentsSay) {
    // Ghost death: SetEventFlag(4047) twice, then CustomLuaCallStart(4063).
    EXPECT_EQ(CallTarget(0x01381FE7), PartyGhostSetEventFlagOffset);
    EXPECT_EQ(CallTarget(0x01381FF6), PartyGhostSetEventFlagOffset);
    EXPECT_EQ(CallTarget(0x0138201B), PartyGhostCustomLuaCallStartOffset);
    // SoloBlockClear returns at 0x01385A4A, before its StopRequest and LeaveSession tail jump.
    EXPECT_EQ(CallTarget(0x01385A95), MultiPlayStopRequest);
    EXPECT_EQ(JumpTarget(0x01385AC6), LeaveSession);
    EXPECT_LT(BossClearKeepSessionPatches().back().offset, 0x01385A95u);
    // The world load's StopRequest returns to the address IsWorldReloadStopRequest knows, and its
    // LeaveSession follows.
    EXPECT_EQ(CallTarget(0x0193C657), MultiPlayStopRequest);
    EXPECT_TRUE(IsWorldReloadStopRequest(0x0193C657 + 5));
    EXPECT_EQ(CallTarget(0x0193C689), LeaveSession);
    // The co-op area check: the flag read, then `xor al, 1`.
    EXPECT_EQ(CallTarget(0x0191A8B5), EventFlagIsSetOffset);
    EXPECT_EQ(Bytes(0x0191A8BA, 2), (std::vector<u8>{0x34, 0x01}));
    // Event commands 2004:5 (Change Character Enable State) and 2004:29 (Set Character Backread
    // State): the enable switch a guest uses on what the host's story removed, and the +0x28
    // byte (`setne byte ptr [rax + 0x28]`) the shown board carries.
    EXPECT_EQ(CallTarget(0x017BAA35), 0x018C6390u);
    EXPECT_EQ(Bytes(0x018C6390, 8),
              (std::vector<u8>{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56}));
    EXPECT_EQ(Bytes(0x017BBCC4, 4), (std::vector<u8>{0x0F, 0x95, 0x40, 0x28}));
    // The kill autoplay uses: the dead flag (`or byte ptr [r15 + 0x1E3], 1`) on its way out.
    EXPECT_EQ(Bytes(0x018BFAD3, 8),
              (std::vector<u8>{0x41, 0x80, 0x8F, 0xE3, 0x01, 0x00, 0x00, 0x01}));
    // The network banner both chime calls go to (it takes the message id the summon picked).
    EXPECT_EQ(Bytes(ShowNetworkBannerOffset, FunctionPrologue.size()), FunctionPrologue);
    for (const CodePatch& patch : QuietSummonBannerPatches()) {
        EXPECT_EQ(CallTarget(patch.offset), ShowNetworkBannerOffset) << std::hex << patch.offset;
    }
    // Host death: the jump lands on the epilogue it names.
    const CodePatch& host_dead = HostDeathKeepGuestsPatches().front();
    EXPECT_EQ(DecodeNearJumpTarget(host_dead.offset, host_dead.patched), HostDeadEpilogueOffset);
}

// The boss-death tracer's sites are the game's dead-flag writes; the two deaths go straight on
// to the death step.
TEST_F(BloodborneEboot, TheDeathSitesSetTheDeadFlag) {
    for (const DeathFlagSite& site : DeathFlagSites()) {
        EXPECT_EQ(Bytes(site.offset, site.instruction.size()), site.instruction)
            << std::hex << site.offset;
    }
    EXPECT_EQ(CallTarget(0x018BF94B), DeathStepOffset);
    EXPECT_EQ(CallTarget(0x018CE6CF), DeathStepOffset);
    EXPECT_EQ(Bytes(DeathStepOffset, FunctionPrologue.size()), FunctionPrologue);
}

// Party death: every IsNoDead caller in the table returns from a call to it, and the hook at its
// exit finds the bytes it copies.
TEST_F(BloodborneEboot, TheNoDeadCallersCallIsNoDead) {
    EXPECT_EQ(Bytes(IsNoDeadOffset, FunctionPrologue.size()), FunctionPrologue);
    EXPECT_EQ(Bytes(IsNoDeadExitOffset, IsNoDeadExit.size()),
              std::vector<u8>(IsNoDeadExit.begin(), IsNoDeadExit.end()));
    for (const NoDeadCaller& caller : NoDeadCallers()) {
        EXPECT_EQ(CallTarget(caller.return_offset - 5), IsNoDeadOffset)
            << std::hex << caller.return_offset;
    }
}

// Shared enemy damage counts this player's own hits at the game's hit function: its entry, and the
// return from the IsNoDead check that follows the applied hit (the AfterHit caller).
TEST_F(BloodborneEboot, TheHitHooksFindTheGamesHit) {
    EXPECT_EQ(Bytes(HitOffset, HitPrologue.size()),
              std::vector<u8>(HitPrologue.begin(), HitPrologue.end()));
    EXPECT_EQ(Bytes(HitAppliedOffset, HitApplied.size()),
              std::vector<u8>(HitApplied.begin(), HitApplied.end()));
    EXPECT_EQ(CallTarget(HitAppliedOffset - 5), IsNoDeadOffset);
    EXPECT_EQ(ClassifyNoDeadCheck(HitAppliedOffset), NoDeadCheck::AfterHit);
    EXPECT_EQ(CallTarget(0x01A2A6B5), HitOffset) << "the damage module's hit calls it";
    // Status effect HP changes: each site sets up the SetHp call that follows it.
    for (const u64 offset : EffectHpChangeOffsets) {
        EXPECT_EQ(Bytes(offset, EffectHpChange.size()),
                  std::vector<u8>(EffectHpChange.begin(), EffectHpChange.end()))
            << std::hex << offset;
        EXPECT_EQ(CallTarget(offset + EffectHpChange.size() + 4), SetHpOffset)
            << std::hex << offset;
    }
    EXPECT_EQ(CallTarget(0x01A2BFA3), SetHpOffset) << "the hit sets HP the same way";
}

// A member's character data (see MemberDataWait): the game's send and receive, its own one-byte
// request on channel 0x0A, the answer to it, the after-load broadcast, and the pending check that
// wants all five bits of the slot's byte.
TEST_F(BloodborneEboot, TheMemberDataRequestIsTheGamesOwn) {
    for (const u64 offset : {NetSyncSendOffset, NetSyncReceiveOffset, BroadcastMemberDataOffset}) {
        EXPECT_EQ(Bytes(offset, FunctionPrologue.size()), FunctionPrologue) << std::hex << offset;
    }
    const std::vector<u8> request_channel{0xBA, MemberDataRequestChannel, 0x00, 0x00, 0x00};
    // The game's request helper: mov edx, 0xA; mov r8d, 1; ... call send.
    EXPECT_EQ(Bytes(0x01781A99, 5), request_channel);
    EXPECT_EQ(CallTarget(0x01781AA4), NetSyncSendOffset);
    // The answer: the request is received at 0x01783EEB, the parts go out after it.
    EXPECT_EQ(Bytes(0x01783EDA, 5), request_channel);
    EXPECT_EQ(CallTarget(0x01783EEB), NetSyncReceiveOffset);
    EXPECT_EQ(CallTarget(0x017849C4), NetSyncSendOffset) << "the gear (channel 0x0C)";
    // The broadcast after a load.
    EXPECT_EQ(CallTarget(0x0193A979), BroadcastMemberDataOffset);
    // cmp byte ptr [rcx + rax], 0x1F: a pending member is done when its slot's byte is full.
    EXPECT_EQ(Bytes(0x0178B578, 4), (std::vector<u8>{0x80, 0x3C, 0x01, 0x1F}));
    EXPECT_EQ(MemberDataParts | 0x01, 0x1F);
}

namespace {
// Target of a `lea reg, [rip + disp32]` (REX.W, or REX.WR for r8-r15) at `offset`, or 0.
u64 LeaTarget(const std::vector<u8>& bytes, u64 offset) {
    if (bytes.size() < 7 || (bytes[0] != 0x48 && bytes[0] != 0x4C) || bytes[1] != 0x8D ||
        (bytes[2] & 0xC7) != 0x05) {
        return 0;
    }
    s32 displacement{};
    std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
    return offset + 7 + static_cast<s64>(displacement);
}
} // namespace

// The vtables the co-op code checks objects against (see IsGameVtable) are what the classes'
// constructors store: `lea rax, [rip + vtable]`, then `mov [object], rax`.
TEST_F(BloodborneEboot, TheCheckedVtablesAreWhatTheConstructorsStore) {
    const auto lea = [](u64 offset) { return LeaTarget(Bytes(offset, 7), offset); };
    // Characters: one constructor each.
    EXPECT_EQ(lea(0x018B9B03), CharacterVtables[0]);
    EXPECT_EQ(lea(0x018E48C9), CharacterVtables[1]);
    EXPECT_EQ(lea(0x018F176C), CharacterVtables[2]);
    EXPECT_EQ(lea(0x01908094), CharacterVtables[3]);
    // The own bloodstain: 0x01467A70 builds it (0x018675B0), puts it at [[SprjEventMan] +0x80]
    // +8 and shows it with the function ShowGuestBloodstain calls.
    EXPECT_EQ(CallTarget(0x01467AE0), 0x018675B0u);
    EXPECT_EQ(lea(0x018675D2), BloodstainVtable);
    EXPECT_EQ(Bytes(0x01467B05, 4), (std::vector<u8>{0x48, 0x89, 0x78, 0x08}));
    EXPECT_EQ(JumpTarget(0x01467B19), 0x01868A50u);
    // EventFlagMan: built by 0x013B5910 into [0x0553B100]; its flag store part (0x013CF0E0, the
    // object itself) stores the vtable, and the flag read (0x013CFD80) takes that object.
    EXPECT_EQ(lea(0x0156C879), 0x0553B100u);
    EXPECT_EQ(CallTarget(0x0156C8A6), 0x013B5910u);
    EXPECT_EQ(CallTarget(0x013B5946), 0x013CF0E0u);
    EXPECT_EQ(lea(0x013CF104), EventFlagManVtable);
    // MapItemMan: built by 0x017D2960 into [0x0553D6E0].
    EXPECT_EQ(lea(0x01929318), 0x0553D6E0u);
    EXPECT_EQ(CallTarget(0x0192934C), 0x017D2960u);
    EXPECT_EQ(lea(0x017D2983), MapItemManVtable);
}

// PlaceCharacter does what the game does when it puts the player at its lamp on a load: right
// after the teleport (`call [rax + 0x5F8]`) the position snap and the follow reset. The CamReset
// script binding (0x0132F5A0) calls the same two.
TEST_F(BloodborneEboot, PlacementCallsWhatTheGamesLampPlacementCalls) {
    EXPECT_EQ(Bytes(0x0154B2F2, 6), (std::vector<u8>{0xFF, 0x90, 0xF8, 0x05, 0x00, 0x00}));
    EXPECT_EQ(CallTarget(0x0154B32A), SnapCharacterPositionOffset);
    EXPECT_EQ(CallTarget(0x0154B3BD), ResetCharacterFollowOffset);
    EXPECT_EQ(CallTarget(0x0132F605), SnapCharacterPositionOffset);
    EXPECT_EQ(CallTarget(0x0132F612), ResetCharacterFollowOffset);
    EXPECT_EQ(Bytes(SnapCharacterPositionOffset, 4), FunctionPrologue);
    EXPECT_EQ(Bytes(ResetCharacterFollowOffset, 4), FunctionPrologue);
}

// The camera manager: the in-game step builds it (0x01435FC0) and keeps it in [[0x0553E860]
// +0x2830] (`lea rcx, [rip + ..]; mov rcx, [rcx]; mov [rcx + 0x2830], rax`); a map's start sets
// its snap byte (`mov byte [rdi + 0x80], 1`) and its update hands it to the cameras.
TEST_F(BloodborneEboot, TheCameraSnapIsWhereTheGameKeepsIt) {
    EXPECT_EQ(CallTarget(0x019379D9), 0x01435FC0u);
    EXPECT_EQ(LeaTarget(Bytes(0x019379F8, 7), 0x019379F8), CameraManagerPointerOffset);
    EXPECT_EQ(Bytes(0x01937A02, 7), (std::vector<u8>{0x48, 0x89, 0x81, 0x30, 0x28, 0x00, 0x00}));
    EXPECT_EQ(ReadAt<u32>(Bytes(0x01937A05, 4), 0), CameraManagerField);
    EXPECT_EQ(Bytes(0x01939C0E, 7), (std::vector<u8>{0xC6, 0x87, 0x80, 0x00, 0x00, 0x00, 0x01}));
    EXPECT_EQ(Bytes(0x01436964, 7), (std::vector<u8>{0x80, 0xBB, 0x80, 0x00, 0x00, 0x00, 0x00}));
    EXPECT_EQ(CameraSnapByte, 0x80u);
}

// The summon manager's update (0x01872360) ages each summon in flight (`vaddss xmm0, xmm0,
// [r12 + 4]` with the frame time at 0x0187291C) and lets it go once it is 55 s old, or 180 s
// old (the two `vucomiss xmm0, [rip + disp32]`); the party's patch points both at a 15.0 among
// the game's constants.
TEST_F(BloodborneEboot, TheSummonSlotHoldsReadTheGamesTimes) {
    const auto float_at = [](u64 offset) {
        const auto bytes = Bytes(offset, sizeof(float));
        float value = 0.0f;
        if (bytes.size() == sizeof(float)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value;
    };
    EXPECT_EQ(Bytes(0x0187291C, 7), (std::vector<u8>{0xC4, 0xC1, 0x7A, 0x58, 0x44, 0x24, 0x04}));
    std::vector<float> holds;
    for (const CoopSite& site : CoopSites) {
        if (site.feature != "summon slot hold") {
            continue;
        }
        const auto bytes = Bytes(site.offset, 8);
        ASSERT_EQ(bytes.size(), 8u);
        EXPECT_EQ(std::vector<u8>(bytes.begin(), bytes.begin() + 4),
                  (std::vector<u8>{0xC5, 0xF8, 0x2E, 0x05}))
            << site.name;
        s32 displacement{};
        std::memcpy(&displacement, bytes.data() + 4, sizeof(displacement));
        holds.push_back(float_at(site.offset + 8 + static_cast<s64>(displacement)));
    }
    EXPECT_EQ(holds, (std::vector<float>{55.0f, 180.0f}));
    EXPECT_EQ(float_at(PartySummonSlotHoldConstant), PartySummonSlotHoldSeconds);
}
