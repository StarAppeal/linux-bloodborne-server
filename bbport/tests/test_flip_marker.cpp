// The prepareFlip check before a flip submit (gpu/shim/bbport_flip_marker.h): a DCB that ends with
// the packet is patched as shadPS4 does, one without it (or shorter than the packet) is not, so
// the game is not stopped. No game, Vulkan device or window.
//   ninja -C out/gpu flip-marker-test && out/gpu/flip-marker-test.exe
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "bbport_flip_marker.h"

using BbFlipMarker::FindPrepareFlip;
using BbFlipMarker::HasPrepareFlip;
using BbFlipMarker::kPrepareFlipHeader;

int main() {
    // A normal flip DCB: draw work, then the 64-dword prepareFlip block at the end.
    std::vector<std::uint32_t> dcb(300, 0xc0001000u);
    dcb[300 - 64] = kPrepareFlipHeader;
    assert(HasPrepareFlip(dcb.data(), 300));
    assert(FindPrepareFlip(dcb.data(), 300, 256) == 236);

    // The crash: the packet is not 64 dwords before the end.
    dcb[300 - 64] = 0xc0001000u;
    assert(!HasPrepareFlip(dcb.data(), 300));
    assert(FindPrepareFlip(dcb.data(), 300, 256) == -1);

    // Elsewhere in the tail: still not where shadPS4 patches, but the log names where it is.
    dcb[290] = kPrepareFlipHeader;
    assert(!HasPrepareFlip(dcb.data(), 300));
    assert(FindPrepareFlip(dcb.data(), 300, 256) == 290);

    // Shorter than the packet: never read before the buffer.
    std::vector<std::uint32_t> tiny(10, kPrepareFlipHeader);
    assert(!HasPrepareFlip(tiny.data(), 10));
    assert(FindPrepareFlip(tiny.data(), 10, 256) == 9);
    assert(!HasPrepareFlip(nullptr, 300));
    assert(FindPrepareFlip(nullptr, 300, 256) == -1);
    assert(!HasPrepareFlip(dcb.data(), 0));

    std::puts("flip marker: ok");
    return 0;
}
