// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_guest_hooks.h"
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/uio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cstdio>
#include <vector>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);
extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
constexpr u64 ImageBase = 0x800000000ull;

// The game's code calls into the hooks (the stubs, a call site's memcpy) with the System V ABI;
// on Windows the host's default is the Microsoft one.
#ifdef _WIN32
#define GUEST_ABI __attribute__((sysv_abi))
#else
#define GUEST_ABI
#endif

#ifndef _WIN32
/// The protection of the mapping holding `address` (/proc/self/maps), -1 when none does; as
/// bbcoop/guest_hooks.cpp reads it for its code writes.
int PageProtection(u64 address) {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        return -1;
    }
    int protection = -1;
    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0, end = 0;
        char perms[5] = {};
        if (std::sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3 && address >= start &&
            address < end) {
            protection = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                         (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    std::fclose(maps);
    return protection;
}
#endif

/// Makes [address, address + size) of the game's code writable; Restore puts back the protection
/// each page had and flushes the instruction cache there. The hooks go in while the loader still
/// writes the image (its patches follow, probe.c apply_patches): read-execute there stopped the
/// patches on Linux (Windows kept the previous protection all along).
struct CodeWrite {
    void* page = nullptr;
    std::size_t span = 0;
#ifdef _WIN32
    DWORD previous = 0;
#else
    std::vector<int> previous;
#endif
    bool Open(u64 address, std::size_t size) {
#ifdef _WIN32
        page = reinterpret_cast<void*>(address);
        span = size;
        return VirtualProtect(page, span, PAGE_EXECUTE_READWRITE, &previous) != 0;
#else
        const u64 page_size = static_cast<u64>(sysconf(_SC_PAGESIZE));
        page = reinterpret_cast<void*>(address & ~(page_size - 1));
        span = (address + size) - reinterpret_cast<u64>(page);
        previous.clear();
        for (u64 at = reinterpret_cast<u64>(page); at < address + size; at += page_size) {
            const int protection = PageProtection(at);
            if (protection < 0) {
                return false;
            }
            previous.push_back(protection);
        }
        return mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
    }
    void Restore() {
#ifdef _WIN32
        DWORD unused = 0;
        VirtualProtect(page, span, previous, &unused);
        FlushInstructionCache(GetCurrentProcess(), page, span);
#else
        const u64 page_size = static_cast<u64>(sysconf(_SC_PAGESIZE));
        for (std::size_t index = 0; index < previous.size(); ++index) {
            mprotect(static_cast<char*>(page) + index * page_size, page_size, previous[index]);
        }
        __builtin___clear_cache(static_cast<char*>(page), static_cast<char*>(page) + span);
#endif
    }
};

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// BB_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// BB_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

/// The game's memcpy at those sites: the GPU side is told before (its write traps lift: no fault
/// per page) and after the copy (the data now there), as for file reads.
GUEST_ABI void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

/// bbport: native detours (default; BB_GUEST_HOOK_TRAPS=1: int3 traps). Each hooked site jumps to
/// a stub next to the image that calls the handler and runs the instructions the jump covers;
/// the copy call sites call LoaderCopy through a trampoline there. An int3 trap costs a signal
/// (a few microseconds in the kernel) per call; a detour costs a call. The int3 written first
/// stays the fallback while the jump's bytes go in: OnTrap sends a thread that meets it to the stub.
struct NativeSite {
    u64 offset;
    std::atomic<u64> stub{0};
};
std::array<NativeSite, 4> native_sites{{{AllocReturn}, {HeapAllocReturns[0]}, {HeapAllocReturns[1]},
                                        {HeapAllocReturns[2]}}};
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 16384;

/// The stub page at `hint`, or null when that place is taken.
u8* MapStubPageAt(u64 hint) {
#ifdef _WIN32
    // Plain memory: the guest range's placeholder reservation (runtime_memory.c) starts above
    // the image.
    void* p = VirtualAlloc(reinterpret_cast<void*>(hint), StubPageSize, MEM_RESERVE | MEM_COMMIT,
                           PAGE_EXECUTE_READWRITE);
    return static_cast<u8*>(p);
#else
    void* p = mmap(reinterpret_cast<void*>(hint), StubPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p != MAP_FAILED ? static_cast<u8*>(p) : nullptr;
#endif
}

/// A page the hooked code reaches with a rel32 jump: below the image first, then above its end.
/// The gigabyte below the image can be taken by others: on a player's PC (2026-10-25, NVIDIA,
/// DLSS frame generation) every place there was, and no hook went in (the output kept 16:9).
u8* MapStubPage() {
    constexpr u64 Step = 1ull << 20;
    constexpr u64 Reach = 0x70000000;       // the farthest site (~0x2ac0000) stays within 2 GiB
    constexpr u64 AboveImage = 0x10000000; // past the image and its linked libraries (~0x5800000)
    for (u64 d = 16ull << 20; d <= Reach; d += Step) {
        if (u8* p = MapStubPageAt(ImageBase - d)) {
            return p;
        }
    }
    for (u64 d = AboveImage; d <= Reach; d += Step) {
        if (u8* p = MapStubPageAt(ImageBase + d)) {
            return p;
        }
    }
    return nullptr;
}

/// The stub page, written: read-execute from now on.
void SealStubPage() {
#ifdef _WIN32
    DWORD old_protect = 0;
    VirtualProtect(stub_page, StubPageSize, PAGE_EXECUTE_READ, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), stub_page, StubPageSize);
#else
    mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
#endif
}

struct Emitter {
    std::array<u8, 512> code{};
    std::size_t size = 0;
    bool overflow = false; ///< more than `code` holds: PlaceStub refuses it
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            if (size < code.size()) {
                code[size] = b;
            }
            ++size;
        }
        overflow = overflow || size > code.size();
    }
    void U32(std::uint32_t v) {
        u8 b[4];
        std::memcpy(b, &v, 4);
        Bytes({b[0], b[1], b[2], b[3]});
    }
    void U64(u64 v) {
        U32(std::uint32_t(v));
        U32(std::uint32_t(v >> 32));
    }
    /// xmm0..15 below the aligned stack (after SaveVolatile): the game's float values around a
    /// handler that may change them (deadinside28's bbport 0.6, bbport_guest_code.h).
    void SaveXmm() {
        Bytes({0x48, 0x81, 0xec}); // sub rsp, 256
        U32(256);
        for (u8 i = 0; i < 16; ++i) { // movdqu [rsp + 16 i], xmm_i
            Bytes({0xf3});
            if (i >= 8) {
                Bytes({0x44});
            }
            Bytes({0x0f, 0x7f, u8(0x84 | (i & 7) << 3), 0x24});
            U32(16u * i);
        }
    }
    void RestoreXmm() {
        for (u8 i = 0; i < 16; ++i) { // movdqu xmm_i, [rsp + 16 i]
            Bytes({0xf3});
            if (i >= 8) {
                Bytes({0x44});
            }
            Bytes({0x0f, 0x6f, u8(0x84 | (i & 7) << 3), 0x24});
            U32(16u * i);
        }
        Bytes({0x48, 0x81, 0xc4}); // add rsp, 256
        U32(256);
    }
    /// jmp [rip]: to `target` anywhere.
    void JmpAbs(u64 target) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(target);
    }
    /// Past (back over) the 128-byte red zone below rsp the game's code may keep values in.
    void SkipRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); } // lea rsp, [rsp - 128]
    void UnskipRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); } // lea rsp, [rsp + 128]
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
};

