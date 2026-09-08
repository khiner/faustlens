#include "query/Query.h"
#include "query/Snapshot.h"
#include "signal/Plan.h"

#include "doctest.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace faustlens;

namespace {

struct Keys {
    bool Ok = false;
    std::multiset<uint64_t> Content, Shape;
    std::vector<Field> Fields;
};

Keys Compile(const std::string &source) {
    Keys k;
    Session s;
    s.SetBuffer("/m.dsp", source);
    Signals sigs;
    const Graph g(s, "/m.dsp", sigs);
    if (!g.Ok) return k;
    const auto plan = g.Lower();
    if (!plan) return k;
    for (const Field &f : plan->Fields) {
        k.Fields.push_back(f);
        k.Content.insert(f.Hash);
        k.Shape.insert(f.Shape);
    }
    k.Ok = true;
    return k;
}

// Exclude recomputed tables and separately stored UI values.
std::multiset<uint64_t> Carried(const Keys &k, bool shape) {
    std::multiset<uint64_t> out;
    for (const Field &f : k.Fields)
        if (f.Kind == FieldKind::Delay || f.Kind == FieldKind::Perm) out.insert(shape ? f.Shape : f.Hash);
    return out;
}

} // namespace

TEST_CASE("a compile is its own baseline") {
    const std::string src = "process = (+ : *(0.5)) ~ _;";
    const Keys a = Compile(src), b = Compile(src);
    REQUIRE(a.Ok);
    REQUIRE(b.Ok);
    CHECK(a.Content == b.Content);
    CHECK(a.Shape == b.Shape);
    CHECK_FALSE(a.Content.empty());
}

TEST_CASE("a gain edit inside a feedback network keeps the shape") {
    // A loop constant changes content hash while preserving shape hash.
    const Keys before = Compile("process = (+ : *(0.5)) ~ _;");
    const Keys after = Compile("process = (+ : *(0.6)) ~ _;");
    REQUIRE(before.Ok);
    REQUIRE(after.Ok);
    CHECK(Carried(before, true) == Carried(after, true));
    CHECK(Carried(before, false) != Carried(after, false));
}

TEST_CASE("a structural edit moves both keys") {
    const Keys before = Compile("process = (+ : *(0.5)) ~ _;");
    const Keys after = Compile("process = (+ : *(0.5) : *(2.0)) ~ _;");
    REQUIRE(before.Ok);
    REQUIRE(after.Ok);
    CHECK(Carried(before, true) != Carried(after, true));
    CHECK(Carried(before, false) != Carried(after, false));
}

TEST_CASE("an edit elsewhere in the file leaves an untouched filter alone") {
    // Preserve content identity across changed intern ids.
    const Keys before = Compile(
        "a = (+ : *(0.5)) ~ _;\n"
        "b = (+ : *(0.25)) ~ _;\n"
        "process = a, b;\n"
    );
    const Keys after = Compile(
        "a = (+ : *(0.5)) ~ _;\n"
        "b = (+ : *(0.30)) ~ _;\n"
        "process = a, b;\n"
    );
    REQUIRE(before.Ok);
    REQUIRE(after.Ok);
    const std::multiset<uint64_t> from = Carried(before, false), to = Carried(after, false);
    REQUIRE(from.size() == 2);
    REQUIRE(to.size() == 2);
    int survived = 0;
    for (const uint64_t h : from) survived += to.contains(h) ? 1 : 0;
    CHECK(survived == 1);
    CHECK(Carried(before, true) == Carried(after, true));
}

TEST_CASE("a whole filter added beside one does not move the first's identity") {
    // Use the same input on both branches to isolate the constant edit.
    const Keys before = Compile("process = (+ : *(0.5)) ~ _;");
    const Keys after = Compile("process = _ <: ((+ : *(0.5)) ~ _), ((+ : @(7) : *(0.25)) ~ _) :> _;");
    REQUIRE(before.Ok);
    REQUIRE(after.Ok);
    for (const uint64_t h : Carried(before, false)) CHECK(Carried(after, false).contains(h));
}

TEST_CASE("a delay line resized keeps its identity, which is what the length rule is for") {
    // Delay extent changes independently of its producer's identity.
    const Keys before = Compile("process = _ @ 128;");
    const Keys after = Compile("process = _ @ 256;");
    REQUIRE(before.Ok);
    REQUIRE(after.Ok);
    const auto line = [](const Keys &k) {
        for (const Field &f : k.Fields)
            if (f.Kind == FieldKind::Delay) return f;
        return Field{};
    };
    CHECK(line(before).Extent != line(after).Extent);
    CHECK(line(before).Hash == line(after).Hash);
    CHECK(line(before).Shape == line(after).Shape);
}

TEST_CASE("a field traces back to where it is written") {
    const std::string src = "lo = (+ : *(0.5)) ~ _;\n"
                            "hi = (+ : *(0.25)) ~ _;\n"
                            "process = lo, hi;\n";
    Session s;
    s.SetBuffer("/p.dsp", src);
    Signals sigs;
    const Graph g(s, "/p.dsp", sigs);
    REQUIRE(g.Ok);
    const auto lowered = g.Lower();
    REQUIRE(lowered);
    const Plan &plan = *lowered;

    const Snapshot snap = Publish(s, {"/p.dsp"});
    const FileView *f = snap.File("/p.dsp");
    REQUIRE(f != nullptr);

    std::set<size_t> lines;
    int traced = 0;
    for (const Field &fl : plan.Fields) {
        if (fl.Kind != FieldKind::Delay && fl.Kind != FieldKind::Perm) continue;
        REQUIRE(fl.Origin != NoTerm);
        const std::vector<Span> at = Marks(*f, fl.Origin);
        REQUIRE_FALSE(at.empty());
        uint32_t first = at[0].Begin;
        for (const Span &sp : at) first = std::min(first, sp.Begin);
        CHECK(first < src.size());
        lines.insert(std::count(src.begin(), src.begin() + first, '\n'));
        ++traced;
    }
    CHECK(traced == 2);
    CHECK(lines.size() == 2);
}

TEST_CASE("a label is content, and its interning order is not") {
    // Compare text payloads across differently ordered intern pools.
    const Keys before = Compile("process = hslider(\"gain\", 0, 0, 1, 0.01) * (_ @ 4);");
    const Keys after = Compile("process = hslider(\"gain\", 0, 0, 1, 0.01) * (_ @ 8) * hslider(\"other\", 1, 0, 1, 0.01);");
    REQUIRE(before.Ok);
    REQUIRE(after.Ok);
    const auto widget = [](const Keys &k) {
        for (const Field &f : k.Fields)
            if (f.Kind == FieldKind::Widget) return f;
        return Field{};
    };
    CHECK(widget(before).Hash == widget(after).Hash);
    CHECK(widget(before).Shape == widget(after).Shape);
}
