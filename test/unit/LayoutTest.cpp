#include "boxview/Layout.h"
#include "property/Corpus.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "unit/Diagram.h"

#include "doctest.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

using namespace faustlens;
using namespace faustlens::boxview;
using namespace faustlens::test;

namespace {

struct Diagram {
    Session Session;
    Node Root;
    const Terms &Terms() const { return Session.Terms; }
};

std::shared_ptr<Diagram> Lay(const std::string &src, Metrics m = {}) {
    auto d = std::make_shared<Diagram>();
    d->Session.SetBuffer("/p.dsp", src);
    const TermsResult &tr = d->Session.TermsOf("/p.dsp");
    const ValueId body = ProcessBody(d->Session.Terms, tr.Root);
    REQUIRE(body != NoTerm);
    Layout layout(d->Session.Terms, m);
    d->Root = layout.Run(body);
    return d;
}

void CheckContained(const Node &n) {
    for (const Node &k : n.Kids) {
        CHECK(k.Bounds.X >= n.Bounds.X);
        CHECK(k.Bounds.Y >= n.Bounds.Y);
        CHECK(k.Bounds.Right() <= n.Bounds.Right() + 1e-3f);
        CHECK(k.Bounds.Bottom() <= n.Bounds.Bottom() + 1e-3f);
        CheckContained(k);
    }
}

void CheckDisjoint(const Node &n) {
    if (n.Kids.size() == 2) {
        const Rect &a = n.Kids[0].Bounds, &b = n.Kids[1].Bounds;
        const bool apart_x = a.Right() <= b.X + 1e-3f || b.Right() <= a.X + 1e-3f;
        const bool apart_y = a.Bottom() <= b.Y + 1e-3f || b.Bottom() <= a.Y + 1e-3f;
        const bool apart = apart_x || apart_y;
        CHECK(apart);
    }
    for (const Node &k : n.Kids) CheckDisjoint(k);
}

size_t Count(const Node &n) {
    size_t c = 1;
    for (const Node &k : n.Kids) c += Count(k);
    return c;
}

std::vector<Link> AllWires(const Node &n) {
    std::vector<Link> out = Wires(n);
    for (const Node &k : n.Kids) {
        const std::vector<Link> below = AllWires(k);
        out.insert(out.end(), below.begin(), below.end());
    }
    return out;
}

bool Near(float a, float b) { return std::abs(a - b) < 1e-3f; }

bool Touches(std::span<const Link> w, float x, float y) {
    for (const Link &l : w)
        if ((Near(l.X0, x) && Near(l.Y0, y)) || (Near(l.X1, x) && Near(l.Y1, y))) return true;
    return false;
}

float MidY(const Node &n) { return n.Bounds.Y + n.Bounds.H / 2; }

} // namespace

TEST_CASE("a composition places its children by kind") {
    const auto seq = Lay("process = _ : _ : _;\n");
    CHECK(seq->Root.Kind == Kind::Seq);
    CHECK(seq->Root.Kids.size() == 2);
    CHECK(seq->Root.Kids[0].Bounds.Right() < seq->Root.Kids[1].Bounds.X);

    // A `,` has no wires between its sides to need a gap, so lanes are left-aligned.
    const auto par = Lay("process = _ , _;\n");
    CHECK(par->Root.Kind == Kind::Par);
    CHECK(par->Root.Kids[0].Bounds.Bottom() < par->Root.Kids[1].Bounds.Y);
    CHECK(par->Root.Kids[0].Bounds.X == par->Root.Kids[1].Bounds.X);

    for (const char *src : {"process = _ <: _ , _;\n", "process = _ , _ :> _;\n"}) {
        const auto d = Lay(src);
        CHECK(d->Root.Kids[0].Bounds.Right() < d->Root.Kids[1].Bounds.X);
    }
}

TEST_CASE("`~` reserves the height its return path needs") {
    Metrics const m;
    const auto rec = Lay("process = _ ~ _;\n", m);
    const auto seq = Lay("process = _ : _;\n", m);
    CHECK(rec->Root.Kind == Kind::RecComp);
    CHECK(rec->Root.Bounds.H == doctest::Approx(seq->Root.Bounds.H + m.Feedback));
}

TEST_CASE("layout invariants hold over the reference corpus") {
    int laid = 0;
    size_t nodes = 0;
    ForEachDiagram([&](Session &, const FileView &, ValueId, const Node &root) {
        CheckContained(root);
        CheckDisjoint(root);
        nodes += Count(root);
        ++laid;
    });
    MESSAGE("laid out ", laid, " diagrams, ", nodes, " nodes");
    CHECK(laid > 80);
}