/// Copies `e` into the stub page; returns its address there, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!stub_page || e.overflow || stub_used + e.size > StubPageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Replaces bytes [1, 5) at `address` (byte 0 is the trap already there), then byte 0.
void WriteJumpOverTrap(u64 address, u8 opcode, std::uint32_t rel) {
    CodeWrite code;
    code.Open(address, 5);
    auto* p = reinterpret_cast<u8*>(address);
    for (int i = 0; i < 4; ++i) {
        __atomic_store_n(p + 1 + i, u8(rel >> (8 * i)), __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p, opcode, __ATOMIC_SEQ_CST);
    code.Restore();
}

GUEST_ABI void AllocHook(u64 node) {
    if (!node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        on_range_allocated(start, end - start);
    }
}

GUEST_ABI void HeapHook(u64 address, u64 size) {
    if (address != 0 && size != 0 && size < (u64(1) << 32)) {
        on_range_allocated(address, size);
    }
}

/// The detour of a site whose int3 is in place (Patch). False: left as a trap.
bool InstallDetour(NativeSite& site) {
    const u64 address = ImageBase + site.offset;
    Emitter e;
    u64 back = 0;
    if (site.offset == AllocReturn) {
        // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
        if (reinterpret_cast<const u8*>(address)[4] != 0x5b) {
            return false;
        }
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax (unchanged by the saving)
        e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
        e.Bytes({0x5b});                   // pop rbx
        back = address + 5;
    } else {
        // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
        const auto* code = reinterpret_cast<const u8*>(address);
        if (code[3] != 0x48 || code[4] != 0x85 || code[5] != 0xdb) {
            return false;
        }
        e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
        e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
        e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
        back = address + 6;
    }
    // jmp back
    const u64 jmp_at = reinterpret_cast<u64>(stub_page + stub_used) + e.size;
    std::uint32_t rel_back = 0;
    if (!Rel32To(jmp_at + 5, back, rel_back)) {
        return false;
    }
    e.Bytes({0xe9});
    e.U32(rel_back);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    site.stub.store(stub, std::memory_order_release); // a thread at the trap now goes there
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The GX swap-chain's per-frame render command: its 16-byte node comes from a per-frame bump
/// allocator that returns null once the frame's heap is used up; the game writes through it
/// unchecked (`mov [r12 + 8], r13` at 0x2199310, then posts it to the render thread): a guest
/// fault at address 8. Without a node the post is skipped and the pending flag (this + 0x3ab)
/// stays set, so the command goes out with the next frame. From deadinside28's bbport 0.6
/// (InstallGxNodeGuard; their issues #162, #192).
constexpr u64 GxNodeSite = 0x2199310;
constexpr u64 GxNodeSkip = 0x219934e; // past the post and the flag's clear
constexpr unsigned char GxNodeCode[9] = {0x4d, 0x89, 0x6c, 0x24, 0x08,  // mov [r12 + 8], r13
                                         0x49, 0x8b, 0x87, 0xb0};        // mov rax, [r15 + 0xb0]
std::atomic<u64> gx_node_skip_trap{0}; ///< the stub's int3 for a null node (OnTrapAt)

bool InstallGxNodeGuard() {
    const u64 address = ImageBase + GxNodeSite;
    if (std::memcmp(reinterpret_cast<const void*>(address), GxNodeCode, sizeof(GxNodeCode)) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0x4d, 0x85, 0xe4});                   // test r12, r12
    e.Bytes({0x74, 0x13});                         // jz null (past the next 19 bytes)
    e.Bytes({0x4d, 0x89, 0x6c, 0x24, 0x08});       // mov [r12 + 8], r13
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + 5);
    const std::size_t trap_at = e.size;
    e.Bytes({0xcc}); // null: OnTrapAt counts it and skips the post
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    gx_node_skip_trap.store(stub + trap_at, std::memory_order_release);
    CodeWrite code;
    if (!code.Open(address, 5)) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<u8*>(address), u8{0xcc}, __ATOMIC_SEQ_CST); // OnTrapAt runs
    code.Restore();                                                             // it meanwhile
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The copy call sites: the call's target becomes a trampoline to LoaderCopy.
bool InstallCallDetour(u64 offset) {
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    const u64 stub = PlaceStub(e);
    const u64 address = ImageBase + offset;
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe8, rel);
    return true;
}
/// The registers a trap reads and changes: the thread's saved context (Linux ucontext gregs,
/// Windows CONTEXT).
struct TrapRegs {
    u64* rip;
    u64* rax;
    u64* rbx;
    u64* rsp;
    u64* rbp;
    u64* rdi;
    u64* r12;
    u64* r13;
};

/// A thread at the int3 written at `rip`: runs what the site does and moves it on. False when the
/// trap is not one of these hooks.
u64 LiveStubFor(u64 rip); // the live detours below

