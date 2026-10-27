// SPDX-License-Identifier: GPL-2.0-or-later
// dlss_modes.h: the DLSS quality mode per render scale and the preset each mode runs.
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/dlss_modes.h"
#include "gpu/shadps4/video_core/renderer_vulkan/scene_resolution.h"

using namespace Vulkan::DlssModes;
using Vulkan::SceneResolution::ForPreset;

int main() {
    // The menu's presets (native, quality, balanced, performance, ultra performance) at 1080p,
    // 1440p and 4K land on DLAA and NVIDIA's four upscaling modes.
    for (const Vulkan::SceneResolution::Size output : {Vulkan::SceneResolution::Size{1920, 1080},
                                                       {2560, 1440}, {3840, 2160}}) {
        for (int preset = 0; preset < 5; ++preset) {
            const auto render = ForPreset(preset, output);
            assert(QualityForScale(float(output.width) / float(render.width)) == preset);
        }
    }

    // Player report: Quality blurrier than Ultra Performance with "Default". NVIDIA's own default
    // ran K on Quality and Balanced; every upscaling mode now runs a second-generation model.
    assert(Preset(PresetDefault, 1) == PresetM);
    assert(Preset(PresetDefault, 2) == PresetM);
    assert(Preset(PresetDefault, 3) == PresetM);
    assert(Preset(PresetDefault, 4) == PresetL);
    assert(Preset(PresetDefault, 0) == PresetDefault); // DLAA: NVIDIA's choice
    // A model picked in the menu wins in every mode.
    for (int quality = 0; quality < 5; ++quality) {
        assert(Preset(PresetK, quality) == PresetK);
        assert(Preset(10, quality) == 10); // J
    }
    std::puts("dlss modes: ok");
}
