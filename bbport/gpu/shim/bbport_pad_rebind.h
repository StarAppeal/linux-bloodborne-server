// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the Controls tab's gamepad bindings (bbport_overlay.cpp). A gamepad button pressed for an
// input swaps with the input that had it: replacing only the one input left the button on two inputs
// and the input's old button on none (Kaniel 2026-10-18: Cross bound to Circle, Circle to Square and
// Triangle to R2 left Cross doing nothing and made R2 drink a vial).
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace BbPadRebind {

/// An input that takes gamepad buttons and its buttons now: a pad.<input> list (runtime_pad.c's SDL
/// names, ", " between them; empty: none).
struct Input {
    std::string name;
    std::string buttons;
};

/// The pad.<input> lines (input name, buttons) to write when `button` is pressed for
/// `inputs[target]`: the target gets `button` alone, and every other input that had `button` gets
/// the target's old buttons in its place. Nothing when the target already has `button`.
std::vector<std::pair<std::string, std::string>> Assign(const std::vector<Input>& inputs,
                                                        std::size_t target, const std::string& button);

} // namespace BbPadRebind
