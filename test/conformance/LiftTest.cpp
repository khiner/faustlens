// The law is that lift, print, parse, evaluate is the identity on Box, not PutGet: a
// lifted `Int(-5)` parses back as a subtraction.
#include "eval/Lift.h"
#include "conformance/BoxCompare.h"
#include "conformance/Sweep.h"
#include "query/Query.h"
#include "syntax/Printer.h"

#include "doctest.h"

#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

TEST_CASE("the Box-to-Term lift: print it, read it back, and it is the same circuit") {
    namespace fs = std::filesystem;
    size_t lifted = 0, declined = 0, round_tripped = 0;
    std::vector<std::string> failures;
    std::map<std::string, size_t> declines; // grouped by cause, not by program

    for (const fs::path &p : DspPaths()) {
        Session s;
        s.AddSearchPath(p.parent_path());
        const std::string path = fs::weakly_canonical(p).string();
        const BoxId box = s.Process(path);
        if (s.Boxes.IsError(box)) continue;

        const Lifted out = Lift(s.Terms, s.Boxes, box);
        if (!out) {
            ++declined;
            declines[std::format("{}: {}", BoxKindName(s.Boxes.KindOf(out.At)), out.Declined)] += 1;
            continue;
        }
        ++lifted;

        const std::string text = "process = " + PrintTerm(s.Terms, out.Term) + ";\n";
        const std::string echo = "/lift_test.dsp";
        s.SetBuffer(echo, text);
        const BoxId again = s.Process(echo);
        if (s.Boxes.IsError(again)) {
            failures.push_back(p.filename().string() + ": the lifted text does not compile");
            continue;
        }
        // Isomorphism, not id equality: slot numbers and side-table indices are fresh per
        // construction.
        const BoxSide side{s.Boxes, s.Terms};
        if (const auto same = Isomorphic(side, box, side, again); !same) {
            failures.push_back(p.filename().string() + ": " + same.error() + "\n  " + text.substr(0, 300));
            continue;
        }
        ++round_tripped;
    }

    for (const std::string &f : failures) MESSAGE(f);
    for (const auto &[why, n] : declines) MESSAGE("declined, ", n, ": ", why);
    MESSAGE("lifted ", lifted, " of ", lifted + declined, ", ", round_tripped, " back to the same box");
    CHECK(failures.empty());
    CHECK(lifted == round_tripped);
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
             // `^` is infix only, so a nullary power is spelled `pow`.
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
