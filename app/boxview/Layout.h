// Geometry is a pure function of Term, never persisted: saved coordinates would make
// text <-> diagram a symmetric lens.
#pragma once

#include "syntax/Term.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace faustlens {

struct Wiring;

namespace boxview {

struct Rect {
    float X = 0, Y = 0, W = 0, H = 0;

    float Right() const { return X + W; }
    float Bottom() const { return Y + H; }
    bool Contains(float px, float py) const { return px >= X && px < Right() && py >= Y && py < Bottom(); }
};

// In one arbitrary unit the renderer scales.
struct Metrics {
    float CharWidth = 7;
    float LineHeight = 16;
    float Pad = 4;
    float StageGap = 24; // between horizontal stages, where wires are drawn
    float LaneGap = 8;
    float Feedback = 14; // height reserved below a `~` for its return path
};

// A `route` drag endpoint. `channel` is 1-based, as in source.
struct Port {
    bool Input = false; // on the left edge
    uint32_t Channel = 1;
    float X = 0, Y = 0;
};

struct Node {
    ValueId Term = NoTerm;
    Kind Kind = Kind::Program;
    Rect Bounds; // absolute, once `Place` has run
    std::string Label;
    bool Evaluated = false; // lifted, so no byte range and no ref
    std::vector<Node> Kids;
    // A `route`'s endpoints and pairs, empty on every other kind.
    std::vector<Port> Ports;
    std::vector<std::pair<uint32_t, uint32_t>> Wires;
};

struct Link {
    float X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
};

// A wire meets a node where its lanes reach the edge. Unequal lane counts bundle through
// one point.
std::vector<Link> Wires(const Node &);

// Sizes memoize per value id, which hash-consing makes sound.
struct Layout {
    const Terms &Terms;
    Metrics Metrics;
    std::unordered_map<ValueId, Node> Sized;
    // Must stay fixed for the layout's lifetime or the size memo goes stale.
    std::unordered_map<ValueId, ValueId> Expansions;

    Layout(const faustlens::Terms &t, boxview::Metrics mx) : Terms(t), Metrics(mx) {}

    Node Run(ValueId root);

    // A path, not a value, since one value can be drawn in several boxes.
    static bool HitPath(const Node &, float x, float y, std::vector<uint32_t> &path);
    // Outermost first, empty where the term is not in the tree.
    static std::vector<ValueId> PathTo(const Node &, ValueId term);
    // The *first* node drawn for `term`, or null. Not enough to place an edit.
    static const Node *Find(const Node &, ValueId term);

    struct Endpoint {
        const Node *Node = nullptr;
        const Port *Port = nullptr;

        explicit operator bool() const { return Port != nullptr; }
    };
    // `reach` is how far from a port's centre still counts, in `Metrics`' unit.
    static Endpoint PortAt(const Node &, float x, float y, float reach);

    const Node &Measure(ValueId);
    // Bounds are relative until this runs, so one measure serves every occurrence.
    static void Place(Node &, float dx, float dy);
    Node Leaf(ValueId) const;
    Node Route(ValueId, const Wiring &) const;
};

} // namespace boxview
} // namespace faustlens
