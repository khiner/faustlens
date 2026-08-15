// Evaluation probes, each targeting behaviour the corpus does not reach.
#include "conformance/BoxCompare.h"
#include "query/Query.h"

#include "doctest.h"

#include <string>

using namespace faustlens;
using namespace faustlens::test;

namespace {

struct Program {
    Session S;
    BoxId Box;

    explicit Program(const std::string &src, const std::string &path = "/probe.dsp") : Box(Load(S, src, path)) {}

    // The buffer has to land before the query runs, so the order lives in one function.
    static BoxId Load(Session &s, const std::string &src, const std::string &path) {
        s.SetBuffer(path, src);
        return s.Process(path);
    }

    bool Ok() const { return FirstError(S).empty(); }
    std::string Error() const { return FirstError(S); }
    std::string Shape() const { return PrintBox({S.Boxes, S.Terms}, Box, 12); }
    Arity Arity() const { return S.Boxes.ArityOf(Box); }
    bool Raised(Code c) const { return faustlens::test::Raised(S, c); }
};

std::string Shape(const std::string &body) {
    const Program p("process = " + body + ";");
    if (!p.Ok()) return "ERROR " + p.Error();
    return p.Shape();
}

bool Accepts(const std::string &src) { return Program(src).Ok(); }

} // namespace

TEST_CASE("evaluation constant-folds numeric tuples through primitives") {
    CHECK(Shape("2, 3 : +") == "Int(5)");
    CHECK(Shape("2+3") == "Int(5)");
    CHECK(Shape("3/2") == "Real(1.500000)");
    CHECK(Shape("3.5 : int") == "Int(3)");
    // The fold is narrow: only numbers into a one- or two-argument primitive.
    CHECK(Shape("2 : mem") == "Seq[Int(2) Prim(mem)]");
    CHECK(Shape("2, 3 : @") == "Seq[Par[Int(2) Int(3)] Prim(@)]");
    CHECK(Shape("2 ^ 3") == "Int(8)");
    CHECK(Shape("2 ^ 3.0") == "Real(8.000000)");
}

TEST_CASE("iteration at zero count is two different answers") {
    // All three yield the 0->0 circuit, so `sum(i, 0, e)` is not 0 and `prod` not 1.
    const std::string zero = "Route[Int(0) Int(0) Par[Int(0) Int(0)]]";
    CHECK(Shape("par(i, 0, _)") == zero);
    CHECK(Shape("sum(i, 0, 1)") == zero);
    CHECK(Shape("prod(i, 0, 1)") == zero);
    // `seq` at zero measures its body once, then returns a bus of that width.
    CHECK(Shape("seq(i, 0, _)") == "Wire");
    CHECK(Shape("seq(i, 0, _,_)") == "Par[Wire Wire]");
    // It still requires `ins == outs`, over a body that never runs.
    CHECK(!Accepts("process = seq(i, 0, _,_ : +);"));
    CHECK(Shape("seq(i, 0, par(j, 0, _))") == zero);
}

TEST_CASE("iteration unrolls in the reference's order") {
    CHECK(Shape("par(i, 3, i)") == "Par[Int(0) Par[Int(1) Int(2)]]");
    CHECK(Shape("seq(i, 2, _)") == "Seq[Wire Wire]");
    // `sum`/`prod` build the composition directly, so the numeric fold never sees it.
    CHECK(Shape("sum(i, 3, i)") == "Seq[Par[Seq[Par[Int(0) Int(1)] Prim(+)] Int(2)] Prim(+)]");
    CHECK(Shape("prod(i, 2, i+1)") == "Seq[Par[Int(1) Int(2)] Prim(*)]");
}

TEST_CASE("`with` sees the enclosing scope and `letrec` becomes one") {
    CHECK(Shape("a with { a = 1; }") == "Int(1)");
    CHECK(Shape("a with { a = b; b = 2; }") == "Int(2)");
    CHECK(Shape("f(3) with { f(x) = x*2; }") == "Int(6)");
    CHECK(Accepts("process = x letrec { 'x = x+1; };"));
    // The `where` definitions sit inside the abstraction body and do not escape.
    CHECK(Accepts("process = x letrec { 'x = f(x); where f(u) = u+1; };"));
    CHECK(!Accepts("process = f letrec { 'x = 1; where f = 1; };"));
}

TEST_CASE("`.` resolves in the captured environment, not the current one") {
    CHECK(Shape("e.a with { e = environment { a = n; n = 1; }; n = 99; }") == "Int(1)");
    CHECK(Shape("e.f.a with { e = environment { f = environment { a = k; }; k = 7; }; }") == "Int(7)");
    CHECK(!Accepts("process = 1 . a;"));
}