bool OnTrapAt(u64 rip, const TrapRegs& g) {
    // A detour over the game's running code going in (DetourLive): its stub runs the site.
    if (const u64 stub = LiveStubFor(rip)) {
        *g.rip = stub;
        return true;
    }
    // The GX node guard: its site while the detour goes in, and its null case.
    const bool gx_site = rip == ImageBase + GxNodeSite;
    if (gx_site || (rip != 0 && rip == gx_node_skip_trap.load(std::memory_order_acquire))) {
        if (gx_site && *g.r12 != 0) {
            *reinterpret_cast<u64*>(*g.r12 + 8) = *g.r13;
            *g.rip = ImageBase + GxNodeSite + 5;
            return true;
        }
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            std::printf("Guest hooks: the GX per-frame command heap was full: a render command "
                        "waits for the next frame\n");
        }
        *g.rip = ImageBase + GxNodeSkip;
        return true;
    }
    for (const auto& site : native_sites) {
        if (rip == ImageBase + site.offset) {
            if (const u64 stub = site.stub.load(std::memory_order_acquire)) {
                *g.rip = stub; // the detour is going in: its stub runs the site
                return true;
            }
        }
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(*g.rax & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        *g.rip = ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2);
        return true;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = *g.rdi;
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        *g.rsp -= 8; // push rbp
        *reinterpret_cast<u64*>(*g.rsp) = *g.rbp;
        *g.rip = ImageBase + CollectorEntry + 1;
        return true;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = *g.rax;
        const u64 size = *reinterpret_cast<const u64*>(*g.rbp - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32)) {
            on_range_allocated(address, size);
        }
        *g.rbx = *g.rax;
        *g.rip = ImageBase + site + sizeof(HeapAllocReturnCode);
        return true;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = *g.rax) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        *g.rsp += 0x18;
        *g.rip = ImageBase + AllocReturn + sizeof(AllocReturnCode);
        return true;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.offset + 5;
        *g.rsp -= 8;
        *reinterpret_cast<u64*>(*g.rsp) = ret;
        *g.rip = reinterpret_cast<u64>(&LoaderCopy);
        return true;
    }
    return false;
}

#ifdef _WIN32
/// Windows: the traps arrive as breakpoint exceptions (first in the vectored handler chain; the
/// fault handler of probe.c does not take breakpoints). ExceptionAddress is the int3 itself.
LONG CALLBACK OnTrapException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = info->ContextRecord;
    const TrapRegs regs{reinterpret_cast<u64*>(&c->Rip), reinterpret_cast<u64*>(&c->Rax),
                        reinterpret_cast<u64*>(&c->Rbx), reinterpret_cast<u64*>(&c->Rsp),
                        reinterpret_cast<u64*>(&c->Rbp), reinterpret_cast<u64*>(&c->Rdi),
                        reinterpret_cast<u64*>(&c->R12), reinterpret_cast<u64*>(&c->R13)};
    const u64 rip = reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress);
    return OnTrapAt(rip, regs) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#else
struct sigaction previous_action {};

void OnTrap(int sig, siginfo_t* info, void* context) {
    auto* g = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    const u64 rip = u64(g[REG_RIP]) - 1; // past the int3
    static_assert(sizeof(greg_t) == sizeof(u64));
    const TrapRegs regs{reinterpret_cast<u64*>(&g[REG_RIP]), reinterpret_cast<u64*>(&g[REG_RAX]),
                        reinterpret_cast<u64*>(&g[REG_RBX]), reinterpret_cast<u64*>(&g[REG_RSP]),
                        reinterpret_cast<u64*>(&g[REG_RBP]), reinterpret_cast<u64*>(&g[REG_RDI]),
                        reinterpret_cast<u64*>(&g[REG_R12]), reinterpret_cast<u64*>(&g[REG_R13])};
    if (OnTrapAt(rip, regs)) {
        return;
    }
    if (previous_action.sa_flags & SA_SIGINFO) {
        if (previous_action.sa_sigaction) {
            previous_action.sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous_action.sa_handler != SIG_DFL && previous_action.sa_handler != SIG_IGN &&
               previous_action.sa_handler) {
        previous_action.sa_handler(sig);
        return;
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}
#endif

bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    unsigned char bytes[16]{};
    iovec local{bytes, size}, remote{reinterpret_cast<void*>(address), size};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(size) ||
        std::memcmp(bytes, expected, size) != 0) {
        return false;
    }
    CodeWrite code;
    if (!code.Open(address, size)) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    code.Restore();
    return true;
}

/// Render jobs (deadinside28 0.6, the Laurence cinematic above 30 FPS): a runner whose job failed
/// (result 1) keeps the slot it took from the job pool (+0x120), and with every slot held no job
/// runs again: the cinematic stopped. The slot goes back to the pool through the game's own
/// 0x1056990, at the stack check every exit of the job function (0x1068092) ends in, and when a
/// runner is destroyed or reset with a slot still held (0x106798a, 0x1067a5c).
std::atomic<u64> job_slots_released{0};

void NoteJobSlotReleased(const char* how) {
    const u64 n = job_slots_released.fetch_add(1) + 1;
    if (n <= 5 || (n & (n - 1)) == 0) {
        std::printf("Guest hooks: %s gave its render job slot back (%llu so far)\n", how,
                    static_cast<unsigned long long>(n));
    }
}

GUEST_ABI void JobSlotFix(u64 runner, u64 queue, std::uint32_t result, std::uint32_t) {
    if (result != 1 || !runtime_memory_is_mapped(runner + 0x120, 8)) {
        return;
    }
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    const u64 item = held;
    if (!item || !queue) {
        return;
    }
    using GiveBack = void(GUEST_ABI*)(u64 queue, u64 item);
    reinterpret_cast<GiveBack>(ImageBase + 0x1056990)(queue, item);
    held = 0;
    NoteJobSlotReleased("a failed render job");
}

GUEST_ABI void JobSlotDrop(u64 runner) {
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    const u64 item = held;
    if (!item) {
        return;
    }
    const u64 queue = *reinterpret_cast<const u64*>(ImageBase + 0x54b8788);
    if (!queue) {
        return;
    }
    using GiveBack = void(GUEST_ABI*)(u64 queue, u64 item);
    reinterpret_cast<GiveBack>(ImageBase + 0x1056990)(queue, item);
    held = 0;
    NoteJobSlotReleased("a render job destroyed with a slot");
}

