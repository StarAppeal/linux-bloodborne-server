// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: DLSS quality modes and the render preset each one runs (tests/test_dlss_modes.cpp).
#pragma once
#include <cstdint>

namespace Vulkan::DlssModes {

// NVSDK_NGX_DLSS_Hint_Render_Preset values (BbSettings::DlssModels).
inline constexpr uint32_t PresetDefault = 0, PresetK = 11, PresetL = 12, PresetM = 13;

/// DLSS quality mode for an output/render size ratio (0 DLAA .. 4 Ultra Performance).
constexpr int QualityForScale(float scale) {
    // DLAA only for equal sizes: any real upscale needs at least the Quality mode.
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

/// The render preset a mode runs. A model chosen in the menu applies to every mode. "Default"
/// left it to NVIDIA, whose DLSS 4.5 library runs the older K on Quality and Balanced but the
/// sharper M and L on Performance and Ultra Performance: Quality looked blurrier than Ultra
/// Performance. Every upscaling mode now gets the second-generation models; DLAA keeps NVIDIA's.
constexpr uint32_t Preset(uint32_t model, int quality) {
    if (model != PresetDefault) return model;
    return quality <= 0 ? PresetDefault : quality >= 4 ? PresetL : PresetM;
}

} // namespace Vulkan::DlssModes
