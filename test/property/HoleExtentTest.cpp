// Every hole's span lies inside the `;`-delimited statement containing the failure.
#include "property/Corpus.h"
#include "syntax/Parser.h"

#include "doctest.h"

#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

std::pair<uint32_t, uint32_t> EnclosingStatement(const TokenVector &tokens, uint32_t offset) {
    uint32_t begin = 0, end = 0;
    int depth = 0;
    for (const Token &t : tokens) {
        if (t.Kind == Tok::LPar || t.Kind == Tok::LBraq || t.Kind == Tok::LCroc) ++depth;
        else if (t.Kind == Tok::RPar || t.Kind == Tok::RBraq || t.Kind == Tok::RCroc) --depth;
        else if (t.Kind == Tok::EndDef && depth <= 0) {
            if (t.End <= offset) begin = t.End;
            else {
                end = t.End;
                break;
            }
        }
        if (t.Kind == Tok::Eof) end = t.End;
    }
    if (end == 0) end = tokens.empty() ? 0 : tokens.back().End;
    return {begin, end};
}

void CheckHoles(const std::string &name, const std::string &src) {
    Terms terms;
    const ParseResult r = Parse(terms, src);
    const TokenVector tokens = Lex(src).Tokens;
    for (const TermRef &ref : r.Refs.Refs) {
        if (terms.KindOf(ref.ValueId) != Kind::Hole) continue;
        const auto [begin, end] = EnclosingStatement(tokens, ref.SpanBegin);
        CHECK_MESSAGE(ref.SpanBegin >= begin, name);
        CHECK_MESSAGE(ref.SpanEnd <= end, name);
    }
}

} // namespace

TEST_CASE("holes carry the children the failing frame had built") {
    Terms terms;
    const ParseResult r = Parse(terms, "process = a : b : ;");
    REQUIRE(!r.Diags.empty());
    bool found = false;
    for (const TermRef &ref : r.Refs.Refs) {
        if (terms.KindOf(ref.ValueId) != Kind::Hole) continue;
        found = true;
        CHECK(terms.Children(ref.ValueId).size() >= 2); // `a` and `b`
    }
    CHECK(found);
}

TEST_CASE("the same bytes recovered in different frames intern apart") {
    // A hole's children are not a function of its lexeme, so two recoveries differ.
    Terms terms;
    const ParseResult top = Parse(terms, "process = ?;");
    const ParseResult nested = Parse(terms, "process = x with { y = ?; };");
    REQUIRE(!top.Diags.empty());
    REQUIRE(!nested.Diags.empty());
    CHECK(top.Root != nested.Root);
}

TEST_CASE("a hole stays inside its statement") {
    for (const std::string &src : {
             std::string("process = a : b : ;\nother = 1;"),
             std::string("bad = ?;\ngood = 1;"),
             std::string("process = x with { bad = ?; good = 1; };"),
             std::string("process = case { (0) => ?; (1) => 2; };"),
             std::string("process = a letrec { 'x = ?; };"),
             std::string("process = f(?, 2);"),
             std::string("process = ("),
             std::string("process ="),
         }) {
        CAPTURE(src);
        CheckHoles(src, src);
    }
}

TEST_CASE("hole extent over the truncation corpus") {
    // Truncating at a few interior offsets breaks the file in many different frames.
    size_t checked = 0;
    for (const CorpusFile &f : TestsCorpus()) {
        if (f.Text.size() < 32) continue;
        for (const size_t denom : {2u, 3u, 4u}) {
            const std::string truncated = f.Text.substr(0, f.Text.size() / denom);
            CheckHoles(f.Relative, truncated);
            ++checked;
        }
    }
    MESSAGE("hole extent over ", checked, " truncations");
    CHECK(checked > 100);
}

TEST_CASE("a recovery never runs past its enclosing frame's stop token") {
    Terms terms;
    const std::string src = "process = x with { bad ? ; };\nafter = 1;";
    const ParseResult r = Parse(terms, src);
    REQUIRE(!r.Diags.empty());
    for (const TermRef &ref : r.Refs.Refs) {
        if (terms.KindOf(ref.ValueId) != Kind::Hole) continue;
        CHECK(ref.SpanEnd <= src.find('}'));
    }
    bool has_after = false;
    for (const ValueId stmt : terms.Children(r.Root)) has_after |= terms.KindOf(stmt) == Kind::Definition && terms.Lexeme(stmt) == "after";
    CHECK(has_after);
}
