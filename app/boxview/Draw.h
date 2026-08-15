#pragma once

#include "boxview/Layout.h"
#include "syntax/Term.h"

#include <span>

struct ImDrawList;

namespace faustlens::boxview {

struct Palette {
    unsigned Stage = 0xFF3B3B3B;
    unsigned Outline = 0xFF6E6E6E;
    unsigned Wire = 0xFF9A9A9A;
    unsigned Text = 0xFFE8E8E8;
    unsigned Selected = 0xFF4A9EFF; // the occurrence the selection names
    // Other boxes drawing the same value.
    unsigned Occurrence = 0xFF2E617F;
    unsigned Enclosing = 0xFF2F5A80; // every node containing the selection
    unsigned Evaluated = 0xFF2C3A2C; // lifted, so read-only
    unsigned Port = 0xFFB9B9B9; // a `route`'s inside, unlike the stage `wire`s
    unsigned Link = 0xFFD8C070;
};

// How far from a port's centre a click still counts, in `Metrics`' unit.
inline constexpr float PortReach = 7.0f;

// `selected` is an occurrence in the drawn tree, not a value -- one value can be drawn
// in several boxes.
void Draw(ImDrawList *, const Node &, float ox, float oy, const Node *selected, std::span<const ValueId> path, const Palette & = {});

} // namespace faustlens::boxview
