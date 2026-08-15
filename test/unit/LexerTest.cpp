#include "syntax/Lexer.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

std::vector<Tok> Kinds(std::string_view src, bool keep_trivia = false) {
    std::vector<Tok> out;
    for (const Token &t : Lex(src).Tokens) {
        if (t.Kind == Tok::Eof) break;
        if (!keep_trivia && IsTrivia(t.Kind)) continue;
        out.push_back(t.Kind);
    }
    return out;
}

std::string TextOf(std::string_view src, const Token &t) { return std::string(src.substr(t.Begin, t.End - t.Begin)); }

} // namespace

TEST_CASE("lexer tiles the file") {
    for (const std::string_view src : {
             "process = a : b;",
             "// comment\nprocess = 1;\n",
             "/* block\n comment */ x = 2;",
             "declare name \"v\";",
             "<mdoc>text<equation>a+b</equation>more</mdoc>",
             "<mdoc><listing dependencies=\"true\" mdoctags=\"false\" /></mdoc>",
             "",
             "\xff\xfe binary",
             "\"unterminated",
             "/* unterminated",
         }) {
        CAPTURE(src);
        CheckTiling(src);
    }
}

TEST_CASE("numbers follow the reference's maximal munch") {
    // `{DIGIT}+` is INT, every other numeric rule is FLOAT.
    CHECK(Kinds("3") == std::vector{Tok::Int});
    CHECK(Kinds("3f") == std::vector{Tok::Float});
    CHECK(Kinds("3.") == std::vector{Tok::Float});
    CHECK(Kinds(".5") == std::vector{Tok::Float});
    CHECK(Kinds("3e2") == std::vector{Tok::Float});
    CHECK(Kinds("3.5e-2f") == std::vector{Tok::Float});
    CHECK(Kinds("3.name") == std::vector{Tok::Float, Tok::Ident});
    CHECK(Kinds("3e") == std::vector{Tok::Int, Tok::Ident});
    CHECK(Kinds("3E5") == std::vector{Tok::Int, Tok::Ident});
    // No lexer rule matches a signed number. `ADD INT` is a parser production.
    CHECK(Kinds("-3") == std::vector{Tok::Sub, Tok::Int});
}

TEST_CASE("identifiers, keywords and namespaces") {
    CHECK(Kinds("os::osc") == std::vector{Tok::Ident});
    CHECK(Kinds("::foo") == std::vector{Tok::Ident});
    // Longest match then earliest rule: `min` is a keyword, `minimum` is not.
    CHECK(Kinds("min") == std::vector{Tok::Min});
    CHECK(Kinds("minimum") == std::vector{Tok::Ident});
    CHECK(Kinds("min::x") == std::vector{Tok::Ident});
    CHECK(Kinds("_") == std::vector{Tok::Wire});
    CHECK(Kinds("_a") == std::vector{Tok::Ident});
    CHECK(Kinds("a::") == std::vector{Tok::Ident, Tok::Seq, Tok::Seq});
}

TEST_CASE("operator spellings") {
    CHECK(Kinds(":> +>") == std::vector{Tok::Mix, Tok::Mix});
    CHECK(Kinds("<:") == std::vector{Tok::Split});
    CHECK(Kinds("<<") == std::vector{Tok::Lsh});
    CHECK(Kinds("<=") == std::vector{Tok::Le});
    CHECK(Kinds("=>") == std::vector{Tok::Arrow});
    CHECK(Kinds("->") == std::vector{Tok::LApply});
    CHECK(Kinds("^ pow") == std::vector{Tok::PowOp, Tok::PowFun});
}

TEST_CASE("FSTRING beats LT, exactly as in the reference") {
    // `"<"{LETTER}*">"` outruns `<` in faustlexer.l. Reproduced for acceptance parity.
    CHECK(Kinds("<math.h>") == std::vector{Tok::FString});
    CHECK(Kinds("a<b>c") == std::vector{Tok::Ident, Tok::FString, Tok::Ident});
    CHECK(Kinds("a<b)c") == std::vector{Tok::Ident, Tok::Lt, Tok::Ident, Tok::RPar, Tok::Ident});
    CHECK(Kinds("a < b > c") == std::vector{Tok::Ident, Tok::Lt, Tok::Ident, Tok::Gt, Tok::Ident});
}

TEST_CASE("comments are one token and survive being unterminated") {
    const std::string_view src = "/* a */ // b\nx";
    const TokenVector tokens = Lex(src).Tokens;
    CHECK(tokens[0].Kind == Tok::BlockComment);
    CHECK(TextOf(src, tokens[0]) == "/* a */");
    CHECK(tokens[2].Kind == Tok::LineComment);
    CHECK(TextOf(src, tokens[2]) == "// b");

    const LexResult bad = Lex("/* never closed");
    CHECK(bad.Diags.size() == 1);
    CHECK(bad.Diags[0].Code == Code::SynUnterminatedComment);
    CheckTiling("/* never closed");
}

TEST_CASE("mdoc modes stack") {
    const std::string_view src = "<mdoc>hi<equation>a+b</equation>bye</mdoc>";
    CHECK(Kinds(src) == std::vector{Tok::BDoc, Tok::DocChar, Tok::BEqn, Tok::Ident, Tok::Add, Tok::Ident, Tok::EEqn, Tok::DocChar, Tok::EDoc});
    const TokenVector tokens = Lex(src).Tokens;
    CHECK(TextOf(src, tokens[1]) == "hi");
}

TEST_CASE("listing mode has the whitespace token flex lacks") {
    const std::string_view src = "<mdoc><listing dependencies=\"true\" /></mdoc>";
    CheckTiling(src);
    CHECK(Kinds(src) == std::vector{Tok::BDoc, Tok::BLst, Tok::LstDependencies, Tok::LstEq, Tok::LstQ, Tok::LstTrue, Tok::LstQ, Tok::ELst, Tok::EDoc});
}

TEST_CASE("seams that would fuse tokens") {
    CHECK(WouldFuse("3", ".name")); // the float `3.`
    CHECK(WouldFuse("/", "* x"));
    CHECK(WouldFuse("<", ":"));
    CHECK(WouldFuse("a", "b"));
    CHECK(WouldFuse("<", "b>")); // an FSTRING
    CHECK(!WouldFuse("3", " .name"));
    CHECK(!WouldFuse("a", ":b"));
    CHECK(!WouldFuse("a", "(b)"));
    CHECK(!WouldFuse("-", "-3"));
}