TEST_CASE("geometry is a function of the value id") {
    const auto d = Lay("process = (_ : _) , (_ : _);\n");
    REQUIRE(d->Root.Kind == Kind::Par);
    const Node &a = d->Root.Kids[0], &b = d->Root.Kids[1];
    CHECK(a.Term == b.Term);
    CHECK(a.Bounds.W == b.Bounds.W);
    CHECK(a.Bounds.H == b.Bounds.H);
    CHECK(a.Bounds.Y != b.Bounds.Y);
}

TEST_CASE("a term resolves to its enclosing chain") {
    const auto d = Lay("process = _ : _ , _;\n");
    const Node &root = d->Root;
    const Node &inner = root.Kids[1].Kids[0];

    const std::vector<ValueId> path = Layout::PathTo(root, inner.Term);
    REQUIRE(path.size() >= 2);
    CHECK(path.front() == root.Term);
    CHECK(path.back() == inner.Term);
    CHECK(Layout::PathTo(root, NoTerm).empty());
}

TEST_CASE("a wire meets a lane, never the gap between two of them") {
    // The middle of a `,`'s bounds is the gap between its lanes.
    const auto d = Lay("process = _, _ : *;\n");
    const Node &root = d->Root;
    REQUIRE(root.Kind == Kind::Seq);
    const Node &par = root.Kids[0], &star = root.Kids[1];
    REQUIRE(par.Kind == Kind::Par);

    const std::vector<Link> w = Wires(root);
    // Counts differ, so the wires meet at one bundle point.
    CHECK(w.size() == 3);
    CHECK(Touches(w, par.Kids[0].Bounds.Right(), MidY(par.Kids[0])));
    CHECK(Touches(w, par.Kids[1].Bounds.Right(), MidY(par.Kids[1])));
    CHECK(Touches(w, star.Bounds.X, MidY(star)));
    CHECK(MidY(par) > par.Kids[0].Bounds.Bottom());
    CHECK(MidY(par) < par.Kids[1].Bounds.Y);
    CHECK_FALSE(Touches(w, par.Bounds.Right(), MidY(par)));
}

TEST_CASE("a `<:` fans to lanes, and does not reach past a stage into the next") {
    // The wide side of this `<:` is a `:`, whose children are stages and not lanes.
    const auto d = Lay("process = _ <: _, _ : route(2, 2, 1, 1, 2, 2);\n");
    const Node &root = d->Root;
    REQUIRE(root.Kind == Kind::Split);
    const Node &src = root.Kids[0], &seq = root.Kids[1];
    REQUIRE(seq.Kind == Kind::Seq);
    const Node &par = seq.Kids[0], &route = seq.Kids[1];
    REQUIRE(route.Kind == Kind::Route);

    const std::vector<Link> w = Wires(root);
    CHECK(Touches(w, src.Bounds.Right(), MidY(src)));
    CHECK(Touches(w, par.Kids[0].Bounds.X, MidY(par.Kids[0])));
    CHECK(Touches(w, par.Kids[1].Bounds.X, MidY(par.Kids[1])));
    for (const Link &l : w) {
        CHECK(l.X0 < route.Bounds.X);
        CHECK(l.X1 < route.Bounds.X);
    }
}

TEST_CASE("matching lane counts wire pairwise, and cross nothing") {
    const auto d = Lay("process = _, _ : *, *;\n");
    const Node &root = d->Root;
    REQUIRE(root.Kind == Kind::Seq);
    const Node &from = root.Kids[0], &to = root.Kids[1];
    REQUIRE(from.Kind == Kind::Par);
    REQUIRE(to.Kind == Kind::Par);

    const std::vector<Link> w = Wires(root);
    REQUIRE(w.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(w[i].X0 == doctest::Approx(from.Kids[i].Bounds.Right()));
        CHECK(w[i].Y0 == doctest::Approx(MidY(from.Kids[i])));
        CHECK(w[i].X1 == doctest::Approx(to.Kids[i].Bounds.X));
        CHECK(w[i].Y1 == doctest::Approx(MidY(to.Kids[i])));
    }
    CHECK(w[0].Y0 < w[1].Y0);
    CHECK(w[0].Y1 < w[1].Y1);
}

