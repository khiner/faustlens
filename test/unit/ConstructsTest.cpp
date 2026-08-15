// The constructs the corpus barely exercises: precision prefixes, `letrec`, `e[defs]`, modulation.
#include "syntax/Parser.h"
#include "syntax/Printer.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <string>
#include <string_view>

using namespace faustlens;
using namespace faustlens::test;

namespace {

struct Parsed {
    Terms Terms;
    std::string Src;
    ParseResult R;

    explicit Parsed(std::string text) : Src(std::move(text)), R(Parse(Terms, Src)) { REQUIRE(R.Diags.empty()); }

    ValueId Stmt(uint32_t i) const { return Terms.Child(R.Root, i); }
    ValueId Body() const { return ClauseBody(Terms, R.Root); }
    std::string BodyShape() const { return Shape(Terms, Body()); }
    std::string Print() const { return PrintTerm(Terms, R.Root); }
    uint16_t Variants(uint32_t i) const { return Terms.Get(Stmt(i)).Variants; }
};

} // namespace

TEST_CASE("the four precision spellings accumulate into one bitmask") {
    Parsed const p(
        "singleprecision process = 1;\n"
        "doubleprecision f = 2;\n"
        "quadprecision g = 3;\n"
        "fixedpointprecision h = 4;\n"
        "singleprecision doubleprecision i = 5;\n"
    );
    CHECK(p.Variants(0) == Single);
    CHECK(p.Variants(1) == Double);
    CHECK(p.Variants(2) == Quad);
    CHECK(p.Variants(3) == FixedPoint);
    CHECK(p.Variants(4) == (Single | Double));
}

TEST_CASE("`float` and `double` are the cast primitives, not prefixes") {
    Parsed const p("process = float : int;");
    CHECK(p.BodyShape() == "Seq[Prim(float) Prim(int)]");
    CHECK(!Accepts("float process = 1;"));
}

TEST_CASE("order and repetition of the prefix normalize away") {
    Terms t;
    const ParseResult a = Parse(t, "singleprecision doubleprecision process = 1;");
    const ParseResult b = Parse(t, "doubleprecision singleprecision doubleprecision process = 1;");
    REQUIRE(a.Diags.empty());
    REQUIRE(b.Diags.empty());
    CHECK(a.Root == b.Root);
    CHECK(PrintTerm(t, a.Root) == "singleprecision doubleprecision process = 1;");
}

TEST_CASE("the prefix rides on every statement kind a `variantlist` precedes") {
    Parsed const p(
        "doubleprecision import(\"stdfaust.lib\");\n"
        "doubleprecision declare name \"v\";\n"
        "doubleprecision declare f author \"me\";\n"
        "doubleprecision <mdoc>text</mdoc>\n"
    );
    CHECK(p.Terms.KindOf(p.Stmt(0)) == Kind::Import);
    CHECK(p.Terms.KindOf(p.Stmt(3)) == Kind::MdocBlock);
    for (uint32_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(p.Variants(i) == Double);
    }
    CheckPutGet(p.Src);
}

TEST_CASE("the prefix reaches a definition inside a `with` and a `[defs]`") {
    Parsed const w("process = a with { doubleprecision a = 1; };");
    const ValueId inner = w.Terms.Children(w.Body()).back();
    CHECK(w.Terms.KindOf(inner) == Kind::Definition);
    CHECK(w.Terms.Get(inner).Variants == Double);
    CheckPutGet(w.Src);

    CheckPutGet("process = a[singleprecision b = 1;];");
    CheckPutGet("process = a letrec { 'x = x; where doubleprecision y = 1; };");
}

TEST_CASE("two definitions differing only by prefix are two values") {
    Terms t;
    CHECK(Parse(t, "process = 1;").Root != Parse(t, "doubleprecision process = 1;").Root);
    Parsed const p("doubleprecision f(0) = 1;\nsingleprecision f(1) = 2;\n");
    CHECK(p.Terms.Children(p.R.Root).size() == 2);
}

TEST_CASE("`letrec` splits its children at the first Definition") {
    Parsed const p("process = a letrec { 'x = y; 'y = x; where u = 1; v = 2; };");
    CHECK(
        p.BodyShape() ==
        "LetRec[Ident(a) RecDef(x)[Ident(y)] RecDef(y)[Ident(x)] "
        "Definition(u)[Clause[Int(1)]] Definition(v)[Clause[Int(2)]]]"
    );
    CheckPutGet(p.Src);
}

TEST_CASE("`letrec` without a `where`, and with an empty rec list") {
    CHECK(Accepts("process = a letrec { 'x = x; };"));
    CHECK(Accepts("process = a letrec { };"));
    CHECK(Accepts("process = a letrec { where y = 1; };"));
    CheckPutGet("process = a letrec { 'x = x+1; };");
    CheckPutGet("process = a letrec { where y = 1; };");
}

TEST_CASE("a rec name is primed and takes no parameters") {
    CHECK(!Accepts("process = a letrec { x = x; };"));
    CHECK(!Accepts("process = a letrec { 'f(x) = x; };"));
    Parsed const p("process = a letrec { 'x = x; };");
    const ValueId rec = p.Terms.Child(p.Body(), 1);
    CHECK(p.Terms.KindOf(rec) == Kind::RecDef);
    CHECK(p.Terms.Lexeme(rec) == "x"); // the prime is syntax, not part of the name
}

TEST_CASE("a `where` list is a deflist, so it holds no import") {
    CHECK(!Accepts("process = a letrec { 'x = x; where import(\"s.lib\"); };"));
    CHECK(!Accepts("process = a letrec { 'x = x; where declare n \"v\"; };"));
}

