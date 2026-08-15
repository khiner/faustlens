#include "syntax/Parser.h"
#include "syntax/Printer.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <format>
#include <string>

using namespace faustlens;
using namespace faustlens::test;

namespace {

std::string BodyShape(std::string_view expr) {
    static Terms terms;
    const std::string src = std::format("process = {};", expr);
    const ParseResult r = Parse(terms, src);
    REQUIRE(r.Diags.empty());
    return Shape(terms, ClauseBody(terms, r.Root));
}

} // namespace

TEST_CASE("precedence and associativity, from the one table the printer also reads") {
    CHECK(BodyShape("a : b : c") == "Seq[Ident(a) Seq[Ident(b) Ident(c)]]"); // right
    CHECK(BodyShape("a , b , c") == "Par[Ident(a) Par[Ident(b) Ident(c)]]"); // right
    CHECK(BodyShape("a ~ b ~ c") == "RecComp[RecComp[Ident(a) Ident(b)] Ident(c)]"); // left
    CHECK(BodyShape("a+b+c") == "BinOp(+)[BinOp(+)[Ident(a) Ident(b)] Ident(c)]"); // left
    CHECK(BodyShape("a : b , c") == "Seq[Ident(a) Par[Ident(b) Ident(c)]]");
    CHECK(BodyShape("a , b ~ c") == "Par[Ident(a) RecComp[Ident(b) Ident(c)]]");
    CHECK(BodyShape("a <: b : c") == "Split[Ident(a) Seq[Ident(b) Ident(c)]]");
    CHECK(BodyShape("a+b*c") == "BinOp(+)[Ident(a) BinOp(*)[Ident(b) Ident(c)]]");
    CHECK(BodyShape("a*b@c") == "BinOp(*)[Ident(a) BinOp(@)[Ident(b) Ident(c)]]"); // @ is 11
    CHECK(BodyShape("a@b*c") == "BinOp(*)[BinOp(@)[Ident(a) Ident(b)] Ident(c)]");
    CHECK(BodyShape("a.b(x)") == "Apply[Access(b)[Ident(a)] Ident(x)]");
    CHECK(BodyShape("a'.b") == "Access(b)[Delay1[Ident(a)]]");
    CHECK(BodyShape("a@b'") == "BinOp(@)[Ident(a) Delay1[Ident(b)]]");
}

TEST_CASE("level 2 admits `:` but not `,`") {
    CHECK(BodyShape("f(a : b, c)") == "Apply[Ident(f) Seq[Ident(a) Ident(b)] Ident(c)]");
    CHECK(BodyShape("f((a, b))") == "Apply[Ident(f) Par[Ident(a) Ident(b)]]");
    CHECK(!Accepts("process = f(a with { b = 1; });"));
}

TEST_CASE("prefix minus: the grammar's three productions, which desugar differently") {
    CHECK(BodyShape("-x") == "NegIdent(x)");
    CHECK(BodyShape("-3") == "Int(-3)");
    CHECK(BodyShape("-3.5") == "Real(-3.5)");
    CHECK(BodyShape("-(1+2)") == "Apply[Prim(-) BinOp(+)[Int(1) Int(2)]]");
    CHECK(BodyShape("+3") == "Int(+3)");
    CHECK(BodyShape("a-3") == "BinOp(-)[Ident(a) Int(3)]");
    CHECK(BodyShape("a- -3") == "BinOp(-)[Ident(a) Int(-3)]");
    CHECK(BodyShape("a : -") == "Seq[Ident(a) Prim(-)]");
    CHECK(BodyShape("-x'") == "Delay1[NegIdent(x)]");
}

TEST_CASE("the two power spellings are one primitive under a form tag") {
    Terms t;
    const ParseResult a = Parse(t, "process = ^;");
    const ParseResult b = Parse(t, "process = pow;");
    REQUIRE(a.Diags.empty());
    REQUIRE(b.Diags.empty());
    CHECK(a.Root != b.Root); // the spelling is kept
    CHECK(BodyShape("x^y") == "BinOp(^)[Ident(x) Ident(y)]");
}