TEST_CASE("patterns are evaluated and pattern variables survive it") {
    // A closed numeric sub-pattern folds, so `f(2+3)` and `f(5)` are one pattern.
    CHECK(Shape("f(5) with { f(2+3) = 1; f(x) = 2; }") == "Int(1)");
    CHECK(Shape("f(0) with { f(0) = 1; f(x) = 2; }") == "Int(1)");
    CHECK(Shape("f(9) with { f(0) = 1; f(x) = 2; }") == "Int(2)");
    // A variable appearing twice binds twice, so `f(x, x)` needs equal arguments.
    CHECK(Shape("f(3, 3) with { f(x, x) = 1; f(x, y) = 2; }") == "Int(1)");
    CHECK(Shape("f(3, 4) with { f(x, x) = 1; f(x, y) = 2; }") == "Int(2)");
    // An identifier callee is a reference, not a binder.
    CHECK(Shape("f(g(1)) with { g(x) = x; f(g(1)) = 7; f(x) = 8; }") == "Int(7)");
    CHECK(!Accepts("process = f(1) with { f(0) = 1; };"));
    // All rules share one pattern count, checked at construction.
    CHECK(!Accepts("f(x) = 1;\nf(x, y) = 2;\nprocess = f;"));
    CHECK(!Accepts("f = 1;\nf = 2;\nprocess = f;"));
}

TEST_CASE("pattern matching is incremental over applied arguments") { CHECK(Shape("g(2) with { f(1, x) = x; f(0, x) = 0; g = f(1); }") == "Int(2)"); }

TEST_CASE("redefining a name within one layer is an error") {
    CHECK(Accepts("a = 1;\nprocess = a;"));
    CHECK(!Accepts("process = a with { a = 1; a = 2; };"));
    // Rejected even where the two agree. Duplicates merge only across layers, as imports do.
    CHECK(!Accepts("process = a with { a = 1; a = 1; };"));
}

TEST_CASE("`e[defs]` is a modification, not a substitution") {
    // Only the closure's top layer is copied.
    CHECK(Shape("e[a = 2;].b with { e = environment { a = 1; b = a; }; }") == "Int(2)");
    CHECK(Shape("e[a = n;].b with { e = environment { a = 1; b = a; }; n = 5; }") == "Int(5)");
    CHECK(Shape("e[a = 2;].c with { e = environment { a = 1; c = k; }; k = 8; }") == "Int(8)");
    CHECK(!Accepts("process = 1[a = 2;];"));
}

TEST_CASE("modulation, the four ways to get it backwards") {
    // An omitted circuit means `*`, with two inputs, so the bare form gains an input.
    const Program bare("process = [\"Wet\" -> hslider(\"Wet\", 0, 0, 1, 0.1)];");
    REQUIRE_MESSAGE(bare.Ok(), bare.Error());
    CHECK(bare.Arity().Ins == 1);
    CHECK(bare.Arity().Outs == 1);
    CHECK(bare.Shape().starts_with("Symbolic["));

    const Program one("process = [\"Wet\": *(2) -> hslider(\"Wet\", 0, 0, 1, 0.1)];");
    REQUIRE_MESSAGE(one.Ok(), one.Error());
    CHECK(one.Arity().Ins == 0);
    CHECK(one.Shape() == "Seq[NumericWidget(Wet) Seq[Par[Wire Int(2)] Prim(*)]]");

    CHECK(Shape("[\"Wet\": 0.5 -> hslider(\"Wet\", 0, 0, 1, 0.1)]") == "Real(0.500000)");

    // Matching is ordered-subsequence containment, not equality.
    CHECK(Shape("[\"Gain\": 0.5 -> vgroup(\"Amp\", hslider(\"Gain\", 0, 0, 1, 0.1))]") == "Group(Amp)[Real(0.500000)]");

    CHECK(Shape("[\"Gain\": 0.5 -> vgroup(\"Other\", hslider(\"Gain\", 0, 0, 1, 0.1))]") == "Group(Other)[Real(0.500000)]");

    CHECK(Shape("[\"Gain\": 0.5 -> hslider(\"Gain [unit:dB]\", 0, 0, 1, 0.1)]") == "Real(0.500000)");

    const Program miss("process = [\"Nope\": 0.5 -> hslider(\"Gain\", 0, 0, 1, 0.1)];");
    CHECK(miss.Ok());
    CHECK(miss.Shape() == "NumericWidget(Gain)");
    CHECK(miss.Raised(Code::EvalNoModulationTarget));

    CHECK(!Accepts("process = [\"a\": (_,_,_ :> _) -> hslider(\"a\", 0, 0, 1, 1)];"));
    CHECK(!Accepts("process = [\"a\": (_,_) -> hslider(\"a\", 0, 0, 1, 1)];"));
}

