#pragma once

#include "signal/Ui.h"

#include <string_view>

namespace faustlens::test {

// The first widget whose cleaned leaf label is `label`, or null.
inline const UiNode *Widget(const UiNode &tree, std::string_view label) {
    const UiNode *found = nullptr;
    ForEachWidget(tree, [&](const UiNode &n) {
        if (n.Label != label) return true;
        found = &n;
        return false;
    });
    return found;
}

} // namespace faustlens::test