TEST_CASE("the two merge spellings are one node under a form tag") {
    Terms t;
    const ParseResult a = Parse(t, "process = x :> y;");
    const ParseResult b = Parse(t, "process = x +> y;");
    REQUIRE(a.Diags.empty());
    REQUIRE(b.Diags.empty());
    CHECK(a.Root != b.Root);
}

TEST_CASE("literal lexemes are kept verbatim") {
    Terms t;
    for (const auto &[a, b] : {std::pair{"3", "3."}, {"3", "3f"}, {".5", "0.5"}, {"3", "+3"}}) {
        const std::string sa = std::format("process = {};", a);
        const std::string sb = std::format("process = {};", b);
        CHECK(Parse(t, sa).Root != Parse(t, sb).Root);
    }
}

TEST_CASE("statements") {
    CHECK(Accepts("import(\"stdfaust.lib\");"));
    CHECK(Accepts("declare name \"x\";"));
    CHECK(Accepts("declare foo author \"me\";"));
    CHECK(Accepts("doubleprecision process = 1;"));
    CHECK(Accepts("singleprecision doubleprecision process = 1;"));
    CHECK(Accepts("process = 1;"));
    CHECK(Accepts("f(x, y) = x + y;"));
    CHECK(Accepts("f(0) = 1; f(1) = 2;"));
    CHECK(!Accepts("process = foo with { import(\"x.lib\"); foo = 1; };"));
    CHECK(Accepts("process = foo with { foo = environment { import(\"x.lib\"); }; };"));
}

TEST_CASE("consecutive clauses of one name form one Definition") {
    Terms t;
    const ParseResult r = Parse(t, "f(0) = 1;\nf(1) = 2;\nprocess = f;");
    REQUIRE(r.Diags.empty());
    const auto stmts = t.Children(r.Root);
    REQUIRE(stmts.size() == 2);
    CHECK(t.KindOf(stmts[0]) == Kind::Definition);
    CHECK(t.Children(stmts[0]).size() == 2);
    const ParseResult split = Parse(t, "f(0) = 1;\ng = 3;\nf(1) = 2;");
    CHECK(t.Children(split.Root).size() == 3);
}

TEST_CASE("the constructs with fixed shapes") {
    CHECK(Accepts("process = \\(x).(x + 1);"));
    CHECK(Accepts("process = case { (0) => 1; (x) => x; };"));
    CHECK(Accepts("process = par(i, 4, i);"));
    CHECK(Accepts("process = seq(i, 4, i) : sum(j, 2, j) : prod(k, 2, k);"));
    CHECK(Accepts("process = inputs(x) , outputs(x);"));
    CHECK(Accepts("process = environment { a = 1; };"));
    CHECK(Accepts("process = waveform{0, 1, -1, 0.5};"));
    CHECK(Accepts("process = route(2, 2, 1,1, 2,2);"));
    CHECK(Accepts("process = route(2, 2);"));
    CHECK(Accepts("process = component(\"x.dsp\") : library(\"y.lib\").f;"));
    CHECK(Accepts("process = hslider(\"a\", 0, 0, 1, 0.1);"));
    CHECK(Accepts("process = vgroup(\"g\", button(\"b\"));"));
    CHECK(Accepts("process = vbargraph(\"a\", 0, 1);"));
    CHECK(Accepts("process = soundfile(\"a\", 2);"));
    // An ffunction name that lexes as a keyword (`sin`, `min`) is unusable, in the reference too.
    CHECK(Accepts("process = ffunction(float sinhf|sinh|sinhl(float), <math.h>, \"\");"));
    CHECK(!Accepts("process = ffunction(float sinf|sin|sinl(float), <math.h>, \"\");"));
    CHECK(Accepts("process = ffunction(float f(), <math.h>, \"\");"));
    CHECK(Accepts("process = fconstant(int x, <math.h>) , fvariable(float y, <math.h>);"));
    CHECK(Accepts("process = a letrec { 'x = x + 1; };"));
    CHECK(Accepts("process = a letrec { 'x = y; where y = 1; };"));
    CHECK(Accepts("process = a[b = 1;];"));
    CHECK(Accepts("process = [\"freq\": replace -> synth] with { replace = _; synth = _; };"));
    Terms t;
    CHECK(Parse(t, "process = route(2,2);").Root != Parse(t, "process = route(2,2,1,1);").Root);
}