bool InstallJobSlotDropHook(u64 offset) {
    // mov qword [rbx + 0x120], 0
    static constexpr std::array<u8, 11> code = {0x48, 0xc7, 0x83, 0x20, 0x01, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), code.data(), code.size())) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx (the runner)
    e.CallAbs(reinterpret_cast<const void*>(&JobSlotDrop));
    e.RestoreVolatile();
    for (const u8 b : code) {
        e.Bytes({b});
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + code.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// BB_JOB_SLOT_FIX=0: not installed.
bool InstallJobSlotFix() {
    if (const char* env = std::getenv("BB_JOB_SLOT_FIX"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 offset = 0x1068092;
    // mov rax, [rbx]; cmp rax, [rbp-0x30]: the stack check every exit of that function ends in.
    static constexpr std::array<u8, 7> code = {0x48, 0x8b, 0x03, 0x48, 0x3b, 0x45, 0xd0};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), code.data(), code.size())) {
        return false;
    }
    // The destroy/reset sites first: all three or none.
    static constexpr std::array<u8, 11> drop = {0x48, 0xc7, 0x83, 0x20, 0x01, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00};
    for (const u64 site : {u64{0x106798a}, u64{0x1067a5c}}) {
        if (std::memcmp(reinterpret_cast<const void*>(ImageBase + site), drop.data(), drop.size())) {
            return false;
        }
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x4c, 0x89, 0xf7}); // mov rdi, r14 (the runner)
    // SaveVolatile keeps its stack anchor in r12; the game's r12 (the pool) is saved at [r12].
    e.Bytes({0x49, 0x8b, 0x34, 0x24}); // mov rsi, [r12] (the saved pool)
    e.Bytes({0x44, 0x89, 0xfa});       // mov edx, r15d (the result)
    e.Bytes({0x89, 0xc1});             // mov ecx, eax (the original error)
    e.CallAbs(reinterpret_cast<const void*>(&JobSlotFix));
    e.RestoreVolatile();
    for (const u8 b : code) {
        e.Bytes({b}); // the two instructions the jump covers
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + code.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    const bool dropped = InstallJobSlotDropHook(0x106798a);
    return InstallJobSlotDropHook(0x1067a5c) && dropped;
}

/// Yebis's blur-kernel sample parameters (GPUTexUtil_GetRecursiveSampleParameters, 0xbbc3e0): the
/// base sample count, its first argument, must be at least 3 (an assertion the retail build
/// compiled out). Game data now and then gives 2; the function then runs on in a state that
/// does not end (the guest stops submitting: a black screen, crash 0xa0020007). The count is
/// raised to 3 at the entry (deadinside28 0.6, their #207). BB_KERNEL_CLAMP=0: not installed.
bool InstallKernelSampleClamp() {
    if (const char* env = std::getenv("BB_KERNEL_CLAMP"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 offset = 0xbbc3e0;
    // push rbp; mov rbp, rsp; push r15
    static constexpr std::array<u8, 6> entry = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), entry.data(), entry.size())) {
        return false;
    }
    Emitter e;
    e.Bytes({0x83, 0xff, 0x03});             // cmp edi, 3
    e.Bytes({0x7d, 0x05});                   // jge over
    e.Bytes({0xbf, 0x03, 0x00, 0x00, 0x00}); // mov edi, 3
    for (const u8 b : entry) {
        e.Bytes({b}); // the instructions the jump covers
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + entry.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

// ---- The output's aspect ratio, live. Ported from deadinside28's bbport 0.6 (8185b88,
// gpu/shim/bbport_guest_hooks.cpp: "any aspect ratio, live, through hooks"; GPL-2.0-or-later,
// the same lineage as this port): the camera's aspect ratio, the HUD markers' places, the
// Scaleform movies' pixel aspect ratio and the HUD's groups at the output's edges. ----

/// Detours over code the game may be running while they go in: the int3 first (a thread there
/// meanwhile goes to the stub, OnTrapAt), the covered bytes past the jump as int3s too (a thread
/// found at `second`, an instruction among them, goes to `second_stub`), then the jump's bytes and
/// its opcode.
struct LiveSite {
    std::atomic<u64> address{0}, stub{0};
    std::atomic<u64> second{0}, second_stub{0};
};
std::array<LiveSite, 16> live_sites;
std::atomic<std::size_t> live_count{0};

u64 LiveStubFor(u64 rip) {
    const std::size_t n = live_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        const LiveSite& site = live_sites[i];
        if (rip == site.address.load(std::memory_order_acquire)) {
            return site.stub.load(std::memory_order_acquire);
        }
        if (rip != 0 && rip == site.second.load(std::memory_order_acquire)) {
            return site.second_stub.load(std::memory_order_acquire);
        }
    }
    return 0;
}

