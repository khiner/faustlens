#pragma once

#include "boxview/Layout.h"
#include "syntax/Term.h"

struct ImDrawList;

namespace faustlens::boxview {

struct Palette {
    unsigned Stage = 0xFF3B3B3B;
    unsigned Outline = 0xFF6E6E6E;
    unsigned Wire = 0xFF9A9A9A;
    unsigned Text = 0xFFE8E8E8;
    unsigned Selected = 0xFF4A9EFF;
    unsigned Occurrence = 0xFF2E617F;
    unsigned Enclosing = 0xFF2F5A80;
    unsigned Evaluated = 0xFF2C3A2C; // read-only lifted term
    unsigned Port = 0xFFB9B9B9; // connections inside a route
    unsigned Link = 0xFFD8C070;
};

// Port hit radius in Metrics units.
inline constexpr float PortReach = 7.0f;

// `selected` identifies one drawn occurrence of a potentially shared value.
void Draw(ImDrawList *, const Node &, float ox, float oy, const Node *selected, const Palette & = {});

} // namespace faustlens::boxview
