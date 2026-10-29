// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include <gtest/gtest.h>
#ifdef __linux__
#include <atomic>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "core/bloodborne_party.h"
#include "core/bloodborne_write_watch.h"

using Core::Bloodborne::CoopTimeMeter;
using Core::Bloodborne::ParsePartySetting;

// SHADPS4_BLOODBORNE_PARTY=<name>:<secret> becomes the X-ShadPS4-Bloodborne-Party header value.
// The server applies the same rules (NormalizePartyKey), so anything rejected here would be
// rejected there too.

TEST(BloodborneParty, AcceptsNameAndSecret) {
    EXPECT_EQ(ParsePartySetting("hunters:blood"), std::optional<std::string>{"hunters:blood"});
}

TEST(BloodborneParty, TrimsSurroundingWhitespace) {
    EXPECT_EQ(ParsePartySetting("  hunters:blood \t"), std::optional<std::string>{"hunters:blood"});
}

TEST(BloodborneParty, KeepsColonsInsideTheSecret) {
    EXPECT_EQ(ParsePartySetting("hunters:a:b"), std::optional<std::string>{"hunters:a:b"});
}

TEST(BloodborneParty, RejectsEmptyOrMalformed) {
    EXPECT_FALSE(ParsePartySetting(""));
    EXPECT_FALSE(ParsePartySetting("   "));
    EXPECT_FALSE(ParsePartySetting("hunters"));
    EXPECT_FALSE(ParsePartySetting(":blood"));
    EXPECT_FALSE(ParsePartySetting("hunters:"));
}

TEST(BloodborneParty, RejectsControlCharactersSoHeadersCannotBeInjected) {
    EXPECT_FALSE(ParsePartySetting("hunters:blood\r\nX-Evil: 1"));
    EXPECT_FALSE(ParsePartySetting(std::string("hunters:bl\0od", 13)));
}

TEST(BloodborneParty, RejectsOversizedValues) {
    EXPECT_FALSE(ParsePartySetting(std::string(200, 'a') + ":b"));
    EXPECT_TRUE(ParsePartySetting(std::string(126, 'a') + ":b"));
}

