#include "syntax/Splice.h"
#include "syntax/Parser.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <string>

using namespace faustlens;
using namespace faustlens::test;

namespace {

// `a : b` rewritten to `a : x : b`, rebuilt so the replaced region is the connective.
ValueId InsertStage(File &f, RefId seq) {
    const ValueId v = f.R.Refs.Refs[seq].ValueId;
    const ValueId x = f.Terms.MakeLeaf(Kind::Ident, f.Terms.InternStr("x"));
    const ValueId inner[] = {x, f.Terms.Child(v, 1)};
    const ValueId outer[] = {f.Terms.Child(v, 0), f.Terms.Make(Kind::Seq, inner)};
    return f.Terms.Make(Kind::Seq, outer);
}

} // namespace

TEST_CASE("Hippocraticness: an identity edit is byte-identical") {
    File f("process = a /* keep */ : (b : c);   // trailing\n");
    for (RefId i = 0; i < f.R.Refs.Refs.size(); ++i) {
        CAPTURE(i);
        CHECK(f.ScriptFor(i, f.R.Refs.Refs[i].ValueId).empty());
    }
}

TEST_CASE("`a : b` becomes `a : x : b`, worked through") {
    File f("process = a : b;");
    const RefId seq = f.RefFor("a : b");
    const ValueId outer = InsertStage(f, seq);
    CHECK(f.ScriptFor(seq, outer).size() == 1); // " : " replaced by " : x : "
    CHECK(f.SpliceTo(seq, outer) == "process = a : x : b;");
}

TEST_CASE("comments inside the rewritten subtree survive") {
    File f("process = a /* about a */ : b;");
    const RefId seq = f.RefFor("a /* about a */ : b");
    CHECK(f.SpliceTo(seq, InsertStage(f, seq)) == "process = a /* about a */ : x : b;");
}

TEST_CASE("comment salvage keeps a comment on the side it was written on") {
    {
        File f("process = a /* keep */ : b;");
        const RefId seq = f.RefFor("a /* keep */ : b");
        CHECK(f.SpliceTo(seq, InsertStage(f, seq)) == "process = a /* keep */ : x : b;");
    }
    {
        File f("process = a : /* about b */ b;");
        const RefId seq = f.RefFor("a : /* about b */ b");
        CHECK(f.SpliceTo(seq, InsertStage(f, seq)) == "process = a : x : /* about b */ b;");
    }
}

TEST_CASE("comment salvage, the cases the corpus does not reach") {
    // The corpus reaches these shapes too rarely to cover them, so they are hand-written.
    SUBCASE("a line comment brings its own newline, or it swallows what follows") {
        File f("process = a // about a\n : b;");
        const RefId seq = f.RefFor("a // about a\n : b");
        CHECK(f.SpliceTo(seq, InsertStage(f, seq)) == "process = a // about a\n : x : b;");
    }
    SUBCASE("two comments in one replaced region keep their order and sides") {
        File f("process = a /*one*/ : /*two*/ b;");
        const RefId seq = f.RefFor("a /*one*/ : /*two*/ b");
        CHECK(f.SpliceTo(seq, InsertStage(f, seq)) == "process = a /*one*/ : x : /*two*/ b;");
    }
    SUBCASE("a comment inside a deleted stage is salvaged rather than dropped") {
        // `a : b : c` -> `a : c`, deleting the stage the comment was written on.
        File f("process = a : /*about b*/ b : c;");
        const RefId seq = f.RefFor("a : /*about b*/ b : c");
        const ValueId v = f.R.Refs.Refs[seq].ValueId;
        const ValueId kids[] = {f.Terms.Child(v, 0), f.Terms.Child(f.Terms.Child(v, 1), 1)};
        const std::string out = f.SpliceTo(seq, f.Terms.Make(Kind::Seq, kids));
        CAPTURE(out);
        CHECK(out.find("/*about b*/") != std::string::npos);
        CHECK(out.find(" b ") == std::string::npos);
    }
    SUBCASE("a comment between two arguments survives an argument rewrite") {
        File f("process = g(a, /*second*/ b);");
        const RefId apply = f.RefFor("g(a, /*second*/ b)");
        const ValueId v = f.R.Refs.Refs[apply].ValueId;
        const ValueId z = f.Terms.MakeLeaf(Kind::Ident, f.Terms.InternStr("z"));
        const ValueId kids[] = {f.Terms.Child(v, 0), z, f.Terms.Child(v, 2)};
        const std::string out = f.SpliceTo(apply, f.Terms.Make(Kind::Apply, kids));
        CAPTURE(out);
        CHECK(out.find("/*second*/") != std::string::npos);
        CHECK(out.find('z') != std::string::npos);
    }
    SUBCASE("a comment in an untouched region is not salvaged, it is simply kept") {
        File f("process = a /*keep*/ : b : c;");
        const RefId inner = f.RefFor("b : c");
        const ValueId v = f.R.Refs.Refs[inner].ValueId;
        const ValueId x = f.Terms.MakeLeaf(Kind::Ident, f.Terms.InternStr("x"));
        const ValueId kids[] = {x, f.Terms.Child(v, 1)};
        CHECK(f.SpliceTo(inner, f.Terms.Make(Kind::Seq, kids)) == "process = a /*keep*/ : x : c;");
    }
}