bool DetourLive(u64 address, u64 stub, std::size_t covered, u64 second = 0, u64 second_stub = 0) {
    std::uint32_t rel = 0;
    const std::size_t index = live_count.load(std::memory_order_relaxed);
    if (!stub || covered < 5 || index == live_sites.size() || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    LiveSite& site = live_sites[index];
    site.stub.store(stub, std::memory_order_release);
    site.second_stub.store(second_stub, std::memory_order_release);
    site.second.store(second, std::memory_order_release);
    site.address.store(address, std::memory_order_release);
    live_count.store(index + 1, std::memory_order_release);
    CodeWrite code;
    if (!code.Open(address, covered)) {
        return false;
    }
    auto* bytes = reinterpret_cast<u8*>(address);
    __atomic_store_n(bytes, u8{0xcc}, __ATOMIC_SEQ_CST);
    for (std::size_t i = 5; i < covered; ++i) {
        __atomic_store_n(bytes + i, u8{0xcc}, __ATOMIC_SEQ_CST);
    }
    code.Restore();
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The camera manager (0x1436b..) copies the active camera's projection parameters into the
/// current camera every frame (+0x50 vertical field of view, +0x54 aspect ratio, +0x58 near, +0x5c
/// far: four plain copies, and a blend of two cameras, one of them per update); the projection
/// (0x1d16830), the culling and the HUD's markers read them there. After the copy the output's
/// aspect ratio replaces the game's 1.777 when one is set (SetDisplayAspect): a wider output sees
/// more to the sides at the same height; a taller one keeps the width and sees more above and
/// below (the field of view widened: 2 atan(tan(fov / 2) * 16:9 / aspect), x87).
alignas(8) std::atomic<std::uint32_t> camera_aspect_bits{0}; ///< float bits; 0: the game's
struct alignas(8) FovScale {
    std::atomic<std::uint32_t> bits{0}; ///< float: the tangent's factor; 0: the game's field of view
    float half = 0.5f;
};
FovScale camera_fov_scale;
std::atomic<int> camera_sites{0};
struct ParamStore {
    u64 offset;
    std::array<u8, 5> code; ///< the store the jump covers (no RIP-relative operand)
};
constexpr std::array<ParamStore, 5> CameraParamStores = {{
    {0x1436c2c, {0xc5, 0xfa, 0x11, 0x43, 0x5c}}, // vmovss %xmm0, 0x5c(%rbx): the follow camera
    {0x1436d8a, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436e68, {0xc5, 0xf8, 0x11, 0x4b, 0x50}}, // vmovups %xmm1, 0x50(%rbx): a blend of two
    {0x1436eec, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436f4b, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
}};

int InstallCameraAspectHooks() {
    int installed = 0;
    for (const auto& site : CameraParamStores) {
        const u64 address = ImageBase + site.offset;
        if (std::memcmp(reinterpret_cast<const void*>(address), site.code.data(), site.code.size()) != 0) {
            continue;
        }
        Emitter e;
        for (const u8 b : site.code) {
            e.Bytes({b});
        }
        e.SkipRedZone();
        e.Bytes({0x9c, 0x50});                   // pushfq; push rax
        e.Bytes({0x48, 0xb8});                   // movabs rax, &camera_aspect_bits
        e.U64(reinterpret_cast<u64>(&camera_aspect_bits));
        e.Bytes({0x8b, 0x00});                   // mov eax, [rax]
        e.Bytes({0x85, 0xc0, 0x74, 0x03});       // test eax, eax; je over the store
        e.Bytes({0x89, 0x43, 0x54});             // mov [rbx + 0x54], eax
        e.Bytes({0x48, 0xb8});                   // movabs rax, &camera_fov_scale
        e.U64(reinterpret_cast<u64>(&camera_fov_scale));
        e.Bytes({0x83, 0x38, 0x00, 0x74, 0x15}); // cmp dword [rax], 0; je over the x87 part
        e.Bytes({0xd9, 0x43, 0x50});             // fld dword [rbx + 0x50]: fov
        e.Bytes({0xd8, 0x48, 0x04});             // fmul dword [rax + 4]: fov / 2
        e.Bytes({0xd9, 0xf2, 0xdd, 0xd8});       // fptan; fstp st0 (its 1.0)
        e.Bytes({0xd8, 0x08});                   // fmul dword [rax]: tan * factor
        e.Bytes({0xd9, 0xe8, 0xd9, 0xf3});       // fld1; fpatan: atan(tan * factor)
        e.Bytes({0xd8, 0xc0});                   // fadd st0, st0
        e.Bytes({0xd9, 0x5b, 0x50});             // fstp dword [rbx + 0x50]
        e.Bytes({0x58, 0x9d});                   // pop rax; popfq
        e.UnskipRedZone();
        e.JmpAbs(address + site.code.size());
        installed += DetourLive(address, PlaceStub(e), site.code.size());
    }
    camera_sites.store(installed, std::memory_order_release);
    return installed;
}

/// The HUD's markers over characters (lock-on, enemies' health bars, damage numbers): projected
/// with the current camera, then placed in the game's 1920x1080 UI by `* (0.5, -0.5, ..) +
/// (0.5, 0.5, ..)` (three places), and drawn with the UI. On a wider (taller) output the UI's
/// 16:9 screen is the middle of a wider view at the output's height (width): there a marker's 0.5
/// becomes 0.5 * output aspect / 16:9 (16:9 / aspect for y), so it lands where its character is.
/// The detour multiplies by the port's vector instead of the game's constant; a marker past the
/// UI's edges is hidden by the game itself, as off screen.
struct MarkerSite {
    u64 offset;
    u64 scale, bias; ///< the constants the two instructions read
};
constexpr std::array<MarkerSite, 3> MarkerSites = {{
    {0x1644379, 0x48f9a00, 0x48f9a10},
    {0x1644c45, 0x48f9a60, 0x48f9a70},
    {0x16452b7, 0x48f9ad0, 0x48f9ae0},
}};
struct alignas(16) MarkerScale {
    float game[4];                     ///< the game's constant
    std::atomic<std::uint32_t> now[4]; ///< what the detour multiplies by
};
std::array<MarkerScale, MarkerSites.size()> marker_scales{};
std::atomic<int> marker_sites{0};
std::atomic<float> marker_kx{1.0f}, marker_ky{1.0f}; ///< SetDisplayAspect's, kept for a later install

void UpdateMarkerScales() {
    const float kx = marker_kx.load(std::memory_order_relaxed), ky = marker_ky.load(std::memory_order_relaxed);
    for (MarkerScale& m : marker_scales) {
        const float now[4] = {m.game[0] * kx, m.game[1] * ky, m.game[2], m.game[3]};
        for (int c = 0; c < 4; ++c) {
            std::uint32_t v;
            std::memcpy(&v, &now[c], 4);
            m.now[c].store(v, std::memory_order_relaxed);
        }
    }
}

/// `vmulps disp(%rip), %xmm0, %xmm0; vaddps disp(%rip), %xmm0, %xmm0` reading `scale`, `bias`.
bool MarkerCodeAt(u64 offset, u64 scale, u64 bias) {
    const auto* p = reinterpret_cast<const u8*>(ImageBase + offset);
    const auto reads = [&](int at, u8 opcode, u64 target) {
        std::int32_t disp;
        std::memcpy(&disp, p + at + 4, 4);
        return p[at] == 0xc5 && p[at + 1] == 0xf8 && p[at + 2] == opcode && p[at + 3] == 0x05 &&
               offset + at + 8 + u64(std::int64_t(disp)) == target;
    };
    return reads(0, 0x59, scale) && reads(8, 0x58, bias);
}

int InstallMarkerHooks() {
    int installed = 0;
    for (std::size_t i = 0; i < MarkerSites.size(); ++i) {
        const MarkerSite& site = MarkerSites[i];
        if (!MarkerCodeAt(site.offset, site.scale, site.bias)) {
            continue;
        }
        MarkerScale& m = marker_scales[i];
        std::memcpy(m.game, reinterpret_cast<const void*>(ImageBase + site.scale), sizeof(m.game));
        UpdateMarkerScales();
        const u64 address = ImageBase + site.offset;
        Emitter e;
        e.SkipRedZone();
        e.Bytes({0x50});                   // push rax
        e.Bytes({0x48, 0xb8});             // movabs rax, &now
        e.U64(reinterpret_cast<u64>(&m.now));
        e.Bytes({0xc5, 0xf8, 0x59, 0x00}); // vmulps xmm0, xmm0, [rax]
        e.Bytes({0x48, 0xb8});             // movabs rax, the game's bias
        e.U64(ImageBase + site.bias);
        e.Bytes({0xc5, 0xf8, 0x58, 0x00}); // vaddps xmm0, xmm0, [rax]
        e.Bytes({0x58});                   // pop rax
        e.UnskipRedZone();
        e.JmpAbs(address + 16);
        const u64 stub = PlaceStub(e);
        // A thread found at the second instruction (the game's multiply done): the add alone.
        Emitter add;
        add.SkipRedZone();
        add.Bytes({0x50, 0x48, 0xb8});
        add.U64(ImageBase + site.bias);
        add.Bytes({0xc5, 0xf8, 0x58, 0x00, 0x58});
        add.UnskipRedZone();
        add.JmpAbs(address + 16);
        const u64 add_stub = PlaceStub(add);
        installed += add_stub && DetourLive(address, stub, 16, address + 8, add_stub);
    }
    marker_sites.store(installed, std::memory_order_relaxed);
    return installed;
}

/// The HUD anchored to the output's edges. The game lays its HUD (menu/fe.gfx, Scaleform) out for
/// 1920x1080: on a wider output its screen is the middle of the movie's view, and its edges would
/// stay there. The HUD's groups are moved instead, as the game moves its own clips: through the
/// movie's members (GFx::Value, ObjectInterface::SetMember "x"/"y") from the thread the HUD lives
/// on. The bars, items and gauges (PlayerHUD) go to the left edge, blood echoes and insight
/// (PlayerHUD.Soul, .HeroPoint) and the autosave icon to the right; on a taller output the top
/// ones go up and the boss bar down. BB_HUD_ANCHOR=0 keeps them in the 16:9 middle.
/// GFx::Value as this build has it (the game's own copies: 0x1adb830, its path lookup 0x1f68aa5).
/// A managed value is linked into its ObjectInterface's list (AddRef 0x2f15b0, Release 0x2f1640).
struct GfxValue {
    u64 links[2] = {~u64(0), ~u64(0)};
    void* object_interface = nullptr;
    std::uint32_t type = 0, pad = 0;
    union {
        double number;
        void* data;
    } value{};
    u64 aux = 0;
};
static_assert(sizeof(GfxValue) == 0x30);
constexpr std::uint32_t GfxManaged = 0x40, GfxNumber = 5, GfxDisplayObject = 10;
/// ObjectInterface's methods in Scaleform 4's order: AddRef, Release, HasMember, GetMember
/// (the slot the game's path lookup calls), SetMember.
constexpr std::size_t SlotRelease = 0x18, SlotGetMember = 0x28, SlotSetMember = 0x30;
using GfxRefFn = GUEST_ABI void (*)(void* oi, GfxValue* value, void* data);
using GfxGetFn = GUEST_ABI bool (*)(void* oi, void* data, const char* name, GfxValue* out, bool isdobj);
using GfxSetFn = GUEST_ABI bool (*)(void* oi, void* data, const char* name, const GfxValue* value, bool isdobj);
template <typename F>
F GfxMethod(void* oi, std::size_t slot) {
    return *reinterpret_cast<F*>(*reinterpret_cast<u8**>(oi) + slot);
}
bool GfxIsObject(const GfxValue& v) {
    return v.object_interface && (v.type & 0x8c) == 8;
}
void GfxRelease(GfxValue& v) {
    if (v.object_interface && (v.type & GfxManaged)) {
        GfxMethod<GfxRefFn>(v.object_interface, SlotRelease)(v.object_interface, &v, v.value.data);
    }
    v = GfxValue{};
}
/// `object`'s member `name` into `out` (released by the caller), false if none.
bool GfxGet(const GfxValue& object, const char* name, GfxValue& out) {
    out = GfxValue{};
    if (!GfxIsObject(object)) {
        return false;
    }
    const bool isdobj = (object.type & 0x8f) == GfxDisplayObject;
    return GfxMethod<GfxGetFn>(object.object_interface, SlotGetMember)(object.object_interface,
                                                                       object.value.data, name, &out, isdobj);
}
bool GfxGetNumber(const GfxValue& object, const char* name, double& number) {
    GfxValue v;
    const bool ok = GfxGet(object, name, v) && (v.type & 0x8f) == GfxNumber;
    if (ok) {
        number = v.value.number;
    }
    GfxRelease(v);
    return ok;
}
bool GfxSetNumber(const GfxValue& object, const char* name, double number) {
    if (!GfxIsObject(object)) {
        return false;
    }
    GfxValue v;
    v.type = GfxNumber;
    v.value.number = number;
    const bool isdobj = (object.type & 0x8f) == GfxDisplayObject;
    return GfxMethod<GfxSetFn>(object.object_interface, SlotSetMember)(object.object_interface,
                                                                       object.value.data, name, &v, isdobj);
}

/// The game's in-game screen (vtable 0x539c988) updates once a frame (0x1bf9e10: rdi the screen,
/// xmm0 the frame time) and makes its HUD there on first use: the HUD (a value wrapper, its
/// movie's root GFx::Value at +0x30) at screen + 0xa80. The groups are moved from that update,
/// through values got and released within it: nothing of a movie is kept past its frame (the HUD
/// movie is unloaded with areas, and its ObjectInterface with it).
constexpr u64 HudFrameSite = 0x1bf9e10;
constexpr unsigned char FunctionEntryCode[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15
constexpr u64 ScreenHudOffset = 0xa80;
constexpr u64 HudWrapperVtable = 0x539a070; ///< the value wrapper's (0x539a060 + 0x10)
std::atomic<float> hud_dx{0.0f}, hud_dy{0.0f}; ///< the shift for an edge, in the HUD's pixels
std::atomic<bool> hud_anchor_on{true};
std::atomic<bool> hud_hooks{false};
/// A HUD group: its parent (0 root, 1 PlayerHUD), name and how it follows the edges (x by dx,
/// y by dy), and where the game put it.
struct HudItem {
    int parent;
    const char* name;
    float fx, fy;
    double base_x = 0, base_y = 0;
    double set_x = 0, set_y = 0; ///< what the port set last
    bool known = false;
    bool missing = false; ///< not in this HUD (a mod's, or one without the party list)
};
std::array<HudItem, 17> hud_items = {{
    {0, "PlayerHUD", -1.0f, -1.0f},
    {1, "Soul", 2.0f, 0.0f}, // in PlayerHUD: twice, for its move to the left
    {1, "HeroPoint", 2.0f, 0.0f},
    {0, "AutoSaveIcon", 1.0f, -1.0f},
    {0, "BossTop", 0.0f, 1.0f},
    // bbport co-op: the party list the HUD patch puts at the top level (scripts/hud_patch.py: the
    // members' icons, portraits, badges and supplies) stays beside the player's bars (PlayerHUD).
    // Its tags (CoopParty0-2) the co-op code places itself each frame (PartyListPosition).
    {0, "CoopSupply0", -1.0f, -1.0f},
    {0, "CoopSupply1", -1.0f, -1.0f},
    {0, "CoopSupply2", -1.0f, -1.0f},
    {0, "CoopPartyIcon0", -1.0f, -1.0f},
    {0, "CoopPartyIcon1", -1.0f, -1.0f},
    {0, "CoopPartyIcon2", -1.0f, -1.0f},
    {0, "CoopPartyFace0", -1.0f, -1.0f},
    {0, "CoopPartyFace1", -1.0f, -1.0f},
    {0, "CoopPartyFace2", -1.0f, -1.0f},
    {0, "CoopPartyBadge0", -1.0f, -1.0f},
    {0, "CoopPartyBadge1", -1.0f, -1.0f},
    {0, "CoopPartyBadge2", -1.0f, -1.0f},
}};

/// Each frame, from the game's screen update: the HUD groups where the output's edges are.
GUEST_ABI void HudFrame(u64 screen) {
    const u64 hud = screen ? *reinterpret_cast<const u64*>(screen + ScreenHudOffset) : 0;
    if (!hud) {
        return;
    }
    if (*reinterpret_cast<const u64*>(hud) != ImageBase + HudWrapperVtable) {
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("HUD: unexpected object at the screen's HUD: not anchored\n");
        }
        return;
    }
    const GfxValue& root = *reinterpret_cast<const GfxValue*>(hud + 0x30);
    static u64 last_hud = 0;
    static unsigned frames = 0;
    static float applied_dx = 0.0f, applied_dy = 0.0f;
    if (hud != last_hud) {
        last_hud = hud; // a new HUD (another area): where the game put its groups is read again
        for (HudItem& item : hud_items) {
            item.known = item.missing = false;
        }
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("HUD: anchored to the output's edges%s\n",
                        hud_anchor_on ? "" : " off (BB_HUD_ANCHOR=0)");
        }
    }
    const float dx = hud_anchor_on ? hud_dx.load(std::memory_order_relaxed) : 0.0f;
    const float dy = hud_anchor_on ? hud_dy.load(std::memory_order_relaxed) : 0.0f;
    bool all_known = true;
    for (const HudItem& item : hud_items) {
        all_known = all_known && (item.known || item.missing);
    }
    // Rechecked every half second: the game may put a group back itself.
    if (dx == applied_dx && dy == applied_dy && all_known && ++frames % 32 != 0) {
        return;
    }
    if (!GfxIsObject(root)) {
        return;
    }
    GfxValue player;
    GfxGet(root, "PlayerHUD", player);
    for (HudItem& item : hud_items) {
        GfxValue clip;
        if (!GfxGet(item.parent ? player : root, item.name, clip) || !GfxIsObject(clip)) {
            GfxRelease(clip);
            item.missing = true; // looked for again at the next recheck
            continue;
        }
        item.missing = false;
        double x = 0, y = 0;
        if (GfxGetNumber(clip, "x", x) && GfxGetNumber(clip, "y", y)) {
            // Positions are in twips (1/20 pixel): a read back is that close.
            const auto moved = [](double a, double b) { return std::abs(a - b) > 0.06; };
            if (!item.known || moved(x, item.set_x) || moved(y, item.set_y)) {
                item.base_x = x; // first seen, or moved by the game since
                item.base_y = y;
                item.known = true;
            }
            const double nx = item.base_x + item.fx * dx, ny = item.base_y + item.fy * dy;
            if (moved(nx, x)) {
                GfxSetNumber(clip, "x", nx);
            }
            if (moved(ny, y)) {
                GfxSetNumber(clip, "y", ny);
            }
            double check_x = nx, check_y = ny;
            GfxGetNumber(clip, "x", check_x);
            GfxGetNumber(clip, "y", check_y);
            item.set_x = check_x;
            item.set_y = check_y;
            if (moved(check_x, nx) || moved(check_y, ny)) {
                static bool told = false;
                if (!told) {
                    told = true;
                    std::printf("HUD: %s did not take its place (%.1f, %.1f instead of %.1f, %.1f)\n",
                                item.name, check_x, check_y, nx, ny);
                }
            }
        }
        GfxRelease(clip);
    }
    GfxRelease(player);
    applied_dx = dx;
    applied_dy = dy;
}

/// The game's UI (Scaleform movies) for the output's aspect ratio. The menu system sets each movie's
/// viewport every frame (0x1f59100 -> MovieImpl::SetViewport 0x1f510, a GFx::Viewport on its
/// stack: buffer 1920x1080, the movie's view mode ShowAll, centred). Given the output's pixel
/// aspect ratio (Viewport::AspectRatio: (output width / height) / 16:9), a movie shows its 16:9
/// screen in the middle of a wider (taller) view, squeezed into the 1920x1080 frame, which the
/// output stretches back (UiComposition::Scale): what lies past the game's screen (the HUD's
/// groups at the edges) is drawn, not culled. BB_RE_VIEWPORT=1 logs the calls.
constexpr u64 SetViewportSite = 0x1f510;
std::atomic<float> gfx_pixel_aspect{1.0f};
GUEST_ABI void ViewportAdjust(u64 movie, u64 viewport, u64 caller) {
    auto* v = reinterpret_cast<std::int32_t*>(viewport);
    static const bool trace = [] {
        const char* env = std::getenv("BB_RE_VIEWPORT");
        return env && env[0] == '1';
    }();
    if (trace) {
        static std::atomic<int> calls{0};
        float scale, aspect;
        std::memcpy(&scale, v + 11, 4);
        std::memcpy(&aspect, v + 12, 4);
        if (++calls <= 24) {
            std::printf("Viewport: movie %#llx from %#llx: buffer %dx%d at %d,%d %dx%d scissor %d,%d %dx%d "
                        "flags %#x scale %.3f aspect %.3f; mode %d align %d\n",
                        (unsigned long long)movie, (unsigned long long)(caller - ImageBase), v[0], v[1], v[2],
                        v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], double(scale), double(aspect),
                        *reinterpret_cast<const int*>(movie + 0xe8), *reinterpret_cast<const int*>(movie + 0xec));
        }
    }
    // The game's full-screen movies (1920x1080 in a 1920x1080 frame) only.
    if (v[0] == 1920 && v[1] == 1080 && v[2] == 0 && v[3] == 0 && v[4] == 1920 && v[5] == 1080) {
        const float aspect = gfx_pixel_aspect.load(std::memory_order_relaxed);
        std::memcpy(v + 12, &aspect, 4);
    }
}

/// A detour at a function's entry (`push rbp; mov rbp, rsp; push r15`): the handler with the
/// registers kept around it (rdi, rsi: the function's first arguments; rdx: its return address
/// when `pass_return`), then the entry's instructions and a jump back.
bool InstallEntryHook(u64 offset, const void* handler, bool pass_return) {
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), FunctionEntryCode, sizeof(FunctionEntryCode)) != 0) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.SaveXmm();
    if (pass_return) {
        e.Bytes({0x49, 0x8b, 0x54, 0x24, 0x50}); // mov rdx, [r12 + 80]: the return address
    }
    e.CallAbs(handler);
    e.RestoreXmm();
    e.RestoreVolatile();
    for (const unsigned char b : FunctionEntryCode) {
        e.Bytes({b});
    }
    e.JmpAbs(address + sizeof(FunctionEntryCode));
    return DetourLive(address, PlaceStub(e), sizeof(FunctionEntryCode));
}

} // namespace

