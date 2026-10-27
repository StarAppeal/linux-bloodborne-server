// SPDX-License-Identifier: GPL-2.0-or-later
// Buffer formats the renderer must take without stopping the game (video_core/amdgpu/pixel_format.h).
// Bloodborne (a co-op player's run on 2026-10-09) bound a buffer as SnormNz with 32-bit channels,
// and MapNumberConversion stopped the game there ("Unreachable code! data_fmt = 14").
#include <cstdio>
#include "video_core/amdgpu/pixel_format.h"

static int failures;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

int main() {
    using namespace AmdGpu;
    // 32-bit SnormNz has no conversion: the data is used as it is.
    for (const auto data_fmt : {DataFormat::Format32, DataFormat::Format32_32,
                                DataFormat::Format32_32_32, DataFormat::Format32_32_32_32}) {
        CHECK(MapNumberConversion(NumberFormat::SnormNz, data_fmt) == NumberConversion::None);
    }
    CHECK(MapNumberConversion(NumberFormat::SnormNz, DataFormat::Format8_8_8_8) ==
          NumberConversion::Sint8ToSnormNz);
    CHECK(MapNumberConversion(NumberFormat::SnormNz, DataFormat::Format16_16) ==
          NumberConversion::Sint16ToSnormNz);
    // shadPS4 #5509: logs and shader dumps name Ubscaled by its own name.
    CHECK(NameOf(NumberFormat::Ubscaled) == "Ubscaled");
    CHECK(NameOf(NumberFormat::Ubint) == "Ubint");
    if (failures != 0) {
        std::fprintf(stderr, "pixel format: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("pixel format: ok");
    return 0;
}
