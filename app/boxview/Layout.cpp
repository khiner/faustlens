#include "boxview/Layout.h"

#include "syntax/Edit.h"
#include "syntax/Printer.h"

#include <algorithm>
#include <format>
#include <span>
#include <string>

namespace faustlens::boxview {
namespace {

// A leaf can be a whole `with` block, so a stage elides past this.
constexpr size_t MaxLabel = 32;

// `<:` and `:>` lay out like `:`, differing only in the drawn fan.
bool IsHorizontal(Kind k) { return k != Kind::Par; }

std::string Label(const Terms &t, ValueId id) {
    const std::string term = PrintTerm(t, id);
    std::string out;
    out.reserve(std::min(term.size(), MaxLabel));
    bool pending = false;
    for (const char c : term) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pending = !out.empty();
            continue;
        }
        if (pending) out += ' ', pending = false;
        out += c;
        if (out.size() == MaxLabel) return out.replace(MaxLabel - 3, 3, "...");
    }
    return out;
}

void MarkEvaluated(Node &n) {
    n.Evaluated = true;
    for (Node &k : n.Kids) MarkEvaluated(k);
}

struct Point {
    float X = 0, Y = 0;
};

// Where wires meet one side of a node, one per lane reaching that edge.
void Edge(const Node &n, bool left, std::vector<Point> &out) {
    if (n.Kids.size() == 2) switch (n.Kind) {
            case Kind::Par:
                Edge(n.Kids[0], left, out);
                Edge(n.Kids[1], left, out);
                return;
            case Kind::Seq:
            case Kind::Split:
            case Kind::Merge:
                // The first stage owns the left edge and the last the right.
                Edge(n.Kids[left ? 0 : 1], left, out);
                return;
            case Kind::RecComp:
                // `a ~ b` has `a`'s inputs and outputs, and `b` reaches neither.
                Edge(n.Kids[0], left, out);
                return;
            default: break;
        }
    out.push_back({left ? n.Bounds.X : n.Bounds.Right(), n.Bounds.Y + n.Bounds.H / 2});
}

void Connect(std::span<const Point> from, std::span<const Point> to, float mid, std::vector<Link> &out) {
    if (from.empty() || to.empty()) return;
    if (from.size() == to.size()) {
        for (size_t i = 0; i < from.size(); ++i) out.push_back({from[i].X, from[i].Y, to[i].X, to[i].Y});
        return;
    }
    Point hub{mid, 0};
    for (const Point &p : from) hub.Y += p.Y;
    for (const Point &p : to) hub.Y += p.Y;
    hub.Y /= float(from.size() + to.size());
    for (const Point &p : from) out.push_back({p.X, p.Y, hub.X, hub.Y});
    for (const Point &p : to) out.push_back({hub.X, hub.Y, p.X, p.Y});
}

} // namespace

std::vector<Link> Wires(const Node &n) {
    std::vector<Link> out;
    if (n.Kids.size() != 2 || n.Kind == Kind::Par) return out;

    const Node &a = n.Kids[0], &c = n.Kids[1];
    std::vector<Point> from, to;
    Edge(a, false, from);
    Edge(c, true, to);
    Connect(from, to, (a.Bounds.Right() + c.Bounds.X) / 2, out);
    if (n.Kind != Kind::RecComp) return out;

    // The return path, in the reserved height below: one trunk whatever the
    // lane counts.
    std::vector<Point> back_from, back_to;
    Edge(c, false, back_from);
    Edge(a, true, back_to);
    if (back_from.empty() || back_to.empty()) return out;
    const float y = n.Bounds.Bottom() - 4;
    float right = back_from.front().X, left = back_to.front().X;
    for (const Point &p : back_from) {
        right = std::max(right, p.X);
        out.push_back({p.X, p.Y, p.X, y});
    }
    for (const Point &p : back_to) {
        left = std::min(left, p.X);
        out.push_back({p.X, y, p.X, p.Y});
    }
    out.push_back({right, y, left, y});
    return out;
}

Node Layout::Leaf(ValueId id) const {
    Node n;
    n.Term = id;
    n.Kind = Terms.KindOf(id);
    if (n.Kind == Kind::Route) {
        const Wiring w = RouteWiring(Terms, id);
        if (w.Drawable()) return Route(id, w);
    }
    n.Label = Label(Terms, id);
    n.Bounds = {0, 0, float(n.Label.size()) * Metrics.CharWidth + 2 * Metrics.Pad, Metrics.LineHeight + 2 * Metrics.Pad};
    return n;
}