void SetDisplayAspect(float aspect) {
    constexpr float game = 16.0f / 9.0f;
    const bool other = aspect > 0.5f && aspect < 8.0f && std::abs(aspect - game) > 0.001f;
    std::uint32_t bits = 0, fov = 0;
    float kx = 1.0f, ky = 1.0f; // the markers' scale: the UI is as high (wide) as the output
    if (other) {
        std::memcpy(&bits, &aspect, 4);
        if (aspect < game) {
            ky = game / aspect;
            std::memcpy(&fov, &ky, 4);
        } else {
            kx = aspect / game;
        }
    }
    camera_aspect_bits.store(bits, std::memory_order_relaxed);
    camera_fov_scale.bits.store(fov, std::memory_order_relaxed);
    // The HUD's groups: as far as the output's edge is from the 16:9 middle (the HUD's pixels).
    hud_dx.store(960.0f * (kx - 1.0f), std::memory_order_relaxed);
    hud_dy.store(540.0f * (ky - 1.0f), std::memory_order_relaxed);
    if (const char* env = std::getenv("BB_HUD_DX")) { // diagnostics: another shift
        hud_dx.store(std::strtof(env, nullptr), std::memory_order_relaxed);
    }
    gfx_pixel_aspect.store(other ? aspect / game : 1.0f, std::memory_order_relaxed);
    marker_kx.store(kx, std::memory_order_relaxed);
    marker_ky.store(ky, std::memory_order_relaxed);
    UpdateMarkerScales();
    std::printf("Display: aspect ratio %.3f (%s): camera %s, HUD markers at %d places\n", double(aspect),
                other ? (aspect > game ? "wider than 16:9" : "taller than 16:9") : "the game's",
                other ? (aspect > game ? "sees more to the sides" : "sees more above and below")
                      : "as the game has it",
                marker_sites.load(std::memory_order_relaxed));
}

