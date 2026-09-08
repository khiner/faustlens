// The edit catalogue entry by entry, as the bytes each key press produces.
#include "syntax/Edit.h"
#include "syntax/Parser.h"
#include "syntax/Splice.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <string>

using namespace faustlens;
using namespace faustlens::test;

TEST_CASE("inline fields trim whitespace and reject comments and malformed tokens") {
    File f("process = hslider(\"gain\",0,0,1,0.1);");
    const RefId number = f.RefFor("0.1");
    for (const char *input : {"2 //note", "2 /*note*/", "2 /*", "2 3"}) {
        CAPTURE(input);
        CHECK_FALSE(f.Ed->Retext(number, input));
    }
    const Edit e = f.Ed->Retext(number, " \t2.5 \n");
    REQUIRE(e);
    CHECK(f.Terms.Lexeme(e.Value) == "2.5");
    const auto parsed = Parse(f.Terms, f.After(e));
    REQUIRE(parsed.Diags.empty());
    const ValueId widget = ClauseBody(f.Terms, parsed.Root);
    CHECK(f.Terms.Child(widget, 3) == e.Value);
    const RefId label = f.RefFor("hslider(\"gain\",0,0,1,0.1)");
    for (const char *input : {"\"new\" //note", "\"new\" /*note*/", "\"unterminated", "\"a\" \"b\""}) {
        CAPTURE(input);
        CHECK_FALSE(f.Ed->Retext(label, input));
    }
    const Edit renamed = f.Ed->Retext(label, " \"new\" \t");
    REQUIRE(renamed);
    CHECK(f.Terms.Lexeme(renamed.Value) == "\"new\"");
    const auto relabelled = Parse(f.Terms, f.After(renamed));
    REQUIRE(relabelled.Diags.empty());
    CHECK(ClauseBody(f.Terms, relabelled.Root) == renamed.Value);
}

TEST_CASE("deleting an equal stage retains the chosen sibling occurrence") {
    File f("process = (1 /*left*/) , (1 /*right*/);");
    const RefId root = f.RefFor("(1 /*left*/) , (1 /*right*/)");
    const auto kids = f.R.Refs.Children(root);
    const std::string out = f.After(f.Ed->Delete(kids[0]));
    const auto p = Parse(f.Terms, out);
    REQUIRE(p.Diags.empty());
    CHECK(out.find("(1 /*right*/)") != std::string::npos);
    CHECK(out.find("(1 /*left*/)") == std::string::npos);
    CHECK(ClauseBody(f.Terms, p.Root) == f.R.Refs.Refs[kids[1]].ValueId);
}

TEST_CASE("deleting inside a function argument preserves its argument boundary") {
    for (const char *source : {"process = f((_ <: _,_ : +));", "process = f((_ <: _,_ : +),0);"}) {
        File f(source);
        const RefId split = f.RefFor("_ <: _,_ : +");
        const Edit e = f.Ed->Delete(f.R.Refs.Children(split)[0]);
        const auto p = Parse(f.Terms, f.After(e));
        REQUIRE(p.Diags.empty());
        const ValueId call = ClauseBody(f.Terms, p.Root);
        CHECK(f.Terms.Children(call).size() == f.Terms.Children(ClauseBody(f.Terms, f.R.Root)).size());
        CHECK(f.Terms.Child(call, 1) == e.Value);
    }
}

TEST_CASE("insert into a sequence: `a : b` becomes `a : x : b`") {
    File f("process = a : b;");
    const ValueId x = f.Terms.MakeLeaf(Kind::Ident, f.Terms.InternStr("x"));
    // `:` is right-associative, so inserting after the left operand builds at the sequence.
    CHECK(f.After(f.Ed->Compose(f.RefFor("a"), Kind::Seq, 0, Side::After, x)) == "process = a : x : b;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::Seq, 0, Side::After, x)) == "process = a : b : x;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("a"), Kind::Seq, 0, Side::Before, x)) == "process = x : a : b;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::Seq, 0, Side::Before, x)) == "process = a : x : b;");
}

TEST_CASE("insert into a recursion: `~` folds the other way") {
    // `~` is the one left-associative connective, so the side needing the parent mirrors `:`.
    File f("process = a ~ b;");
    const ValueId x = f.Terms.MakeLeaf(Kind::Ident, f.Terms.InternStr("x"));
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::RecComp, 0, Side::After, x)) == "process = a ~ b ~ x;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("a"), Kind::RecComp, 0, Side::After, x)) == "process = a ~ x ~ b;");
}

TEST_CASE("wrap a selection: the bare key press supplies `_`") {
    File f("process = a : b;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::Seq, 0, Side::After)) == "process = a : b : _;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::Par, 0, Side::After)) == "process = a : b,_;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("a : b"), Kind::Par, 0, Side::After)) == "process = (a : b),_;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::Split, 0, Side::After)) == "process = a : (b <: _);");
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::Merge, uint8_t(MergeSpelling::Plus), Side::After)) == "process = a : (b +> _);");
}

