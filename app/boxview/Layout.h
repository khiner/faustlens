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

// Geometry uses renderer-scaled units.
struct Metrics {
    float CharWidth = 7;
    float LineHeight = 16;
    float Pad = 4;
    float StageGap = 24;
    float LaneGap = 8;
    float Feedback = 14; // feedback connection height
};

// A route drag endpoint with a 1-based source channel.
struct Port {
    bool Input = false;
    uint32_t Channel = 1;
    float X = 0, Y = 0;
};

struct Node {
    ValueId Term = NoTerm;
    Kind Kind = Kind::Program;
    Rect Bounds; // absolute after Place
    std::string Label;
    bool Evaluated = false; // lifted term without a source ref
    std::vector<Node> Kids;
    std::vector<Port> Ports;
    std::vector<std::pair<uint32_t, uint32_t>> Wires;
};

struct Link {
    float X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
};

// Connect unequal lane counts through a shared edge point.
std::vector<Link> Wires(const Node &);

struct Layout {
    const Terms &Terms;
    Metrics Metrics;
    std::unordered_map<uint64_t, Node> Sized;
    const RefTree *Refs = nullptr;
    // Keep fixed for the layout's lifetime to preserve the size cache.
    std::unordered_map<RefId, ValueId> Expansions;

    Layout(const faustlens::Terms &t, boxview::Metrics mx) : Terms(t), Metrics(mx) {}

    Node Run(ValueId root);
    Node Run(const RefTree &, RefId root);

    // Return a child-index path identifying one drawn occurrence.
    static bool HitPath(const Node &, float x, float y, std::vector<uint32_t> &path);
    // Return ancestors outermost first, or empty if absent.
    static std::vector<ValueId> PathTo(const Node &, ValueId term);
    // Return the first node with this value, or null.
    static const Node *Find(const Node &, ValueId term);

    struct Endpoint {
        const Node *Node = nullptr;
        const Port *Port = nullptr;

        explicit operator bool() const { return Port != nullptr; }
    };
    // `reach` is the port hit radius in Metrics units.
    static Endpoint PortAt(const Node &, float x, float y, float reach);

    const Node &Measure(ValueId, RefId = NoRef);
    // Convert relative bounds to absolute positions.
    static void Place(Node &, float dx, float dy);
    Node Leaf(ValueId) const;
    Node Route(ValueId, const Wiring &) const;
};

} // namespace boxview
} // namespace faustlens
