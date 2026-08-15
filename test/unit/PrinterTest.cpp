#include "syntax/Printer.h"
#include "syntax/Parser.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <string>

using namespace faustlens;
using namespace faustlens::test;

namespace {

std::string RoundTrip(std::string_view src) {
    static Terms terms;
    const ParseResult r = Parse(terms, src);
    REQUIRE(r.Diags.empty());
    return PrintTerm(terms, r.Root);
}

} // namespace

TEST_CASE("parentheses are re-derived, not stored") {
    CHECK(RoundTrip("process = (a:b):c;") == "process = (a : b) : c;");
    CHECK(RoundTrip("process = a:(b:c);") == "process = a : b : c;");
    CHECK(RoundTrip("process = (a+b)*c;") == "process = (a+b)*c;");
    CHECK(RoundTrip("process = a+(b*c);") == "process = a+b*c;");
    CHECK(RoundTrip("process = a+(b+c);") == "process = a+(b+c);");
    // Levels 12-15 chain, so no parentheses are needed between them.
    CHECK(RoundTrip("process = (a.b)(x);") == "process = a.b(x);");
    CHECK(RoundTrip("process = (a')(x);") == "process = a'(x);");
    CHECK(RoundTrip("process = f((a,b));") == "process = f((a,b));");
    CHECK(RoundTrip("process = f(a:b);") == "process = f(a : b);");
    CHECK(RoundTrip("process = a : (b with { b = 1; });") == "process = a : (b with {\n    b = 1;\n});");
}

TEST_CASE("layout follows the corpus's dominant convention") {
    // Diagram-shaped operators take one space each side, expression-shaped ones are tight.
    CHECK(RoundTrip("process = a:b;") == "process = a : b;");
    CHECK(RoundTrip("process = a <: b :> c;") == "process = a <: b :> c;");
    CHECK(RoundTrip("process = a  ,  b;") == "process = a,b;");
    CHECK(RoundTrip("process = f( a , b );") == "process = f(a,b);");
    CHECK(RoundTrip("process = a  *  b;") == "process = a*b;");
    CHECK(RoundTrip("process=1;") == "process = 1;");
    CHECK(RoundTrip("process = a with { b = 1; c = 2; };") == "process = a with {\n    b = 1;\n    c = 2;\n};");
}

TEST_CASE("spellings survive the round trip") {
    CHECK(RoundTrip("process = a :> b;") == "process = a :> b;");
    CHECK(RoundTrip("process = a +> b;") == "process = a +> b;");
    CHECK(RoundTrip("process = ^;") == "process = ^;");
    CHECK(RoundTrip("process = pow;") == "process = pow;");
    CHECK(RoundTrip("process = 3f;") == "process = 3f;");
    CHECK(RoundTrip("process = .5;") == "process = .5;");
    CHECK(RoundTrip("process = +3;") == "process = +3;");
    CHECK(RoundTrip("process = x';") == "process = x';");
    CHECK(RoundTrip("process = x : mem;") == "process = x : mem;");
    CHECK(RoundTrip("process = -x;") == "process = -x;");
}

TEST_CASE("PutGet over the constructs") {
    for (const std::string_view src : {
             "process = a : b <: c :> d ~ e;",
             "process = a+b-c*d/e%f&g|h xor i<<j>>k;",
             "process = a<b , a<=b , a>b , a>=b , a==b , a!=b;",
             "process = x@2 : x^2 : x';",
             "f(x, y) = x + y;\nprocess = f(1, 2);",
             "f(0) = 1;\nf(x) = x;\nprocess = f;",
             "process = \\(x, y).(x : y);",
             "process = case { (0) => 1; (x, y) => x; };",
             "process = par(i, 4, i) : seq(j, 2, j);",
             "process = a with { b = 1; } letrec { 'c = c; };",
             "process = a letrec { 'x = y; where y = 1; };",
             "process = environment { a = 1; b = 2; };",
             "process = waveform{0, -1, 1.5, +2};",
             "process = route(2, 2) , route(2, 2, 1, 1);",
             "process = hslider(\"a [unit:dB]\", 0, -70, 12, 0.1);",
             "process = vgroup(\"g\", hbargraph(\"b\", 0, 1));",
             "process = soundfile(\"s\", 2);",
             "process = ffunction(float sinhf|sinh|sinhl(float), <math.h>, \"\");",
             "process = ffunction(int f(int, float, any), \"x.h\", \"lib\");",
             "process = fconstant(int x, <math.h>) , fvariable(float y, <m.h>);",
             "process = component(\"a.dsp\") : library(\"b.lib\").f;",
             "process = a[b = 1; c = 2;];",
             "process = [\"freq\": r, \"gain\" -> s] with { r = _; s = _; };",
             "process = inputs(x) , outputs(y);",
             "declare name \"v\";\ndeclare f author \"me\";\nimport(\"stdfaust.lib\");",
             "doubleprecision process = 1;",
             "process = os::osc(440) : ::foo;",
             "process = -(1+2) : - : -3 : -x;",
         }) {
        CAPTURE(src);
        CheckPutGet(src);
    }
}

TEST_CASE("printing inserts a space where a seam would fuse tokens") {
    // `3.name` would lex as the float `3.` then `name`, a different program.
    Terms terms;
    const ValueId three = terms.MakeLeaf(Kind::Int, terms.InternStr("3"));
    const ValueId access = terms.Make(Kind::Access, 0, 0, terms.InternStr("name"), {three});
    CHECK(PrintTerm(terms, access) == "3 .name");

    // Word-shaped operators are tight in the table and would otherwise fuse.
    const ValueId a = terms.MakeLeaf(Kind::Ident, terms.InternStr("a"));
    const ValueId b = terms.MakeLeaf(Kind::Ident, terms.InternStr("b"));
    const ValueId pair[] = {a, b};
    CHECK(PrintTerm(terms, terms.Make(Kind::BinOp, 0, 0, uint32_t(Tok::Xor), pair)) == "a xor b");
    CHECK(PrintTerm(terms, terms.Make(Kind::BinOp, 0, 0, uint32_t(Tok::Mul), pair)) == "a*b");
}

TEST_CASE("a recovered hole stays on the rec side of `where`") {
    // Splitting at the first non-`RecDef` would drag every `RecDef` after a hole across `where`.
    Terms terms;
    const std::string src = "process = a letrec { 'x = ?; 'y = y; where z = 1; };";
    const ParseResult r = Parse(terms, src);
    REQUIRE(!r.Diags.empty()); // the `?` is what makes the hole
    const std::string printed = PrintTerm(terms, r.Root);
    CAPTURE(printed);
    CHECK(printed.find("'y = y;") < printed.find("where"));
    CHECK(printed.find("z = 1;") > printed.find("where"));
}

TEST_CASE("the precision prefix rides on every clause of a definition") {
    Terms terms;
    const std::string src = "doubleprecision f(0) = 1;\ndoubleprecision f(1) = 2;";
    const ParseResult a = Parse(terms, src);
    REQUIRE(a.Diags.empty());
    REQUIRE(terms.Children(a.Root).size() == 1);
    CHECK(terms.Children(terms.Child(a.Root, 0)).size() == 2);
    CheckPutGet(src);

    const ParseResult mixed = Parse(terms, "doubleprecision f(0) = 1;\nf(1) = 2;");
    REQUIRE(mixed.Diags.empty());
    CHECK(terms.Children(mixed.Root).size() == 2);
}