TEST_CASE("a moved subtree is retained rather than reprinted") {
    File f("process = a /*L*/ : b;");
    const RefId seq = f.RefFor("a /*L*/ : b");
    const ValueId v = f.R.Refs.Refs[seq].ValueId;
    const ValueId swapped = f.Terms.Make(Kind::Seq, {f.Terms.Child(v, 1), f.Terms.Child(v, 0)});
    const std::string out = f.SpliceTo(seq, swapped);
    CHECK(out.find('b') < out.find('a'));
}

TEST_CASE("the script stays inside the target span") {
    File f("outside = zzz;\nprocess = a : b;");
    const RefId seq = f.RefFor("a : b");
    RefId zzz = NoRef;
    for (RefId i = 0; i < f.R.Refs.Refs.size(); ++i)
        if (f.Terms.KindOf(f.R.Refs.Refs[i].ValueId) == Kind::Ident && f.Terms.Lexeme(f.R.Refs.Refs[i].ValueId) == "zzz") zzz = i;
    REQUIRE(zzz != NoRef);

    const TermRef &t = f.R.Refs.Refs[seq];
    for (const Replacement &rep : f.ScriptFor(seq, f.R.Refs.Refs[zzz].ValueId)) {
        CHECK(rep.Begin >= t.OuterBegin);
        CHECK(rep.End <= t.OuterEnd);
    }
    CHECK(f.SpliceTo(seq, f.R.Refs.Refs[zzz].ValueId) == "outside = zzz;\nprocess = zzz;");
}

TEST_CASE("an identity edit on a ref carrying an outer span reprints nothing") {
    // Bounding by `span` rather than `outer_span` would add parentheses on a no-op.
    File f("process = (a : b) : c;");
    for (RefId i = 0; i < f.R.Refs.Refs.size(); ++i) {
        const TermRef &t = f.R.Refs.Refs[i];
        if (t.OuterBegin == t.SpanBegin && t.OuterEnd == t.SpanEnd) continue;
        CAPTURE(i);
        CHECK(f.ScriptFor(i, t.ValueId).empty());
    }
}

TEST_CASE("a seam that would fuse tokens gets a space") {
    File f("process = 3 . name;");
    const RefId access = f.RefFor("3 . name");
    const std::string out = f.SpliceTo(access, f.R.Refs.Refs[access].ValueId);
    CHECK(out == f.Src);

    // Reprinting the connective puts `3` beside `.name`, which would lex as `3.`.
    File g("process = 3.5 . name;");
    const RefId a2 = g.RefFor("3.5 . name");
    const ValueId three = g.Terms.MakeLeaf(Kind::Int, g.Terms.InternStr("3"));
    const ValueId kids[] = {three};
    const ValueId rebuilt = g.Terms.Make(Kind::Access, 0, 0, g.Terms.Get(g.R.Refs.Refs[a2].ValueId).Payload, kids);
    const std::string spliced = g.SpliceTo(a2, rebuilt);
    CAPTURE(spliced);
    Terms check;
    const ParseResult reparsed = Parse(check, spliced);
    CHECK(reparsed.Diags.empty());
    CHECK(spliced.find("3 .") != std::string::npos);
}

TEST_CASE("normalization idempotence") {
    File f("process = a  ,  b;");
    const RefId par = f.RefFor("a  ,  b");
    const ValueId v = f.R.Refs.Refs[par].ValueId;
    const ValueId x = f.Terms.MakeLeaf(Kind::Ident, f.Terms.InternStr("x"));
    const ValueId rewritten = f.Terms.Make(Kind::Par, {f.Terms.Child(v, 0), x});

    const std::string once = f.SpliceTo(par, rewritten);
    File g(once);
    const RefId again = g.RefFor("a,x");
    CHECK(g.ScriptFor(again, g.R.Refs.Refs[again].ValueId).empty());
}