TEST_CASE("labels are evaluated, not copied") {
    // The padding is `printf` field width, not zero-fill, so `%2i` at `i = 3` is `" 3"`.
    CHECK(Shape("par(i, 2, button(\"b%i\"))") == "Par[Button(b0) Button(b1)]");
    CHECK(Shape("par(i, 1, button(\"b%{i}\"))") == "Button(b0)");
    CHECK(Shape("par(i, 4, button(\"b%2i\"))").find("b 3") != std::string::npos);
    CHECK(Shape("button(\"100%\")") == "Button(100%)");
}

TEST_CASE("`inputs` and `outputs` report arity as a number") {
    CHECK(Shape("inputs(_,_)") == "Int(2)");
    CHECK(Shape("outputs(_ <: _,_,_)") == "Int(3)");
    CHECK(Shape("inputs(!)") == "Int(1)");
}

TEST_CASE("arity is checked at construction and `Error` absorbs its neighbours") {
    CHECK(!Accepts("process = _ : _,_;"));
    CHECK(!Accepts("process = _,_ :> _,_,_;")); // v must be a multiple of x
    CHECK(!Accepts("process = _,_ <: _,_,_;")); // x must be a multiple of v
    CHECK(Accepts("process = _ <: _,_,_;"));
    CHECK(Accepts("process = _,_ :> _;"));
    const Program p("process = (_ : _,_) : _ : _;");
    int errors = 0;
    for (const Diagnostic &d : p.S.Diagnostics())
        if (d.Severity == Severity::Error) ++errors;
    CHECK(errors == 1);
}

TEST_CASE("divergence is bounded") {
    const Program p("foo = foo;\nprocess = foo;");
    CHECK(!p.Ok());
    CHECK(p.Raised(Code::EvalLoopDetected));
}

TEST_CASE("a hole evaluates to Error, and the rest of the file still evaluates") {
    const Program p("broken = ?;\nprocess = 1;");
    CHECK(p.Shape() == "Int(1)");
}

TEST_CASE("the precision filter drops a statement and says so") {
    // The build mode is f64, so the `singleprecision` definition is filtered out.
    const Program p("singleprecision process = 1;\ndoubleprecision process = 2;");
    REQUIRE_MESSAGE(p.Ok(), p.Error());
    CHECK(p.Shape() == "Int(2)");
    bool info = false;
    for (const Diagnostic &d : p.S.Diagnostics()) info = info || (d.Code == Code::InfoPrecisionFiltered && d.Severity == Severity::Info);
    CHECK(info);
    CHECK(!Accepts("singleprecision process = 1;"));
}

TEST_CASE("`component` and `library` evaluate in a fresh, empty environment") {
    Session s;
    s.SetBuffer("/lib.dsp", "n = 1;\nprocess = n;\n");
    s.SetBuffer("/main.dsp", "n = 99;\nprocess = component(\"/lib.dsp\");\n");
    const BoxId b = s.Process("/main.dsp");
    REQUIRE(!s.Boxes.IsError(b));
    CHECK(PrintBox({s.Boxes, s.Terms}, b, 4) == "Int(1)");

    Session t;
    t.SetBuffer("/lib.dsp", "n = 1;\na = n;\n");
    t.SetBuffer("/main.dsp", "n = 99;\nprocess = library(\"/lib.dsp\").a;\n");
    const BoxId c = t.Process("/main.dsp");
    REQUIRE(!t.Boxes.IsError(c));
    CHECK(PrintBox({t.Boxes, t.Terms}, c, 4) == "Int(1)");
}

TEST_CASE("`declare name key \"v\"` is reachability-dependent") {
    // It wraps the named definition's body, so an unreached one contributes nothing.
    const auto has = [](const Session &s, std::string_view suffix) {
        for (const auto &[k, v] : s.Metadata.Entries)
            if (k.size() >= suffix.size() && k.ends_with(suffix)) return true;
        return false;
    };
    Program const reached("declare used author \"me\";\nused = 1;\nprocess = used;");
    REQUIRE(reached.Ok());
    CHECK(has(reached.S, "used:author"));

    Program const unreached("declare unused author \"me\";\nunused = 1;\nprocess = 2;");
    REQUIRE(unreached.Ok());
    CHECK(!has(unreached.S, "unused:author"));

    Program const file_level("declare author \"me\";\nprocess = 1;");
    REQUIRE(file_level.Ok());
    CHECK(has(file_level.S, "author"));
}
