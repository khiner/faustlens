#include "boxview/Draw.h"

#include "imgui.h"

#include <algorithm>
#include <span>
#include <string>

namespace faustlens::boxview {
namespace {

bool Contains(std::span<const ValueId> path, ValueId v) { return std::ranges::contains(path, v); }

void Inside(ImDrawList *dl, const Node &n, float ox, float oy, const Palette &pal) {
    if (n.Ports.empty()) return;
    const auto at = [&](bool input, uint32_t channel) {
        for (const Port &p : n.Ports)
            if (p.Input == input && p.Channel == channel) return ImVec2{ox + p.X, oy + p.Y};
        return ImVec2{ox + n.Bounds.X, oy + n.Bounds.Y};
    };
    for (const auto &[in, out] : n.Wires) dl->AddLine(at(true, in), at(false, out), pal.Link, 1.5f);
    for (const Port &p : n.Ports) dl->AddCircleFilled({ox + p.X, oy + p.Y}, 3.0f, pal.Port);
}

} // namespace

void Draw(ImDrawList *dl, const Node &n, float ox, float oy, const Node *selected, std::span<const ValueId> path, const Palette &pal) {
    const Rect &b = n.Bounds;
    // Node identity for the box an edit lands on, value equality for the rest.
    const bool is_selected = &n == selected;
    const bool is_occurrence = selected != nullptr && !is_selected && n.Term == selected->Term;

    if (n.Kids.empty()) {
        const ImVec2 tl{ox + b.X, oy + b.Y}, br{ox + b.Right(), oy + b.Bottom()};
        dl->AddRectFilled(tl, br, n.Evaluated ? pal.Evaluated : pal.Stage, 3);
        const unsigned edge = is_selected ? pal.Selected : is_occurrence ? pal.Occurrence : Contains(path, n.Term) ? pal.Enclosing : pal.Outline;
        dl->AddRect(tl, br, edge, 3, 0, is_selected ? 2.0f : 1.0f);
        const ImVec2 size = ImGui::CalcTextSize(n.Label.c_str());
        // A `route`'s label sits at the top, since the middle holds its wires.
        const float ty = n.Ports.empty() ? tl.y + (b.H - size.y) / 2 : tl.y + 2;
        dl->AddText({tl.x + (b.W - size.x) / 2, ty}, pal.Text, n.Label.c_str());
        Inside(dl, n, ox, oy, pal);
        return;
    }

    for (const Link &l : Wires(n)) dl->AddLine({ox + l.X0, oy + l.Y0}, {ox + l.X1, oy + l.Y1}, pal.Wire);

    for (const Node &k : n.Kids) Draw(dl, k, ox, oy, selected, path, pal);

    // Outlined only on the selection path, so no nested frames otherwise.
    if (is_selected || is_occurrence || Contains(path, n.Term)) {
        dl->AddRect(
            {ox + b.X - 2, oy + b.Y - 2}, {ox + b.Right() + 2, oy + b.Bottom() + 2},
            is_selected       ? pal.Selected :
                is_occurrence ? pal.Occurrence :
                                pal.Enclosing,
            4, 0, is_selected ? 2.0f : 1.0f
        );
    }
}

} // namespace faustlens::boxview
