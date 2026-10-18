// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the Controls tab's gamepad bindings swap (bbport_pad_rebind.h).
#include "bbport_pad_rebind.h"

#include <algorithm>

namespace BbPadRebind {

namespace {

std::vector<std::string> Split(const std::string& list) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < list.size()) {
        std::size_t end = list.find(',', at);
        if (end == std::string::npos) {
            end = list.size();
        }
        std::string name = list.substr(at, end - at);
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        if (!name.empty()) {
            out.push_back(std::move(name));
        }
        at = end + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

bool Has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

std::vector<std::pair<std::string, std::string>> Assign(const std::vector<Input>& inputs,
                                                        std::size_t target, const std::string& button) {
    if (target >= inputs.size() || button.empty()) {
        return {};
    }
    const std::vector<std::string> given = Split(inputs[target].buttons);
    if (Has(given, button)) {
        return {};
    }
    std::vector<std::pair<std::string, std::string>> lines{{inputs[target].name, button}};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const std::vector<std::string> old = Split(inputs[i].buttons);
        if (i == target || !Has(old, button)) {
            continue;
        }
        // The pressed button's place takes the target's old buttons this input does not have yet.
        std::vector<std::string> next;
        for (const std::string& name : old) {
            if (name != button) {
                next.push_back(name);
                continue;
            }
            for (const std::string& swapped : given) {
                if (!Has(old, swapped) && !Has(next, swapped)) {
                    next.push_back(swapped);
                }
            }
        }
        lines.emplace_back(inputs[i].name, Join(next));
    }
    return lines;
}

} // namespace BbPadRebind