Node Layout::Route(ValueId id, const Wiring &w) const {
    Node n;
    n.Term = id;
    n.Kind = Kind::Route;
    n.Wires = w.Pairs;
    n.Label = std::format("route({}, {})", w.Ins, w.Outs);

    const uint32_t rows = std::max(w.Ins, w.Outs);
    // The label gets a row of its own, or it strikes through the first wire.
    const float text = float(n.Label.size()) * Metrics.CharWidth + 2 * Metrics.Pad;
    n.Bounds = {0, 0, std::max(text, 2 * Metrics.StageGap), float(rows + 1) * Metrics.LineHeight + 2 * Metrics.Pad};

    // Each port centred in its own share of the edge, so 2 against 8 fan out.
    const auto side = [&](bool input, uint32_t count) {
        const float top = Metrics.Pad + Metrics.LineHeight;
        const float span = n.Bounds.H - top - Metrics.Pad;
        for (uint32_t c = 1; c <= count; ++c) n.Ports.push_back({input, c, input ? 0.0f : n.Bounds.W, top + span * (float(c) - 0.5f) / float(count)});
    };
    side(true, w.Ins);
    side(false, w.Outs);
    return n;
}

const Node &Layout::Measure(ValueId id) {
    if (const auto it = Sized.find(id); it != Sized.end()) return it->second;
    // Only a stop against a cyclic `Expansions` map: a cycle draws an empty node.
    Sized.emplace(id, Node{});

    Node out;
    const auto ex = Expansions.find(id);
    if (ex != Expansions.end() && ex->second != NoTerm && ex->second != id) {
        // The evaluated form's layout under the source's term, so it collapses back.
        out = Measure(ex->second);
        out.Term = id;
        out.Kind = Terms.KindOf(id);
        for (Node &k : out.Kids) MarkEvaluated(k);
    } else if (const Kind kind = Terms.KindOf(id); !IsComposition(kind) || Terms.Children(id).size() != 2) {
        out = Leaf(id);
    } else {
        const std::span<const ValueId> kids = Terms.Children(id);
        const Node a = Measure(kids[0]);
        const Node b = Measure(kids[1]);
        out.Term = id;
        out.Kind = kind;
        out.Kids = {a, b};

        if (IsHorizontal(kind)) {
            const float h = std::max(a.Bounds.H, b.Bounds.H);
            out.Bounds = {0, 0, a.Bounds.W + Metrics.StageGap + b.Bounds.W, h};
            Place(out.Kids[0], 0, (h - a.Bounds.H) / 2);
            Place(out.Kids[1], a.Bounds.W + Metrics.StageGap, (h - b.Bounds.H) / 2);
        } else {
            // No wires run between lanes, so no gap.
            const float w = std::max(a.Bounds.W, b.Bounds.W);
            out.Bounds = {0, 0, w, a.Bounds.H + Metrics.LaneGap + b.Bounds.H};
            Place(out.Kids[1], 0, a.Bounds.H + Metrics.LaneGap);
        }
        // `~`'s return path would otherwise escape the node's bounds.
        if (kind == Kind::RecComp) out.Bounds.H += Metrics.Feedback;
    }

    Sized[id] = std::move(out);
    return Sized[id];
}

void Layout::Place(Node &n, float dx, float dy) {
    n.Bounds.X += dx;
    n.Bounds.Y += dy;
    for (Port &p : n.Ports) {
        p.X += dx;
        p.Y += dy;
    }
    for (Node &k : n.Kids) Place(k, dx, dy);
}

Node Layout::Run(ValueId root) {
    if (root == NoTerm) return {};
    return Measure(root);
}

bool Layout::HitPath(const Node &n, float x, float y, std::vector<uint32_t> &path) {
    if (!n.Bounds.Contains(x, y)) return false;
    // A composition's children never overlap, so the first hit is the only one.
    for (uint32_t i = 0; i < n.Kids.size(); ++i)
        if (HitPath(n.Kids[i], x, y, path)) {
            path.insert(path.begin(), i);
            return true;
        }
    return true;
}

std::vector<ValueId> Layout::PathTo(const Node &n, ValueId term) {
    if (n.Term == term) return {n.Term};
    for (const Node &k : n.Kids) {
        std::vector<ValueId> below = PathTo(k, term);
        if (below.empty()) continue;
        below.insert(below.begin(), n.Term);
        return below;
    }
    return {};
}

const Node *Layout::Find(const Node &n, ValueId term) {
    if (n.Term == term) return &n;
    for (const Node &k : n.Kids)
        if (const Node *found = Find(k, term)) return found;
    return nullptr;
}

Layout::Endpoint Layout::PortAt(const Node &n, float x, float y, float reach) {
    // Nearest, not first: adjacent `route`s put an output and an input close
    // enough that first-hit would depend on traversal order.
    Endpoint best;
    float nearest = reach * reach;
    const auto visit = [&](const Node &m, const auto &self) -> void {
        for (const Port &p : m.Ports) {
            const float dx = p.X - x, dy = p.Y - y;
            const float d = dx * dx + dy * dy;
            if (d > nearest) continue;
            nearest = d;
            best = {&m, &p};
        }
        for (const Node &k : m.Kids) self(k, self);
    };
    visit(n, visit);
    return best;
}

} // namespace faustlens::boxview