TEST_CASE("every wire stays inside the composition that drew it") {
    size_t wires = 0;
    ForEachDiagram([&](Session &, const FileView &, ValueId, const Node &root) {
        const auto check = [&](const Node &n, const auto &self) -> void {
            for (const Link &l : Wires(n)) {
                ++wires;
                CHECK(l.X0 >= n.Bounds.X - 1e-3f);
                CHECK(l.X1 <= n.Bounds.Right() + 1e-3f);
                CHECK(l.Y0 >= n.Bounds.Y - 1e-3f);
                CHECK(l.Y1 <= n.Bounds.Bottom() + 1e-3f);
            }
            for (const Node &k : n.Kids) self(k, self);
        };
        check(root, check);
        // A zero-length wire means an endpoint rule answered with one point twice.
        for (const Link &l : AllWires(root)) {
            const bool degenerate = Near(l.X0, l.X1) && Near(l.Y0, l.Y1);
            CHECK_FALSE(degenerate);
        }
    });
    MESSAGE("checked ", wires, " wires");
    CHECK(wires > 500);
}

TEST_CASE("a route is laid out around its channels, not around its text") {
    const auto d = Lay("process = route(2, 3, 1, 1, 2, 3);\n");
    const Node &r = d->Root;
    REQUIRE(r.Kind == Kind::Route);

    CHECK(r.Ports.size() == 5);
    CHECK(r.Wires == std::vector<std::pair<uint32_t, uint32_t>>{{1, 1}, {2, 3}});
    // The label carries the counts. The entries are the wires.
    CHECK(r.Label == "route(2, 3)");

    const auto port = [&](bool input, uint32_t channel) {
        for (const Port &p : r.Ports)
            if (p.Input == input && p.Channel == channel) return p;
        FAIL("no such port");
        return Port{};
    };
    CHECK(port(true, 1).X == doctest::Approx(r.Bounds.X));
    CHECK(port(false, 1).X == doctest::Approx(r.Bounds.Right()));
    CHECK(port(true, 1).Y < port(true, 2).Y);
    CHECK(port(false, 1).Y < port(false, 2).Y);
    CHECK(port(false, 2).Y < port(false, 3).Y);
    for (const Port &p : r.Ports) {
        CHECK(p.Y > r.Bounds.Y);
        CHECK(p.Y < r.Bounds.Bottom());
    }
    // One row per channel plus one for the label, which sits above the ports.
    CHECK(r.Bounds.H >= 4 * Metrics{}.LineHeight);
    for (const Port &p : r.Ports) CHECK(p.Y > r.Bounds.Y + Metrics{}.LineHeight);
}

TEST_CASE("a route's ports move with it") {
    const auto d = Lay("process = _ : route(2, 2, 1, 2, 2, 1);\n");
    const Node &r = d->Root.Kids[1];
    REQUIRE(r.Kind == Kind::Route);
    CHECK(r.Bounds.X > 0);
    for (const Port &p : r.Ports) CHECK(p.X >= r.Bounds.X);
}

TEST_CASE("a port answers to a point near it, and the nearest one wins") {
    const auto d = Lay("process = route(1, 1, 1, 1) : route(1, 1, 1, 1);\n");
    const Node &root = d->Root;
    const Node &left = root.Kids[0], &right = root.Kids[1];
    REQUIRE(left.Kind == Kind::Route);
    REQUIRE(right.Ports.size() == 2);

    const Port &out = *std::ranges::find_if(left.Ports, [](const Port &p) { return !p.Input; });
    const Layout::Endpoint hit = Layout::PortAt(root, out.X + 1, out.Y, 7.0f);
    REQUIRE(hit);
    CHECK(hit.Node == &left);
    CHECK(hit.Port->Channel == 1);
    CHECK_FALSE(hit.Port->Input);

    const Port &in = *std::ranges::find_if(right.Ports, [](const Port &p) { return p.Input; });
    const float midway = (out.X + in.X) / 2;
    CHECK(Layout::PortAt(root, out.X + 1, out.Y, 100.0f).Node == &left);
    CHECK(Layout::PortAt(root, in.X - 1, in.Y, 100.0f).Node == &right);
    CHECK(midway > out.X);

    CHECK_FALSE(Layout::PortAt(root, root.Bounds.Right() + 50, root.Bounds.Y, 7.0f));
}

TEST_CASE("a route the catalogue cannot rewire draws no ports") {
    // The view must decline wherever the rewrite does, or a drag is always refused.
    const auto d = Lay("n = 2;\nprocess = route(n, n, 1, 1);\n");
    CHECK(d->Root.Ports.empty());

    // An odd entry list pairs up nowhere.
    const auto odd = Lay("process = route(2, 2, 1, 1, 2);\n");
    CHECK(odd->Root.Wires.empty());
    CHECK(odd->Root.Ports.size() == 4);

    // An out-of-range entry carries no signal, so it is not drawn.
    const auto wide = Lay("process = route(2, 2, 1, 1, 3, 1);\n");
    CHECK(wide->Root.Wires == std::vector<std::pair<uint32_t, uint32_t>>{{1, 1}});
}