TEST_CASE("`where` definitions print on the where side and rec ones do not") {
    Parsed const p("process = a letrec { 'x = y; where y = 1; };");
    const std::string out = p.Print();
    CAPTURE(out);
    CHECK(out.find("'x = y;") < out.find("where"));
    CHECK(out.find("y = 1;") > out.find("where"));
}

TEST_CASE("a splice inside a `where` definition leaves the rec side alone") {
    File f("process = a letrec { 'x = y /*keep*/; where y = 1; };");
    const ValueId two = f.Terms.MakeLeaf(Kind::Int, f.Terms.InternStr("2"));
    CHECK(f.SpliceTo(f.RefFor("1"), two) == "process = a letrec { 'x = y /*keep*/; where y = 2; };");
}

TEST_CASE("`e[defs]` is a postfix operator at level 15") {
    Parsed const p("process = f[a = 1; b = 2;];");
    CHECK(p.BodyShape() == "ModifLocalDef[Ident(f) Definition(a)[Clause[Int(1)]] Definition(b)[Clause[Int(2)]]]");
    CheckPutGet(p.Src);
}

TEST_CASE("levels 13 to 15 chain in source order") {
    Parsed const a("process = f.g[x = 1;];");
    CHECK(a.BodyShape() == "ModifLocalDef[Access(g)[Ident(f)] Definition(x)[Clause[Int(1)]]]");
    Parsed const b("process = f[x = 1;].g;");
    CHECK(b.BodyShape() == "Access(g)[ModifLocalDef[Ident(f) Definition(x)[Clause[Int(1)]]]]");
    Parsed const c("process = f[x = 1;](y);");
    CHECK(c.BodyShape() == "Apply[ModifLocalDef[Ident(f) Definition(x)[Clause[Int(1)]]] Ident(y)]");
    CheckPutGet(a.Src);
    CheckPutGet(b.Src);
    CheckPutGet(c.Src);
}

TEST_CASE("`[defs]` takes a deflist, empty included") {
    CHECK(Accepts("process = f[];"));
    CHECK(Accepts("process = f[a(x) = x;];"));
    CHECK(Accepts("process = f[a(0) = 1; a(1) = 2;];"));
    CHECK(!Accepts("process = f[import(\"s.lib\");];"));
    CHECK(!Accepts("process = f[declare n \"v\";];"));
    // An expression, so it stays on one line. Only statement lists break.
    Parsed const p("process = f[a = 1; b = 2;];");
    CHECK(p.Print() == "process = f[a = 1; b = 2;];");
}

TEST_CASE("`[` in infix position is `[defs]` and in prefix position a modulation") {
    Parsed const a("process = f[x = 1;];");
    Parsed const b("process = [\"x\" -> f];");
    CHECK(a.Terms.KindOf(a.Body()) == Kind::ModifLocalDef);
    CHECK(b.Terms.KindOf(b.Body()) == Kind::Modulation);
    CHECK(Accepts("process = g(f[x = 1;], [\"y\" -> h]);"));
}

TEST_CASE("a modulator's circuit is optional") {
    // An omitted circuit means `*` at evaluation, so the absence is kept, not defaulted.
    Parsed const p("process = [\"Wet\" -> e];");
    CHECK(p.BodyShape() == "Modulation[Modulator(\"Wet\") Ident(e)]");
    Parsed const q("process = [\"Wet\": *(2) -> e];");
    CHECK(q.BodyShape() == "Modulation[Modulator(\"Wet\")[Apply[Prim(*) Int(2)]] Ident(e)]");
    CheckPutGet(p.Src);
    CheckPutGet(q.Src);
}

TEST_CASE("modulators are comma-separated and keep source order") {
    Parsed const p("process = [\"freq\": r, \"gain\", \"gate\": s -> e];");
    const auto kids = p.Terms.Children(p.Body());
    REQUIRE(kids.size() == 4); // three entries, then the expression
    CHECK(p.Terms.Lexeme(kids[0]) == "\"freq\"");
    CHECK(p.Terms.Lexeme(kids[1]) == "\"gain\"");
    CHECK(p.Terms.Lexeme(kids[2]) == "\"gate\"");
    CHECK(p.Terms.Children(kids[1]).empty()); // the one with no circuit
    CHECK(p.Terms.KindOf(kids[3]) == Kind::Ident);
    CheckPutGet(p.Src);
}

TEST_CASE("a modulator's circuit is a level-2 position") {
    // `,` separates entries rather than building a parallel composition.
    Parsed const p("process = [\"a\": (x,y) -> e];");
    CHECK(p.Terms.Children(p.Body()).size() == 2); // one entry, one expression
    CHECK(p.BodyShape() == "Modulation[Modulator(\"a\")[Par[Ident(x) Ident(y)]] Ident(e)]");
    CheckPutGet(p.Src);
    // Without them it is two entries, the second with no string to name it.
    CHECK(!Accepts("process = [\"a\": x,y -> e];"));
    CheckPutGet("process = [\"a\" -> x,y];");
    CheckPutGet("process = [\"a\" -> x with { x = 1; }];");
}

TEST_CASE("the modulator separator is not the sequential composition operator") {
    // This `:` separates a target path from its circuit, so `:`'s spacing rule misses it.
    Parsed const p("process = [\"freq\"   :   replace -> e];");
    CHECK(p.Print() == "process = [\"freq\": replace -> e];");
}

TEST_CASE("a modulation nests and composes like any other primitive") {
    CheckPutGet("process = [\"a\" -> [\"b\" -> e]];");
    CheckPutGet("process = [\"a\" -> e] : [\"b\" -> f];");
    CheckPutGet("process = g([\"a\" -> e]);");
    CheckPutGet("process = [\"v:Group/Wet\": *(0.5) -> e];");
}