// Ada's FPS drop (2026-10-12): the co-op work on the game's threads is measured and logged next
// to the port's Perf lines, once per period, per section: the average and the longest call.
TEST(BloodborneCoopTime, ReportsEachSectionOncePerPeriod) {
    CoopTimeMeter meter{30'000};
    EXPECT_EQ(meter.Report(0), std::nullopt) << "nothing measured";
    meter.Add("tick", 1'000'000, 0);
    meter.Add("tick", 3'000'000, 10);
    meter.Add("enemies", 500'000, 20);
    EXPECT_EQ(meter.Report(29'999), std::nullopt) << "the period is not over";
    EXPECT_EQ(meter.Report(30'000),
              std::optional<std::string>{
                  "tick avg 2.00 ms max 3.00 ms (2 calls), enemies avg 0.50 ms max 0.50 ms (1 "
                  "calls) over 30 s"});
    EXPECT_EQ(meter.Report(30'001), std::nullopt) << "counts start over";
}

// AtiniirSpectre 2026-10-23 (v1.2.0): after a death the co-op tick's longest call went from 21 ms
// to 111 ms. The tick's parts are measured too, and only the slowest few are named, by their
// total time, so a player's log says which part grew.
TEST(BloodborneCoopTime, NamesOnlyTheSlowestSectionsWhenAsked) {
    CoopTimeMeter meter{30'000};
    meter.Add("place", 100'000, 0);
    meter.Add("map screen", 9'000'000, 0);
    meter.Add("trip", 2'000'000, 0);
    meter.Add("trip", 2'000'000, 0);
    EXPECT_EQ(meter.Report(30'000, 2),
              std::optional<std::string>{
                  "map screen avg 9.00 ms max 9.00 ms (1 calls), trip avg 2.00 ms max 2.00 ms "
                  "(2 calls) over 30 s"});
}

using namespace Core::Bloodborne;

// Kaniel 2026-10-15: the host and a guest went to the Hunter's Dream and the guest left behind
// still saw them in Central Yharnam, frozen. Who shows comes from one place, the server's map
// board: another player's character shows while a member on this map stands near it.
TEST(MemberPresence, AnotherPlayerShowsOnlyWhileAMemberOnThisMapStandsThere) {
    constexpr u32 Yharnam = 0x18010000, Dream = 0x15000000;
    const auto member = [](const char* name, u32 map, float x) {
        return MemberVitals{.member = name, .map = map, .vital = Vital::Up, .at = {x, 0.0f, 0.0f}};
    };
    const std::vector<std::optional<LampPosition>> characters{LampPosition{100.0f, 0.0f, 0.0f},
                                                              LampPosition{200.0f, 0.0f, 0.0f}};
    // Both on this map, each near its character.
    std::vector<MemberVitals> board{member("me", Yharnam, 0.0f), member("a", Yharnam, 102.0f),
                                    member("b", Yharnam, 198.0f)};
    EXPECT_EQ(ShownOtherPlayers(characters, board, "me", Yharnam), (std::vector<bool>{true, true}));
    // Both in the Dream: neither shows.
    board = {member("me", Yharnam, 0.0f), member("a", Dream, 102.0f), member("b", Dream, 198.0f)};
    EXPECT_EQ(ShownOtherPlayers(characters, board, "me", Yharnam),
              (std::vector<bool>{false, false}));
    // One back: its character shows, the other's not.
    board = {member("me", Yharnam, 0.0f), member("a", Yharnam, 101.0f), member("b", Dream, 0.0f)};
    EXPECT_EQ(ShownOtherPlayers(characters, board, "me", Yharnam),
              (std::vector<bool>{true, false}));
    // A member loading (map 0) is on no map; this player's own entry never counts.
    board = {member("me", Yharnam, 100.0f), member("a", 0, 100.0f)};
    EXPECT_EQ(ShownOtherPlayers(characters, board, "me", Yharnam),
              (std::vector<bool>{false, false}));
    // A character without a position shows (nothing to tell).
    const std::vector<std::optional<LampPosition>> unknown{std::nullopt};
    EXPECT_EQ(ShownOtherPlayers(unknown, board, "me", Yharnam), (std::vector<bool>{true}));
}

// Kaniel's playtest 2026-10-16: krolebord's game crashed scanning 256 stack slots past the end of a
// worker thread's stack (a watched write). The scan stops where the stack's readable bytes do.
TEST(BloodborneWriteWatch, TheStackScanStaysOnTheStack) {
    EXPECT_EQ(StackSlotsToScan(2000, 256), 250u);
    EXPECT_EQ(StackSlotsToScan(4096, 256), 256u);
    EXPECT_EQ(StackSlotsToScan(7, 256), 0u);
    EXPECT_EQ(StackSlotsToScan(0, 256), 0u);
}

#ifdef __linux__
// The readable bytes the scan gets: up to the first page it cannot read, as on Windows.
TEST(BloodborneWriteWatch, ReadableBytesStopAtTheFirstUnreadablePage) {
    using Core::Bloodborne::ReadableBytesAt;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    // Two readable pages, then one that cannot be read (a stack's guard page).
    auto* base = static_cast<unsigned char*>(
        mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(base, MAP_FAILED);
    ASSERT_EQ(mprotect(base + 2 * page, page, PROT_NONE), 0);
    const u64 at = reinterpret_cast<u64>(base);
    EXPECT_EQ(ReadableBytesAt(at + 100, 16 * page), 2 * page - 100);
    EXPECT_EQ(ReadableBytesAt(at + 100, 256 * 8), 256u * 8); // `most` caps it
    EXPECT_EQ(ReadableBytesAt(at + 2 * page - 8, 256 * 8), 8u);
    EXPECT_EQ(ReadableBytesAt(at + 2 * page, 256 * 8), 0u);
    // Read-only counts; a page unmapped after it ends the run as the guard page does.
    ASSERT_EQ(mprotect(base, page, PROT_READ), 0);
    EXPECT_EQ(ReadableBytesAt(at, page), page);
    ASSERT_EQ(munmap(base + page, 2 * page), 0);
    EXPECT_EQ(ReadableBytesAt(at, 4 * page), page);
    ASSERT_EQ(munmap(base, page), 0);
    EXPECT_EQ(ReadableBytesAt(at, 256 * 8), 0u);
    EXPECT_EQ(ReadableBytesAt(0, 256 * 8), 0u);
}

namespace {
std::atomic<int> watched_hits{0};
std::atomic<u64> watched_rip{0}, watched_rsp{0};
void CountWatchedWrite(u64 rip, u64 rsp) {
    watched_rip = rip;
    watched_rsp = rsp;
    ++watched_hits;
}
} // namespace

// The write watch fires on the writing thread after each write to the byte, and only for that
// byte; the calling thread is left out, as on Windows. Skipped where perf events are refused
// (perf_event_paranoid 3, containers).
TEST(BloodborneWriteWatch, WritesToTheWatchedByteReachTheObserver) {
    using Core::Bloodborne::WatchByteWrites;
    alignas(64) static volatile unsigned char bytes[64]{};
    std::atomic<int> step{0};
    std::thread writer([&] {
        while (step.load() == 0) {
        }
        bytes[1] = 1; // not watched
        bytes[0] = 7;
        bytes[0] = 8;
    });
    if (!WatchByteWrites(reinterpret_cast<u64>(&bytes[0]), CountWatchedWrite)) {
        step = 1;
        writer.join();
        GTEST_SKIP() << "perf_event hardware breakpoints unavailable";
    }
    bytes[0] = 1; // this thread is not armed
    step = 1;
    writer.join();
    EXPECT_EQ(watched_hits.load(), 2);
    EXPECT_NE(watched_rip.load(), 0u);
    EXPECT_NE(watched_rsp.load(), 0u);
}
#endif

// Kaniel's playtest 2026-10-16: krolebord's copy stayed by the Cleric Beast lantern after his Dream
// trip: Lisara stood 16 m away and kept it shown. Each member vouches for one character only.
TEST(MemberPresence, OneMemberKeepsOnlyOneCharacterShown) {
    constexpr u32 Yharnam = 0x18010000, Dream = 0x15000000;
    const auto member = [](std::string name, u32 map, float x) {
        return MemberVitals{.member = name, .map = map, .vital = Vital::Up, .at = {x, 0.0f, 0.0f}};
    };
    const std::vector<std::optional<LampPosition>> characters{LampPosition{0.0f, 0.0f, 0.0f},
                                                              LampPosition{16.0f, 0.0f, 0.0f}};
    const std::vector<MemberVitals> board{member("me", Yharnam, 50.0f),
                                          member("lisara", Yharnam, 0.5f),
                                          member("krolebord", Dream, 16.0f)};
    EXPECT_EQ(ShownOtherPlayers(characters, board, "me", Yharnam),
              (std::vector<bool>{true, false}));
    EXPECT_EQ(VouchingMembers(characters, board, "me", Yharnam),
              (std::vector<std::optional<size_t>>{1, std::nullopt}))
        << "lisara (board entry 1) vouches for the nearer character, nobody for the other";
    const std::vector<std::optional<LampPosition>> unknown{std::nullopt};
    EXPECT_EQ(VouchingMembers(unknown, board, "me", Yharnam),
              (std::vector<std::optional<size_t>>{std::nullopt}));
}

// Player report 2026-10-23 (v1.1.9): AppleML fell off a cliff into the void, woke at the lamp (its
// trip) and saw itself, but Provi's copy of it stayed faded out (+0x20C at 0.0, rig
// void-fall-respawn). It fades back in once the member died and is up again.
TEST(MemberPresence, AFadedOutCopyFadesBackInOnceItsMemberDiedAndIsUpAgain) {
    FadeBack fade;
    EXPECT_FALSE(fade.Tick(1.0f, false, Vital::Up)) << "shown";
    EXPECT_FALSE(fade.Tick(0.6f, false, Vital::Up)) << "still falling: the game's own fade-out";
    EXPECT_FALSE(fade.Tick(0.0f, true, Vital::Dead)) << "dead in the void";
    EXPECT_FALSE(fade.Tick(0.0f, false, std::nullopt)) << "its trip: on no map for a while";
    EXPECT_TRUE(fade.Tick(0.0f, false, Vital::Up)) << "up at the lamp: faded back in";
    EXPECT_FALSE(fade.Tick(1.0f, false, Vital::Up));
    EXPECT_FALSE(fade.Tick(0.0f, false, Vital::Up)) << "faded out again: no death since";
}

TEST(MemberPresence, AFadedOutCopyStaysWhileItsMemberIsDownOrDead) {
    FadeBack fade;
    EXPECT_FALSE(fade.Tick(0.0f, true, Vital::Dead));
    EXPECT_FALSE(fade.Tick(0.0f, true, Vital::Down));
    EXPECT_FALSE(fade.Tick(0.0f, false, std::nullopt));
    EXPECT_TRUE(fade.Tick(0.5f, false, Vital::Up));
    EXPECT_FALSE(fade.Tick(0.5f, false, Vital::Up)) << "once";
}

TEST(MemberPresence, AShownCopyForgetsAnEarlierDeath) {
    FadeBack fade;
    EXPECT_FALSE(fade.Tick(1.0f, true, Vital::Dead)) << "another member's death, this one shown";
    EXPECT_FALSE(fade.Tick(0.5f, false, Vital::Up)) << "a fade-out after that is the game's own";
}

// Rig 2026-10-22 (walls-woods-stairs): a guest joined as the host left for the Ward; both games
// made the other's character, hid it 3 s later (no board member by it yet) and, hidden, its update
// never ran to bring it to where the member stood: 55 s on the game dropped the session. A
// character the game has just made gets PresenceNewCharacterMs to reach its member first.
TEST(MemberPresence, ANewCharacterGetsTimeToReachItsMember) {
    constexpr s64 Made = 10'000;
    EXPECT_FALSE(PresenceHides(Made + 3'500, Made, Made + 100)) << "just made: not yet";
    EXPECT_FALSE(PresenceHides(Made + PresenceNewCharacterMs - 1, Made, Made + 100));
    EXPECT_TRUE(PresenceHides(Made + PresenceNewCharacterMs, Made, Made + 100))
        << "still by no member once it had its time";
    constexpr s64 Old = 0;
    EXPECT_FALSE(PresenceHides(50'000 + PresenceGraceMs - 1, Old, 50'000)) << "a sprint's grace";
    EXPECT_TRUE(PresenceHides(50'000 + PresenceGraceMs, Old, 50'000)) << "a member gone";
}

// Kaniel's playtest 2026-10-16: krolebord joined in place and never saw Lisara (her task stayed at
// step 1): a remote player's task before its character is made waits for its data.
TEST(MemberPresence, AJoinStillBeforeItsCharacterAsksForTheData) {
    EXPECT_TRUE(WaitsForMemberData(RemoteGuestTask, 1));
    EXPECT_TRUE(WaitsForMemberData(RemoteHostTask, 3));
    EXPECT_FALSE(WaitsForMemberData(RemoteGuestTask, CreateCharacterStep));
    EXPECT_FALSE(WaitsForMemberData(RemoteGuestTask, InSessionStep));
    EXPECT_FALSE(WaitsForMemberData(LocalTaskKind(false), 1)) << "its own task";
}

// Kaniel's playtest 2026-10-19: Kaniel's game crashed and he joined back; Lisara's and krolebord's
// games crashed (a double free at 0x020B56D2): his old copy had stayed, the game bound it to his
// new join as well, and both copies took his packets off one queue on two threads. Of the copies
// of one member, all but the one seen last stay hidden.
TEST(MemberPresence, OnlyTheNewestCopyOfAMemberShows) {
    // Kaniel's old copy (seen first) and his new one share his identity; Lisara's is her own.
    EXPECT_EQ(StaleDuplicatePlayers(std::vector<u64>{0x6440, 0x6800, 0x6440},
                                    std::vector<s64>{100, 200, 900}),
              (std::vector<bool>{true, false, false}));
    // Order does not matter, only when each was seen.
    EXPECT_EQ(StaleDuplicatePlayers(std::vector<u64>{0x6440, 0x6440}, std::vector<s64>{900, 100}),
              (std::vector<bool>{false, true}));
    // Seen in the same tick: the later one in the list is kept.
    EXPECT_EQ(StaleDuplicatePlayers(std::vector<u64>{0x6440, 0x6440}, std::vector<s64>{5, 5}),
              (std::vector<bool>{true, false}));
    // An unknown identity (0) is no one's.
    EXPECT_EQ(StaleDuplicatePlayers(std::vector<u64>{0, 0, 0x6800}, std::vector<s64>{1, 2, 3}),
              (std::vector<bool>{false, false, false}));
}

// Kaniel's crash 2026-10-19 (a call to 0x47F8 from 0x01524B24): Lisara's hidden copy kept the
// object it stood on in a map block his game had since dropped; the memory was reused and the
// object's table pointed at the heap. A game object's table is in the game's image.
TEST(MemberPresence, AGoneContactObjectsTableIsOutsideTheImage) {
    constexpr u64 Base = 0x8'0000'0000, Size = 0x0600'0000;
    EXPECT_TRUE(InGameImage(0x8'0538'DF00, Base, Size));
    EXPECT_FALSE(InGameImage(0x10'1FAA'8D40, Base, Size)) << "the heap";
    EXPECT_FALSE(InGameImage(0x47F8, Base, Size));
    EXPECT_FALSE(InGameImage(Base + Size, Base, Size));
    EXPECT_TRUE(InGameImage(Base, Base, Size));
}

// Kaniel's playtest 2026-10-19: two copies of Kaniel in Lisara's game (his old one, bound again to
// his new join, and the new one) took his packets off one queue on two threads: a double free.
// Of the network states with one identity, only the one seen last takes them.
TEST(MemberPresence, OneCopyOfAMemberTakesItsPackets) {
    MemberPacketTakers takers;
    // His old copy alone takes them.
    EXPECT_TRUE(takers.Takes(0x1020, 0x6440, 0));
    EXPECT_TRUE(takers.Takes(0x1020, 0x6440, 16));
    // Lisara's copy is another member's.
    EXPECT_TRUE(takers.Takes(0x3000, 0x6800, 16));
    // His new copy comes: it takes them, the old one no longer does.
    EXPECT_TRUE(takers.Takes(0x2040, 0x6440, 33));
    EXPECT_FALSE(takers.Takes(0x1020, 0x6440, 33));
    EXPECT_TRUE(takers.Takes(0x2040, 0x6440, 50));
    EXPECT_FALSE(takers.Takes(0x1020, 0x6440, 50));
    EXPECT_TRUE(takers.Takes(0x3000, 0x6800, 50));
    // The new copy gone (not seen for a while): the old one takes them again.
    EXPECT_TRUE(takers.Takes(0x1020, 0x6440, 50 + MemberPacketTakers::ForgetMs + 1));
    // A state whose memory now holds another member's starts over as that member's (seen anew).
    const s64 later = 50 + MemberPacketTakers::ForgetMs + 1;
    EXPECT_TRUE(takers.Takes(0x3000, 0x6800, later));
    EXPECT_TRUE(takers.Takes(0x1020, 0x6800, later + 1));
    EXPECT_FALSE(takers.Takes(0x3000, 0x6800, later + 1));
    // An unknown identity is never held back.
    EXPECT_TRUE(takers.Takes(0x4000, 0, 0));
    EXPECT_TRUE(takers.Takes(0x5000, 0, 0));
}
