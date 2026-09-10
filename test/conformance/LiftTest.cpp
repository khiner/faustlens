// Check circuit equivalence after lift, print, parse, and evaluation.
#include "eval/Lift.h"
#include "conformance/BoxCompare.h"
#include "conformance/Sweep.h"
#include "query/Query.h"
#include "syntax/Printer.h"

#include "doctest.h"

#include <filesystem>
#include <format>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

TEST_CASE("lifting requires every symbolic slot to have a visible binder") {
    Session s;
    const StrId x = s.Terms.InternStr("fl_slot2");
    const BoxId ambient = s.Boxes.NewSlot(x);
    const BoxId local = s.Boxes.NewSlot(s.Terms.InternStr("y"));
    const BoxId body = s.Boxes.Make(BoxKind::Par, {ambient, local});
    const BoxId lambda = s.Boxes.Make(BoxKind::Symbolic, {local, body});
    CHECK_FALSE(Lift(s.Terms, s.Boxes, ambient));
    const std::vector<SlotName> names{{s.Boxes.Get(ambient).Aux, x}};
    const auto lifted = Lift(s.Terms, s.Boxes, lambda, names);
    REQUIRE(lifted);
    CHECK(PrintTerm(s.Terms, lifted.Term) == "\\(fl_slot2_1).(fl_slot2,fl_slot2_1)");
    const BoxId escaped = s.Boxes.Make(BoxKind::Par, {lambda, body});
    CHECK_FALSE(Lift(s.Terms, s.Boxes, escaped, names));
}

TEST_CASE("the Box-to-Term lift: print it, read it back, and it is the same circuit") {
    namespace fs = std::filesystem;
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    for (const auto &path : paths) {
        INFO(path.string());
        Session session;
        session.AddSearchPath(path.parent_path());
        const auto box = session.Process(fs::weakly_canonical(path).string());
        REQUIRE_FALSE(session.Boxes.IsError(box));
        const auto lifted = Lift(session.Terms, session.Boxes, box);
        REQUIRE_MESSAGE(lifted, lifted.Declined);
        const auto text = "process = " + PrintTerm(session.Terms, lifted.Term) + ";\n";
        session.SetBuffer("/lift_test.dsp", text);
        const auto again = session.Process("/lift_test.dsp");
        REQUIRE_FALSE(session.Boxes.IsError(again));
        const BoxSide side{session.Boxes, session.Terms};
        const auto same = Isomorphic(side, box, side, again);
        CHECK_MESSAGE(same, (same ? "" : same.error()));
    }
}

TEST_CASE("the lift takes the desugared spelling everywhere") {
    struct Case {
        const char *Source;
        const char *Lifted;
    };
    for (const Case c : {
             Case{"process = _ + _;", "process = _,_ : +;"},
             Case{"process = _';", "process = _ : mem;"},
             Case{"process = 0 - _;", "process = 0,_ : -;"},
             Case{"process = _ @ 3;", "process = _,3 : @;"},
             Case{"process = _ ^ 2;", "process = _,2 : pow;"},
         }) {
        CAPTURE(c.Source);
        Session s;
        const std::string path = "/desugar.dsp";
        s.SetBuffer(path, c.Source);
        const BoxId box = s.Process(path);
        REQUIRE_FALSE(s.Boxes.IsError(box));
        const Lifted out = Lift(s.Terms, s.Boxes, box);
        REQUIRE(out.Term != NoTerm);
        CHECK(std::format("process = {};", PrintTerm(s.Terms, out.Term)) == c.Lifted);
    }
}

TEST_CASE("the lift is partial by design, and says which node it stopped on") {
    Session s;
    const std::string path = "/partial.dsp";
    s.SetBuffer(path, "process = _ : undefined_name;");
    const BoxId box = s.Process(path);
    const Lifted out = Lift(s.Terms, s.Boxes, box);
    CHECK(out.Term == NoTerm);
    CHECK(out.Declined != nullptr);
    CHECK(out.At != NoBox);
}

TEST_CASE("iterations are unrolled, which is what the evaluated view shows") {
    Session s;
    const std::string path = "/unroll.dsp";
    s.SetBuffer(path, "process = par(i, 3, _ * i);");
    const BoxId box = s.Process(path);
    REQUIRE_FALSE(s.Boxes.IsError(box));
    const Lifted out = Lift(s.Terms, s.Boxes, box);
    REQUIRE(out.Term != NoTerm);
    CHECK(PrintTerm(s.Terms, out.Term) == "(_,0 : *),(_,1 : *),(_,2 : *)");
}