bool AspectSupported() {
    return camera_sites.load(std::memory_order_acquire) > 0;
}

HudShift HudEdgeShift() {
    if (!hud_hooks.load(std::memory_order_acquire) || !hud_anchor_on.load(std::memory_order_relaxed)) {
        return {};
    }
    return {hud_dx.load(std::memory_order_relaxed), hud_dy.load(std::memory_order_relaxed)};
}

namespace {
void InstallAspectHooks() {
    const int aspect_sites = InstallCameraAspectHooks();
    const int marker_places = InstallMarkerHooks();
    if (const char* env = std::getenv("BB_HUD_ANCHOR"); env && env[0] == '0') {
        hud_anchor_on = false;
    }
    const bool viewport = InstallEntryHook(SetViewportSite, reinterpret_cast<const void*>(&ViewportAdjust), true);
    hud_hooks.store(viewport && InstallEntryHook(HudFrameSite, reinterpret_cast<const void*>(&HudFrame), false),
                    std::memory_order_release);
    std::printf("Guest hooks: the output's aspect ratio on the fly: the camera at %d of %zu places, "
                "the HUD's markers at %d of %zu, its groups %s\n",
                aspect_sites, CameraParamStores.size(), marker_places, MarkerSites.size(),
                hud_hooks.load() ? "at the edges" : "in the middle (no hook)");
}
} // namespace

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
#ifdef _WIN32
    AddVectoredExceptionHandler(1, OnTrapException);
