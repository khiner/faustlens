// The mdoc lexer modes, which only four corpus files reach, so hand-written.
#include "syntax/Lexer.h"
#include "syntax/Parser.h"
#include "syntax/Printer.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <string>
#include <string_view>

using namespace faustlens;
using namespace faustlens::test;

namespace {

ValueId FirstStatement(Terms &t, const ParseResult &r) { return t.Child(r.Root, 0); }

} // namespace

TEST_CASE("an mdoc block has six part kinds") {
    Terms t;
    const std::string src = "<mdoc>\n"
                            "prose here\n"
                            "<equation>a+b</equation>\n"
                            "<diagram>a : b</diagram>\n"
                            "<notice/>\n"
                            "<listing dependencies=\"true\" mdoctags=\"false\" distributed=\"true\" />\n"
                            "<metadata>author</metadata>\n"
                            "</mdoc>\n";
    const ParseResult r = Parse(t, src);
    REQUIRE(r.Diags.empty());
    const ValueId block = FirstStatement(t, r);
    REQUIRE(t.KindOf(block) == Kind::MdocBlock);

    int prose = 0, eqn = 0, dgm = 0, notice = 0, listing = 0, metadata = 0;
    for (const ValueId part : t.Children(block)) {
        switch (t.KindOf(part)) {
            case Kind::MdocProse: ++prose; break;
            case Kind::MdocEquation: ++eqn; break;
            case Kind::MdocDiagram: ++dgm; break;
            case Kind::MdocNotice: ++notice; break;
            case Kind::MdocListing: ++listing; break;
            case Kind::MdocMetadata: ++metadata; break;
            default: FAIL("unexpected mdoc part"); break;
        }
    }
    CHECK(eqn == 1);
    CHECK(dgm == 1);
    CHECK(notice == 1);
    CHECK(listing == 1);
    CHECK(metadata == 1);
    CHECK(prose > 0);
}

TEST_CASE("equations and diagrams hold real expressions") {
    Terms t;
    const ParseResult r = Parse(t, "<mdoc><equation>a : b</equation></mdoc>");
    REQUIRE(r.Diags.empty());
    const ValueId eqn = t.Child(FirstStatement(t, r), 0);
    REQUIRE(t.KindOf(eqn) == Kind::MdocEquation);
    CHECK(t.KindOf(t.Child(eqn, 0)) == Kind::Seq);
}

TEST_CASE("listing attributes normalize on a reprint") {
    Terms t;
    const ParseResult a = Parse(t, "<mdoc><listing distributed=\"true\" dependencies=\"false\" /></mdoc>");
    const ParseResult b = Parse(
        t,
        "<mdoc><listing dependencies=\"true\" dependencies=\"false\" "
        "distributed=\"true\" /></mdoc>"
    );
    REQUIRE(a.Diags.empty());
    REQUIRE(b.Diags.empty());
    CHECK(a.Root == b.Root);
    CHECK(PrintTerm(t, a.Root) == "<mdoc><listing dependencies=\"false\" distributed=\"true\"/></mdoc>");
}

TEST_CASE("both notice spellings are accepted") {
    Terms t;
    CHECK(Parse(t, "<mdoc><notice/></mdoc>").Diags.empty());
    CHECK(Parse(t, "<mdoc><notice /></mdoc>").Diags.empty());
}

TEST_CASE("an unterminated mdoc block is diagnosed") {
    Terms t;
    const ParseResult r = Parse(t, "<mdoc>prose with no close");
    bool found = false;
    for (const Diagnostic &d : r.Diags) found |= d.Code == Code::SynUnterminatedMdoc;
    CHECK(found);
}

TEST_CASE("mdoc round-trips") {
    Terms t;
    const std::string src = "<mdoc>text <equation>a+b</equation> tail</mdoc>\nprocess = 1;";
    const ParseResult a = Parse(t, src);
    REQUIRE(a.Diags.empty());
    const std::string printed = PrintTerm(t, a.Root);
    CAPTURE(printed);
    const ParseResult b = Parse(t, printed);
    REQUIRE(b.Diags.empty());
    CHECK(a.Root == b.Root);
}

TEST_CASE("the mode stack returns to prose after every pushing tag") {
    // Prose either side is what shows the pop: in default mode those words would be identifiers.
    const std::string src = "<mdoc>one<equation>a</equation>two<diagram>b</diagram>"
                            "three<metadata>author</metadata>four</mdoc>";
    CheckTiling(src);
    Terms t;
    const ParseResult r = Parse(t, src);
    REQUIRE(r.Diags.empty());
    const auto parts = t.Children(FirstStatement(t, r));
    // prose, equation, prose, diagram, prose, metadata, prose
    REQUIRE(parts.size() == 7);
    CHECK(t.KindOf(parts[0]) == Kind::MdocProse);
    CHECK(t.KindOf(parts[1]) == Kind::MdocEquation);
    CHECK(t.KindOf(parts[3]) == Kind::MdocDiagram);
    CHECK(t.KindOf(parts[5]) == Kind::MdocMetadata);
    CHECK(t.Lexeme(parts[6]) == "four");
    CheckPutGet(src);
}