TEST_CASE("a wrap keeps the comments in the stage it wraps") {
    File f("process = a /* keep */ : b;");
    CHECK(f.After(f.Ed->Compose(f.RefFor("b"), Kind::Seq, 0, Side::After)) == "process = a /* keep */ : b : _;");
}

TEST_CASE("delete a stage: the composition becomes its other operand") {
    File f("process = a : b : c;");
    CHECK(f.After(f.Ed->Delete(f.RefFor("b"))) == "process = a : c;");
    CHECK(f.After(f.Ed->Delete(f.RefFor("a"))) == "process = b : c;");
    CHECK(f.After(f.Ed->Delete(f.RefFor("b : c"))) == "process = a;");
}

TEST_CASE("delete is declined where no composition holds the stage") {
    File f("process = a : b;\nfoo = 1;\n");
    CHECK(f.Ed->Delete(f.R.Refs.Root()).Declined != nullptr);
    CHECK(f.Ed->Delete(f.RefFor("a : b")).Declined != nullptr); // the body of a definition
    CHECK(f.Ed->Delete(f.RefFor("1")).Declined != nullptr);
}

TEST_CASE("change a literal: the siblings are never reprinted") {
    File f("process = hslider(\"gain\", 0, /* lo */ 0, 1, 0.1);");
    CHECK(f.After(f.Ed->Retext(f.RefFor("0.1"), "0.01")) == "process = hslider(\"gain\", 0, /* lo */ 0, 1, 0.01);");
    // The kind is what the bytes lex as, so an `Int` retexted to `1.5` becomes a `Real`.
    const Edit e = f.Ed->Retext(f.RefFor("1"), "1.5");
    CHECK(f.Terms.KindOf(e.Value) == Kind::Real);
    CHECK(f.After(e) == "process = hslider(\"gain\", 0, /* lo */ 0, 1.5, 0.1);");
}

TEST_CASE("change a UI label: the text is the source spelling, quotes included") {
    File f("process = hslider(\"gain\", 0, 0, 1, 0.1);");
    // The label is the one text an edit can change, and separators come back tight.
    CHECK(f.After(f.Ed->Retext(f.RefFor("hslider(\"gain\", 0, 0, 1, 0.1)"), "\"level\"")) == "process = hslider(\"level\",0,0,1,0.1);");
    CHECK(f.Ed->Retext(f.RefFor("hslider(\"gain\", 0, 0, 1, 0.1)"), "level").Declined != nullptr);
}

TEST_CASE("retext is declined where the text is not what the node spells") {
    File f("process = hslider(\"gain\", 0, 0, 1, 0.1) : foo;");
    CHECK(f.Ed->Retext(f.RefFor("0.1"), "banana").Declined != nullptr);
    CHECK(f.Ed->Retext(f.RefFor("0.1"), "1 2").Declined != nullptr);
    CHECK(f.Ed->Retext(f.RefFor("0.1"), "-1").Declined != nullptr); // a `BinOp` outside a waveform
    CHECK(f.Ed->Retext(f.RefFor("foo"), "bar").Declined != nullptr); // not a catalogue entry
}

TEST_CASE("a waveform's numbers carry their sign, and take no stage") {
    File f("process = waveform{0, -1, 2};");
    CHECK(f.After(f.Ed->Retext(f.RefFor("2"), "-3")) == "process = waveform{0, -1, -3};");
    // A `vallist` is numbers, not expressions, so nothing may be composed with one.
    CHECK(f.Ed->Compose(f.RefFor("2"), Kind::Seq, 0, Side::After).Declined != nullptr);
}

TEST_CASE("rewire a route: one pair added and taken back out") {
    File f("process = route(2, 2, 1, 1, 2, 2);");
    const RefId route = f.RefFor("route(2, 2, 1, 1, 2, 2)");
    // Surviving entries keep their bytes and the commas come back from the printer.
    CHECK(f.After(f.Ed->Connect(route, 1, 2)) == "process = route(2,2,1,1,2,2,1,2);");
    CHECK(f.After(f.Ed->Disconnect(route, 1, 1)) == "process = route(2,2,2, 2);");
    CHECK(f.After(f.Ed->Connect(route, 2, 2)) == f.Src);
    CHECK(f.Ed->Disconnect(route, 3, 3).Declined != nullptr);
}

TEST_CASE("rewire the last pair out, and the route loses its entries") {
    // `route(n, m)` is the only spelling the grammar has for no entries.
    File f("process = route(2, 2, 1, 1);");
    const RefId route = f.RefFor("route(2, 2, 1, 1)");
    CHECK(f.After(f.Ed->Disconnect(route, 1, 1)) == "process = route(2,2);");
}

TEST_CASE("rewire a route that has no entries yet") {
    File f("process = route(2, 2);");
    CHECK(f.After(f.Ed->Connect(f.RefFor("route(2, 2)"), 1, 1)) == "process = route(2,2,1,1);");
}