#else
    struct sigaction action {};
    action.sa_sigaction = OnTrap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous_action);
#endif
    // Only the PC memory model (a range callback) needs to hear of the game's GPU allocations
    // and resource copies; the crash fixes below go in with either model (deadinside28 PR #214).
    const bool memory_hooks = on_gpu_range_allocated != nullptr;
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += memory_hooks && Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    on_range_allocated = on_gpu_range_allocated;
    const bool allocator =
        memory_hooks && Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // BB_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = memory_hooks && (!heap_env || heap_env[0] != '0');
    if (memory_hooks && BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    if (memory_hooks) {
        std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                    patched, MemcpySites.size());
        std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                    HeapAllocReturns.size());
        std::printf("Guest hooks: GPU memory allocator %s\n",
                    allocator ? "tells the GPU side of new ranges" : "not found");
    } else {
        std::printf("Guest hooks: none for the GPU memory (this memory model keeps copies)\n");
    }
    // Native detours over the traps just placed (BB_GUEST_HOOK_TRAPS=1 keeps the traps).
    const char* traps_env = std::getenv("BB_GUEST_HOOK_TRAPS");
    if (!(traps_env && traps_env[0] == '1') && (stub_page = MapStubPage()) != nullptr) {
        int detours = 0;
        for (const auto& site : MemcpySites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallCallDetour(site.offset);
            }
        }
        for (auto& site : native_sites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallDetour(site);
            }
        }
        const bool gx_guard = InstallGxNodeGuard();
        const bool job_slots = InstallJobSlotFix();
        const bool kernel_clamp = InstallKernelSampleClamp();
        // BB_ASPECT_HOOKS=0: the camera, HUD markers and UI keep 16:9 (the output too).
        if (const char* env = std::getenv("BB_ASPECT_HOOKS"); !(env && env[0] == '0')) {
            InstallAspectHooks();
        }
        SealStubPage();
        std::printf("Guest hooks: %d native detours (no traps; BB_GUEST_HOOK_TRAPS=1: int3)\n",
                    detours);
        std::printf("Guest hooks: GX command heap guard %s\n",
                    gx_guard ? "in" : "not installed (other code there)");
        std::printf("Guest hooks: render job slot fix %s\n", job_slots ? "on" : "not installed");
        std::printf("Guest hooks: blur-kernel sample count clamp %s\n",
                    kernel_clamp ? "on" : "not installed");
    } else if (!(traps_env && traps_env[0] == '1')) {
        std::printf("Guest hooks: no memory within reach of the game's code for detours: no crash "
                    "fixes, and the output keeps 16:9\n");
    }
    if (!memory_hooks) {
        return;
    }
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
} // namespace BbGuestHooks