TEST_CASE("an equation holds a whole level-1 expression") {
    CheckPutGet("<mdoc><equation>a,b : c with { c = 1; }</equation></mdoc>");
    CheckPutGet("<mdoc><diagram>par(i, 4, i)</diagram></mdoc>");
    Terms t;
    const ParseResult r = Parse(t, "<mdoc><equation>a with { a = 1; }</equation></mdoc>");
    REQUIRE(r.Diags.empty());
    CHECK(t.KindOf(t.Child(t.Child(FirstStatement(t, r), 0), 0)) == Kind::With);
}

TEST_CASE("prose is one token per maximal run, and `<` in it is ordinary text") {
    // Flex returns DOCCHAR a character at a time, so one token per run sizes the token
    // vector by structure rather than by file length.
    const std::string src = "<mdoc>a < b and a <= b, but not <equation>x</equation></mdoc>";
    CheckTiling(src);
    const LexResult lex = Lex(src);
    size_t prose_tokens = 0;
    for (const Token &tok : lex.Tokens)
        if (tok.Kind == Tok::DocChar) ++prose_tokens;
    CHECK(prose_tokens == 1);
    CheckPutGet(src);
}

TEST_CASE("listing mode tiles its own whitespace") {
    // Flex's `lst` state has no whitespace production and silently ECHOes these spaces.
    for (const std::string_view src : {
             "<mdoc><listing /></mdoc>",
             "<mdoc><listing/></mdoc>",
             "<mdoc><listing   dependencies=\"true\"   mdoctags=\"false\"   /></mdoc>",
             "<mdoc><listing\n  distributed=\"true\"\n/></mdoc>",
         }) {
        CAPTURE(src);
        CheckTiling(src);
        CheckPutGet(src);
    }
    Terms t;
    const ParseResult r = Parse(t, "<mdoc><listing /></mdoc>");
    REQUIRE(r.Diags.empty());
    CHECK(t.Get(t.Child(FirstStatement(t, r), 0)).Form == 0);
    CHECK(PrintTerm(t, r.Root) == "<mdoc><listing/></mdoc>");
}

TEST_CASE("an unknown listing attribute is diagnosed, not consumed silently") {
    Terms t;
    const ParseResult r = Parse(t, "<mdoc><listing colour=\"true\" /></mdoc>");
    bool found = false;
    for (const Diagnostic &d : r.Diags) found |= d.Code == Code::SynBadListingAttribute;
    CHECK(found);
}

TEST_CASE("`<mdoc>` beats the fstring rule, and `<math.h>` is unaffected") {
    // Both rules match the same six bytes, and flex breaks the tie by rule order.
    const LexResult a = Lex("<mdoc></mdoc>");
    REQUIRE(a.Tokens.size() >= 2);
    CHECK(a.Tokens[0].Kind == Tok::BDoc);
    const LexResult b = Lex("<math.h>");
    REQUIRE(b.Tokens.size() >= 1);
    CHECK(b.Tokens[0].Kind == Tok::FString);
    Terms t;
    CHECK(Parse(t, "process = fconstant(int x, <math.h>);").Diags.empty());
}

TEST_CASE("an mdoc block sits among ordinary statements and keeps its place") {
    const std::string src = "declare name \"v\";\n"
                            "<mdoc>\n### A section\n<equation>x</equation>\n</mdoc>\n"
                            "process = 1;\n";
    CheckTiling(src);
    Terms t;
    const ParseResult r = Parse(t, src);
    REQUIRE(r.Diags.empty());
    const auto stmts = t.Children(r.Root);
    REQUIRE(stmts.size() == 3);
    CHECK(t.KindOf(stmts[0]) == Kind::Declare);
    CHECK(t.KindOf(stmts[1]) == Kind::MdocBlock);
    CHECK(t.KindOf(stmts[2]) == Kind::Definition);
    CheckPutGet(src);
}

TEST_CASE("a splice next to an mdoc block leaves its bytes alone") {
    const std::string src = "<mdoc>\nprose with `backticks` and a <equation>a+b</equation>\n</mdoc>\n"
                            "process = 1;\n";
    File f(src);
    const std::string out = f.SpliceTo(f.RefFor("1"), f.Terms.MakeLeaf(Kind::Int, f.Terms.InternStr("2")));
    CHECK(out.find("prose with `backticks`") != std::string::npos);
    CHECK(out.ends_with("process = 2;\n"));

    for (RefId i = 0; i < f.R.Refs.Refs.size(); ++i) {
        CAPTURE(i);
        CHECK(f.ScriptFor(i, f.R.Refs.Refs[i].ValueId).empty());
    }
}
