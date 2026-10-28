// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the `prepareFlip` NOP that ends the last DCB of sceGnmSubmitAndFlipCommandBuffers.
// shadPS4 asserts on it and the game stopped (exit 23) when a frame came without one: Lisara's
// guest, the frame after the Old Hunters grab cutscene, 2026-10-28. Without the marker the DCB is
// submitted as it is and the flip goes from the CPU instead (at worst one frame is not shown).
#pragma once

#include <cstdint>

namespace BbFlipMarker {

/// The PM4 type-3 NOP header (0x3e payload dwords) libGnm writes 64 dwords before a flip DCB's end.
inline constexpr std::uint32_t kPrepareFlipHeader = 0xc03e1000u;
inline constexpr std::uint32_t kPrepareFlipDwords = 64;

/// True when the DCB of `size_dw` dwords ends with the prepareFlip packet shadPS4 patches.
inline bool HasPrepareFlip(const std::uint32_t* dcb, std::uint32_t size_dw) {
    return dcb && size_dw >= kPrepareFlipDwords &&
           dcb[size_dw - kPrepareFlipDwords] == kPrepareFlipHeader;
}

/// Dword offset of the last prepareFlip header within the DCB's final `window` dwords, -1 when
/// there is none. For the log only.
inline std::int64_t FindPrepareFlip(const std::uint32_t* dcb, std::uint32_t size_dw,
                                    std::uint32_t window) {
    if (!dcb) {
        return -1;
    }
    const std::uint32_t first = size_dw > window ? size_dw - window : 0;
    for (std::uint32_t i = size_dw; i > first; --i) {
        if (dcb[i - 1] == kPrepareFlipHeader) {
            return static_cast<std::int64_t>(i - 1);
        }
    }
    return -1;
}

} // namespace BbFlipMarker
