// SPDX-License-Identifier: GPL-2.0-or-later
// bbport_pad_rebind.cpp: a gamepad button pressed for an input in the Controls tab swaps with the
// input that had it, so no button is left doing nothing (Kaniel 2026-10-18: Cross bound to Circle,
// Circle to Square and Triangle to R2 left Cross dead and made R2 drink a vial).
#include <cassert>
#include <string>
#include <utility>
#include <vector>
#include "gpu/shim/bbport_pad_rebind.h"

using namespace BbPadRebind;
using Lines = std::vector<std::pair<std::string, std::string>>;

// The default gamepad buttons (runtime_pad.c bind_defaults), the inputs that take one.
static std::vector<Input> Defaults() {
    return {{"cross", "a"},        {"circle", "b"},       {"square", "x"},           {"triangle", "y"},
            {"l1", "leftshoulder"}, {"r1", "rightshoulder"}, {"l2", "lefttrigger"},  {"r2", "righttrigger"},
            {"l3", "leftstick"},   {"r3", "rightstick"},  {"options", "start"},      {"touchpad", "back, touchpad"},
            {"touchpad_right", ""}, {"up", "dpup"},        {"down", "dpdown"},       {"left", "dpleft"},
            {"right", "dpright"}};
}

static std::vector<Input> Apply(std::vector<Input> inputs, const Lines& lines) {
    for (const auto& [name, buttons] : lines) {
        for (auto& input : inputs) {
            if (input.name == name) {
                input.buttons = buttons;
            }
        }
    }
    return inputs;
}

int main() {
    // Circle pressed for Cross: the two swap.
    assert((Assign(Defaults(), 0, "b") == Lines{{"cross", "b"}, {"circle", "a"}}));
    // Kaniel's three presses leave every button on some input.
    auto inputs = Apply(Defaults(), Assign(Defaults(), 0, "b"));
    inputs = Apply(inputs, Assign(inputs, 1, "x"));
    inputs = Apply(inputs, Assign(inputs, 3, "righttrigger"));
    for (const char* button : {"a", "b", "x", "y", "righttrigger"}) {
        int holders = 0;
        for (const auto& input : inputs) {
            holders += input.buttons == button;
        }
        assert(holders == 1);
    }
    assert(inputs[7].buttons == "y"); // R2 took Triangle's old button: R2 no longer drinks a vial
    // A button of a two-button input: the target's old button goes in its place, the other stays.
    assert((Assign(Defaults(), 0, "back") == Lines{{"cross", "back"}, {"touchpad", "a, touchpad"}}));
    // The target's old buttons: both go to the input that had the pressed one.
    assert((Assign(Defaults(), 11, "x") == Lines{{"touchpad", "x"}, {"square", "back, touchpad"}}));
    // A button nobody had: the target takes it, its old one is free.
    assert((Assign(Defaults(), 12, "misc1") == Lines{{"touchpad_right", "misc1"}}));
    // An unbound target giving its (no) buttons away: the other input loses the button.
    assert((Assign(Defaults(), 12, "a") == Lines{{"touchpad_right", "a"}, {"cross", ""}}));
    // A button the target already has: nothing changes (Touchpad keeps Back too).
    assert(Assign(Defaults(), 11, "touchpad").empty());
    assert(Assign(Defaults(), 0, "a").empty());
    // No duplicate when the other input already has one of the target's buttons.
    std::vector<Input> shared = {{"cross", "a, b"}, {"circle", "b, x"}};
    assert((Assign(shared, 1, "a") == Lines{{"circle", "a"}, {"cross", "x, b"}}));
    // Out of range: nothing.
    assert(Assign(Defaults(), 99, "a").empty());
    return 0;
}