TEST_CASE("the two-argument route keeps its arity") {
    Terms t;
    const ParseResult r = Parse(t, "process = route(2, 2);");
    REQUIRE(r.Diags.empty());
    const ValueId route = t.Children(t.Children(t.Child(r.Root, 0))[0]).back();
    CHECK(t.KindOf(route) == Kind::Route);
    CHECK(t.Children(route).size() == 2);
}

TEST_CASE("`[` heads two productions, separated by position") {
    CHECK(BodyShape("a[b = 1;]").starts_with("ModifLocalDef"));
    CHECK(BodyShape("[\"a\" -> b]").starts_with("Modulation"));
}

TEST_CASE("grouping parentheses live on the ref, not the value") {
    Terms t;
    const ParseResult plain = Parse(t, "process = a : b;");
    const ParseResult paren = Parse(t, "process = (a : b);");
    REQUIRE(paren.Diags.empty());
    CHECK(plain.Root == paren.Root);
    bool found = false;
    for (const TermRef &r : paren.Refs.Refs) {
        if (r.OuterBegin != r.SpanBegin) {
            found = true;
            CHECK(r.OuterEnd > r.SpanEnd);
        }
    }
    CHECK(found);
}

TEST_CASE("spans nest and cover their children") {
    Terms t;
    const std::string src = "process = (a : b) , c;";
    const ParseResult r = Parse(t, src);
    REQUIRE(r.Diags.empty());
    for (RefId i = 0; i < r.Refs.Refs.size(); ++i) {
        const TermRef &p = r.Refs.Refs[i];
        CHECK(p.OuterBegin <= p.SpanBegin);
        CHECK(p.OuterEnd >= p.SpanEnd);
        for (const RefId c : r.Refs.Children(i)) {
            CHECK(r.Refs.Refs[c].OuterBegin >= p.SpanBegin);
            CHECK(r.Refs.Refs[c].OuterEnd <= p.SpanEnd);
        }
    }
}

TEST_CASE("cursor to box finds the innermost ref") {
    Terms t;
    const std::string src = "process = a : bbb;";
    const ParseResult r = Parse(t, src);
    const RefId at = r.Refs.Innermost(uint32_t(src.find("bbb") + 1));
    REQUIRE(at != NoRef);
    CHECK(t.KindOf(r.Refs.Refs[at].ValueId) == Kind::Ident);
    CHECK(t.Lexeme(r.Refs.Refs[at].ValueId) == "bbb");
}

TEST_CASE("nesting past the depth bound is a diagnostic, not a crash") {
    // Every traversal is recursive.
    Terms t;
    const std::string deep = std::format("process = {}1{};", std::string(MaxTermDepth + 8, '('), std::string(MaxTermDepth + 8, ')'));
    const ParseResult r = Parse(t, deep);
    bool found = false;
    for (const Diagnostic &d : r.Diags) found |= d.Code == Code::SynDepthExceeded;
    CHECK(found);
    CHECK(r.Root != NoTerm);

    const uint32_t ok = MaxTermDepth - 8;
    const std::string shallow = std::format("process = {}1{};", std::string(ok, '('), std::string(ok, ')'));
    const ParseResult s = Parse(t, shallow);
    CHECK(s.Diags.empty());
    CHECK(PrintTerm(t, s.Root) == "process = 1;");
}

TEST_CASE("interning deduplicates globally") {
    Terms t;
    const ParseResult a = Parse(t, "process = os::osc(440) : _;");
    const ParseResult b = Parse(t, "process = os::osc(440) : _;");
    CHECK(a.Root == b.Root);
    const size_t before = t.Size();
    Parse(t, "process = os::osc(440) : _;");
    CHECK(t.Size() == before);
}
